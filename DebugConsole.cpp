#include "plugin-std.h"
#include "Live07CleanupFix.h"
#include "LoadingOptimization.h"
#include "DebugConsole.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <io.h>
#include <fcntl.h>
#include <intrin.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <Windows.h>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include "GameStageInternal.h"
#include "PracticeSetupInternal.h"
#include <climits>
#include <cstdarg>
#include "AutoStartInternal.h"

#pragma intrinsic(_ReturnAddress)

// NBA Live 06 loading capture (optional, console must be disabled):
//   [DEBUG] CONSOLE=0
//   [LOADING_PROFILE] ENABLED=1, AUTO_START=0, MAX_EVENTS=65536
// AUTO_CAPTURE=1 (default) captures 5on5.scn entry through pregame intro setup.
// MAX_SECONDS=120 saves a labeled partial capture if the intro is not reached.
// AUTO_CAPTURE=0 restores optional F9 start/stop (AUTO_START also available).
// Use separate INI lines for each key. Reports go beside the EXE in logs/.
// loading_profile_06_status.log reports READY/CAPTURING/WRITING and hook coverage.
// Each completed capture produces loading_06_*.csv and loading_06_*.log.
// The CSV records native asset entry order plus intercepted file API timings;
// it does not time native asset completion, parsing, decompression or GPU work.
// AUTO_START=1 captures from profiler startup until F9. ENABLED=0 is the default.
// Optional 2005/06/07/08 optimization: [LOADING] FAST_FILE_WAIT=1. Default 0.
// Changes only the verified filesystem completion-poll delay call. Keeps its
// callback pump and completion checks; uses a separate auto-reset event signaled
// by native file-worker completion, with an alertable 1-ms timeout. If the
// completion hook cannot install, falls back to 63 yields then a 1-ms sleep.
// No global Sleep/FPS/timer-resolution changes; native events stay untouched.

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
const char kFilteredAssetSentinel[] = "__nbl_filtered_asset__|";

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
    if (path && std::strstr(path, kFilteredAssetSentinel)) return;
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


// Optional, bounded, quiet loading capture. This block lives in DebugConsole's
// anonymous namespace and needs no additional project files or dependencies.
namespace loadingprofile06 {
enum Operation { ASSET, OPEN_A, OPEN_W, READ, FILE_WAIT, OP_COUNT };
struct Event {
    LONGLONG begin, end;
    DWORD generation, thread, operation, error, flags, requested, completed;
    DWORD bytesKnown, asynchronous, kind;
    uintptr_t caller, context, handle;
    char path[260];
};
struct FileHandle { HANDLE handle; char path[260]; };
typedef BOOL (WINAPI *ReadFileFn)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
CreateFileA_t originalOpenA = nullptr;
CreateFileW_t originalOpenW = nullptr;
CloseHandle_t originalClose = nullptr;
ReadFileFn originalRead = nullptr;
CRITICAL_SECTION lock;
Event* events = nullptr;
FileHandle handles[2048] = {};
size_t count = 0, capacity = 65536;
DWORD generation = 0, dropped = 0, handleDrops = 0;
volatile LONG capturing = 0;
bool ready = false, assetHook = false, openAHook = false;
bool openWHook = false, readHook = false, closeHook = false;
bool autoCapture = true, introHook = false;
DWORD maxCaptureSeconds = 120;
volatile LONG pendingSave = 0, pendingStatus = 0;
const char* startReason = "manual";
const char* stopReason = "manual";
struct WaitSummary {
    unsigned long long calls, zeroMs, oneMs, otherMs;
    unsigned long long notified, timedOut, alertable, waitFailed;
    LONGLONG ticks, maximum;
};
WaitSummary waitTotal = {}, waitSeconds[601] = {};
using WaitSample = LiveLoading::WaitSample;

LARGE_INTEGER frequency = {}, captureBegin = {}, captureEnd = {};
char logDirectory[MAX_PATH] = {};
char captureStem[MAX_PATH] = {};
__declspec(thread) bool internal = false;

void CopyPath(char* out, const char* path) {
    size_t i = 0;
    __try {
        if (path) for (; i < 259 && path[i]; ++i) out[i] = path[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const char invalid[] = "<invalid-path>";
        std::memcpy(out, invalid, sizeof(invalid));
        return;
    }
    out[i] = 0;
}

// Open-addressed table: null is empty, INVALID_HANDLE_VALUE is a tombstone.
// Tracking continues between captures so archives opened earlier retain names.
size_t HandleBucket(HANDLE h) {
    return (reinterpret_cast<uintptr_t>(h) >> 2) & 2047;
}
void Remember(HANDLE h, const char* path) {
    if (!h || h == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&lock);
    size_t firstFree = 2048;
    for (size_t n = 0; n < 2048; ++n) {
        const size_t i = (HandleBucket(h) + n) & 2047;
        if (handles[i].handle == h) { firstFree = i; break; }
        if (handles[i].handle == INVALID_HANDLE_VALUE && firstFree == 2048)
            firstFree = i;
        if (!handles[i].handle) {
            if (firstFree == 2048) firstFree = i;
            break;
        }
    }
    if (firstFree < 2048) {
        handles[firstFree].handle = h;
        CopyPath(handles[firstFree].path, path);
    } else ++handleDrops;
    LeaveCriticalSection(&lock);
}
void Lookup(HANDLE h, char* path, bool erase) {
    CopyPath(path, "<untracked-handle>");
    EnterCriticalSection(&lock);
    for (size_t n = 0; n < 2048; ++n) {
        const size_t i = (HandleBucket(h) + n) & 2047;
        if (!handles[i].handle) break;
        if (handles[i].handle == h) {
            CopyPath(path, handles[i].path);
            if (erase) handles[i].handle = INVALID_HANDLE_VALUE;
            break;
        }
    }
    LeaveCriticalSection(&lock);
}

bool Begin(Event& e, Operation op, uintptr_t caller) {
    if (!capturing || internal) return false;
    EnterCriticalSection(&lock);
    const bool active = capturing != 0;
    if (active) e.generation = generation;
    LeaveCriticalSection(&lock);
    if (!active) return false;
    e.operation = op;
    e.caller = caller;
    e.thread = GetCurrentThreadId();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    e.begin = now.QuadPart;
    return true;
}
void Append(Event& e) {
    EnterCriticalSection(&lock);
    // Calls crossing a stop/restart boundary belong to neither capture.
    if (capturing && e.generation == generation) {
        if (count < capacity) events[count++] = e;
        else ++dropped;
    }
    LeaveCriticalSection(&lock);
}
void End(Event& e) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    e.end = now.QuadPart;
}

WaitSample BeginWait() {
    WaitSample sample = {};
    if (!ready || !capturing || internal) return sample;
    EnterCriticalSection(&lock);
    if (capturing) {
        sample.active = true; sample.generation = generation;
        LARGE_INTEGER now; QueryPerformanceCounter(&now); sample.begin = now.QuadPart;
    }
    LeaveCriticalSection(&lock);
    return sample;
}
void AddWait(WaitSummary& sum, DWORD delay, LONGLONG ticks, bool eventWait, DWORD result) {
    ++sum.calls;
    if (!delay) ++sum.zeroMs;
    else if (delay == 1) ++sum.oneMs;
    else ++sum.otherMs;
    if (eventWait) {
        if (result == WAIT_OBJECT_0) ++sum.notified;
        else if (result == WAIT_TIMEOUT) ++sum.timedOut;
        else if (result == WAIT_IO_COMPLETION) ++sum.alertable;
        else ++sum.waitFailed;
    }
    sum.ticks += ticks;
    if (ticks > sum.maximum) sum.maximum = ticks;
}
void FinishWait(const WaitSample& sample, DWORD actual, bool eventWait = false, DWORD result = 0) {
    if (!sample.active) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    EnterCriticalSection(&lock);
    if (capturing && generation == sample.generation) {
        const LONGLONG ticks = now.QuadPart - sample.begin;
        size_t second = static_cast<size_t>((sample.begin - captureBegin.QuadPart) / frequency.QuadPart);
        if (second > 600) second = 600;
        AddWait(waitTotal, actual, ticks, eventWait, result);
        AddWait(waitSeconds[second], actual, ticks, eventWait, result);
    }
    LeaveCriticalSection(&lock);
}
bool StartCore(const char* reason);
void StopCore(const char* reason);

void __cdecl OnPregameIntro() {
    if (ready && autoCapture && capturing) StopCore("pregame-intro-entry:00461B69");
}
#if defined(_M_IX86)
uintptr_t introDestination = 0x0045D240;
__declspec(naked) void IntroBoundary() {
    __asm {
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, -16
        fxsave [esp]
        call GetLastError
        mov [esp + 512], eax
        call OnPregameIntro
        push dword ptr [esp + 512]
        call SetLastError
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        // Tail-jump to the original entry (including its existing CustomPaths
        // hook). The original return address and arguments remain untouched.
        jmp dword ptr [introDestination]
    }
}
#endif
bool InstallIntroBoundary() {
#if defined(_M_IX86)
    const BYTE expected[] = { 0xE8, 0xD2, 0xB6, 0xFF, 0xFF };
    BYTE* call = reinterpret_cast<BYTE*>(0x00461B69);
    __try {
        if (std::memcmp(call, expected, sizeof(expected))) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    DWORD protect = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &protect)) return false;
    *reinterpret_cast<int32_t*>(call + 1) = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&IntroBoundary) - 0x00461B6E);
    DWORD ignored = 0;
    VirtualProtect(call, 5, protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    introHook = true;
    return true;
#else
    return false;
#endif
}

