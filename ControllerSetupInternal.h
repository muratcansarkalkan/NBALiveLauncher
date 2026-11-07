#pragma once
namespace LiveControllerSetup {
// Internal startup bridge; GameSetup owns the legacy public initializer.
void InitializeConfiguration(void (*onNativeApiReady)());
}
