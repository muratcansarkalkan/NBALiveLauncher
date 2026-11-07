#include "plugin-std.h"
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>
#include <intrin.h>
#include "LoadingOptimization.h"

using namespace plugin;
namespace LiveLoading {
namespace {
BeginWaitObserver g_beginWait = nullptr;
FinishWaitObserver g_finishWait = nullptr;
}
__declspec(thread) unsigned int shortWaitCount = 0;
bool fileWaitHook = false, fastFileWait = false;
HANDLE fileCompletionEvent = nullptr;
bool completionSignalHook = false;
typedef BOOL (WINAPI *SetEventFn)(HANDLE);
SetEventFn nativeSetEvent = nullptr;
struct FileWaitBuild {
    const char* name;
    uintptr_t waitPush, delayHelper, sleepExIat, signalCall, setEventIat;
};
const FileWaitBuild fileWaitBuilds[] = {
    { "2005", 0x006C5B01, 0x004292B3, 0x00CCED6C, 0x006C574C, 0x00CCED84 },
    { "06",   0x006DC59B, 0x00429642, 0x00D30D88, 0x006DC196, 0x00D30DAC },
    { "07",   0x007FF170, 0x00428E82, 0x00D49D90, 0x007FED6B, 0x00D49DB4 },
    { "08",   0x0082E18F, 0x0042B6B2, 0x00E07D9C, 0x0082DD8A, 0x00E07DC0 }
};
const FileWaitBuild* waitBuild = nullptr;
uintptr_t nativeDelayHelper = 0x00429642;

// Shared signature validation is separate from memory access and patching.
// This also lets the offline check use the actual bytes from all four EXEs.
bool ValidateWaitBytes(const FileWaitBuild& build, const BYTE* wait, const BYTE* helper) {
    BYTE expectedWait[] = { 0x6A, 0x01, 0xE8, 0, 0, 0, 0, 0x59 };
    const int32_t relative = static_cast<int32_t>(build.delayHelper - (build.waitPush + 7));
    std::memcpy(expectedWait + 3, &relative, 4);
    BYTE expectedHelper[] = { 0x55, 0x8B, 0xEC, 0x6A, 0x01, 0xFF, 0x75, 0x08,
        0xFF, 0x15, 0, 0, 0, 0, 0x5D, 0xC3 };
    const DWORD iat = static_cast<DWORD>(build.sleepExIat);
    std::memcpy(expectedHelper + 10, &iat, 4);
    return !std::memcmp(wait, expectedWait, sizeof(expectedWait)) &&
        !std::memcmp(helper, expectedHelper, sizeof(expectedHelper));
}
bool ValidateSignalBytes(const FileWaitBuild& build, const BYTE* bytes) {
    BYTE expected[] = { 0xFF, 0x76, 0x78, 0xFF, 0x15, 0, 0, 0, 0,
        0x83, 0x7E, 0x08, 0x00 };
    const DWORD iat = static_cast<DWORD>(build.setEventIat);
    std::memcpy(expected + 5, &iat, 4);
    return !std::memcmp(bytes, expected, sizeof(expected));
}
const FileWaitBuild* DetectFileWaitBuild() {
#if defined(_M_IX86)
    const uintptr_t ep = FM::GetEntryPoint();
    if (ep == 0xCD8005) return &fileWaitBuilds[0];
    if (ep == 0x40109F) {
        if (plugin::patch::GetFloat(0xBD832C) == 1.3333334f) return &fileWaitBuilds[1];
        if (plugin::patch::GetFloat(0xBBBC3C) == 1.3333334f) return &fileWaitBuilds[2];
        if (plugin::patch::GetFloat(0xC3DF84) == 1.3333334f) return &fileWaitBuilds[3];
    }
#endif
    return nullptr;
}

BOOL WINAPI SignalFileCompletion(HANDLE nativeEvent) {
    // Invoke the original SetEvent first and preserve its result/LastError.
    // The plugin event has independent state; never reset the engine's event.
    const BOOL result = nativeSetEvent(nativeEvent);
    const DWORD error = GetLastError();
    if (fileCompletionEvent) SetEvent(fileCompletionEvent);
    SetLastError(error);
    return result;
}

bool InstallCompletionSignal() {
#if defined(_M_IX86)
    if (completionSignalHook) return true;
    // Each verified worker moves the request to its completion list, invokes
    // its callback, then signals context+78 at the selected signalCall.
    // The existing HANDLE argument and stdcall cleanup are unchanged.
    if (!waitBuild) return false;
    BYTE* site = reinterpret_cast<BYTE*>(waitBuild->signalCall);
    __try {
        if (!ValidateSignalBytes(*waitBuild, site - 3)) return false;
        nativeSetEvent = *reinterpret_cast<SetEventFn*>(waitBuild->setEventIat);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!nativeSetEvent) return false;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    DWORD protect = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &protect)) {
        CloseHandle(event); return false;
    }
    fileCompletionEvent = event;
    site[0] = 0xE8;
    *reinterpret_cast<int32_t*>(site + 1) = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&SignalFileCompletion) - (waitBuild->signalCall + 5));
    site[5] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(site, 6, protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    completionSignalHook = true;
    return true;
#else
    return false;
#endif
}

