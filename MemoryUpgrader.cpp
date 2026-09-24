#include "plugin-std.h"

#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>

namespace
{
    bool gDebugLoggingEnabled = false;

    void InitializeDebugLogging()
    {
        char exePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return;

        char* slash = std::strrchr(exePath, '\\');
        if (!slash)
            return;

        *(slash + 1) = '\0';

        char iniPath[MAX_PATH] = {};
        std::snprintf(iniPath, sizeof(iniPath), "%smain.ini", exePath);

        gDebugLoggingEnabled =
            GetPrivateProfileIntA("DEBUG", "CONSOLE", 0, iniPath) != 0;

        if (!gDebugLoggingEnabled)
            return;

        char logsPath[MAX_PATH] = {};
        std::snprintf(logsPath, sizeof(logsPath), "%slogs", exePath);
        CreateDirectoryA(logsPath, nullptr);
    }

    bool BuildDebugLogPath(
        char* outPath,
        size_t outSize,
        const char* fileName)
    {
        if (!outPath || !outSize || !fileName || !gDebugLoggingEnabled)
        {
            if (outPath && outSize)
                outPath[0] = '\0';
            return false;
        }

        char exePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (!length || length >= MAX_PATH)
        {
            outPath[0] = '\0';
            return false;
        }

        char* slash = std::strrchr(exePath, '\\');
        if (!slash)
        {
            outPath[0] = '\0';
            return false;
        }

        *(slash + 1) = '\0';

        const int written =
            std::snprintf(outPath, outSize, "%slogs\\%s", exePath, fileName);

        if (written < 0 || static_cast<size_t>(written) >= outSize)
        {
            outPath[0] = '\0';
            return false;
        }

        return true;
    }

    void MemoryLog(const char* text)
    {
        if (!gDebugLoggingEnabled || !text)
            return;

        char logPath[MAX_PATH] = {};
        if (BuildDebugLogPath(
                logPath,
                sizeof(logPath),
                "MemoryUpgrader.log"))
        {
            FILE* f = nullptr;
            if (fopen_s(&f, logPath, "a") == 0 && f)
            {
                fputs(text, f);
                fclose(f);
            }
        }

        OutputDebugStringA(text);
    }

