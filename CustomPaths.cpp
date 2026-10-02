#include "plugin-std.h"

#include <Windows.h>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>

using namespace plugin;

namespace custompaths
{
    enum GameVersion
    {
        GAME_UNKNOWN,
        GAME_LIVE_2005,
        GAME_LIVE_06,
        GAME_LIVE_07,
        GAME_LIVE_08
    };

    enum OverrideSlot
    {
        SLOT_STAGE_COURT,
        SLOT_STAGE_BACKBOARD,
        SLOT_STAGE_DORNA,
        SLOT_PLAYOFF_COURT,
        SLOT_PLAYOFF_BACKBOARD,
        SLOT_PLAYOFF_DORNA,
        SLOT_REGULAR_COURT,
        SLOT_REGULAR_BACKBOARD,
        SLOT_REGULAR_DORNA,
        SLOT_COUNT
    };

    enum PostseasonStage
    {
        STAGE_REGULAR,
        STAGE_PLAYOFF,
        STAGE_ECF,
        STAGE_WCF,
        STAGE_FINALS
    };

    const size_t kOverridePathCapacity = 96;
    const size_t kOverrideCacheCapacity = 256;

    // These paths are never registered with the game's global filesystem.
    // They are only consulted by the direct OpenResolved override hook.
    char gOverridePaths[SLOT_COUNT][kOverridePathCapacity] = {};

    struct OverrideCacheEntry
    {
        char logicalName[128];
        char resolvedPath[MAX_PATH];
        bool found;
    };

    OverrideCacheEntry gOverrideCache[kOverrideCacheCapacity] = {};
    unsigned int gOverrideCacheCount = 0;
    unsigned int gOverrideCacheCursor = 0;
    bool gGameContextValid = false;
    char gCurrentStadiumId4[5] = {};
    PostseasonStage gCurrentStage = STAGE_REGULAR;

    GameVersion gGameVersion = GAME_UNKNOWN;

#if defined(_M_IX86)
    // Resolve custom stadium files directly at OpenResolved entry. The nine
    // directories are no longer registered as global search paths, so normal
    // assets pay no custom-path enumeration cost.
    uintptr_t gOpenResolvedContinuation = 0;
    bool gOpenResolvedGateInstalled = false;
#endif

    const unsigned int kLive08GameCourtNameCalls[] = {
        0x0066F677, 0x00670EC2, 0x00671532, 0x00671F12,
        0x0067671B, 0x0067743D, 0x006780D3, 0x00678613
    };
    const unsigned int kLive08GameCourtSetStadiumName = 0x0069EFC0;

    template <unsigned int GetterAddress, unsigned int AddPathAddress>
    DWORD* AddCustomPathsForBuild()
    {
        DWORD* fsm = CallAndReturn<DWORD*, GetterAddress>();

        // Faces remains a normal global search path. Stadium overrides are
        // handled directly and are deliberately absent from this list.
        CallMethod<AddPathAddress>(fsm, "faces\\");
        return fsm;
    }

    static DWORD* AddCustomPaths05()
    {
        return AddCustomPathsForBuild<0x429E10, 0x429E90>();
    }

    static DWORD* AddCustomPaths06()
    {
        return AddCustomPathsForBuild<0x429F50, 0x429FD0>();
    }

    static DWORD* AddCustomPaths07()
    {
        return AddCustomPathsForBuild<0x44DCA0, 0x44DD20>();
    }

    static DWORD* AddCustomPaths08()
    {
        return AddCustomPathsForBuild<0x450500, 0x450580>();
    }

    bool IsValidId4(const char* value)
    {
        if (!value)
            return false;

        for (int i = 0; i < 4; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (c == '\0' || !std::isalnum(c))
                return false;
        }
        return value[4] == '\0';
    }

    void NormalizeId4(const char* source, char* destination)
    {
        for (int i = 0; i < 4; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(source[i]);
            destination[i] = static_cast<char>(std::tolower(c));
        }
        destination[4] = '\0';
    }

