#pragma once
#include <Windows.h>
namespace LiveLoading {
struct WaitSample { DWORD generation; LONGLONG begin; bool active; };
using BeginWaitObserver = WaitSample (*)();
using FinishWaitObserver = void (*)(const WaitSample&, DWORD, bool, DWORD);
// Registered before installing the wait hook; never changed while it is active.
void SetWaitObserver(BeginWaitObserver begin, FinishWaitObserver finish);
bool InstallFileCompletionDelay(bool fast);
void WriteFileWaitInstallStatus(bool installed);
bool IsInstalled();
bool IsFastWaitEnabled();
bool HasCompletionSignal();
}
