#include "plugin-std.h"
#include "AutoStartGame.h"

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <algorithm>

using namespace plugin;

namespace
{
    // NBA Live 06 1.0 NOCD only.
    constexpr unsigned int kLive06EntryPoint       = 0x0040109F;
    constexpr unsigned int kLive06AspectMarker     = 0x00BD832C;
    constexpr float        kStockAspectMarker      = 1.3333334f;

    // Native 06 anchors.
    constexpr unsigned int kFESystemGlobal         = 0x00CB2AF4;
    constexpr unsigned int kFESystemUpdate         = 0x00598980;

    // APT callback used by the title flow to ask where startup should go.
    constexpr unsigned int kGetStartupScreen       = 0x005CBA30;
    constexpr unsigned int kSetAptEnumReturn       = 0x005C8C60;

    // GD_FE_RUN -> FESystem::Update(delta) call site.
    constexpr unsigned int kFEUpdateCall           = 0x005F6AAF;

    // Intercept the two boot packages at FileSystem::OpenResolved.
    // The supplied modified BIG files contain the APT transition to TITLE;
    // we leave their bytes unchanged and supply them as alternate assets.
    // If another module already detours this entry with JMP rel32, chain it.
    constexpr unsigned int kOpenResolved06          = 0x006DC8F9;

    // Async file-request constructor instrumentation.
    //
    // sub_6DD894:
    //   006DD8A3 call sub_6DC39E
    //   006DD8A8 mov eax,[ebp+08]
    //   006DD8AB mov ecx,esi
    //   006DD8AD mov [esi],00BD506C
    //
    // sub_6DC39E has already populated the request object here, including
    // the filename pointer later consumed from [request+0x30].
    constexpr unsigned int kRequestCtorTracePatch06  = 0x006DD8A8;
    constexpr unsigned int kRequestCtorTraceResume06 = 0x006DD8AD;

    // FEUberGameMode singleton getter, useful only for diagnostics here.
    constexpr unsigned int kGetFEUberGameMode      = 0x004B8870;

    constexpr int kStateFeRun                      = 4;
    constexpr int kGDFeRun                         = 5;
    constexpr int kGDGameInit                      = 14;

    bool g_enabled = false;
    bool g_pending = false;
    bool g_consumed = false;

    // GetStartupScreen() is called while the title package is still being
    // initialized (FE_INIT).  Arm there, then wait until the title has
    // actually entered FE_RUN and has remained there briefly before starting
    // the game.
    DWORD g_feRunSeenAt = 0;
    DWORD g_autoStartDelayMs = 750;
    // Diagnostics: the INI delay begins at FE_RUN, not when Title first renders.
    DWORD g_autoStartInitializedAt = 0;
    DWORD g_titleStartupCallbackAt = 0;

    // ---------------------------------------------------------------------
    // title.apt: suppress only the normal PC MAIN_MENU transition.
    //
    // The supplied screens~bootup~title.big was decoded and the relevant
    // action stream is:
    //
    //   CallFunction("GetStartupScreen()")
    //   compare STARTUP_SCREEN_ONLINE
    //   if not online -> branch +0x1C to:
    //
    //       SCREEN.MAIN_MENU
    //       gSM.changeMainScreen(...)
    //
    // The first byte after that MAIN_MENU action is +0x25 from the same
    // branch base.  While autostart is armed we change only:
    //
    //       9D 1C 00 00 00  ->  9D 25 00 00 00
    //
    // so the non-online path skips changeMainScreen(MAIN_MENU).
    //
    // This is a PROCESS-MEMORY patch only.  No BIG/APT file is modified.
    // The original byte is restored once the native GD_GAME_INIT handoff
    // is consumed.
    // ---------------------------------------------------------------------
    constexpr unsigned char kTitleStartupBranchPattern[] =
    {
        0xA2, 0x1E,                   // GetStartupScreen()
        0x5A,
        0xA2, 0x0B,                   // CallFunction
        0x3D,
        0xA2, 0x1F,                   // STARTUP_SCREEN_ONLINE
        0x49,
        0x12,
        0x9D, 0x1C, 0x00, 0x00, 0x00,

        0xA2, 0x20,                   // ChangeGameMode(GAMEMODE_ONLINE)
        0x5A,
        0xB0, 0x0B,
        0xAE, 0x11,
        0xAF, 0x21,                   // SCREEN.MAIN_MENU
        0x5A,
        0xAE, 0x13,
        0xB2, 0x22,                   // gSM.pushScreen (online branch)
        0xAE, 0x11,
        0xAF, 0x23,                   // SCREEN.SN_START
        0x5A,
        0xAE, 0x13,
        0xB2, 0x14,                   // gSM.changeMainScreen
        0x99, 0x09, 0x00, 0x00, 0x00,

        // Normal PC branch target in stock title.apt:
        0xAE, 0x11,
        0xAF, 0x21,                   // SCREEN.MAIN_MENU
        0x5A,
        0xAE, 0x13,
        0xB2, 0x14                    // gSM.changeMainScreen
    };

    constexpr size_t kTitleBranchOperandOffset = 11;
    constexpr unsigned char kTitleBranchStockOffset = 0x1C;
    constexpr unsigned char kTitleBranchSkipMainMenuOffset = 0x25;

    struct TitleBranchPatch
    {
        unsigned char* operand = nullptr;
        unsigned char original = 0;
    };

    TitleBranchPatch g_titleBranchPatches[8] = {};
    size_t g_titleBranchPatchCount = 0;

    // Auto-start asset overrides are enabled only when AUTO_START_GAME=1.
    // They are one-shot for the initial boot; Title initialization disables
    // redirection so later FE navigation uses the original game assets.
    volatile LONG g_bootAssetOverridesActive = 0;
    const char* g_splashOverridePath = nullptr;
    const char* g_healthOverridePath = nullptr;
    volatile LONG g_splashOverrideRequests = 0;
    volatile LONG g_healthOverrideRequests = 0;

    // These paths are relative to the NBA Live 06 executable directory.
    // Accept either the native archive member filename or shorter aliases.
    constexpr char kSplashAssetNative[] =
        "assets\\autostart\\06\\screens~bootup~splash.big";
    constexpr char kHealthAssetNative[] =
        "assets\\autostart\\06\\screens~bootup~health.big";
    constexpr char kSplashAssetShort[] =
        "assets\\autostart\\06\\splash.big";
    constexpr char kHealthAssetShort[] =
        "assets\\autostart\\06\\health.big";

    constexpr char kBootSplashPackage[] =
        "screens~bootup~splash.big";
    constexpr char kBootHealthPackage[] =
        "screens~bootup~health.big";

    bool g_bootSelectorTrace = false;
    FILE* g_bootSelectorTraceFile = nullptr;
    bool g_tracedLegal = false;
    bool g_tracedNbaLogo = false;
    bool g_tracedTitle = false;

#if defined(_M_IX86)
    bool g_requestCtorTraceHookInstalled = false;
#endif

#if defined(_M_IX86)
    uintptr_t g_bootOpenResolvedContinuation = 0;
    bool g_bootOpenResolvedHookInstalled = false;
#endif