void RecordAsset(const uintptr_t* stack) {
    if (!ready || internal || !stack) return;
    const DWORD previousError = GetLastError();
    Event e = {};
    __try {
        // Trigger before recording the scene request, so it is the first marker.
        // No disk output or heap allocation runs on this loader entry path.
        if (autoCapture && !capturing && !pendingSave &&
            !_stricmp(BaseNameOf(reinterpret_cast<const char*>(stack[2])), "5on5.scn"))
            StartCore("asset-entry:5on5.scn");
        if (Begin(e, ASSET, stack[0])) {
            e.flags = static_cast<DWORD>(stack[3]);
            e.context = stack[4];
            e.kind = ClassifyAssetRequest(reinterpret_cast<void*>(stack[0]));
            CopyPath(e.path, reinterpret_cast<const char*>(stack[2]));
            e.end = e.begin; // ENTRY ONLY: never label this as a load duration.
            Append(e);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    SetLastError(previousError);
}

HANDLE WINAPI OpenA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                    DWORD creation, DWORD attributes, HANDLE templateFile) {
    const DWORD incomingError = GetLastError();
    Event e = {};
    const bool active = Begin(e, OPEN_A, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (active) { CopyPath(e.path, path); e.flags = access; }
    // Time only the original API, excluding name copying and bookkeeping.
    if (active) { LARGE_INTEGER t; QueryPerformanceCounter(&t); e.begin = t.QuadPart; }
    SetLastError(incomingError);
    HANDLE h = originalOpenA(path, access, share, sa, creation, attributes, templateFile);
    const DWORD error = GetLastError();
    if (active) End(e);
    if (!internal && closeHook) Remember(h, path);
    if (active) {
        e.handle = reinterpret_cast<uintptr_t>(h);
        e.error = h == INVALID_HANDLE_VALUE ? error : 0;
        Append(e);
    }
    SetLastError(error);
    return h;
}
HANDLE WINAPI OpenW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                    DWORD creation, DWORD attributes, HANDLE templateFile) {
    const DWORD incomingError = GetLastError();
    Event e = {};
    const bool active = Begin(e, OPEN_W, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    char text[260] = {};
    if (!internal) {
        __try {
            if (path && !WideCharToMultiByte(CP_UTF8, 0, path, -1, text, 260, nullptr, nullptr))
                CopyPath(text, "<wide-path-too-long>");
        } __except (EXCEPTION_EXECUTE_HANDLER) { CopyPath(text, "<invalid-path>"); }
    }
    if (active) {
        CopyPath(e.path, text); e.flags = access;
        LARGE_INTEGER t; QueryPerformanceCounter(&t); e.begin = t.QuadPart;
    }
    SetLastError(incomingError);
    HANDLE h = originalOpenW(path, access, share, sa, creation, attributes, templateFile);
    const DWORD error = GetLastError();
    if (active) End(e);
    if (!internal && closeHook) Remember(h, text);
    if (active) {
        e.handle = reinterpret_cast<uintptr_t>(h);
        e.error = h == INVALID_HANDLE_VALUE ? error : 0;
        Append(e);
    }
    SetLastError(error);
    return h;
}
BOOL WINAPI Read(HANDLE h, LPVOID buffer, DWORD bytes, LPDWORD completed, LPOVERLAPPED overlap) {
    const DWORD incomingError = GetLastError();
    Event e = {};
    const bool active = Begin(e, READ, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (active) {
        Lookup(h, e.path, false);
        e.handle = reinterpret_cast<uintptr_t>(h);
        e.requested = bytes; e.asynchronous = overlap ? 1 : 0;
        LARGE_INTEGER t; QueryPerformanceCounter(&t); e.begin = t.QuadPart;
    }
    SetLastError(incomingError);
    BOOL result = originalRead(h, buffer, bytes, completed, overlap);
    const DWORD error = GetLastError();
    if (active) {
        End(e);
        e.error = result ? 0 : error;
        // Async calls measure submission only, never completion or throughput.
        if (result && !overlap && completed) {
            __try { e.completed = *completed; e.bytesKnown = 1; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        Append(e);
    }
    SetLastError(error);
    return result;
}
BOOL WINAPI Close(HANDLE h) {
    const DWORD incomingError = GetLastError();
    // Remove before closing to avoid erasing a newly reused handle afterwards.
    if (!internal) { char path[260]; Lookup(h, path, true); }
    SetLastError(incomingError);
    BOOL result = originalClose(h);
    return result; // No API calls after originalClose: preserve its LastError.
}

const char* OperationName(DWORD op) {
    const char* names[] = { "asset-entry", "CreateFileA", "CreateFileW", "ReadFile", "file-wait" };
    return op < OP_COUNT ? names[op] : "unknown";
}
const char* Category(const char* path) {
    const char* dot = std::strrchr(path, '.');
    if (!dot) return "other";
    if (!_stricmp(dot, ".viv") || !_stricmp(dot, ".big")) return "archive";
    if (!_stricmp(dot, ".ebo")) return "model";
    if (!_stricmp(dot, ".fsh")) return "texture";
    if (!_stricmp(dot, ".ubb") || !_stricmp(dot, ".ubi") || !_stricmp(dot, ".o")) return "script-or-bank";
    if (!_stricmp(dot, ".mgd") || !_stricmp(dot, ".scn")) return "scene-or-camera";
    if (!_stricmp(dot, ".apt") || !_stricmp(dot, ".const")) return "frontend";
    if (!_stricmp(dot, ".dbf") || !_stricmp(dot, ".dat")) return "data";
    return "other";
}
void CsvText(FILE* out, const char* value) {
    std::fputc('"', out);
    for (const char* p = value; *p; ++p) {
        if (*p == '"') std::fputc('"', out);
        std::fputc(*p, out);
    }
    std::fputc('"', out);
}
std::string Key(const char* text) {
    std::string key(text);
    for (size_t i = 0; i < key.size(); ++i) {
        if (key[i] >= 'A' && key[i] <= 'Z') key[i] += 'a' - 'A';
        if (key[i] == '/') key[i] = '\\';
    }
    return key;
}
struct Aggregate {
    unsigned long long calls = 0, failed = 0, bytes = 0, asyncCalls = 0;
    double ms = 0;
};
double Milliseconds(LONGLONG ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency.QuadPart);
}
void Status(const char* message) {
    char path[MAX_PATH];
    sprintf_s(path, "%s\\loading_profile_06_status.log", logDirectory);
    FILE* out = nullptr;
    if (!fopen_s(&out, path, "w") && out) {
        std::fprintf(out, "%s\nasset-entry=%d CreateFileA=%d CreateFileW=%d ReadFile=%d CloseHandle=%d\n",
            message, assetHook, openAHook, openWHook, readHook, closeHook);
        std::fprintf(out, "file-wait=%d FAST_FILE_WAIT=%d\n", LiveLoading::IsInstalled(), LiveLoading::IsFastWaitEnabled());
        std::fprintf(out, "file-wait-strategy=%s completion-signal-hook=%d\n",
            !LiveLoading::IsFastWaitEnabled() ? "native" : LiveLoading::HasCompletionSignal() ? "completion-event" : "bounded-yields", LiveLoading::HasCompletionSignal());
        std::fprintf(out, "AUTO_CAPTURE=%d intro-boundary-hook=%d MAX_SECONDS=%lu\n",
            autoCapture, introHook, static_cast<unsigned long>(maxCaptureSeconds));
        std::fclose(out);
    }
}
bool StartCore(const char* reason) {
    SYSTEMTIME t; GetLocalTime(&t);
    EnterCriticalSection(&lock);
    if (capturing || pendingSave) { LeaveCriticalSection(&lock); return false; }
    ++generation; count = 0; dropped = 0;
    waitTotal = {};
    std::memset(waitSeconds, 0, sizeof(waitSeconds));
    startReason = reason; stopReason = "not-stopped";
    sprintf_s(captureStem, "%s\\loading_06_%04u%02u%02u_%02u%02u%02u_%lu_%lu",
        logDirectory, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned long>(generation));
    QueryPerformanceCounter(&captureBegin);
    capturing = 1;
    pendingStatus = 1;
    LeaveCriticalSection(&lock);
    return true;
}
void Start() { StartCore("manual-or-startup"); }
void StopCore(const char* reason) {
    EnterCriticalSection(&lock);
    if (capturing) {
        capturing = 0; QueryPerformanceCounter(&captureEnd);
        stopReason = reason; pendingSave = 1;
    }
    LeaveCriticalSection(&lock);
}
bool WriteCapture() {
    // Only the worker calls this; capture is stopped before sorting/writing.
    std::stable_sort(events, events + count, [](const Event& a, const Event& b) {
        return a.begin < b.begin;
    });
    char path[MAX_PATH];
    sprintf_s(path, "%s.csv", captureStem);
    FILE* csv = nullptr;
    if (fopen_s(&csv, path, "w") || !csv) return false;
    std::fprintf(csv, "sequence,start_ms,duration_ms,thread,operation,category,kind,caller,context,handle,error,flags,requested_bytes,completed_bytes,bytes_known,async_submission,requested_wait_ms,actual_wait_ms,path\n");
    Aggregate totals[OP_COUNT];
    std::map<std::string, Aggregate> files;
    std::map<std::string, unsigned long long> requests;
    for (size_t i = 0; i < count; ++i) {
        const Event& e = events[i];
        const double duration = Milliseconds(e.end - e.begin);
        const char* kind = e.operation == ASSET ?
            (e.kind == ASSET_REQUEST_PROBE ? "probe" : e.kind == ASSET_REQUEST_ACQUIRE ? "acquire" : "other") : "";
        std::fprintf(csv, "%lu,%.6f,", static_cast<unsigned long>(i + 1), Milliseconds(e.begin - captureBegin.QuadPart));
        if (e.operation != ASSET) std::fprintf(csv, "%.6f", duration);
        std::fprintf(csv, ",%lu,%s,%s,%s,%08lX,%08lX,%08lX,%lu,%08lX,%lu,%lu,%lu,%lu,",
            static_cast<unsigned long>(e.thread), OperationName(e.operation), Category(e.path), kind,
            static_cast<unsigned long>(e.caller), static_cast<unsigned long>(e.context),
            static_cast<unsigned long>(e.handle), static_cast<unsigned long>(e.error),
            static_cast<unsigned long>(e.flags), static_cast<unsigned long>(e.operation == FILE_WAIT ? 0 : e.requested),
            static_cast<unsigned long>(e.completed), static_cast<unsigned long>(e.bytesKnown),
            static_cast<unsigned long>(e.asynchronous));
        std::fprintf(csv, "%lu,%lu,", static_cast<unsigned long>(e.operation == FILE_WAIT ? e.flags : 0),
            static_cast<unsigned long>(e.operation == FILE_WAIT ? e.requested : 0));
        CsvText(csv, e.path); std::fputc('\n', csv);
        Aggregate& total = totals[e.operation];
        ++total.calls;
        if (e.error && e.error != ERROR_IO_PENDING) ++total.failed;
        total.ms += duration;
        if (e.bytesKnown) total.bytes += e.completed;
        total.asyncCalls += e.asynchronous;
        if (e.operation == ASSET) ++requests[Key(e.path) + " [" + kind + "]"];
        else if (e.operation != FILE_WAIT) {
            Aggregate& file = files[Key(e.path)];
            ++file.calls; file.ms += duration;
            if (e.bytesKnown) file.bytes += e.completed;
            if (e.error && e.error != ERROR_IO_PENDING) ++file.failed;
            file.asyncCalls += e.asynchronous;
        }
    }
    const bool csvError = std::ferror(csv) != 0;
    const int csvCloseError = std::fclose(csv);
    sprintf_s(path, "%s.log", captureStem);
    FILE* out = nullptr;
    if (fopen_s(&out, path, "w") || !out) return false;
    std::fprintf(out, "NBA Live 06 quiet loading capture\nWall interval: %.3f ms\nEvents: %lu / %lu; dropped=%lu\n",
        Milliseconds(captureEnd.QuadPart - captureBegin.QuadPart), static_cast<unsigned long>(count),
        static_cast<unsigned long>(capacity), static_cast<unsigned long>(dropped));
    std::fprintf(out, "Coverage: asset-entry=%d CreateFileA=%d CreateFileW=%d ReadFile=%d CloseHandle=%d\n",
        assetHook, openAHook, openWHook, readHook, closeHook);
    std::fprintf(out, "File completion delay: hook=%d FAST_FILE_WAIT=%d\n",
        LiveLoading::IsInstalled(), LiveLoading::IsFastWaitEnabled());
    std::fprintf(out, "File wait strategy: %s completion-signal-hook=%d\n",
        !LiveLoading::IsFastWaitEnabled() ? "native" : LiveLoading::HasCompletionSignal() ? "completion-event" : "bounded-yields", LiveLoading::HasCompletionSignal());
    std::fprintf(out, "Completion-event waits wake on worker notification or alertable callback, with a 1-ms timeout.\n"
        "Bounded-yields fallback uses 63 zero-ms yields, then one 1-ms wait. Native events are unchanged.\n");
    std::fprintf(out, "Capture boundaries: start=%s stop=%s intro-hook=%d\n", startReason, stopReason, introHook);
    std::fprintf(out, "Automatic window measures scene request through pregame intro setup, not the full intro video.\n");
    EnterCriticalSection(&lock); const DWORD lostHandles = handleDrops; LeaveCriticalSection(&lock);
    std::fprintf(out, "Handle-map insertions lost since initialization: %lu\n", static_cast<unsigned long>(lostHandles));
    std::fprintf(out, "\nScope: main EXE imports only. DLL, mapped-file, unhooked I/O and parsing/GPU time are unmeasured.\n"
        "Native assets are entry markers, without success/failure or duration. Categories are filename hints, not engine phases.\n"
        "API time excludes profiler bookkeeping; it is not disk latency. Concurrent API times can overlap.\n"
        "Async ReadFile durations measure submission only; completed bytes are unknown.\n"
        "Calls crossing capture boundaries are omitted. Paths truncate at 259 bytes.\n"
        "Repeated requests do not prove redundant disk reads. First/repeat loads reflect both OS and game caching.\n\n");
    totals[FILE_WAIT].calls = waitTotal.calls;
    totals[FILE_WAIT].ms = Milliseconds(waitTotal.ticks);
    std::fprintf(out, "\nFile waits are aggregated for the entire capture, independently of CSV event capacity.\n"
        "No per-yield CSV rows are stored. Other totals below cover stored CSV events only.\n");
    for (DWORD op = 0; op < OP_COUNT; ++op) {
        const Aggregate& a = totals[op];
        std::fprintf(out, "%s calls=%llu failed=%llu summed_api_ms=%.3f completed_sync_bytes=%llu async=%llu\n",
            OperationName(op), a.calls, a.failed, a.ms, a.bytes, a.asyncCalls);
    }
    std::fprintf(out, "file-wait detail: zero_ms=%llu one_ms=%llu other_ms=%llu max_ms=%.6f\n",
        waitTotal.zeroMs, waitTotal.oneMs, waitTotal.otherMs, Milliseconds(waitTotal.maximum));
    std::fprintf(out, "file-wait events: notified=%llu timeout=%llu alertable=%llu failed=%llu\n",
        waitTotal.notified, waitTotal.timedOut, waitTotal.alertable, waitTotal.waitFailed);
    std::fprintf(out, "\nFile waits by capture second (bucket 600 includes later calls):\n");
    for (size_t i = 0; i < 601; ++i) {
        const WaitSummary& w = waitSeconds[i];
        if (w.calls) std::fprintf(out, "second=%lu calls=%llu zero_ms=%llu one_ms=%llu other_ms=%llu total_ms=%.6f max_ms=%.6f notified=%llu timeout=%llu alertable=%llu failed=%llu\n",
            static_cast<unsigned long>(i), w.calls, w.zeroMs, w.oneMs, w.otherMs,
            Milliseconds(w.ticks), Milliseconds(w.maximum), w.notified, w.timedOut, w.alertable, w.waitFailed);
    }
    typedef std::pair<std::string, Aggregate> RankedFile;
    std::vector<RankedFile> ranked(files.begin(), files.end());
    std::sort(ranked.begin(), ranked.end(), [](const RankedFile& a, const RankedFile& b) {
        return a.second.ms > b.second.ms;
    });
    std::fprintf(out, "\nTop files by summed intercepted API time (up to 40):\n");
    for (size_t i = 0; i < ranked.size() && i < 40; ++i) {
        const Aggregate& a = ranked[i].second;
        std::fprintf(out, "%.3f ms calls=%llu failed=%llu bytes=%llu async=%llu %s\n",
            a.ms, a.calls, a.failed, a.bytes, a.asyncCalls, ranked[i].first.c_str());
    }
    typedef std::pair<std::string, unsigned long long> RankedRequest;
    std::vector<RankedRequest> repeated(requests.begin(), requests.end());
    std::sort(repeated.begin(), repeated.end(), [](const RankedRequest& a, const RankedRequest& b) {
        return a.second > b.second;
    });
    std::fprintf(out, "\nRepeated native request names by kind (up to 40):\n");
    for (size_t i = 0; i < repeated.size() && i < 40 && repeated[i].second > 1; ++i)
        std::fprintf(out, "%llu %s\n", repeated[i].second, repeated[i].first.c_str());
    const bool logError = std::ferror(out) != 0;
    const int logCloseError = std::fclose(out);
    return !csvError && !csvCloseError && !logError && !logCloseError;
}
DWORD WINAPI Worker(LPVOID autoStart) {
    internal = true;
    if (!autoCapture && autoStart) Start();
    else Status(autoCapture ? "ARMED. Captures 5on5.scn through pregame intro setup automatically." :
        "READY. F9 in the game starts capture; F9 again stops and saves it.");
    bool held = false;
    for (;;) {
        Sleep(50);
        if (pendingStatus) {
            pendingStatus = 0;
            Status("CAPTURING. Automatic mode stops at pregame intro setup; F9 can also stop manually.");
        }
        EnterCriticalSection(&lock);
        if (capturing) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (now.QuadPart - captureBegin.QuadPart >= frequency.QuadPart * maxCaptureSeconds)
                StopCore("timeout-partial-intro-boundary-not-reached");
        }
        LeaveCriticalSection(&lock);
        if (pendingSave) {
            Status("WRITING. Saving capture, then rearming.");
            bool saved = false;
            try { saved = WriteCapture(); } catch (...) {}
            EnterCriticalSection(&lock); pendingSave = 0; LeaveCriticalSection(&lock);
            Status(saved ? (autoCapture ? "ARMED. Capture saved; ready for the next 5on5.scn request." :
                "READY. Capture saved. F9 starts another capture.") :
                "SAVE FAILED. Check logs directory permissions/free space.");
        }
        DWORD process = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &process);
        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !held && process == GetCurrentProcessId()) {
            if (capturing) {
                StopCore("manual-F9");
            } else Start();
        }
        held = down;
    }
}
// Defined after the native asset-entry installer below.
bool Initialize();
} // namespace loadingprofile06

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


