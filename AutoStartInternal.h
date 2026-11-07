#pragma once
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
using namespace plugin;
#include <vector>
#include <algorithm>
#include <cstdarg>

namespace autostartdetail {
constexpr unsigned int kLive06EntryPoint       = 0x0040109F;

constexpr unsigned int kLive06AspectMarker     = 0x00BD832C;

constexpr float        kStockAspectMarker      = 1.3333334f;

constexpr unsigned int kFESystemGlobal         = 0x00CB2AF4;

constexpr unsigned int kFESystemUpdate         = 0x00598980;

constexpr unsigned int kGetStartupScreen       = 0x005CBA30;

constexpr unsigned int kSetAptEnumReturn       = 0x005C8C60;

constexpr unsigned int kFEUpdateCall           = 0x005F6AAF;

constexpr unsigned int kOpenResolved06          = 0x006DC8F9;

constexpr unsigned int kRequestCtorTracePatch06  = 0x006DD8A8;

constexpr unsigned int kRequestCtorTraceResume06 = 0x006DD8AD;

constexpr unsigned int kGetFEUberGameMode      = 0x004B8870;

constexpr int kStateFeRun                      = 4;

constexpr int kGDFeRun                         = 5;

constexpr int kGDGameInit                      = 14;

extern bool g_enabled;

extern bool g_pending;

extern bool g_consumed;

extern DWORD g_feRunSeenAt;

extern DWORD g_autoStartDelayMs;

extern DWORD g_autoStartInitializedAt;

extern DWORD g_titleStartupCallbackAt;

constexpr unsigned char kTitleStartupBranchPattern[] =
    {
        0xA2, 0x1E,                   // GetStartupScreen()
        0x5A,
        0xA2, 0x0B,                   // CallFunction
        0x3D,
        0xA2, 0x1F,                   // STARTUP_SCREEN_ONLINE
        0x49,
        0x12,
        0x9D, 0x1C, 0x00, 0x00, 0x00,

        0xA2, 0x20,                   // ChangeGameMode(GAMEMODE_ONLINE)
        0x5A,
        0xB0, 0x0B,
        0xAE, 0x11,
        0xAF, 0x21,                   // SCREEN.MAIN_MENU
        0x5A,
        0xAE, 0x13,
        0xB2, 0x22,                   // gSM.pushScreen (online branch)
        0xAE, 0x11,
        0xAF, 0x23,                   // SCREEN.SN_START
        0x5A,
        0xAE, 0x13,
        0xB2, 0x14,                   // gSM.changeMainScreen
        0x99, 0x09, 0x00, 0x00, 0x00,

        // Normal PC branch target in stock title.apt:
        0xAE, 0x11,
        0xAF, 0x21,                   // SCREEN.MAIN_MENU
        0x5A,
        0xAE, 0x13,
        0xB2, 0x14                    // gSM.changeMainScreen
    };

constexpr size_t kTitleBranchOperandOffset = 11;

constexpr unsigned char kTitleBranchStockOffset = 0x1C;

constexpr unsigned char kTitleBranchSkipMainMenuOffset = 0x25;

struct TitleBranchPatch
    {
        unsigned char* operand = nullptr;
        unsigned char original = 0;
    };

extern TitleBranchPatch g_titleBranchPatches[8];

extern size_t g_titleBranchPatchCount;

extern volatile LONG g_bootAssetOverridesActive;

extern const char* g_splashOverridePath;

extern const char* g_healthOverridePath;

extern volatile LONG g_splashOverrideRequests;

extern volatile LONG g_healthOverrideRequests;

constexpr char kSplashAssetNative[] =
        "assets\\autostart\\06\\screens~bootup~splash.big";

constexpr char kHealthAssetNative[] =
        "assets\\autostart\\06\\screens~bootup~health.big";

constexpr char kSplashAssetShort[] =
        "assets\\autostart\\06\\splash.big";

constexpr char kHealthAssetShort[] =
        "assets\\autostart\\06\\health.big";

constexpr char kBootSplashPackage[] =
        "screens~bootup~splash.big";

constexpr char kBootHealthPackage[] =
        "screens~bootup~health.big";

extern bool g_bootSelectorTrace;

extern FILE* g_bootSelectorTraceFile;

extern bool g_tracedLegal;

extern bool g_tracedNbaLogo;

extern bool g_tracedTitle;

#if defined(_M_IX86)
extern bool g_requestCtorTraceHookInstalled;

extern uintptr_t g_bootOpenResolvedContinuation;

extern bool g_bootOpenResolvedHookInstalled;

#endif
const char kBootLegalPackage[] =
        "screens~bootup~legal.big";

const char kBootTitlePackage[] =
        "screens~bootup~title.big";

const char kBootNbaLogoPackage[] =
        "screens~bootup~nbalogo.big";

void Log(const char* format, ...);

std::string GetMainIniPath();

bool AssetFileExists06(const char* relativePath);

const char* ChooseBootOverride06(
        const char* nativeName,
        const char* shortName);

bool IsLive06();

int ReadFEState(void* feSystem);

int ReadGDState(void* feSystem);

void DumpUberState(const char* reason);

const char* BaseNameOfPath(const char* path);

bool IsLikelyCodeAddress06(uintptr_t value);

bool CopyReadableCString(
        const char* source,
        char* destination,
        size_t destinationSize);

void OpenBootSelectorTraceFile();

bool IsMainExeCodeAddress(uintptr_t value);

void WriteBootSelectorTraceLine(
        const char* format,
        ...);

void TraceBootSelectorRequest(
        const char* requestedPath,
        uintptr_t* entryStack);

#if defined(_M_IX86)
void __cdecl TraceBootRequestConstructed(
        uintptr_t requestObject,
        uintptr_t constructorFrame);

void BootRequestCtorTraceHook06();

bool InstallBootRequestCtorTraceHook06();

void __cdecl RedirectBootPackageAtOpen(uintptr_t* entryStack);

void BootOpenResolvedHook06();

bool InstallBootOpenResolvedHook06();

#endif
#if defined(_M_IX86)
#else
bool InstallBootRequestCtorTraceHook06();

bool InstallBootOpenResolvedHook06();

#endif
bool IsReadableCommittedProtection(DWORD protect);

bool ReadRuntimeByte(
        const unsigned char* address,
        unsigned char& value);

bool WriteRuntimeByte(
        unsigned char* address,
        unsigned char value);

void RestoreTitleMainMenuBranch();

bool PatchLiveTitleMainMenuBranch();

void __cdecl GetStartupScreen_AutoStart(void* aptCall);

int __fastcall FESystemUpdate_AutoStart(
        void* feSystem,
        void* /*edx*/,
        int delta);

bool VerifyOriginalBytes();

}
