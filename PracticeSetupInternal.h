#pragma once
#include "plugin-std.h"
#include <Windows.h>
#include <cstdint>
namespace practicesetup {
struct PracticeProbe06 {
        bool enabled = false;
        int dbId = -1;
        int jerseyId = -1;     // -1 = EA default; >=0 = teamgear.dbf.ID
        int kind = 0;
        bool useKeyboard = false;
        bool autoSelectMode = false;  // opt-in: construct Practice as initial FE mode
        int lastFePlayer = -1;
        int lastFeJersey = -1;
        int lastFeKind = -1;
        unsigned long frontendCalls = 0;
        unsigned long backendCalls = 0;
    };

extern PracticeProbe06 g_practice06;

constexpr uintptr_t kPracticeNativePlayerSetter06 = 0x00536190;

constexpr uintptr_t kPracticeSceneStart06 = 0x0061DEA0;

using PracticeSetPlayerFn06 = void (__thiscall*)(void*, int);

using PracticeSceneFn06 = void (__thiscall*)(void*, int);

void PracticeLog06(const char* message);

bool PracticeApplyJersey06(const char* phase, void* participant);

void __fastcall PracticePlayerCallHook06(void* feObject, void*, int originalNativeId);

constexpr uintptr_t kPracticeBootModeCall06 = 0x005DDF59;

constexpr uintptr_t kNativeModeConstructor06 = 0x00582340;

constexpr int kExhibitionMode06 = 5;

constexpr int kIndividualPracticeMode06 = 10;

using PracticeBootModeFn06 = void (__thiscall*)(void*, int);

void __fastcall PracticeBootModeCallHook06(void* feModeManager, void*, int originalMode);

bool InstallPracticeBootModeHook06();

constexpr uintptr_t kPracticePrepareCallsite06 = 0x005EF462;

constexpr uintptr_t kPracticePrepareTarget06 = 0x005EB690;

using PracticePrepareFn06 = unsigned char (__thiscall*)(void*, void*, void*);

void PracticeDescribeParticipant06(const char* stage, const void* participant);

unsigned char __fastcall PracticePrepareCallHook06(
        void* runtimeSetup, void*, void* participant, void* stagingRecord);

bool InstallPracticePrepareHook06();

void PracticeDumpWords06(const char* label, const void* ptr, size_t byteCount);

void PracticeTraceStack06();

void __fastcall PracticeSceneCallHook06(void* sceneObject, void*, int setupArg);

bool VerifyPracticeCall06(uintptr_t site);

bool InstallPracticePlayerCall06(uintptr_t site);

void InitializePracticeProbe06();


bool IsReadableAddress(const void*, size_t);
}