bool IsReadablePointerRange(uintptr_t address, size_t size);

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

bool IsReadablePointerRange(uintptr_t, size_t) { return false; }

bool InstallLive07ResourceOwnerHook()
{
    return false;
}

bool InstallLive07CleanupEntryHook()
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
    if (!entryStack)
        return;

    loadingprofile06::RecordAsset(entryStack);
    if (!g_enabled || !g_assetFiles || !g_lockReady)
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

    // CustomPaths uses this Win32-invalid sentinel to enter the engine's
    // native failure path without touching the filesystem. It is internal
    // filtering activity, not a real logical asset request.
    if (fileName && std::strcmp(fileName, kFilteredAssetSentinel) == 0)
        return;

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

        // Preserve floating point/SSE state as well as GPRs and flags.
        // EBX is already saved by pushad; use it to restore the exact stack.
        mov     ebx, esp
        sub     esp, 528
        and     esp, -16
        fxsave  [esp]
        call    GetLastError
        mov     [esp + 512], eax
        lea     edx, [ebx + 36]

        // EAX saved by pushad is at [esp + 28].
        // For Live 2005 this is the original register context.
        mov     eax, [ebx + 28]

        push    edx
        push    eax
        call    LogFileSystemOpenResolved
        add     esp, 8

        push    dword ptr [esp + 512]
        call    SetLastError
        fxrstor [esp]
        mov     esp, ebx

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
    if (g_FileSystemOpenResolvedInstalled)
        return true;
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

    bool chained = false;
    __try {
        // CustomPaths can initialize first. Its entry gate leaves four NOPs
        // after a relative JMP; chain without replacing its trampoline.
        chained = target[0] == 0xE9 && target[5] == 0x90 &&
            target[6] == 0x90 && target[7] == 0x90 && target[8] == 0x90;
        if (!chained && std::memcmp(target, expected, kPatchLength) != 0) {
            if (g_enabled) {
                PrintPrefix(
                    "DEBUG",
                    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
                std::printf(
                    "FileSystem::OpenResolved hook skipped: unexpected bytes at %08lX\n",
                    static_cast<unsigned long>(targetAddress));
            }
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    BYTE* trampoline = chained ? nullptr : reinterpret_cast<BYTE*>(
        VirtualAlloc(
            nullptr,
            32,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));

    if (!chained && !trampoline)
        return false;

    if (!chained) {
        std::memcpy(trampoline, target, kPatchLength);

        trampoline[kPatchLength] = 0xE9;
        *reinterpret_cast<int32_t*>(trampoline + kPatchLength + 1) =
            static_cast<int32_t>(
                (targetAddress + kPatchLength) -
                reinterpret_cast<uintptr_t>(trampoline + kPatchLength + 5));
    }

    g_FileSystemOpenResolvedTrampoline =
        chained ? targetAddress + 5 + *reinterpret_cast<int32_t*>(target + 1) :
            reinterpret_cast<uintptr_t>(trampoline);
    g_assetOpenLayout = layout;

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            target,
            kPatchLength,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        if (trampoline) VirtualFree(trampoline, 0, MEM_RELEASE);
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
// NBA Live 2005/06/07/08 animation-bank pair debugger
// -------------------------------------------------------------------------
//
// AnimationBankSystem_LoadPair:
 //   05: 00637550
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
// identical in the Live 2005/07/08 pair loaders:
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

__declspec(naked) void HookAnimBankPair2005()
{
    __asm {
        // Live 2005 entry stack:
        //   ESP+04 = anim bank index
        //   ESP+08 = variant
        //   ESP+0C = ABK stem / formatted bank string
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

        // Reproduce the exact five bytes overwritten at 00637550:
        //   53                push ebx
        //   8B 5C 24 08      mov ebx,[esp+08h]
        // After push ebx, original ESP+08 (variant) is at ESP+0C.
        push    ebx
        mov     ebx, [esp+0Ch]

        jmp     dword ptr [g_AnimBankPairContinue]
    }
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

bool InstallAnimBankPairHookAt2005(uintptr_t targetAddress)
{
    constexpr size_t patchLength = 5;

    // NBA Live 2005 @ 00637550:
    //   53                push ebx
    //   8B 5C 24 08      mov ebx,[esp+08h]
    const BYTE expected[patchLength] = {
        0x53,
        0x8B, 0x5C, 0x24, 0x08
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
            reinterpret_cast<uintptr_t>(&HookAnimBankPair2005) -
            (targetAddress + 5));

    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);

    return true;
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
        // NBA Live 2005
        // Pair loader: 00637550
        return InstallAnimBankPairHookAt2005(0x00637550);
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


struct ModuleAddressInfo {
    bool valid;
    uintptr_t base;
    uintptr_t rva;
    char modulePath[MAX_PATH];
    char moduleName[MAX_PATH];
};

ModuleAddressInfo ResolveModuleAddress(uintptr_t address)
{
    ModuleAddressInfo info = {};

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address),
            &module) ||
        !module)
    {
        return info;
    }

    const DWORD chars = GetModuleFileNameA(
        module,
        info.modulePath,
        static_cast<DWORD>(sizeof(info.modulePath)));

    if (!chars || chars >= sizeof(info.modulePath))
        return info;

    info.modulePath[sizeof(info.modulePath) - 1] = '\0';

    const char* baseName = BaseNameOf(info.modulePath);
    std::strncpy(
        info.moduleName,
        (baseName && *baseName) ? baseName : "<unknown>",
        sizeof(info.moduleName) - 1);
    info.moduleName[sizeof(info.moduleName) - 1] = '\0';

    info.base = reinterpret_cast<uintptr_t>(module);
    info.rva = address >= info.base ? address - info.base : 0;
    info.valid = true;
    return info;
}

void PrintStack32(CONTEXT* c) {
#if defined(_M_IX86)
    if (!c)
        return;

    std::printf("\nCall stack (EBP chain):\n");

    {
        const ModuleAddressInfo m = ResolveModuleAddress(c->Eip);
        if (m.valid) {
            std::printf(
                "  #0  %08lX  %-24s +%08lX  <faulting instruction>\n",
                c->Eip,
                m.moduleName,
                static_cast<unsigned long>(m.rva));
        }
        else {
            std::printf(
                "  #0  %08lX  %-24s  <faulting instruction>\n",
                c->Eip,
                "<module unresolved>");
        }
    }

    DWORD ebp = c->Ebp;
    for (int i = 1; i < 24 && ebp; ++i) {
        __try {
            DWORD* frame = reinterpret_cast<DWORD*>(ebp);
            DWORD next = frame[0];
            DWORD ret = frame[1];

            if (!ret || next <= ebp || next - ebp > 0x100000)
                break;

            const ModuleAddressInfo m = ResolveModuleAddress(ret);
            if (m.valid) {
                std::printf(
                    "  #%d  %08lX  %-24s +%08lX\n",
                    i,
                    ret,
                    m.moduleName,
                    static_cast<unsigned long>(m.rva));
            }
            else {
                std::printf(
                    "  #%d  %08lX  %s\n",
                    i,
                    ret,
                    "<module unresolved>");
            }

            ebp = next;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
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

    {
        const uintptr_t faultAddress =
            reinterpret_cast<uintptr_t>(er->ExceptionAddress);
        const ModuleAddressInfo m = ResolveModuleAddress(faultAddress);

        if (m.valid) {
            std::printf("Module    : %s\n", m.moduleName);
            std::printf("ModuleBase: %08lX\n",
                static_cast<unsigned long>(m.base));
            std::printf("ModuleRVA : %08lX\n",
                static_cast<unsigned long>(m.rva));
            std::printf("ModulePath: %s\n", m.modulePath);
        }
        else {
            std::printf("Module    : <unresolved>\n");
        }
    }

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

bool loadingprofile06::Initialize() {
#if defined(_M_IX86)
    if (FM::GetEntryPoint() != 0x40109F ||
        plugin::patch::GetFloat(0xBD832C) != 1.3333334f)
        return false; // First version intentionally supports only verified 06.
    const DWORD length = GetModuleFileNameA(nullptr, logDirectory, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    char* slash = std::strrchr(logDirectory, '\\');
    if (!slash) return false;
    *slash = 0;
    // Leave room for unique capture filenames, not just the directory itself.
    if (std::strlen(logDirectory) > 170) return false;
    strcat_s(logDirectory, "\\logs");
    if (!CreateDirectoryA(logDirectory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return false;
    const int requested = GetPrivateProfileIntA("LOADING_PROFILE", "MAX_EVENTS", 65536, ".\\main.ini");
    if (requested >= 4096 && requested <= 262144) capacity = static_cast<size_t>(requested);
    events = static_cast<Event*>(VirtualAlloc(nullptr, sizeof(Event) * capacity,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!events) { Status("INITIALIZATION FAILED: cannot allocate capture buffer."); return false; }
    InitializeCriticalSection(&lock);
    autoCapture = GetPrivateProfileIntA("LOADING_PROFILE", "AUTO_CAPTURE", 1, ".\\main.ini") != 0;
    const int maxSeconds = GetPrivateProfileIntA("LOADING_PROFILE", "MAX_SECONDS", 120, ".\\main.ini");
    if (maxSeconds >= 10 && maxSeconds <= 600) maxCaptureSeconds = static_cast<DWORD>(maxSeconds);
    ready = true;
    if (autoCapture) InstallIntroBoundary();
    const bool fastWait = GetPrivateProfileIntA("LOADING", "FAST_FILE_WAIT", 0, ".\\main.ini") != 0;
    LiveLoading::InstallFileCompletionDelay(fastWait);
    assetHook = InstallFileSystemOpenResolvedHookForCurrentGame();
    closeHook = PatchIAT("KERNEL32.dll", "CloseHandle", reinterpret_cast<void*>(&Close), reinterpret_cast<void**>(&originalClose));
    openAHook = PatchIAT("KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&OpenA), reinterpret_cast<void**>(&originalOpenA));
    openWHook = PatchIAT("KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(&OpenW), reinterpret_cast<void**>(&originalOpenW));
    readHook = PatchIAT("KERNEL32.dll", "ReadFile", reinterpret_cast<void*>(&Read), reinterpret_cast<void**>(&originalRead));
    if (!assetHook && !openAHook && !openWHook && !readHook) {
        Status("INITIALIZATION FAILED: no verified hooks installed."); return false;
    }
    const bool autoStart = GetPrivateProfileIntA("LOADING_PROFILE", "AUTO_START", 0, ".\\main.ini") != 0;
    HANDLE worker = CreateThread(nullptr, 0, Worker,
        autoStart ? reinterpret_cast<void*>(1) : nullptr, 0, nullptr);
    if (!worker) { Status("INITIALIZATION FAILED: cannot start capture worker."); return false; }
    CloseHandle(worker);
    return true;
#else
    return false;
#endif
}

void ReportLive07CleanupRejection(const char* reason, uintptr_t objectPtr,
    uintptr_t ownerPtr, uintptr_t ownerListPtr, DWORD ownerCount)
{
        if (g_lockReady) {
            EnterCriticalSection(&g_consoleLock);
            PrintPrefix(
                "FIX07",
                FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);

            std::printf(
                "SKIP stale cleanup resource reason=%s "
                "object=%08lX owner=%08lX list=%08lX count=%08lX\n",
                reason,
                static_cast<unsigned long>(objectPtr),
                static_cast<unsigned long>(ownerPtr),
                static_cast<unsigned long>(ownerListPtr),
                static_cast<unsigned long>(ownerCount));

            LeaveCriticalSection(&g_consoleLock);
        }

}
} // namespace

void InitializeDebugConsole() {
    g_enabled = GetPrivateProfileIntA("DEBUG", "CONSOLE", 0, ".\\main.ini") != 0;
    const bool loadingProfile = GetPrivateProfileIntA("LOADING_PROFILE", "ENABLED", 0, ".\\main.ini") != 0;
    const bool fastFileWait = GetPrivateProfileIntA("LOADING", "FAST_FILE_WAIT", 0, ".\\main.ini") != 0;
    if (!g_enabled && !loadingProfile && !fastFileWait) return;

    // The IAT hooks must be installed exactly once. If initialization runs
    // twice and PatchIAT records HookCreateFileA/HookCloseHandle as the
    // "original" function, the hooks recurse into themselves until the thread
    // stack is exhausted.
    if (InterlockedCompareExchange(&g_debugConsoleInitState, 1, 0) != 0)
        return;

    LiveLoading::SetWaitObserver(&loadingprofile06::BeginWait, &loadingprofile06::FinishWait);

    if (fastFileWait) {
        const bool installed = LiveLoading::InstallFileCompletionDelay(true);
        LiveLoading::WriteFileWaitInstallStatus(installed);
        if (!installed)
            OutputDebugStringA("[Loading] FAST_FILE_WAIT skipped: unsupported build or changed instruction bytes.\n");
    }
    if (!g_enabled && !loadingProfile) return;

    if (!g_enabled) {
        if (!loadingprofile06::Initialize())
            OutputDebugStringA("[LoadingProfile06] Initialization failed (requires supported NBA Live 06 x86).\n");
        return;
    }
    if (loadingProfile)
        OutputDebugStringA("[LoadingProfile06] Quiet capture requires [DEBUG] CONSOLE=0; profiler skipped.\n");

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
            const bool cleanupGuard07 = Live07CleanupFix::Install(&IsReadablePointerRange, &ReportLive07CleanupRejection);
            PrintPrefix(
                "DEBUG",
                cleanupGuard07 ? (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
                               : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY));
            std::printf(
                "NBA Live 07 stale-cleanup guard v2: %s\n",
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

// Game context and commentary diagnostics.
using namespace plugin;

namespace custompaths {
void* gLastPlayoffSnapshotOptions = nullptr;

unsigned int gLastPlayoffSnapshotHash = 0;

unsigned char gPreviousOptionsSnapshot[0x200] = {};

bool gPreviousOptionsSnapshotValid = false;

FILE* gPlayoffStateLogFile = nullptr;

bool gLastTournamentSnapshotValid = false;

unsigned int gLastTournamentSnapshotHash = 0;

int gLastTournamentGameMode = -1;

unsigned int gPlayoffSnapshotSequence = 0;

unsigned int gLastTournamentPlayoffSequence = 0;

unsigned int gTournamentSnapshotSequence = 0;

TournamentChildSnapshot
        gTournamentChildSnapshots[kTournamentChildCandidateCount] = {};

unsigned char
        gLive06PlayoffBracketTeams[kLive06PlayoffBracketSlotCount] = {};

short gLive06PlayoffBracketWins[kLive06PlayoffBracketSlotCount] = {};

bool gLive06PlayoffBracketCacheValid = false;

void* gLive06CapturedPlayoffBracket = nullptr;

PlayoffAccessorTrace
        gPlayoffAccessorTraces[kPlayoffAccessorTraceCapacity] = {};

unsigned int gPlayoffAccessorTraceCount = 0;

unsigned int gPlayoffAccessorTraceCursor = 0;

CommentaryParameterTrace
        gCommentaryParameterTraces[kCommentaryParameterTraceCapacity] = {};

unsigned int gCommentaryParameterTraceCount = 0;

unsigned int gCommentaryParameterTraceCursor = 0;

unsigned int gLive06IntroArguments[36] = {};

bool gLive06IntroArgumentsValid = false;

unsigned int gLive06IntroTraceCount = 0;

void InitializePlayoffStateLog()
    {
        if (!gPlayoffStateDebug || gPlayoffStateLogFile)
            return;

        if (!CreateDirectoryA("logs", nullptr))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS)
            {
                OutputDebugStringA(
                    "[CustomPaths] Could not create the logs directory.\n");
                return;
            }
        }

        if (fopen_s(
                &gPlayoffStateLogFile,
                "logs\\playoff_state.log",
                "a") != 0 ||
            !gPlayoffStateLogFile)
        {
            gPlayoffStateLogFile = nullptr;
            OutputDebugStringA(
                "[CustomPaths] Could not open logs\\playoff_state.log.\n");
            return;
        }

        SYSTEMTIME now = {};
        GetLocalTime(&now);
        fprintf_s(
            gPlayoffStateLogFile,
            "\n============================================================\n"
            "Playoff-state session: %04u-%02u-%02u %02u:%02u:%02u "
            "NBA Live %s\n"
            "============================================================\n",
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            GameVersionName());
        fflush(gPlayoffStateLogFile);

        OutputDebugStringA(
            "[CustomPaths] Playoff state log: "
            "logs\\playoff_state.log.\n");
    }

void WritePlayoffStateLog(const char* message)
    {
        if (!message)
            return;

        OutputDebugStringA(message);
        if (!gPlayoffStateLogFile)
            return;

        fputs(message, gPlayoffStateLogFile);
        fflush(gPlayoffStateLogFile);
    }

bool AddMemoryToHash(
        const void* address,
        size_t size,
        unsigned int& hash)
    {
        if (!IsReadableMemoryRange(address, size))
            return false;

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(address);
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 16777619u;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

void LogRawMemoryBlock(
        const char* label,
        const void* address,
        size_t size)
    {
        if (!label || !IsReadableMemoryRange(address, size))
            return;

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(address);
            char message[256] = {};
            for (size_t offset = 0; offset < size; offset += 16)
            {
                const unsigned int* values =
                    reinterpret_cast<const unsigned int*>(bytes + offset);
                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] %s +%04X: %08X %08X %08X %08X\n",
                    label,
                    static_cast<unsigned int>(offset),
                    values[0],
                    values[1],
                    values[2],
                    values[3]);
                WritePlayoffStateLog(message);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] Tournament raw-memory read failed.\n");
        }
    }

void LogLive06PlayoffSettings(const void* settings)
    {
        // NBA Live 06 SettingsObject is 0x428 bytes. The executable's
        // SettingsObject accessors use the bool array at +0x7E and integer
        // array at +0xBC. The IDs below come directly from the game's
        // registered SETTINGS_* enum names. These are setup/configuration
        // values; the live series results are owned by the bracket object.
        const size_t settingsSize = 0x428;
        if (!IsReadableMemoryRange(settings, settingsSize))
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_SETTINGS readable=no.\n");
            return;
        }

        const unsigned int awayLostId = 0x0B;
        const unsigned int awayWonId = 0x0C;
        const unsigned int homeLostId = 0x23;
        const unsigned int homeWonId = 0x24;
        const unsigned int awayCouldWinId = 0x2A;
        const unsigned int homeCouldWinId = 0x2B;
        const unsigned int seriesLengthId = 0x3B;
        const unsigned int roundId = 0x3C;
        const unsigned int seriesTypeId = 0x3D;

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(settings);
            char message[512] = {};

            const unsigned int boolBase = 0x7E;
            const unsigned int intBase = 0xBC;
            const int awayLost = *reinterpret_cast<const int*>(
                bytes + intBase + awayLostId * sizeof(int));
            const int awayWon = *reinterpret_cast<const int*>(
                bytes + intBase + awayWonId * sizeof(int));
            const int homeLost = *reinterpret_cast<const int*>(
                bytes + intBase + homeLostId * sizeof(int));
            const int homeWon = *reinterpret_cast<const int*>(
                bytes + intBase + homeWonId * sizeof(int));
            const int seriesLength = *reinterpret_cast<const int*>(
                bytes + intBase + seriesLengthId * sizeof(int));
            const int round = *reinterpret_cast<const int*>(
                bytes + intBase + roundId * sizeof(int));
            const int seriesType = *reinterpret_cast<const int*>(
                bytes + intBase + seriesTypeId * sizeof(int));
            const unsigned int awayCouldWin =
                bytes[boolBase + awayCouldWinId] ? 1u : 0u;
            const unsigned int homeCouldWin =
                bytes[boolBase + homeCouldWinId] ? 1u : 0u;

            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_CONFIG "
                "awayW=%d awayL=%d homeW=%d homeL=%d "
                "awayCouldWin=%u homeCouldWin=%u "
                "length=%d round=%d seriesType=%d\n",
                awayWon,
                awayLost,
                homeWon,
                homeLost,
                awayCouldWin,
                homeCouldWin,
                seriesLength,
                round,
                seriesType);
            WritePlayoffStateLog(message);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_SETTINGS read failed.\n");
        }
    }

void LogLive06PlayoffBracket(const void* bracket)
    {
        // The persistent playoff tree contains 30 nodes: 16 first-round
        // teams, 8 conference-semifinal teams, 4 conference-final teams and
        // 2 Finals teams. Native function 0x004F8820 reads the consecutive
        // team bytes at +0x04 and 0x004F8730 writes the corresponding signed
        // 16-bit win counts at +0x22. Unlike the temporary records at +0x3BC,
        // these arrays remain valid after the game-launch transition.
        const size_t bracketSize = 0x4BC;
        const bool bracketReadable =
            IsReadableMemoryRange(bracket, bracketSize);
        if (!bracketReadable && !gLive06PlayoffBracketCacheValid)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_BRACKET readable=no.\n");
            return;
        }

        __try
        {
            const unsigned char* teams = gLive06PlayoffBracketTeams;
            const short* wins = gLive06PlayoffBracketWins;
            if (bracketReadable)
            {
                const unsigned char* bytes =
                    reinterpret_cast<const unsigned char*>(bracket);
                teams = bytes + 0x04;
                wins = reinterpret_cast<const short*>(bytes + 0x22);
            }
            else
            {
                WritePlayoffStateLog(
                    "[CustomPaths] PLAYOFF_BRACKET source=cached-before-"
                    "destruction\n");
            }
            char message[192] = {};

            for (size_t i = 0;
                 i < kLive06PlayoffBracketSlotCount;
                 ++i)
            {
                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] PLAYOFF_BRACKET_SLOT slot=%02u "
                    "team=%u wins=%d\n",
                    static_cast<unsigned int>(i),
                    static_cast<unsigned int>(teams[i]),
                    static_cast<int>(wins[i]));
                WritePlayoffStateLog(message);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_BRACKET persistent read failed.\n");
        }
    }

void CaptureLive06PlayoffBracket(const void* bracket)
    {
        if (!gPlayoffStateDebug ||
            !IsReadableMemoryRange(bracket, 0x4BC))
        {
            return;
        }

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(bracket);
            std::memcpy(
                gLive06PlayoffBracketTeams,
                bytes + 0x04,
                sizeof(gLive06PlayoffBracketTeams));
            std::memcpy(
                gLive06PlayoffBracketWins,
                bytes + 0x22,
                sizeof(gLive06PlayoffBracketWins));
            gLive06CapturedPlayoffBracket = const_cast<void*>(bracket);
            gLive06PlayoffBracketCacheValid = true;

            char message[160] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_BRACKET_CAPTURE bracket=%08X "
                "slots=%u\n",
                static_cast<unsigned int>(
                    reinterpret_cast<uintptr_t>(bracket)),
                static_cast<unsigned int>(
                    kLive06PlayoffBracketSlotCount));
            WritePlayoffStateLog(message);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_BRACKET capture failed.\n");
        }
    }

void __fastcall Live06PlayoffBracketDestructorHook(
        void* bracket,
        void*)
    {
        CaptureLive06PlayoffBracket(bracket);
        CallMethodDynGlobal(0x0051E700, bracket);
    }

const char* Live06PlayoffSettingName(unsigned int id)
    {
        switch (id)
        {
        case 0x0B: return "AWAY_LOST";
        case 0x0C: return "AWAY_WON";
        case 0x23: return "HOME_LOST";
        case 0x24: return "HOME_WON";
        case 0x2A: return "AWAY_COULD_WIN";
        case 0x2B: return "HOME_COULD_WIN";
        case 0x3B: return "LENGTH";
        case 0x3C: return "ROUND";
        case 0x3D: return "SERIES_TYPE";
        default:   return "UNKNOWN";
        }
    }

void LogLive06PlayoffAccessor(
        void* settings,
        uintptr_t caller,
        unsigned int id,
        unsigned int source,
        int value,
        bool isBoolean)
    {
        if (!gPlayoffStateDebug)
            return;

        for (unsigned int i = 0;
             i < gPlayoffAccessorTraceCount;
             ++i)
        {
            const PlayoffAccessorTrace& trace =
                gPlayoffAccessorTraces[i];
            if (trace.settings == settings &&
                trace.caller == caller &&
                trace.id == id &&
                trace.source == source &&
                trace.value == value &&
                trace.isBoolean == isBoolean)
            {
                return;
            }
        }

        PlayoffAccessorTrace& trace =
            gPlayoffAccessorTraces[gPlayoffAccessorTraceCursor];
        trace.settings = settings;
        trace.caller = caller;
        trace.id = id;
        trace.source = source;
        trace.value = value;
        trace.isBoolean = isBoolean;

        if (gPlayoffAccessorTraceCount < kPlayoffAccessorTraceCapacity)
            ++gPlayoffAccessorTraceCount;
        gPlayoffAccessorTraceCursor =
            (gPlayoffAccessorTraceCursor + 1) %
            kPlayoffAccessorTraceCapacity;

        char message[256] = {};
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] PLAYOFF_ACCESS type=%s name=%s id=%02X "
            "value=%d source=%u settings=%08X caller=%08X\n",
            isBoolean ? "bool" : "int",
            Live06PlayoffSettingName(id),
            id,
            value,
            source,
            static_cast<unsigned int>(
                reinterpret_cast<uintptr_t>(settings)),
            static_cast<unsigned int>(caller));
        WritePlayoffStateLog(message);
    }

