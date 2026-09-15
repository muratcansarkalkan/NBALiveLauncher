#include "plugin-std.h"
#include "DebugConsole.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <io.h>
#include <fcntl.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

namespace {

bool g_enabled = false;
bool g_files = true;
bool g_failedFiles = true;
bool g_showCaller = true;
bool g_crashes = true;
bool g_assetFiles = true;
HANDLE g_console = nullptr;
CRITICAL_SECTION g_consoleLock;
bool g_lockReady = false;
volatile LONG g_debugConsoleInitState = 0;

using CreateFileA_t = HANDLE (WINAPI *)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE (WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CloseHandle_t = BOOL (WINAPI *)(HANDLE);

CreateFileA_t g_CreateFileA = nullptr;
CreateFileW_t g_CreateFileW = nullptr;
CloseHandle_t g_CloseHandle = nullptr;

struct HandleEntry {
    HANDLE handle;
    char path[260];
};
HandleEntry g_handles[256] = {};
LONG g_handleCursor = 0;

enum AssetRequestKind : DWORD {
    ASSET_REQUEST_OTHER = 0,
    ASSET_REQUEST_PROBE = 1,
    ASSET_REQUEST_ACQUIRE = 2
};

struct AssetHistoryEntry {
    LONG sequence;
    LONG requestCount;
    DWORD flags;
    AssetRequestKind kind;
    uintptr_t caller;
    uintptr_t context;
    char path[260];
    char logicalName[128];
};

AssetHistoryEntry g_assetHistory[64] = {};
LONG g_assetHistoryCursor = -1;
LONG g_assetHistorySequence = 0;

const char* BaseNameOf(const char* path) {
    if (!path) return "";
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/')
            base = p + 1;
    }
    return base;
}

bool IsInterestingAssetName(const char* name) {
    if (!name || !*name) return false;
    const char* dot = std::strrchr(name, '.');
    if (!dot) return false;

    return _stricmp(dot, ".ebo") == 0 ||
           _stricmp(dot, ".fsh") == 0 ||
           _stricmp(dot, ".apt") == 0 ||
           _stricmp(dot, ".scn") == 0 ||
           _stricmp(dot, ".mgd") == 0 ||
           _stricmp(dot, ".ubb") == 0 ||
           _stricmp(dot, ".viv") == 0 ||
           _stricmp(dot, ".big") == 0;
}

AssetRequestKind ClassifyAssetRequest(void* caller) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(caller);

    // NBA Live 06:
    // 006DD915 call FileSystem_OpenResolved
    // 006DD91A ... returned object is reduced to boolean existence state.
    if (ret == 0x006DD91A)
        return ASSET_REQUEST_PROBE;

    // NBA Live 06:
    // 006DD94D call FileSystem_OpenResolved
    // 006DD952 ...
    // 006DD955 mov [esi+14h],eax  -- returned file object is retained.
    if (ret == 0x006DD952)
        return ASSET_REQUEST_ACQUIRE;

    return ASSET_REQUEST_OTHER;
}

const char* AssetRequestKindName(AssetRequestKind kind) {
    switch (kind) {
    case ASSET_REQUEST_PROBE:   return "PROBE";
    case ASSET_REQUEST_ACQUIRE: return "ACQUIRE";
    default:                    return "OTHER";
    }
}

void RememberAssetRequest(const char* path, DWORD flags, void* caller, void* context) {
    if (!path) return;

    char safePath[260] = {};
    __try {
        std::strncpy(safePath, path, sizeof(safePath) - 1);
        safePath[sizeof(safePath) - 1] = '\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    const char* base = BaseNameOf(safePath);
    if (!IsInterestingAssetName(base))
        return;

    const AssetRequestKind kind = ClassifyAssetRequest(caller);

    // Collapse the game's repeated search-path probes for the same logical
    // resource. A repeated basename updates the existing entry instead of
    // consuming another history slot.
    for (auto& e : g_assetHistory) {
        if (e.sequence > 0 &&
            e.kind == kind &&
            _stricmp(e.logicalName, base) == 0) {
            InterlockedIncrement(&e.requestCount);
            e.flags = flags;
            e.caller = reinterpret_cast<uintptr_t>(caller);
            e.context = reinterpret_cast<uintptr_t>(context);
            std::strncpy(e.path, safePath, sizeof(e.path) - 1);
            e.path[sizeof(e.path) - 1] = '\0';
            return;
        }
    }

    LONG seq = InterlockedIncrement(&g_assetHistorySequence);
    LONG cursor = InterlockedIncrement(&g_assetHistoryCursor);
    AssetHistoryEntry& e = g_assetHistory[static_cast<unsigned long>(cursor) & 63];

    e.sequence = 0;
    e.requestCount = 1;
    e.flags = flags;
    e.kind = kind;
    e.caller = reinterpret_cast<uintptr_t>(caller);
    e.context = reinterpret_cast<uintptr_t>(context);
    std::strncpy(e.path, safePath, sizeof(e.path) - 1);
    e.path[sizeof(e.path) - 1] = '\0';
    std::strncpy(e.logicalName, base, sizeof(e.logicalName) - 1);
    e.logicalName[sizeof(e.logicalName) - 1] = '\0';
    InterlockedExchange(&e.sequence, seq);
}

void PrintRecentAssetActivity() {
    LONG newest = g_assetHistoryCursor;
    if (newest < 0) return;

    std::printf("\nRecent logical resources (search-path probes collapsed):\n");
    LONG count = (newest + 1 < 64) ? newest + 1 : 64;
    LONG first = newest - count + 1;

    for (LONG i = first; i <= newest; ++i) {
        const AssetHistoryEntry& e =
            g_assetHistory[static_cast<unsigned long>(i) & 63];

        LONG seq = e.sequence;
        if (seq <= 0 || !e.logicalName[0]) continue;

        std::printf("  #%ld  %-8s %-32s count=%ld",
            seq, AssetRequestKindName(e.kind), e.logicalName, e.requestCount);

        if (std::strcmp(e.logicalName, e.path) != 0)
            std::printf(" last=%s", e.path);

        if (g_showCaller)
            std::printf(" caller=%08lX",
                static_cast<unsigned long>(e.caller));

        std::printf("\n");
    }

    std::printf(
        "\nNote: these are logical resource requests, not proof of crash causation.\n");
}

const char* GameName() {
    const uintptr_t ep = FM::GetEntryPoint();
    if (ep == 0xCD8005) return "NBA Live 2005";
    if (ep == 0x40109F) {
        if (plugin::patch::GetFloat(0xBD832C) == 1.3333334f) return "NBA Live 06";
        if (plugin::patch::GetFloat(0xBBBC3C) == 1.3333334f) return "NBA Live 07";
        if (plugin::patch::GetFloat(0xC3DF84) == 1.3333334f) return "NBA Live 08";
    }
    return "NBA Live (unknown build)";
}

void SetColor(WORD color) {
    if (g_console) SetConsoleTextAttribute(g_console, color);
}

void PrintPrefix(const char* channel, WORD color) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    SetColor(color);
    std::printf("[%02u:%02u:%02u.%03u] %-7s ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, channel);
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

void RememberHandle(HANDLE h, const char* path) {
    if (!h || h == INVALID_HANDLE_VALUE || !path) return;
    const LONG slot = InterlockedIncrement(&g_handleCursor) & 255;
    g_handles[slot].handle = h;
    std::strncpy(g_handles[slot].path, path, sizeof(g_handles[slot].path) - 1);
    g_handles[slot].path[sizeof(g_handles[slot].path) - 1] = '\0';
}

void ForgetHandle(HANDLE h) {
    for (auto& e : g_handles) {
        if (e.handle == h) {
            e.handle = nullptr;
            e.path[0] = '\0';
            return;
        }
    }
}

void PrintFileResult(const char* path, HANDLE result, DWORD error, void* caller) {
    if (!g_files) return;
    const bool failed = result == INVALID_HANDLE_VALUE;
    if (failed && !g_failedFiles) return;

    EnterCriticalSection(&g_consoleLock);
    PrintPrefix("FILES", failed ? (FOREGROUND_RED | FOREGROUND_INTENSITY)
                                : (FOREGROUND_GREEN | FOREGROUND_INTENSITY));
    std::printf("%-6s %s", failed ? "FAILED" : "OPEN", path ? path : "<null>");
    if (failed) std::printf("  [Win32=%lu]", error);
    if (g_showCaller) std::printf("  caller=%p", caller);
    std::printf("\n");
    LeaveCriticalSection(&g_consoleLock);
}

HANDLE WINAPI HookCreateFileA(LPCSTR fileName, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags, HANDLE templ) {
    void* caller = _ReturnAddress();
    HANDLE h = g_CreateFileA(fileName, access, share, sa, creation, flags, templ);
    DWORD error = (h == INVALID_HANDLE_VALUE) ? GetLastError() : ERROR_SUCCESS;
    PrintFileResult(fileName, h, error, caller);
    if (h != INVALID_HANDLE_VALUE) RememberHandle(h, fileName);
    SetLastError(error);
    return h;
}

HANDLE WINAPI HookCreateFileW(LPCWSTR fileName, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags, HANDLE templ) {
    void* caller = _ReturnAddress();
    HANDLE h = g_CreateFileW(fileName, access, share, sa, creation, flags, templ);
    DWORD error = (h == INVALID_HANDLE_VALUE) ? GetLastError() : ERROR_SUCCESS;

    char path[1024] = "<wide path>";
    if (fileName) WideCharToMultiByte(CP_ACP, 0, fileName, -1, path, sizeof(path), nullptr, nullptr);
    PrintFileResult(path, h, error, caller);
    if (h != INVALID_HANDLE_VALUE) RememberHandle(h, path);
    SetLastError(error);
    return h;
}

BOOL WINAPI HookCloseHandle(HANDLE h) {
    ForgetHandle(h);
    return g_CloseHandle(h);
}

bool PatchIAT(const char* dllName, const char* procName, void* replacement, void** original) {
    auto base = reinterpret_cast<unsigned char*>(GetModuleHandleA(nullptr));
    if (!base) return false;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);

    for (; desc->Name; ++desc) {
        const char* importedDll = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(importedDll, dllName) != 0) continue;

        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto names = desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
            : thunk;

        for (; names->u1.AddressOfData; ++names, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), procName) != 0) continue;

            // If this exact hook is already installed, do not overwrite the
            // saved original with the hook itself. Doing so would make the
            // hook call itself recursively and eventually stack-overflow.
            const uintptr_t currentFunction =
                static_cast<uintptr_t>(thunk->u1.Function);

            if (currentFunction == reinterpret_cast<uintptr_t>(replacement))
                return true;

            DWORD oldProtect = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) return false;
            if (original) *original = reinterpret_cast<void*>(currentFunction);
            thunk->u1.Function = reinterpret_cast<uintptr_t>(replacement);
            DWORD ignored = 0;
            VirtualProtect(&thunk->u1.Function, sizeof(uintptr_t), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &thunk->u1.Function, sizeof(uintptr_t));
            return true;
        }
    }
    return false;
}


