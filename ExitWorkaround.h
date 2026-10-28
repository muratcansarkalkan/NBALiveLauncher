#pragma once

// Installs the ExitProcess -> TerminateProcess compatibility hook in the
// main NBA Live executable. The implementation is address-independent and
// is intended for NBA Live 2005, 06, 07 and 08 (32-bit PC).
//
// Returns true if the hook is installed (or was already installed).
bool InstallExitWorkaround();

// Restores the original ExitProcess IAT entry.
// Intended mainly for testing/reverting before process shutdown.
bool UninstallExitWorkaround();

bool IsExitWorkaroundInstalled();
