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
bool g_animBanks = false;
bool g_live07CleanupFix = true;
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

    // NBA Live 06
    // 006DD915 call FileSystem_OpenResolved -> 006DD91A: boolean probe
    // 006DD94D call FileSystem_OpenResolved -> 006DD952: retained file object
    if (ret == 0x006DD91A)
        return ASSET_REQUEST_PROBE;
    if (ret == 0x006DD952)
        return ASSET_REQUEST_ACQUIRE;

    // NBA Live 07
    // 008004EA call FileSystem_OpenResolved -> 008004EF: boolean probe
    // 00800522 call FileSystem_OpenResolved -> 00800527: retained file object
    if (ret == 0x008004EF)
        return ASSET_REQUEST_PROBE;
    if (ret == 0x00800527)
        return ASSET_REQUEST_ACQUIRE;

    // NBA Live 08
    // 0082F509 call FileSystem_OpenResolved -> 0082F50E: boolean probe
    // 0082F541 call FileSystem_OpenResolved -> 0082F546: retained file object
    if (ret == 0x0082F50E)
        return ASSET_REQUEST_PROBE;
    if (ret == 0x0082F546)
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

HANDLE WINAPI HookCreateFileA(
    LPCSTR fileName,
    DWORD access,
    DWORD share,
    LPSECURITY_ATTRIBUTES sa,
    DWORD creation,
    DWORD flags,
    HANDLE templ)
{
    void* caller = _ReturnAddress();

    HANDLE h = g_CreateFileA(
        fileName, access, share, sa, creation, flags, templ);

    // Preserve EXACT Win32 state returned by CreateFileA.
    const DWORD error = GetLastError();

    PrintFileResult(fileName, h, error, caller);

    if (h != INVALID_HANDLE_VALUE)
        RememberHandle(h, fileName);

    SetLastError(error);
    return h;
}

HANDLE WINAPI HookCreateFileW(
    LPCWSTR fileName,
    DWORD access,
    DWORD share,
    LPSECURITY_ATTRIBUTES sa,
    DWORD creation,
    DWORD flags,
    HANDLE templ)
{
    void* caller = _ReturnAddress();

    HANDLE h = g_CreateFileW(
        fileName, access, share, sa, creation, flags, templ);

    // Capture it BEFORE WideCharToMultiByte / printf / anything else.
    const DWORD error = GetLastError();

    char path[1024] = "<wide path>";

    if (fileName) {
        WideCharToMultiByte(
            CP_ACP,
            0,
            fileName,
            -1,
            path,
            sizeof(path),
            nullptr,
            nullptr);
    }

    PrintFileResult(path, h, error, caller);

    if (h != INVALID_HANDLE_VALUE)
        RememberHandle(h, path);

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
// NBA Live 07 invalid resource-owner diagnostics
// -------------------------------------------------------------------------
//
// Wrapper @ 0080B32B:
//   arg1 = result/status pointer
//   arg2 = resource object
//   [resource+2C] = owner/manager pointer
//
// It forwards that owner pointer as ECX to 0080B103. The observed crash at
// 0080B10B is the first dereference of owner+1C, meaning the owner pointer was
// already invalid before entering 0080B103.

struct Live07ResourceOwnerContext {
    volatile LONG valid;
    DWORD threadId;
    uintptr_t caller;
    uintptr_t resultPtr;
    uintptr_t objectPtr;
    uintptr_t ownerPtr;

    DWORD objectWords[16];
    DWORD ownerWords[16];

    BOOL objectReadable;
    BOOL ownerReadable;

    uintptr_t objectRegionBase;
    SIZE_T objectRegionSize;
    DWORD objectProtect;
    DWORD objectRegionType;
    uintptr_t objectAllocationBase;
    DWORD objectAllocationProtect;

    uintptr_t ownerRegionBase;
    SIZE_T ownerRegionSize;
    DWORD ownerProtect;
    DWORD ownerRegionType;
    uintptr_t ownerAllocationBase;
    DWORD ownerAllocationProtect;

    BOOL cleanupArrayMatch;
    uintptr_t cleanupObject;
    uintptr_t cleanupSlotAddress;
    DWORD cleanupSlotIndex;
    DWORD cleanupSlots[20];
};

Live07ResourceOwnerContext g_live07ResourceOwner = {};


struct Live07CleanupContext {
    volatile LONG valid;
    DWORD threadId;
    uintptr_t caller;
    uintptr_t cleanupObject;
};

Live07CleanupContext g_live07Cleanup = {};

void __cdecl CaptureLive07CleanupEntry(
    uintptr_t cleanupObject,
    const uintptr_t* entryStack)
{
    g_live07Cleanup.threadId = GetCurrentThreadId();
    g_live07Cleanup.cleanupObject = cleanupObject;
    g_live07Cleanup.caller =
        entryStack ? entryStack[0] : 0;
    InterlockedExchange(&g_live07Cleanup.valid, 1);
}

__declspec(naked) void HookLive07CleanupEntry()
{
    __asm {
        // Original entry to 00689B70:
        //   ECX = cleanup object
        //
        // Preserve the complete entry state before logging.
        pushfd
        pushad

        // Original ECX saved by pushad.
        mov     eax, [esp+24]

        // Original entry ESP = current ESP + 36.
        lea     edx, [esp+36]

        push    edx
        push    eax
        call    CaptureLive07CleanupEntry
        add     esp, 8

        popad
        popfd

        // Reproduce overwritten bytes:
        //   83 EC 0C    sub esp,0Ch
        //   53          push ebx
        //   55          push ebp
        sub     esp, 0Ch
        push    ebx
        push    ebp

        push    0689B75h
        ret
    }
}


using Live07ReleaseResource_t = void (__cdecl *)(void* resultPtr, void* resourceObject);

bool IsReadablePointerRange(uintptr_t address, size_t size);

bool IsReadableObject07(uintptr_t p, size_t size)
{
    return IsReadablePointerRange(p, size);
}

bool IsSafeCleanupResource07(uintptr_t objectPtr, uintptr_t& ownerPtr)
{
    ownerPtr = 0;

    if (!objectPtr || !IsReadableObject07(objectPtr, 0x30))
        return false;

    __try {
        ownerPtr =
            *reinterpret_cast<const uintptr_t*>(objectPtr + 0x2C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ownerPtr = 0;
        return false;
    }

    if (!ownerPtr || !IsReadableObject07(ownerPtr, 0x20))
        return false;

    return true;
}

void __cdecl SafeReleaseResource07(void* resultPtr, void* resourceObject)
{
    const uintptr_t objectPtr =
        reinterpret_cast<uintptr_t>(resourceObject);

    uintptr_t ownerPtr = 0;
    if (!IsSafeCleanupResource07(objectPtr, ownerPtr)) {
        if (g_lockReady) {
            EnterCriticalSection(&g_consoleLock);
            PrintPrefix(
                "FIX07",
                FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);

            std::printf(
                "SKIP stale cleanup resource object=%08lX owner=%08lX\n",
                static_cast<unsigned long>(objectPtr),
                static_cast<unsigned long>(ownerPtr));

            LeaveCriticalSection(&g_consoleLock);
        }

        // The original cleanup loop clears its slot immediately after the
        // call returns, so returning here safely discards the stale entry.
        return;
    }

    reinterpret_cast<Live07ReleaseResource_t>(0x0080B32B)(
        resultPtr,
        resourceObject);
}

bool InstallLive07CleanupGuard()
{
    constexpr uintptr_t callAddress = 0x00689BA5;
    constexpr size_t patchLength = 5;

    // Original:
    //   E8 81 17 18 00    call 0080B32B
    const BYTE expected[patchLength] = {
        0xE8, 0x81, 0x17, 0x18, 0x00
    };

    BYTE* target = reinterpret_cast<BYTE*>(callAddress);

    __try {
        if (std::memcmp(target, expected, patchLength) != 0)
            return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            target,
            patchLength,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
        return false;

    target[0] = 0xE8;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&SafeReleaseResource07) -
            (callAddress + 5));

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    return true;
}

bool InstallLive07CleanupEntryHook()
{
    constexpr uintptr_t targetAddress = 0x00689B70;
    constexpr size_t patchLength = 5;

    const BYTE expected[patchLength] = {
        0x83, 0xEC, 0x0C,
        0x53,
        0x55
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
    if (!VirtualProtect(
            target,
            patchLength,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
        return false;

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookLive07CleanupEntry) -
            (targetAddress + 5));

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    return true;
}

bool IsReadablePointerRange(uintptr_t address, size_t size)
{
    if (!address || !size)
        return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)))
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD protect = mbi.Protect & 0xFF;
    if (protect == PAGE_NOACCESS || protect == PAGE_GUARD)
        return false;

    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionStart + mbi.RegionSize;

    return address >= regionStart &&
           address + size >= address &&
           address + size <= regionEnd;
}