void LogLive06CommentaryParameter(
        uintptr_t caller,
        unsigned int index,
        int value,
        bool write)
    {
        if (!gPlayoffStateDebug)
            return;

        for (unsigned int i = 0;
             i < gCommentaryParameterTraceCount;
             ++i)
        {
            const CommentaryParameterTrace& trace =
                gCommentaryParameterTraces[i];
            if (trace.caller == caller &&
                trace.index == index &&
                trace.value == value &&
                trace.write == write)
            {
                return;
            }
        }

        CommentaryParameterTrace& trace =
            gCommentaryParameterTraces[gCommentaryParameterTraceCursor];
        trace.caller = caller;
        trace.index = index;
        trace.value = value;
        trace.write = write;
        if (gCommentaryParameterTraceCount <
            kCommentaryParameterTraceCapacity)
        {
            ++gCommentaryParameterTraceCount;
        }
        gCommentaryParameterTraceCursor =
            (gCommentaryParameterTraceCursor + 1) %
            kCommentaryParameterTraceCapacity;

        char message[224] = {};
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] PLAYOFF_COMMENTARY_PARAM op=%s index=%u "
            "value=%d caller=%08X\n",
            write ? "write" : "read",
            index,
            value,
            static_cast<unsigned int>(caller));
        WritePlayoffStateLog(message);
    }