    const char kBootLegalPackage[] =
        "screens~bootup~legal.big";

    const char kBootTitlePackage[] =
        "screens~bootup~title.big";

    const char kBootNbaLogoPackage[] =
        "screens~bootup~nbalogo.big";

    void Log(const char* format, ...)
    {
        char buffer[768] = {};

        va_list args;
        va_start(args, format);
        vsnprintf_s(
            buffer,
            sizeof(buffer),
            _TRUNCATE,
            format,
            args);
        va_end(args);

        OutputDebugStringA(buffer);

        // The launcher already has a debug console in development builds.
        std::printf("%s", buffer);
        std::fflush(stdout);
    }

    std::string GetMainIniPath()
    {
        char exePath[MAX_PATH] = {};

        const DWORD len =
            GetModuleFileNameA(
                nullptr,
                exePath,
                static_cast<DWORD>(sizeof(exePath)));

        if (len == 0 || len >= sizeof(exePath))
            return ".\\main.ini";

        char* lastSlash = strrchr(exePath, '\\');
        char* lastForwardSlash = strrchr(exePath, '/');

        char* cut = lastSlash;
        if (!cut || (lastForwardSlash && lastForwardSlash > cut))
            cut = lastForwardSlash;

        if (cut)
            *(cut + 1) = '\0';
        else
            exePath[0] = '\0';

        return std::string(exePath) + "main.ini";
    }