    void* GetCurrentOptions(void* suppliedOptions)
    {
        if (suppliedOptions)
            return suppliedOptions;

        __try
        {
            if (gGameVersion == GAME_LIVE_07)
            {
                void* game = *reinterpret_cast<void**>(0x00CEBBB0);
                return game
                    ? *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(game) + 0x0AC0)
                    : nullptr;
            }

            if (gGameVersion == GAME_LIVE_08)
            {
                void* game = *reinterpret_cast<void**>(0x00DA7A20);
                return game
                    ? *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(game) + 0x0B70)
                    : nullptr;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }

        return nullptr;
    }

    PostseasonStage GetPostseasonStage(void* suppliedOptions)
    {
        void* options = GetCurrentOptions(suppliedOptions);
        if (!options)
            return STAGE_REGULAR;

        const unsigned int modeOffset =
            gGameVersion == GAME_LIVE_08 ? 0x64 : 0x5C;
        const unsigned int roundOffset =
            gGameVersion == GAME_LIVE_08 ? 0x168 : 0x158;

        __try
        {
            const int mode = *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(options) + modeOffset);
            const int round = *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(options) + roundOffset);

            // These are the same fields used by the games' native playoff-logo
            // selection. The bracket enumerates East then West, with the NBA
            // Finals last. A normal exhibition uses the round sentinel 16.
            if (mode != 2 && mode != 4)
                return STAGE_REGULAR;

            if (round == 6)
                return STAGE_FINALS;
            if (round == 4)
                return STAGE_ECF;
            if (round == 5)
                return STAGE_WCF;
            if (round >= 0 && round <= 3)
                return STAGE_PLAYOFF;

            return STAGE_REGULAR;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return STAGE_REGULAR;
        }
    }

    const char* GetStageFolder(PostseasonStage stage)
    {
        switch (stage)
        {
        case STAGE_ECF:
            return "ECF";
        case STAGE_WCF:
            return "WCF";
        case STAGE_FINALS:
            return "finals";
        default:
            return nullptr;
        }
    }

    void SetSlot(OverrideSlot slot, const char* value)
    {
        strcpy_s(
            gOverridePaths[slot],
            kOverridePathCapacity,
            value ? value : "");
    }

    void DisableAllSlots()
    {
        for (unsigned int i = 0; i < SLOT_COUNT; ++i)
            gOverridePaths[i][0] = '\0';

        gGameContextValid = false;
        gCurrentStadiumId4[0] = '\0';
        gCurrentStage = STAGE_REGULAR;
        gOverrideCacheCount = 0;
        gOverrideCacheCursor = 0;
        std::memset(gOverrideCache, 0, sizeof(gOverrideCache));
    }

    void UpdateGameContext(const char* stadiumId4, void* suppliedOptions)
    {
        if (!IsValidId4(stadiumId4))
        {
            DisableAllSlots();
            return;
        }

        char id4[5] = {};
        NormalizeId4(stadiumId4, id4);
        const PostseasonStage stage = GetPostseasonStage(suppliedOptions);
        const bool playoffs = stage != STAGE_REGULAR;
        const char* stageFolder = GetStageFolder(stage);

        // Repeated GameCourt notifications for the same arena/stage do not
        // invalidate an already-warm filename cache.
        if (gGameContextValid &&
            _stricmp(gCurrentStadiumId4, id4) == 0 &&
            gCurrentStage == stage)
        {
            return;
        }

        DisableAllSlots();
        gGameContextValid = true;
        strcpy_s(gCurrentStadiumId4, sizeof(gCurrentStadiumId4), id4);
        gCurrentStage = stage;

        char path[kOverridePathCapacity] = {};

        if (stageFolder)
        {
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\%s\\court\\",
                id4,
                stageFolder);
            SetSlot(SLOT_STAGE_COURT, path);
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\%s\\backboard\\",
                id4,
                stageFolder);
            SetSlot(SLOT_STAGE_BACKBOARD, path);
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\%s\\dorna\\",
                id4,
                stageFolder);
            SetSlot(SLOT_STAGE_DORNA, path);
        }
        else
        {
            SetSlot(SLOT_STAGE_COURT, "");
            SetSlot(SLOT_STAGE_BACKBOARD, "");
            SetSlot(SLOT_STAGE_DORNA, "");
        }

        if (playoffs)
        {
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\playoff\\court\\",
                id4);
            SetSlot(SLOT_PLAYOFF_COURT, path);
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\playoff\\backboard\\",
                id4);
            SetSlot(SLOT_PLAYOFF_BACKBOARD, path);
            sprintf_s(
                path,
                sizeof(path),
                "assets\\stadia\\%s\\playoff\\dorna\\",
                id4);
            SetSlot(SLOT_PLAYOFF_DORNA, path);
        }
        else
        {
            SetSlot(SLOT_PLAYOFF_COURT, "");
            SetSlot(SLOT_PLAYOFF_BACKBOARD, "");
            SetSlot(SLOT_PLAYOFF_DORNA, "");
        }

        sprintf_s(
            path,
            sizeof(path),
            "assets\\stadia\\%s\\court\\",
            id4);
        SetSlot(SLOT_REGULAR_COURT, path);
        sprintf_s(
            path,
            sizeof(path),
            "assets\\stadia\\%s\\backboard\\",
            id4);
        SetSlot(SLOT_REGULAR_BACKBOARD, path);
        sprintf_s(
            path,
            sizeof(path),
            "assets\\stadia\\%s\\dorna\\",
            id4);
        SetSlot(SLOT_REGULAR_DORNA, path);

        char message[192] = {};
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] Team '%s': stage=%s asset paths selected.\n",
            id4,
            stage == STAGE_FINALS ? "finals" :
            stage == STAGE_ECF ? "ECF" :
            stage == STAGE_WCF ? "WCF" :
            stage == STAGE_PLAYOFF ? "playoff" : "regular");
        OutputDebugStringA(message);
    }

    bool ValidateCallSites(
        const unsigned int* sites,
        size_t count,
        unsigned int expectedTarget)
    {
        for (size_t i = 0; i < count; ++i)
        {
            if (patch::GetUChar(sites[i]) != 0xE8)
                return false;

            const int displacement = patch::GetInt(sites[i] + 1);
            if (sites[i] + 5 + displacement != expectedTarget)
                return false;
        }
        return true;
    }

    bool ContainsCaseInsensitive(const char* text, const char* token)
    {
        if (!text || !token || token[0] == '\0')
            return false;

        for (const char* start = text; *start; ++start)
        {
            const char* left = start;
            const char* right = token;
            while (*left && *right)
            {
                const unsigned char a = static_cast<unsigned char>(*left);
                const unsigned char b = static_cast<unsigned char>(*right);
                if (std::tolower(a) != std::tolower(b))
                    break;
                ++left;
                ++right;
            }

            if (*right == '\0')
                return true;
        }
        return false;
    }

    bool ShouldSearchStadiumPaths(const char* requestedFilename)
    {
        static const char* const tokens[] = {
            "bbd",
            "crt",
            "dorna",
            "std",
            "playoff",
            "finals",
            "crowd3d",
            "crowdbase",
            "conffinals"
        };

        for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); ++i)
        {
            if (ContainsCaseInsensitive(requestedFilename, tokens[i]))
                return true;
        }
        return false;
    }

    const char* BaseNameOf(const char* path)
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

    const char* ResolveCachedCustomPath(const char* requestedPath)
    {
        if (!gGameContextValid || !requestedPath)
            return nullptr;

        const char* logicalName = BaseNameOf(requestedPath);
        if (!*logicalName || !ShouldSearchStadiumPaths(logicalName))
            return nullptr;

        for (unsigned int i = 0; i < gOverrideCacheCount; ++i)
        {
            const OverrideCacheEntry& entry = gOverrideCache[i];
            if (_stricmp(entry.logicalName, logicalName) == 0)
                return entry.found ? entry.resolvedPath : nullptr;
        }

        unsigned int index = 0;
        if (gOverrideCacheCount < kOverrideCacheCapacity)
        {
            index = gOverrideCacheCount++;
        }
        else
        {
            index = gOverrideCacheCursor++ % kOverrideCacheCapacity;
        }

        OverrideCacheEntry& entry = gOverrideCache[index];
        std::memset(&entry, 0, sizeof(entry));
        strncpy_s(
            entry.logicalName,
            sizeof(entry.logicalName),
            logicalName,
            _TRUNCATE);

        for (unsigned int i = 0; i < SLOT_COUNT; ++i)
        {
            if (!gOverridePaths[i][0])
                continue;

            char candidate[MAX_PATH] = {};
            const int written = sprintf_s(
                candidate,
                sizeof(candidate),
                "%s%s",
                gOverridePaths[i],
                logicalName);
            if (written <= 0)
                continue;

            const DWORD attributes = GetFileAttributesA(candidate);
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                strcpy_s(
                    entry.resolvedPath,
                    sizeof(entry.resolvedPath),
                    candidate);
                entry.found = true;
                return entry.resolvedPath;
            }
        }

        // Negative entries are cached too. A missing custom file immediately
        // falls through to the game's untouched sgsm/archive resolver on all
        // subsequent requests for this stadium and stage.
        entry.found = false;
        return nullptr;
    }