#if defined(_M_IX86)
uintptr_t gLive06IntroEventTarget = 0x00434610;

uintptr_t gLive06SpeechDispatchContinuation = 0;

void __stdcall LogLive06SpeechDispatch(const unsigned int* entryStack)
    {
        static unsigned int snapshots = 0;
        static unsigned int previousHash = 0;
        static bool previousValid = false;
        if (!gPlayoffStateDebug || snapshots >= 512)
            return;
        __try
        {
            const unsigned int caller = entryStack[0];
            // Native speech emitters. Exclude frontend and middleware traffic.
            const bool live07 = gGameVersion == GAME_LIVE_07 || gGameVersion == GAME_LIVE_08;
            if (caller < (live07 ? 0x00460000u : 0x00430000u) ||
                caller >= (live07 ? 0x004A0000u : 0x00470000u))
                return;
            const unsigned int event = entryStack[1];
            const unsigned int count = entryStack[2];
            if (count > 64 ||
                !IsReadableMemoryRange(entryStack, (count + 3) * sizeof(unsigned int)))
                return;
            unsigned int hash = 2166136261u;
            for (unsigned int i = 0; i < count + 3; ++i)
                hash = (hash ^ entryStack[i]) * 16777619u;
            if (previousValid && hash == previousHash)
                return;
            previousHash = hash;
            previousValid = true;
            ++snapshots;
            char message[320] = {};
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_SPEECH_EVENT snapshot=%u "
                "event=%u route=%u group=%u packed=%08X count=%u caller=%08X\n",
                snapshots, event & 0xFFFFu, event >> 24,
                (event >> 16) & 0xFFu, event, count, caller);
            WritePlayoffStateLog(message);
            for (unsigned int i = 0; i < count; ++i)
            {
                sprintf_s(message, sizeof(message),
                    "[CustomPaths] PLAYOFF_SPEECH_ARG snapshot=%u "
                    "index=%02u hex=%08X signed=%d\n", snapshots, i,
                    entryStack[i + 3], static_cast<int>(entryStack[i + 3]));
                WritePlayoffStateLog(message);
            }
            // Capture the settings at the actual speech dispatch boundary.
            // This also works when the event-8 emitter never runs.
            LogLive06AudioSettings("speech-dispatch");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_SPEECH_EVENT unreadable.\n");
        }
    }

void __declspec(naked) Live06SpeechDispatchTraceHook()
    {
        __asm
        {
            pushfd
            pushad
            cld
            mov ebx, esp
            lea eax, [ebx + 36]
            sub esp, 528
            and esp, 0FFFFFFF0h
            fxsave [esp]
            push eax
            call LogLive06SpeechDispatch
            fxrstor [esp]
            mov esp, ebx
            popad
            popfd
            jmp dword ptr [gLive06SpeechDispatchContinuation]
        }
    }