void CapturePointerRegion07(
    uintptr_t p,
    BOOL& readable,
    uintptr_t& regionBase,
    SIZE_T& regionSize,
    DWORD& protect,
    DWORD& regionType,
    uintptr_t& allocationBase,
    DWORD& allocationProtect)
{
    readable = FALSE;
    regionBase = 0;
    regionSize = 0;
    protect = 0;
    regionType = 0;
    allocationBase = 0;
    allocationProtect = 0;

    if (!p)
        return;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi)))
        return;

    regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    regionSize = mbi.RegionSize;
    protect = mbi.Protect;
    regionType = mbi.Type;
    allocationBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    allocationProtect = mbi.AllocationProtect;

    if (mbi.State != MEM_COMMIT)
        return;

    const DWORD pflags = mbi.Protect & 0xFF;
    if (pflags == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD))
        return;

    readable = TRUE;
}

void SnapshotWords07(uintptr_t p, DWORD* dst, size_t count)
{
    if (!dst || !count)
        return;

    for (size_t i = 0; i < count; ++i)
        dst[i] = 0;

    if (!p)
        return;

    __try {
        for (size_t i = 0; i < count; ++i)
            dst[i] = *reinterpret_cast<const DWORD*>(p + i * 4);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __cdecl CaptureLive07ResourceOwner(const uintptr_t* entryStack)
{
    if (!entryStack)
        return;

    Live07ResourceOwnerContext c = {};
    c.threadId = GetCurrentThreadId();
    c.caller = entryStack[0];
    c.resultPtr = entryStack[1];
    c.objectPtr = entryStack[2];

    __try {
        if (c.objectPtr)
            c.ownerPtr =
                *reinterpret_cast<const uintptr_t*>(c.objectPtr + 0x2C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        c.ownerPtr = 0;
    }

    CapturePointerRegion07(
        c.objectPtr,
        c.objectReadable,
        c.objectRegionBase,
        c.objectRegionSize,
        c.objectProtect,
        c.objectRegionType,
        c.objectAllocationBase,
        c.objectAllocationProtect);

    CapturePointerRegion07(
        c.ownerPtr,
        c.ownerReadable,
        c.ownerRegionBase,
        c.ownerRegionSize,
        c.ownerProtect,
        c.ownerRegionType,
        c.ownerAllocationBase,
        c.ownerAllocationProtect);

    if (c.objectReadable)
        SnapshotWords07(c.objectPtr, c.objectWords, 16);

    if (c.ownerReadable)
        SnapshotWords07(c.ownerPtr, c.ownerWords, 16);

    if (g_live07Cleanup.valid &&
        g_live07Cleanup.threadId == c.threadId)
    {
        c.cleanupObject = g_live07Cleanup.cleanupObject;

        __try {
            const uintptr_t firstSlot = c.cleanupObject + 0x7E0;

            for (DWORD i = 0; i < 20; ++i) {
                const uintptr_t slotAddress =
                    firstSlot + static_cast<uintptr_t>(i) * 4;

                const DWORD value =
                    *reinterpret_cast<const DWORD*>(slotAddress);

                c.cleanupSlots[i] = value;

                if (!c.cleanupArrayMatch &&
                    static_cast<uintptr_t>(value) == c.objectPtr)
                {
                    c.cleanupArrayMatch = TRUE;
                    c.cleanupSlotAddress = slotAddress;
                    c.cleanupSlotIndex = i;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    g_live07ResourceOwner.threadId = c.threadId;
    g_live07ResourceOwner.caller = c.caller;
    g_live07ResourceOwner.resultPtr = c.resultPtr;
    g_live07ResourceOwner.objectPtr = c.objectPtr;
    g_live07ResourceOwner.ownerPtr = c.ownerPtr;

    std::memcpy(g_live07ResourceOwner.objectWords, c.objectWords, sizeof(c.objectWords));
    std::memcpy(g_live07ResourceOwner.ownerWords, c.ownerWords, sizeof(c.ownerWords));

    g_live07ResourceOwner.objectReadable = c.objectReadable;
    g_live07ResourceOwner.ownerReadable = c.ownerReadable;
    g_live07ResourceOwner.objectRegionBase = c.objectRegionBase;
    g_live07ResourceOwner.objectRegionSize = c.objectRegionSize;
    g_live07ResourceOwner.objectProtect = c.objectProtect;
    g_live07ResourceOwner.objectRegionType = c.objectRegionType;
    g_live07ResourceOwner.objectAllocationBase = c.objectAllocationBase;
    g_live07ResourceOwner.objectAllocationProtect = c.objectAllocationProtect;

    g_live07ResourceOwner.ownerRegionBase = c.ownerRegionBase;
    g_live07ResourceOwner.ownerRegionSize = c.ownerRegionSize;
    g_live07ResourceOwner.ownerProtect = c.ownerProtect;
    g_live07ResourceOwner.ownerRegionType = c.ownerRegionType;
    g_live07ResourceOwner.ownerAllocationBase = c.ownerAllocationBase;
    g_live07ResourceOwner.ownerAllocationProtect = c.ownerAllocationProtect;

    g_live07ResourceOwner.cleanupArrayMatch = c.cleanupArrayMatch;
    g_live07ResourceOwner.cleanupObject = c.cleanupObject;
    g_live07ResourceOwner.cleanupSlotAddress = c.cleanupSlotAddress;
    g_live07ResourceOwner.cleanupSlotIndex = c.cleanupSlotIndex;
    std::memcpy(
        g_live07ResourceOwner.cleanupSlots,
        c.cleanupSlots,
        sizeof(c.cleanupSlots));

    InterlockedExchange(&g_live07ResourceOwner.valid, 1);

    if (c.objectPtr && c.ownerPtr &&
        !IsReadablePointerRange(c.ownerPtr, 0x20))
    {
        EnterCriticalSection(&g_consoleLock);
        PrintPrefix(
            "EAGL07",
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);

        std::printf(
            "INVALID OWNER object=%08lX owner=%08lX caller=%08lX\n",
            static_cast<unsigned long>(c.objectPtr),
            static_cast<unsigned long>(c.ownerPtr),
            static_cast<unsigned long>(c.caller));

        LeaveCriticalSection(&g_consoleLock);
    }
}

__declspec(naked) void HookLive07ResourceOwner()
{
    __asm {
        mov     eax, esp
        pushfd
        pushad
        push    eax
        call    CaptureLive07ResourceOwner
        add     esp, 4
        popad
        popfd

        // Reproduce overwritten bytes at 0080B32B:
        // push ebp
        // mov  ebp,esp
        // mov  eax,[ebp+0Ch]
        push    ebp
        mov     ebp, esp
        mov     eax, [ebp+0Ch]

        mov     edx, 080B331h
        jmp     edx
    }
}

bool InstallLive07ResourceOwnerHook()
{
    constexpr uintptr_t targetAddress = 0x0080B32B;
    constexpr size_t patchLength = 6;

    const BYTE expected[patchLength] = {
        0x55,
        0x8B, 0xEC,
        0x8B, 0x45, 0x0C
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
    if (!VirtualProtect(
            target,
            patchLength,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
        return false;

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookLive07ResourceOwner) -
            (targetAddress + 5));
    target[5] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    return true;
}


void PrintLive07RawStackCandidates(const CONTEXT* c)
{
#if defined(_M_IX86)
    if (!c)
        return;

    std::printf("\nRaw stack executable-address candidates:\n");

    int shown = 0;
    for (DWORD off = 0; off < 0x180 && shown < 24; off += 4) {
        DWORD value = 0;

        __try {
            value = *reinterpret_cast<const DWORD*>(c->Esp + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }

        // NBA Live 07 executable code is principally in this range.
        if (value >= 0x00400000 && value < 0x00B00000) {
            std::printf(
                "  ESP+%03lX = %08lX\n",
                static_cast<unsigned long>(off),
                static_cast<unsigned long>(value));
            ++shown;
        }
    }

    if (!shown)
        std::printf("  <none>\n");
#endif
}

void PrintLive07ResourceOwnerCrash(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ContextRecord ||
        ep->ContextRecord->Eip != 0x0080B10B)
        return;

    std::printf("\n");
    std::printf("==============================================================\n");
    std::printf(" NBA LIVE 07 RESOURCE OWNER CORRUPTION\n");
    std::printf("==============================================================\n");
    std::printf("Crash address   : 0080B10B\n");
    std::printf("Faulting read   : mov edx,[esi+1Ch]\n");
    std::printf("Owner / ESI     : %08lX\n",
        static_cast<unsigned long>(ep->ContextRecord->Esi));

    if (g_live07ResourceOwner.valid &&
        g_live07ResourceOwner.threadId == GetCurrentThreadId())
    {
        const Live07ResourceOwnerContext c = g_live07ResourceOwner;
        std::printf("Resource object : %08lX\n",
            static_cast<unsigned long>(c.objectPtr));
        std::printf("Object +2C      : %08lX\n",
            static_cast<unsigned long>(c.ownerPtr));
        std::printf("Wrapper caller  : %08lX\n",
            static_cast<unsigned long>(c.caller));

        if (c.ownerPtr == ep->ContextRecord->Esi)
            std::printf("Pointer match   : EXACT\n");

        std::printf("Object region   : %08lX + %08lX protect=%08lX type=%08lX readable=%s\n",
            static_cast<unsigned long>(c.objectRegionBase),
            static_cast<unsigned long>(c.objectRegionSize),
            static_cast<unsigned long>(c.objectProtect),
            static_cast<unsigned long>(c.objectRegionType),
            c.objectReadable ? "YES" : "NO");
        std::printf("Object alloc    : base=%08lX allocProtect=%08lX\n",
            static_cast<unsigned long>(c.objectAllocationBase),
            static_cast<unsigned long>(c.objectAllocationProtect));

        std::printf("Owner region    : %08lX + %08lX protect=%08lX type=%08lX readable=%s\n",
            static_cast<unsigned long>(c.ownerRegionBase),
            static_cast<unsigned long>(c.ownerRegionSize),
            static_cast<unsigned long>(c.ownerProtect),
            static_cast<unsigned long>(c.ownerRegionType),
            c.ownerReadable ? "YES" : "NO");
        std::printf("Owner alloc     : base=%08lX allocProtect=%08lX\n",
            static_cast<unsigned long>(c.ownerAllocationBase),
            static_cast<unsigned long>(c.ownerAllocationProtect));

        std::printf("\nResource object snapshot (+00..+3C):\n");
        for (int i = 0; i < 16; i += 4) {
            std::printf("  +%02X: %08lX %08lX %08lX %08lX\n",
                i * 4,
                static_cast<unsigned long>(c.objectWords[i + 0]),
                static_cast<unsigned long>(c.objectWords[i + 1]),
                static_cast<unsigned long>(c.objectWords[i + 2]),
                static_cast<unsigned long>(c.objectWords[i + 3]));
        }

        if (c.cleanupObject) {
            std::printf("\nCleanup array provenance:\n");
            std::printf("  Cleanup object : %08lX\n",
                static_cast<unsigned long>(c.cleanupObject));

            if (c.cleanupArrayMatch) {
                std::printf("  Slot index     : %lu / 19\n",
                    static_cast<unsigned long>(c.cleanupSlotIndex));
                std::printf("  Slot address   : %08lX\n",
                    static_cast<unsigned long>(c.cleanupSlotAddress));
                std::printf("  Slot offset    : +%08lX\n",
                    static_cast<unsigned long>(
                        c.cleanupSlotAddress - c.cleanupObject));
                std::printf("  Finding        : exact pointer came from the 20-entry cleanup array.\n");
            }
            else {
                std::printf("  Slot match     : NONE\n");
            }

            std::printf("  Slots 00-19    :\n");
            for (int i = 0; i < 20; i += 4) {
                std::printf(
                    "    [%02d] %08lX  [%02d] %08lX  [%02d] %08lX  [%02d] %08lX\n",
                    i + 0, static_cast<unsigned long>(c.cleanupSlots[i + 0]),
                    i + 1, static_cast<unsigned long>(c.cleanupSlots[i + 1]),
                    i + 2, static_cast<unsigned long>(c.cleanupSlots[i + 2]),
                    i + 3, static_cast<unsigned long>(c.cleanupSlots[i + 3]));
            }
        }

        if (c.ownerReadable) {
            std::printf("\nOwner snapshot (+00..+3C):\n");
            for (int i = 0; i < 16; i += 4) {
                std::printf("  +%02X: %08lX %08lX %08lX %08lX\n",
                    i * 4,
                    static_cast<unsigned long>(c.ownerWords[i + 0]),
                    static_cast<unsigned long>(c.ownerWords[i + 1]),
                    static_cast<unsigned long>(c.ownerWords[i + 2]),
                    static_cast<unsigned long>(c.ownerWords[i + 3]));
            }
        }
    }

    PrintLive07RawStackCandidates(ep->ContextRecord);

    std::printf("\nDiagnosis:\n");
    std::printf("  00689B70 is a cleanup/reset routine. It walks 20 stored resource\n");
    std::printf("  pointers and releases non-NULL entries. The direct call at\n");
    std::printf("  00689BA5 passes one of those stored pointers to 0080B32B.\n");
    std::printf("  The failing object exists, but its +2C owner field is NULL.\n");
    std::printf("  This is a longstanding Live 07 cleanup edge case and is not\n");
    std::printf("  evidence of memory-expansion failure or a specific asset file.\n");
    std::printf("==============================================================\n");
}

#else

bool InstallLive07ResourceOwnerHook()
{
    return false;
}

bool InstallLive07CleanupEntryHook()
{
    return false;
}

bool InstallLive07CleanupGuard()
{
    return false;
}

void PrintLive07ResourceOwnerCrash(EXCEPTION_POINTERS*)
{
}

#endif


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
// NBA Live 06/07/08:
//   FileSystem_OpenResolved @
//     06: 0x006DC8F9
//     07: 0x007FF4CE
//     08: 0x0082E4ED
//   Shared cdecl-style stack arguments:
//     ESP+04  = internal constant/type (observed 0x28)
//     ESP+08  = filename
//     ESP+0C  = flags
//     ESP+10  = search/filesystem context
//
// All supported resolver functions have a complete 9-byte prologue:
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
        // Preserve ALL original registers first.
        pushfd
        pushad

        // Entry ESP was 36 bytes above current ESP:
        //   pushfd = 4
        //   pushad = 32
        lea     edx, [esp + 36]

        // EAX saved by pushad is at [esp + 28].
        // For Live 2005 this is the original register context.
        mov     eax, [esp + 28]

        push    edx
        push    eax
        call    LogFileSystemOpenResolved
        add     esp, 8

        // Restore the caller's exact original register state.
        popad
        popfd

        jmp     dword ptr[g_FileSystemOpenResolvedTrampoline]
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

    if (ep == 0x40109F) {
        if (plugin::patch::GetFloat(0xBD832C) == 1.3333334f) {
            // NBA Live 06 1.0 NOCD
            // 006DC8F9: push ebp / mov ebp,esp / sub esp,108h
            return InstallFileSystemOpenResolvedHook(
                0x006DC8F9,
                0x108,
                ASSET_LAYOUT_LIVE06);
        }

        if (plugin::patch::GetFloat(0xBBBC3C) == 1.3333334f) {
            // NBA Live 07 1.1 NOCD
            // 007FF4CE: push ebp / mov ebp,esp / sub esp,108h
            // Argument layout is structurally identical to Live 06.
            return InstallFileSystemOpenResolvedHook(
                0x007FF4CE,
                0x108,
                ASSET_LAYOUT_LIVE06);
        }

        if (plugin::patch::GetFloat(0xC3DF84) == 1.3333334f) {
            // NBA Live 08 1.0 NOCD
            // 0082E4ED: push ebp / mov ebp,esp / sub esp,108h
            // Argument layout is structurally identical to Live 06.
            return InstallFileSystemOpenResolvedHook(
                0x0082E4ED,
                0x108,
                ASSET_LAYOUT_LIVE06);
        }
    }

    return false;
}



// -------------------------------------------------------------------------
// NBA Live 06/07/08 animation-bank pair debugger
// -------------------------------------------------------------------------
//
// AnimationBankSystem_LoadPair:
 //   06: 00645D10
 //   07: 006A3F00
 //   08: 006CB0F0
//   ECX      = animation-bank system
//   ESP+04   = bank index
//   ESP+08   = variant
//   ESP+0C   = ABK stem (normally "%08d")
//
// The function resolves the same (index, variant) through the parsed
// animbank.log table, then loads:
//   <abkStem>.abk
//   <variantRecord.objectStem>.o
//
// AnimBankEntry layout established in Live 06 and confirmed structurally
// identical in the Live 07/08 pair loaders:
//   +40 index
//   +44 first variant
//   +4C next-by-index
//
// AnimVariant:
//   +00 object stem string
//   +40 variant
//   +44 next variant

uintptr_t g_AnimBankPairContinue = 0;

bool SafeCopyCString(const char* src, char* dst, size_t dstSize)
{
    if (!dst || dstSize == 0)
        return false;

    dst[0] = '\0';
    if (!src)
        return false;

    __try {
        size_t i = 0;
        for (; i + 1 < dstSize; ++i) {
            const char c = src[i];
            dst[i] = c;
            if (!c)
                return true;
        }
        dst[dstSize - 1] = '\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        dst[0] = '\0';
        return false;
    }
}

const char* ResolveAnimBankObjectStem(
    void* self,
    int bankIndex,
    int variant,
    char* outStem,
    size_t outStemSize)
{
    if (!self || !outStem || outStemSize == 0)
        return nullptr;

    outStem[0] = '\0';

    __try {
        const uintptr_t base = reinterpret_cast<uintptr_t>(self);
        const uintptr_t table = *reinterpret_cast<const uintptr_t*>(base + 0x04);
        if (!table)
            return nullptr;

        const uintptr_t buckets =
            *reinterpret_cast<const uintptr_t*>(table + 0x04);
        const int bucketCount =
            *reinterpret_cast<const int*>(table + 0x08);

        if (!buckets || bucketCount <= 0 || bankIndex < 0)
            return nullptr;

        const int bucket = bankIndex % bucketCount;
        uintptr_t entry =
            *reinterpret_cast<const uintptr_t*>(
                buckets + static_cast<uintptr_t>(bucket) * 4);

        for (int guard = 0; entry && guard < 512; ++guard) {
            const int entryIndex =
                *reinterpret_cast<const int*>(entry + 0x40);

            if (entryIndex == bankIndex) {
                uintptr_t v =
                    *reinterpret_cast<const uintptr_t*>(entry + 0x44);

                for (int vguard = 0; v && vguard < 128; ++vguard) {
                    const int entryVariant =
                        *reinterpret_cast<const int*>(v + 0x40);

                    if (entryVariant == variant) {
                        if (SafeCopyCString(
                                reinterpret_cast<const char*>(v),
                                outStem,
                                outStemSize))
                            return outStem;
                        return nullptr;
                    }

                    v = *reinterpret_cast<const uintptr_t*>(v + 0x44);
                }

                return nullptr;
            }

            entry = *reinterpret_cast<const uintptr_t*>(entry + 0x4C);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }

    return nullptr;
}

void __cdecl LogAnimBankPair(
    void* self,
    const uintptr_t* entryStack)
{
    if (!g_animBanks || !g_lockReady || !entryStack)
        return;

    const void* caller =
        reinterpret_cast<const void*>(entryStack[0]);
    const int bankIndex =
        static_cast<int>(entryStack[1]);
    const int variant =
        static_cast<int>(entryStack[2]);
    const char* abkStem =
        reinterpret_cast<const char*>(entryStack[3]);

    char safeAbkStem[128] = {};
    char objectStem[128] = {};

    SafeCopyCString(abkStem, safeAbkStem, sizeof(safeAbkStem));
    ResolveAnimBankObjectStem(
        self,
        bankIndex,
        variant,
        objectStem,
        sizeof(objectStem));

    EnterCriticalSection(&g_consoleLock);
    PrintPrefix(
        "ANIM",
        FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);

    std::printf(
        "PAIR index=%d variant=%d ABK=%s.abk O=%s.o",
        bankIndex,
        variant,
        safeAbkStem[0] ? safeAbkStem : "<unknown>",
        objectStem[0] ? objectStem : "<unresolved>");

    if (g_showCaller)
        std::printf(" caller=%p", caller);

    std::printf("\n");
    LeaveCriticalSection(&g_consoleLock);
}

__declspec(naked) void HookAnimBankPair()
{
    __asm {
        // Preserve entry stack and original ECX before touching registers.
        mov     eax, esp
        pushfd
        pushad

        push    eax
        push    ecx
        call    LogAnimBankPair
        add     esp, 8

        popad
        popfd

        // Reproduce the exact five bytes overwritten at 00645D10:
        //   53                push ebx
        //   8B 5C 24 0C      mov ebx,[esp+0Ch]
        push    ebx
        mov     ebx, [esp+0Ch]

        jmp     dword ptr [g_AnimBankPairContinue]
    }
}

bool InstallAnimBankPairHookAt(uintptr_t targetAddress)
{
    constexpr size_t patchLength = 5;

    // All three games use the same entry sequence:
    //   53                push ebx
    //   8B 5C 24 0C      mov ebx,[esp+0Ch]
    //
    // Entry stack layout:
    //   ESP+04 = anim bank index
    //   ESP+08 = variant
    //   ESP+0C = ABK stem (normally "%08d")
    const BYTE expected[patchLength] = {
        0x53,
        0x8B, 0x5C, 0x24, 0x0C
    };

    BYTE* target = reinterpret_cast<BYTE*>(targetAddress);

    __try {
        if (std::memcmp(target, expected, patchLength) != 0)
            return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    g_AnimBankPairContinue = targetAddress + patchLength;

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            target,
            patchLength,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        g_AnimBankPairContinue = 0;
        return false;
    }

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookAnimBankPair) -
            (targetAddress + 5));

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);

    return true;
}

bool InstallAnimBankPairHookForCurrentGame()
{
    if (!g_animBanks)
        return false;

    const uintptr_t ep = FM::GetEntryPoint();

    if (ep == 0xCD8005) {
        // Live 2005 not mapped yet.
        return false;
    }

    if (ep == 0x40109F) {
        if (plugin::patch::GetFloat(0xBD832C) == 1.3333334f) {
            // NBA Live 06
            // Pair loader: 00645D10
            return InstallAnimBankPairHookAt(0x00645D10);
        }

        if (plugin::patch::GetFloat(0xBBBC3C) == 1.3333334f) {
            // NBA Live 07
            // Pair loader: 006A3F00
            return InstallAnimBankPairHookAt(0x006A3F00);
        }

        if (plugin::patch::GetFloat(0xC3DF84) == 1.3333334f) {
            // NBA Live 08
            // Pair loader: 006CB0F0
            return InstallAnimBankPairHookAt(0x006CB0F0);
        }
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

bool InstallAnimBankPairHookForCurrentGame()
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


// -------------------------------------------------------------------------
// NBA Live 06 render/material float4 diagnostics
// -------------------------------------------------------------------------
//
// sub_9F6D30 updates cached renderer state. The slot at materialState+0x5C
// becomes global dword_C9414C and is later consumed by sub_9FEBFE as a
// pointer to four floats. Crash 009FEC0D occurs when that pointer is NULL.
//
// Hook point 009F6EC0:
//   EAX = material-state object
//   [EAX+5C] = raw float4 pointer
//   [EAX+60] = flags (bit 0 selects per-frame array addressing)
//   word_CC0A3C = active frame/index

struct RenderFloat4LiveContext {
    volatile LONG valid;
    DWORD threadId;
    uintptr_t materialState;
    uintptr_t rawPointer;
    DWORD flags;
    WORD activeIndex;
    uintptr_t resolvedPointer;
};

RenderFloat4LiveContext g_renderFloat4Live = {};

void __cdecl CaptureRenderFloat4Live06(uintptr_t materialState)
{
    RenderFloat4LiveContext c = {};
    c.threadId = GetCurrentThreadId();
    c.materialState = materialState;

    __try {
        if (materialState) {
            c.rawPointer =
                *reinterpret_cast<const uintptr_t*>(materialState + 0x5C);
            c.flags =
                *reinterpret_cast<const DWORD*>(materialState + 0x60);
        }

        c.activeIndex =
            *reinterpret_cast<const WORD*>(0x00CC0A3C);

        c.resolvedPointer = c.rawPointer;
        if (c.flags & 1)
            c.resolvedPointer +=
                static_cast<uintptr_t>(c.activeIndex) * 0x10;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    g_renderFloat4Live.threadId = c.threadId;
    g_renderFloat4Live.materialState = c.materialState;
    g_renderFloat4Live.rawPointer = c.rawPointer;
    g_renderFloat4Live.flags = c.flags;
    g_renderFloat4Live.activeIndex = c.activeIndex;
    g_renderFloat4Live.resolvedPointer = c.resolvedPointer;
    InterlockedExchange(&g_renderFloat4Live.valid, 1);
}

__declspec(naked) void HookRenderFloat4StateLive06()
{
    __asm {
        pushfd
        pushad
        push    eax
        call    CaptureRenderFloat4Live06
        add     esp, 4
        popad
        popfd

        // Reproduce 7 overwritten bytes at 009F6EC0:
        // test byte ptr [eax+60h],1
        // mov  esi,[eax+5Ch]
        test    byte ptr [eax+60h], 1
        mov     esi, [eax+5Ch]

        mov     edx, 009F6EC7h
        jmp     edx
    }
}

bool InstallRenderFloat4StateHookLive06()
{
    constexpr uintptr_t targetAddress = 0x009F6EC0;
    constexpr size_t patchLength = 7;

    const BYTE expected[patchLength] = {
        0xF6, 0x40, 0x60, 0x01,
        0x8B, 0x70, 0x5C
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
    if (!VirtualProtect(
            target,
            patchLength,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
        return false;

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookRenderFloat4StateLive06) -
            (targetAddress + 5));

    for (size_t i = 5; i < patchLength; ++i)
        target[i] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    return true;
}

bool InstallRenderFloat4StateHookForCurrentGame()
{
    if (FM::GetEntryPoint() == 0x40109F &&
        plugin::patch::GetFloat(0xBD832C) == 1.3333334f)
        return InstallRenderFloat4StateHookLive06();

    return false;
}


struct RenderFloat4UseContext {
    volatile LONG valid;
    DWORD threadId;
    uintptr_t frameEbp;
    uintptr_t renderObject;
    DWORD renderFlags;
    uintptr_t modeValue;
    uintptr_t c9413c;
    uintptr_t c94140;
    uintptr_t c94144;
    uintptr_t c94148;
    uintptr_t c9414c;
    uintptr_t c94150;
    uintptr_t c94154;
    uintptr_t c94158;
};

RenderFloat4UseContext g_renderFloat4Use = {};

void __cdecl CaptureRenderFloat4UseLive06(uintptr_t frameEbp)
{
    RenderFloat4UseContext c = {};
    c.threadId = GetCurrentThreadId();
    c.frameEbp = frameEbp;

    __try {
        if (frameEbp) {
            c.renderObject =
                *reinterpret_cast<const uintptr_t*>(frameEbp - 0x0C);
            c.renderFlags =
                *reinterpret_cast<const DWORD*>(frameEbp - 0x08);
            c.modeValue =
                *reinterpret_cast<const uintptr_t*>(frameEbp - 0x1C);
        }

        c.c9413c = *reinterpret_cast<const uintptr_t*>(0x00C9413C);
        c.c94140 = *reinterpret_cast<const uintptr_t*>(0x00C94140);
        c.c94144 = *reinterpret_cast<const uintptr_t*>(0x00C94144);
        c.c94148 = *reinterpret_cast<const uintptr_t*>(0x00C94148);
        c.c9414c = *reinterpret_cast<const uintptr_t*>(0x00C9414C);
        c.c94150 = *reinterpret_cast<const uintptr_t*>(0x00C94150);
        c.c94154 = *reinterpret_cast<const uintptr_t*>(0x00C94154);
        c.c94158 = *reinterpret_cast<const uintptr_t*>(0x00C94158);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    g_renderFloat4Use.threadId = c.threadId;
    g_renderFloat4Use.frameEbp = c.frameEbp;
    g_renderFloat4Use.renderObject = c.renderObject;
    g_renderFloat4Use.renderFlags = c.renderFlags;
    g_renderFloat4Use.modeValue = c.modeValue;
    g_renderFloat4Use.c9413c = c.c9413c;
    g_renderFloat4Use.c94140 = c.c94140;
    g_renderFloat4Use.c94144 = c.c94144;
    g_renderFloat4Use.c94148 = c.c94148;
    g_renderFloat4Use.c9414c = c.c9414c;
    g_renderFloat4Use.c94150 = c.c94150;
    g_renderFloat4Use.c94154 = c.c94154;
    g_renderFloat4Use.c94158 = c.c94158;
    InterlockedExchange(&g_renderFloat4Use.valid, 1);
}

__declspec(naked) void HookRenderFloat4UseLive06()
{
    __asm {
        pushfd
        pushad
        push    ebp
        call    CaptureRenderFloat4UseLive06
        add     esp, 4
        popad
        popfd

        // Reproduce the 6-byte instruction overwritten at 009F72E0:
        // mov edx, dword ptr [00C9414C]
        mov     edx, dword ptr [0C9414Ch]

        mov     eax, 009F72E6h
        jmp     eax
    }
}

bool InstallRenderFloat4UseHookLive06()
{
    constexpr uintptr_t targetAddress = 0x009F72E0;
    constexpr size_t patchLength = 6;

    const BYTE expected[patchLength] = {
        0x8B, 0x15, 0x4C, 0x41, 0xC9, 0x00
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
    if (!VirtualProtect(
            target,
            patchLength,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
        return false;

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookRenderFloat4UseLive06) -
            (targetAddress + 5));
    target[5] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    return true;
}


void PrintRenderFloat4CrashContext(EXCEPTION_POINTERS* ep)
{
#if defined(_M_IX86)
    if (!ep || !ep->ContextRecord)
        return;

    if (ep->ContextRecord->Eip != 0x009FEC0D)
        return;

    std::printf("\n");
    std::printf("==============================================================\n");
    std::printf(" RENDER / MATERIAL FLOAT4 STATE FAILURE\n");
    std::printf("==============================================================\n");
    std::printf("Crash address   : 009FEC0D\n");
    std::printf("Faulting read   : fld dword ptr [ESI]\n");
    std::printf("ESI             : %08lX\n",
        static_cast<unsigned long>(ep->ContextRecord->Esi));

    DWORD cached = 0;
    SafeReadU32(0x00C9414C, cached);
    std::printf("Cached pointer  : %08lX  (dword_C9414C)\n",
        static_cast<unsigned long>(cached));


    if (g_renderFloat4Use.valid &&
        g_renderFloat4Use.threadId == GetCurrentThreadId())
    {
        const RenderFloat4UseContext u = g_renderFloat4Use;
        std::printf("\nLast consumer state:\n");
        std::printf("  Render object  : %08lX\n",
            static_cast<unsigned long>(u.renderObject));
        std::printf("  Render flags   : %08lX\n",
            static_cast<unsigned long>(u.renderFlags));
        std::printf("  Mode value     : %08lX\n",
            static_cast<unsigned long>(u.modeValue));
        std::printf("  C9413C..58     : %08lX %08lX %08lX %08lX\n",
            static_cast<unsigned long>(u.c9413c),
            static_cast<unsigned long>(u.c94140),
            static_cast<unsigned long>(u.c94144),
            static_cast<unsigned long>(u.c94148));
        std::printf("                    %08lX %08lX %08lX %08lX\n",
            static_cast<unsigned long>(u.c9414c),
            static_cast<unsigned long>(u.c94150),
            static_cast<unsigned long>(u.c94154),
            static_cast<unsigned long>(u.c94158));

        if (u.c9414c == 0) {
            std::printf("  Consumer check : C9414C was already NULL before sub_9FEBFE\n");
        }
    }

    if (g_renderFloat4Live.valid &&
        g_renderFloat4Live.threadId == GetCurrentThreadId())
    {
        const RenderFloat4LiveContext c = g_renderFloat4Live;

        std::printf("\nLast material-state update:\n");
        std::printf("  Material state : %08lX\n",
            static_cast<unsigned long>(c.materialState));
        std::printf("  Raw +5C ptr    : %08lX\n",
            static_cast<unsigned long>(c.rawPointer));
        std::printf("  +60 flags      : %08lX\n",
            static_cast<unsigned long>(c.flags));
        std::printf("  Active index   : %u\n",
            static_cast<unsigned>(c.activeIndex));
        std::printf("  Resolved ptr   : %08lX\n",
            static_cast<unsigned long>(c.resolvedPointer));

        if (c.resolvedPointer == 0) {
            std::printf("\nDiagnosis:\n");
            std::printf("  The material/render-state object supplied a NULL float4\n");
            std::printf("  source before sub_9FEBFE attempted to read it.\n");
            std::printf("  This is a state/material initialization failure, not an\n");
            std::printf("  allocator-exhaustion signature by itself.\n");
        }
    }
    else {
        std::printf("\nLive material-state capture unavailable.\n");
    }

    std::printf("==============================================================\n");
#endif
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

    // Known renderer/material float4 null-source signature.
    if (live06 && eip == 0x009FEC0D) {
        PrintRenderFloat4CrashContext(ep);
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
    PrintLive07ResourceOwnerCrash(ep);
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
    g_animBanks = GetPrivateProfileIntA("DEBUG", "ANIMBANK", 0, ".\\main.ini") != 0;
    g_live07CleanupFix = GetPrivateProfileIntA("DEBUG", "LIVE07_CLEANUP_FIX", 1, ".\\main.ini") != 0;

    InitializeCriticalSection(&g_consoleLock);
    g_lockReady = true;
    OpenConsole();
    if (!g_console) {
        InterlockedExchange(&g_debugConsoleInitState, 0);
        return;
    }

    // FILES=0 means no Win32 file IAT hooks at all.
    //
    // NBA Live 07 is sensitive to the EXE-level CreateFileA/W/CloseHandle IAT
    // interception: enabling those hooks can prevent main.ini-backed launcher
    // settings from taking effect. NBA Live 08 uses the same newer filesystem
    // family, so keep the low-level Win32 hooks disabled there as well.
    //
    // 07/08 still get the higher-level FileSystem::OpenResolved asset tracking
    // through ASSET_FILES, plus the generic crash handler.
    const uintptr_t ep = FM::GetEntryPoint();
    const bool live07 =
        ep == 0x40109F &&
        plugin::patch::GetFloat(0xBBBC3C) == 1.3333334f;
    const bool live08 =
        ep == 0x40109F &&
        plugin::patch::GetFloat(0xC3DF84) == 1.3333334f;
    const bool allowWin32FileHooks = !(live07 || live08);

    if (g_files && allowWin32FileHooks) {
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

        if (g_files && !allowWin32FileHooks) {
            std::printf(
                "Win32 file hooks: disabled for %s (use ASSET_FILES for logical resource tracking)\n",
                GameName());
        }
        else {
            std::printf("Win32 file hooks: disabled (FILES=0)\n");
        }
    }

    if (FM::GetEntryPoint() == 0x40109F &&
        plugin::patch::GetFloat(0xBBBC3C) == 1.3333334f)
    {
        const bool owner07 = InstallLive07ResourceOwnerHook();
        PrintPrefix(
            "DEBUG",
            owner07 ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
                    : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
        std::printf(
            "NBA Live 07 resource-owner hook: %s\n",
            owner07 ? "OK" : "MISS");

        const bool cleanup07 = InstallLive07CleanupEntryHook();
        PrintPrefix(
            "DEBUG",
            cleanup07 ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
                      : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
        std::printf(
            "NBA Live 07 cleanup-array hook: %s\n",
            cleanup07 ? "OK" : "MISS");

        if (g_live07CleanupFix) {
            const bool cleanupGuard07 = InstallLive07CleanupGuard();
            PrintPrefix(
                "DEBUG",
                cleanupGuard07 ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
                               : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
            std::printf(
                "NBA Live 07 stale-cleanup guard: %s\n",
                cleanupGuard07 ? "OK" : "MISS");
        }
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

    if (g_animBanks) {
        const bool animBank = InstallAnimBankPairHookForCurrentGame();
        PrintPrefix(
            "DEBUG",
            animBank ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
                     : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
        std::printf(
            "%s animation-bank pair hook: %s\n",
            GameName(),
            animBank ? "OK" : "MISS");
    }

    if (g_assetFiles) {
        if (FM::GetEntryPoint() == 0x40109F &&
            plugin::patch::GetFloat(0xBD832C) == 1.3333334f)
        {
            PrintPrefix("DEBUG",
                FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
            std::printf("NBA Live 06 GeomFile processing: observed via crash stack/live EAGL capture\n");

            const bool renderFloat4 = InstallRenderFloat4StateHookForCurrentGame();
            PrintPrefix("DEBUG",
                renderFloat4 ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
                             : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
            std::printf("NBA Live 06 render float4 state hook: %s\n",
                renderFloat4 ? "OK" : "MISS");

            const bool renderFloat4Use = InstallRenderFloat4UseHookLive06();
            PrintPrefix("DEBUG",
                renderFloat4Use ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
                                : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
            std::printf("NBA Live 06 render float4 consumer hook: %s\n",
                renderFloat4Use ? "OK" : "MISS");

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

