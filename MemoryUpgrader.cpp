#include "plugin-std.h"

#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>

namespace
{
    void MemoryLog(const char* text)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, "MemoryUpgrader.log", "a") == 0 && f)
        {
            fputs(text, f);
            fclose(f);
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

    bool InstallLive2005MemoryUpgrade()
    {
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
            OutputDebugStringA(
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

        OutputDebugStringA(
            ok
                ? "[MemoryUpgrader] NBA Live 2005 capacity: 118 -> 512 entries.\n"
                : "[MemoryUpgrader] NBA Live 2005 patch failed.\n");

        return ok;
    }


    // =====================================================================
    // NBA Live 06
    // =====================================================================

    constexpr std::uintptr_t k06PageSizeAddress  = 0x00C3BB6C;
    constexpr std::uintptr_t k06ArenaSizeAddress = 0x00C3BB70;

    constexpr DWORD k06ExpectedPageSize = 0x00002000; // 8 KiB
    constexpr DWORD k06OriginalArena    = 0x01040000; // 16.25 MiB
    constexpr DWORD k06ExpandedArena    = 0x02080000; // 32.5 MiB

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

        __try
        {
            pageSize =
                *reinterpret_cast<const DWORD*>(k06PageSizeAddress);

            arenaSize =
                *reinterpret_cast<const DWORD*>(k06ArenaSizeAddress);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 06 allocator config read failed.\n");

            return false;
        }

        if (pageSize != k06ExpectedPageSize ||
            arenaSize != k06OriginalArena)
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 06 allocator verification failed.\n");

            return false;
        }

        if (!PatchBytes(
                k06ArenaSizeAddress,
                &k06ExpandedArena,
                sizeof(k06ExpandedArena)))
        {
            MemoryLog(
                "[MemoryUpgrader] NBA Live 06 arena patch failed.\n");

            return false;
        }

        {
            char buf[192];
            sprintf_s(
                buf,
                "[MemoryUpgrader] 06 pre-construction config: "
                "arena=%08X page=%08X\n",
                *reinterpret_cast<const DWORD*>(k06ArenaSizeAddress),
                *reinterpret_cast<const DWORD*>(k06PageSizeAddress));
            MemoryLog(buf);
        }

        // Hook only the specific MasterFixedPageAllocator creation call.
        plugin::patch::RedirectCall(
            k06MasterAllocatorCall,
            AcquireMasterAllocatorHook);

        MemoryLog(
            "[MemoryUpgrader] NBA Live 06 MasterFixedPageAllocator config: "
            "16.25 MiB -> 32.5 MiB (2080 -> 4160 pages).\n");

        return true;
    }
}


// =====================================================================
// Public entry point
// =====================================================================

void InitializeMemoryUpgrader()
{
    const std::uintptr_t ep = FM::GetEntryPoint();

    if (ep == 0xCD8005)
    {
        InstallLive2005MemoryUpgrade();
        return;
    }

    if (ep == 0x40109F &&
        plugin::patch::GetFloat(0xBD832C) == 1.3333334f)
    {
        //InstallLive06MemoryUpgrade();
        return;
    }
}