bool InstallLive06SpeechDispatchTrace()
    {
        // Whole instructions: PUSH EBX; PUSH ESI; PUSH EDI;
        // MOV EDI,[ESP+14h]. No relative instructions in this prologue.
        const bool live07 = gGameVersion == GAME_LIVE_07;
        const bool live08 = gGameVersion == GAME_LIVE_08;
        const size_t patchLength = live07 || live08 ? 6 : 7;
        unsigned char* target = reinterpret_cast<unsigned char*>(gGameVersion == GAME_LIVE_2005 ? 0x0070DDB0 : (live08 ? 0x00851580 : (live07 ? 0x00822540 : 0x00722C0D)));
        const unsigned char expected[7] = {
            0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14
        };
        const unsigned char expected07[6] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 };
        if (std::memcmp(target, live07 || live08 ? expected07 : expected, patchLength) != 0)
            return false;
        unsigned char* trampoline = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, patchLength + 5, MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (!trampoline)
            return false;
        std::memcpy(trampoline, target, patchLength);
        trampoline[patchLength] = 0xE9;
        const int back = static_cast<int>(
            reinterpret_cast<uintptr_t>(target + patchLength) -
            reinterpret_cast<uintptr_t>(trampoline + patchLength + 5));
        std::memcpy(trampoline + patchLength + 1, &back, sizeof(back));
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE,
                            &oldProtect))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }
        gLive06SpeechDispatchContinuation =
            reinterpret_cast<uintptr_t>(trampoline);
        unsigned char replacement[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
        const int forward = static_cast<int>(
            reinterpret_cast<uintptr_t>(Live06SpeechDispatchTraceHook) -
            reinterpret_cast<uintptr_t>(target + 5));
        std::memcpy(replacement + 1, &forward, sizeof(forward));
        std::memcpy(target, replacement, patchLength);
        DWORD ignored = 0;
        VirtualProtect(target, patchLength, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), trampoline, patchLength + 5);
        FlushInstructionCache(GetCurrentProcess(), target, patchLength);
        return true;
    }

void LogLive06AudioSettings(const char* phase)
    {
        char message[320] = {};
        __try
        {
            const unsigned char* root =
                *reinterpret_cast<unsigned char**>(gGameVersion == GAME_LIVE_2005
                    ? 0x00C44A30 : (gGameVersion == GAME_LIVE_08
                    ? 0x00DA3D08 : (gGameVersion == GAME_LIVE_07 ? 0x00CE8378 : 0x00CAF5D8)));
            if (!IsReadableMemoryRange(root, 0x3C))
                return;
            const unsigned char* rules =
                *reinterpret_cast<const unsigned char* const*>(root + 0x28);
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_AUDIO_SETTINGS phase=%s root=%08X "
                "rules=%08X lastOptions=%08X ready=%u active=%u\n", phase,
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(root)),
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(rules)),
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(gLastPlayoffSnapshotOptions)),
                static_cast<unsigned int>(root[8]),
                static_cast<unsigned int>(root[9]));
            WritePlayoffStateLog(message);
            if (!IsReadableMemoryRange(rules, 0x174))
                return;
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_AUDIO_RULES phase=%s mode=%d "
                "round=%d adjacent=%d length=%d\n", phase,
                *reinterpret_cast<const int*>(rules + (gGameVersion == GAME_LIVE_08 ? 0x64 : 0x5C)),
                *reinterpret_cast<const int*>(rules + (gGameVersion == GAME_LIVE_08 ? 0x168 : 0x158)),
                *reinterpret_cast<const int*>(rules + (gGameVersion == GAME_LIVE_08 ? 0x16C : 0x15C)),
                *reinterpret_cast<const int*>(rules + (gGameVersion == GAME_LIVE_08 ? 0x170 : 0x160)));
            WritePlayoffStateLog(message);
            LogRawMemoryBlock("AUDIO_RULES_100", rules + 0x100, 0x70);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_AUDIO_SETTINGS unreadable.\n");
        }
    }

void __stdcall LogLive06SetupHandoff(
        const unsigned char* setup, const unsigned int* entryStack,
        const unsigned int* vtable, void* interfaceObject)
    {
        static unsigned int count = 0;
        if (!gPlayoffStateDebug || count >= 32)
            return;
        ++count;
        char message[320] = {};
        __try
        {
            const unsigned char* rules =
                reinterpret_cast<const unsigned char*>(entryStack[2]);
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_SETUP_HANDOFF snapshot=%u "
                "setup=%08X interface=%08X target=%08X "
                "settings=%08X rules=%08X\n", count,
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(setup)),
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(interfaceObject)),
                vtable[0x15C / 4], entryStack[1], entryStack[2]);
            WritePlayoffStateLog(message);
            if (IsReadableMemoryRange(rules, 0x174))
            {
                sprintf_s(message, sizeof(message),
                    "[CustomPaths] PLAYOFF_SETUP_RULES mode=%d "
                    "field158=%d field15C=%d length160=%d\n",
                    *reinterpret_cast<const int*>(rules + 0x5C),
                    *reinterpret_cast<const int*>(rules + 0x158),
                    *reinterpret_cast<const int*>(rules + 0x15C),
                    *reinterpret_cast<const int*>(rules + 0x160));
                WritePlayoffStateLog(message);
                LogRawMemoryBlock("SETUP_RULES_100", rules + 0x100, 0x70);
            }
            LogLive06AudioSettings("setup-handoff-before");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_SETUP_HANDOFF unreadable.\n");
        }
    }

void __declspec(naked) Live06SetupHandoffTraceHook()
    {
        // Replace only CALL [eax+15Ch]. EAX is the native vtable and ECX
        // the native interface. Restore all state before dispatching it.
        __asm
        {
            pushfd
            pushad
            cld
            mov ebx, esp
            sub esp, 528
            and esp, 0FFFFFFF0h
            fxsave [esp]
            push dword ptr [ebx + 24]
            push dword ptr [ebx + 28]
            lea edx, [ebx + 36]
            push edx
            push dword ptr [ebx + 4]
            call LogLive06SetupHandoff
            fxrstor [esp]
            mov esp, ebx
            popad
            popfd
            jmp dword ptr [eax + 15Ch]
        }
    }

bool InstallLive06SetupHandoffTrace()
    {
        unsigned char* site = reinterpret_cast<unsigned char*>(0x0062F7DF);
        const unsigned char expected[6] = { 0xFF, 0x90, 0x5C, 0x01, 0, 0 };
        if (std::memcmp(site, expected, sizeof(expected)) != 0)
            return false;
        DWORD oldProtect = 0;
        if (!VirtualProtect(site, sizeof(expected), PAGE_EXECUTE_READWRITE,
                            &oldProtect))
            return false;
        unsigned char replacement[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        const int displacement = static_cast<int>(
            reinterpret_cast<uintptr_t>(Live06SetupHandoffTraceHook) -
            reinterpret_cast<uintptr_t>(site + 5));
        std::memcpy(replacement + 1, &displacement, sizeof(displacement));
        std::memcpy(site, replacement, sizeof(replacement));
        DWORD ignored = 0;
        VirtualProtect(site, sizeof(expected), oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(expected));
        return true;
    }

void __stdcall LogLive06IntroArguments(const unsigned int* entryStack)
    {
        if (!gPlayoffStateDebug || gLive06IntroTraceCount >= 64)
            return;

        unsigned int arguments[36] = {};
        __try
        {
            std::memcpy(arguments, entryStack + 1, sizeof(arguments));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_INTRO arguments unreadable.\n");
            return;
        }

        if (gLive06IntroArgumentsValid &&
            std::memcmp(arguments, gLive06IntroArguments,
                        sizeof(arguments)) == 0)
        {
            return;
        }
        std::memcpy(gLive06IntroArguments, arguments, sizeof(arguments));
        gLive06IntroArgumentsValid = true;
        ++gLive06IntroTraceCount;

        char message[320] = {};
        sprintf_s(message, sizeof(message),
            "[CustomPaths] PLAYOFF_INTRO snapshot=%u event=8 count=36 "
            "caller=%08X forced=%u\n",
            gLive06IntroTraceCount, entryStack[0],
            gForcedSeriesEnabled && gForcedStageEnabled &&
                gForceGlobalGameMode ? 1u : 0u);
        WritePlayoffStateLog(message);

        LogLive06AudioSettings("intro-event");
        for (unsigned int i = 0; i < 36; i += 4)
        {
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_INTRO_ARGS snapshot=%u "
                "index=%02u..%02u hex=%08X,%08X,%08X,%08X "
                "signed=%d,%d,%d,%d\n",
                gLive06IntroTraceCount, i, i + 3,
                arguments[i], arguments[i + 1],
                arguments[i + 2], arguments[i + 3],
                static_cast<int>(arguments[i]),
                static_cast<int>(arguments[i + 1]),
                static_cast<int>(arguments[i + 2]),
                static_cast<int>(arguments[i + 3]));
            WritePlayoffStateLog(message);
        }
    }

void __declspec(naked) Live06IntroEventTraceHook()
    {
        // The native cdecl emitter receives 36 dwords. Preserve its exact
        // stack, registers, flags, x87 and SSE state, then tail-call it.
        __asm
        {
            pushfd
            pushad
            cld
            mov ebx, esp
            lea eax, [ebx + 36]
            sub esp, 528
            and esp, 0FFFFFFF0h
            fxsave [esp]
            push eax
            call LogLive06IntroArguments
            fxrstor [esp]
            mov esp, ebx
            popad
            popfd
            jmp dword ptr [gLive06IntroEventTarget]
        }
    }

