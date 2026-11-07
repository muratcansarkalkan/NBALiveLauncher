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
#include "Live07CleanupFix.h"

using namespace plugin;
namespace Live07CleanupFix {
namespace {
#if defined(_M_IX86)
using Live07ReleaseResource_t = void (__cdecl *)(void* resultPtr, void* resourceObject);

bool (*g_isReadable)(uintptr_t, size_t) = nullptr;
Live07CleanupFix::RejectReporter g_reportRejected = nullptr;

bool IsReadableObject07(uintptr_t p, size_t size)
{
    return g_isReadable && g_isReadable(p, size);
}

enum Live07CleanupRejectReason : DWORD {
    LIVE07_CLEANUP_OK = 0,
    LIVE07_CLEANUP_BAD_OBJECT,
    LIVE07_CLEANUP_BAD_OWNER,
    LIVE07_CLEANUP_BAD_OWNER_COUNT,
    LIVE07_CLEANUP_BAD_OWNER_LIST
};

const char* Live07CleanupRejectReasonName(
    Live07CleanupRejectReason reason)
{
    switch (reason) {
    case LIVE07_CLEANUP_BAD_OBJECT:      return "BAD_OBJECT";
    case LIVE07_CLEANUP_BAD_OWNER:       return "BAD_OWNER";
    case LIVE07_CLEANUP_BAD_OWNER_COUNT: return "BAD_OWNER_COUNT";
    case LIVE07_CLEANUP_BAD_OWNER_LIST:  return "BAD_OWNER_LIST";
    default:                             return "OK";
    }
}

bool IsSafeCleanupResource07(
    uintptr_t objectPtr,
    uintptr_t& ownerPtr,
    uintptr_t& ownerListPtr,
    DWORD& ownerCount,
    Live07CleanupRejectReason& rejectReason)
{
    ownerPtr = 0;
    ownerListPtr = 0;
    ownerCount = 0;
    rejectReason = LIVE07_CLEANUP_OK;

    // 0080B32B reads object+2C before calling 0080B103.
    if (!objectPtr || !IsReadableObject07(objectPtr, 0x30)) {
        rejectReason = LIVE07_CLEANUP_BAD_OBJECT;
        return false;
    }

    __try {
        ownerPtr =
            *reinterpret_cast<const uintptr_t*>(objectPtr + 0x2C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ownerPtr = 0;
        rejectReason = LIVE07_CLEANUP_BAD_OBJECT;
        return false;
    }

    // 0080B103 uses owner+18 as the list pointer and owner+1C as count.
    if (!ownerPtr || !IsReadableObject07(ownerPtr, 0x20)) {
        rejectReason = LIVE07_CLEANUP_BAD_OWNER;
        return false;
    }

    __try {
        ownerListPtr =
            *reinterpret_cast<const uintptr_t*>(ownerPtr + 0x18);
        ownerCount =
            *reinterpret_cast<const DWORD*>(ownerPtr + 0x1C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ownerListPtr = 0;
        ownerCount = 0;
        rejectReason = LIVE07_CLEANUP_BAD_OWNER;
        return false;
    }

    // Observed bad stock-game state:
    //   owner      = 00060000
    //   owner+18   = 00021000
    //   owner+1C   = D24841A3
    //
    // Use a deliberately generous ceiling. This catches clearly stale/reused
    // owner memory without pretending to know the exact engine maximum.
    constexpr DWORD kMaxReasonableOwnerEntries = 0x00010000;

    if (ownerCount > kMaxReasonableOwnerEntries) {
        rejectReason = LIVE07_CLEANUP_BAD_OWNER_COUNT;
        return false;
    }

    // Zero entries means the list pointer is irrelevant.
    if (ownerCount == 0)
        return true;

    if (!ownerListPtr) {
        rejectReason = LIVE07_CLEANUP_BAD_OWNER_LIST;
        return false;
    }

    const size_t listBytes =
        static_cast<size_t>(ownerCount) * sizeof(uintptr_t);

    if (!IsReadableObject07(ownerListPtr, listBytes)) {
        rejectReason = LIVE07_CLEANUP_BAD_OWNER_LIST;
        return false;
    }

    return true;
}

void __cdecl SafeReleaseResource07(void* resultPtr, void* resourceObject)
{
    const uintptr_t objectPtr =
        reinterpret_cast<uintptr_t>(resourceObject);

    uintptr_t ownerPtr = 0;
    uintptr_t ownerListPtr = 0;
    DWORD ownerCount = 0;
    Live07CleanupRejectReason rejectReason = LIVE07_CLEANUP_OK;

    if (!IsSafeCleanupResource07(
            objectPtr,
            ownerPtr,
            ownerListPtr,
            ownerCount,
            rejectReason))
    {
        if (g_reportRejected) {
            g_reportRejected(Live07CleanupRejectReasonName(rejectReason),
                objectPtr, ownerPtr, ownerListPtr, ownerCount);
        }

        // 00689B70 clears the cleanup slot immediately after this call
        // returns, so the rejected stale entry is discarded by the game's
        // normal cleanup control flow.
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


#endif
}
bool Install(bool (*isReadable)(uintptr_t, size_t), RejectReporter report) {
#if defined(_M_IX86)
    g_isReadable = isReadable;
    g_reportRejected = report;
    return InstallLive07CleanupGuard();
#else
    return false;
#endif
}
}