#if defined(_M_IX86)

// -------------------------------------------------------------------------
// Logical asset-open hooks
// -------------------------------------------------------------------------
//
// NBA Live 2005:
//   FileSystem_OpenResolved @ 0x006C5E6D
//   custom convention:
//     EAX     = search/filesystem context
//     ESP+04  = filename
//     ESP+08  = flags
//
// NBA Live 06:
//   FileSystem_OpenResolved @ 0x006DC8F9
//   cdecl-style stack arguments:
//     ESP+04  = internal constant/type (observed 0x28)
//     ESP+08  = filename
//     ESP+0C  = flags
//     ESP+10  = search/filesystem context
//
// Both functions have a complete 9-byte prologue:
//   push ebp
//   mov  ebp, esp
//   sub  esp, imm32
//
// We overwrite all 9 bytes and execute them in an executable trampoline.
// This avoids guessing either game's complete C++ prototype.

uintptr_t g_FileSystemOpenResolvedTrampoline = 0;
bool g_FileSystemOpenResolvedInstalled = false;

enum AssetOpenLayout : DWORD {
    ASSET_LAYOUT_NONE = 0,
    ASSET_LAYOUT_LIVE2005 = 2005,
    ASSET_LAYOUT_LIVE06 = 2006
};

AssetOpenLayout g_assetOpenLayout = ASSET_LAYOUT_NONE;

void __cdecl LogFileSystemOpenResolved(
    void* registerContext,
    const uintptr_t* entryStack)
{
    if (!entryStack || !g_lockReady)
        return;

    void* caller = reinterpret_cast<void*>(entryStack[0]);
    const char* fileName = nullptr;
    DWORD flags = 0;
    void* context = registerContext;

    if (g_assetOpenLayout == ASSET_LAYOUT_LIVE2005) {
        fileName = reinterpret_cast<const char*>(entryStack[1]);
        flags = static_cast<DWORD>(entryStack[2]);
    }
    else if (g_assetOpenLayout == ASSET_LAYOUT_LIVE06) {
        fileName = reinterpret_cast<const char*>(entryStack[2]);
        flags = static_cast<DWORD>(entryStack[3]);
        context = reinterpret_cast<void*>(entryStack[4]);
    }
    else {
        return;
    }

    RememberAssetRequest(fileName, flags, caller, context);

    EnterCriticalSection(&g_consoleLock);
    PrintPrefix(
        "ASSET",
        FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);

    __try {
        std::printf("OPEN   %s", fileName ? fileName : "<null>");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("OPEN   <invalid filename %p>", fileName);
    }

    std::printf("  flags=%08lX", static_cast<unsigned long>(flags));

    if (g_showCaller)
        std::printf("  caller=%p", caller);

    std::printf("  context=%p\n", context);
    LeaveCriticalSection(&g_consoleLock);
}

__declspec(naked) void HookFileSystemOpenResolved()
{
    __asm {
        // Snapshot the untouched entry stack in EDX. pushad will preserve the
        // original EDX and EAX values before the C logger is called.
        mov     edx, esp
        pushfd
        pushad

        push    edx
        push    eax
        call    LogFileSystemOpenResolved
        add     esp, 8

        popad
        popfd

        jmp     dword ptr [g_FileSystemOpenResolvedTrampoline]
    }
}

bool InstallFileSystemOpenResolvedHook(
    uintptr_t targetAddress,
    DWORD localStackSize,
    AssetOpenLayout layout)
{
    constexpr size_t kPatchLength = 9;

    BYTE expected[kPatchLength] = {
        0x55,
        0x8B, 0xEC,
        0x81, 0xEC,
        static_cast<BYTE>(localStackSize & 0xFF),
        static_cast<BYTE>((localStackSize >> 8) & 0xFF),
        static_cast<BYTE>((localStackSize >> 16) & 0xFF),
        static_cast<BYTE>((localStackSize >> 24) & 0xFF)
    };

    BYTE* target = reinterpret_cast<BYTE*>(targetAddress);

    __try {
        if (std::memcmp(target, expected, kPatchLength) != 0) {
            PrintPrefix(
                "DEBUG",
                FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::printf(
                "FileSystem::OpenResolved hook skipped: unexpected bytes at %08lX\n",
                static_cast<unsigned long>(targetAddress));
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    BYTE* trampoline = reinterpret_cast<BYTE*>(
        VirtualAlloc(
            nullptr,
            32,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));

    if (!trampoline)
        return false;

    std::memcpy(trampoline, target, kPatchLength);

    trampoline[kPatchLength] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + kPatchLength + 1) =
        static_cast<int32_t>(
            (targetAddress + kPatchLength) -
            reinterpret_cast<uintptr_t>(trampoline + kPatchLength + 5));

    g_FileSystemOpenResolvedTrampoline =
        reinterpret_cast<uintptr_t>(trampoline);
    g_assetOpenLayout = layout;

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            target,
            kPatchLength,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        g_FileSystemOpenResolvedTrampoline = 0;
        g_assetOpenLayout = ASSET_LAYOUT_NONE;
        return false;
    }

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookFileSystemOpenResolved) -
            (targetAddress + 5));

    for (size_t i = 5; i < kPatchLength; ++i)
        target[i] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, kPatchLength, oldProtect, &ignored);
    FlushInstructionCache(
        GetCurrentProcess(),
        target,
        kPatchLength);

    g_FileSystemOpenResolvedInstalled = true;
    return true;
}

bool InstallFileSystemOpenResolvedHookForCurrentGame()
{
    const uintptr_t ep = FM::GetEntryPoint();

    if (ep == 0xCD8005) {
        // NBA Live 2005 1.0 NOCD
        // 006C5E6D: sub esp, 104h
        return InstallFileSystemOpenResolvedHook(
            0x006C5E6D,
            0x104,
            ASSET_LAYOUT_LIVE2005);
    }

    if (ep == 0x40109F &&
        plugin::patch::GetFloat(0xBD832C) == 1.3333334f)
    {
        // NBA Live 06 1.0 NOCD
        // 006DC8F9: sub esp, 108h
        return InstallFileSystemOpenResolvedHook(
            0x006DC8F9,
            0x108,
            ASSET_LAYOUT_LIVE06);
    }

    return false;
}