void __cdecl FileCompletionDelay(DWORD milliseconds) {
    DWORD actual = milliseconds;
    const bool eventWait = fastFileWait && completionSignalHook && milliseconds == 1;
    if (fastFileWait && !eventWait && milliseconds == 1) {
        // A bounded yield sequence: never replace long waits with a busy loop.
        // TLS avoids cross-thread interference. The original native loop still
        // checks the queue and pumps callbacks between every call here.
        shortWaitCount = (shortWaitCount + 1) & 63;
        actual = shortWaitCount ? 0 : 1;
    }
    const DWORD incomingError = GetLastError();
    const WaitSample sample = g_beginWait ? g_beginWait() : WaitSample{};
    SetLastError(incomingError);
    DWORD waitResult = 0;
    if (eventWait) {
        // Auto-reset retains a completion arriving just before this wait.
        // A completion for a different request is only a wakeup: the native
        // loop rechecks its own request before deciding that it has finished.
        waitResult = WaitForSingleObjectEx(fileCompletionEvent, 1, TRUE);
    }
    if (!eventWait || waitResult == WAIT_FAILED) {
        if (eventWait) SetLastError(incomingError);
        // Original helper remains the OFF/installation-failure fallback.
        reinterpret_cast<void (__cdecl *)(DWORD)>(nativeDelayHelper)(actual);
    }
    const DWORD error = GetLastError();
    if (g_finishWait) g_finishWait(sample, actual, eventWait, waitResult);
    SetLastError(error);
}

bool InstallFileCompletionDelay(bool fast) {
#if defined(_M_IX86)
    if (fileWaitHook) return true;
    waitBuild = DetectFileWaitBuild();
    if (!waitBuild) return false;
    // Verify push 1 / call delayHelper / pop ecx plus the complete helper.
    // No instruction relocation, return-address rewriting, or global API hook.
    BYTE* site = reinterpret_cast<BYTE*>(waitBuild->waitPush);
    __try {
        if (!ValidateWaitBytes(*waitBuild, site, reinterpret_cast<const BYTE*>(waitBuild->delayHelper)))
            return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    DWORD protect = 0;
    if (!VirtualProtect(site + 2, 5, PAGE_EXECUTE_READWRITE, &protect)) return false;
    fastFileWait = fast;
    nativeDelayHelper = waitBuild->delayHelper;
    if (fast) InstallCompletionSignal();
    *reinterpret_cast<int32_t*>(site + 3) = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&FileCompletionDelay) - (waitBuild->waitPush + 7));
    DWORD ignored = 0;
    VirtualProtect(site + 2, 5, protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site + 2, 5);
    fileWaitHook = true;
    OutputDebugStringA(fast ? "[Loading] FAST_FILE_WAIT installed.\n" :
        "[Loading] Native file-completion wait timing installed (delay unchanged).\n");
    return true;
#else
    return false;
#endif
}

void WriteFileWaitInstallStatus(bool installed) {
    // One small startup report; does not enable console/file tracing or run
    // during individual file operations. FAST_FILE_WAIT works without profiling.
    char directory[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    char* slash = std::strrchr(directory, '\\');
    if (!slash) return;
    *slash = 0;
    if (std::strlen(directory) > 190) return;
    strcat_s(directory, "\\logs");
    if (!CreateDirectoryA(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    char path[MAX_PATH];
    sprintf_s(path, "%s\\loading_wait_%s.log", directory, waitBuild ? waitBuild->name : "unsupported");
    FILE* out = nullptr;
    if (!fopen_s(&out, path, "w") && out) {
        SYSTEMTIME t; GetLocalTime(&t);
        std::fprintf(out, "File-wait initialization %04u-%02u-%02u %02u:%02u:%02u\n",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::fprintf(out, "game=%s requested_FAST_FILE_WAIT=1 installed=%d strategy=%s completion-signal-hook=%d\n",
            waitBuild ? waitBuild->name : "unsupported", installed,
            !installed ? "skipped" : completionSignalHook ? "completion-event" : "bounded-yields", completionSignalHook);
        if (waitBuild) std::fprintf(out, "wait_call=%08lX helper=%08lX signal_call=%08lX\n",
            static_cast<unsigned long>(waitBuild->waitPush + 2),
            static_cast<unsigned long>(waitBuild->delayHelper), static_cast<unsigned long>(waitBuild->signalCall));
        std::fprintf(out, "Startup status only; not a loading-duration capture. Native callbacks, completion checks and events remain in place.\n");
        std::fclose(out);
    }
}


void SetWaitObserver(BeginWaitObserver begin, FinishWaitObserver finish) {
    g_beginWait = begin;
    g_finishWait = finish;
}
bool IsInstalled() { return fileWaitHook; }
bool IsFastWaitEnabled() { return fastFileWait; }
bool HasCompletionSignal() { return completionSignalHook; }
}
