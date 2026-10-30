#include "ExitWorkaround.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace
{
    using ExitProcessFn = VOID(WINAPI*)(UINT);

    ExitProcessFn g_originalExitProcess = nullptr;
    void**        g_exitProcessSlot     = nullptr;
    bool          g_installed           = false;

    std::atomic<bool> g_altF4Running{ false };
    HANDLE            g_altF4Thread = nullptr;

    // Alt+F4 first asks the game's foreground window to close normally.
    // If the game ignores WM_CLOSE (or never reaches its normal shutdown)
    // for this long, we fall back to FastTerminateProcess().
    //
    // This preserves EA's own shutdown when possible while still making
    // Alt+F4 reliable from gameplay/loading/menu states.
    constexpr ULONGLONG kAltF4GracePeriodMs = 1500;

    void DebugLog(const char* text)
    {
        OutputDebugStringA(text);
    }

    bool WritePointer(void** slot, void* value)
    {
        if (!slot)
            return false;

        DWORD oldProtect = 0;

        if (!VirtualProtect(
                slot,
                sizeof(void*),
                PAGE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        *slot = value;

        FlushInstructionCache(
            GetCurrentProcess(),
            slot,
            sizeof(void*));

        DWORD ignored = 0;

        VirtualProtect(
            slot,
            sizeof(void*),
            oldProtect,
            &ignored);

        return true;
    }

    bool IsCurrentProcessForeground(HWND* outWindow = nullptr)
    {
        HWND foreground = GetForegroundWindow();

        if (!foreground)
            return false;

        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(
            foreground,
            &foregroundPid);

        if (foregroundPid != GetCurrentProcessId())
            return false;

        if (outWindow)
            *outWindow = foreground;

        return true;
    }

    __declspec(noreturn) VOID WINAPI HookedExitProcess(UINT exitCode)
    {
        DebugLog(
            "[NBALiveLauncher] FastExit: intercepted final ExitProcess.\n");

        FastTerminateProcess(exitCode);
    }

    bool IsExitProcessImport(
        std::uint8_t* base,
        std::size_t index,
        IMAGE_THUNK_DATA* originalThunk,
        IMAGE_THUNK_DATA* firstThunk)
    {
        // Preferred path: identify the import by name.
        if (originalThunk)
        {
            const auto ordinal =
                originalThunk[index].u1.Ordinal;

            if (IMAGE_SNAP_BY_ORDINAL(ordinal))
                return false;

            const auto nameRva =
                originalThunk[index].u1.AddressOfData;

            if (!nameRva)
                return false;

            auto* import =
                reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + nameRva);

            if (!import)
                return false;

            return std::strcmp(
                       reinterpret_cast<const char*>(import->Name),
                       "ExitProcess") == 0;
        }

        // Fallback for an image without OriginalFirstThunk.
        HMODULE kernel32 =
            GetModuleHandleW(L"kernel32.dll");

        if (!kernel32)
            kernel32 =
                LoadLibraryW(L"kernel32.dll");

        if (!kernel32)
            return false;

        FARPROC systemExitProcess =
            GetProcAddress(
                kernel32,
                "ExitProcess");

        if (!systemExitProcess)
            return false;

        return reinterpret_cast<void*>(
                   firstThunk[index].u1.Function) ==
               reinterpret_cast<void*>(systemExitProcess);
    }

    bool FindAndPatchExitProcessImport()
    {
        static_assert(
            sizeof(void*) == 4,
            "NBA Live 2005-08 compatibility hook must be built x86.");

        HMODULE module =
            GetModuleHandleW(nullptr);

        if (!module)
            return false;

        auto* base =
            reinterpret_cast<std::uint8_t*>(module);

        auto* dos =
            reinterpret_cast<IMAGE_DOS_HEADER*>(base);

        if (!dos ||
            dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew <= 0)
        {
            return false;
        }

        auto* nt =
            reinterpret_cast<IMAGE_NT_HEADERS*>(
                base + dos->e_lfanew);

        if (!nt ||
            nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            return false;
        }

        const IMAGE_DATA_DIRECTORY& importDir =
            nt->OptionalHeader.DataDirectory[
                IMAGE_DIRECTORY_ENTRY_IMPORT];

        if (!importDir.VirtualAddress ||
            !importDir.Size)
        {
            return false;
        }

        auto* descriptor =
            reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                base + importDir.VirtualAddress);

        for (; descriptor->Name; ++descriptor)
        {
            if (!descriptor->FirstThunk)
                continue;

            auto* firstThunk =
                reinterpret_cast<IMAGE_THUNK_DATA*>(
                    base + descriptor->FirstThunk);

            IMAGE_THUNK_DATA* originalThunk = nullptr;

            if (descriptor->OriginalFirstThunk)
            {
                originalThunk =
                    reinterpret_cast<IMAGE_THUNK_DATA*>(
                        base + descriptor->OriginalFirstThunk);
            }

            for (std::size_t i = 0;
                 firstThunk[i].u1.Function;
                 ++i)
            {
                if (!IsExitProcessImport(
                        base,
                        i,
                        originalThunk,
                        firstThunk))
                {
                    continue;
                }

                void** slot =
                    reinterpret_cast<void**>(
                        &firstThunk[i].u1.Function);

                ExitProcessFn original =
                    reinterpret_cast<ExitProcessFn>(*slot);

                if (!original)
                    return false;

                // Already installed.
                if (original == &HookedExitProcess)
                {
                    g_exitProcessSlot = slot;
                    g_installed = true;
                    return true;
                }

                g_originalExitProcess = original;
                g_exitProcessSlot = slot;

                if (!WritePointer(
                        slot,
                        reinterpret_cast<void*>(&HookedExitProcess)))
                {
                    g_originalExitProcess = nullptr;
                    g_exitProcessSlot = nullptr;
                    return false;
                }

                g_installed = true;

                DebugLog(
                    "[NBALiveLauncher] FastExit compatibility hook installed.\n");

                return true;
            }
        }

        DebugLog(
            "[NBALiveLauncher] FastExit: ExitProcess import was not found "
            "in the main executable.\n");

        return false;
    }

    DWORD WINAPI AltF4WatcherThread(LPVOID)
    {
        bool previousPressed = false;
        bool closePending = false;
        ULONGLONG closeRequestedAt = 0;
        HWND closeWindow = nullptr;

        while (g_altF4Running.load(std::memory_order_relaxed))
        {
            const bool altDown =
                (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

            const bool f4Down =
                (GetAsyncKeyState(VK_F4) & 0x8000) != 0;

            const bool pressed =
                altDown && f4Down;

            if (pressed && !previousPressed && !closePending)
            {
                HWND foreground = nullptr;

                // Do not treat Alt+F4 pressed in another application as a
                // command to kill NBA Live while it is in the background.
                if (IsCurrentProcessForeground(&foreground))
                {
                    DebugLog(
                        "[NBALiveLauncher] Alt+F4 detected. "
                        "Requesting normal game-window close first.\n");

                    closePending = true;
                    closeRequestedAt = GetTickCount64();
                    closeWindow = foreground;

                    // Give NBA Live the opportunity to perform its own
                    // normal shutdown. If it reaches ExitProcess, our IAT
                    // hook will still replace only the final Windows teardown.
                    PostMessageW(
                        closeWindow,
                        WM_CLOSE,
                        0,
                        0);
                }
            }

            previousPressed = pressed;

            if (closePending)
            {
                const ULONGLONG elapsed =
                    GetTickCount64() - closeRequestedAt;

                if (elapsed >= kAltF4GracePeriodMs)
                {
                    // If the process is still here after the grace period,
                    // WM_CLOSE did not complete shutdown. Alt+F4 is defined
                    // as "quit now", so use the shared force-exit path.
                    DebugLog(
                        "[NBALiveLauncher] Alt+F4 normal-close grace period "
                        "expired. Forcing process termination.\n");

                    FastTerminateProcess(0);
                }
            }

            Sleep(10);
        }

        return 0;
    }
}

[[noreturn]] void FastTerminateProcess(UINT exitCode)
{
    DebugLog(
        "[NBALiveLauncher] FastExit: TerminateProcess(GetCurrentProcess()).\n");

    if (TerminateProcess(
            GetCurrentProcess(),
            exitCode))
    {
        // Terminating the current process should prevent meaningful
        // execution from continuing. Keep this function non-returning even
        // if termination completion is asynchronous on a particular system.
        for (;;)
            Sleep(INFINITE);
    }

    DebugLog(
        "[NBALiveLauncher] FastExit: TerminateProcess failed; "
        "falling back to original ExitProcess.\n");

    ExitProcessFn original =
        g_originalExitProcess;

    if (original &&
        original != &HookedExitProcess)
    {
        original(exitCode);
    }

    // Ultimate fallback if both termination paths fail unexpectedly.
    for (;;)
        Sleep(INFINITE);
}

bool StartAltF4ExitWatcher()
{
    if (g_altF4Running.exchange(true))
        return true;

    g_altF4Thread =
        CreateThread(
            nullptr,
            0,
            AltF4WatcherThread,
            nullptr,
            0,
            nullptr);

    if (!g_altF4Thread)
    {
        g_altF4Running.store(false);
        return false;
    }

    DebugLog(
        "[NBALiveLauncher] Alt+F4 exit watcher started.\n");

    return true;
}

void StopAltF4ExitWatcher()
{
    if (!g_altF4Running.exchange(false))
        return;

    HANDLE thread = g_altF4Thread;
    g_altF4Thread = nullptr;

    if (thread)
    {
        // The watcher sleeps only 10 ms per loop, so this should return
        // immediately under normal circumstances.
        WaitForSingleObject(
            thread,
            1000);

        CloseHandle(thread);
    }

    DebugLog(
        "[NBALiveLauncher] Alt+F4 exit watcher stopped.\n");
}

bool IsAltF4ExitWatcherRunning()
{
    return g_altF4Running.load();
}

bool InstallExitWorkaround()
{
    bool hookInstalled = g_installed;

    if (!hookInstalled)
        hookInstalled = FindAndPatchExitProcessImport();

    if (!hookInstalled)
        return false;

    // Drop-in behavior: existing callers that already invoke only
    // InstallExitWorkaround() automatically gain Alt+F4 support.
    if (!StartAltF4ExitWatcher())
    {
        DebugLog(
            "[NBALiveLauncher] FastExit hook installed, but Alt+F4 watcher "
            "could not be started.\n");
    }

    return true;
}

bool UninstallExitWorkaround()
{
    StopAltF4ExitWatcher();

    if (!g_installed)
        return true;

    if (!g_exitProcessSlot ||
        !g_originalExitProcess)
    {
        return false;
    }

    if (!WritePointer(
            g_exitProcessSlot,
            reinterpret_cast<void*>(g_originalExitProcess)))
    {
        return false;
    }

    g_installed = false;
    g_exitProcessSlot = nullptr;
    g_originalExitProcess = nullptr;

    DebugLog(
        "[NBALiveLauncher] FastExit compatibility hook removed.\n");

    return true;
}

bool IsExitWorkaroundInstalled()
{
    return g_installed;
}