#endif
void LogLive06TournamentState()
    {
        if (!gPlayoffStateDebug || gGameVersion != GAME_LIVE_06)
            return;

        // Log exactly once for each PLAYOFF_STATE notification, even when
        // the decoded SettingsObject and bracket bytes happen to be equal.
        if (gPlayoffSnapshotSequence == 0 ||
            gLastTournamentPlayoffSequence == gPlayoffSnapshotSequence)
        {
            return;
        }

        const size_t uberSize = 0x90;
        // Executable research identified +0x58 as SettingsObject and +0x74
        // as the tournament/bracket object. Do not scan unrelated FE objects.
        const unsigned int childOffsets[kTournamentChildCandidateCount] = {
            0x58, 0x74
        };
        const size_t childSizes[kTournamentChildCandidateCount] = {
            0x428, 0x854
        };
        void* uber = nullptr;

        __try
        {
            uber = *reinterpret_cast<void**>(0x00CB2A2C);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }

        if (!IsReadableMemoryRange(uber, uberSize))
            return;

        void* children[kTournamentChildCandidateCount] = {};
        void* tournamentRoot = nullptr;
        void* bracket = nullptr;
        int gameMode = -1;
        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(uber);
            gameMode = *reinterpret_cast<const int*>(bytes + 0x08);
            for (size_t i = 0; i < kTournamentChildCandidateCount; ++i)
            {
                children[i] = *reinterpret_cast<void* const*>(
                    bytes + childOffsets[i]);
            }

            // The global game-mode owner stores the active playoff bracket at
            // +0x8E4. Native playoff code at 0x004F82D2 follows this exact
            // chain, and the bracket destructor at 0x0051E710 clears it.
            tournamentRoot = *reinterpret_cast<void**>(0x00CB2B08);
            const unsigned char* tournamentRootBytes =
                reinterpret_cast<const unsigned char*>(tournamentRoot);
            if (IsReadableMemoryRange(tournamentRootBytes, 0x8E8))
            {
                bracket = *reinterpret_cast<void* const*>(
                    tournamentRootBytes + 0x8E4);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }

        unsigned int hash = 2166136261u;
        if (!AddMemoryToHash(uber, uberSize, hash))
            return;

        for (size_t i = 0; i < kTournamentChildCandidateCount; ++i)
        {
            const size_t childSize = childSizes[i];
            const uintptr_t pointerValue =
                reinterpret_cast<uintptr_t>(children[i]);
            hash ^= static_cast<unsigned int>(pointerValue);
            hash *= 16777619u;
            AddMemoryToHash(children[i], childSize, hash);
        }

        const uintptr_t bracketPointerValue =
            reinterpret_cast<uintptr_t>(bracket);
        hash ^= static_cast<unsigned int>(bracketPointerValue);
        hash *= 16777619u;
        AddMemoryToHash(bracket, 0x4BC, hash);

        const bool gameModeChanged = gameMode != gLastTournamentGameMode;

        gLastTournamentPlayoffSequence = gPlayoffSnapshotSequence;
        gLastTournamentSnapshotValid = true;
        gLastTournamentSnapshotHash = hash;
        ++gTournamentSnapshotSequence;

        if (gameModeChanged)
        {
            gLastTournamentGameMode = gameMode;
            std::memset(
                gTournamentChildSnapshots,
                0,
                sizeof(gTournamentChildSnapshots));
        }

        char message[256] = {};
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] TOURNAMENT_STATE snapshot=%u uber=%08X "
            "gameMode=%d hash=%08X\n",
            gTournamentSnapshotSequence,
            static_cast<unsigned int>(reinterpret_cast<uintptr_t>(uber)),
            gameMode,
            hash);
        WritePlayoffStateLog(message);

        LogLive06PlayoffSettings(children[0]);
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] PLAYOFF_BRACKET root=%08X bracket=%08X\n",
            static_cast<unsigned int>(
                reinterpret_cast<uintptr_t>(tournamentRoot)),
            static_cast<unsigned int>(
                reinterpret_cast<uintptr_t>(bracket)));
        WritePlayoffStateLog(message);
        LogLive06PlayoffBracket(bracket);

        if (gameModeChanged)
        {
            WritePlayoffStateLog(
                "[CustomPaths] TOURNAMENT_CONTEXT changed; child "
                "baselines reset.\n");
        }

        LogRawMemoryBlock("FE_UBER", uber, uberSize);

        for (size_t i = 0; i < kTournamentChildCandidateCount; ++i)
        {
            const size_t childSize = childSizes[i];
            const bool readable =
                IsReadableMemoryRange(children[i], childSize);
            TournamentChildSnapshot& previous =
                gTournamentChildSnapshots[i];

            if (!readable)
            {
                if (previous.valid || previous.pointer != children[i])
                {
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] FE_CHILD_%02X pointer=%08X "
                        "readable=no\n",
                        childOffsets[i],
                        static_cast<unsigned int>(
                            reinterpret_cast<uintptr_t>(children[i])));
                    WritePlayoffStateLog(message);
                }
                previous.pointer = children[i];
                previous.valid = false;
                continue;
            }

            __try
            {
                const unsigned char* current =
                    reinterpret_cast<const unsigned char*>(children[i]);

                if (!previous.valid || previous.pointer != children[i])
                {
                    previous.pointer = children[i];
                    std::memcpy(previous.bytes, current, childSize);
                    previous.valid = true;
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] FE_CHILD_%02X baseline pointer=%08X "
                        "size=%04X\n",
                        childOffsets[i],
                        static_cast<unsigned int>(
                            reinterpret_cast<uintptr_t>(children[i])),
                        static_cast<unsigned int>(childSize));
                    WritePlayoffStateLog(message);
                    continue;
                }

                unsigned int changed = 0;
                const unsigned int maximumLoggedChanges = 256;
                for (size_t offset = 0;
                     offset + sizeof(unsigned int) <= childSize;
                     offset += sizeof(unsigned int))
                {
                    const unsigned int oldValue =
                        *reinterpret_cast<const unsigned int*>(
                            previous.bytes + offset);
                    const unsigned int newValue =
                        *reinterpret_cast<const unsigned int*>(
                            current + offset);
                    if (oldValue == newValue)
                        continue;

                    if (changed < maximumLoggedChanges)
                    {
                        sprintf_s(
                            message,
                            sizeof(message),
                            "[CustomPaths] FE_CHILD_%02X_CHANGE +%04X: "
                            "%08X -> %08X\n",
                            childOffsets[i],
                            static_cast<unsigned int>(offset),
                            oldValue,
                            newValue);
                        WritePlayoffStateLog(message);
                    }
                    ++changed;
                }

                if (changed > maximumLoggedChanges)
                {
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] FE_CHILD_%02X_CHANGE truncated: "
                        "%u total changed dwords.\n",
                        childOffsets[i],
                        changed);
                    WritePlayoffStateLog(message);
                }

                if (changed != 0)
                {
                    std::memcpy(previous.bytes, current, childSize);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] FE_CHILD_%02X comparison failed.\n",
                    childOffsets[i]);
                WritePlayoffStateLog(message);
                previous.valid = false;
            }
        }
    }

void LogPlayoffStateSnapshot(void* options)
    {
        if (!gPlayoffStateDebug || !options)
            return;

        const unsigned int firstOffset = 0x40;
        const unsigned int endOffset =
            gGameVersion == GAME_LIVE_08 ? 0x190 : 0x180;
        const unsigned int modeOffset =
            gGameVersion == GAME_LIVE_08 ? 0x64 : 0x5C;
        const unsigned int roundOffset =
            gGameVersion == GAME_LIVE_08 ? 0x168 : 0x158;

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(options);
            unsigned int hash = 2166136261u;
            for (unsigned int i = firstOffset; i < endOffset; ++i)
            {
                hash ^= bytes[i];
                hash *= 16777619u;
            }

            // Repeated GameCourt notifications normally carry the same state.
            // If the Live 06 schedule database was not ready on the first
            // notification, keep retrying until the exact series score has
            // been resolved.
            if (options == gLastPlayoffSnapshotOptions &&
                hash == gLastPlayoffSnapshotHash &&
                !gRetryLive06PlayoffSeries)
            {
                return;
            }
            gLastPlayoffSnapshotOptions = options;
            gLastPlayoffSnapshotHash = hash;
            gRetryLive06PlayoffSeries = false;
            ++gPlayoffSnapshotSequence;

            const int mode = *reinterpret_cast<const int*>(bytes + modeOffset);
            const int round = *reinterpret_cast<const int*>(bytes + roundOffset);
            char iniPath[MAX_PATH] = {};
            const DWORD exeLength = GetModuleFileNameA(
                nullptr, iniPath, static_cast<DWORD>(sizeof(iniPath)));
            if (exeLength == 0 || exeLength >= sizeof(iniPath))
            {
                strcpy_s(iniPath, ".\\main.ini");
            }
            else
            {
                char* slash = strrchr(iniPath, '\\');
                if (slash)
                    strcpy_s(slash + 1,
                        sizeof(iniPath) -
                            static_cast<size_t>(slash + 1 - iniPath),
                        "main.ini");
                else
                    strcpy_s(iniPath, "main.ini");
            }

            char rawStage[32] = {};
            GetPrivateProfileStringA(
                "PLAYOFF", "FORCE_STAGE", "OFF",
                rawStage, sizeof(rawStage), iniPath);

            char message[512] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_FORCE stage=%s enabled=%u global=%u "
                "series=%u game=%d homeWins=%d awayWins=%d "
                "iniStage=%s ini=%s\n",
                StageName(gForcedStage),
                gForcedStageEnabled ? 1u : 0u,
                gForceGlobalGameMode ? 1u : 0u,
                gForcedSeriesEnabled ? 1u : 0u,
                gForcedSeriesGame,
                gForcedHomeWins,
                gForcedAwayWins,
                rawStage,
                iniPath);
            WritePlayoffStateLog(message);

            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_STATE game=%s options=%08X "
                "mode[+%04X]=%d round[+%04X]=%d hash=%08X\n",
                GameVersionName(),
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(options)),
                modeOffset,
                mode,
                roundOffset,
                round,
                hash);
            WritePlayoffStateLog(message);

            if (gGameVersion == GAME_LIVE_06)
            {
                // RulesSettings carries the two native playoff-clinching
                // flags directly. Gameplay copies +0x10D/+0x10E into
                // 0x00D212D5/0x00D212D4 in sub_8CF820, and GdCentral copies
                // the same bytes into its presentation state at +0x24/+0x25.
                const bool awayCanWin = bytes[0x10D] != 0;
                const bool homeCanWin = bytes[0x10E] != 0;
                const char* clinchState = "none";
                if (awayCanWin && homeCanWin)
                    clinchState = "decisive";
                else if (awayCanWin)
                    clinchState = "away-can-clinch";
                else if (homeCanWin)
                    clinchState = "home-can-clinch";

                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] PLAYOFF_RULES awayCanWin=%u "
                    "homeCanWin=%u clinchState=%s\n",
                    awayCanWin ? 1u : 0u,
                    homeCanWin ? 1u : 0u,
                    clinchState);
                WritePlayoffStateLog(message);

                // ScheduleInfo.GetGame/GetLastGame read these two native
                // identifiers from the Live 06 schedule service. Keep this
                // probe narrow: it avoids reopening scheduleplayoff.dbf or
                // tracing the generic database layer during every file load.
                const unsigned char* scheduleInfo =
                    *reinterpret_cast<unsigned char* const*>(0x00CB2AE0);
                if (scheduleInfo)
                {
                    const unsigned int currentGame =
                        *reinterpret_cast<const unsigned short*>(
                            scheduleInfo + 0x64);
                    const unsigned int lastGame =
                        *reinterpret_cast<const unsigned short*>(
                            scheduleInfo + 0x66);

                    // scheduleplayoff.dbf interleaves the 15 bracket series:
                    // one potential game for every series per block. Thus a
                    // series advances by 15 GAMENUM values each game.
                    const bool validPlayoffGame =
                        currentGame > 0u && currentGame <= 15u * 7u;
                    const unsigned int seriesGame =
                        validPlayoffGame
                            ? ((currentGame - 1u) / 15u) + 1u
                            : 0u;
                    const unsigned int bracketSlot =
                        validPlayoffGame
                            ? ((currentGame - 1u) % 15u) + 1u
                            : 0u;

                    char seriesState[64] = "unknown";
                    if (seriesGame == 1u)
                    {
                        strcpy_s(seriesState, "tied-0-0");
                    }
                    else if (awayCanWin && homeCanWin)
                    {
                        strcpy_s(seriesState, "tied-3-3");
                    }
                    else if (awayCanWin && seriesGame >= 4u)
                    {
                        sprintf_s(
                            seriesState,
                            sizeof(seriesState),
                            "away-leads-3-%u",
                            seriesGame - 4u);
                    }
                    else if (homeCanWin && seriesGame >= 4u)
                    {
                        sprintf_s(
                            seriesState,
                            sizeof(seriesState),
                            "home-leads-3-%u",
                            seriesGame - 4u);
                    }
                    else if (seriesGame == 5u)
                    {
                        // Four completed games and neither team has three
                        // wins can only be a 2-2 series.
                        strcpy_s(seriesState, "tied-2-2");
                    }

                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] PLAYOFF_SCHEDULE currentGame=%u "
                        "lastGame=%u seriesGame=%u bracketSlot=%u "
                        "seriesState=%s service=%08X\n",
                        currentGame,
                        lastGame,
                        seriesGame,
                        bracketSlot,
                        seriesState,
                        static_cast<unsigned int>(
                            reinterpret_cast<uintptr_t>(scheduleInfo)));
                    WritePlayoffStateLog(message);

                    if (gForcedSeriesEnabled &&
                        gForcedStageEnabled &&
                        gForceGlobalGameMode)
                    {
                        gRetryLive06PlayoffSeries = false;
                        sprintf_s(
                            message,
                            sizeof(message),
                            "[CustomPaths] PLAYOFF_SERIES forced=yes "
                            "game=%d homeWins=%d awayWins=%d exact=yes\n",
                            gForcedSeriesGame,
                            gForcedHomeWins,
                            gForcedAwayWins);
                        WritePlayoffStateLog(message);
                    }
                    else
                    {
                        // Read the live schedule records owned by the game and
                        // count the completed games in this exact bracket slot.
                        // The DBF layout advances a series by 15 IDs per game.
                        Live06ScheduleGame currentScheduleGame = {};
                        if (validPlayoffGame &&
                            ReadLive06ScheduleGame(
                                currentGame,
                                currentScheduleGame))
                        {
                            // Native normalized layout: +20 home team, +22
                            // home score, +26 away team, +28 away score.
                            const short homeTeam = ReadScheduleShort(
                                currentScheduleGame,
                                20);
                            const short awayTeam = ReadScheduleShort(
                                currentScheduleGame,
                                26);
                            unsigned int homeWins = 0;
                            unsigned int awayWins = 0;
                            unsigned int historyGames = 0;
                            bool exactSeriesScore =
                                homeTeam >= 0 && awayTeam >= 0 &&
                                homeTeam != awayTeam;

                            unsigned int previousGame = currentGame;
                            while (exactSeriesScore && previousGame > 15u)
                            {
                                previousGame -= 15u;
                                Live06ScheduleGame prior = {};
                                if (!ReadLive06ScheduleGame(previousGame, prior))
                                {
                                    exactSeriesScore = false;
                                    break;
                                }

                                const short priorHome =
                                    ReadScheduleShort(prior, 20);
                                const short priorAway =
                                    ReadScheduleShort(prior, 26);
                                const short priorHomeScore =
                                    ReadScheduleShort(prior, 22);
                                const short priorAwayScore =
                                    ReadScheduleShort(prior, 28);
                                const bool sameTeams =
                                    (priorHome == homeTeam &&
                                     priorAway == awayTeam) ||
                                    (priorHome == awayTeam &&
                                     priorAway == homeTeam);

                                // +18 is PLAYED in the normalized native record.
                                if (prior.bytes[18] == 0 ||
                                    !sameTeams ||
                                    priorHomeScore == priorAwayScore)
                                {
                                    exactSeriesScore = false;
                                    break;
                                }

                                const short winner =
                                    priorHomeScore > priorAwayScore
                                        ? priorHome
                                        : priorAway;
                                if (winner == homeTeam)
                                    ++homeWins;
                                else if (winner == awayTeam)
                                    ++awayWins;
                                else
                                {
                                    exactSeriesScore = false;
                                    break;
                                }
                                ++historyGames;
                            }

                            // All preceding games must have been accounted for.
                            if (historyGames + 1u != seriesGame)
                                exactSeriesScore = false;

                            gRetryLive06PlayoffSeries = !exactSeriesScore;

                            sprintf_s(
                                message,
                                sizeof(message),
                                "[CustomPaths] PLAYOFF_SERIES game=%u "
                                "homeTeam=%d awayTeam=%d homeWins=%u "
                                "awayWins=%u historyGames=%u exact=%s\n",
                                seriesGame,
                                homeTeam,
                                awayTeam,
                                homeWins,
                                awayWins,
                                historyGames,
                                exactSeriesScore ? "yes" : "no");
                            WritePlayoffStateLog(message);
                        }
                        else
                        {
                            gRetryLive06PlayoffSeries = true;
                            WritePlayoffStateLog(
                                "[CustomPaths] PLAYOFF_SERIES available=no.\n");
                        }
                    }
                }
                else
                {
                    gRetryLive06PlayoffSeries = true;
                    WritePlayoffStateLog(
                        "[CustomPaths] PLAYOFF_SCHEDULE available=no.\n");
                }
            }

            if (!gPreviousOptionsSnapshotValid)
            {
                std::memcpy(
                    gPreviousOptionsSnapshot,
                    bytes,
                    endOffset);
                gPreviousOptionsSnapshotValid = true;
                WritePlayoffStateLog(
                    "[CustomPaths] OPTIONS_BASELINE captured; subsequent "
                    "snapshots will list changed dwords.\n");
            }
            else
            {
                unsigned int changed = 0;
                for (unsigned int offset = firstOffset;
                     offset + sizeof(unsigned int) <= endOffset;
                     offset += sizeof(unsigned int))
                {
                    const unsigned int previous =
                        *reinterpret_cast<const unsigned int*>(
                            gPreviousOptionsSnapshot + offset);
                    const unsigned int current =
                        *reinterpret_cast<const unsigned int*>(bytes + offset);
                    if (previous == current)
                        continue;

                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] OPTIONS_CHANGE +%04X: "
                        "%08X -> %08X\n",
                        offset,
                        previous,
                        current);
                    WritePlayoffStateLog(message);
                    ++changed;
                }

                if (changed == 0)
                {
                    WritePlayoffStateLog(
                        "[CustomPaths] OPTIONS_CHANGE: no aligned dword "
                        "changes found.\n");
                }

                std::memcpy(
                    gPreviousOptionsSnapshot,
                    bytes,
                    endOffset);
            }

        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] Could not read playoff-state snapshot.\n");
        }
    }