// NBA Live 06 GeomFile processor @ 00747550.
// If it crashes before returning, this context remains active for CrashHandler.
struct GeomFileDebugContext {
    volatile LONG active;
    volatile LONG depth;
    DWORD threadId;
    uintptr_t objectThis;
    uintptr_t sourceObject;
    uintptr_t value10;
    uintptr_t state;
    uintptr_t alternateObject;
};
GeomFileDebugContext g_geomFileContext = {};
uintptr_t g_GeomFileProcessTrampoline = 0;

void __cdecl BeginGeomFileProcessing(void* self) {
    if (!self) return;

    uintptr_t source = 0, value10 = 0, state = 0, alternate = 0;
    __try {
        uintptr_t p = reinterpret_cast<uintptr_t>(self);
        source    = *reinterpret_cast<uintptr_t*>(p + 0x08);
        value10   = *reinterpret_cast<uintptr_t*>(p + 0x10);
        state     = *reinterpret_cast<uintptr_t*>(p + 0x18);
        alternate = *reinterpret_cast<uintptr_t*>(p + 0x1C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    g_geomFileContext.threadId = GetCurrentThreadId();
    InterlockedIncrement(&g_geomFileContext.depth);
    g_geomFileContext.objectThis = reinterpret_cast<uintptr_t>(self);
    g_geomFileContext.sourceObject = source;
    g_geomFileContext.value10 = value10;
    g_geomFileContext.state = state;
    g_geomFileContext.alternateObject = alternate;
    InterlockedExchange(&g_geomFileContext.active, 1);
}

void __cdecl EndGeomFileProcessing() {
    if (g_geomFileContext.threadId != GetCurrentThreadId())
        return;

    LONG depth = InterlockedDecrement(&g_geomFileContext.depth);
    if (depth <= 0) {
        InterlockedExchange(&g_geomFileContext.depth, 0);
        InterlockedExchange(&g_geomFileContext.active, 0);
    }
}

__declspec(naked) void HookGeomFileProcess() {
    __asm {
        pushfd
        pushad
        push    ecx
        call    BeginGeomFileProcessing
        add     esp, 4
        popad
        popfd

        call    dword ptr [g_GeomFileProcessTrampoline]

        pushfd
        pushad
        call    EndGeomFileProcessing
        popad
        popfd
        retn    4
    }
}

bool InstallGeomFileProcessHookLive06() {
    constexpr uintptr_t targetAddress = 0x00747550;
    constexpr size_t copiedLength = 9;
    constexpr size_t patchLength = 9;

    const BYTE expected[copiedLength] = {
        0x81, 0xEC, 0x24, 0x00, 0x00, 0x00, // sub esp,24h
        0x56,                               // push esi
        0x8B, 0xF1                         // mov esi,ecx
    };

    BYTE* target = reinterpret_cast<BYTE*>(targetAddress);
    __try {
        if (std::memcmp(target, expected, copiedLength) != 0)
            return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    BYTE* trampoline = reinterpret_cast<BYTE*>(
        VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;

    std::memcpy(trampoline, target, copiedLength);
    trampoline[copiedLength] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + copiedLength + 1) =
        static_cast<int32_t>((targetAddress + copiedLength) -
            reinterpret_cast<uintptr_t>(trampoline + copiedLength + 5));

    g_GeomFileProcessTrampoline = reinterpret_cast<uintptr_t>(trampoline);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        g_GeomFileProcessTrampoline = 0;
        return false;
    }

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HookGeomFileProcess) -
            (targetAddress + 5));
    for (size_t i = 5; i < patchLength; ++i) target[i] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    return true;
}

bool InstallGeomFileProcessHookForCurrentGame() {
    if (FM::GetEntryPoint() == 0x40109F &&
        plugin::patch::GetFloat(0xBD832C) == 1.3333334f)
        return InstallGeomFileProcessHookLive06();
    return false;
}

#else

bool InstallFileSystemOpenResolvedHookForCurrentGame()
{
    return false;
}

bool InstallGeomFileProcessHookForCurrentGame()
{
    return false;
}

#endif

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INTEGER_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIVILEGED_INSTRUCTION";
    default: return "UNHANDLED_EXCEPTION";
    }
}