    bool AssetFileExists06(const char* relativePath)
    {
        // OpenResolved accepts game-root-relative paths. Verify that these
        // files live next to the actual EXE, not an arbitrary current CWD.
        if (!relativePath)
            return false;
        const std::string iniPath = GetMainIniPath();
        const size_t suffix = sizeof("main.ini") - 1;
        if (iniPath.size() < suffix)
            return false;
        const std::string fullPath =
            iniPath.substr(0, iniPath.size() - suffix) + relativePath;
        const DWORD attributes = GetFileAttributesA(fullPath.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    const char* ChooseBootOverride06(
        const char* nativeName,
        const char* shortName)
    {
        if (AssetFileExists06(nativeName))
            return nativeName;
        if (AssetFileExists06(shortName))
            return shortName;
        return nullptr;
    }

    bool IsLive06()
    {
        return FM::GetEntryPoint() == kLive06EntryPoint &&
               patch::GetFloat(kLive06AspectMarker) == kStockAspectMarker;
    }

    int ReadFEState(void* feSystem)
    {
        if (!feSystem)
            return -1;

        return *reinterpret_cast<int*>(
            static_cast<std::uint8_t*>(feSystem) + 0x0C);
    }

    int ReadGDState(void* feSystem)
    {
        if (!feSystem)
            return -1;

        return *reinterpret_cast<int*>(
            static_cast<std::uint8_t*>(feSystem) + 0x10);
    }

    void DumpUberState(const char* reason)
    {
        void* uber =
            reinterpret_cast<void*(__cdecl*)()>(
                kGetFEUberGameMode)();

        if (!uber)
        {
            Log(
                "[AutoStart06] %s: FEUberGameMode=null\n",
                reason);
            return;
        }

        const auto* bytes =
            static_cast<const std::uint8_t*>(uber);

        const int currentMode =
            *reinterpret_cast<const int*>(bytes + 0x08);

        const int nextMode =
            *reinterpret_cast<const int*>(bytes + 0x0C);

        const int inGameplay =
            bytes[0x84] ? 1 : 0;

        Log(
            "[AutoStart06] %s: uber=%p currentMode=%d "
            "nextMode=%d inGameplay=%d\n",
            reason,
            uber,
            currentMode,
            nextMode,
            inGameplay);
    }

    const char* BaseNameOfPath(const char* path)
    {
        if (!path)
            return "";

        const char* base = path;

        for (const char* p = path; *p; ++p)
        {
            if (*p == '\\' || *p == '/')
                base = p + 1;
        }

        return base;
    }

    bool IsLikelyCodeAddress06(uintptr_t value)
    {
        return value >= 0x00401000 &&
               value <  0x00B91E51;
    }

    bool CopyReadableCString(
        const char* source,
        char* destination,
        size_t destinationSize)
    {
        if (!source ||
            !destination ||
            destinationSize == 0)
        {
            return false;
        }

        destination[0] = '\0';

        __try
        {
            size_t i = 0;

            for (;
                 i + 1 < destinationSize;
                 ++i)
            {
                const char c = source[i];
                destination[i] = c;

                if (c == '\0')
                    return i != 0;

                const unsigned char uc =
                    static_cast<unsigned char>(c);

                if (uc < 0x20 &&
                    c != '\t')
                {
                    destination[0] = '\0';
                    return false;
                }
            }

            destination[destinationSize - 1] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            destination[0] = '\0';
            return false;
        }
    }

    void OpenBootSelectorTraceFile()
    {
        if (!g_bootSelectorTrace || g_bootSelectorTraceFile)
            return;

        char exePath[MAX_PATH] = {};
        const DWORD len =
            GetModuleFileNameA(
                nullptr,
                exePath,
                static_cast<DWORD>(sizeof(exePath)));

        if (!len || len >= sizeof(exePath))
            return;

        char* slash = std::strrchr(exePath, '\\');
        if (!slash)
            return;

        *(slash + 1) = '\0';

        char logsDir[MAX_PATH] = {};
        sprintf_s(
            logsDir,
            sizeof(logsDir),
            "%slogs",
            exePath);

        CreateDirectoryA(logsDir, nullptr);

        char logPath[MAX_PATH] = {};
        sprintf_s(
            logPath,
            sizeof(logPath),
            "%slogs\\boot_selector_trace06.log",
            exePath);

        fopen_s(
            &g_bootSelectorTraceFile,
            logPath,
            "a");

        if (!g_bootSelectorTraceFile)
            return;

        SYSTEMTIME st = {};
        GetLocalTime(&st);

        std::fprintf(
            g_bootSelectorTraceFile,
            "\n============================================================\n"
            "NBA Live 06 boot selector trace %04u-%02u-%02u "
            "%02u:%02u:%02u\n"
            "============================================================\n",
            st.wYear,
            st.wMonth,
            st.wDay,
            st.wHour,
            st.wMinute,
            st.wSecond);

        std::fflush(g_bootSelectorTraceFile);
    }

    bool IsMainExeCodeAddress(uintptr_t value)
    {
        return IsLikelyCodeAddress06(value);
    }

    void WriteBootSelectorTraceLine(
        const char* format,
        ...)
    {
        if (!g_bootSelectorTrace)
            return;

        OpenBootSelectorTraceFile();

        if (!g_bootSelectorTraceFile)
            return;

        char line[2048] = {};

        va_list args;
        va_start(args, format);

        vsnprintf_s(
            line,
            sizeof(line),
            _TRUNCATE,
            format,
            args);

        va_end(args);

        std::fputs(
            line,
            g_bootSelectorTraceFile);

        std::fflush(
            g_bootSelectorTraceFile);

        OutputDebugStringA(line);
    }

    void TraceBootSelectorRequest(
        const char* requestedPath,
        uintptr_t* entryStack)
    {
        if (!g_bootSelectorTrace ||
            !requestedPath ||
            !entryStack)
        {
            return;
        }

        const char* base =
            BaseNameOfPath(requestedPath);

        bool* alreadyTraced = nullptr;
        const char* stageName = nullptr;

        if (_stricmp(base, kBootLegalPackage) == 0)
        {
            alreadyTraced = &g_tracedLegal;
            stageName = "LEGAL";
        }
        else if (_stricmp(base, kBootNbaLogoPackage) == 0)
        {
            alreadyTraced = &g_tracedNbaLogo;
            stageName = "NBA_LOGO";
        }
        else if (_stricmp(base, kBootTitlePackage) == 0)
        {
            alreadyTraced = &g_tracedTitle;
            stageName = "TITLE";
        }
        else
        {
            return;
        }

        if (*alreadyTraced)
            return;

        *alreadyTraced = true;

        WriteBootSelectorTraceLine(
            "\n[BOOT_SELECTOR] stage=%s requested='%s'\n",
            stageName,
            requestedPath);

        // Direct OpenResolved caller from its original entry stack.
        WriteBootSelectorTraceLine(
            "[BOOT_SELECTOR] direct_return=%08lX\n",
            static_cast<unsigned long>(
                entryStack[0]));

        // RtlCaptureStackBackTrace gives the real active call chain. The
        // first frames are our ASI hook/helper and are expected; later frames
        // should converge on OpenResolved and then the APT/resource caller.
        void* frames[48] = {};
        const USHORT captured =
            CaptureStackBackTrace(
                0,
                static_cast<DWORD>(
                    sizeof(frames) / sizeof(frames[0])),
                frames,
                nullptr);

        WriteBootSelectorTraceLine(
            "[BOOT_SELECTOR] captured_frames=%u\n",
            static_cast<unsigned>(captured));

        for (USHORT i = 0; i < captured; ++i)
        {
            const uintptr_t address =
                reinterpret_cast<uintptr_t>(
                    frames[i]);

            WriteBootSelectorTraceLine(
                "[BOOT_SELECTOR] frame[%02u]=%08lX%s\n",
                static_cast<unsigned>(i),
                static_cast<unsigned long>(address),
                IsMainExeCodeAddress(address)
                    ? "  EXE"
                    : "");
        }

        // Also scan the original caller stack for code-looking values. This
        // is not treated as a formal call chain; it is a second independent
        // fingerprint that survives imperfect x86 unwinding.
        WriteBootSelectorTraceLine(
            "[BOOT_SELECTOR] stack_code_candidates:\n");

        unsigned emitted = 0;

        __try
        {
            for (unsigned i = 0;
                 i < 384 && emitted < 64;
                 ++i)
            {
                const uintptr_t value =
                    entryStack[i];

                if (!IsMainExeCodeAddress(value))
                    continue;

                bool duplicate = false;

                for (unsigned j = 0; j < i; ++j)
                {
                    if (entryStack[j] == value)
                    {
                        duplicate = true;
                        break;
                    }
                }

                if (duplicate)
                    continue;

                WriteBootSelectorTraceLine(
                    "[BOOT_SELECTOR]   stack[%03u]=%08lX\n",
                    i,
                    static_cast<unsigned long>(value));

                ++emitted;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WriteBootSelectorTraceLine(
                "[BOOT_SELECTOR]   stack scan stopped at unreadable page\n");
        }

        WriteBootSelectorTraceLine(
            "[BOOT_SELECTOR] end stage=%s\n",
            stageName);
    }

#if defined(_M_IX86)
    void __cdecl TraceBootRequestConstructed(
        uintptr_t requestObject,
        uintptr_t constructorFrame)
    {
        if (!g_bootSelectorTrace ||
            !requestObject)
        {
            return;
        }

        char path[512] = {};
        const char* pathPointer = nullptr;

        __try
        {
            pathPointer =
                *reinterpret_cast<const char**>(
                    requestObject + 0x30);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }

        if (!CopyReadableCString(
                pathPointer,
                path,
                sizeof(path)))
        {
            return;
        }

        const char* base =
            BaseNameOfPath(path);

        bool* alreadyTraced = nullptr;
        const char* stageName = nullptr;

        if (_stricmp(base, kBootLegalPackage) == 0)
        {
            alreadyTraced = &g_tracedLegal;
            stageName = "LEGAL";
        }
        else if (_stricmp(base, kBootNbaLogoPackage) == 0)
        {
            alreadyTraced = &g_tracedNbaLogo;
            stageName = "NBA_LOGO";
        }
        else if (_stricmp(base, kBootTitlePackage) == 0)
        {
            alreadyTraced = &g_tracedTitle;
            stageName = "TITLE";
        }
        else
        {
            return;
        }

        if (*alreadyTraced)
            return;

        *alreadyTraced = true;

        DWORD field0C = 0;
        DWORD field14 = 0;
        DWORD field20 = 0;
        DWORD field24 = 0;
        DWORD field30 = 0;

        __try
        {
            field0C =
                *reinterpret_cast<DWORD*>(
                    requestObject + 0x0C);

            field14 =
                *reinterpret_cast<DWORD*>(
                    requestObject + 0x14);

            field20 =
                *reinterpret_cast<DWORD*>(
                    requestObject + 0x20);

            field24 =
                *reinterpret_cast<DWORD*>(
                    requestObject + 0x24);

            field30 =
                *reinterpret_cast<DWORD*>(
                    requestObject + 0x30);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        WriteBootSelectorTraceLine(
            "\n[BOOT_ORIGIN] stage=%s request=%08lX path='%s'\n",
            stageName,
            static_cast<unsigned long>(
                requestObject),
            path);

        WriteBootSelectorTraceLine(
            "[BOOT_ORIGIN] fields "
            "+0C=%08lX +14=%08lX +20=%08lX "
            "+24=%08lX +30=%08lX\n",
            static_cast<unsigned long>(field0C),
            static_cast<unsigned long>(field14),
            static_cast<unsigned long>(field20),
            static_cast<unsigned long>(field24),
            static_cast<unsigned long>(field30));

        // At 006DD8A8, EBP still belongs to sub_6DD894.
        // Walk the actual x86 EBP chain instead of scanning arbitrary stack
        // contents. This should retain:
        //
        //   sub_6DD894
        //   sub_6DC795 request factory
        //   logical asset caller
        //   mainFE/APT caller
        //
        // as long as the upstream functions use frame pointers.
        WriteBootSelectorTraceLine(
            "[BOOT_ORIGIN] ebp_chain start=%08lX\n",
            static_cast<unsigned long>(
                constructorFrame));

        uintptr_t frame = constructorFrame;

        for (unsigned depth = 0;
             depth < 32 && frame;
             ++depth)
        {
            uintptr_t nextFrame = 0;
            uintptr_t returnAddress = 0;

            __try
            {
                nextFrame =
                    *reinterpret_cast<uintptr_t*>(
                        frame);

                returnAddress =
                    *reinterpret_cast<uintptr_t*>(
                        frame + sizeof(uintptr_t));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                WriteBootSelectorTraceLine(
                    "[BOOT_ORIGIN] ebp[%02u] unreadable frame=%08lX\n",
                    depth,
                    static_cast<unsigned long>(frame));
                break;
            }

            WriteBootSelectorTraceLine(
                "[BOOT_ORIGIN] ebp[%02u] frame=%08lX "
                "ret=%08lX%s next=%08lX\n",
                depth,
                static_cast<unsigned long>(frame),
                static_cast<unsigned long>(returnAddress),
                IsLikelyCodeAddress06(returnAddress)
                    ? " EXE"
                    : "",
                static_cast<unsigned long>(nextFrame));

            if (nextFrame <= frame ||
                nextFrame - frame > 0x20000)
            {
                break;
            }

            frame = nextFrame;
        }

        void* frames[64] = {};
        const USHORT captured =
            CaptureStackBackTrace(
                0,
                static_cast<DWORD>(
                    sizeof(frames) /
                    sizeof(frames[0])),
                frames,
                nullptr);

        WriteBootSelectorTraceLine(
            "[BOOT_ORIGIN] capture count=%u\n",
            static_cast<unsigned>(captured));

        for (USHORT i = 0;
             i < captured;
             ++i)
        {
            const uintptr_t address =
                reinterpret_cast<uintptr_t>(
                    frames[i]);

            WriteBootSelectorTraceLine(
                "[BOOT_ORIGIN] capture[%02u]=%08lX%s\n",
                static_cast<unsigned>(i),
                static_cast<unsigned long>(address),
                IsLikelyCodeAddress06(address)
                    ? " EXE"
                    : "");
        }

        WriteBootSelectorTraceLine(
            "[BOOT_ORIGIN] end stage=%s\n",
            stageName);
    }

    __declspec(naked) void BootRequestCtorTraceHook06()
    {
        __asm
        {
            // Hooked at 006DD8A8, immediately after sub_6DC39E.
            // Live registers:
            //   ESI = newly constructed request object
            //   EBP = sub_6DD894 frame

            pushfd
            pushad

            mov     eax, [esp + 4]    // saved ESI
            mov     edx, [esp + 8]    // saved EBP
            push    edx
            push    eax
            call    TraceBootRequestConstructed
            add     esp, 8

            popad
            popfd

            // Original bytes overwritten at 006DD8A8:
            //   8B 45 08    mov eax,[ebp+08]
            //   8B CE       mov ecx,esi
            mov     eax, [ebp + 8]
            mov     ecx, esi

            jmp     kRequestCtorTraceResume06
        }
    }

    bool InstallBootRequestCtorTraceHook06()
    {
        if (g_requestCtorTraceHookInstalled)
            return true;

        unsigned char* target =
            reinterpret_cast<unsigned char*>(
                kRequestCtorTracePatch06);

        const unsigned char expected[5] =
        {
            0x8B, 0x45, 0x08,
            0x8B, 0xCE
        };

        __try
        {
            if (std::memcmp(
                    target,
                    expected,
                    sizeof(expected)) != 0)
            {
                Log(
                    "[AutoStart06] Boot-origin constructor trace "
                    "validation failed at %08X.\n",
                    kRequestCtorTracePatch06);

                return false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        DWORD oldProtect = 0;

        if (!VirtualProtect(
                target,
                sizeof(expected),
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        target[0] = 0xE9;

        *reinterpret_cast<int32_t*>(
            target + 1) =
            static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(
                    &BootRequestCtorTraceHook06) -
                (kRequestCtorTracePatch06 + 5));

        DWORD ignored = 0;

        VirtualProtect(
            target,
            sizeof(expected),
            oldProtect,
            &ignored);

        FlushInstructionCache(
            GetCurrentProcess(),
            target,
            sizeof(expected));

        g_requestCtorTraceHookInstalled = true;

        Log(
            "[AutoStart06] Boot-origin request-constructor trace "
            "installed at %08X.\n",
            kRequestCtorTracePatch06);

        return true;
    }

    void __cdecl RedirectBootPackageAtOpen(uintptr_t* entryStack)
    {
        if (!entryStack)
            return;

        // Optional legacy boot selector tracer. Do not change its results:
        // these refer to the original filename before any substitution.
        __try
        {
            TraceBootSelectorRequest(
                reinterpret_cast<const char*>(entryStack[2]),
                entryStack);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        if (InterlockedCompareExchange(&g_bootAssetOverridesActive, 0, 0) == 0)
            return;

        // Live 06 OpenResolved entry stack:
        // [0] return address; [1] internal type; [2] filename;
        // [3] flags; [4] context.
        // Never mutate filename storage: only replace the stack argument with
        // a pointer to a permanent, read-only relative path string.
        __try
        {
            const char* requestedPath =
                reinterpret_cast<const char*>(entryStack[2]);
            if (!requestedPath)
                return;

            const char* baseName = BaseNameOfPath(requestedPath);
            const char* overridePath = nullptr;
            volatile LONG* count = nullptr;

            if (_stricmp(baseName, kBootSplashPackage) == 0)
            {
                overridePath = g_splashOverridePath;
                count = &g_splashOverrideRequests;
            }
            else if (_stricmp(baseName, kBootHealthPackage) == 0)
            {
                overridePath = g_healthOverridePath;
                count = &g_healthOverrideRequests;
            }

            if (!overridePath)
                return; // Missing override = original game file.

            entryStack[2] = reinterpret_cast<uintptr_t>(overridePath);
            const LONG requestNumber = InterlockedIncrement(count);
            if (requestNumber <= 4)
            {
                Log("[AutoStart06] Boot asset override #%ld: '%s' -> '%s'\n",
                    requestNumber, requestedPath, overridePath);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Invalid pointers should never prevent normal file loading.
        }
    }

    __declspec(naked) void BootOpenResolvedHook06()
    {
        __asm
        {
            pushfd
            pushad

            // Entry ESP before pushfd/pushad is 36 bytes above us.
            lea     eax, [esp + 36]
            push    eax
            call    RedirectBootPackageAtOpen
            add     esp, 4

            popad
            popfd
            jmp     dword ptr [g_bootOpenResolvedContinuation]
        }
    }

    bool InstallBootOpenResolvedHook06()
    {
        if (g_bootOpenResolvedHookInstalled)
            return true;

        constexpr size_t patchLength = 9;

        unsigned char* target =
            reinterpret_cast<unsigned char*>(
                kOpenResolved06);

        __try
        {
            // If CustomPaths/DebugConsole already owns the entry, chain to it.
            if (target[0] == 0xE9)
            {
                const int32_t displacement =
                    *reinterpret_cast<const int32_t*>(
                        target + 1);

                g_bootOpenResolvedContinuation =
                    kOpenResolved06 +
                    5 +
                    displacement;

                Log(
                    "[AutoStart06] Boot OpenResolved hook chaining "
                    "existing JMP destination=%08X.\\n",
                    static_cast<unsigned int>(
                        g_bootOpenResolvedContinuation));
            }
            else
            {
                // Stock NBA Live 06 prologue:
                //
                // 006DC8F9  55
                // 006DC8FA  8B EC
                // 006DC8FC  81 EC 08 01 00 00
                const unsigned char expected[patchLength] =
                {
                    0x55,
                    0x8B, 0xEC,
                    0x81, 0xEC, 0x08, 0x01, 0x00, 0x00
                };

                if (std::memcmp(
                        target,
                        expected,
                        patchLength) != 0)
                {
                    Log(
                        "[AutoStart06] Boot OpenResolved hook validation "
                        "failed at %08X.\\n",
                        kOpenResolved06);
                    return false;
                }

                unsigned char* trampoline =
                    reinterpret_cast<unsigned char*>(
                        VirtualAlloc(
                            nullptr,
                            32,
                            MEM_COMMIT | MEM_RESERVE,
                            PAGE_EXECUTE_READWRITE));

                if (!trampoline)
                    return false;

                std::memcpy(
                    trampoline,
                    target,
                    patchLength);

                trampoline[patchLength] = 0xE9;

                *reinterpret_cast<int32_t*>(
                    trampoline +
                    patchLength +
                    1) =
                    static_cast<int32_t>(
                        (kOpenResolved06 + patchLength) -
                        reinterpret_cast<uintptr_t>(
                            trampoline +
                            patchLength +
                            5));

                g_bootOpenResolvedContinuation =
                    reinterpret_cast<uintptr_t>(
                        trampoline);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        DWORD oldProtect = 0;

        if (!VirtualProtect(
                target,
                patchLength,
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        target[0] = 0xE9;

        *reinterpret_cast<int32_t*>(
            target + 1) =
            static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(
                    &BootOpenResolvedHook06) -
                (kOpenResolved06 + 5));

        for (size_t i = 5; i < patchLength; ++i)
            target[i] = 0x90;

        DWORD ignored = 0;

        VirtualProtect(
            target,
            patchLength,
            oldProtect,
            &ignored);

        FlushInstructionCache(
            GetCurrentProcess(),
            target,
            patchLength);

        g_bootOpenResolvedHookInstalled = true;

        Log(
            "[AutoStart06] Selective autostart asset override hook "
            "installed at %08X.\n",
            kOpenResolved06);

        return true;
    }
#else
    bool InstallBootRequestCtorTraceHook06()
    {
        return false;
    }

    bool InstallBootOpenResolvedHook06()
    {
        return false;
    }
#endif

    bool IsReadableCommittedProtection(DWORD protect)
    {
        if (protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;

        const DWORD baseProtect = protect & 0xFF;

        return baseProtect == PAGE_READONLY ||
               baseProtect == PAGE_READWRITE ||
               baseProtect == PAGE_WRITECOPY ||
               baseProtect == PAGE_EXECUTE ||
               baseProtect == PAGE_EXECUTE_READ ||
               baseProtect == PAGE_EXECUTE_READWRITE ||
               baseProtect == PAGE_EXECUTE_WRITECOPY;
    }

    bool ReadRuntimeByte(
        const unsigned char* address,
        unsigned char& value)
    {
        if (!address)
            return false;

        SIZE_T bytesRead = 0;

        return ReadProcessMemory(
                   GetCurrentProcess(),
                   address,
                   &value,
                   sizeof(value),
                   &bytesRead) != FALSE &&
               bytesRead == sizeof(value);
    }

    bool WriteRuntimeByte(
        unsigned char* address,
        unsigned char value)
    {
        if (!address)
            return false;

        MEMORY_BASIC_INFORMATION mbi = {};

        if (!VirtualQuery(address, &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT ||
            !IsReadableCommittedProtection(mbi.Protect))
        {
            return false;
        }

        DWORD oldProtect = 0;

        if (!VirtualProtect(
                address,
                sizeof(value),
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        SIZE_T bytesWritten = 0;

        const BOOL writeOk =
            WriteProcessMemory(
                GetCurrentProcess(),
                address,
                &value,
                sizeof(value),
                &bytesWritten);

        if (writeOk && bytesWritten == sizeof(value))
        {
            FlushInstructionCache(
                GetCurrentProcess(),
                address,
                sizeof(value));
        }

        DWORD ignored = 0;

        // If the allocation changed underneath us, restoring protection can
        // legitimately fail. Do not dereference the address in that case.
        VirtualProtect(
            address,
            sizeof(value),
            oldProtect,
            &ignored);

        return writeOk != FALSE &&
               bytesWritten == sizeof(value);
    }

    void RestoreTitleMainMenuBranch()
    {
        if (!g_titleBranchPatchCount)
            return;

        size_t restored = 0;

        for (size_t i = 0; i < g_titleBranchPatchCount; ++i)
        {
            TitleBranchPatch& patchInfo = g_titleBranchPatches[i];

            if (!patchInfo.operand)
                continue;

            unsigned char current = 0;

            if (ReadRuntimeByte(
                    patchInfo.operand,
                    current) &&
                current == kTitleBranchSkipMainMenuOffset &&
                WriteRuntimeByte(
                    patchInfo.operand,
                    patchInfo.original))
            {
                ++restored;
            }

            patchInfo.operand = nullptr;
            patchInfo.original = 0;
        }

        Log(
            "[AutoStart06] Restored title MAIN_MENU branch patch: "
            "%u location(s).\n",
            static_cast<unsigned>(restored));

        g_titleBranchPatchCount = 0;
    }

    bool PatchLiveTitleMainMenuBranch()
    {
        if (g_titleBranchPatchCount)
            return true;

        SYSTEM_INFO systemInfo = {};
        GetSystemInfo(&systemInfo);

        const uintptr_t minimum =
            reinterpret_cast<uintptr_t>(
                systemInfo.lpMinimumApplicationAddress);

        const uintptr_t maximum =
            reinterpret_cast<uintptr_t>(
                systemInfo.lpMaximumApplicationAddress);

        constexpr SIZE_T kScanChunk = 64 * 1024;
        constexpr SIZE_T kPatternSize =
            sizeof(kTitleStartupBranchPattern);
        constexpr SIZE_T kOverlap =
            kPatternSize > 0 ? kPatternSize - 1 : 0;

        std::vector<unsigned char> snapshot(
            kScanChunk + kOverlap);

        HANDLE process = GetCurrentProcess();
        uintptr_t cursor = minimum;
        size_t matches = 0;
        size_t skippedChangingRegions = 0;
        SIZE_T snapshotBytesRead = 0;
        const DWORD scanStartedAt = GetTickCount();

        while (cursor < maximum)
        {
            MEMORY_BASIC_INFORMATION mbi = {};

            if (!VirtualQuery(
                    reinterpret_cast<const void*>(cursor),
                    &mbi,
                    sizeof(mbi)))
            {
                break;
            }

            const uintptr_t regionStart =
                reinterpret_cast<uintptr_t>(mbi.BaseAddress);

            const SIZE_T regionSize =
                static_cast<SIZE_T>(mbi.RegionSize);

            uintptr_t regionEnd = regionStart;

            if (regionSize >
                static_cast<SIZE_T>(
                    static_cast<uintptr_t>(-1) - regionStart))
            {
                regionEnd = maximum;
            }
            else
            {
                regionEnd =
                    regionStart +
                    static_cast<uintptr_t>(regionSize);
            }

            if (regionEnd <= cursor)
                break;

            // IMPORTANT:
            //
            // Never compare directly against live heap memory here.
            // title.apt lives in MEM_PRIVATE storage, but unrelated private
            // allocations can be committed/freed by other game threads while
            // this callback runs.  v6 used std::memcmp(candidate, ...) on the
            // live region and could cross into a page that disappeared after
            // VirtualQuery.
            //
            // v7 snapshots each chunk through ReadProcessMemory first. If a
            // page changes, ReadProcessMemory simply fails/returns a partial
            // copy instead of raising an access violation in our ASI.
            if (mbi.State == MEM_COMMIT &&
                mbi.Type == MEM_PRIVATE &&
                IsReadableCommittedProtection(mbi.Protect) &&
                regionSize >= kPatternSize)
            {
                SIZE_T regionOffset = 0;

                while (regionOffset < regionSize)
                {
                    const SIZE_T remaining =
                        regionSize - regionOffset;

                    const SIZE_T wanted =
                        std::min<SIZE_T>(
                            remaining,
                            kScanChunk + kOverlap);

                    SIZE_T bytesRead = 0;

                    const uintptr_t readAddress =
                        regionStart +
                        static_cast<uintptr_t>(regionOffset);

                    const BOOL readOk =
                        ReadProcessMemory(
                            process,
                            reinterpret_cast<const void*>(
                                readAddress),
                            snapshot.data(),
                            wanted,
                            &bytesRead);

                    snapshotBytesRead += bytesRead;

                    // ERROR_PARTIAL_COPY is possible if another thread
                    // changes a page between VirtualQuery and this read.
                    // A returned partial prefix is still safe to inspect.
                    if (bytesRead >= kPatternSize)
                    {
                        const SIZE_T scanLimit =
                            bytesRead - kPatternSize;

                        // Fast byte prefilter: avoid calling memcmp() for
                        // every individual byte across the process heap.
                        // memchr() uses the local ReadProcessMemory snapshot,
                        // never a live (potentially changing) game pointer.
                        for (SIZE_T offset = 0; offset <= scanLimit; )
                        {
                            const unsigned char* hit =
                                static_cast<const unsigned char*>(
                                    std::memchr(
                                        snapshot.data() + offset,
                                        kTitleStartupBranchPattern[0],
                                        scanLimit + 1 - offset));

                            if (!hit)
                                break;

                            const SIZE_T candidateOffset =
                                static_cast<SIZE_T>(hit - snapshot.data());
                            offset = candidateOffset + 1;

                            if (std::memcmp(
                                    hit,
                                    kTitleStartupBranchPattern,
                                    kPatternSize) != 0)
                            {
                                continue;
                            }

                            const uintptr_t candidateAddress =
                                readAddress +
                                static_cast<uintptr_t>(candidateOffset);

                            unsigned char* operand =
                                reinterpret_cast<unsigned char*>(
                                    candidateAddress +
                                    kTitleBranchOperandOffset);

                            unsigned char currentOperand = 0;

                            if (!ReadRuntimeByte(
                                    operand,
                                    currentOperand) ||
                                currentOperand !=
                                    kTitleBranchStockOffset)
                            {
                                continue;
                            }

                            if (g_titleBranchPatchCount >=
                                (sizeof(g_titleBranchPatches) /
                                 sizeof(g_titleBranchPatches[0])))
                            {
                                break;
                            }

                            TitleBranchPatch& patchInfo =
                                g_titleBranchPatches[
                                    g_titleBranchPatchCount];

                            patchInfo.operand = operand;
                            patchInfo.original = currentOperand;

                            if (!WriteRuntimeByte(
                                    operand,
                                    kTitleBranchSkipMainMenuOffset))
                            {
                                patchInfo.operand = nullptr;
                                patchInfo.original = 0;
                                continue;
                            }

                            ++g_titleBranchPatchCount;
                            ++matches;

                            Log(
                                "[AutoStart06] Suppressed title MAIN_MENU "
                                "action at runtime stream=%p "
                                "branchOperand=%p (1C -> 25).\n",
                                reinterpret_cast<void*>(
                                    candidateAddress),
                                operand);

                            // This candidate was already handled. Skip its
                            // complete signature rather than rescanning it.
                            offset = candidateOffset + kPatternSize;
                        }
                    }

                    if (!readOk && bytesRead == 0)
                    {
                        ++skippedChangingRegions;
                        break;
                    }

                    // Advance by the non-overlap portion. The next snapshot
                    // re-reads kOverlap bytes so a signature spanning two
                    // chunks cannot be missed.
                    if (remaining <= kScanChunk)
                        break;

                    regionOffset += kScanChunk;
                }
            }

            cursor = regionEnd;
        }

        const DWORD scanDurationMs = GetTickCount() - scanStartedAt;
        Log(
            "[AutoStart06] TIMING title MAIN_MENU scan: %lu ms, "
            "snapshots=%llu bytes, matches=%u.\n",
            static_cast<unsigned long>(scanDurationMs),
            static_cast<unsigned long long>(snapshotBytesRead),
            static_cast<unsigned>(matches));

        if (!matches)
        {
            Log(
                "[AutoStart06] WARNING: live title MAIN_MENU action stream "
                "was not found. Autostart will still use the native "
                "GD_GAME_INIT handoff, but MAIN_MENU suppression was not "
                "applied. changingRegionsSkipped=%u\n",
                static_cast<unsigned>(skippedChangingRegions));

            return false;
        }

        Log(
            "[AutoStart06] Title MAIN_MENU suppression active: "
            "%u runtime copy/copies patched. "
            "changingRegionsSkipped=%u\n",
            static_cast<unsigned>(matches),
            static_cast<unsigned>(skippedChangingRegions));

        return true;
    }

    // Original 0x005CBA30:
    //
    //   mov ecx,[esp+4]
    //   push 0                    ; STARTUP_SCREEN_MAINMENU
    //   call 005C8C60
    //   ret
    //
    // This function is called by the title/startup APT flow to obtain the
    // destination screen. We preserve the stock MAINMENU return value and
    // merely latch a one-shot request. The actual game-state transition is
    // deferred until the current FESystem::Update() finishes.
    void __cdecl GetStartupScreen_AutoStart(void* aptCall)
    {
        void* feSystem =
            *reinterpret_cast<void**>(kFESystemGlobal);

        const int feState = ReadFEState(feSystem);
        const int gdState = ReadGDState(feSystem);

        // title.const asks GetStartupScreen() during FE_INIT.  That is not
        // itself the "title finished" moment, so use it only to arm the
        // one-shot autostart request.  The FE_RUN update hook below performs
        // the actual transition after the title has been live for the
        // configured delay.
        if (g_enabled &&
            !g_consumed &&
            !g_pending &&
            feSystem)
        {
            g_pending = true;
            g_feRunSeenAt = 0;
            g_titleStartupCallbackAt = GetTickCount();
            Log(
                "[AutoStart06] TIMING title GetStartupScreen at +%lu ms "
                "since launcher initialization.\n",
                static_cast<unsigned long>(
                    g_titleStartupCallbackAt - g_autoStartInitializedAt));

            // End this one-shot boot override at the Title lifecycle, before
            // any later menu navigation can request the boot files again.
            if (InterlockedExchange(&g_bootAssetOverridesActive, 0) != 0)
            {
                Log("[AutoStart06] Title initialized: boot asset overrides "
                    "disabled (splash requests=%ld, health requests=%ld).\n",
                    InterlockedCompareExchange(&g_splashOverrideRequests, 0, 0),
                    InterlockedCompareExchange(&g_healthOverrideRequests, 0, 0));
            }

            Log(
                "[AutoStart06] Title GetStartupScreen observed. "
                "Autostart ARMED. FE=%d GD=%d; suppressing MAIN_MENU "
                "and waiting for FE_RUN.\n",
                feState,
                gdState);

            PatchLiveTitleMainMenuBranch();
            Log(
                "[AutoStart06] TIMING GetStartupScreen after "
                "MAIN_MENU scan: +%lu ms since callback.\n",
                static_cast<unsigned long>(
                    GetTickCount() - g_titleStartupCallbackAt));

            DumpUberState("title-arm");
        }
        else
        {
            Log(
                "[AutoStart06] GetStartupScreen: enabled=%d "
                "consumed=%d pending=%d FE=%d GD=%d\n",
                g_enabled ? 1 : 0,
                g_consumed ? 1 : 0,
                g_pending ? 1 : 0,
                feState,
                gdState);
        }

        // Preserve the stock title script result:
        // STARTUP_SCREEN_MAINMENU == 0.
        //
        // title.const will still believe its normal destination is MAIN_MENU;
        // our native FE_RUN transition is consumed before that frontend flow
        // becomes the user's next interactive screen.
        reinterpret_cast<void(__thiscall*)(void*, int)>(
            kSetAptEnumReturn)(
                aptCall,
                0);
    }

    // Replacement only for the GameDirector GD_FE_RUN call site at 005F6AAF.
    // ABI at the original site:
    //
    //   ECX = FESystem*
    //   push delta
    //   call FESystem::Update
    //
    // A __fastcall wrapper preserves ECX and pops the single stack argument.
    int __fastcall FESystemUpdate_AutoStart(
        void* feSystem,
        void* /*edx*/,
        int delta)
    {
        const int originalResult =
            reinterpret_cast<int(__thiscall*)(void*, int)>(
                kFESystemUpdate)(
                    feSystem,
                    delta);

        if (!g_enabled ||
            g_consumed ||
            !g_pending)
        {
            return originalResult;
        }

        const int feStateBefore =
            ReadFEState(feSystem);

        const int gdStateBefore =
            ReadGDState(feSystem);

        // Do nothing until the title/frontend has actually reached FE_RUN.
        // This is the correction from v2: GetStartupScreen() itself occurs in
        // FE_INIT on the supplied title package.
        if (feStateBefore != kStateFeRun ||
            originalResult != kGDFeRun)
        {
            return originalResult;
        }

        const DWORD now = GetTickCount();

        // AUTO_START_DELAY_MS=0 means: launch on the first safe FE_RUN
        // boundary.  The title package has already initialized by this point,
        // but we do not allow another timed FE_RUN interval in which the
        // frontend can advance toward MAIN_MENU.
        if (g_feRunSeenAt == 0)
        {
            g_feRunSeenAt = now ? now : 1;
            Log(
                "[AutoStart06] TIMING first safe FE_RUN: +%lu ms "
                "since title callback, +%lu ms since initialization.\n",
                static_cast<unsigned long>(
                    now - g_titleStartupCallbackAt),
                static_cast<unsigned long>(
                    now - g_autoStartInitializedAt));

            if (g_autoStartDelayMs != 0)
            {
                Log(
                    "[AutoStart06] Title reached FE_RUN. "
                    "Holding it on screen for %lu ms before game start.\n",
                    static_cast<unsigned long>(g_autoStartDelayMs));

                DumpUberState("title-visible");
                return originalResult;
            }

            Log(
                "[AutoStart06] Title reached FE_RUN with zero delay. "
                "Launching immediately at the first safe FE_RUN boundary.\n");
        }

        const DWORD elapsed =
            (g_autoStartDelayMs == 0)
                ? 0
                : (now - g_feRunSeenAt);

        if (g_autoStartDelayMs != 0 &&
            elapsed < g_autoStartDelayMs)
        {
            return originalResult;
        }

        g_pending = false;
        g_consumed = true;

        if (g_autoStartDelayMs == 0)
        {
            Log(
                "[AutoStart06] Zero-delay title handoff: "
                "Returning GD_GAME_INIT=14 to GameDirector WITHOUT changing "
                "FESystem state.\n");
        }
        else
        {
            Log(
                "[AutoStart06] Title display delay complete (%lu ms). "
                "Returning GD_GAME_INIT=14 to GameDirector WITHOUT changing "
                "FESystem state.\n",
                static_cast<unsigned long>(elapsed));
        }

        DumpUberState("before-gd-game-init-request");

        // The title's non-online branch has already executed by now.
        // Restore its bytecode before frontend teardown so this remains a
        // strictly one-shot runtime modification.
        RestoreTitleMainMenuBranch();

        // IMPORTANT:
        //
        // Do NOT call FESystem::SetState(STATE_GAME_INIT) here.
        //
        // Native GameDirector sequencing is:
        //
        //   GD_FE_RUN:
        //       result = FESystem::Update(...)
        //       if result != GD_FE_RUN:
        //           nextState    = result
        //           currentState = GD_FE_TERM
        //
        //   GD_FE_TERM:
        //       FESystem::SetState(STATE_FE_TERM)
        //       currentState = nextState
        //
        //   GD_GAME_INIT -> GD_GAME_SYNC_ENTRY -> GD_GAME_SYNC_ENTRY_WAIT
        //
        //   GD_GAME_SYNC_ENTRY_WAIT:
        //       native game setup (0x005F6730)
        //       -> mode-specific preparation
        //       -> FESystem::SetState(STATE_GAME_INIT)
        //       -> later STATE_GAME_RUN
        //
        // Our v3 implementation called SetState(9) here, while the frontend
        // was still live.  GameDirector then entered GD_FE_TERM and called
        // SetState(STATE_FE_TERM) on an FESystem that had already been moved
        // into GAME_INIT.  The resulting teardown hit an indirect null call.
        //
        // The APT load-game action's essential state-machine effect is the
        // return value, not an early SetState(9).
        const int feStateStill =
            ReadFEState(feSystem);

        const int gdMappedStill =
            ReadGDState(feSystem);

        Log(
            "[AutoStart06] Synthetic director request: "
            "return=14, FESystem remains FE=%d mappedGD=%d "
            "(expected FE=4 mappedGD=5).\n",
            feStateStill,
            gdMappedStill);

        return kGDGameInit;
    }

    bool VerifyOriginalBytes()
    {
        // 005CBA30:
        //   8B 4C 24 04 6A 00 ...
        const unsigned char startupExpected[] =
            { 0x8B, 0x4C, 0x24, 0x04, 0x6A };

        // 005F6AAF:
        //   E8 CC 1E FA FF   call 00598980
        const unsigned char updateExpected[] =
            { 0xE8, 0xCC, 0x1E, 0xFA, 0xFF };

        if (g_enabled)
        {
            for (unsigned int i = 0;
                 i < sizeof(startupExpected);
                 ++i)
            {
                if (patch::GetUChar(kGetStartupScreen + i) !=
                    startupExpected[i])
                {
                    Log(
                        "[AutoStart06] Refusing install: "
                        "GetStartupScreen bytes differ at %08X.\n",
                        kGetStartupScreen + i);
                    return false;
                }
            }

            for (unsigned int i = 0;
                 i < sizeof(updateExpected);
                 ++i)
            {
                if (patch::GetUChar(kFEUpdateCall + i) !=
                    updateExpected[i])
                {
                    Log(
                        "[AutoStart06] Refusing install: "
                        "FE_RUN Update call bytes differ at %08X.\n",
                        kFEUpdateCall + i);
                    return false;
                }
            }

        }

        return true;
    }
}

void InitializeAutoStartGame()
{
    if (!IsLive06())
        return;

    g_autoStartInitializedAt = GetTickCount();
    g_titleStartupCallbackAt = 0;

    const std::string iniPath = GetMainIniPath();

    char rawValue[64] = {};
    const DWORD rawLength =
        GetPrivateProfileStringA(
            "STARTUP",
            "AUTO_START_GAME",
            "<missing>",
            rawValue,
            static_cast<DWORD>(sizeof(rawValue)),
            iniPath.c_str());

    const DWORD attributes = GetFileAttributesA(iniPath.c_str());
    const bool iniExists =
        attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;

    const int iniValue =
        GetPrivateProfileIntA(
            "STARTUP",
            "AUTO_START_GAME",
            0,
            iniPath.c_str());

    int delayValue =
        GetPrivateProfileIntA(
            "STARTUP",
            "AUTO_START_DELAY_MS",
            750,
            iniPath.c_str());

    const int bootSelectorTraceValue =
        GetPrivateProfileIntA(
            "STARTUP",
            "BOOT_SELECTOR_TRACE",
            0,
            iniPath.c_str());

    g_bootSelectorTrace =
        bootSelectorTraceValue != 0;

    if (delayValue < 0)
        delayValue = 0;
    if (delayValue > 10000)
        delayValue = 10000;

    g_autoStartDelayMs =
        static_cast<DWORD>(delayValue);

    g_enabled = iniValue != 0;

    g_pending = false;
    g_consumed = false;
    g_feRunSeenAt = 0;
    InterlockedExchange(&g_splashOverrideRequests, 0);
    InterlockedExchange(&g_healthOverrideRequests, 0);
    g_tracedLegal = false;
    g_tracedNbaLogo = false;
    g_tracedTitle = false;

    g_splashOverridePath = g_enabled
        ? ChooseBootOverride06(kSplashAssetNative, kSplashAssetShort)
        : nullptr;
    g_healthOverridePath = g_enabled
        ? ChooseBootOverride06(kHealthAssetNative, kHealthAssetShort)
        : nullptr;
    InterlockedExchange(
        &g_bootAssetOverridesActive,
        (g_enabled && (g_splashOverridePath || g_healthOverridePath)) ? 1 : 0);

    g_titleBranchPatchCount = 0;
    std::memset(
        g_titleBranchPatches,
        0,
        sizeof(g_titleBranchPatches));

    char cwd[MAX_PATH] = {};
    GetCurrentDirectoryA(
        static_cast<DWORD>(sizeof(cwd)),
        cwd);

    Log(
        "[AutoStart06] INI path='%s' exists=%d cwd='%s' "
        "raw AUTO_START_GAME='%s' rawLen=%lu parsed=%d\n",
        iniPath.c_str(),
        iniExists ? 1 : 0,
        cwd,
        rawValue,
        static_cast<unsigned long>(rawLength),
        iniValue);

    Log(
        "[AutoStart06] Initialize: AUTO_START_GAME=%d "
        "AUTO_START_DELAY_MS=%lu "
        "ASSET_OVERRIDE=%d BOOT_SELECTOR_TRACE=%d\n",
        g_enabled ? 1 : 0,
        static_cast<unsigned long>(g_autoStartDelayMs),
        g_bootAssetOverridesActive ? 1 : 0,
        g_bootSelectorTrace ? 1 : 0);

    if (g_enabled)
    {
        Log("[AutoStart06] Asset overrides: splash='%s' health='%s'.\n",
            g_splashOverridePath ? g_splashOverridePath : "<stock>",
            g_healthOverridePath ? g_healthOverridePath : "<stock>");
        if (!g_splashOverridePath || !g_healthOverridePath)
            Log("[AutoStart06] WARNING: one or both custom boot assets are "
                "missing; missing files will use the stock package.\n");
    }

    if (!g_enabled && !g_bootSelectorTrace)
        return;

    if (!VerifyOriginalBytes())
        return;

    if (g_bootSelectorTrace)
    {
        if (!InstallBootRequestCtorTraceHook06())
        {
            Log(
                "[AutoStart06] WARNING: BOOT_SELECTOR_TRACE requested "
                "but the request-constructor hook could not be installed.\n");
        }
    }

    if (g_bootAssetOverridesActive || g_bootSelectorTrace)
    {
        if (!InstallBootOpenResolvedHook06())
        {
            InterlockedExchange(&g_bootAssetOverridesActive, 0);
            Log("[AutoStart06] ERROR: asset override hook installation failed; "
                "boot uses stock BIG files.\n");
        }
    }

    if (g_enabled)
    {
        // Preserve the already working Title -> GD_GAME_INIT=14 handoff.
        patch::RedirectJump(
            kGetStartupScreen,
            reinterpret_cast<void*>(&GetStartupScreen_AutoStart));
        patch::RedirectCall(
            kFEUpdateCall,
            reinterpret_cast<void*>(&FESystemUpdate_AutoStart));
    }

    Log(
        "[AutoStart06] Hooks installed: "
        "GetStartupScreen=%d FE_RUN_UpdateCall=%d "
        "Asset_Override=%d\n",
        g_enabled ? 1 : 0,
        g_enabled ? 1 : 0,
        g_bootAssetOverridesActive ? 1 : 0);
}