void InstallLive06PlayoffBracketCaptureHook()
    {
        const unsigned int destructorCallSite = 0x005491B3;
        if (!ValidateCallSites(
                &destructorCallSite,
                1,
                0x0051E700))
        {
            OutputDebugStringA(
                "[CustomPaths] NBA Live 06 playoff-bracket capture hook "
                "validation failed.\n");
            return;
        }

        patch::RedirectCall(
            destructorCallSite,
            Live06PlayoffBracketDestructorHook);
        OutputDebugStringA(
            "[CustomPaths] NBA Live 06 playoff-bracket capture hook "
            "installed.\n");
    }

}

namespace practicesetup {
constexpr const char* kIniFile = ".\\main.ini";
void PracticeLog06(const char* message) {
        OutputDebugStringA(message);
        HANDLE file = CreateFileA(".\\PracticeSetup.log", FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, message, static_cast<DWORD>(strlen(message)), &written, nullptr);
            CloseHandle(file);
        }
    }

void PracticeDescribeParticipant06(const char* stage, const void* participant) {
        char message[320] = {};
        if (!IsReadableAddress(participant, 0x14)) {
            sprintf_s(message, sizeof(message),
                "[PracticeSetup] %s participant=%p NOT READABLE\n", stage, participant);
            PracticeLog06(message);
            return;
        }
        const auto* bytes = static_cast<const unsigned char*>(participant);
        int player = -1, jersey = -1, kind = -1;
        memcpy(&player, bytes + 0x08, sizeof(player));
        memcpy(&jersey, bytes + 0x0C, sizeof(jersey));
        memcpy(&kind, bytes + 0x10, sizeof(kind));
        sprintf_s(message, sizeof(message),
            "[PracticeSetup] %s participant=%p playerId=%d jersey=%d kind=%d requestedDbId=%d requestedJersey=%d playerMatch=%s jerseyMatch=%s\n",
            stage, participant, player, jersey, kind, g_practice06.dbId,
            g_practice06.jerseyId,
            player == g_practice06.dbId ? "yes" : "NO",
            g_practice06.jerseyId < 0 ? "native-default" :
                (jersey == g_practice06.jerseyId ? "yes" : "NO"));
        PracticeLog06(message);
    }

void PracticeDumpWords06(const char* label, const void* ptr, size_t byteCount) {
        char line[320] = {};
        sprintf_s(line, sizeof(line),
            "[PracticeSetup] BE snapshot %s base=%p bytes=%u (raw DWORDs; offsets are not verified fields)\n",
            label, ptr, static_cast<unsigned>(byteCount));
        PracticeLog06(line);
        if (!ptr || byteCount > 0x100 || (byteCount % sizeof(uint32_t)) != 0 ||
            !IsReadableAddress(ptr, byteCount)) {
            PracticeLog06("[PracticeSetup] BE snapshot skipped: address not readable as one contiguous region.\n");
            return;
        }
        const auto* data = static_cast<const unsigned char*>(ptr);
        for (size_t offset = 0; offset < byteCount; offset += 16) {
            uint32_t w[4] = {};
            memcpy(w, data + offset, sizeof(w));
            sprintf_s(line, sizeof(line),
                "[PracticeSetup]   +%03X: %08X %08X %08X %08X%s\n",
                static_cast<unsigned>(offset),
                static_cast<unsigned>(w[0]), static_cast<unsigned>(w[1]),
                static_cast<unsigned>(w[2]), static_cast<unsigned>(w[3]),
                (w[0] == static_cast<uint32_t>(g_practice06.dbId) ||
                 w[1] == static_cast<uint32_t>(g_practice06.dbId) ||
                 w[2] == static_cast<uint32_t>(g_practice06.dbId) ||
                 w[3] == static_cast<uint32_t>(g_practice06.dbId))
                    ? "  [requested ID value present; not proof of player binding]" : "");
            PracticeLog06(line);
        }
    }

void PracticeTraceStack06() {
        void* frames[16] = {};
        const USHORT count = CaptureStackBackTrace(0, 16, frames, nullptr);
        char line[200] = {};
        sprintf_s(line, sizeof(line),
            "[PracticeSetup] BE entry stack trace: %u captured addresses (ret addr chain).\n",
            static_cast<unsigned>(count));
        PracticeLog06(line);
        for (USHORT i = 0; i < count; ++i) {
            sprintf_s(line, sizeof(line), "[PracticeSetup]   stack[%u]=%p\n",
                static_cast<unsigned>(i), frames[i]);
            PracticeLog06(line);
        }
    }

void __fastcall PracticeSceneCallHook06(void* sceneObject, void*, int setupArg) {
        // Virtual function at vtable 0x00BC6C38 + 0x0C. Native ends in RET 4.
        // DO NOT treat the FE participant as transferred into gameplay until
        // actor/player-creation handling has been identified and checked.
        ++g_practice06.backendCalls;
        char message[384] = {};
        sprintf_s(message, sizeof(message),
            "[PracticeSetup] BE Practice scene entry #%lu object=%p arg=%d; FE updates=%lu; lastFePlayer=%d lastFeJersey=%d lastFeKind=%d; requestedDbId=%d\n",
            g_practice06.backendCalls, sceneObject, setupArg,
            g_practice06.frontendCalls, g_practice06.lastFePlayer,
            g_practice06.lastFeJersey, g_practice06.lastFeKind,
            g_practice06.dbId);
        PracticeLog06(message);
        if (g_practice06.frontendCalls == 0) {
            PracticeLog06("[PracticeSetup] IMPORTANT: no intercepted FE player setter has executed. "
                "Player override is NOT applied; FE calls may have run earlier or used an alternate path.\n");
        }
        PracticeTraceStack06();
        // The original vtable +4 stores the selection/config pointer at +14;
        // the Practice scene routine reads [sceneObject+14] at 0061DF12.
        if (IsReadableAddress(sceneObject, 0x18)) {
            const auto* p = static_cast<const unsigned char*>(sceneObject);
            const void* participant = nullptr;
            memcpy(&participant, p + 0x14, sizeof(participant));
            PracticeDescribeParticipant06("BE SCENE selection +14", participant);
        }
        PracticeDumpWords06("sceneObject BEFORE", sceneObject, 0x80);
        // setupArg is still passed as its original integer. Treat its bit
        // pattern as an address ONLY for a guarded read-only diagnostic.
        const void* argCandidate = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(static_cast<uint32_t>(setupArg)));
        PracticeDumpWords06("setupArg BEFORE (pointer candidate)", argCandidate, 0x40);
        reinterpret_cast<PracticeSceneFn06>(kPracticeSceneStart06)(
            sceneObject, setupArg);
        PracticeDumpWords06("sceneObject AFTER", sceneObject, 0x80);
        PracticeLog06("[PracticeSetup] BE Practice scene initialization returned. "
            "Gameplay player identity NOT YET VERIFIED.\n");
    }

}

// Boot package origin diagnostics.
using namespace plugin;
namespace autostartdetail {
FILE* g_bootSelectorTraceFile = nullptr;

bool g_tracedLegal = false;

bool g_tracedNbaLogo = false;

bool g_tracedTitle = false;

#if defined(_M_IX86)
bool g_requestCtorTraceHookInstalled = false;

#endif
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

#endif
#if defined(_M_IX86)
#else
bool InstallBootRequestCtorTraceHook06()
    {
        return false;
    }

#endif
}