void PrintStack32(CONTEXT* c) {
#if defined(_M_IX86)
    std::printf("\nCall stack (EBP chain):\n");
    std::printf("  #0  %08lX  <faulting instruction>\n", c->Eip);
    DWORD ebp = c->Ebp;
    for (int i = 1; i < 24 && ebp; ++i) {
        __try {
            DWORD* frame = reinterpret_cast<DWORD*>(ebp);
            DWORD next = frame[0];
            DWORD ret = frame[1];
            if (!ret || next <= ebp || next - ebp > 0x100000) break;
            std::printf("  #%d  %08lX\n", i, ret);
            ebp = next;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
    }
#endif
}


#if defined(_M_IX86)
bool StackContainsReturnAddress(const CONTEXT* c, DWORD wanted) {
    if (!c) return false;

    DWORD ebp = c->Ebp;
    for (int i = 1; i < 24 && ebp; ++i) {
        __try {
            const DWORD* frame = reinterpret_cast<const DWORD*>(ebp);
            const DWORD next = frame[0];
            const DWORD ret = frame[1];

            if (ret == wanted)
                return true;

            if (!ret || next <= ebp || next - ebp > 0x100000)
                break;

            ebp = next;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return false;
}
#endif

bool SafeReadU32(uintptr_t address, DWORD& value) {
    __try {
        value = *reinterpret_cast<const DWORD*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
}

bool SafeReadU16(uintptr_t address, WORD& value) {
    __try {
        value = *reinterpret_cast<const WORD*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
        return false;
    }
}

struct EaglFieldLiveContext {
    volatile LONG valid;
    DWORD threadId;
    uintptr_t fieldOffset;
    uintptr_t selectedBase;
    uintptr_t descriptor;
    uintptr_t dataObject;
    uintptr_t typeObject;
    uintptr_t dataOffset;
    WORD relocationState;
    DWORD fieldsRemaining;
    uintptr_t finalStringPointer;
};

EaglFieldLiveContext g_eaglFieldLive = {};

void __cdecl CaptureEaglFieldLive(
    uintptr_t fieldOffset,
    uintptr_t selectedBase,
    uintptr_t descriptor,
    uintptr_t dataObject,
    uintptr_t traversalEbp)
{
    EaglFieldLiveContext c = {};
    c.threadId = GetCurrentThreadId();
    c.fieldOffset = fieldOffset;
    c.selectedBase = selectedBase;
    c.descriptor = descriptor;
    c.dataObject = dataObject;
    c.finalStringPointer = selectedBase + fieldOffset;

    __try {
        c.typeObject = *reinterpret_cast<uintptr_t*>(traversalEbp - 4);
        c.fieldsRemaining = *reinterpret_cast<DWORD*>(traversalEbp + 0x10);

        if (dataObject) {
            c.dataOffset = *reinterpret_cast<uintptr_t*>(dataObject + 0x20);
            c.relocationState = *reinterpret_cast<WORD*>(dataObject + 0x2C);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Publish fields before setting valid.
    g_eaglFieldLive.threadId = c.threadId;
    g_eaglFieldLive.fieldOffset = c.fieldOffset;
    g_eaglFieldLive.selectedBase = c.selectedBase;
    g_eaglFieldLive.descriptor = c.descriptor;
    g_eaglFieldLive.dataObject = c.dataObject;
    g_eaglFieldLive.typeObject = c.typeObject;
    g_eaglFieldLive.dataOffset = c.dataOffset;
    g_eaglFieldLive.relocationState = c.relocationState;
    g_eaglFieldLive.fieldsRemaining = c.fieldsRemaining;
    g_eaglFieldLive.finalStringPointer = c.finalStringPointer;
    InterlockedExchange(&g_eaglFieldLive.valid, 1);
}

__declspec(naked) void HookEaglFieldStringLive06() {
    __asm {
        // Hook point: 006EC6DE
        // Live state here:
        //   EAX = current field offset (restored from [EBP+arg_0])
        //   ESI = selected data base (nominal or relocated)
        //   EBX = current descriptor
        //   EDI = EAGL data object
        //   [EBP-4] = traversal/type object

        pushfd
        pushad
        push    ebp
        push    edi
        push    ebx
        push    esi
        push    eax
        call    CaptureEaglFieldLive
        add     esp, 20
        popad
        popfd

        // Reproduce the exact five bytes overwritten at 006EC6DE:
        //   6A 00       push 0
        //   03 F0       add esi,eax
        //   56          push esi
        push    0
        add     esi, eax
        push    esi

        // Continue at: mov ecx,ebx
        // MSVC inline assembler cannot encode a direct absolute numeric JMP
        // operand here, so load the continuation address into EAX first.
        // EAX no longer needs to retain fieldOffset after add esi,eax.
        mov     eax, 006EC6E3h
        jmp     eax
    }
}

bool InstallEaglFieldStringHookLive06() {
    constexpr uintptr_t targetAddress = 0x006EC6DE;
    constexpr size_t patchLength = 5;

    const BYTE expected[patchLength] = {
        0x6A, 0x00,       // push 0
        0x03, 0xF0,       // add esi,eax
        0x56              // push esi
    };

    BYTE* target = reinterpret_cast<BYTE*>(targetAddress);
    __try {
        if (std::memcmp(target, expected, patchLength) != 0)
            return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookEaglFieldStringLive06) -
            (targetAddress + 5));

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    return true;
}

bool InstallEaglFieldStringHookForCurrentGame() {
    if (FM::GetEntryPoint() == 0x40109F &&
        plugin::patch::GetFloat(0xBD832C) == 1.3333334f)
        return InstallEaglFieldStringHookLive06();
    return false;
}

bool LooksLikeFiniteFloatBits(DWORD raw, float& value) {
    std::memcpy(&value, &raw, sizeof(value));
    return _finite(value) != 0;
}

void PrintEboDescriptorBackReferences(uintptr_t dataObject,
                                      DWORD fileSize,
                                      uintptr_t descriptor)
{
    if (!dataObject || !fileSize || descriptor < dataObject)
        return;

    const DWORD descriptorOffset =
        static_cast<DWORD>(descriptor - dataObject);

    // Keep crash-time scanning bounded and only scan the loaded EBO image.
    if (descriptorOffset >= fileSize || fileSize > 0x04000000)
        return;

    int shown = 0;
    std::printf("  Descriptor refs:");

    for (DWORD off = 0; off + 4 <= fileSize && shown < 8; off += 4) {
        DWORD v = 0;
        if (!SafeReadU32(dataObject + off, v))
            continue;

        if (v == descriptorOffset || v == descriptor) {
            std::printf("%s EBO+%08lX",
                shown == 0 ? "" : ",",
                (unsigned long)off);
            ++shown;
        }
    }

    if (!shown)
        std::printf(" none found");
    std::printf("\n");
}

bool DetectRawBufferOwner(uintptr_t dataObject,
                          DWORD fileSize,
                          uintptr_t descriptor,
                          DWORD& ownerFileOffset,
                          DWORD& payloadBytes,
                          DWORD& elementStride,
                          DWORD& payloadFileOffset,
                          DWORD& elementCount,
                          DWORD& ownerTag)
{
    // Observed EBO raw-buffer descriptor layout immediately before payload:
    //   +00 payload byte size
    //   +04 element stride
    //   +08 payload file-relative offset
    //   +0C 0
    //   +10 0
    //   +14 tag/flags (0xF0 in the supplied court)
    //   +18 0
    //   +1C payload begins
    //
    // We only call this a match when the +08 pointer lands exactly on the
    // descriptor currently being traversed and the size/stride are plausible.
    if (!dataObject || !fileSize || descriptor < dataObject + 0x1C)
        return false;

    const uintptr_t owner = descriptor - 0x1C;
    if (owner < dataObject)
        return false;

    const DWORD ownerOff = static_cast<DWORD>(owner - dataObject);
    if (ownerOff + 0x1C > fileSize)
        return false;

    DWORD bytes = 0, stride = 0, payloadOff = 0;
    DWORD zero0 = 1, zero1 = 1, tag = 0, zero2 = 1;

    if (!SafeReadU32(owner + 0x00, bytes) ||
        !SafeReadU32(owner + 0x04, stride) ||
        !SafeReadU32(owner + 0x08, payloadOff) ||
        !SafeReadU32(owner + 0x0C, zero0) ||
        !SafeReadU32(owner + 0x10, zero1) ||
        !SafeReadU32(owner + 0x14, tag) ||
        !SafeReadU32(owner + 0x18, zero2))
        return false;

    const DWORD descOff = static_cast<DWORD>(descriptor - dataObject);

    if (payloadOff != descOff ||
        bytes == 0 ||
        stride == 0 ||
        stride > 0x100 ||
        bytes > fileSize ||
        payloadOff >= fileSize ||
        zero0 != 0 || zero1 != 0 || zero2 != 0)
        return false;

    if (bytes % stride != 0)
        return false;

    ownerFileOffset = ownerOff;
    payloadBytes = bytes;
    elementStride = stride;
    payloadFileOffset = payloadOff;
    elementCount = bytes / stride;
    ownerTag = tag;
    return true;
}


struct EaglTraversalEntry {
    LONG sequence;
    DWORD threadId;
    uintptr_t caller;
    uintptr_t typeObject;
    DWORD typeMode;
    BYTE typeFlag8;
    uintptr_t childList;
    DWORD childCount;
    uintptr_t dataObject;
    uintptr_t cursor;
    DWORD count;
    DWORD eboSize;
    DWORD cursorFileOffset;
    BOOL cursorInsideEbo;
};

EaglTraversalEntry g_eaglTraversalHistory[64] = {};
volatile LONG g_eaglTraversalSequence = 0;

void __cdecl CaptureEaglTraversalEntry(uintptr_t typeObject, uintptr_t* entryStack) {
    if (!entryStack)
        return;

    EaglTraversalEntry e = {};
    e.sequence = InterlockedIncrement(&g_eaglTraversalSequence);
    e.threadId = GetCurrentThreadId();
    e.caller = entryStack[0];
    e.typeObject = typeObject;
    e.dataObject = entryStack[1];
    e.cursor = entryStack[2];
    e.count = static_cast<DWORD>(entryStack[3]);

    __try {
        if (typeObject) {
            e.typeMode = *reinterpret_cast<DWORD*>(typeObject + 4);
            e.typeFlag8 = *reinterpret_cast<BYTE*>(typeObject + 8);
            e.childList = *reinterpret_cast<uintptr_t*>(typeObject + 0x28);
            e.childCount = *reinterpret_cast<DWORD*>(typeObject + 0x2C);
        }

        if (e.dataObject)
            e.eboSize = *reinterpret_cast<DWORD*>(e.dataObject + 8);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (e.dataObject && e.eboSize &&
        e.cursor >= e.dataObject &&
        (e.cursor - e.dataObject) < e.eboSize)
    {
        e.cursorInsideEbo = TRUE;
        e.cursorFileOffset =
            static_cast<DWORD>(e.cursor - e.dataObject);
    }

    g_eaglTraversalHistory[(e.sequence - 1) & 63] = e;
}

__declspec(naked) void HookEaglTraversalEntryLive06() {
    __asm {
        // Entry to sub_6EC9FA:
        //   ECX      = type object
        //   [ESP+04] = data object
        //   [ESP+08] = current EBO cursor
        //   [ESP+0C] = element/count value
        mov     eax, esp
        pushfd
        pushad
        push    eax
        push    ecx
        call    CaptureEaglTraversalEntry
        add     esp, 8
        popad
        popfd

        // Reproduce the five overwritten bytes:
        // 55          push ebp
        // 8B EC       mov ebp,esp
        // 51          push ecx
        // 57          push edi
        push    ebp
        mov     ebp, esp
        push    ecx
        push    edi

        // Resume at 006EC9FF: mov edi,ecx
        mov     eax, 006EC9FFh
        jmp     eax
    }
}

bool InstallEaglTraversalEntryHookLive06() {
    constexpr uintptr_t targetAddress = 0x006EC9FA;
    constexpr size_t patchLength = 5;

    const BYTE expected[patchLength] = {
        0x55,
        0x8B, 0xEC,
        0x51,
        0x57
    };

    BYTE* target = reinterpret_cast<BYTE*>(targetAddress);

    __try {
        if (std::memcmp(target, expected, patchLength) != 0)
            return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookEaglTraversalEntryLive06) -
            (targetAddress + 5));

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    return true;
}

bool InstallEaglTraversalEntryHookForCurrentGame() {
    if (FM::GetEntryPoint() == 0x40109F &&
        plugin::patch::GetFloat(0xBD832C) == 1.3333334f)
        return InstallEaglTraversalEntryHookLive06();

    return false;
}


struct EaglOuterRecordContext {
    volatile LONG valid;
    DWORD threadId;
    uintptr_t record;
    uintptr_t dataObject;
    uintptr_t cursor;
    DWORD recordFlags;
    WORD typeIndex;
    DWORD repeatCount;
    DWORD cursorStride;
    DWORD relativeCursor;
    uintptr_t typeObject;
    DWORD recordFileOffset;
    DWORD cursorFileOffset;
};

EaglOuterRecordContext g_eaglOuterRecord = {};

void __cdecl CaptureEaglOuterRecordLive06(
    uintptr_t record,
    uintptr_t cursor,
    uintptr_t traversalEbp)
{
    EaglOuterRecordContext c = {};
    c.threadId = GetCurrentThreadId();
    c.record = record;
    c.cursor = cursor;

    __try {
        c.dataObject = *reinterpret_cast<uintptr_t*>(traversalEbp + 0x0C);

        if (record) {
            c.recordFlags = *reinterpret_cast<DWORD*>(record + 0x00);
            c.typeIndex = *reinterpret_cast<WORD*>(record + 0x02);
            c.repeatCount = *reinterpret_cast<DWORD*>(record + 0x04);
            c.cursorStride = *reinterpret_cast<DWORD*>(record + 0x08);
            c.relativeCursor = *reinterpret_cast<DWORD*>(record + 0x0C);
        }

        uintptr_t typeTable = *reinterpret_cast<uintptr_t*>(traversalEbp + 0x08);
        if (typeTable)
            c.typeObject =
                *reinterpret_cast<uintptr_t*>(
                    typeTable + static_cast<uintptr_t>(c.typeIndex) * 4);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (c.dataObject) {
        if (record >= c.dataObject)
            c.recordFileOffset =
                static_cast<DWORD>(record - c.dataObject);

        if (cursor >= c.dataObject)
            c.cursorFileOffset =
                static_cast<DWORD>(cursor - c.dataObject);
    }

    g_eaglOuterRecord.threadId = c.threadId;
    g_eaglOuterRecord.record = c.record;
    g_eaglOuterRecord.dataObject = c.dataObject;
    g_eaglOuterRecord.cursor = c.cursor;
    g_eaglOuterRecord.recordFlags = c.recordFlags;
    g_eaglOuterRecord.typeIndex = c.typeIndex;
    g_eaglOuterRecord.repeatCount = c.repeatCount;
    g_eaglOuterRecord.cursorStride = c.cursorStride;
    g_eaglOuterRecord.relativeCursor = c.relativeCursor;
    g_eaglOuterRecord.typeObject = c.typeObject;
    g_eaglOuterRecord.recordFileOffset = c.recordFileOffset;
    g_eaglOuterRecord.cursorFileOffset = c.cursorFileOffset;
    InterlockedExchange(&g_eaglOuterRecord.valid, 1);
}

__declspec(naked) void HookEaglOuterRecordLive06() {
    __asm {
        // Hook point: 006E9CFB
        // ESI = current 16-byte outer record
        // EBX = cursor computed as ESI + [ESI+0C]
        // EBP = sub_6E9C9F frame
        pushfd
        pushad
        push    ebp
        push    ebx
        push    esi
        call    CaptureEaglOuterRecordLive06
        add     esp, 12
        popad
        popfd

        // Reproduce overwritten bytes:
        // 8B 4D F4    mov ecx,[ebp-0Ch]
        // 6A 01       push 1
        mov     ecx, [ebp-0Ch]
        push    1

        mov     eax, 006E9D00h
        jmp     eax
    }
}

bool InstallEaglOuterRecordHookLive06() {
    constexpr uintptr_t targetAddress = 0x006E9CFB;
    constexpr size_t patchLength = 5;

    const BYTE expected[patchLength] = {
        0x8B, 0x4D, 0xF4,
        0x6A, 0x01
    };

    BYTE* target = reinterpret_cast<BYTE*>(targetAddress);

    __try {
        if (std::memcmp(target, expected, patchLength) != 0)
            return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookEaglOuterRecordLive06) -
            (targetAddress + 5));

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    return true;
}

bool InstallEaglOuterRecordHookForCurrentGame() {
    if (FM::GetEntryPoint() == 0x40109F &&
        plugin::patch::GetFloat(0xBD832C) == 1.3333334f)
        return InstallEaglOuterRecordHookLive06();

    return false;
}

void PrintEaglOuterRecordForensics(DWORD threadId,
                                   uintptr_t dataObject,
                                   uintptr_t badCursor)
{
    if (!g_eaglOuterRecord.valid)
        return;

    const EaglOuterRecordContext c = g_eaglOuterRecord;

    if (c.threadId != threadId ||
        (dataObject && c.dataObject != dataObject) ||
        (badCursor && c.cursor != badCursor))
        return;

    std::printf("\nOuter EAGL record that selected the bad cursor:\n");
    std::printf("  Record ptr      : %08lX\n", (unsigned long)c.record);
    std::printf("  Record EBO off  : %08lX\n", (unsigned long)c.recordFileOffset);
    std::printf("  Type index      : %u\n", (unsigned)c.typeIndex);
    std::printf("  Type object     : %08lX\n", (unsigned long)c.typeObject);
    std::printf("  Repeat count    : %lu\n", (unsigned long)c.repeatCount);
    std::printf("  Cursor stride   : %08lX\n", (unsigned long)c.cursorStride);
    std::printf("  Relative cursor : %08lX\n", (unsigned long)c.relativeCursor);
    std::printf("  Cursor EBO off  : %08lX\n", (unsigned long)c.cursorFileOffset);

    const DWORD recomputed =
        c.recordFileOffset + c.relativeCursor;

    std::printf("  Recomputed off  : %08lX  (record off + relative cursor)\n",
        (unsigned long)recomputed);

    if (recomputed == c.cursorFileOffset) {
        std::printf("  Cursor match    : EXACT\n");
        std::printf("  Finding         : this 16-byte EAGL record explicitly selects the bad EBO cursor.\n");
    }

    std::printf("  Raw record      : %08lX %08lX %08lX %08lX\n",
        (unsigned long)c.recordFlags,
        (unsigned long)c.repeatCount,
        (unsigned long)c.cursorStride,
        (unsigned long)c.relativeCursor);
}

void PrintRecentEaglTraversalHistory(DWORD threadId,
                                     uintptr_t dataObject,
                                     uintptr_t faultDescriptor)
{
    const LONG newest = g_eaglTraversalSequence;
    if (newest <= 0)
        return;

    std::printf("\nRecent EAGL traversal chain:\n");

    int shown = 0;
    const LONG oldest = (newest > 64) ? (newest - 63) : 1;

    for (LONG seq = newest; seq >= oldest && shown < 16; --seq) {
        const EaglTraversalEntry& e =
            g_eaglTraversalHistory[(seq - 1) & 63];

        if (e.sequence != seq ||
            e.threadId != threadId ||
            (dataObject && e.dataObject != dataObject))
            continue;

        std::printf("  #%ld type=%08lX mode=%lu flag8=%u count=%lu",
            (long)e.sequence,
            (unsigned long)e.typeObject,
            (unsigned long)e.typeMode,
            (unsigned)e.typeFlag8,
            (unsigned long)e.count);

        if (e.cursorInsideEbo)
            std::printf(" cursor=EBO+%08lX",
                (unsigned long)e.cursorFileOffset);
        else
            std::printf(" cursor=%08lX",
                (unsigned long)e.cursor);

        std::printf(" children=%lu list=%08lX caller=%08lX",
            (unsigned long)e.childCount,
            (unsigned long)e.childList,
            (unsigned long)e.caller);

        if (e.cursor == faultDescriptor)
            std::printf("  <== BAD DESCRIPTOR ENTERED HERE");

        std::printf("\n");
        ++shown;
    }

    if (!shown)
        std::printf("  <no matching traversal entries>\n");
}

void PrintLive06EaglStringForensics(EXCEPTION_POINTERS* ep) {
#if defined(_M_IX86)
    if (!ep || !ep->ContextRecord || ep->ContextRecord->Eip != 0x006E5821)
        return;

    if (!g_eaglFieldLive.valid) {
        std::printf("\nEBO field forensics:\n");
        std::printf("  Live capture   : unavailable\n");
        return;
    }

    const EaglFieldLiveContext c = g_eaglFieldLive;
    const uintptr_t faultData = ep->ContextRecord->Ecx;
    const uintptr_t faultHeader = faultData - 8;

    DWORD fileSize = 0;
    SafeReadU32(c.dataObject + 8, fileSize);

    const bool descriptorInsideFile =
        c.descriptor >= c.dataObject &&
        fileSize != 0 &&
        (c.descriptor - c.dataObject) < fileSize;

    DWORD descriptorFileOffset = 0;
    if (descriptorInsideFile)
        descriptorFileOffset =
            static_cast<DWORD>(c.descriptor - c.dataObject);

    const uintptr_t calculatedAddress =
        c.selectedBase + c.fieldOffset;

    // In this exact failure path the value computed by sub_6EC68D is later
    // used as the address that faults in the string hash. Do not subtract 8
    // here: the runtime evidence shows calculatedAddress == faultData.
    const bool exactFaultAddress =
        calculatedAddress == faultData;

    uintptr_t nominalBase = c.dataObject + c.dataOffset;
    uintptr_t nominalAddress = nominalBase + c.fieldOffset;

    float fieldAsFloat = 0.0f;
    const bool finiteFloat =
        LooksLikeFiniteFloatBits(static_cast<DWORD>(c.fieldOffset), fieldAsFloat);

    const bool fieldOffsetOutOfRange =
        fileSize != 0 && c.fieldOffset >= fileSize;

    std::printf("\nEBO field forensics (live capture @ 006EC6DE):\n");
    std::printf("  Data object    : %08lX\n", (unsigned long)c.dataObject);
    if (fileSize)
        std::printf("  EBO size       : %08lX\n", (unsigned long)fileSize);
    std::printf("  Type object    : %08lX\n", (unsigned long)c.typeObject);
    std::printf("  Descriptor     : %08lX\n", (unsigned long)c.descriptor);

    if (descriptorInsideFile)
        std::printf("  Desc file off  : %08lX  (descriptor - EBO base)\n",
            (unsigned long)descriptorFileOffset);

    std::printf("  Field raw      : %08lX\n",
        (unsigned long)c.fieldOffset);

    if (finiteFloat)
        std::printf("  Field as float : %.9g\n", fieldAsFloat);

    std::printf("  Field validity : %s\n",
        fieldOffsetOutOfRange
            ? "INVALID / outside EBO image"
            : "within EBO image range");

    std::printf("  Data offset    : %08lX  ([data object+20])\n",
        (unsigned long)c.dataOffset);
    std::printf("  Reloc state    : %04X  ([data object+2C])\n",
        (unsigned)c.relocationState);
    std::printf("  Fields left    : %lu\n", (unsigned long)c.fieldsRemaining);
    std::printf("  Selected base  : %08lX\n",
        (unsigned long)c.selectedBase);
    std::printf("  Calculated ptr : %08lX  (selected base + field raw)\n",
        (unsigned long)calculatedAddress);
    std::printf("  Fault address  : %08lX\n",
        (unsigned long)faultData);

    if (exactFaultAddress) {
        std::printf("  Address match  : EXACT\n");
        std::printf("  Finding        : sub_6EC68D calculated the exact address that faulted.\n");
    }
    else {
        std::printf("  Address match  : NO\n");
    }

    std::printf("  Nominal base   : %08lX  (data object + data offset)\n",
        (unsigned long)nominalBase);
    std::printf("  Nominal ptr    : %08lX\n",
        (unsigned long)nominalAddress);

    if (nominalBase == c.selectedBase)
        std::printf("  Relocation     : none for this field\n");
    else {
        std::printf("  Relocation     : alternate base selected\n");
        std::printf("  Base delta     : %08lX\n",
            (unsigned long)(c.selectedBase - nominalBase));
    }

    DWORD descriptorValue = 0;
    if (SafeReadU32(c.descriptor, descriptorValue)) {
        std::printf("  Desc [0]       : %08lX%s\n",
            (unsigned long)descriptorValue,
            descriptorValue == c.fieldOffset
                ? "  (matches field raw)"
                : "");
    }

    if (descriptorInsideFile)
        PrintEboDescriptorBackReferences(
            c.dataObject, fileSize, c.descriptor);

    PrintRecentEaglTraversalHistory(
        GetCurrentThreadId(),
        c.dataObject,
        c.descriptor);

    PrintEaglOuterRecordForensics(
        GetCurrentThreadId(),
        c.dataObject,
        c.descriptor);

    DWORD rawOwnerOff = 0;
    DWORD rawPayloadBytes = 0;
    DWORD rawStride = 0;
    DWORD rawPayloadOff = 0;
    DWORD rawElementCount = 0;
    DWORD rawOwnerTag = 0;

    const bool isRawBufferPayload =
        descriptorInsideFile &&
        DetectRawBufferOwner(
            c.dataObject,
            fileSize,
            c.descriptor,
            rawOwnerOff,
            rawPayloadBytes,
            rawStride,
            rawPayloadOff,
            rawElementCount,
            rawOwnerTag);

    if (isRawBufferPayload) {
        std::printf("\n  RAW BUFFER OWNERSHIP DETECTED\n");
        std::printf("  -----------------------------\n");
        std::printf("  Buffer header off : %08lX\n",
            (unsigned long)rawOwnerOff);
        std::printf("  Payload off       : %08lX\n",
            (unsigned long)rawPayloadOff);
        std::printf("  Payload bytes     : %lu\n",
            (unsigned long)rawPayloadBytes);
        std::printf("  Element stride    : %lu bytes\n",
            (unsigned long)rawStride);
        std::printf("  Element count     : %lu\n",
            (unsigned long)rawElementCount);
        std::printf("  Header tag        : %08lX\n",
            (unsigned long)rawOwnerTag);
        std::printf("  Meaning           : current 'descriptor' is actually the start of a raw data-buffer payload.\n");

        if (rawStride == 8)
            std::printf("  Data shape clue   : 8-byte elements are compatible with a Float2-style stream.\n");
        else if (rawStride == 12)
            std::printf("  Data shape clue   : 12-byte elements are compatible with a Float3-style stream.\n");
        else if (rawStride == 4)
            std::printf("  Data shape clue   : 4-byte elements are compatible with scalar/packed data.\n");
    }

    if (exactFaultAddress && fieldOffsetOutOfRange) {
        std::printf("\n  CORRUPT EBO FIELD DETECTED\n");
        std::printf("  --------------------------\n");
        if (descriptorInsideFile)
            std::printf("  Descriptor EBO offset : %08lX\n",
                (unsigned long)descriptorFileOffset);
        std::printf("  Raw descriptor value  : %08lX\n",
            (unsigned long)c.fieldOffset);
        if (finiteFloat)
            std::printf("  Same bits as float     : %.9g\n", fieldAsFloat);
        std::printf("  EBO image size         : %08lX\n",
            (unsigned long)fileSize);
        std::printf("  Selected runtime base  : %08lX\n",
            (unsigned long)c.selectedBase);
        std::printf("  Calculated bad address : %08lX\n",
            (unsigned long)calculatedAddress);
        std::printf("  Actual fault address   : %08lX\n",
            (unsigned long)faultData);
        std::printf("  Diagnosis              : descriptor data is being interpreted as an out-of-range string offset.\n");

        if (finiteFloat)
            std::printf("  Structural clue        : raw value is also a plausible floating-point geometry value.\n");

        if (isRawBufferPayload) {
            std::printf("  Root-cause clue        : traversal entered a raw EBO data-buffer payload as though it were a string-descriptor array.\n");
            std::printf("  Owner header           : EBO+%08lX -> payload EBO+%08lX\n",
                (unsigned long)rawOwnerOff,
                (unsigned long)rawPayloadOff);
            std::printf("  Next target            : determine which parent EAGL record passed this payload pointer into sub_6EC68D.\n");
        }
    }
#endif
}

const AssetHistoryEntry* FindNewestAcquiredWithExtension(const char* extension) {
    if (!extension)
        return nullptr;

    const LONG newest = g_assetHistoryCursor;
    if (newest < 0)
        return nullptr;

    const LONG count = (newest + 1 < 64) ? newest + 1 : 64;
    const LONG first = newest - count + 1;

    for (LONG i = newest; i >= first; --i) {
        const AssetHistoryEntry& e =
            g_assetHistory[static_cast<unsigned long>(i) & 63];

        if (e.sequence <= 0 ||
            e.kind != ASSET_REQUEST_ACQUIRE ||
            !e.logicalName[0])
            continue;

        const char* dot = std::strrchr(e.logicalName, '.');
        if (dot && _stricmp(dot, extension) == 0)
            return &e;
    }

    return nullptr;
}

const AssetHistoryEntry* FindNewestAcquiredEboOrFsh() {
    const LONG newest = g_assetHistoryCursor;
    if (newest < 0)
        return nullptr;

    const LONG count = (newest + 1 < 64) ? newest + 1 : 64;
    const LONG first = newest - count + 1;

    for (LONG i = newest; i >= first; --i) {
        const AssetHistoryEntry& e =
            g_assetHistory[static_cast<unsigned long>(i) & 63];

        if (e.sequence <= 0 ||
            e.kind != ASSET_REQUEST_ACQUIRE ||
            !e.logicalName[0])
            continue;

        const char* dot = std::strrchr(e.logicalName, '.');
        if (!dot)
            continue;

        if (_stricmp(dot, ".ebo") == 0 ||
            _stricmp(dot, ".fsh") == 0)
            return &e;
    }

    return nullptr;
}

void PrintAssetCrashContext(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ContextRecord)
        return;

#if defined(_M_IX86)
    const DWORD eip = ep->ContextRecord->Eip;
    const bool live06 =
        FM::GetEntryPoint() == 0x40109F &&
        plugin::patch::GetFloat(0xBD832C) == 1.3333334f;

    const AssetHistoryEntry* newestEbo =
        FindNewestAcquiredWithExtension(".ebo");
    const AssetHistoryEntry* newestFsh =
        FindNewestAcquiredWithExtension(".fsh");
    const AssetHistoryEntry* newestAsset =
        FindNewestAcquiredEboOrFsh();

    bool eboCrashSignature = false;

    // Known NBA Live 06 bug-report signatures discovered through runtime/IDA
    // tracing. These are deliberately reported before the generic newest-FSH
    // correlation block so a render/capacity failure is not mislabeled as an
    // FSH parser error.
    const bool resourceCapacityOverflow06 =
        live06 && eip == 0x007E1A71;

    const bool shaderConstantCrash06 =
        live06 &&
        StackContainsReturnAddress(ep->ContextRecord, 0x006FD1BE) &&
        StackContainsReturnAddress(ep->ContextRecord, 0x009E8836);

    if (live06) {
        eboCrashSignature =
            (eip >= 0x006E5810 && eip < 0x006E582D) ||
            (eip >= 0x006E5AD8 && eip < 0x006E5B25) ||
            (eip >= 0x006E62C5 && eip < 0x006E6354) ||
            (eip >= 0x006E9C9F && eip < 0x006E9D2E) ||
            (eip >= 0x006EAABA && eip < 0x006EAB7B) ||
            (eip >= 0x006EC68D && eip < 0x006EC6FE) ||
            (eip >= 0x006EC9FA && eip < 0x006ECA8B);
    }

    if (resourceCapacityOverflow06) {
        std::printf("\n");
        std::printf("==============================================================\n");
        std::printf(" RESOURCE CAPACITY / SIZE OVERFLOW\n");
        std::printf("==============================================================\n");
        if (newestFsh)
            std::printf("Last FSH        : %s\n", newestFsh->logicalName);
        else
            std::printf("Last FSH        : <unknown>\n");
        std::printf("Game            : NBA Live 06\n");
        std::printf("Crash address   : %08lX\n", (unsigned long)eip);
        std::printf("Function        : sub_7E17A0\n");
        std::printf("Fault           : corrupted local scene/resource pointer\n");
        std::printf("Dereference     : [var_C + 0x48]\n");
        std::printf("\nDiagnosis:\n");
        std::printf("  sub_7E17A0 uses fixed-size indexed temporary stack storage.\n");
        std::printf("  A sufficiently large scene/resource count can overwrite later\n");
        std::printf("  locals, including var_C, before the fault at 007E1A71.\n");
        std::printf("  Approximate overwrite boundary for the observed array is near\n");
        std::printf("  index 167; this is a diagnostic boundary, not a declared engine\n");
        std::printf("  specification limit.\n");
        std::printf("\nAsset evidence:\n");
        std::printf("  The last FSH is context only and is NOT proof of corruption.\n");
        std::printf("  Classification : scene/resource capacity overflow\n");
        std::printf("  Confidence     : HIGH for stack-local corruption mechanism\n");
        std::printf("==============================================================\n");
        return;
    }

    if (shaderConstantCrash06) {
        std::printf("\n");
        std::printf("==============================================================\n");
        std::printf(" SHADER / MATERIAL STATE FAILURE\n");
        std::printf("==============================================================\n");
        if (newestFsh)
            std::printf("Last FSH        : %s\n", newestFsh->logicalName);
        else
            std::printf("Last FSH        : <unknown>\n");
        std::printf("Game            : NBA Live 06\n");
        std::printf("Crash address   : %08lX\n", (unsigned long)eip);
        std::printf("Game wrapper    : 006FD1A4\n");
        std::printf("D3D call        : IDirect3DDevice9::SetVertexShaderConstantF\n");
        std::printf("Game call site  : 009E8831\n");
        std::printf("Start register  : 4\n");
        std::printf("Vector count    : 1\n");
        std::printf("Pointer source  : [render constant block + 0x08]\n");
        std::printf("Upstream cache  : dword_C87C98 / source material-state field +0x48\n");
        std::printf("\nDiagnosis:\n");
        std::printf("  The selected shader/material path reached the renderer, but the\n");
        std::printf("  vertex-shader constant data supplied for register 4 was invalid\n");
        std::printf("  when Direct3D consumed it. This is consistent with an unsupported\n");
        std::printf("  or incompletely initialized shader/material state.\n");
        std::printf("\nPossible contributing factor:\n");
        std::printf("  texture/VRAM/resource-allocation pressure may expose the state\n");
        std::printf("  failure, but is not proven by this crash signature alone.\n");
        std::printf("\nAsset evidence:\n");
        std::printf("  The last FSH is context only and is NOT proof of malformed FSH.\n");
        std::printf("  Classification : shader compatibility/state-allocation failure\n");
        std::printf("  Confidence     : HIGH for D3D shader-constant failure path\n");
        std::printf("==============================================================\n");
        return;
    }

    if (eboCrashSignature) {
        std::printf("\n");
        std::printf("==============================================================\n");
        std::printf(" EBO TOOLS DIAGNOSTIC\n");
        std::printf("==============================================================\n");

        if (newestEbo)
            std::printf("Asset           : %s\n", newestEbo->logicalName);
        else
            std::printf("Asset           : <unknown EBO>\n");

        std::printf("Game            : NBA Live 06\n");
        std::printf("Crash address   : %08lX\n", (unsigned long)eip);
        std::printf("Crash stage     : EAGL / GeomFile traversal\n");

        if (eip == 0x006E5821)
            std::printf("Crash operation : EaglCore string hash/read\n");

        if (g_eaglFieldLive.valid &&
            g_eaglFieldLive.threadId == GetCurrentThreadId())
        {
            const EaglFieldLiveContext c = g_eaglFieldLive;

            DWORD eboSize = 0;
            SafeReadU32(c.dataObject + 8, eboSize);

            DWORD descriptorOff = 0;
            const bool descriptorInside =
                c.descriptor >= c.dataObject &&
                eboSize &&
                (c.descriptor - c.dataObject) < eboSize;

            if (descriptorInside)
                descriptorOff =
                    static_cast<DWORD>(c.descriptor - c.dataObject);

            const uintptr_t calculated =
                c.selectedBase + c.fieldOffset;

            std::printf("\nRuntime field:\n");
            std::printf("  EBO size       : %08lX\n",
                (unsigned long)eboSize);

            if (descriptorInside)
                std::printf("  Bad cursor     : EBO+%08lX\n",
                    (unsigned long)descriptorOff);
            else
                std::printf("  Bad cursor     : %08lX\n",
                    (unsigned long)c.descriptor);

            std::printf("  Raw value      : %08lX\n",
                (unsigned long)c.fieldOffset);

            float rawFloat = 0.0f;
            if (LooksLikeFiniteFloatBits(
                    static_cast<DWORD>(c.fieldOffset), rawFloat))
                std::printf("  Raw as float   : %.9g\n", rawFloat);

            std::printf("  Selected base  : %08lX\n",
                (unsigned long)c.selectedBase);
            std::printf("  Calculated ptr : %08lX\n",
                (unsigned long)calculated);
            std::printf("  Fault address  : %08lX\n",
                (unsigned long)ep->ContextRecord->Ecx);

            if (calculated == ep->ContextRecord->Ecx)
                std::printf("  Address match  : EXACT\n");

            if (eboSize && c.fieldOffset >= eboSize)
                std::printf("  Error           : descriptor value is outside EBO range\n");
        }

        if (g_eaglOuterRecord.valid &&
            g_eaglOuterRecord.threadId == GetCurrentThreadId() &&
            g_eaglFieldLive.valid &&
            g_eaglOuterRecord.dataObject == g_eaglFieldLive.dataObject &&
            g_eaglOuterRecord.cursor == g_eaglFieldLive.descriptor)
        {
            const EaglOuterRecordContext r = g_eaglOuterRecord;

            std::printf("\nParent record:\n");
            std::printf("  Record offset  : EBO+%08lX\n",
                (unsigned long)r.recordFileOffset);
            std::printf("  Type index     : %u\n",
                (unsigned)r.typeIndex);
            std::printf("  Repeat count   : %lu\n",
                (unsigned long)r.repeatCount);
            std::printf("  Cursor stride  : %08lX\n",
                (unsigned long)r.cursorStride);
            std::printf("  Relative cursor: %08lX\n",
                (unsigned long)r.relativeCursor);
            std::printf("  Selected cursor: EBO+%08lX\n",
                (unsigned long)r.cursorFileOffset);

            if (r.recordFileOffset + r.relativeCursor ==
                r.cursorFileOffset)
                std::printf("  Cursor match   : EXACT\n");
        }

        std::printf("\nFailure summary:\n");
        if (eip == 0x006E5821 &&
            g_eaglFieldLive.valid)
        {
            std::printf("  EAGL interpreted data at the selected EBO cursor as a\n");
            std::printf("  string/offset descriptor. The resulting pointer caused\n");
            std::printf("  the access violation.\n");
        }
        else {
            std::printf("  Crash occurred inside the mapped NBA Live 06 EBO/EAGL\n");
            std::printf("  processing path.\n");
        }

        std::printf("==============================================================\n");
        return;
    }

    // FSH parsing/decoding functions are not mapped yet. For non-EBO crashes,
    // only emit an FSH diagnostic when an FSH is the newest acquired EBO/FSH
    // resource. This is correlation, not parser-level proof.
    if (newestAsset) {
        const char* dot = std::strrchr(newestAsset->logicalName, '.');

        if (dot && _stricmp(dot, ".fsh") == 0) {
            std::printf("\n");
            std::printf("==============================================================\n");
            std::printf(" FSH TOOLS DIAGNOSTIC\n");
            std::printf("==============================================================\n");
            std::printf("Asset           : %s\n",
                newestAsset->logicalName);
            std::printf("Crash address   : %08lX\n",
                (unsigned long)eip);
            std::printf("Evidence        : newest acquired EBO/FSH resource\n");
            std::printf("Confidence      : CORRELATION ONLY\n");
            std::printf("Parser detail   : FSH decoder/parser addresses not mapped yet\n");
            std::printf("==============================================================\n");
            return;
        }
    }

    // If the crash is not attributable to EBO or FSH, keep the asset-debug
    // output silent. The generic exception/register/stack report still prints.
#endif
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    if (!g_crashes || !ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    // Do not take the console lock here: the crashing thread may already own it.
    SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
    std::printf("\n\n==============================================================\n");
    std::printf(" GAME CRASHED\n");
    std::printf("==============================================================\n");
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    auto* er = ep->ExceptionRecord;
    auto* c = ep->ContextRecord;
    std::printf("Exception : %s (0x%08lX)\n", ExceptionName(er->ExceptionCode), er->ExceptionCode);
    std::printf("Address   : %p\n", er->ExceptionAddress);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* operation = er->ExceptionInformation[0] == 0 ? "READ" :
                                er->ExceptionInformation[0] == 1 ? "WRITE" : "EXECUTE";
        std::printf("Operation : %s %p\n", operation, reinterpret_cast<void*>(er->ExceptionInformation[1]));
    }

#if defined(_M_IX86)
    std::printf("\nRegisters:\n");
    std::printf("EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\n", c->Eax, c->Ebx, c->Ecx, c->Edx);
    std::printf("ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX\n", c->Esi, c->Edi, c->Ebp, c->Esp);
    std::printf("EIP=%08lX EFLAGS=%08lX\n", c->Eip, c->EFlags);
#endif
    PrintStack32(c);
    PrintAssetCrashContext(ep);

    std::printf("\nThe process is about to terminate. Press ENTER to close it.\n");
    std::fflush(stdout);
    (void)getchar();
    return EXCEPTION_EXECUTE_HANDLER;
}

void OpenConsole() {
    if (!AllocConsole()) return;
    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTitleA("NBA Live Debug Console");

    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("==============================================================\n");
    std::printf(" NBA Live Debug Console\n");
    std::printf(" %s\n", GameName());
    std::printf("==============================================================\n\n");
}

} // namespace

void InitializeDebugConsole() {
    g_enabled = GetPrivateProfileIntA("DEBUG", "CONSOLE", 0, ".\\main.ini") != 0;
    if (!g_enabled) return;

    // The IAT hooks must be installed exactly once. If initialization runs
    // twice and PatchIAT records HookCreateFileA/HookCloseHandle as the
    // "original" function, the hooks recurse into themselves until the thread
    // stack is exhausted.
    if (InterlockedCompareExchange(&g_debugConsoleInitState, 1, 0) != 0)
        return;

    g_files = GetPrivateProfileIntA("DEBUG", "FILES", 1, ".\\main.ini") != 0;
    g_failedFiles = GetPrivateProfileIntA("DEBUG", "FAILED_FILES", 1, ".\\main.ini") != 0;
    g_showCaller = GetPrivateProfileIntA("DEBUG", "FILE_CALLERS", 1, ".\\main.ini") != 0;
    g_crashes = GetPrivateProfileIntA("DEBUG", "CRASHES", 1, ".\\main.ini") != 0;
    g_assetFiles = GetPrivateProfileIntA("DEBUG", "ASSET_FILES", 1, ".\\main.ini") != 0;

    InitializeCriticalSection(&g_consoleLock);
    g_lockReady = true;
    OpenConsole();
    if (!g_console) {
        InterlockedExchange(&g_debugConsoleInitState, 0);
        return;
    }

    // FILES=0 now means no Win32 file IAT hooks at all. Previously the hooks
    // were installed unconditionally even when file logging was disabled.
    if (g_files) {
        bool a = PatchIAT(
            "KERNEL32.dll",
            "CreateFileA",
            reinterpret_cast<void*>(&HookCreateFileA),
            reinterpret_cast<void**>(&g_CreateFileA));

        bool w = PatchIAT(
            "KERNEL32.dll",
            "CreateFileW",
            reinterpret_cast<void*>(&HookCreateFileW),
            reinterpret_cast<void**>(&g_CreateFileW));

        bool c = PatchIAT(
            "KERNEL32.dll",
            "CloseHandle",
            reinterpret_cast<void*>(&HookCloseHandle),
            reinterpret_cast<void**>(&g_CloseHandle));

        PrintPrefix("DEBUG",
            FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        std::printf(
            "File hooks: CreateFileA=%s CreateFileW=%s CloseHandle=%s\n",
            a ? "OK" : "MISS",
            w ? "OK" : "MISS",
            c ? "OK" : "MISS");
    }
    else {
        PrintPrefix("DEBUG",
            FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        std::printf("Win32 file hooks: disabled (FILES=0)\n");
    }

    if (g_assetFiles) {
        const bool fs = InstallFileSystemOpenResolvedHookForCurrentGame();
        PrintPrefix(
            "DEBUG",
            fs ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
               : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
        std::printf(
            "%s FileSystem::OpenResolved hook: %s\n",
            GameName(),
            fs ? "OK" : "MISS");
    }

    if (g_assetFiles) {
        if (FM::GetEntryPoint() == 0x40109F &&
            plugin::patch::GetFloat(0xBD832C) == 1.3333334f)
        {
            PrintPrefix("DEBUG",
                FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
            std::printf("NBA Live 06 GeomFile processing: observed via crash stack/live EAGL capture\n");

            const bool eaglField = InstallEaglFieldStringHookForCurrentGame();
            PrintPrefix("DEBUG",
                eaglField ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
                          : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
            std::printf("NBA Live 06 EAGL field live hook: %s\n",
                eaglField ? "OK" : "MISS");

            const bool eaglTraversal = InstallEaglTraversalEntryHookForCurrentGame();
            PrintPrefix("DEBUG",
                eaglTraversal ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
                              : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
            std::printf("NBA Live 06 EAGL traversal hook: %s\n",
                eaglTraversal ? "OK" : "MISS");

            const bool eaglOuter = InstallEaglOuterRecordHookForCurrentGame();
            PrintPrefix("DEBUG",
                eaglOuter ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
                          : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
            std::printf("NBA Live 06 EAGL outer-record hook: %s\n",
                eaglOuter ? "OK" : "MISS");
        }
    }

    if (g_crashes) {
        SetUnhandledExceptionFilter(&CrashHandler);
        PrintPrefix("DEBUG",
            FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        std::printf("Crash handler installed\n");
    }

    std::printf("\n");
}