    bool PatchBytes(std::uintptr_t address, const void* data, std::size_t size)
    {
        DWORD oldProtect = 0;

        if (!VirtualProtect(
                reinterpret_cast<void*>(address),
                size,
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        std::memcpy(
            reinterpret_cast<void*>(address),
            data,
            size);

        FlushInstructionCache(
            GetCurrentProcess(),
            reinterpret_cast<void*>(address),
            size);

        DWORD ignored = 0;
        VirtualProtect(
            reinterpret_cast<void*>(address),
            size,
            oldProtect,
            &ignored);

        return true;
    }

    bool Matches(
        std::uintptr_t address,
        const unsigned char* bytes,
        std::size_t size)
    {
        return std::memcmp(
            reinterpret_cast<const void*>(address),
            bytes,
            size) == 0;
    }


    // =====================================================================
    // NBA Live 2005
    // =====================================================================

    constexpr std::uintptr_t k05PageSizeAddress   = 0x00BCC9EC;
    constexpr std::uintptr_t k05FixedArenaAddress = 0x00BCC9F0;
    constexpr std::uintptr_t k05RealMemoryAddress = 0x00BCC9F4;

    constexpr DWORD k05ExpectedPageSize   = 0x00002000; // 8 KiB
    constexpr DWORD k05OriginalFixedArena = 0x01040000; // 16.25 MiB
    constexpr DWORD k05ExpandedFixedArena = 0x02080000; // 32.5 MiB
    constexpr DWORD k05OriginalRealMemory = 0x04600000; // 70 MiB
    constexpr DWORD k05ExpandedRealMemory = 0x08C00000; // 140 MiB

    bool InstallLive2005MemoryUpgrade()
    {
        DWORD pageSize = 0;
        DWORD fixedArena = 0;
        DWORD realMemory = 0;

        __try
        {
            pageSize =
                *reinterpret_cast<const DWORD*>(k05PageSizeAddress);

            fixedArena =
                *reinterpret_cast<const DWORD*>(k05FixedArenaAddress);

            realMemory =
                *reinterpret_cast<const DWORD*>(k05RealMemoryAddress);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 2005 memory config read failed.\n");
            return false;
        }

        if (pageSize != k05ExpectedPageSize ||
            fixedArena != k05OriginalFixedArena ||
            realMemory != k05OriginalRealMemory)
        {
            char buf[256];
            sprintf_s(
                buf,
                "[MemoryUpgrader] NBA Live 2005 memory verification failed: "
                "page=%08X fixedArena=%08X realMemory=%08X\n",
                pageSize,
                fixedArena,
                realMemory);
            MemoryLog(buf);
            return false;
        }

        if (!PatchBytes(
                k05FixedArenaAddress,
                &k05ExpandedFixedArena,
                sizeof(k05ExpandedFixedArena)))
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 2005 fixed-page arena patch failed.\n");
            return false;
        }

        if (!PatchBytes(
                k05RealMemoryAddress,
                &k05ExpandedRealMemory,
                sizeof(k05ExpandedRealMemory)))
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 2005 RealMemory arena patch failed.\n");
            return false;
        }

        {
            char buf[256];
            sprintf_s(
                buf,
                "[MemoryUpgrader] NBA Live 2005 pre-construction config: "
                "fixedArena=%08X page=%08X realMemory=%08X\n",
                *reinterpret_cast<const DWORD*>(k05FixedArenaAddress),
                *reinterpret_cast<const DWORD*>(k05PageSizeAddress),
                *reinterpret_cast<const DWORD*>(k05RealMemoryAddress));
            MemoryLog(buf);
        }

        constexpr std::uintptr_t kStack       = 0x007C186D;
        constexpr std::uintptr_t kWrite       = 0x007C193E;
        constexpr std::uintptr_t kReadNext    = 0x007C1AC6;
        constexpr std::uintptr_t kRead        = 0x007C1AD0;
        constexpr std::uintptr_t kReadLastSub = 0x007C1C11;
        constexpr std::uintptr_t kReadLast    = 0x007C1C6C;

        const unsigned char oStack[6] =
        {
            0x81, 0xEC, 0xE8, 0x01, 0x00, 0x00
        };

        const unsigned char oWrite[7] =
        {
            0x89, 0x84, 0x8D, 0x20, 0xFE, 0xFF, 0xFF
        };

        const unsigned char oReadNext[7] =
        {
            0x8B, 0xBC, 0x85, 0x24, 0xFE, 0xFF, 0xFF
        };

        const unsigned char oRead[7] =
        {
            0x8B, 0x8C, 0x85, 0x20, 0xFE, 0xFF, 0xFF
        };

        const unsigned char oReadLastSub[7] =
        {
            0x2B, 0xBC, 0x05, 0x1C, 0xFE, 0xFF, 0xFF
        };

        const unsigned char oReadLast[7] =
        {
            0x8B, 0x8C, 0x05, 0x1C, 0xFE, 0xFF, 0xFF
        };

        if (!Matches(kStack,       oStack,       sizeof(oStack)) ||
            !Matches(kWrite,       oWrite,       sizeof(oWrite)) ||
            !Matches(kReadNext,    oReadNext,    sizeof(oReadNext)) ||
            !Matches(kRead,        oRead,        sizeof(oRead)) ||
            !Matches(kReadLastSub, oReadLastSub, sizeof(oReadLastSub)) ||
            !Matches(kReadLast,    oReadLast,    sizeof(oReadLast)))
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 2005 verification failed.\n");

            return false;
        }

        // Raise the known temporary-list capacity from 118 to 512 entries.

        const unsigned char pStack[6] =
        {
            0x81, 0xEC, 0x10, 0x08, 0x00, 0x00
        };

        const unsigned char pWrite[7] =
        {
            0x89, 0x84, 0x8D, 0xF8, 0xF7, 0xFF, 0xFF
        };

        const unsigned char pReadNext[7] =
        {
            0x8B, 0xBC, 0x85, 0xFC, 0xF7, 0xFF, 0xFF
        };

        const unsigned char pRead[7] =
        {
            0x8B, 0x8C, 0x85, 0xF8, 0xF7, 0xFF, 0xFF
        };

        const unsigned char pReadLastSub[7] =
        {
            0x2B, 0xBC, 0x05, 0xF4, 0xF7, 0xFF, 0xFF
        };

        const unsigned char pReadLast[7] =
        {
            0x8B, 0x8C, 0x05, 0xF4, 0xF7, 0xFF, 0xFF
        };

        const bool ok =
            PatchBytes(kStack,       pStack,       sizeof(pStack)) &&
            PatchBytes(kWrite,       pWrite,       sizeof(pWrite)) &&
            PatchBytes(kReadNext,    pReadNext,    sizeof(pReadNext)) &&
            PatchBytes(kRead,        pRead,        sizeof(pRead)) &&
            PatchBytes(kReadLastSub, pReadLastSub, sizeof(pReadLastSub)) &&
            PatchBytes(kReadLast,    pReadLast,    sizeof(pReadLast));