#if defined(_M_IX86)
    void __cdecl ResolveCustomPathAtOpen(uintptr_t* entryStack)
    {
        if (!entryStack)
            return;

        const char* requestedPath = nullptr;
        unsigned int filenameIndex = 0;
        if (gGameVersion == GAME_LIVE_2005)
        {
            // Return address, filename, flags.
            filenameIndex = 1;
        }
        else
        {
            // Return address, internal type, filename, flags, context.
            filenameIndex = 2;
        }
        requestedPath = reinterpret_cast<const char*>(entryStack[filenameIndex]);

        __try
        {
            const char* resolved = ResolveCachedCustomPath(requestedPath);
            if (resolved)
                entryStack[filenameIndex] =
                    reinterpret_cast<uintptr_t>(resolved);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
    }

    __declspec(naked) void FilteredOpenResolved()
    {
        __asm
        {
            pushfd
            pushad

            // Original entry ESP is 36 bytes above the saved state.
            lea     eax, [esp + 36]
            push    eax
            call    ResolveCustomPathAtOpen
            add     esp, 4

            popad
            popfd
            jmp     dword ptr [gOpenResolvedContinuation]
        }
    }

    bool InstallOpenResolvedGate(unsigned int targetAddress)
    {
        if (gOpenResolvedGateInstalled)
            return true;

        const size_t patchLength = 9;
        unsigned char* target =
            reinterpret_cast<unsigned char*>(targetAddress);

        __try
        {
            // DebugConsole may already own the entry hook.  Chain to its JMP
            // destination so permitted requests retain all diagnostics.
            if (target[0] == 0xE9)
            {
                const int32_t displacement =
                    *reinterpret_cast<const int32_t*>(target + 1);
                gOpenResolvedContinuation =
                    targetAddress + 5 + displacement;
            }
            else if (target[0] == 0x55 &&
                     target[1] == 0x8B && target[2] == 0xEC &&
                     target[3] == 0x81 && target[4] == 0xEC)
            {
                unsigned char* trampoline =
                    reinterpret_cast<unsigned char*>(VirtualAlloc(
                        nullptr,
                        32,
                        MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE));
                if (!trampoline)
                    return false;

                std::memcpy(trampoline, target, patchLength);
                trampoline[patchLength] = 0xE9;
                *reinterpret_cast<int32_t*>(trampoline + patchLength + 1) =
                    static_cast<int32_t>(
                        (targetAddress + patchLength) -
                        reinterpret_cast<uintptr_t>(
                            trampoline + patchLength + 5));
                gOpenResolvedContinuation =
                    reinterpret_cast<uintptr_t>(trampoline);
            }
            else
            {
                OutputDebugStringA(
                    "[CustomPaths] OpenResolved gate validation failed.\n");
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
                patchLength,
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        target[0] = 0xE9;
        *reinterpret_cast<int32_t*>(target + 1) =
            static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(&FilteredOpenResolved) -
                (targetAddress + 5));
        for (size_t i = 5; i < patchLength; ++i)
            target[i] = 0x90;

        DWORD ignored = 0;
        VirtualProtect(target, patchLength, oldProtect, &ignored);
        FlushInstructionCache(
            GetCurrentProcess(),
            target,
            patchLength);

        gOpenResolvedGateInstalled = true;
        OutputDebugStringA(
            "[CustomPaths] Cached direct stadium override hook installed.\n");
        return true;
    }
#else
    bool InstallOpenResolvedGate(unsigned int)
    {
        return false;
    }
#endif

    void __fastcall GameCourtSetStadiumNameHook08(
        void* gameCourt,
        void*,
        const char* stadiumId4)
    {
        UpdateGameContext(stadiumId4, nullptr);
        CallMethodDynGlobal(
            kLive08GameCourtSetStadiumName,
            gameCourt,
            stadiumId4);
    }

    void InstallLive08ContextHook()
    {
        const size_t count = sizeof(kLive08GameCourtNameCalls) /
            sizeof(kLive08GameCourtNameCalls[0]);
        if (!ValidateCallSites(
                kLive08GameCourtNameCalls,
                count,
                kLive08GameCourtSetStadiumName))
        {
            OutputDebugStringA(
                "[CustomPaths] NBA Live 08 context-hook validation failed.\n");
            return;
        }

        for (size_t i = 0; i < count; ++i)
            patch::RedirectCall(
                kLive08GameCourtNameCalls[i],
                GameCourtSetStadiumNameHook08);
    }

    void Initialize()
    {
        switch (FM::GetEntryPoint())
        {
        case 0xCD8005: // NBA Live 2005 1.0 NOCD
            gGameVersion = GAME_LIVE_2005;
            patch::RedirectCall(0x005D8AF1, AddCustomPaths05);
            InstallOpenResolvedGate(0x006C5E6D);
            break;

        case 0x40109F:
            if (patch::GetFloat(0xBD832C) == 1.3333334f)
            {
                // NBA Live 06 1.0 NOCD
                gGameVersion = GAME_LIVE_06;
                patch::RedirectCall(0x005E756C, AddCustomPaths06);
                InstallOpenResolvedGate(0x006DC8F9);
            }
            else if (patch::GetFloat(0xBBBC3C) == 1.3333334f)
            {
                // NBA Live 07 1.1 NOCD
                gGameVersion = GAME_LIVE_07;
                patch::RedirectCall(0x0063A19E, AddCustomPaths07);
                InstallOpenResolvedGate(0x007FF4CE);
            }
            else if (patch::GetFloat(0xC3DF84) == 1.3333334f)
            {
                // NBA Live 08 1.0 NOCD
                gGameVersion = GAME_LIVE_08;
                patch::RedirectCall(0x00659E0E, AddCustomPaths08);
                InstallOpenResolvedGate(0x0082E4ED);
                InstallLive08ContextHook();
            }
            break;
        }
    }
}

// Called by CustomCamera's existing 2005/06/07 GameCourt hooks. Keeping one
// owner for those call sites prevents the two launcher features from replacing
// each other's hooks.
void UpdateCustomPathsGameContext(const char* stadiumId4, void* options)
{
    custompaths::UpdateGameContext(stadiumId4, options);
}

void InitializeCustomPaths()
{
    custompaths::Initialize();
}
