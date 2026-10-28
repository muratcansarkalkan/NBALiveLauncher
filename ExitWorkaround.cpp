#include "ExitWorkaround.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace
{
    using ExitProcessFn = VOID(WINAPI*)(UINT);

    ExitProcessFn g_originalExitProcess = nullptr;
    void**        g_exitProcessSlot     = nullptr;
    bool          g_installed           = false;

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

    __declspec(noreturn) VOID WINAPI FastExitProcess(UINT exitCode)
    {
        // NBA Live has already reached its final ExitProcess call at this
        // point. The compatibility workaround intentionally bypasses
        // ExitProcess -> RtlExitUserProcess and its user-mode DLL/CRT/heap
        // teardown, which is the shutdown path implicated in the intermittent
        // stale-process hang.
        //
        // TerminateProcess is intentional here.

        DebugLog(
            "[NBALiveLauncher] FastExit: replacing final ExitProcess "
            "with TerminateProcess.\n");

        if (TerminateProcess(GetCurrentProcess(), exitCode))
        {
            // Terminating the current process should prevent meaningful
            // execution from continuing. Keep a non-returning fallback in
            // case the request is asynchronous on a particular system.
            for (;;)
                Sleep(INFINITE);
        }

        // If TerminateProcess fails, fall back to the game's original
        // ExitProcess behavior instead of leaving the process running.
        DebugLog(
            "[NBALiveLauncher] FastExit: TerminateProcess failed; "
            "falling back to original ExitProcess.\n");

        ExitProcessFn original = g_originalExitProcess;

        if (original &&
            original != &FastExitProcess)
        {
            original(exitCode);
        }

        // Last resort. ExitProcess should not return.
        for (;;)
            Sleep(INFINITE);
    }

    bool IsExitProcessImport(
        std::uint8_t* base,
        const IMAGE_IMPORT_DESCRIPTOR& descriptor,
        std::size_t index,
        IMAGE_THUNK_DATA* originalThunk,
        IMAGE_THUNK_DATA* firstThunk)
    {
        // Preferred path: identify the import by name from OriginalFirstThunk.
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
        // Compare the resolved IAT value against the system export.
        HMODULE kernel32 =
            GetModuleHandleW(L"kernel32.dll");

        if (!kernel32)
            kernel32 =
                LoadLibraryW(L"kernel32.dll");

        if (!kernel32)
            return false;

        FARPROC systemExitProcess =
            GetProcAddress(kernel32, "ExitProcess");

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
            "NBA Live 2005-08 PC compatibility hook must be built x86.");

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
            nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return false;
        }

        // These games are PE32/x86.
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return false;

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
                        *descriptor,
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

                // If something installed us twice, treat it as success.
                if (original == &FastExitProcess)
                {
                    g_exitProcessSlot = slot;
                    g_installed = true;
                    return true;
                }

                g_originalExitProcess = original;
                g_exitProcessSlot = slot;

                if (!WritePointer(
                        slot,
                        reinterpret_cast<void*>(&FastExitProcess)))
                {
                    g_originalExitProcess = nullptr;
                    g_exitProcessSlot = nullptr;
                    return false;
                }

                g_installed = true;

                DebugLog(
                    "[NBALiveLauncher] FastExit compatibility hook "
                    "installed.\n");

                return true;
            }
        }

        DebugLog(
            "[NBALiveLauncher] FastExit: ExitProcess import "
            "was not found in the main executable.\n");

        return false;
    }
}

bool InstallExitWorkaround()
{
    if (g_installed)
        return true;

    return FindAndPatchExitProcessImport();
}

bool UninstallExitWorkaround()
{
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