        if (ok)
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 2005 memory expansion active: "
                "RealMemory 70 MiB -> 140 MiB; "
                "MasterFixedPageAllocator 16.25 MiB -> 32.5 MiB; "
                "temporary resource list 118 -> 512 entries.\n");
        }
        else
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 2005 temporary-list patch failed.\n");
        }

        return ok;
    }


    // =====================================================================
    // NBA Live 06
    // =====================================================================

    constexpr std::uintptr_t k06PageSizeAddress   = 0x00C3BB6C;
    constexpr std::uintptr_t k06ArenaSizeAddress  = 0x00C3BB70;
    constexpr std::uintptr_t k06RealMemoryAddress = 0x00C3BB74;

    constexpr DWORD k06ExpectedPageSize   = 0x00002000; // 8 KiB
    constexpr DWORD k06OriginalArena      = 0x01040000; // 16.25 MiB
    constexpr DWORD k06ExpandedArena      = 0x02080000; // 32.5 MiB
    constexpr DWORD k06OriginalRealMemory = 0x04600000; // 70 MiB
    constexpr DWORD k06ExpandedRealMemory = 0x08C00000; // 140 MiB

    constexpr std::uintptr_t k06AcquireAllocatorAddress = 0x004176F0;
    constexpr std::uintptr_t k06MasterAllocatorCall     = 0x00417B1A;

    using AcquireAllocatorFn =
        void* (__cdecl*)(
            DWORD type,
            void* baseAllocator,
            const char* name,
            DWORD* arenaSize,
            DWORD* pageSize,
            void* suppliedBuffer);

    AcquireAllocatorFn g_AcquireAllocator =
        reinterpret_cast<AcquireAllocatorFn>(k06AcquireAllocatorAddress);

    void* __cdecl AcquireMasterAllocatorHook(
        DWORD type,
        void* baseAllocator,
        const char* name,
        DWORD* arenaSize,
        DWORD* pageSize,
        void* suppliedBuffer)
    {
        void* result = g_AcquireAllocator(
            type,
            baseAllocator,
            name,
            arenaSize,
            pageSize,
            suppliedBuffer);

        if (result &&
            type == 4 &&
            name &&
            std::strcmp(name, "MasterFixedPageAllocator") == 0)
        {
            DWORD liveArenaSize = 0;
            DWORD livePageSize = 0;

            __try
            {
                liveArenaSize =
                    *reinterpret_cast<DWORD*>(
                        reinterpret_cast<std::uintptr_t>(result) + 0x33C);

                livePageSize =
                    *reinterpret_cast<DWORD*>(
                        reinterpret_cast<std::uintptr_t>(result) + 0x340);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                liveArenaSize = 0;
                livePageSize = 0;
            }

            char buf[320];

            sprintf_s(
                buf,
                "[MemoryUpgrader] MasterFixedPageAllocator CREATED: "
                "object=%p requestedArena=%08X requestedPage=%08X "
                "liveArena=%08X livePage=%08X\n",
                result,
                arenaSize ? *arenaSize : 0,
                pageSize ? *pageSize : 0,
                liveArenaSize,
                livePageSize);

            MemoryLog(buf);
        }

        return result;
    }

    bool InstallLive06MemoryUpgrade()
    {
        DWORD pageSize = 0;
        DWORD arenaSize = 0;
        DWORD realMemorySize = 0;

        __try
        {
            pageSize =
                *reinterpret_cast<const DWORD*>(k06PageSizeAddress);

            arenaSize =
                *reinterpret_cast<const DWORD*>(k06ArenaSizeAddress);

            realMemorySize =
                *reinterpret_cast<const DWORD*>(k06RealMemoryAddress);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 06 allocator config read failed.\n");

            return false;
        }

        if (pageSize != k06ExpectedPageSize ||
            arenaSize != k06OriginalArena ||
            realMemorySize != k06OriginalRealMemory)
        {
            char buf[256];
            sprintf_s(
                buf,
                "[MemoryUpgrader] NBA Live 06 allocator verification failed: "
                "page=%08X fixedArena=%08X realMemory=%08X\n",
                pageSize,
                arenaSize,
                realMemorySize);
            MemoryLog(buf);

            return false;
        }

        if (!PatchBytes(
                k06ArenaSizeAddress,
                &k06ExpandedArena,
                sizeof(k06ExpandedArena)))
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 06 fixed-page arena patch failed.\n");

            return false;
        }

        if (!PatchBytes(
                k06RealMemoryAddress,
                &k06ExpandedRealMemory,
                sizeof(k06ExpandedRealMemory)))
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 06 RealMemory arena patch failed.\n");

            return false;
        }

        {
            char buf[256];
            sprintf_s(
                buf,
                "[MemoryUpgrader] 06 pre-construction config: "
                "fixedArena=%08X page=%08X realMemory=%08X\n",
                *reinterpret_cast<const DWORD*>(k06ArenaSizeAddress),
                *reinterpret_cast<const DWORD*>(k06PageSizeAddress),
                *reinterpret_cast<const DWORD*>(k06RealMemoryAddress));
            MemoryLog(buf);
        }

        // Hook only the specific MasterFixedPageAllocator creation call.
        plugin::patch::RedirectCall(
            k06MasterAllocatorCall,
            AcquireMasterAllocatorHook);

        MemoryLog(
            "[MemoryUpgrader] NBA Live 06 memory expansion active: "
            "RealMemory 70 MiB -> 140 MiB; "
            "MasterFixedPageAllocator 16.25 MiB -> 32.5 MiB.\n");

        return true;
    }

    // =====================================================================
    // NBA Live 07
    //
    // Services::Memory::Initialize @ 00415650
    //
    //   C52B2C = 00002000  page size = 8 KiB
    //   C52B30 = 00D20000  fixed-page arena = 13.125 MiB
    //   C52B34 = 04600000  RealMemory arena = 70 MiB
    //
    // The RealMemory init path matches 2005/06:
    //   mMemOpts.size       <- C52B34
    //   mMemOpts.backing    <- malloc(size)
    //   Scf::RealMemory::Init(&mMemOpts)
    //
    // Preserve the game's own ratio by doubling both arenas.
    // =====================================================================

    constexpr std::uintptr_t k07PageSizeAddress   = 0x00C52B2C;
    constexpr std::uintptr_t k07FixedArenaAddress = 0x00C52B30;
    constexpr std::uintptr_t k07RealMemoryAddress = 0x00C52B34;

    constexpr DWORD k07ExpectedPageSize   = 0x00002000; // 8 KiB
    constexpr DWORD k07OriginalFixedArena = 0x00D20000; // 13.125 MiB
    constexpr DWORD k07ExpandedFixedArena = 0x01A40000; // 26.25 MiB
    constexpr DWORD k07OriginalRealMemory = 0x04600000; // 70 MiB
    constexpr DWORD k07ExpandedRealMemory = 0x08C00000; // 140 MiB

    bool InstallLive07MemoryUpgrade()
    {
        DWORD pageSize = 0;
        DWORD fixedArena = 0;
        DWORD realMemory = 0;

        __try
        {
            pageSize =
                *reinterpret_cast<const DWORD*>(k07PageSizeAddress);

            fixedArena =
                *reinterpret_cast<const DWORD*>(k07FixedArenaAddress);

            realMemory =
                *reinterpret_cast<const DWORD*>(k07RealMemoryAddress);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 07 memory config read failed.\n");
            return false;
        }

        if (pageSize != k07ExpectedPageSize ||
            fixedArena != k07OriginalFixedArena ||
            realMemory != k07OriginalRealMemory)
        {
            char buf[256];
            sprintf_s(
                buf,
                "[MemoryUpgrader] NBA Live 07 memory verification failed: "
                "page=%08X fixedArena=%08X realMemory=%08X\n",
                pageSize,
                fixedArena,
                realMemory);
            MemoryLog(buf);
            return false;
        }

        if (!PatchBytes(
                k07FixedArenaAddress,
                &k07ExpandedFixedArena,
                sizeof(k07ExpandedFixedArena)))
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 07 fixed-page arena patch failed.\n");
            return false;
        }

        if (!PatchBytes(
                k07RealMemoryAddress,
                &k07ExpandedRealMemory,
                sizeof(k07ExpandedRealMemory)))
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 07 RealMemory arena patch failed.\n");
            return false;
        }

        char buf[320];
        sprintf_s(
            buf,
            "[MemoryUpgrader] NBA Live 07 memory expansion active: "
            "RealMemory 70 MiB -> 140 MiB; "
            "MasterFixedPageAllocator 13.125 MiB -> 26.25 MiB; "
            "page=%08X fixedArena=%08X realMemory=%08X\n",
            *reinterpret_cast<const DWORD*>(k07PageSizeAddress),
            *reinterpret_cast<const DWORD*>(k07FixedArenaAddress),
            *reinterpret_cast<const DWORD*>(k07RealMemoryAddress));

        MemoryLog(buf);
        return true;
    }


    // =====================================================================
    // NBA Live 08
    //
    // Services::Memory::Initialize @ 00415B40
    //
    //   D0FAF4 = 00002000  page size = 8 KiB
    //   D0FAF8 = 00D20000  fixed-page arena = 13.125 MiB
    //   D0FAFC = 04600000  RealMemory arena = 70 MiB
    //
    // RealMemory thunk:
    //   004536E0 -> 00E2A936
    //
    // RealMemory body confirms:
    //   [opts+00] = arena size
    //   [opts+14] = supplied backing buffer
    //
    // Preserve the game's original sizing ratio by doubling both arenas.
    // =====================================================================

    constexpr std::uintptr_t k08PageSizeAddress   = 0x00D0FAF4;
    constexpr std::uintptr_t k08FixedArenaAddress = 0x00D0FAF8;
    constexpr std::uintptr_t k08RealMemoryAddress = 0x00D0FAFC;

    constexpr DWORD k08ExpectedPageSize   = 0x00002000; // 8 KiB
    constexpr DWORD k08OriginalFixedArena = 0x00D20000; // 13.125 MiB
    constexpr DWORD k08ExpandedFixedArena = 0x01A40000; // 26.25 MiB
    constexpr DWORD k08OriginalRealMemory = 0x04600000; // 70 MiB
    constexpr DWORD k08ExpandedRealMemory = 0x08C00000; // 140 MiB

    bool InstallLive08MemoryUpgrade()
    {
        DWORD pageSize = 0;
        DWORD fixedArena = 0;
        DWORD realMemory = 0;

        __try
        {
            pageSize =
                *reinterpret_cast<const DWORD*>(k08PageSizeAddress);

            fixedArena =
                *reinterpret_cast<const DWORD*>(k08FixedArenaAddress);

            realMemory =
                *reinterpret_cast<const DWORD*>(k08RealMemoryAddress);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 08 memory config read failed.\n");
            return false;
        }

        if (pageSize != k08ExpectedPageSize ||
            fixedArena != k08OriginalFixedArena ||
            realMemory != k08OriginalRealMemory)
        {
            char buf[256];
            sprintf_s(
                buf,
                "[MemoryUpgrader] NBA Live 08 memory verification failed: "
                "page=%08X fixedArena=%08X realMemory=%08X\n",
                pageSize,
                fixedArena,
                realMemory);
            MemoryLog(buf);
            return false;
        }

        if (!PatchBytes(
                k08FixedArenaAddress,
                &k08ExpandedFixedArena,
                sizeof(k08ExpandedFixedArena)))
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 08 fixed-page arena patch failed.\n");
            return false;
        }

        if (!PatchBytes(
                k08RealMemoryAddress,
                &k08ExpandedRealMemory,
                sizeof(k08ExpandedRealMemory)))
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 08 RealMemory arena patch failed.\n");
            return false;
        }

        char buf[320];
        sprintf_s(
            buf,
            "[MemoryUpgrader] NBA Live 08 memory expansion active: "
            "RealMemory 70 MiB -> 140 MiB; "
            "MasterFixedPageAllocator 13.125 MiB -> 26.25 MiB; "
            "page=%08X fixedArena=%08X realMemory=%08X\n",
            *reinterpret_cast<const DWORD*>(k08PageSizeAddress),
            *reinterpret_cast<const DWORD*>(k08FixedArenaAddress),
            *reinterpret_cast<const DWORD*>(k08RealMemoryAddress));

        MemoryLog(buf);
        return true;
    }

}


// =====================================================================
// Public entry point
// =====================================================================

void InitializeMemoryUpgrader()
{
    InitializeDebugLogging();

    const std::uintptr_t ep = FM::GetEntryPoint();

    if (ep == 0xCD8005)
    {
        InstallLive2005MemoryUpgrade();
        return;
    }

    if (ep == 0x40109F &&
        plugin::patch::GetFloat(0xBD832C) == 1.3333334f)
    {
        InstallLive06MemoryUpgrade();
        return;
    }

    if (ep == 0x40109F &&
        plugin::patch::GetFloat(0xBBBC3C) == 1.3333334f)
    {
        InstallLive07MemoryUpgrade();
        return;
    }

    if (ep == 0x40109F &&
        plugin::patch::GetFloat(0xC3DF84) == 1.3333334f)
    {
        InstallLive08MemoryUpgrade();
        return;
    }
}
