#pragma once

#include <Windows.h>

// Installs the main-EXE ExitProcess -> TerminateProcess compatibility hook.
// This is address-independent and intended for NBA Live 2005, 06, 07 and 08
// (32-bit PC).
//
// By default this also starts the Alt+F4 watcher, so replacing the previous
// ExitWorkaround.cpp/.h is enough; existing InstallExitWorkaround() calls do
// not need to change.
bool InstallExitWorkaround();

// Restores the original ExitProcess IAT entry and stops the Alt+F4 watcher.
// Intended mainly for testing/reverting while the process is still alive.
bool UninstallExitWorkaround();

bool IsExitWorkaroundInstalled();

// Shared hard-termination path used by:
//   1) the hooked final ExitProcess call
//   2) the Alt+F4 force-exit fallback
//
// This intentionally bypasses ExitProcess/RtlExitUserProcess teardown.
[[noreturn]] void FastTerminateProcess(UINT exitCode = 0);

// Alt+F4 support can be controlled independently if desired.
// The watcher only reacts while a window owned by the current NBA Live
// process is the foreground window.
bool StartAltF4ExitWatcher();
void StopAltF4ExitWatcher();
bool IsAltF4ExitWatcherRunning();
