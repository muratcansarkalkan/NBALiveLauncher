#pragma once
// Private bridge for native game/stadium hooks and diagnostics. Not a public API.
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

namespace custompaths {
enum GameVersion
    {
        GAME_UNKNOWN,
        GAME_LIVE_2005,
        GAME_LIVE_06,
        GAME_LIVE_07,
        GAME_LIVE_08
    };

enum BenchPose { BENCH_NATIVE, BENCH_SEATED, BENCH_FLOOR };

struct BenchSlotConfig
    {
        BenchPose pose = BENCH_NATIVE;
        bool hasOffset = false;
        float offset[3] = {};
        bool hasYaw = false;
        float yaw = 0.0f;
    };

extern BenchSlotConfig gBenchSlots[2][7];

extern bool gBenchHasPositionRequests;

extern uintptr_t gBenchActorRecords[2][7];

void LoadBenchStadiumConfig(const char* id4);

#if defined(_M_IX86)
int __stdcall ResolveBenchPose(int original, uintptr_t controller, int slot);

void BenchStartPoseHook();

void BenchReplayPoseHook();

bool InstallBenchPoseBranch(unsigned int address,const unsigned char* bytes,
                                size_t length,void* hook);

using BenchFactoryFn = void (__cdecl *)(uintptr_t, uintptr_t, uintptr_t,
        uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static const BenchFactoryFn kBenchFactory = reinterpret_cast<BenchFactoryFn>(0x00605A20);

void __cdecl BenchFactoryCapture(uintptr_t record, uintptr_t name,
        uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5,
        uintptr_t a6, uintptr_t a7, uintptr_t a8);

void __stdcall BenchApplyRootOffset(uintptr_t matrix);

void BenchRootTransformHook();

void InstallLive06BenchPositionHooks();

void InstallLive06BenchPoseHooks();

#endif
enum OverrideSlot
    {
        SLOT_STAGE_COURT,
        SLOT_STAGE_BACKBOARD,
        SLOT_STAGE_DORNA,
        SLOT_PLAYOFF_COURT,
        SLOT_PLAYOFF_BACKBOARD,
        SLOT_PLAYOFF_DORNA,
        SLOT_REGULAR_COURT,
        SLOT_REGULAR_BACKBOARD,
        SLOT_REGULAR_DORNA,
        SLOT_COUNT
    };

enum PostseasonStage
    {
        STAGE_REGULAR,
        STAGE_PLAYOFF,
        STAGE_ECF,
        STAGE_WCF,
        STAGE_FINALS,
        STAGE_ECSF,
        STAGE_WCSF,
        STAGE_ECR1,
        STAGE_WCR1
    };

enum ForcedNativeGameMode
    {
        NATIVE_MODE_OFF,
        NATIVE_MODE_ALLSTAR,
        NATIVE_MODE_ROOKIE
    };

const size_t kOverridePathCapacity = 96;

const size_t kOverrideCacheCapacity = 256;

const size_t kTournamentChildCandidateCount = 2;

const size_t kTournamentChildSnapshotCapacity = 0x854;

extern char gOverridePaths[SLOT_COUNT][kOverridePathCapacity];

struct OverrideCacheEntry
    {
        char logicalName[128];
        char resolvedPath[MAX_PATH];
        bool found;
    };

struct TournamentChildSnapshot
    {
        void* pointer;
        unsigned char bytes[kTournamentChildSnapshotCapacity];
        bool valid;
    };

extern OverrideCacheEntry gOverrideCache[kOverrideCacheCapacity];

extern unsigned int gOverrideCacheCount;

extern unsigned int gOverrideCacheCursor;

extern bool gGameContextValid;

extern char gCurrentStadiumId4[5];

extern PostseasonStage gCurrentStage;

extern bool gForcedStageEnabled;

extern PostseasonStage gForcedStage;

extern bool gForceGlobalGameMode;

extern bool gForcedSeriesEnabled;

extern int gForcedSeriesGame;

extern int gForcedSeriesLength;

extern int gForcedHomeWins;

extern int gForcedAwayWins;

extern ForcedNativeGameMode gForcedNativeGameMode;

extern bool gPlayoffStateDebug;

extern bool gLoggedMissingGlobalOptions;

extern bool gLoggedMissingNativeModeOptions;

extern bool gLoggedUnsupportedNativeMode;

extern void* gLastPlayoffSnapshotOptions;

extern unsigned int gLastPlayoffSnapshotHash;

extern bool gRetryLive06PlayoffSeries;

extern bool gLoggedForcedScheduleGetter;

extern bool gLoggedForcedScheduleSelection;

extern unsigned char gPreviousOptionsSnapshot[0x200];

extern bool gPreviousOptionsSnapshotValid;

extern void* gLastRejectedOptions;

extern int gLastRejectedMode;

extern int gLastRejectedRound;

extern FILE* gPlayoffStateLogFile;

extern bool gLastTournamentSnapshotValid;

extern unsigned int gLastTournamentSnapshotHash;

extern int gLastTournamentGameMode;

extern unsigned int gPlayoffSnapshotSequence;

extern unsigned int gLastTournamentPlayoffSequence;

extern unsigned int gTournamentSnapshotSequence;

extern TournamentChildSnapshot
        gTournamentChildSnapshots[kTournamentChildCandidateCount];

const size_t kLive06PlayoffBracketSlotCount = 30;

extern unsigned char
        gLive06PlayoffBracketTeams[kLive06PlayoffBracketSlotCount];

extern short gLive06PlayoffBracketWins[kLive06PlayoffBracketSlotCount];

extern bool gLive06PlayoffBracketCacheValid;

extern void* gLive06CapturedPlayoffBracket;

struct PlayoffAccessorTrace
    {
        void* settings;
        uintptr_t caller;
        unsigned int id;
        unsigned int source;
        int value;
        bool isBoolean;
    };

const size_t kPlayoffAccessorTraceCapacity = 256;

extern PlayoffAccessorTrace
        gPlayoffAccessorTraces[kPlayoffAccessorTraceCapacity];

extern unsigned int gPlayoffAccessorTraceCount;

extern unsigned int gPlayoffAccessorTraceCursor;

struct CommentaryParameterTrace
    {
        uintptr_t caller;
        unsigned int index;
        int value;
        bool write;
    };

const size_t kCommentaryParameterTraceCapacity = 256;

extern CommentaryParameterTrace
        gCommentaryParameterTraces[kCommentaryParameterTraceCapacity];

extern unsigned int gCommentaryParameterTraceCount;

extern unsigned int gCommentaryParameterTraceCursor;

extern unsigned int gLive06IntroArguments[36];

extern bool gLive06IntroArgumentsValid;

extern unsigned int gLive06IntroTraceCount;

extern GameVersion gGameVersion;

#if defined(_M_IX86)
extern uintptr_t gOpenResolvedContinuation;

extern bool gOpenResolvedGateInstalled;

#endif
const unsigned int kLive08GameCourtNameCalls[] = {
        0x0066F677, 0x00670EC2, 0x00671532, 0x00671F12,
        0x0067671B, 0x0067743D, 0x006780D3, 0x00678613
    };

const unsigned int kLive08GameCourtSetStadiumName = 0x0069EFC0;

const unsigned int kLive06ScheduleSelectionCalls[] = {
        0x00515DFB, 0x00515F6B, 0x00530BDF,
        0x00530DC3, 0x0055E61F, 0x005A6FEA,
        0x005AE3F6, 0x005AE6B4, 0x005BD225
    };

bool IsValidId4(const char* value);

void NormalizeId4(const char* source, char* destination);

void* GetCurrentOptions(void* suppliedOptions);

const char* StageName(PostseasonStage stage);

const char* ForcedNativeGameModeName(ForcedNativeGameMode mode);

int StageRoundValue(PostseasonStage stage);

const char* GameVersionName();

void InitializePlayoffStateLog();

void WritePlayoffStateLog(const char* message);

struct Live06ScheduleGame
    {
        unsigned char bytes[44];
    };

short ReadScheduleShort(
        const Live06ScheduleGame& game,
        size_t offset);

bool ReadLive06ScheduleGame(
        unsigned int gameId,
        Live06ScheduleGame& game);

struct Live08ScheduleGame
    {
        unsigned char bytes[64];
    };

short ReadLive08ScheduleShort(
        const Live08ScheduleGame& game,
        size_t offset);

bool ReadLive08ScheduleGame(
        unsigned int gameId,
        Live08ScheduleGame& game);

bool IsReadableMemoryRange(const void* address, size_t size);

bool AddMemoryToHash(
        const void* address,
        size_t size,
        unsigned int& hash);

void LogRawMemoryBlock(
        const char* label,
        const void* address,
        size_t size);

void LogLive06PlayoffSettings(const void* settings);

void LogLive06PlayoffBracket(const void* bracket);

void CaptureLive06PlayoffBracket(const void* bracket);

void __fastcall Live06PlayoffBracketDestructorHook(
        void* bracket,
        void*);

bool IsLive06PlayoffIntegerSetting(unsigned int id);

bool IsLive06PlayoffBooleanSetting(unsigned int id);

const char* Live06PlayoffSettingName(unsigned int id);

void LogLive06PlayoffAccessor(
        void* settings,
        uintptr_t caller,
        unsigned int id,
        unsigned int source,
        int value,
        bool isBoolean);

int __fastcall Live06IntegerSettingGetterHook(
        void* settings,
        void*,
        unsigned int id,
        unsigned int source);

bool __fastcall Live06BooleanSettingGetterHook(
        void* settings,
        void*,
        unsigned int id,
        unsigned int source);

void LogLive06CommentaryParameter(
        uintptr_t caller,
        unsigned int index,
        int value,
        bool write);

int __stdcall Live06CommentaryParameterGetterHook(unsigned int index);

void __stdcall Live06CommentaryParameterSetterHook(
        unsigned int index,
        int value);

#if defined(_M_IX86)
extern uintptr_t gLive06IntroEventTarget;

extern uintptr_t gLive06SpeechDispatchContinuation;

extern uintptr_t gLive06PregameContinuation;

extern uintptr_t gLive06PlayoffCacheInitializer;

void ApplyForcedSeriesRulesForBuild(void* options);

void __stdcall ApplyLive06ForcedSpeechContext();

bool Live05ForcedSpeechEnabled();

short* __fastcall Live05SpeechPlayoffQueryHook(
        void* object, void*, short* output);

unsigned char* __fastcall Live05SpeechSeriesQueryHook(
        void* object, void*, unsigned char* output,
        unsigned int period, unsigned int roundAndSlot,
        unsigned int slotAndGame);

void Live06PlayoffCacheInitializerHook();

void Live06PregameContextHook();

bool InstallLive06PregameContextHook();

void LogLive06AudioSettings(const char* phase);

void __stdcall LogLive06SpeechDispatch(const unsigned int* entryStack);

void Live06SpeechDispatchTraceHook();

bool InstallLive06SpeechDispatchTrace();

void LogLive06AudioSettings(const char* phase);

void __stdcall LogLive06SetupHandoff(
        const unsigned char* setup, const unsigned int* entryStack,
        const unsigned int* vtable, void* interfaceObject);

void Live06SetupHandoffTraceHook();

bool InstallLive06SetupHandoffTrace();

void __stdcall LogLive06IntroArguments(const unsigned int* entryStack);

void Live06IntroEventTraceHook();

#endif
void LogLive06TournamentState();

void InitializeForcedStageSettings();

void LogPlayoffStateSnapshot(void* options);

void ApplyForcedNativeGameMode(void* options);

void ApplyGlobalForcedStage(void* options);

unsigned int ForcedStageBracketSlot(
        PostseasonStage stage,
        unsigned int currentGame);

unsigned int ForcedScheduleGameId(unsigned int currentGame);

short __cdecl Live06ForcedCurrentGameGetterHook();

void __fastcall Live06ScheduleSelectionHook(
        void* scheduleInfo,
        void*,
        int selection,
        void* selectionContext);

void ApplyForcedSeriesState(void* options);

PostseasonStage GetPostseasonStage(void* suppliedOptions);

const char* GetStageFolder(PostseasonStage stage);

void SetSlot(OverrideSlot slot, const char* value);

void DisableAllSlots();

void BeginNewGameStadiumLoad();

void UpdateGameContext(const char* stadiumId4, void* suppliedOptions);

PostseasonStage PrepareGameContext(void* suppliedOptions);

bool ValidateCallSites(
        const unsigned int* sites,
        size_t count,
        unsigned int expectedTarget);

bool ContainsCaseInsensitive(const char* text, const char* token);

bool ShouldSearchStadiumPaths(const char* requestedFilename);

const char* BaseNameOf(const char* path);

const char* ResolveCachedCustomPath(const char* requestedPath);

#if defined(_M_IX86)
void __cdecl ResolveCustomPathAtOpen(uintptr_t* entryStack);

void FilteredOpenResolved();

bool InstallOpenResolvedGate(unsigned int targetAddress);

#endif
#if defined(_M_IX86)
#else
bool InstallOpenResolvedGate(unsigned int);

#endif
void __fastcall GameCourtSetStadiumNameHook08(
        void* gameCourt,
        void*,
        const char* stadiumId4);

void InstallLive06PlayoffBracketCaptureHook();

bool InstallLive06AccessorEntryHook(
        uintptr_t address,
        const unsigned char* expected,
        void* hook);

void InstallLive06PlayoffAccessorHooks();

void InstallLive08ContextHook();

void Initialize();

void InitializeGameContextHooks();

}
