#pragma once
#include <Windows.h>
#include <cstdint>
namespace Live07CleanupFix {
using RejectReporter = void (*)(const char*, uintptr_t, uintptr_t, uintptr_t, DWORD);
// Called under the original DEBUG/CONSOLE and LIVE07_CLEANUP_FIX conditions.
bool Install(bool (*isReadable)(uintptr_t, size_t), RejectReporter report);
}
