#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

class ResolutionMenuStringPatcher
{
public:
    ResolutionMenuStringPatcher()
        : m_count(0),
          m_started(0)
    {
        std::memset(m_pairs, 0, sizeof(m_pairs));
    }

    template <typename ResolutionType>
    void Initialize(
        std::uintptr_t resolutionTable,
        const ResolutionType* resolutions,
        std::size_t count)
    {
        if (!resolutionTable || !resolutions || count == 0)
            return;

        if (InterlockedCompareExchange(&m_started, 1, 0) != 0)
            return;

        if (count > kMaxPairs)
            count = kMaxPairs;

        m_count = count;

        // Capture the ORIGINAL game table before GameLiveXX.cpp overwrites it.
        // Each stock entry is 20 bytes:
        //   +04 width
        //   +08 height
        //   +0C depth
        for (std::size_t i = 0; i < m_count; ++i)
        {
            const std::uintptr_t entry =
                resolutionTable + (20 * i);

            const unsigned int oldWidth =
                *reinterpret_cast<const unsigned int*>(entry + 4);

            const unsigned int oldHeight =
                *reinterpret_cast<const unsigned int*>(entry + 8);

            const unsigned int oldDepth =
                *reinterpret_cast<const unsigned int*>(entry + 12);

            _snprintf_s(
                m_pairs[i].oldText,
                sizeof(m_pairs[i].oldText),
                _TRUNCATE,
                "%ux%ux%u",
                oldWidth,
                oldHeight,
                oldDepth);

            // The custom menu does not need the x32 suffix because all
            // replacement modes are 32-bit.
            _snprintf_s(
                m_pairs[i].newText,
                sizeof(m_pairs[i].newText),
                _TRUNCATE,
                "%ux%u",
                static_cast<unsigned int>(resolutions[i].width),
                static_cast<unsigned int>(resolutions[i].height));

            m_pairs[i].seen = false;
        }

        HANDLE thread = CreateThread(
            nullptr,
            0,
            &ResolutionMenuStringPatcher::ThreadProc,
            this,
            0,
            nullptr);

        if (thread)
        {
            // The worker owns no external resources and exits by itself once
            // every stock label has been replaced.
            CloseHandle(thread);
        }
        else
        {
            // Allow a later retry if thread creation failed.
            InterlockedExchange(&m_started, 0);
        }
    }

private:
    static const std::size_t kMaxPairs = 32;

    struct Pair
    {
        char oldText[32];
        char newText[32];
        bool seen;
    };

    Pair m_pairs[kMaxPairs];
    std::size_t m_count;
    volatile LONG m_started;

    static bool IsWritablePrivateRegion(const MEMORY_BASIC_INFORMATION& mbi)
    {
        if (mbi.State != MEM_COMMIT)
            return false;

        if (mbi.Type != MEM_PRIVATE)
            return false;

        if (mbi.Protect & PAGE_GUARD)
            return false;

        if (mbi.Protect & PAGE_NOACCESS)
            return false;

        const DWORD protect = mbi.Protect & 0xFF;

        return
            protect == PAGE_READWRITE ||
            protect == PAGE_WRITECOPY ||
            protect == PAGE_EXECUTE_READWRITE ||
            protect == PAGE_EXECUTE_WRITECOPY;
    }

    bool ScanOnce()
    {
        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);

        std::uintptr_t address =
            reinterpret_cast<std::uintptr_t>(
                systemInfo.lpMinimumApplicationAddress);

        const std::uintptr_t maximumAddress =
            reinterpret_cast<std::uintptr_t>(
                systemInfo.lpMaximumApplicationAddress);

        while (address < maximumAddress)
        {
            MEMORY_BASIC_INFORMATION mbi{};

            if (VirtualQuery(
                    reinterpret_cast<const void*>(address),
                    &mbi,
                    sizeof(mbi)) != sizeof(mbi))
            {
                break;
            }

            if (IsWritablePrivateRegion(mbi))
            {
                unsigned char* base =
                    static_cast<unsigned char*>(mbi.BaseAddress);

                const std::size_t regionSize =
                    static_cast<std::size_t>(mbi.RegionSize);

                // A region can theoretically change between VirtualQuery and
                // the scan. Keep a stale region from taking the game down.
                __try
                {
                    for (std::size_t pairIndex = 0;
                         pairIndex < m_count;
                         ++pairIndex)
                    {
                        Pair& pair = m_pairs[pairIndex];

                        const std::size_t oldLen =
                            std::strlen(pair.oldText);

                        const std::size_t newLen =
                            std::strlen(pair.newText);

                        if (oldLen == 0 ||
                            newLen > oldLen ||
                            regionSize < oldLen + 1)
                        {
                            continue;
                        }

                        for (std::size_t i = 0;
                             i + oldLen < regionSize;
                             ++i)
                        {
                            unsigned char* p = base + i;

                            if (std::memcmp(
                                    p,
                                    pair.oldText,
                                    oldLen) != 0)
                            {
                                continue;
                            }

                            // Exact C-string match only.
                            if (p[oldLen] != '\0')
                                continue;

                            std::memset(
                                p,
                                0,
                                oldLen + 1);

                            std::memcpy(
                                p,
                                pair.newText,
                                newLen);

                            pair.seen = true;
                            i += oldLen;
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    // Region changed while scanning. Retry on the next pass.
                }
            }

            const std::uintptr_t nextAddress =
                reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) +
                static_cast<std::uintptr_t>(mbi.RegionSize);

            if (nextAddress <= address)
                break;

            address = nextAddress;
        }

        for (std::size_t i = 0; i < m_count; ++i)
        {
            if (!m_pairs[i].seen)
                return false;
        }

        return true;
    }

    static DWORD WINAPI ThreadProc(LPVOID parameter)
    {
        ResolutionMenuStringPatcher* self =
            static_cast<ResolutionMenuStringPatcher*>(parameter);

        if (!self)
            return 0;

        // Frontend/localization buffers are not guaranteed to exist when the
        // ASI installs. Retry until all stock labels have appeared and been
        // replaced.
        for (;;)
        {
            if (self->ScanOnce())
                break;

            Sleep(250);
        }

        return 0;
    }
};
