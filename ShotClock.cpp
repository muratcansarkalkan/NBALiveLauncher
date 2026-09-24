#include "plugin-std.h"
#include "ShotClock.h"
#include "ShotClockGames.h"
#include <Windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <climits>
#include <TlHelp32.h>
#include <d3d9.h>
#include "ScoreboardRenderer.h"
#include "PopupTheme.h"
#include "PopupFont.h"
#include "ScoreboardConfig.h"
#include "StatOverlayTypes.h"

#pragma comment(lib, "d3d9.lib")

using namespace plugin;

namespace {

bool g_debugConsoleEnabled = false;

CRITICAL_SECTION g_logLock;
bool g_logReady = false;

void InitializeDebugLoggingInternal()
{
    char exePath[MAX_PATH] = {};

    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH))
        return;

    char* slash = std::strrchr(exePath, '\\');
    if (!slash)
        return;

    *(slash + 1) = '\0';

    char iniPath[MAX_PATH] = {};
    std::snprintf(
        iniPath,
        sizeof(iniPath),
        "%smain.ini",
        exePath
    );

    g_debugConsoleEnabled =
        GetPrivateProfileIntA(
            "DEBUG",
            "CONSOLE",
            0,
            iniPath
        ) != 0;

    if (!g_debugConsoleEnabled)
        return;

    char logsPath[MAX_PATH] = {};
    std::snprintf(
        logsPath,
        sizeof(logsPath),
        "%slogs",
        exePath
    );

    CreateDirectoryA(logsPath, nullptr);

    InitializeCriticalSection(&g_logLock);
    g_logReady = true;
}

void BuildLogPath(char* outPath, size_t outSize, const char* fileName)
{
    if (!g_debugConsoleEnabled) {
        outPath[0] = '\0';
        return;
    }

    char exePath[MAX_PATH] = {};

    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
        outPath[0] = '\0';
        return;
    }

    char* slash = std::strrchr(exePath, '\\');
    if (!slash) {
        outPath[0] = '\0';
        return;
    }

    *(slash + 1) = '\0';

    std::snprintf(
        outPath,
        outSize,
        "%slogs\\%s",
        exePath,
        fileName
    );
}

struct GdAI { void** __vtable; };
struct GdInfoCentral;
struct IDTeam { int value; };
struct BBallString { char* sharedstring; };

using SendEventFn = int (__cdecl *)(
    void*, const char*, const char*,
    const char*, const char*, const char*, const char*, const char*);
using GetGameClockFn = unsigned int (__thiscall *)(GdInfoCentral*);
using IsShotClockValidFn = bool (__thiscall *)(GdInfoCentral*);
using GetShotClockFn = unsigned int (__thiscall *)(GdInfoCentral*);
using GetTeamScoreFn = int (__thiscall *)(GdInfoCentral*, int);
using GetTeamIDFromSideFn = IDTeam* (__stdcall *)(IDTeam*, int);
using GetTeamValueFn = int (__stdcall *)(int);
using GetOverlayDataFn = DWORD (__thiscall *)(DWORD*, int, int);
using StoreOverlayDataFn = void (__thiscall *)(void*, DWORD*);
using StatsRequestFn = bool (__thiscall *)(void*, void*);
using GetPlayerPackageFn = BBallString* (__thiscall *)(
    void*, BBallString*, const DWORD*);
using PlayerQueryGetIntFn = int (__thiscall *)(void*, int);
using ResolveRuntimePlayerFn = void* (__cdecl *)(const DWORD*, DWORD*);
using BoxScoreInitFn = void (__thiscall *)(void*);
using GetBoxScoreGameFn = void* (__thiscall *)(void*, int, int);
using PlayerGameStatsGetStatFn = int (__thiscall *)(void*, int);
using SubstitutionBuilderFn = bool (__thiscall *)(void*, DWORD*, DWORD*);

const GameAddresses* g_game = nullptr;
SendEventFn g_originalSendEvent = nullptr;
bool g_polling = false;
bool g_gettersDisabled = false;
bool g_scoreboardVisible = false;
bool g_scoreboardSuppressed = false;
bool g_gameplayStarted = false;
bool g_seenIntroOverlayHide = false;
int g_pendingFoulResetQuarter = INT_MIN;
constexpr int MAX_OVERLAY_TYPE = 12;
unsigned int g_lastOverlayHash[MAX_OVERLAY_TYPE + 1] = {};
bool g_seenOverlayType[MAX_OVERLAY_TYPE + 1] = {};

const char* GetOverlayTypeName(int type)
{
    switch (type) {
    case 0: return "Stat";
    case 1: return "Violation";
    case 2: return "PlayCall";
    case 3: return "Intro";
    case 4:
        return g_game && g_game->version == GameVersion::Live2006
            ? "FSS"
            : "Overlay4";
    case 8:
        return g_game && g_game->version == GameVersion::Live2006
            ? "Overlay8"
            : "Unknown";
    case 10:
        return g_game &&
            (g_game->version == GameVersion::Live2007 ||
             g_game->version == GameVersion::Live2008)
            ? "StartingLineupPreGame"
            : "Unknown";
    case 12:
        return g_game && g_game->version == GameVersion::Live2008
            ? "StartingLineupInGame"
            : "Unknown";
    default:
        return "Unknown";
    }
}

struct ExtendedState {
    int quarter;
    int gameValid;
    unsigned int gameRaw;
    unsigned int clockUnitsPerSecond;
    int shotValid;
    unsigned int shotRaw;
    int homeScore;
    int awayScore;
    int homeTeamID;
    int awayTeamID;
    int homeTeamDBID;
    int awayTeamDBID;
    int homeFouls;
    int awayFouls;
    int homeTimeouts;
    int awayTimeouts;
};

ExtendedState g_lastState = {
    INT_MIN, INT_MIN, UINT_MAX, UINT_MAX, INT_MIN, UINT_MAX,
    INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN,
    INT_MIN, INT_MIN, INT_MIN, INT_MIN
};

struct BroadcastIdentity {
    char awayName[64];
    char homeName[64];
    char awayLogoId[16];
    char homeLogoId[16];
    D3DCOLOR awayColor;
    D3DCOLOR homeColor;
};

BroadcastIdentity g_broadcast = {
    "AWAY", "HOME", "", "",
    D3DCOLOR_XRGB(46, 63, 92), D3DCOLOR_XRGB(152, 30, 45)
};

struct ViolationState {
    char title[96];
    char possession[96];
    D3DCOLOR teamColor;
    unsigned int payloadHash;
    DWORD startedAt;
    DWORD pausedAt;
    bool active;
    bool paused;
};

ViolationState g_violation = {};
bool g_violationPresentationSuppressed = false;
DWORD g_violationTransitionHiddenAt = 0;

struct PlayCallState {
    char team[96];
    char call[128];
    char rawColor[32];
    char extra[128];
    D3DCOLOR teamColor;
    unsigned int payloadHash;
    DWORD startedAt;
    bool active;
};

PlayCallState g_playCall = {};
bool g_playCallPresentationSuppressed = false;
DWORD g_playCallTransitionHiddenAt = 0;
bool g_playCallLayoutAvailable = false;

struct IntroState {
    char values[15][128];
    unsigned int payloadHash;
    DWORD startedAt;
    bool active;
};

IntroState g_intro = {};
bool g_introLayoutAvailable = false;
bool g_introPresentationSuppressed = false;
DWORD g_introTransitionHiddenAt = 0;

struct Starting5State {
    char values[11][128];
    unsigned int payloadHash;
    DWORD startedAt;
    bool active;
};

Starting5State g_starting5 = {};
Starting5State g_starting5Pending = {};
bool g_starting5LayoutAvailable = false;

struct OutroState {
    char values[15][128];
    unsigned int payloadHash;
    DWORD startedAt;
    bool active;
};

OutroState g_outro = {};
bool g_outroLayoutAvailable = false;

struct LineupsState {
    char values[14][128];
    unsigned int payloadHash;
    DWORD startedAt;
    bool active;
};

LineupsState g_lineups = {};
bool g_lineupsLayoutAvailable = false;
DWORD g_lineupsTransitionHiddenAt = 0;

struct PlayerFoulState {
    char firstName[64];
    char lastName[64];
    char jerseyNumber[8];
    char label1[64];
    char value1[32];
    char label2[64];
    char value2[32];
    char logoId[32];
    char portraitId[32];
    D3DCOLOR teamColor;
    unsigned int payloadHash;
    DWORD startedAt;
    bool active;
};

PlayerFoulState g_playerFoul = {};
bool g_playerFoulPresentationSuppressed = false;
DWORD g_playerFoulTransitionHiddenAt = 0;
bool g_playerFoulLayoutAvailable = false;
bool g_suppressCurrentStatsRequest = false;
bool g_statsRequestHookInstalled = false;
char g_loggedStatTeamCode[32] = {};

struct GenericStatState {
    char values[15][128];
    int count;
    char jerseyNumber[8];
    char subtypeKey[64];
    char teamCode[32];
    char portraitId[32];
    D3DCOLOR teamColor;
    DWORD startedAt;
    int valueCase;
    bool playerPayload;
    bool active;
};

GenericStatState g_genericStat = {};
bool g_genericStatPresentationSuppressed = false;
DWORD g_genericStatTransitionHiddenAt = 0;
bool g_fullScreenTransitionActive = false;

bool DeferTransientPresentation()
{
    return g_fullScreenTransitionActive && g_game &&
        (g_game->version == GameVersion::Live2007 ||
         g_game->version == GameVersion::Live2008);
}

struct StatCatalogEntry {
    DWORD control14;
    DWORD control18;
    DWORD control1C;
    int count;
    uint64_t nonEmptyMask;
    unsigned int occurrences;
    bool used;
};

constexpr unsigned int MAX_STAT_CATALOG_ENTRIES = 96;
StatCatalogEntry g_statCatalog[MAX_STAT_CATALOG_ENTRIES] = {};

using Direct3DCreate9Fn = IDirect3D9* (WINAPI *)(UINT);
using PresentFn = HRESULT (WINAPI *)(IDirect3DDevice9*, const RECT*,
    const RECT*, HWND, const RGNDATA*);

Direct3DCreate9Fn g_originalDirect3DCreate9 = nullptr;
PresentFn g_originalPresent = nullptr;
void** g_hookedDeviceVtable = nullptr;
bool g_presentReached = false;
HMODULE g_systemD3D9Module = nullptr;
volatile LONG g_d3d9ProbeScheduled = 0;
bool g_reloadKeyWasDown = false;
int g_loggedAwayLogoTeam = INT_MIN;
int g_loggedHomeLogoTeam = INT_MIN;
int g_visibilityPreviousAwayScore = INT_MIN;
int g_visibilityPreviousHomeScore = INT_MIN;
DWORD g_visibilityLastTick = 0;
unsigned int g_scoreboardShowRemaining = 0;
ULONGLONG g_reloadFileStamp = 0;
DWORD g_reloadFileLastCheck = 0;
bool g_customOverlayEnabled = true;
char g_customOverlayName[64] = "TEST";
char g_customOverlayIniPath[MAX_PATH] = {};
char g_customOverlayScoreboardPath[MAX_PATH] = {};

StoreOverlayDataFn g_originalViolationDataStore = nullptr;
StoreOverlayDataFn g_originalPlayCallDataStore = nullptr;
StoreOverlayDataFn g_originalIntroDataStore = nullptr;
StoreOverlayDataFn g_originalStatsDataStore = nullptr;
StoreOverlayDataFn g_originalPresentationDataStore = nullptr;
StatsRequestFn g_originalStatsRequest = nullptr;
GetPlayerPackageFn g_originalGetPlayerPackage = nullptr;
PlayerQueryGetIntFn g_originalPlayerQueryGetInt = nullptr;
bool g_captureFeaturedPlayerQuery = false;
DWORD g_pendingFeaturedPlayerId = 0xFFFFFFFFu;
int g_pendingFeaturedDatabasePlayerId = -1;
int g_pendingFeaturedField85 = -1;
int g_pendingFeaturedField87 = -1;
int g_pendingFeaturedField89 = -1;
DWORD g_pendingFeaturedPlayerTick = 0;
char g_pendingFeaturedPlayerPackage[32] = {};

struct FeaturedLiveStats08 {
    int points;
    int rebounds;
    int assists;
    int steals;
    int blocks;
    int fouls;
    int minutes;
    int raw[0x20];
    bool valid;
};

FeaturedLiveStats08 g_pendingFeaturedLiveStats08 = {};

void CopyText(char* destination, size_t capacity, const char* source);

struct RecentFeaturedIdentity {
    char package[32];
    DWORD nativePlayerId;
    int databasePlayerId;
    int jerseyNumber;
    int position;
    int rosterSlot;
    DWORD capturedAt;
    bool valid;
};

constexpr int kRecentFeaturedIdentityCount = 8;
RecentFeaturedIdentity g_recentFeaturedIdentities[kRecentFeaturedIdentityCount] = {};
unsigned int g_recentFeaturedIdentityWrite = 0;

void RememberFeaturedIdentity(const char* package)
{
    if (!package || !*package || g_pendingFeaturedField85 < -1)
        return;
    RecentFeaturedIdentity& e =
        g_recentFeaturedIdentities[g_recentFeaturedIdentityWrite % kRecentFeaturedIdentityCount];
    std::memset(&e, 0, sizeof(e));
    CopyText(e.package, sizeof(e.package), package);
    e.nativePlayerId = g_pendingFeaturedPlayerId;
    e.databasePlayerId = g_pendingFeaturedDatabasePlayerId;
    e.jerseyNumber = g_pendingFeaturedField85;
    e.position = g_pendingFeaturedField87;
    e.rosterSlot = g_pendingFeaturedField89;
    e.capturedAt = g_pendingFeaturedPlayerTick;
    e.valid = true;
    ++g_recentFeaturedIdentityWrite;
}

const RecentFeaturedIdentity* FindRecentFeaturedIdentity(
    const char* package, DWORD now, DWORD maxAgeMs)
{
    if (!package || !*package)
        return nullptr;
    const RecentFeaturedIdentity* best = nullptr;
    DWORD bestAge = 0xFFFFFFFFu;
    for (int i = 0; i < kRecentFeaturedIdentityCount; ++i) {
        const RecentFeaturedIdentity& e = g_recentFeaturedIdentities[i];
        if (!e.valid || !e.package[0] || _stricmp(e.package, package) != 0)
            continue;
        const DWORD age = now - e.capturedAt;
        if (age <= maxAgeMs && age < bestAge) {
            best = &e;
            bestAge = age;
        }
    }
    return best;
}

ResolveRuntimePlayerFn g_resolveRuntimePlayer08 = nullptr;
PlayerQueryGetIntFn g_getRuntimePlayerInt08 = nullptr;
ResolveRuntimePlayerFn g_resolveRuntimePlayer07 = nullptr;
PlayerQueryGetIntFn g_getRuntimePlayerInt07 = nullptr;
ResolveRuntimePlayerFn g_resolveRuntimePlayer06 = nullptr;
PlayerQueryGetIntFn g_getRuntimePlayerInt06 = nullptr;
ResolveRuntimePlayerFn g_resolveRuntimePlayer05 = nullptr;
PlayerQueryGetIntFn g_getRuntimePlayerInt05 = nullptr;

enum PlayerGameStatsId08 {
    BOXSTAT_FG_MADE=0, BOXSTAT_FG_ATTEMPTS=1, BOXSTAT_3PT_MADE=2, BOXSTAT_3PT_ATTEMPTS=3,
    BOXSTAT_FT_MADE=4, BOXSTAT_FT_ATTEMPTS=5, BOXSTAT_OFF_REBOUNDS=6, BOXSTAT_DEF_REBOUNDS=7,
    BOXSTAT_BLOCKS=8, BOXSTAT_STEALS=9, BOXSTAT_ASSISTS=10, BOXSTAT_TURNOVERS=11,
    BOXSTAT_FOULS=12, BOXSTAT_MINUTES=13, BOXSTAT_POINTS=15, BOXSTAT_REBOUNDS=16
};
struct LivePlayerData08 {
    int nativePlayerId, databasePlayerId, jerseyNumber, currentPosition, initialPosition;
    int courtSlot; // -1 = bench, 0..4 = active lineup slot
    int fgMade, fgAttempts, threeMade, threeAttempts, ftMade, ftAttempts;
    int offensiveRebounds, defensiveRebounds, rebounds, blocks, steals, assists, turnovers, fouls, minutes, points;
    bool onCourt;
    bool valid;
};
struct LiveTeamData08 { int teamId, playerCount; LivePlayerData08 players[12]; };
struct LiveGameData08 { LiveTeamData08 teams[2]; int homeTeamIndex, awayTeamIndex; DWORD refreshedAt; bool valid; };
void* g_boxScoreMgr08 = nullptr;
BoxScoreInitFn g_originalBoxScoreInit08 = nullptr;
GetBoxScoreGameFn g_getBoxScoreGame08 = nullptr;
PlayerGameStatsGetStatFn g_playerGameStatsGetStat08 = nullptr;
LiveGameData08 g_liveGame08 = {};
LiveGameData08 g_liveGame07 = {};
LiveGameData08 g_liveGame06 = {};
LiveGameData08 g_liveGame05 = {};
SubstitutionBuilderFn g_originalSubstitutionBuilder08 = nullptr;
SubstitutionBuilderFn g_originalSubstitutionBuilder07 = nullptr;
SubstitutionBuilderFn g_originalSubstitutionBuilder06 = nullptr;
SubstitutionBuilderFn g_originalSubstitutionBuilder05 = nullptr;
bool g_onCourt08[2][12] = {};
int g_courtSlot08[2][12] = {};
bool g_lineupInitialized08 = false;
bool g_onCourt07[2][12] = {};
int g_courtSlot07[2][12] = {};
bool g_lineupInitialized07 = false;
bool g_onCourt06[2][12] = {};
int g_courtSlot06[2][12] = {};
bool g_lineupInitialized06 = false;
bool g_onCourt05[2][12] = {};
int g_courtSlot05[2][12] = {};
bool g_lineupInitialized05 = false;
bool g_loggedBoxScoreReady05 = false;
bool g_loggedBoxScoreReady06 = false;
bool g_loggedBoxScoreReady07 = false;
bool g_boxScoreCsvKeyWasDown = false;
bool g_loggedBoxScoreReady08 = false;
int g_boxScoreDay08 = -1;
int g_boxScoreGameIndex08 = -1;

void CopyText(char* destination, size_t capacity, const char* source);

bool __stdcall SuppressNativeViolationRequest(void*)
{
    // Matches sub_55B020's one stack argument and returns AL=1 so
    // sub_597270 continues into its normal payload-storage branch.
    return true;
}

bool __stdcall SuppressNativePlayCallRequest(void*)
{
    // All four builders use the same one-argument request convention as the
    // violation builder. Success keeps their normal payload-store path alive.
    return true;
}

bool __stdcall SuppressNativeIntroRequest(void*)
{
    // Keep the builder's success path alive without creating overlays~intro.big.
    return true;
}

bool __stdcall SuppressNativePresentationRequest(void*)
{
    // NBA Live 07/08 Starting5 and Outro use the same one-argument
    // native-movie request convention. Report success so each builder
    // continues through its normal payload-store and finished-state paths
    // without creating the native presentation movie.
    return true;
}

bool __fastcall HookStatsNativeRequest(void* thisPtr, void*, void* request)
{
    if (g_suppressCurrentStatsRequest)
        return true;
    return g_originalStatsRequest ? g_originalStatsRequest(thisPtr, request) : false;
}

int __fastcall HookFeaturedPlayerQueryGetInt(void* thisPtr, void*, int fieldId)
{
    const bool captureIdentityField = g_game &&
        ((g_game->version == GameVersion::Live2008 && fieldId == 0xA0) ||
         (g_game->version == GameVersion::Live2007 && fieldId == 0x8F) ||
         (g_game->version == GameVersion::Live2006 && fieldId == 0x8F));
    if (g_captureFeaturedPlayerQuery && g_originalPlayerQueryGetInt &&
        captureIdentityField) {
        __try {
            // 0x88 is confirmed by the executable's own "PLAYERID = %d"
            // formatting path. The three adjacent compact fields are logged
            // once so the jersey-number field can be identified empirically.
            g_pendingFeaturedField85 =
                g_originalPlayerQueryGetInt(thisPtr, 0x85);
            g_pendingFeaturedField87 =
                g_originalPlayerQueryGetInt(thisPtr, 0x87);
            g_pendingFeaturedDatabasePlayerId =
                g_originalPlayerQueryGetInt(thisPtr, 0x88);
            g_pendingFeaturedField89 =
                g_originalPlayerQueryGetInt(thisPtr, 0x89);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_pendingFeaturedDatabasePlayerId = -1;
            g_pendingFeaturedField85 = -1;
            g_pendingFeaturedField87 = -1;
            g_pendingFeaturedField89 = -1;
        }
    }
    return g_originalPlayerQueryGetInt ?
        g_originalPlayerQueryGetInt(thisPtr, fieldId) : 0;
}

void ResetFeaturedLiveStats08()
{
    std::memset(&g_pendingFeaturedLiveStats08, 0,
        sizeof(g_pendingFeaturedLiveStats08));
    g_pendingFeaturedLiveStats08.points = -1;
    g_pendingFeaturedLiveStats08.rebounds = -1;
    g_pendingFeaturedLiveStats08.assists = -1;
    g_pendingFeaturedLiveStats08.steals = -1;
    g_pendingFeaturedLiveStats08.blocks = -1;
    g_pendingFeaturedLiveStats08.fouls = -1;
    g_pendingFeaturedLiveStats08.minutes = -1;
    for (unsigned int i = 0;
         i < sizeof(g_pendingFeaturedLiveStats08.raw) /
             sizeof(g_pendingFeaturedLiveStats08.raw[0]); ++i)
        g_pendingFeaturedLiveStats08.raw[i] = INT_MIN;
}

bool CaptureFeaturedLiveStats08(const DWORD* playerId)
{
    ResetFeaturedLiveStats08();
    if (!g_game || !playerId)
        return false;

    ResolveRuntimePlayerFn resolver = nullptr;
    PlayerQueryGetIntFn getter = nullptr;
    if (g_game->version == GameVersion::Live2008) {
        resolver = g_resolveRuntimePlayer08;
        getter = g_getRuntimePlayerInt08;
    }
    else if (g_game->version == GameVersion::Live2007) {
        resolver = g_resolveRuntimePlayer07;
        getter = g_getRuntimePlayerInt07;
    }
    else if (g_game->version == GameVersion::Live2006) {
        resolver = g_resolveRuntimePlayer06;
        getter = g_getRuntimePlayerInt06;
    }
    else if (g_game->version == GameVersion::Live2005) {
        resolver = g_resolveRuntimePlayer05;
        getter = g_getRuntimePlayerInt05;
    }
    if (!resolver || !getter)
        return false;

    __try {
        DWORD resolverScratch = 0;
        void* player = resolver(playerId, &resolverScratch);
        if (!player)
            return false;

        // Live 06, 07 and 08 use the same compact live-stat IDs.
        g_pendingFeaturedLiveStats08.assists = getter(player, 0x06);
        g_pendingFeaturedLiveStats08.blocks = getter(player, 0x07);
        g_pendingFeaturedLiveStats08.fouls = getter(player, 0x09);
        g_pendingFeaturedLiveStats08.minutes = getter(player, 0x0B);
        g_pendingFeaturedLiveStats08.steals = getter(player, 0x0C);
        g_pendingFeaturedLiveStats08.points = getter(player, 0x0F);
        g_pendingFeaturedLiveStats08.rebounds = getter(player, 0x10);

        for (int statId = 0; statId < 0x20; ++statId)
            g_pendingFeaturedLiveStats08.raw[statId] = getter(player, statId);

        g_pendingFeaturedLiveStats08.valid = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ResetFeaturedLiveStats08();
        return false;
    }
}

BBallString* __fastcall HookGetPlayerPackage(void* thisPtr, void*,
    BBallString* output, const DWORD* playerId)
{
    g_pendingFeaturedDatabasePlayerId = -1;
    g_pendingFeaturedField85 = -1;
    g_pendingFeaturedField87 = -1;
    g_pendingFeaturedField89 = -1;
    ResetFeaturedLiveStats08();
    g_captureFeaturedPlayerQuery = true;
    BBallString* result = g_originalGetPlayerPackage ?
        g_originalGetPlayerPackage(thisPtr, output, playerId) : output;
    g_captureFeaturedPlayerQuery = false;
    __try {
        if (playerId) {
            g_pendingFeaturedPlayerId = *playerId;
            g_pendingFeaturedPlayerTick = GetTickCount();

            // Live 06 popup builders do not consistently preserve the same
            // query-object identity path used by 07/08. The native player ID
            // passed to GetPlayerPackage is authoritative, and the 06 runtime
            // resolver/getter pair is already proven by the 24-player cache.
            // Resolve the featured player directly so every player popup
            // (including player_foul and injury/update families) receives the
            // same jersey/DBID/position/roster-slot identity as boxscore.csv.
            if (g_game &&
                (g_game->version == GameVersion::Live2005 ||
                 g_game->version == GameVersion::Live2006)) {
                ResolveRuntimePlayerFn directResolver =
                    g_game->version == GameVersion::Live2005
                        ? g_resolveRuntimePlayer05
                        : g_resolveRuntimePlayer06;
                PlayerQueryGetIntFn directGetter =
                    g_game->version == GameVersion::Live2005
                        ? g_getRuntimePlayerInt05
                        : g_getRuntimePlayerInt06;
                if (directResolver && directGetter) {
                    __try {
                        DWORD resolverScratch = 0;
                        void* player = directResolver(playerId, &resolverScratch);
                        if (player) {
                            g_pendingFeaturedField85 = directGetter(player, 0x85);
                            g_pendingFeaturedField87 = directGetter(player, 0x87);
                            g_pendingFeaturedDatabasePlayerId = directGetter(player, 0x88);
                            g_pendingFeaturedField89 = directGetter(player, 0x89);
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                }
            }

            CaptureFeaturedLiveStats08(playerId);
            const BBallString* resolved = result ? result : output;
            CopyText(g_pendingFeaturedPlayerPackage,
                sizeof(g_pendingFeaturedPlayerPackage),
                resolved ? resolved->sharedstring : nullptr);
            RememberFeaturedIdentity(g_pendingFeaturedPlayerPackage);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_captureFeaturedPlayerQuery = false;
        g_pendingFeaturedPlayerId = 0xFFFFFFFFu;
        g_pendingFeaturedDatabasePlayerId = -1;
        g_pendingFeaturedField85 = -1;
        g_pendingFeaturedField87 = -1;
        g_pendingFeaturedField89 = -1;
        ResetFeaturedLiveStats08();
        g_pendingFeaturedPlayerTick = 0;
        g_pendingFeaturedPlayerPackage[0] = '\0';
    }
    return result;
}

bool IsSafeOverlayName(const char* name)
{
    if (!name || !*name || std::strstr(name, "..")) return false;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(name); *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return false;
    }
    return true;
}

void LoadCustomOverlaySettings()
{
    char iniPath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, iniPath, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    char* slash = std::strrchr(iniPath, '\\');
    if (!slash) return;
    std::strcpy(slash + 1, "main.ini");
    std::strncpy(g_customOverlayIniPath, iniPath,
        sizeof(g_customOverlayIniPath) - 1);

    g_customOverlayEnabled = GetPrivateProfileIntA(
        "OVERLAY", "CUSTOM_OVERLAY", 1, iniPath) != 0;

    char configuredName[64] = {};
    GetPrivateProfileStringA("OVERLAY", "CUSTOM_OVERLAY_NAME", "TEST",
        configuredName, sizeof(configuredName), iniPath);
    if (IsSafeOverlayName(configuredName)) {
        std::strncpy(g_customOverlayName, configuredName,
            sizeof(g_customOverlayName) - 1);
        g_customOverlayName[sizeof(g_customOverlayName) - 1] = '\0';
    }
    else
        std::strcpy(g_customOverlayName, "TEST");

    std::snprintf(slash + 1,
        MAX_PATH - static_cast<size_t>(slash + 1 - iniPath),
        "assets\\popups\\%s\\scoreboard\\scoreboard.json", g_customOverlayName);
    std::strncpy(g_customOverlayScoreboardPath, iniPath,
        sizeof(g_customOverlayScoreboardPath) - 1);
}

ULONGLONG GetPopupReloadFileStamp()
{
    char path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return 0;
    char* slash = std::strrchr(path, '\\');
    if (!slash) return 0;
    std::snprintf(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path),
        "assets\\popups\\%s\\.reload", g_customOverlayName);
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return 0;
    return (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        data.ftLastWriteTime.dwLowDateTime;
}

void CopyText(char* destination, size_t capacity, const char* source)
{
    if (!destination || !capacity) return;
    if (!source) source = "";
    std::strncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
}

D3DCOLOR ParsePackedColor(const char* value, D3DCOLOR fallback)
{
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const unsigned long rgb = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || rgb > 0xFFFFFFul)
        return fallback;
    return 0xFF000000u | static_cast<D3DCOLOR>(rgb);
}

bool IsMainScreen(const char* path)
{
    return path && std::strcmp(path, "_level0.gMainScreen") == 0;
}

bool IsLifecycleEvent(const char* name)
{
    if (!name) return false;
    return std::strstr(name, "Pause") != nullptr ||
           std::strstr(name, "Resume") != nullptr ||
           std::strstr(name, "Menu") != nullptr ||
           std::strstr(name, "Overlay") != nullptr ||
           std::strstr(name, "Hide") != nullptr ||
           std::strstr(name, "Show") != nullptr;
}

void WriteEscaped(FILE* file, const char* value)
{
    if (!value) { std::fputs("NULL", file); return; }
    std::fputc('"', file);
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        switch (*p) {
        case '\\': std::fputs("\\\\", file); break;
        case '"':  std::fputs("\\\"", file); break;
        case '\r': std::fputs("\\r", file); break;
        case '\n': std::fputs("\\n", file); break;
        case '\t': std::fputs("\\t", file); break;
        default:
            if (*p >= 32 && *p < 127) std::fputc(*p, file);
            else std::fprintf(file, "\\x%02X", *p);
            break;
        }
    }
    std::fputc('"', file);
}

void AppendDiagnostic(const char* format, ...)
{
    if (!g_debugConsoleEnabled || !g_logReady)
        return;

    char logPath[MAX_PATH] = {};
    BuildLogPath(
        logPath,
        sizeof(logPath),
        "extended_score_state.log"
    );

    if (!logPath[0])
        return;

    EnterCriticalSection(&g_logLock);

    FILE* file = std::fopen(logPath, "a");

    if (file) {
        va_list args;
        va_start(args, format);
        std::vfprintf(file, format, args);
        va_end(args);

        std::fclose(file);
    }

    LeaveCriticalSection(&g_logLock);
}

void LogEvent(uintptr_t caller, const char* path, const char* name,
              const char* const parameters[5])
{
	if (!g_debugConsoleEnabled || !g_logReady)
    return;

    char logPath[MAX_PATH] = {};
    BuildLogPath(
        logPath,
        sizeof(logPath),
        "score_events.log"
    );

    if (!logPath[0])
        return;
    EnterCriticalSection(&g_logLock);
    FILE* file = std::fopen(logPath, "a");
    if (file) {
        std::fprintf(file, "caller=%08X target=",
            static_cast<unsigned int>(caller));
        WriteEscaped(file, path);
        std::fputs(" event=", file);
        WriteEscaped(file, name);
        std::fputs(" args=[", file);
        bool first = true;
        for (int i = 0; i < 5 && parameters[i]; ++i) {
            if (!first) std::fputs(", ", file);
            WriteEscaped(file, parameters[i]);
            first = false;
        }
        std::fputs("]\n", file);
        std::fclose(file);
    }
    LeaveCriticalSection(&g_logLock);
}

unsigned int HashOverlayPayload(int type, const BBallString* values, int count)
{
    unsigned int hash = 2166136261u;
    hash = (hash ^ static_cast<unsigned int>(type)) * 16777619u;
    hash = (hash ^ static_cast<unsigned int>(count)) * 16777619u;
    for (int i = 0; i < count; ++i) {
        const unsigned char* text = reinterpret_cast<const unsigned char*>(
            values[i].sharedstring ? values[i].sharedstring : "");
        while (*text)
            hash = (hash ^ *text++) * 16777619u;
        hash = (hash ^ 0xFFu) * 16777619u;
    }
    return hash;
}

void CaptureBroadcastIdentity(int type, const BBallString* values, int count)
{
    if (type == 3 && count == 15) {
        CopyText(g_broadcast.awayName, sizeof(g_broadcast.awayName),
            values[2].sharedstring);
        CopyText(g_broadcast.homeName, sizeof(g_broadcast.homeName),
            values[5].sharedstring);
        CopyText(g_broadcast.awayLogoId, sizeof(g_broadcast.awayLogoId),
            values[12].sharedstring);
        CopyText(g_broadcast.homeLogoId, sizeof(g_broadcast.homeLogoId),
            values[13].sharedstring);
        g_broadcast.homeColor = ParsePackedColor(
            values[10].sharedstring, g_broadcast.homeColor);
        return;
    }

    // Player cards carry a reliable team logo/color pair.
    if (type == 0 && count == 15) {
        const char* logoId = values[13].sharedstring;
        const D3DCOLOR color = ParsePackedColor(
            values[12].sharedstring, D3DCOLOR_XRGB(48, 48, 48));
        if (logoId && std::strcmp(logoId, g_broadcast.homeLogoId) == 0)
            g_broadcast.homeColor = color;
        else if (logoId && std::strcmp(logoId, g_broadcast.awayLogoId) == 0)
            g_broadcast.awayColor = color;
    }
}

void CaptureViolationPayload(DWORD* vector)
{
    if (!g_customOverlayEnabled || !vector) return;
    __try {
        const int count = static_cast<int>(vector[3]);
        const BBallString* values = reinterpret_cast<const BBallString*>(
            vector[0]);
        if (count != 3 || !values) return;
        const unsigned int hash = HashOverlayPayload(1, values, count);
        if (g_violation.active && g_violation.payloadHash == hash) return;
        CopyText(g_violation.title, sizeof(g_violation.title),
            values[0].sharedstring);
        CopyText(g_violation.possession, sizeof(g_violation.possession),
            values[1].sharedstring);
        g_violation.teamColor = ParsePackedColor(values[2].sharedstring,
            D3DCOLOR_XRGB(40, 40, 40));
        g_violation.payloadHash = hash;
        g_violation.startedAt = GetTickCount();
        g_violation.pausedAt = 0;
        g_violation.paused = false;
        g_violation.active = true;
        // Live 07/08 build transient payloads while their full-screen
        // transition is still visible. Defer there; 05/06 display now.
        g_violationPresentationSuppressed = DeferTransientPresentation();
        g_violationTransitionHiddenAt =
            g_violationPresentationSuppressed ? g_violation.startedAt : 0;
        scoreboardconfig::LoadViolation(g_customOverlayName);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_violation.active = false;
    }
}

void __fastcall HookViolationDataStore(
    void* thisPtr, void*, DWORD* vector)
{
    // sub_597270 has already converted the three violation parameters into
    // the same BBallString vector later returned by GetOverlayData(type 1).
    // Capture it here because the native overlay request immediately before
    // this call is intentionally bypassed.
    CaptureViolationPayload(vector);

    // Preserve the game's internal payload state even though no native movie
    // instance is being requested. This keeps event bookkeeping intact.
    if (g_originalViolationDataStore)
        g_originalViolationDataStore(thisPtr, vector);
}

void CapturePlayCallPayload(DWORD* vector)
{
    if (!g_customOverlayEnabled || !g_playCallLayoutAvailable || !vector)
        return;
    __try {
        const int count = static_cast<int>(vector[3]);
        const BBallString* values = reinterpret_cast<const BBallString*>(
            vector[0]);
        if (count != 4 || !values || !values[1].sharedstring ||
            !*values[1].sharedstring) return;
        const unsigned int hash = HashOverlayPayload(2, values, count);
        if (g_playCall.active && g_playCall.payloadHash == hash) return;
        CopyText(g_playCall.team, sizeof(g_playCall.team),
            values[0].sharedstring);
        CopyText(g_playCall.call, sizeof(g_playCall.call),
            values[1].sharedstring);
        CopyText(g_playCall.rawColor, sizeof(g_playCall.rawColor),
            values[2].sharedstring);
        CopyText(g_playCall.extra, sizeof(g_playCall.extra),
            values[3].sharedstring);
        g_playCall.teamColor = ParsePackedColor(values[2].sharedstring,
            D3DCOLOR_XRGB(40, 40, 40));
        g_playCall.payloadHash = hash;
        g_playCall.startedAt = GetTickCount();
        g_playCall.active = true;
        g_playCallPresentationSuppressed = DeferTransientPresentation();
        g_playCallTransitionHiddenAt =
            g_playCallPresentationSuppressed ? g_playCall.startedAt : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_playCall.active = false;
    }
}

void __fastcall HookPlayCallDataStore(void* thisPtr, void*, DWORD* vector)
{
    CapturePlayCallPayload(vector);
    if (g_originalPlayCallDataStore)
        g_originalPlayCallDataStore(thisPtr, vector);
}

void CaptureIntroPayload(DWORD* vector)
{
    if (!g_customOverlayEnabled || !g_introLayoutAvailable || !vector)
        return;
    __try {
        const int count = static_cast<int>(vector[3]);
        const BBallString* values = reinterpret_cast<const BBallString*>(vector[0]);
        if (count != 15 || !values || !values[0].sharedstring ||
            !*values[0].sharedstring) return;
        const unsigned int hash = HashOverlayPayload(3, values, count);
        // GetOverlayData may expose the same stored intro repeatedly after the
        // animation has completed. Do not create a new presentation merely
        // because the previous instance is no longer active.
        if (g_intro.payloadHash == hash) return;
        for (int i = 0; i < 15; ++i)
            CopyText(g_intro.values[i], sizeof(g_intro.values[i]),
                values[i].sharedstring);
        g_intro.payloadHash = hash;
        g_intro.startedAt = GetTickCount();
        g_intro.active = true;
        // A new accepted payload is a new presentation instance. Do not let a
        // stale HideOverlays/pause flag from the preceding game suppress it.
        g_introPresentationSuppressed = false;
        g_introTransitionHiddenAt = 0;
        CaptureBroadcastIdentity(3, values, count);
        AppendDiagnostic(
            "Intro accepted: homeHeading=%s away=%s %s record=%s "
            "home=%s %s record=%s arena=%s location=%s "
            "awayCode=%s homeCode=%s league=%s.\n",
            g_intro.values[0], g_intro.values[1], g_intro.values[2],
            g_intro.values[3], g_intro.values[4], g_intro.values[5],
            g_intro.values[6], g_intro.values[8], g_intro.values[9],
            g_intro.values[12], g_intro.values[13], g_intro.values[14]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_intro.active = false;
    }
}

void __fastcall HookIntroDataStore(void* thisPtr, void*, DWORD* vector)
{
    CaptureIntroPayload(vector);
    if (g_originalIntroDataStore)
        g_originalIntroDataStore(thisPtr, vector);
}

void LogPresentationPayload(const char* category, DWORD* vector)
{
    if (!g_debugConsoleEnabled || !g_logReady || !category || !vector)
        return;

    __try {
        const int count = static_cast<int>(vector[3]);
        if (count < 0 || count > 128)
            return;

        const BBallString* values = reinterpret_cast<const BBallString*>(
            vector[0]);
        if (count > 0 && !values)
            return;

		char logPath[MAX_PATH] = {};
		BuildLogPath(
			logPath,
			sizeof(logPath),
			"presentation_payloads.log"
		);
		
		if (!logPath[0])
			return;

        FILE* file = nullptr;
        EnterCriticalSection(&g_logLock);
        __try {
            file = std::fopen(logPath, "a");
            if (file) {
                std::fprintf(file,
                    "category=%s count=%d vector=[%08X,%08X,%08X,%08X] "
                    "values=[",
                    category, count,
                    static_cast<unsigned int>(vector[0]),
                    static_cast<unsigned int>(vector[1]),
                    static_cast<unsigned int>(vector[2]),
                    static_cast<unsigned int>(vector[3]));
                for (int i = 0; i < count; ++i) {
                    if (i) std::fputs(", ", file);
                    WriteEscaped(file, values[i].sharedstring);
                }
                std::fputs("]\n", file);
            }
        }
        __finally {
            if (file) std::fclose(file);
            LeaveCriticalSection(&g_logLock);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // A malformed or transient game-owned vector must not affect play.
    }
}

void CaptureStarting5Payload(DWORD* vector)
{
    if (!g_customOverlayEnabled || !g_starting5LayoutAvailable || !vector)
        return;
    __try {
        const int count = static_cast<int>(vector[3]);
        const BBallString* values = reinterpret_cast<const BBallString*>(
            vector[0]);
        if (count != 11 || !values || !values[5].sharedstring ||
            !*values[5].sharedstring)
            return;
        const unsigned int hash = HashOverlayPayload(10, values, count);
        if ((g_starting5.active && g_starting5.payloadHash == hash) ||
            (g_starting5Pending.active &&
             g_starting5Pending.payloadHash == hash))
            return;
        Starting5State* target = g_starting5.active ?
            &g_starting5Pending : &g_starting5;
        for (int i = 0; i < 11; ++i)
            CopyText(target->values[i], sizeof(target->values[i]),
                values[i].sharedstring);
        target->payloadHash = hash;
        target->startedAt = target == &g_starting5 ? GetTickCount() : 0;
        target->active = true;
        AppendDiagnostic(
            "Starting5 accepted: teamCode=%s players=%s,%s,%s,%s,%s "
            "queued=%s.\n",
            target->values[10], target->values[5], target->values[6],
            target->values[7], target->values[8], target->values[9],
            target == &g_starting5Pending ? "yes" : "no");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_starting5.active = false;
        g_starting5Pending.active = false;
    }
}

void CaptureOutroPayload(DWORD* vector)
{
    if (!g_customOverlayEnabled || !g_outroLayoutAvailable || !vector)
        return;
    __try {
        const int count = static_cast<int>(vector[3]);
        const BBallString* values = reinterpret_cast<const BBallString*>(
            vector[0]);
        if (count != 15 || !values || !values[0].sharedstring ||
            !*values[0].sharedstring)
            return;
        for (int i = 0; i < 15; ++i)
            CopyText(g_outro.values[i], sizeof(g_outro.values[i]),
                values[i].sharedstring);
        g_outro.payloadHash = HashOverlayPayload(11, values, count);
        g_outro.startedAt = GetTickCount();
        g_outro.active = true;
        CopyText(g_broadcast.awayName, sizeof(g_broadcast.awayName),
            values[2].sharedstring);
        CopyText(g_broadcast.homeName, sizeof(g_broadcast.homeName),
            values[5].sharedstring);
        CopyText(g_broadcast.awayLogoId, sizeof(g_broadcast.awayLogoId),
            values[12].sharedstring);
        CopyText(g_broadcast.homeLogoId, sizeof(g_broadcast.homeLogoId),
            values[13].sharedstring);
        AppendDiagnostic(
            "Outro accepted: away=%s %s score=%s home=%s %s score=%s "
            "awayCode=%s homeCode=%s.\n",
            g_outro.values[1], g_outro.values[2], g_outro.values[10],
            g_outro.values[4], g_outro.values[5], g_outro.values[11],
            g_outro.values[12], g_outro.values[13]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_outro.active = false;
    }
}

void __fastcall HookStarting5DataStore(void* thisPtr, void*, DWORD* vector)
{
    CaptureStarting5Payload(vector);
    LogPresentationPayload("starting5", vector);
    if (g_originalPresentationDataStore)
        g_originalPresentationDataStore(thisPtr, vector);
}

void __fastcall HookOutroDataStore(void* thisPtr, void*, DWORD* vector)
{
    CaptureOutroPayload(vector);
    LogPresentationPayload("outro", vector);
    if (g_originalPresentationDataStore)
        g_originalPresentationDataStore(thisPtr, vector);
}

void CaptureLineupsPayload(DWORD* vector)
{
    if (!g_customOverlayEnabled || !g_lineupsLayoutAvailable || !vector)
        return;
    __try {
        const int count = static_cast<int>(vector[3]);
        const BBallString* values = reinterpret_cast<const BBallString*>(
            vector[0]);
        if (count != 14 || !values || !values[0].sharedstring ||
            !*values[0].sharedstring)
            return;
        const unsigned int hash = HashOverlayPayload(12, values, count);
        if (g_lineups.active && g_lineups.payloadHash == hash)
            return;
        for (int i = 0; i < 14; ++i)
            CopyText(g_lineups.values[i], sizeof(g_lineups.values[i]),
                values[i].sharedstring);
        g_lineups.payloadHash = hash;
        g_lineups.startedAt = GetTickCount();
        g_lineups.active = true;
        // Live 08 can build this payload while a full-screen transition is
        // still covering the court. Start its visible lifetime only after
        // ShowOverlaysEvent returns gameplay to the screen.
        g_lineupsTransitionHiddenAt = g_fullScreenTransitionActive ?
            g_lineups.startedAt : 0;
        CopyText(g_broadcast.awayName, sizeof(g_broadcast.awayName),
            values[11].sharedstring);
        CopyText(g_broadcast.homeName, sizeof(g_broadcast.homeName),
            values[13].sharedstring);
        CopyText(g_broadcast.awayLogoId, sizeof(g_broadcast.awayLogoId),
            values[10].sharedstring);
        CopyText(g_broadcast.homeLogoId, sizeof(g_broadcast.homeLogoId),
            values[12].sharedstring);
        AppendDiagnostic(
            "In-game lineups accepted: away=%s code=%s home=%s code=%s "
            "players=%s,%s,%s,%s,%s | %s,%s,%s,%s,%s.\n",
            g_lineups.values[11], g_lineups.values[10],
            g_lineups.values[13], g_lineups.values[12],
            g_lineups.values[0], g_lineups.values[1],
            g_lineups.values[2], g_lineups.values[3],
            g_lineups.values[4], g_lineups.values[5],
            g_lineups.values[6], g_lineups.values[7],
            g_lineups.values[8], g_lineups.values[9]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lineups.active = false;
    }
}

void __fastcall HookLineupsDataStore(void* thisPtr, void*, DWORD* vector)
{
    CaptureLineupsPayload(vector);
    LogPresentationPayload("lineups", vector);
    if (g_originalPresentationDataStore)
        g_originalPresentationDataStore(thisPtr, vector);
}

const char* GetKnownStatSubtypeName(DWORD control18, DWORD control1C)
{
    if (!g_game)
        return "unidentified";
    return statoverlay::Resolve(
        g_game->version,
        static_cast<unsigned int>(control18),
        static_cast<unsigned int>(control1C)).key;
}

StatCatalogEntry* FindStatCatalogEntry(
    DWORD control14, DWORD control18, DWORD control1C,
    int count, uint64_t nonEmptyMask, bool& created)
{
    created = false;
    StatCatalogEntry* freeEntry = nullptr;
    for (unsigned int i = 0; i < MAX_STAT_CATALOG_ENTRIES; ++i) {
        StatCatalogEntry& entry = g_statCatalog[i];
        if (!entry.used) {
            if (!freeEntry) freeEntry = &entry;
            continue;
        }
        if (entry.control14 == control14 &&
            entry.control18 == control18 &&
            entry.control1C == control1C &&
            entry.count == count &&
            entry.nonEmptyMask == nonEmptyMask)
            return &entry;
    }
    if (!freeEntry) return nullptr;
    freeEntry->control14 = control14;
    freeEntry->control18 = control18;
    freeEntry->control1C = control1C;
    freeEntry->count = count;
    freeEntry->nonEmptyMask = nonEmptyMask;
    freeEntry->occurrences = 0;
    freeEntry->used = true;
    created = true;
    return freeEntry;
}

void LogCompletedStatPayload(DWORD* payload)
{
    if (!g_debugConsoleEnabled || !g_logReady || !payload)
        return;

    __try {
        const int count = static_cast<int>(payload[3]);
        if (count < 0 || count > 128)
            return;

        const BBallString* values = reinterpret_cast<const BBallString*>(
            payload[0]);
        if (count > 0 && !values)
            return;

        uint64_t nonEmptyMask = 0;
        for (int i = 0; i < count && i < 64; ++i) {
            const char* value = values[i].sharedstring;
            if (value && *value)
                nonEmptyMask |= uint64_t(1) << i;
        }

		char logPath[MAX_PATH] = {};
		BuildLogPath(
			logPath,
			sizeof(logPath),
			"stat_payloads.log"
		);

		if (!logPath[0])
			return;
		
		char logPathCat[MAX_PATH] = {};
		BuildLogPath(
			logPathCat,
			sizeof(logPathCat),
			"stat_catalog.log"
		);
		
		if (!logPathCat[0])
			return;
        FILE* file = nullptr;
        FILE* catalogFile = nullptr;
        EnterCriticalSection(&g_logLock);
        __try {
            file = std::fopen(logPath, "a");
            if (file) {
                std::fprintf(file,
                    "tick=%lu count=%d control14=%u control18=%u "
                    "control1C=%u values=[",
                    static_cast<unsigned long>(GetTickCount()), count,
                    static_cast<unsigned int>(payload[5]),
                    static_cast<unsigned int>(payload[6]),
                    static_cast<unsigned int>(payload[7]));
                for (int i = 0; i < count; ++i) {
                    if (i) std::fputs(", ", file);
                    WriteEscaped(file, values[i].sharedstring);
                }
                std::fputs("]\n", file);
            }

            bool created = false;
            StatCatalogEntry* entry = FindStatCatalogEntry(
                payload[5], payload[6], payload[7], count,
                nonEmptyMask, created);
            if (entry) ++entry->occurrences;
            if (created) {
                catalogFile = std::fopen(logPathCat, "a");
                if (catalogFile) {
                    std::fprintf(catalogFile,
                        "discovered tick=%lu name=%s count=%d "
                        "control14=%u control18=%u control1C=%u "
                        "nonEmptyMask=%016llX values=[",
                        static_cast<unsigned long>(GetTickCount()),
                        GetKnownStatSubtypeName(payload[6], payload[7]),
                        count,
                        static_cast<unsigned int>(payload[5]),
                        static_cast<unsigned int>(payload[6]),
                        static_cast<unsigned int>(payload[7]),
                        static_cast<unsigned long long>(nonEmptyMask));
                    for (int i = 0; i < count; ++i) {
                        if (i) std::fputs(", ", catalogFile);
                        std::fprintf(catalogFile, "%d=", i);
                        WriteEscaped(catalogFile, values[i].sharedstring);
                    }
                    std::fputs("]\n", catalogFile);
                }
            }
        }
        __finally {
            if (file) std::fclose(file);
            if (catalogFile) std::fclose(catalogFile);
            LeaveCriticalSection(&g_logLock);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // A transient or malformed game-owned payload must not affect play.
    }
}

void __fastcall HookStatsDataStore(void* thisPtr, void*, DWORD* payload)
{
    LogCompletedStatPayload(payload);
    bool playerFoul = false;
    bool genericStat = false;
    char matchedFeaturedJersey[8] = {};
    if (g_customOverlayEnabled && payload) {
        __try {
            const int count = static_cast<int>(payload[3]);
            const BBallString* values = reinterpret_cast<const BBallString*>(
                payload[0]);
            // A new native stat request replaces the previous stat instance,
            // even when this particular request falls back to the game.
            g_playerFoul.active = false;
            g_genericStat.active = false;
            const statoverlay::Subtype subtype = g_game ?
                statoverlay::Resolve(g_game->version,
                    static_cast<unsigned int>(payload[6]),
                    static_cast<unsigned int>(payload[7])) :
                statoverlay::Subtype{ "unidentified", false, false };
            if (subtype.playerPayload) {
                const DWORD now = GetTickCount();
                const DWORD age = g_pendingFeaturedPlayerTick ?
                    now - g_pendingFeaturedPlayerTick : 0xFFFFFFFFu;
                const char* payloadPackage = count > 14 && values &&
                    values[14].sharedstring ? values[14].sharedstring : "";
                const bool fresh = age <= 2000u;
                const bool packageMatches = fresh &&
                    g_pendingFeaturedPlayerPackage[0] && payloadPackage[0] &&
                    _stricmp(g_pendingFeaturedPlayerPackage,
                        payloadPackage) == 0;
                const RecentFeaturedIdentity* recentIdentity = nullptr;
                int matchedJerseyNumber = INT_MIN;
                bool matchedDirect06 = false;

                // Live 06 follows the same dedicated popup-parameter path as
                // Live 07: field 0x85 captured for the featured player is the
                // jersey-number value fed into player.jerseyNumber. Do not
                // repurpose an existing native payload slot.
                if (g_game && g_game->version == GameVersion::Live2006 &&
                    fresh && g_pendingFeaturedField85 >= -1) {
                    matchedJerseyNumber = g_pendingFeaturedField85;
                    matchedDirect06 = true;
                }
                else if (packageMatches) {
                    matchedJerseyNumber = g_pendingFeaturedField85;
                }
                else {
                    // Some stat builders perform another player/package query
                    // before StoreCompletedPayload. Keep a short package-keyed
                    // history so dedicated player_foul can recover the player
                    // that actually owns values[14].
                    recentIdentity = FindRecentFeaturedIdentity(
                        payloadPackage, now, 3000u);
                    if (recentIdentity)
                        matchedJerseyNumber = recentIdentity->jerseyNumber;
                }
                if (matchedJerseyNumber == -1)
                    std::snprintf(matchedFeaturedJersey,
                        sizeof(matchedFeaturedJersey), "00");
                else if (matchedJerseyNumber >= 0)
                    std::snprintf(matchedFeaturedJersey,
                        sizeof(matchedFeaturedJersey), "%d",
                        matchedJerseyNumber);
                AppendDiagnostic(
                    "Featured player context: subtype=%s nativePlayerID=%u "
                    "nativePlayerIDHex=%08X databasePlayerID=%d "
                    "jerseyNumber=%d currentPosition=%d initialPosition=%d "
                    "points=%d rebounds=%d assists=%d steals=%d blocks=%d "
                    "fouls=%d minutes=%d liveStatsValid=%s package=%s payloadPackage=%s "
                    "ageMs=%lu matched=%s.\n",
                    subtype.key,
                    static_cast<unsigned int>(g_pendingFeaturedPlayerId),
                    static_cast<unsigned int>(g_pendingFeaturedPlayerId),
                    g_pendingFeaturedDatabasePlayerId,
                    g_pendingFeaturedField85,
                    g_pendingFeaturedField87,
                    g_pendingFeaturedField89,
                    g_pendingFeaturedLiveStats08.points,
                    g_pendingFeaturedLiveStats08.rebounds,
                    g_pendingFeaturedLiveStats08.assists,
                    g_pendingFeaturedLiveStats08.steals,
                    g_pendingFeaturedLiveStats08.blocks,
                    g_pendingFeaturedLiveStats08.fouls,
                    g_pendingFeaturedLiveStats08.minutes,
                    g_pendingFeaturedLiveStats08.valid ? "yes" : "no",
                    g_pendingFeaturedPlayerPackage,
                    payloadPackage,
                    static_cast<unsigned long>(age),
                    matchedDirect06 ? "field85-06" :
                        (packageMatches ? "yes" :
                            (recentIdentity ? "recent" : "no")));
                if (g_pendingFeaturedLiveStats08.valid) {
                    AppendDiagnostic(
                        "Featured player raw stats 00-1F: "
                        "00=%d 01=%d 02=%d 03=%d 04=%d 05=%d 06=%d 07=%d "
                        "08=%d 09=%d 0A=%d 0B=%d 0C=%d 0D=%d 0E=%d 0F=%d "
                        "10=%d 11=%d 12=%d 13=%d 14=%d 15=%d 16=%d 17=%d "
                        "18=%d 19=%d 1A=%d 1B=%d 1C=%d 1D=%d 1E=%d 1F=%d.\n",
                        g_pendingFeaturedLiveStats08.raw[0x00],
                        g_pendingFeaturedLiveStats08.raw[0x01],
                        g_pendingFeaturedLiveStats08.raw[0x02],
                        g_pendingFeaturedLiveStats08.raw[0x03],
                        g_pendingFeaturedLiveStats08.raw[0x04],
                        g_pendingFeaturedLiveStats08.raw[0x05],
                        g_pendingFeaturedLiveStats08.raw[0x06],
                        g_pendingFeaturedLiveStats08.raw[0x07],
                        g_pendingFeaturedLiveStats08.raw[0x08],
                        g_pendingFeaturedLiveStats08.raw[0x09],
                        g_pendingFeaturedLiveStats08.raw[0x0A],
                        g_pendingFeaturedLiveStats08.raw[0x0B],
                        g_pendingFeaturedLiveStats08.raw[0x0C],
                        g_pendingFeaturedLiveStats08.raw[0x0D],
                        g_pendingFeaturedLiveStats08.raw[0x0E],
                        g_pendingFeaturedLiveStats08.raw[0x0F],
                        g_pendingFeaturedLiveStats08.raw[0x10],
                        g_pendingFeaturedLiveStats08.raw[0x11],
                        g_pendingFeaturedLiveStats08.raw[0x12],
                        g_pendingFeaturedLiveStats08.raw[0x13],
                        g_pendingFeaturedLiveStats08.raw[0x14],
                        g_pendingFeaturedLiveStats08.raw[0x15],
                        g_pendingFeaturedLiveStats08.raw[0x16],
                        g_pendingFeaturedLiveStats08.raw[0x17],
                        g_pendingFeaturedLiveStats08.raw[0x18],
                        g_pendingFeaturedLiveStats08.raw[0x19],
                        g_pendingFeaturedLiveStats08.raw[0x1A],
                        g_pendingFeaturedLiveStats08.raw[0x1B],
                        g_pendingFeaturedLiveStats08.raw[0x1C],
                        g_pendingFeaturedLiveStats08.raw[0x1D],
                        g_pendingFeaturedLiveStats08.raw[0x1E],
                        g_pendingFeaturedLiveStats08.raw[0x1F]);
                }
                g_pendingFeaturedPlayerId = 0xFFFFFFFFu;
                g_pendingFeaturedDatabasePlayerId = -1;
                g_pendingFeaturedField85 = -1;
                g_pendingFeaturedField87 = -1;
                g_pendingFeaturedField89 = -1;
                ResetFeaturedLiveStats08();
                g_pendingFeaturedPlayerTick = 0;
                g_pendingFeaturedPlayerPackage[0] = '\0';
            }
            playerFoul = g_playerFoulLayoutAvailable &&
                count >= 15 && values && subtype.supported &&
                std::strcmp(subtype.key, "player_foul") == 0 &&
                ((values[0].sharedstring && *values[0].sharedstring) ||
                 (values[1].sharedstring && *values[1].sharedstring)) &&
                ((values[2].sharedstring && *values[2].sharedstring) ||
                 (values[4].sharedstring && *values[4].sharedstring));
            if (playerFoul) {
                const unsigned int hash = HashOverlayPayload(0, values, count);
                CopyText(g_playerFoul.firstName,
                    sizeof(g_playerFoul.firstName), values[0].sharedstring);
                CopyText(g_playerFoul.lastName,
                    sizeof(g_playerFoul.lastName), values[1].sharedstring);
                CopyText(g_playerFoul.jerseyNumber,
                    sizeof(g_playerFoul.jerseyNumber), matchedFeaturedJersey);
                AppendDiagnostic("Player-foul popup parameter: player.jerseyNumber='%s'.\n",
                    g_playerFoul.jerseyNumber);
                CopyText(g_playerFoul.label1,
                    sizeof(g_playerFoul.label1), values[2].sharedstring);
                CopyText(g_playerFoul.value1,
                    sizeof(g_playerFoul.value1), values[3].sharedstring);
                CopyText(g_playerFoul.label2,
                    sizeof(g_playerFoul.label2), values[4].sharedstring);
                CopyText(g_playerFoul.value2,
                    sizeof(g_playerFoul.value2), values[5].sharedstring);
                CopyText(g_playerFoul.logoId,
                    sizeof(g_playerFoul.logoId), values[13].sharedstring);
                CopyText(g_playerFoul.portraitId,
                    sizeof(g_playerFoul.portraitId), values[14].sharedstring);
                g_playerFoul.teamColor = ParsePackedColor(
                    values[12].sharedstring, D3DCOLOR_XRGB(40, 40, 40));
                g_playerFoul.payloadHash = hash;
                g_playerFoul.startedAt = GetTickCount();
                g_playerFoul.active = true;
                g_playerFoulPresentationSuppressed =
                    DeferTransientPresentation();
                g_playerFoulTransitionHiddenAt =
                    g_playerFoulPresentationSuppressed ?
                        g_playerFoul.startedAt : 0;
            }
            else if (g_customOverlayEnabled && g_statsRequestHookInstalled &&
                subtype.supported && values &&
                ((subtype.playerPayload && count >= 15) ||
                 (!subtype.playerPayload && count >= 14))) {
                bool hasContent = false;
                const int contentCount = 12;
                for (int i = 0; i < count && i < contentCount; ++i) {
                    if (values[i].sharedstring && *values[i].sharedstring) {
                        hasContent = true;
                        break;
                    }
                }
                int valueCase = 0;
                if (subtype.playerPayload) {
                    // Player payloads expose up to five label/value pairs in
                    // raw2..raw11. Count populated pairs independently so a
                    // gap does not select the wrong visual case.
                    for (int pair = 0; pair < 5; ++pair) {
                        const int labelIndex = 2 + pair * 2;
                        const int valueIndex = labelIndex + 1;
                        const bool hasLabel = labelIndex < count &&
                            values[labelIndex].sharedstring &&
                            *values[labelIndex].sharedstring;
                        const bool hasValue = valueIndex < count &&
                            values[valueIndex].sharedstring &&
                            *values[valueIndex].sharedstring;
                        if (hasLabel || hasValue)
                            ++valueCase;
                    }
                }
                else {
                    // Team payload columns are raw1/raw5/raw9,
                    // raw2/raw6/raw10, and raw3/raw7/raw11. A column is
                    // present if its heading or either row contains data.
                    for (int column = 0; column < 3; ++column) {
                        const int indices[3] = {
                            1 + column, 5 + column, 9 + column
                        };
                        bool populated = false;
                        for (int field = 0; field < 3; ++field) {
                            const int index = indices[field];
                            if (index < count && values[index].sharedstring &&
                                *values[index].sharedstring) {
                                populated = true;
                                break;
                            }
                        }
                        if (populated)
                            ++valueCase;
                    }
                    // Message-style team payloads use raw4/raw8 without
                    // headings or numeric columns. They intentionally share
                    // team_1.json; its empty fields simply render nothing.
                    if (valueCase == 0 && hasContent)
                        valueCase = 1;
                }
                genericStat = hasContent &&
                    valueCase > 0 &&
                    scoreboardconfig::LoadStat(g_customOverlayName,
                        subtype.key, subtype.playerPayload, valueCase);
                if (genericStat) {
                    std::memset(&g_genericStat, 0, sizeof(g_genericStat));
                    if (subtype.playerPayload) {
                        CopyText(g_genericStat.jerseyNumber,
                            sizeof(g_genericStat.jerseyNumber),
                            matchedFeaturedJersey);
                        AppendDiagnostic("Generic-stat popup parameter: subtype=%s player.jerseyNumber='%s'.\n",
                            subtype.key, g_genericStat.jerseyNumber);
                    }
                    g_genericStat.count = count < 15 ? count : 15;
                    for (int i = 0; i < g_genericStat.count; ++i)
                        CopyText(g_genericStat.values[i],
                            sizeof(g_genericStat.values[i]),
                            values[i].sharedstring);
                    if (subtype.playerPayload) {
                        // Compact valid pairs into raw2..raw11. Layout files
                        // can always bind pairs 1..5 without knowing which
                        // source pair happened to be empty.
                        for (int i = 2; i <= 11; ++i)
                            g_genericStat.values[i][0] = '\0';
                        int destinationPair = 0;
                        for (int pair = 0; pair < 5; ++pair) {
                            const int labelIndex = 2 + pair * 2;
                            const int valueIndex = labelIndex + 1;
                            const char* label = labelIndex < count ?
                                values[labelIndex].sharedstring : nullptr;
                            const char* value = valueIndex < count ?
                                values[valueIndex].sharedstring : nullptr;
                            if ((!label || !*label) && (!value || !*value))
                                continue;
                            const int destination = 2 + destinationPair * 2;
                            CopyText(g_genericStat.values[destination],
                                sizeof(g_genericStat.values[destination]), label);
                            CopyText(g_genericStat.values[destination + 1],
                                sizeof(g_genericStat.values[destination + 1]), value);
                            ++destinationPair;
                        }
                    }
                    else {
                        // Compact team headings and both value rows together,
                        // preserving the relationship between each column.
                        char compactHeadings[3][128] = {};
                        char compactLeft[3][128] = {};
                        char compactRight[3][128] = {};
                        int destinationColumn = 0;
                        for (int column = 0; column < 3; ++column) {
                            const int headingIndex = 1 + column;
                            const int leftIndex = 5 + column;
                            const int rightIndex = 9 + column;
                            const char* heading = headingIndex < count ?
                                values[headingIndex].sharedstring : nullptr;
                            const char* left = leftIndex < count ?
                                values[leftIndex].sharedstring : nullptr;
                            const char* right = rightIndex < count ?
                                values[rightIndex].sharedstring : nullptr;
                            if ((!heading || !*heading) &&
                                (!left || !*left) && (!right || !*right))
                                continue;
                            CopyText(compactHeadings[destinationColumn],
                                sizeof(compactHeadings[destinationColumn]), heading);
                            CopyText(compactLeft[destinationColumn],
                                sizeof(compactLeft[destinationColumn]), left);
                            CopyText(compactRight[destinationColumn],
                                sizeof(compactRight[destinationColumn]), right);
                            ++destinationColumn;
                        }
                        for (int column = 0; column < 3; ++column) {
                            CopyText(g_genericStat.values[1 + column],
                                sizeof(g_genericStat.values[1 + column]),
                                compactHeadings[column]);
                            CopyText(g_genericStat.values[5 + column],
                                sizeof(g_genericStat.values[5 + column]),
                                compactLeft[column]);
                            CopyText(g_genericStat.values[9 + column],
                                sizeof(g_genericStat.values[9 + column]),
                                compactRight[column]);
                        }
                    }
                    CopyText(g_genericStat.subtypeKey,
                        sizeof(g_genericStat.subtypeKey), subtype.key);
                    CopyText(g_genericStat.teamCode,
                        sizeof(g_genericStat.teamCode),
                        count > 13 ? values[13].sharedstring : "");
                    CopyText(g_genericStat.portraitId,
                        sizeof(g_genericStat.portraitId),
                        count > 14 ? values[14].sharedstring : "");
                    g_genericStat.teamColor = ParsePackedColor(
                        count > 12 ? values[12].sharedstring : "",
                        D3DCOLOR_XRGB(40, 40, 40));
                    g_genericStat.startedAt = GetTickCount();
                    g_genericStat.valueCase = valueCase;
                    g_genericStat.playerPayload = subtype.playerPayload;
                    g_genericStat.active = true;
                    g_genericStatPresentationSuppressed =
                        DeferTransientPresentation();
                    g_genericStatTransitionHiddenAt =
                        g_genericStatPresentationSuppressed ?
                            g_genericStat.startedAt : 0;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            playerFoul = false;
            genericStat = false;
            g_playerFoul.active = false;
            g_genericStat.active = false;
        }
    }

    // sub_584890 still performs all native payload storage/bookkeeping. Its
    // internal movie request is the only operation conditionally suppressed.
    g_suppressCurrentStatsRequest = playerFoul || genericStat;
    __try {
        if (g_originalStatsDataStore)
            g_originalStatsDataStore(thisPtr, payload);
    }
    __finally {
        g_suppressCurrentStatsRequest = false;
    }
}

void LogOverlayPayload(int type, DWORD* vector)
{
    if (!g_debugConsoleEnabled || !g_logReady ||
        type < 0 || type > MAX_OVERLAY_TYPE || !vector)
        return;

    __try {
        const int count = static_cast<int>(vector[3]);
        if (count < 0 || count > 128)
            return;

        const BBallString* values = reinterpret_cast<const BBallString*>(
            vector[0]);
        if (count > 0 && !values)
            return;

        CaptureBroadcastIdentity(type, values, count);

        const unsigned int hash = HashOverlayPayload(type, values, count);
        if (g_seenOverlayType[type] && g_lastOverlayHash[type] == hash)
            return;

        g_seenOverlayType[type] = true;
        g_lastOverlayHash[type] = hash;

		char logPath[MAX_PATH] = {};
		BuildLogPath(
			logPath,
			sizeof(logPath),
			"overlay_payloads.log"
		);

		if (!logPath[0])
			return;

        FILE* file = nullptr;
        EnterCriticalSection(&g_logLock);
        __try {
            file = std::fopen(logPath, "a");
            if (file) {
                std::fprintf(file, "type=%d category=%s count=%d values=[",
                    type, GetOverlayTypeName(type), count);
                for (int i = 0; i < count; ++i) {
                    if (i) std::fputs(", ", file);
                    WriteEscaped(file, values[i].sharedstring);
                }
                std::fputs("]\n", file);
            }
        }
        __finally {
            if (file) std::fclose(file);
            LeaveCriticalSection(&g_logLock);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Ignore malformed or temporarily unavailable overlay payloads.
    }
}

GdAI* GetGdAI()
{
    if (!g_game || !g_game->gdAIAddress) return nullptr;
    return *reinterpret_cast<GdAI**>(g_game->gdAIAddress);
}

GdInfoCentral* GetGdInfoCentral()
{
    if (!g_game || !g_game->gdInfoCentralAddress) return nullptr;
    return *reinterpret_cast<GdInfoCentral**>(g_game->gdInfoCentralAddress);
}

int GetDatabaseTeamID(IDTeam team)
{
    return team.value > 0 ? team.value - 1 : -1;
}

int GetQuarter(GdAI* gdAI)
{
    if (!gdAI || !gdAI->__vtable ||
        g_game->getQuarterSlot == INVALID_SLOT) return -1;
    using Function = int (__thiscall *)(GdAI*);
    const auto function = reinterpret_cast<Function>(
        gdAI->__vtable[g_game->getQuarterSlot]);
    return function(gdAI);
}

bool IsGameClockValid(GdAI* gdAI)
{
    if (!gdAI || !gdAI->__vtable ||
        g_game->isGameClockValidSlot == INVALID_SLOT) return true;
    using Function = bool (__thiscall *)(GdAI*);
    const auto function = reinterpret_cast<Function>(
        gdAI->__vtable[g_game->isGameClockValidSlot]);
    return function(gdAI);
}

int GetTeamScore(GdInfoCentral* info, int side)
{
    const auto function = reinterpret_cast<GetTeamScoreFn>(
        g_game->getTeamScore);
    return function(info, side);
}

IDTeam GetTeamIDFromSide(int side)
{
    IDTeam result = {};
    const auto function = reinterpret_cast<GetTeamIDFromSideFn>(
        g_game->getTeamIDFromSide);
    function(&result, side);
    return result;
}

int GetOptionalTeamValue(uintptr_t address, IDTeam team)
{
    if (!address) return -1;
    const auto function = reinterpret_cast<GetTeamValueFn>(address);
    return function(team.value);
}

void ResetLiveGameData05()
{
    std::memset(&g_liveGame05, 0, sizeof(g_liveGame05));
    std::memset(g_onCourt05, 0, sizeof(g_onCourt05));
    for (int side = 0; side < 2; ++side)
        for (int slot = 0; slot < 12; ++slot)
            g_courtSlot05[side][slot] = -1;
    g_lineupInitialized05 = false;
    g_liveGame05.homeTeamIndex = g_liveGame05.awayTeamIndex = -1;
}

void InitializeLiveLineup05()
{
    if (g_lineupInitialized05) return;
    std::memset(g_onCourt05, 0, sizeof(g_onCourt05));
    for (int side = 0; side < 2; ++side) {
        for (int slot = 0; slot < 12; ++slot)
            g_courtSlot05[side][slot] = -1;
        for (int slot = 0; slot < 5; ++slot) {
            g_onCourt05[side][slot] = true;
            g_courtSlot05[side][slot] = slot;
        }
    }
    g_lineupInitialized05 = true;
}

bool FindLivePlayerLocation05(DWORD nativePlayerId, int& sideOut, int& slotOut)
{
    if (!nativePlayerId || nativePlayerId == 0xFFFFFFFFu) return false;
    for (int side = 0; side < 2; ++side) {
        for (int slot = 0; slot < 12; ++slot) {
            const LivePlayerData08& p = g_liveGame05.teams[side].players[slot];
            if (p.valid && static_cast<DWORD>(p.nativePlayerId) == nativePlayerId) {
                sideOut = side;
                slotOut = slot;
                return true;
            }
        }
    }
    return false;
}

void ApplySubstitutionToLiveLineup05(DWORD incomingId, DWORD outgoingId)
{
    if (!g_liveGame05.valid) return;
    InitializeLiveLineup05();
    int inSide = -1, inSlot = -1, outSide = -1, outSlot = -1;
    const bool haveIn = FindLivePlayerLocation05(incomingId, inSide, inSlot);
    const bool haveOut = FindLivePlayerLocation05(outgoingId, outSide, outSlot);
    if (!haveIn || !haveOut) {
        AppendDiagnostic(
            "NBA Live 05 substitution lineup update unresolved: incoming=%u found=%s outgoing=%u found=%s.\n",
            static_cast<unsigned int>(incomingId), haveIn ? "yes" : "no",
            static_cast<unsigned int>(outgoingId), haveOut ? "yes" : "no");
        return;
    }
    if (inSide != outSide) {
        AppendDiagnostic(
            "NBA Live 05 substitution lineup update rejected: incoming=%u side=%d outgoing=%u side=%d.\n",
            static_cast<unsigned int>(incomingId), inSide,
            static_cast<unsigned int>(outgoingId), outSide);
        return;
    }
    const int inheritedCourtSlot = g_courtSlot05[outSide][outSlot];
    if (inheritedCourtSlot < 0 || inheritedCourtSlot > 4) {
        AppendDiagnostic(
            "NBA Live 05 substitution court-slot update rejected: outgoing native=%u rosterSlot=%d had courtSlot=%d.\n",
            static_cast<unsigned int>(outgoingId), outSlot, inheritedCourtSlot);
        return;
    }
    g_onCourt05[outSide][outSlot] = false;
    g_courtSlot05[outSide][outSlot] = -1;
    g_onCourt05[inSide][inSlot] = true;
    g_courtSlot05[inSide][inSlot] = inheritedCourtSlot;
    g_liveGame05.teams[outSide].players[outSlot].onCourt = false;
    g_liveGame05.teams[outSide].players[outSlot].courtSlot = -1;
    g_liveGame05.teams[inSide].players[inSlot].onCourt = true;
    g_liveGame05.teams[inSide].players[inSlot].courtSlot = inheritedCourtSlot;
    AppendDiagnostic(
        "NBA Live 05 live lineup substitution: side=%s courtSlot=%d outgoing native=%u rosterSlot=%d incoming native=%u rosterSlot=%d.\n",
        inSide == 0 ? "home" : "away", inheritedCourtSlot,
        static_cast<unsigned int>(outgoingId), outSlot,
        static_cast<unsigned int>(incomingId), inSlot);
}

bool __fastcall HookSubstitutionBuilder05(
    void* thisPtr, void*, DWORD* incomingPlayerId, DWORD* outgoingPlayerId)
{
    DWORD incomingId = 0;
    DWORD outgoingId = 0;
    __try {
        if (incomingPlayerId) incomingId = *incomingPlayerId;
        if (outgoingPlayerId) outgoingId = *outgoingPlayerId;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        incomingId = outgoingId = 0;
    }
    const bool result = g_originalSubstitutionBuilder05
        ? g_originalSubstitutionBuilder05(thisPtr, incomingPlayerId, outgoingPlayerId)
        : true;
    if (incomingId && outgoingId)
        ApplySubstitutionToLiveLineup05(incomingId, outgoingId);
    return result;
}

bool FillLiveRuntimePlayerData05(LivePlayerData08& o, DWORD nativePlayerId, void* player)
{
    if (!player || !g_getRuntimePlayerInt05 || nativePlayerId == 0 || nativePlayerId == 0xFFFFFFFFu)
        return false;
    __try {
        std::memset(&o, 0, sizeof(o));
        o.courtSlot = -1;
        o.nativePlayerId = static_cast<int>(nativePlayerId);
        o.databasePlayerId = g_getRuntimePlayerInt05(player, 0x88);
        o.jerseyNumber = g_getRuntimePlayerInt05(player, 0x85);
        o.currentPosition = g_getRuntimePlayerInt05(player, 0x87);
        o.initialPosition = g_getRuntimePlayerInt05(player, 0x89);
        o.threeAttempts = g_getRuntimePlayerInt05(player, 0x00);
        o.threeMade = g_getRuntimePlayerInt05(player, 0x01);
        o.fgAttempts = g_getRuntimePlayerInt05(player, 0x02);
        o.fgMade = g_getRuntimePlayerInt05(player, 0x03);
        o.ftAttempts = g_getRuntimePlayerInt05(player, 0x04);
        o.ftMade = g_getRuntimePlayerInt05(player, 0x05);
        o.assists = g_getRuntimePlayerInt05(player, 0x06);
        o.blocks = g_getRuntimePlayerInt05(player, 0x07);
        o.defensiveRebounds = g_getRuntimePlayerInt05(player, 0x08);
        o.fouls = g_getRuntimePlayerInt05(player, 0x09);
        o.offensiveRebounds = g_getRuntimePlayerInt05(player, 0x0A);
        o.minutes = g_getRuntimePlayerInt05(player, 0x0B);
        o.steals = g_getRuntimePlayerInt05(player, 0x0C);
        o.turnovers = g_getRuntimePlayerInt05(player, 0x0D);
        o.points = g_getRuntimePlayerInt05(player, 0x0F);
        o.rebounds = g_getRuntimePlayerInt05(player, 0x10);
        o.valid = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        o.valid = false;
        return false;
    }
}

bool RefreshLiveGameData05(const ExtendedState& state)
{
    if (!g_game || g_game->version != GameVersion::Live2005 ||
        !g_resolveRuntimePlayer05 || !g_getRuntimePlayerInt05)
        return false;
    __try {
        void* manager = *reinterpret_cast<void**>(0x00C49014);
        if (!manager) {
            ResetLiveGameData05();
            return false;
        }
        DWORD* nativeIds = reinterpret_cast<DWORD*>(
            static_cast<unsigned char*>(manager) + 0x08);
        LiveGameData08 n = {};
        n.homeTeamIndex = 0;
        n.awayTeamIndex = 1;
        n.teams[0].teamId = state.homeTeamDBID >= 0 ? state.homeTeamDBID : state.homeTeamID;
        n.teams[1].teamId = state.awayTeamDBID >= 0 ? state.awayTeamDBID : state.awayTeamID;
        int resolved[2] = {0, 0};
        InitializeLiveLineup05();
        for (int managerSlot = 0; managerSlot < 24; ++managerSlot) {
            const DWORD nativeId = nativeIds[managerSlot];
            if (nativeId == 0 || nativeId == 0xFFFFFFFFu)
                continue;
            DWORD resolverType = 0;
            void* player = g_resolveRuntimePlayer05(&nativeId, &resolverType);
            if (!player)
                continue;
            const int side = managerSlot < 12 ? 0 : 1;
            LivePlayerData08 temp = {};
            if (!FillLiveRuntimePlayerData05(temp, nativeId, player))
                continue;
            int rosterSlot = temp.initialPosition;
            if (rosterSlot < 0 || rosterSlot >= 12)
                rosterSlot = managerSlot % 12;
            temp.onCourt = g_onCourt05[side][rosterSlot];
            temp.courtSlot = g_courtSlot05[side][rosterSlot];
            n.teams[side].players[rosterSlot] = temp;
            ++resolved[side];
        }
        n.teams[0].playerCount = resolved[0];
        n.teams[1].playerCount = resolved[1];
        n.refreshedAt = GetTickCount();
        n.valid = resolved[0] > 0 && resolved[1] > 0;
        g_liveGame05 = n;
        if (n.valid && !g_loggedBoxScoreReady05) {
            AppendDiagnostic(
                "NBA Live 05 live-player cache ready: home=%d(%d players) away=%d(%d players), manager=%p. F6 exports boxscore.csv; onCourt/courtSlot track substitutions.\n",
                n.teams[0].teamId, n.teams[0].playerCount,
                n.teams[1].teamId, n.teams[1].playerCount, manager);
            g_loggedBoxScoreReady05 = true;
        }
        return n.valid;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ResetLiveGameData05();
        return false;
    }
}

void ResetLiveGameData07()
{
    std::memset(&g_liveGame07, 0, sizeof(g_liveGame07));
    std::memset(g_onCourt07, 0, sizeof(g_onCourt07));
    for (int side = 0; side < 2; ++side)
        for (int slot = 0; slot < 12; ++slot)
            g_courtSlot07[side][slot] = -1;
    g_lineupInitialized07 = false;
    g_liveGame07.homeTeamIndex = g_liveGame07.awayTeamIndex = -1;
}

void InitializeLiveLineup07()
{
    if (g_lineupInitialized07) return;
    std::memset(g_onCourt07, 0, sizeof(g_onCourt07));
    for (int side = 0; side < 2; ++side) {
        for (int slot = 0; slot < 12; ++slot)
            g_courtSlot07[side][slot] = -1;
        for (int slot = 0; slot < 5; ++slot) {
            g_onCourt07[side][slot] = true;
            g_courtSlot07[side][slot] = slot;
        }
    }
    g_lineupInitialized07 = true;
}

bool FindLivePlayerLocation07(DWORD nativePlayerId, int& sideOut, int& slotOut)
{
    if (!nativePlayerId || nativePlayerId == 0xFFFFFFFFu) return false;
    for (int side = 0; side < 2; ++side) {
        for (int slot = 0; slot < 12; ++slot) {
            const LivePlayerData08& p = g_liveGame07.teams[side].players[slot];
            if (p.valid && static_cast<DWORD>(p.nativePlayerId) == nativePlayerId) {
                sideOut = side;
                slotOut = slot;
                return true;
            }
        }
    }
    return false;
}

void ApplySubstitutionToLiveLineup07(DWORD incomingId, DWORD outgoingId)
{
    if (!g_liveGame07.valid) return;
    InitializeLiveLineup07();
    int inSide = -1, inSlot = -1, outSide = -1, outSlot = -1;
    const bool haveIn = FindLivePlayerLocation07(incomingId, inSide, inSlot);
    const bool haveOut = FindLivePlayerLocation07(outgoingId, outSide, outSlot);
    if (!haveIn || !haveOut) {
        AppendDiagnostic(
            "NBA Live 07 substitution lineup update unresolved: incoming=%u found=%s outgoing=%u found=%s.\n",
            static_cast<unsigned int>(incomingId), haveIn ? "yes" : "no",
            static_cast<unsigned int>(outgoingId), haveOut ? "yes" : "no");
        return;
    }
    if (inSide != outSide) {
        AppendDiagnostic(
            "NBA Live 07 substitution lineup update rejected: incoming=%u side=%d outgoing=%u side=%d.\n",
            static_cast<unsigned int>(incomingId), inSide,
            static_cast<unsigned int>(outgoingId), outSide);
        return;
    }
    const int inheritedCourtSlot = g_courtSlot07[outSide][outSlot];
    if (inheritedCourtSlot < 0 || inheritedCourtSlot > 4) {
        AppendDiagnostic(
            "NBA Live 07 substitution court-slot update rejected: outgoing native=%u rosterSlot=%d had courtSlot=%d.\n",
            static_cast<unsigned int>(outgoingId), outSlot, inheritedCourtSlot);
        return;
    }
    g_onCourt07[outSide][outSlot] = false;
    g_courtSlot07[outSide][outSlot] = -1;
    g_onCourt07[inSide][inSlot] = true;
    g_courtSlot07[inSide][inSlot] = inheritedCourtSlot;
    g_liveGame07.teams[outSide].players[outSlot].onCourt = false;
    g_liveGame07.teams[outSide].players[outSlot].courtSlot = -1;
    g_liveGame07.teams[inSide].players[inSlot].onCourt = true;
    g_liveGame07.teams[inSide].players[inSlot].courtSlot = inheritedCourtSlot;
    AppendDiagnostic(
        "NBA Live 07 live lineup substitution: side=%s courtSlot=%d outgoing native=%u rosterSlot=%d incoming native=%u rosterSlot=%d.\n",
        inSide == 0 ? "home" : "away", inheritedCourtSlot,
        static_cast<unsigned int>(outgoingId), outSlot,
        static_cast<unsigned int>(incomingId), inSlot);
}

bool __fastcall HookSubstitutionBuilder07(
    void* thisPtr, void*, DWORD* incomingPlayerId, DWORD* outgoingPlayerId)
{
    DWORD incomingId = 0;
    DWORD outgoingId = 0;
    __try {
        if (incomingPlayerId) incomingId = *incomingPlayerId;
        if (outgoingPlayerId) outgoingId = *outgoingPlayerId;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        incomingId = outgoingId = 0;
    }
    const bool result = g_originalSubstitutionBuilder07
        ? g_originalSubstitutionBuilder07(thisPtr, incomingPlayerId, outgoingPlayerId)
        : true;
    if (incomingId && outgoingId)
        ApplySubstitutionToLiveLineup07(incomingId, outgoingId);
    return result;
}

bool FillLiveRuntimePlayerData07(LivePlayerData08& o, DWORD nativePlayerId, void* player)
{
    if (!player || !g_getRuntimePlayerInt07 || nativePlayerId == 0 || nativePlayerId == 0xFFFFFFFFu)
        return false;
    __try {
        std::memset(&o, 0, sizeof(o));
        o.courtSlot = -1;
        o.nativePlayerId = static_cast<int>(nativePlayerId);
        o.databasePlayerId = g_getRuntimePlayerInt07(player, 0x88);
        o.jerseyNumber = g_getRuntimePlayerInt07(player, 0x85);
        o.currentPosition = g_getRuntimePlayerInt07(player, 0x87);
        o.initialPosition = g_getRuntimePlayerInt07(player, 0x89);
        o.threeAttempts = g_getRuntimePlayerInt07(player, 0x00);
        o.threeMade = g_getRuntimePlayerInt07(player, 0x01);
        o.fgAttempts = g_getRuntimePlayerInt07(player, 0x02);
        o.fgMade = g_getRuntimePlayerInt07(player, 0x03);
        o.ftAttempts = g_getRuntimePlayerInt07(player, 0x04);
        o.ftMade = g_getRuntimePlayerInt07(player, 0x05);
        o.assists = g_getRuntimePlayerInt07(player, 0x05);
        o.blocks = g_getRuntimePlayerInt07(player, 0x07);
        o.defensiveRebounds = g_getRuntimePlayerInt07(player, 0x08);
        o.fouls = g_getRuntimePlayerInt07(player, 0x09);
        o.offensiveRebounds = g_getRuntimePlayerInt07(player, 0x0A);
        o.minutes = g_getRuntimePlayerInt07(player, 0x0B);
        o.steals = g_getRuntimePlayerInt07(player, 0x0C);
        o.turnovers = g_getRuntimePlayerInt07(player, 0x0D);
        o.points = g_getRuntimePlayerInt07(player, 0x0F);
        o.rebounds = g_getRuntimePlayerInt07(player, 0x10);
        o.valid = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        o.valid = false;
        return false;
    }
}

void ResetLiveGameData06()
{
    std::memset(&g_liveGame06, 0, sizeof(g_liveGame06));
    std::memset(g_onCourt06, 0, sizeof(g_onCourt06));
    for (int side = 0; side < 2; ++side)
        for (int slot = 0; slot < 12; ++slot)
            g_courtSlot06[side][slot] = -1;
    g_lineupInitialized06 = false;
    g_liveGame06.homeTeamIndex = g_liveGame06.awayTeamIndex = -1;
}

void InitializeLiveLineup06()
{
    if (g_lineupInitialized06) return;
    std::memset(g_onCourt06, 0, sizeof(g_onCourt06));
    for (int side = 0; side < 2; ++side) {
        for (int slot = 0; slot < 12; ++slot)
            g_courtSlot06[side][slot] = -1;
        for (int slot = 0; slot < 5; ++slot) {
            g_onCourt06[side][slot] = true;
            g_courtSlot06[side][slot] = slot;
        }
    }
    g_lineupInitialized06 = true;
}

bool FindLivePlayerLocation06(DWORD nativePlayerId, int& sideOut, int& slotOut)
{
    if (!nativePlayerId || nativePlayerId == 0xFFFFFFFFu) return false;
    for (int side = 0; side < 2; ++side) {
        for (int slot = 0; slot < 12; ++slot) {
            const LivePlayerData08& p = g_liveGame06.teams[side].players[slot];
            if (p.valid && static_cast<DWORD>(p.nativePlayerId) == nativePlayerId) {
                sideOut = side;
                slotOut = slot;
                return true;
            }
        }
    }
    return false;
}

void ApplySubstitutionToLiveLineup06(DWORD incomingId, DWORD outgoingId)
{
    if (!g_liveGame06.valid) return;
    InitializeLiveLineup06();
    int inSide = -1, inSlot = -1, outSide = -1, outSlot = -1;
    const bool haveIn = FindLivePlayerLocation06(incomingId, inSide, inSlot);
    const bool haveOut = FindLivePlayerLocation06(outgoingId, outSide, outSlot);
    if (!haveIn || !haveOut) {
        AppendDiagnostic(
            "NBA Live 06 substitution lineup update unresolved: incoming=%u found=%s outgoing=%u found=%s.\n",
            static_cast<unsigned int>(incomingId), haveIn ? "yes" : "no",
            static_cast<unsigned int>(outgoingId), haveOut ? "yes" : "no");
        return;
    }
    if (inSide != outSide) {
        AppendDiagnostic(
            "NBA Live 06 substitution lineup update rejected: incoming=%u side=%d outgoing=%u side=%d.\n",
            static_cast<unsigned int>(incomingId), inSide,
            static_cast<unsigned int>(outgoingId), outSide);
        return;
    }
    const int inheritedCourtSlot = g_courtSlot06[outSide][outSlot];
    if (inheritedCourtSlot < 0 || inheritedCourtSlot > 4) {
        AppendDiagnostic(
            "NBA Live 06 substitution court-slot update rejected: outgoing native=%u rosterSlot=%d had courtSlot=%d.\n",
            static_cast<unsigned int>(outgoingId), outSlot, inheritedCourtSlot);
        return;
    }
    g_onCourt06[outSide][outSlot] = false;
    g_courtSlot06[outSide][outSlot] = -1;
    g_onCourt06[inSide][inSlot] = true;
    g_courtSlot06[inSide][inSlot] = inheritedCourtSlot;
    g_liveGame06.teams[outSide].players[outSlot].onCourt = false;
    g_liveGame06.teams[outSide].players[outSlot].courtSlot = -1;
    g_liveGame06.teams[inSide].players[inSlot].onCourt = true;
    g_liveGame06.teams[inSide].players[inSlot].courtSlot = inheritedCourtSlot;
    AppendDiagnostic(
        "NBA Live 06 live lineup substitution: side=%s courtSlot=%d outgoing native=%u rosterSlot=%d incoming native=%u rosterSlot=%d.\n",
        inSide == 0 ? "home" : "away", inheritedCourtSlot,
        static_cast<unsigned int>(outgoingId), outSlot,
        static_cast<unsigned int>(incomingId), inSlot);
}

bool __fastcall HookSubstitutionBuilder06(
    void* thisPtr, void*, DWORD* incomingPlayerId, DWORD* outgoingPlayerId)
{
    DWORD incomingId = 0;
    DWORD outgoingId = 0;
    __try {
        if (incomingPlayerId) incomingId = *incomingPlayerId;
        if (outgoingPlayerId) outgoingId = *outgoingPlayerId;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        incomingId = outgoingId = 0;
    }
    const bool result = g_originalSubstitutionBuilder06
        ? g_originalSubstitutionBuilder06(thisPtr, incomingPlayerId, outgoingPlayerId)
        : true;
    if (incomingId && outgoingId)
        ApplySubstitutionToLiveLineup06(incomingId, outgoingId);
    return result;
}

bool FillLiveRuntimePlayerData06(LivePlayerData08& o, DWORD nativePlayerId, void* player)
{
    if (!player || !g_getRuntimePlayerInt06 || nativePlayerId == 0 || nativePlayerId == 0xFFFFFFFFu)
        return false;
    __try {
        std::memset(&o, 0, sizeof(o));
        o.courtSlot = -1;
        o.nativePlayerId = static_cast<int>(nativePlayerId);
        o.databasePlayerId = g_getRuntimePlayerInt06(player, 0x88);
        o.jerseyNumber = g_getRuntimePlayerInt06(player, 0x85);
        o.currentPosition = g_getRuntimePlayerInt06(player, 0x87);
        o.initialPosition = g_getRuntimePlayerInt06(player, 0x89);
        o.threeAttempts = g_getRuntimePlayerInt06(player, 0x00);
        o.threeMade = g_getRuntimePlayerInt06(player, 0x01);
        o.fgAttempts = g_getRuntimePlayerInt06(player, 0x02);
        o.fgMade = g_getRuntimePlayerInt06(player, 0x03);
        o.ftAttempts = g_getRuntimePlayerInt06(player, 0x04);
        o.ftMade = g_getRuntimePlayerInt06(player, 0x05);
        o.assists = g_getRuntimePlayerInt06(player, 0x06);
        o.blocks = g_getRuntimePlayerInt06(player, 0x07);
        o.defensiveRebounds = g_getRuntimePlayerInt06(player, 0x08);
        o.fouls = g_getRuntimePlayerInt06(player, 0x09);
        o.offensiveRebounds = g_getRuntimePlayerInt06(player, 0x0A);
        o.minutes = g_getRuntimePlayerInt06(player, 0x0B);
        o.steals = g_getRuntimePlayerInt06(player, 0x0C);
        o.turnovers = g_getRuntimePlayerInt06(player, 0x0D);
        o.points = g_getRuntimePlayerInt06(player, 0x0F);
        o.rebounds = g_getRuntimePlayerInt06(player, 0x10);
        o.valid = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        o.valid = false;
        return false;
    }
}

bool RefreshLiveGameData06(const ExtendedState& state)
{
    if (!g_game || g_game->version != GameVersion::Live2006 ||
        !g_resolveRuntimePlayer06 || !g_getRuntimePlayerInt06)
        return false;
    __try {
        void* manager = *reinterpret_cast<void**>(0x00CB1D30);
        if (!manager) {
            ResetLiveGameData06();
            return false;
        }
        DWORD* nativeIds = reinterpret_cast<DWORD*>(
            static_cast<unsigned char*>(manager) + 0x08);
        LiveGameData08 n = {};
        n.homeTeamIndex = 0;
        n.awayTeamIndex = 1;
        n.teams[0].teamId = state.homeTeamDBID >= 0 ? state.homeTeamDBID : state.homeTeamID;
        n.teams[1].teamId = state.awayTeamDBID >= 0 ? state.awayTeamDBID : state.awayTeamID;
        int resolved[2] = {0, 0};
        InitializeLiveLineup06();
        for (int managerSlot = 0; managerSlot < 24; ++managerSlot) {
            const DWORD nativeId = nativeIds[managerSlot];
            if (nativeId == 0 || nativeId == 0xFFFFFFFFu)
                continue;
            DWORD resolverType = 0;
            void* player = g_resolveRuntimePlayer06(&nativeId, &resolverType);
            if (!player)
                continue;
            const int side = managerSlot < 12 ? 0 : 1;
            LivePlayerData08 temp = {};
            if (!FillLiveRuntimePlayerData06(temp, nativeId, player))
                continue;
            int rosterSlot = temp.initialPosition;
            if (rosterSlot < 0 || rosterSlot >= 12)
                rosterSlot = managerSlot % 12;
            temp.onCourt = g_onCourt06[side][rosterSlot];
            temp.courtSlot = g_courtSlot06[side][rosterSlot];
            n.teams[side].players[rosterSlot] = temp;
            ++resolved[side];
        }
        n.teams[0].playerCount = resolved[0];
        n.teams[1].playerCount = resolved[1];
        n.refreshedAt = GetTickCount();
        n.valid = resolved[0] > 0 && resolved[1] > 0;
        g_liveGame06 = n;
        if (n.valid && !g_loggedBoxScoreReady06) {
            AppendDiagnostic(
                "NBA Live 06 live-player cache ready: home=%d(%d players) away=%d(%d players), manager=%p. F6 exports boxscore.csv; onCourt/courtSlot track substitutions.\n",
                n.teams[0].teamId, n.teams[0].playerCount,
                n.teams[1].teamId, n.teams[1].playerCount, manager);
            g_loggedBoxScoreReady06 = true;
        }
        return n.valid;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ResetLiveGameData06();
        return false;
    }
}

bool RefreshLiveGameData07(const ExtendedState& state)
{
    if (!g_game || g_game->version != GameVersion::Live2007 ||
        !g_resolveRuntimePlayer07 || !g_getRuntimePlayerInt07)
        return false;
    __try {
        void* manager = *reinterpret_cast<void**>(0x00CEAA08);
        if (!manager) {
            ResetLiveGameData07();
            return false;
        }
        DWORD* nativeIds = reinterpret_cast<DWORD*>(
            static_cast<unsigned char*>(manager) + 0x08);
        LiveGameData08 n = {};
        n.homeTeamIndex = 0;
        n.awayTeamIndex = 1;
        n.teams[0].teamId = state.homeTeamDBID >= 0 ? state.homeTeamDBID : state.homeTeamID;
        n.teams[1].teamId = state.awayTeamDBID >= 0 ? state.awayTeamDBID : state.awayTeamID;
        int resolved[2] = {0, 0};
        InitializeLiveLineup07();
        for (int managerSlot = 0; managerSlot < 24; ++managerSlot) {
            const DWORD nativeId = nativeIds[managerSlot];
            if (nativeId == 0 || nativeId == 0xFFFFFFFFu)
                continue;
            DWORD resolverType = 0;
            void* player = g_resolveRuntimePlayer07(&nativeId, &resolverType);
            if (!player)
                continue;
            const int side = managerSlot < 12 ? 0 : 1;
            LivePlayerData08 temp = {};
            if (!FillLiveRuntimePlayerData07(temp, nativeId, player))
                continue;
            int rosterSlot = temp.initialPosition;
            if (rosterSlot < 0 || rosterSlot >= 12)
                rosterSlot = managerSlot % 12;
            temp.onCourt = g_onCourt07[side][rosterSlot];
            temp.courtSlot = g_courtSlot07[side][rosterSlot];
            n.teams[side].players[rosterSlot] = temp;
            ++resolved[side];
        }
        n.teams[0].playerCount = resolved[0];
        n.teams[1].playerCount = resolved[1];
        n.refreshedAt = GetTickCount();
        n.valid = resolved[0] > 0 && resolved[1] > 0;
        g_liveGame07 = n;
        if (n.valid && !g_loggedBoxScoreReady07) {
            AppendDiagnostic(
                "NBA Live 07 live-player cache ready: home=%d(%d players) away=%d(%d players), manager=%p. F6 exports boxscore.csv; onCourt/courtSlot track substitutions.\n",
                n.teams[0].teamId, n.teams[0].playerCount,
                n.teams[1].teamId, n.teams[1].playerCount, manager);
            g_loggedBoxScoreReady07 = true;
        }
        return n.valid;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ResetLiveGameData07();
        return false;
    }
}

void ResetLiveGameData08()
{
    std::memset(&g_liveGame08, 0, sizeof(g_liveGame08));
    std::memset(g_onCourt08, 0, sizeof(g_onCourt08));
    for (int side = 0; side < 2; ++side)
        for (int slot = 0; slot < 12; ++slot)
            g_courtSlot08[side][slot] = -1;
    g_lineupInitialized08 = false;
    g_liveGame08.homeTeamIndex = g_liveGame08.awayTeamIndex = -1;
    g_boxScoreDay08 = -1;
    g_boxScoreGameIndex08 = -1;
}

void InitializeLiveLineup08()
{
    if (g_lineupInitialized08) return;
    std::memset(g_onCourt08, 0, sizeof(g_onCourt08));
    for (int side = 0; side < 2; ++side) {
        for (int slot = 0; slot < 12; ++slot)
            g_courtSlot08[side][slot] = -1;
        for (int slot = 0; slot < 5; ++slot) {
            g_onCourt08[side][slot] = true;
            g_courtSlot08[side][slot] = slot;
        }
    }
    g_lineupInitialized08 = true;
}

bool FindLivePlayerLocation08(DWORD nativePlayerId, int& sideOut, int& slotOut)
{
    if (!nativePlayerId || nativePlayerId == 0xFFFFFFFFu) return false;
    for (int side = 0; side < 2; ++side) {
        for (int slot = 0; slot < 12; ++slot) {
            const LivePlayerData08& p = g_liveGame08.teams[side].players[slot];
            if (p.valid && static_cast<DWORD>(p.nativePlayerId) == nativePlayerId) {
                sideOut = side;
                slotOut = slot;
                return true;
            }
        }
    }
    return false;
}

void ApplySubstitutionToLiveLineup08(DWORD incomingId, DWORD outgoingId)
{
    if (!g_liveGame08.valid) return;
    InitializeLiveLineup08();

    int inSide = -1, inSlot = -1, outSide = -1, outSlot = -1;
    const bool haveIn = FindLivePlayerLocation08(incomingId, inSide, inSlot);
    const bool haveOut = FindLivePlayerLocation08(outgoingId, outSide, outSlot);
    if (!haveIn || !haveOut) {
        AppendDiagnostic(
            "NBA Live 08 substitution lineup update unresolved: incoming=%u found=%s outgoing=%u found=%s.\n",
            static_cast<unsigned int>(incomingId), haveIn ? "yes" : "no",
            static_cast<unsigned int>(outgoingId), haveOut ? "yes" : "no");
        return;
    }

    if (inSide != outSide) {
        AppendDiagnostic(
            "NBA Live 08 substitution lineup update rejected: incoming=%u side=%d outgoing=%u side=%d.\n",
            static_cast<unsigned int>(incomingId), inSide,
            static_cast<unsigned int>(outgoingId), outSide);
        return;
    }

    int inheritedCourtSlot = g_courtSlot08[outSide][outSlot];
    if (inheritedCourtSlot < 0 || inheritedCourtSlot > 4) {
        AppendDiagnostic(
            "NBA Live 08 substitution court-slot update rejected: outgoing native=%u rosterSlot=%d had courtSlot=%d.\n",
            static_cast<unsigned int>(outgoingId), outSlot, inheritedCourtSlot);
        return;
    }

    g_onCourt08[outSide][outSlot] = false;
    g_courtSlot08[outSide][outSlot] = -1;
    g_onCourt08[inSide][inSlot] = true;
    g_courtSlot08[inSide][inSlot] = inheritedCourtSlot;

    g_liveGame08.teams[outSide].players[outSlot].onCourt = false;
    g_liveGame08.teams[outSide].players[outSlot].courtSlot = -1;
    g_liveGame08.teams[inSide].players[inSlot].onCourt = true;
    g_liveGame08.teams[inSide].players[inSlot].courtSlot = inheritedCourtSlot;

    AppendDiagnostic(
        "NBA Live 08 live lineup substitution: side=%s courtSlot=%d outgoing native=%u rosterSlot=%d incoming native=%u rosterSlot=%d.\n",
        inSide == 0 ? "home" : "away", inheritedCourtSlot,
        static_cast<unsigned int>(outgoingId), outSlot,
        static_cast<unsigned int>(incomingId), inSlot);
}

bool __fastcall HookSubstitutionBuilder08(
    void* thisPtr, void*, DWORD* incomingPlayerId, DWORD* outgoingPlayerId)
{
    DWORD incomingId = 0;
    DWORD outgoingId = 0;
    __try {
        if (incomingPlayerId) incomingId = *incomingPlayerId;
        if (outgoingPlayerId) outgoingId = *outgoingPlayerId;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        incomingId = outgoingId = 0;
    }

    const bool result = g_originalSubstitutionBuilder08
        ? g_originalSubstitutionBuilder08(thisPtr, incomingPlayerId, outgoingPlayerId)
        : true;

    if (incomingId && outgoingId)
        ApplySubstitutionToLiveLineup08(incomingId, outgoingId);
    return result;
}

void __fastcall HookBoxScoreInit08(void* thisPtr, void*)
{
    if (thisPtr) {
        g_boxScoreMgr08 = reinterpret_cast<unsigned char*>(thisPtr) - 0x18;
        g_loggedBoxScoreReady08 = false;
    }
    if (g_originalBoxScoreInit08) g_originalBoxScoreInit08(thisPtr);
}

bool FillLiveRuntimePlayerData08(LivePlayerData08& o, DWORD nativePlayerId, void* player)
{
    if (!player || !g_getRuntimePlayerInt08 || nativePlayerId == 0 || nativePlayerId == 0xFFFFFFFFu)
        return false;

    __try {
        std::memset(&o, 0, sizeof(o));
        o.courtSlot = -1;
        o.nativePlayerId = static_cast<int>(nativePlayerId);
        o.databasePlayerId = g_getRuntimePlayerInt08(player, 0x88);
        o.jerseyNumber = g_getRuntimePlayerInt08(player, 0x85);
        o.currentPosition = g_getRuntimePlayerInt08(player, 0x87);
        o.initialPosition = g_getRuntimePlayerInt08(player, 0x89);

        // Runtime-player stat IDs, proven from the featured-player path.
        o.threeAttempts      = g_getRuntimePlayerInt08(player, 0x00);
        o.threeMade          = g_getRuntimePlayerInt08(player, 0x01);
        o.fgAttempts         = g_getRuntimePlayerInt08(player, 0x02);
        o.fgMade             = g_getRuntimePlayerInt08(player, 0x03);
        o.ftAttempts         = g_getRuntimePlayerInt08(player, 0x04);
        o.ftMade             = g_getRuntimePlayerInt08(player, 0x05);
        o.assists            = g_getRuntimePlayerInt08(player, 0x06);
        o.blocks             = g_getRuntimePlayerInt08(player, 0x07);
        o.defensiveRebounds  = g_getRuntimePlayerInt08(player, 0x08);
        o.fouls              = g_getRuntimePlayerInt08(player, 0x09);
        o.offensiveRebounds  = g_getRuntimePlayerInt08(player, 0x0A);
        o.minutes            = g_getRuntimePlayerInt08(player, 0x0B);
        o.steals             = g_getRuntimePlayerInt08(player, 0x0C);
        o.turnovers          = g_getRuntimePlayerInt08(player, 0x0D);
        o.points             = g_getRuntimePlayerInt08(player, 0x0F);
        o.rebounds           = g_getRuntimePlayerInt08(player, 0x10);

        o.valid = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        o.valid = false;
        return false;
    }
}

bool RefreshLiveGameData08(const ExtendedState& state)
{
    if (!g_game || g_game->version != GameVersion::Live2008 ||
        !g_resolveRuntimePlayer08 || !g_getRuntimePlayerInt08)
        return false;

    __try {
        void* manager = *reinterpret_cast<void**>(0x00DA63C8);
        if (!manager) {
            ResetLiveGameData08();
            return false;
        }

        DWORD* nativeIds = reinterpret_cast<DWORD*>(
            static_cast<unsigned char*>(manager) + 0x08);

        LiveGameData08 n = {};
        n.homeTeamIndex = 0;
        n.awayTeamIndex = 1;
        n.teams[0].teamId = state.homeTeamDBID >= 0 ? state.homeTeamDBID : state.homeTeamID;
        n.teams[1].teamId = state.awayTeamDBID >= 0 ? state.awayTeamDBID : state.awayTeamID;

        int resolved[2] = {0, 0};
        InitializeLiveLineup08();

        // Live 08 manager layout proven in-game:
        // slots 0..11 = home roster, slots 12..23 = away roster.
        // field 0x89 is the stable original roster slot (0..11) for each side.
        for (int managerSlot = 0; managerSlot < 24; ++managerSlot) {
            const DWORD nativeId = nativeIds[managerSlot];
            if (nativeId == 0 || nativeId == 0xFFFFFFFFu)
                continue;

            DWORD resolverType = 0;
            void* player = g_resolveRuntimePlayer08(&nativeId, &resolverType);
            if (!player)
                continue;

            const int side = managerSlot < 12 ? 0 : 1; // 0=home, 1=away
            LivePlayerData08 temp = {};
            if (!FillLiveRuntimePlayerData08(temp, nativeId, player))
                continue;

            int rosterSlot = temp.initialPosition;
            if (rosterSlot < 0 || rosterSlot >= 12)
                rosterSlot = managerSlot % 12;

            temp.onCourt = g_onCourt08[side][rosterSlot];
            temp.courtSlot = g_courtSlot08[side][rosterSlot];
            n.teams[side].players[rosterSlot] = temp;
            ++resolved[side];
        }

        n.teams[0].playerCount = resolved[0];
        n.teams[1].playerCount = resolved[1];
        n.refreshedAt = GetTickCount();
        n.valid = resolved[0] > 0 && resolved[1] > 0;

        g_liveGame08 = n;

        if (n.valid && !g_loggedBoxScoreReady08) {
            AppendDiagnostic(
                "NBA Live 08 live-player cache ready: home=%d(%d players) away=%d(%d players), manager=%p. F6 exports boxscore.csv; onCourt/courtSlot track substitutions.\n",
                n.teams[0].teamId, n.teams[0].playerCount,
                n.teams[1].teamId, n.teams[1].playerCount,
                manager);
            g_loggedBoxScoreReady08 = true;
        }

        return n.valid;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ResetLiveGameData08();
        return false;
    }
}

struct LiveManagerEnumRow08 {
    int managerSlot;
    DWORD nativePlayerId;
    DWORD resolverType;
    void* runtimeObject;
    int fields80to8F[16];
    int stats00to1F[32];
};

bool ExportLivePlayerManagerCsv08()
{
    if (!g_game || g_game->version != GameVersion::Live2008 ||
        !g_resolveRuntimePlayer08 || !g_getRuntimePlayerInt08)
        return false;

    FILE* f = std::fopen("live_players.csv", "w");
    if (!f) return false;

    std::fputs(
        "manager_slot,native_player_id,resolver_type,runtime_object,"
        "field80,field81,field82,field83,field84,field85,field86,field87,"
        "field88,field89,field8A,field8B,field8C,field8D,field8E,field8F,"
        "stat00,stat01,stat02,stat03,stat04,stat05,stat06,stat07,"
        "stat08,stat09,stat0A,stat0B,stat0C,stat0D,stat0E,stat0F,"
        "stat10,stat11,stat12,stat13,stat14,stat15,stat16,stat17,"
        "stat18,stat19,stat1A,stat1B,stat1C,stat1D,stat1E,stat1F\n", f);

    int occupied = 0;
    int resolved = 0;
    void* manager = nullptr;

    __try {
        manager = *reinterpret_cast<void**>(0x00DA63C8);
        if (!manager) {
            std::fclose(f);
            AppendDiagnostic("NBA Live 08 live-player export: dword_DA63C8 is NULL.\n");
            return false;
        }

        DWORD* nativeIds = reinterpret_cast<DWORD*>(
            static_cast<unsigned char*>(manager) + 0x08);

        for (int slot = 0; slot < 0x60; ++slot) {
            const DWORD nativeId = nativeIds[slot];

            // Manager construction zeroes the 96-ID table. Player IDs observed
            // in presentation code are positive, so zero is an unused slot.
            if (nativeId == 0 || nativeId == 0xFFFFFFFFu)
                continue;

            ++occupied;
            DWORD resolverType = 0;
            void* player = g_resolveRuntimePlayer08(&nativeId, &resolverType);
            if (!player)
                continue;
            ++resolved;

            std::fprintf(f, "%d,%u,%u,%p",
                slot, static_cast<unsigned int>(nativeId),
                static_cast<unsigned int>(resolverType), player);

            for (int field = 0x80; field <= 0x8F; ++field) {
                int value = INT_MIN;
                __try { value = g_getRuntimePlayerInt08(player, field); }
                __except (EXCEPTION_EXECUTE_HANDLER) { value = INT_MIN; }
                std::fprintf(f, ",%d", value);
            }

            for (int stat = 0; stat < 0x20; ++stat) {
                int value = INT_MIN;
                __try { value = g_getRuntimePlayerInt08(player, stat); }
                __except (EXCEPTION_EXECUTE_HANDLER) { value = INT_MIN; }
                std::fprintf(f, ",%d", value);
            }
            std::fputc('\n', f);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fclose(f);
        AppendDiagnostic(
            "NBA Live 08 live-player export aborted by access exception: mgr=%p occupied=%d resolved=%d.\n",
            manager, occupied, resolved);
        return false;
    }

    std::fclose(f);
    AppendDiagnostic(
        "NBA Live 08 live-player CSV exported: live_players.csv mgr=%p occupied=%d resolved=%d. "
        "Fields 85/87/88/89 are jersey/currentPosition/databasePlayerID/initialPosition; "
        "80-8F retained to identify team/side.\n",
        manager, occupied, resolved);
    return resolved > 0;
}

void WriteCsvQuoted08(FILE* f, const char* x)
{
    if (!x) x = "";
    std::fputc('"', f);
    for (; *x; ++x) {
        if (*x == '"') std::fputc('"', f);
        std::fputc(*x, f);
    }
    std::fputc('"', f);
}

bool ExportBoxScoreCsv05()
{
    if (!g_liveGame05.valid) return false;
    FILE* f = std::fopen("boxscore.csv", "w");
    if (!f) return false;
    std::fputs("side,team_index,team_id,slot,runtime_player_id,database_player_id,first_name,last_name,jersey,jersey_raw,position,roster_slot,on_court,court_slot,fgm,fga,3pm,3pa,ftm,fta,oreb,dreb,reb,blk,stl,ast,to,pf,minutes,points\n", f);
    for (int sp = 0; sp < 2; ++sp) {
        const bool home = sp == 1;
        const int ti = home ? g_liveGame05.homeTeamIndex : g_liveGame05.awayTeamIndex;
        if (ti < 0 || ti > 1) continue;
        const auto& t = g_liveGame05.teams[ti];
        for (int i = 0; i < 12; ++i) {
            const auto& p = t.players[i];
            if (!p.valid) continue;
            char j[16] = {};
            if (p.jerseyNumber == -1) std::strcpy(j, "00");
            else if (p.jerseyNumber != INT_MIN) std::snprintf(j, sizeof(j), "%d", p.jerseyNumber);
            std::fprintf(f, "%s,%d,%d,%d,%d,%d,", home ? "home" : "away", ti, t.teamId, i, p.nativePlayerId, p.databasePlayerId);
            WriteCsvQuoted08(f, ""); std::fputc(',', f);
            WriteCsvQuoted08(f, ""); std::fputc(',', f);
            WriteCsvQuoted08(f, j);
            std::fprintf(f, ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                p.jerseyNumber, p.currentPosition, p.initialPosition,
                p.onCourt ? 1 : 0, p.courtSlot,
                p.fgMade, p.fgAttempts, p.threeMade, p.threeAttempts,
                p.ftMade, p.ftAttempts, p.offensiveRebounds, p.defensiveRebounds,
                p.rebounds, p.blocks, p.steals, p.assists, p.turnovers,
                p.fouls, p.minutes, p.points);
        }
    }
    std::fclose(f);
    AppendDiagnostic("NBA Live 05 box score CSV exported: boxscore.csv.\n");
    return true;
}

bool ExportBoxScoreCsv06()
{
    if (!g_liveGame06.valid) return false;
    FILE* f = std::fopen("boxscore.csv", "w");
    if (!f) return false;
    std::fputs("side,team_index,team_id,slot,runtime_player_id,database_player_id,first_name,last_name,jersey,jersey_raw,position,roster_slot,on_court,court_slot,fgm,fga,3pm,3pa,ftm,fta,oreb,dreb,reb,blk,stl,ast,to,pf,minutes,points\n", f);
    for (int sp = 0; sp < 2; ++sp) {
        const bool home = sp == 1;
        const int ti = home ? g_liveGame06.homeTeamIndex : g_liveGame06.awayTeamIndex;
        if (ti < 0 || ti > 1) continue;
        const auto& t = g_liveGame06.teams[ti];
        for (int i = 0; i < 12; ++i) {
            const auto& p = t.players[i];
            if (!p.valid) continue;
            char j[16] = {};
            if (p.jerseyNumber == -1) std::strcpy(j, "00");
            else if (p.jerseyNumber != INT_MIN) std::snprintf(j, sizeof(j), "%d", p.jerseyNumber);
            std::fprintf(f, "%s,%d,%d,%d,%d,%d,", home ? "home" : "away", ti, t.teamId, i, p.nativePlayerId, p.databasePlayerId);
            WriteCsvQuoted08(f, ""); std::fputc(',', f);
            WriteCsvQuoted08(f, ""); std::fputc(',', f);
            WriteCsvQuoted08(f, j);
            std::fprintf(f, ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                p.jerseyNumber, p.currentPosition, p.initialPosition,
                p.onCourt ? 1 : 0, p.courtSlot,
                p.fgMade, p.fgAttempts, p.threeMade, p.threeAttempts,
                p.ftMade, p.ftAttempts, p.offensiveRebounds, p.defensiveRebounds,
                p.rebounds, p.blocks, p.steals, p.assists, p.turnovers,
                p.fouls, p.minutes, p.points);
        }
    }
    std::fclose(f);
    AppendDiagnostic("NBA Live 06 box score CSV exported: boxscore.csv.\n");
    return true;
}

bool ExportBoxScoreCsv07()
{
    if (!g_liveGame07.valid) return false;
    FILE* f = std::fopen("boxscore.csv", "w");
    if (!f) return false;
    std::fputs("side,team_index,team_id,slot,runtime_player_id,database_player_id,first_name,last_name,jersey,jersey_raw,position,roster_slot,on_court,court_slot,fgm,fga,3pm,3pa,ftm,fta,oreb,dreb,reb,blk,stl,ast,to,pf,minutes,points\n", f);
    for (int sp = 0; sp < 2; ++sp) {
        const bool home = sp == 1;
        const int ti = home ? g_liveGame07.homeTeamIndex : g_liveGame07.awayTeamIndex;
        if (ti < 0 || ti > 1) continue;
        const auto& t = g_liveGame07.teams[ti];
        for (int i = 0; i < 12; ++i) {
            const auto& p = t.players[i];
            if (!p.valid) continue;
            char j[16] = {};
            if (p.jerseyNumber == -1) std::strcpy(j, "00");
            else if (p.jerseyNumber != INT_MIN) std::snprintf(j, sizeof(j), "%d", p.jerseyNumber);
            std::fprintf(f, "%s,%d,%d,%d,%d,%d,", home ? "home" : "away", ti, t.teamId, i, p.nativePlayerId, p.databasePlayerId);
            WriteCsvQuoted08(f, ""); std::fputc(',', f);
            WriteCsvQuoted08(f, ""); std::fputc(',', f);
            WriteCsvQuoted08(f, j);
            std::fprintf(f, ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                p.jerseyNumber, p.currentPosition, p.initialPosition,
                p.onCourt ? 1 : 0, p.courtSlot,
                p.fgMade, p.fgAttempts, p.threeMade, p.threeAttempts,
                p.ftMade, p.ftAttempts, p.offensiveRebounds, p.defensiveRebounds,
                p.rebounds, p.blocks, p.steals, p.assists, p.turnovers,
                p.fouls, p.minutes, p.points);
        }
    }
    std::fclose(f);
    AppendDiagnostic("NBA Live 07 box score CSV exported: boxscore.csv.\n");
    return true;
}

bool ExportBoxScoreCsv08()
{
    if(!g_liveGame08.valid) return false; FILE* f=std::fopen("boxscore.csv","w"); if(!f)return false;
    std::fputs("side,team_index,team_id,slot,runtime_player_id,database_player_id,first_name,last_name,jersey,jersey_raw,position,roster_slot,on_court,court_slot,fgm,fga,3pm,3pa,ftm,fta,oreb,dreb,reb,blk,stl,ast,to,pf,minutes,points\n",f);
    for(int sp=0;sp<2;++sp){ bool home=sp==1; int ti=home?g_liveGame08.homeTeamIndex:g_liveGame08.awayTeamIndex; if(ti<0||ti>1)continue; const auto& t=g_liveGame08.teams[ti];
        for(int i=0;i<12;++i){ const auto& p=t.players[i]; if(!p.valid)continue; char j[16]={}; if(p.jerseyNumber==-1)std::strcpy(j,"00"); else if(p.jerseyNumber!=INT_MIN)std::snprintf(j,sizeof(j),"%d",p.jerseyNumber);
            std::fprintf(f,"%s,%d,%d,%d,%d,%d,",home?"home":"away",ti,t.teamId,i,p.nativePlayerId,p.databasePlayerId); WriteCsvQuoted08(f,""); fputc(',',f); WriteCsvQuoted08(f,""); fputc(',',f); WriteCsvQuoted08(f,j);
            std::fprintf(f,",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",p.jerseyNumber,p.currentPosition,p.initialPosition,p.onCourt ? 1 : 0,p.courtSlot,p.fgMade,p.fgAttempts,p.threeMade,p.threeAttempts,p.ftMade,p.ftAttempts,p.offensiveRebounds,p.defensiveRebounds,p.rebounds,p.blocks,p.steals,p.assists,p.turnovers,p.fouls,p.minutes,p.points); }
    }
    std::fclose(f); AppendDiagnostic("Box score CSV exported: boxscore.csv. Name columns are reserved but blank until direct all-player name getters are mapped.\n"); return true;
}

bool TryReadState(ExtendedState* state)
{
    __try {
        GdInfoCentral* info = GetGdInfoCentral();
        if (!info) return false;

        GdAI* gdAI = GetGdAI();
        const auto getGameClock = reinterpret_cast<GetGameClockFn>(
            g_game->getGameClock);
        const auto isShotClockValid = reinterpret_cast<IsShotClockValidFn>(
            g_game->isShotClockValid);
        const auto getShotClock = reinterpret_cast<GetShotClockFn>(
            g_game->getShotClock);

        state->quarter = GetQuarter(gdAI);
        state->gameValid = IsGameClockValid(gdAI) ? 1 : 0;
        state->gameRaw = getGameClock(info);
        if (g_game->getClockUnitsPerSecond) {
            const auto getClockUnitsPerSecond =
                reinterpret_cast<GetGameClockFn>(
                    g_game->getClockUnitsPerSecond);
            state->clockUnitsPerSecond = getClockUnitsPerSecond(info);
        }
        else {
            state->clockUnitsPerSecond = 60;
        }
        if (!state->clockUnitsPerSecond)
            state->clockUnitsPerSecond = 60;
        state->shotValid = isShotClockValid(info) ? 1 : 0;
        state->shotRaw = getShotClock(info);
        if (g_game->getTeamScore && g_game->getTeamIDFromSide) {
            state->homeScore = GetTeamScore(info, 0);
            state->awayScore = GetTeamScore(info, 1);

            const IDTeam homeTeam = GetTeamIDFromSide(0);
            const IDTeam awayTeam = GetTeamIDFromSide(1);
            state->homeTeamID = homeTeam.value;
            state->awayTeamID = awayTeam.value;
            state->homeTeamDBID = GetDatabaseTeamID(homeTeam);
            state->awayTeamDBID = GetDatabaseTeamID(awayTeam);
            state->homeFouls = GetOptionalTeamValue(
                g_game->getTeamFouls, homeTeam);
            state->awayFouls = GetOptionalTeamValue(
                g_game->getTeamFouls, awayTeam);
            state->homeTimeouts = GetOptionalTeamValue(
                g_game->getTeamTimeoutsLeft, homeTeam);
            state->awayTimeouts = GetOptionalTeamValue(
                g_game->getTeamTimeoutsLeft, awayTeam);
        }
        else {
            state->homeScore = -1;
            state->awayScore = -1;
            state->homeTeamID = -1;
            state->awayTeamID = -1;
            state->homeTeamDBID = -1;
            state->awayTeamDBID = -1;
            state->homeFouls = -1;
            state->awayFouls = -1;
            state->homeTimeouts = -1;
            state->awayTimeouts = -1;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool StateChanged(const ExtendedState& current,
                  const ExtendedState& previous)
{
    return std::memcmp(&current, &previous, sizeof(current)) != 0;
}

void PollState()
{
    if (g_polling || g_gettersDisabled || !g_game) return;
    g_polling = true;

    ExtendedState state = {};
    if (!TryReadState(&state)) {
        AppendDiagnostic("Scoreboard getter call faulted; polling disabled.\n");
        g_gettersDisabled = true;
        g_polling = false;
        return;
    }

    if (g_game->version == GameVersion::Live2005)
        RefreshLiveGameData05(state);
    else if (g_game->version == GameVersion::Live2006)
        RefreshLiveGameData06(state);
    else if (g_game->version == GameVersion::Live2007)
        RefreshLiveGameData07(state);
    else if (g_game->version == GameVersion::Live2008)
        RefreshLiveGameData08(state);

    // Team fouls reset between periods. The game's foul getters can retain
    // the previous period's values for a very short time after the quarter
    // number changes, which would otherwise flash BONUS on the first frame.
    if (g_lastState.quarter != INT_MIN &&
        state.quarter != g_lastState.quarter) {
        g_pendingFoulResetQuarter = state.quarter;
    }
    if (g_pendingFoulResetQuarter == state.quarter) {
        const bool gameHasResetFouls =
            state.homeFouls == 0 && state.awayFouls == 0;
        state.homeFouls = 0;
        state.awayFouls = 0;
        if (gameHasResetFouls)
            g_pendingFoulResetQuarter = INT_MIN;
    }

    if (StateChanged(state, g_lastState)) {
        const unsigned int units = state.clockUnitsPerSecond;
        const unsigned int gameSeconds =
            state.gameRaw / units;
        const unsigned int shotSeconds =
            (state.shotRaw + units - 1u) / units;
        AppendDiagnostic(
            "quarter=%d gameClockValid=%d gameClockRaw=%u "
            "clockUnitsPerSecond=%u gameClockSeconds=%u "
            "shotClockValid=%d shotClockRaw=%u shotClockSeconds=%u "
            "awayTeamScore=%d awayRuntimeTeamID=%d awayDatabaseTeamID=%d "
            "awayFouls=%d awayTimeouts=%d "
            "homeTeamScore=%d homeRuntimeTeamID=%d homeDatabaseTeamID=%d "
            "homeFouls=%d homeTimeouts=%d\n",
            state.quarter, state.gameValid, state.gameRaw,
            units, gameSeconds,
            state.shotValid, state.shotRaw, shotSeconds,
            state.awayScore, state.awayTeamID, state.awayTeamDBID,
            state.awayFouls, state.awayTimeouts,
            state.homeScore, state.homeTeamID, state.homeTeamDBID,
            state.homeFouls, state.homeTimeouts);
        g_lastState = state;
    }

    g_polling = false;
}

int __cdecl HookSendEvent(
    void* event, const char* path, const char* name,
    const char* p0, const char* p1, const char* p2,
    const char* p3, const char* p4)
{
    const uintptr_t caller =
        reinterpret_cast<uintptr_t>(_ReturnAddress()) - 5;
    const bool mainScreen = IsMainScreen(path);
    if (mainScreen || IsLifecycleEvent(name)) {
        const char* parameters[5] = { p0, p1, p2, p3, p4 };
        LogEvent(caller, path, name, parameters);
    }

    // Pause/menu overlay events are not guaranteed to target
    // _level0.gMainScreen. Treat them as global presentation state.
    if (name) {
        // "UnPauseEvent" contains the word "Pause", so resume events must
        // be recognized before the generic pause substring check. Otherwise
        // 07/08 stay suppressed until the next ScoreClockUpdateEvent.
        const bool resumeEvent =
            std::strstr(name, "Resume") != nullptr ||
            std::strstr(name, "UnPause") != nullptr;
        const bool pauseEvent =
            std::strstr(name, "Pause") != nullptr && !resumeEvent;

        // Mirror FEOverlayViolation's presentation lifecycle without loading
        // overlays~viol.big. Full-screen transitions temporarily hide/freeze
        // the active custom instance; pauses permanently dismiss it.
        if (pauseEvent) {
            g_fullScreenTransitionActive = false;
            g_violationPresentationSuppressed = true;
            g_violation.active = false;
            g_violation.paused = false;
            g_violation.pausedAt = 0;
            g_violationTransitionHiddenAt = 0;
            g_playCallPresentationSuppressed = true;
            g_playCall.active = false;
            g_playCallTransitionHiddenAt = 0;
            g_playerFoulPresentationSuppressed = true;
            g_playerFoul.active = false;
            g_playerFoulTransitionHiddenAt = 0;
            g_genericStatPresentationSuppressed = true;
            g_genericStat.active = false;
            g_genericStatTransitionHiddenAt = 0;
            g_introPresentationSuppressed = true;
            g_intro.active = false;
            g_introTransitionHiddenAt = 0;
            g_starting5.active = false;
            g_starting5Pending.active = false;
            g_outro.active = false;
            g_lineups.active = false;
            g_lineupsTransitionHiddenAt = 0;
        }
        else if (std::strcmp(name, "HideOverlaysEvent") == 0) {
            g_fullScreenTransitionActive = true;
            if (!g_violationPresentationSuppressed && g_violation.active)
                g_violationTransitionHiddenAt = GetTickCount();
            g_violationPresentationSuppressed = true;
            if (!g_playCallPresentationSuppressed && g_playCall.active)
                g_playCallTransitionHiddenAt = GetTickCount();
            g_playCallPresentationSuppressed = true;
            if (!g_playerFoulPresentationSuppressed && g_playerFoul.active)
                g_playerFoulTransitionHiddenAt = GetTickCount();
            g_playerFoulPresentationSuppressed = true;
            if (!g_genericStatPresentationSuppressed && g_genericStat.active)
                g_genericStatTransitionHiddenAt = GetTickCount();
            g_genericStatPresentationSuppressed = true;
            // A full-screen transition permanently ends the current match
            // intro. Keep payloadHash unchanged so GetOverlayData cannot
            // recreate the same intro at tipoff. The normal end-of-game reset
            // clears that hash so the following game's intro is accepted.
            g_intro.active = false;
            g_introPresentationSuppressed = true;
            g_introTransitionHiddenAt = 0;

            // Starting5 and Outro are native 07/08 presentation families.
            // Once a full-screen transition begins, end the current custom
            // presentation permanently so ShowOverlaysEvent cannot restore it
            // over the following scene. Clear the queued second Starting5
            // entry as well because it belongs to the presentation sequence
            // that the transition just ended.
            if (g_game &&
                (g_game->version == GameVersion::Live2007 ||
                 g_game->version == GameVersion::Live2008)) {
                g_starting5.active = false;
                g_starting5Pending.active = false;
                g_outro.active = false;
                // FEOverlayLineups is different: Live 08 commonly builds it
                // during this transition. Keep the captured payload, hide it
                // behind the transition, and freeze its presentation timer.
                if (g_game->version == GameVersion::Live2008 &&
                    g_lineups.active && !g_lineupsTransitionHiddenAt)
                    g_lineupsTransitionHiddenAt = GetTickCount();
            }
        }
        else if (std::strcmp(name, "ShowOverlaysEvent") == 0) {
            g_fullScreenTransitionActive = false;
            if (g_violationPresentationSuppressed && g_violation.active &&
                g_violationTransitionHiddenAt) {
                g_violation.startedAt +=
                    GetTickCount() - g_violationTransitionHiddenAt;
            }
            g_violationPresentationSuppressed = false;
            g_violationTransitionHiddenAt = 0;
            if (g_playCallPresentationSuppressed && g_playCall.active &&
                g_playCallTransitionHiddenAt)
                g_playCall.startedAt +=
                    GetTickCount() - g_playCallTransitionHiddenAt;
            g_playCallPresentationSuppressed = false;
            g_playCallTransitionHiddenAt = 0;
            if (g_playerFoulPresentationSuppressed && g_playerFoul.active &&
                g_playerFoulTransitionHiddenAt)
                g_playerFoul.startedAt +=
                    GetTickCount() - g_playerFoulTransitionHiddenAt;
            g_playerFoulPresentationSuppressed = false;
            g_playerFoulTransitionHiddenAt = 0;
            if (g_genericStatPresentationSuppressed && g_genericStat.active &&
                g_genericStatTransitionHiddenAt)
                g_genericStat.startedAt +=
                    GetTickCount() - g_genericStatTransitionHiddenAt;
            g_genericStatPresentationSuppressed = false;
            g_genericStatTransitionHiddenAt = 0;
            // The previous intro was ended by HideOverlaysEvent. Re-arm
            // presentation for a future new payload without reviving it.
            g_introPresentationSuppressed = false;
            g_introTransitionHiddenAt = 0;
            if (g_game && g_game->version == GameVersion::Live2008 &&
                g_lineups.active && g_lineupsTransitionHiddenAt) {
                g_lineups.startedAt +=
                    GetTickCount() - g_lineupsTransitionHiddenAt;
                g_lineupsTransitionHiddenAt = 0;
            }
        }
        else if (resumeEvent) {
            g_fullScreenTransitionActive = false;
            // A pause killed the previous instance; resume merely allows the
            // next native violation request to create a new custom one.
            g_violationPresentationSuppressed = false;
            g_violationTransitionHiddenAt = 0;
            g_playCallPresentationSuppressed = false;
            g_playCallTransitionHiddenAt = 0;
            g_playerFoulPresentationSuppressed = false;
            g_playerFoulTransitionHiddenAt = 0;
            g_genericStatPresentationSuppressed = false;
            g_genericStatTransitionHiddenAt = 0;
            g_introPresentationSuppressed = false;
            g_introTransitionHiddenAt = 0;
        }

        const bool freezeViolation =
            scoreboardconfig::GetViolation().freezeWhilePaused;
        if (pauseEvent && freezeViolation && g_violation.active &&
            !g_violation.paused) {
            g_violation.paused = true;
            g_violation.pausedAt = GetTickCount();
        }
        else if (resumeEvent && g_violation.active && g_violation.paused) {
            g_violation.startedAt += GetTickCount() - g_violation.pausedAt;
            g_violation.paused = false;
            g_violation.pausedAt = 0;
        }

        if (pauseEvent) {
            g_scoreboardSuppressed = true;
            g_scoreboardVisible = false;
            g_visibilityLastTick = 0;
        }
        else if (std::strcmp(name, "ScoreHideEvent") == 0 ||
                 std::strcmp(name, "HideOverlaysEvent") == 0) {
            if (!g_gameplayStarted &&
                std::strcmp(name, "HideOverlaysEvent") == 0)
                g_seenIntroOverlayHide = true;
            g_scoreboardSuppressed = true;
            g_scoreboardVisible = false;
        }
        else if (resumeEvent ||
                 std::strcmp(name, "ScoreShowEvent") == 0 ||
                 std::strcmp(name, "ShowOverlaysEvent") == 0) {
            g_scoreboardSuppressed = false;
            g_scoreboardVisible = g_gameplayStarted;
            g_visibilityLastTick = 0;
        }
    }

    if (mainScreen) {
        if (name) {
            if (std::strcmp(name, "ScoreShowEvent") == 0 ||
                std::strcmp(name, "ShowOverlaysEvent") == 0) {
                g_scoreboardSuppressed = false;
                g_scoreboardVisible = g_gameplayStarted;
            }
            else if (std::strcmp(name, "ScoreHideEvent") == 0 ||
                     std::strcmp(name, "HideOverlaysEvent") == 0) {
                g_scoreboardSuppressed = true;
                g_scoreboardVisible = false;
            }

            if (std::strcmp(name, "ScoreShowEvent") == 0 &&
                !g_gameplayStarted) {
                const bool live07IntroFinished =
                    g_game->version == GameVersion::Live2007 &&
                    g_seenIntroOverlayHide;
                if (g_game->version != GameVersion::Live2007 ||
                    live07IntroFinished) {
                    g_gameplayStarted = true;
                    g_scoreboardSuppressed = false;
                    g_scoreboardVisible = true;
                }
            }

            const bool stateEvent =
                std::strcmp(name, "ScoreClockUpdateEvent") == 0 ||
                std::strcmp(name, "HomeScoreUpdateEvent") == 0 ||
                std::strcmp(name, "AwayScoreUpdateEvent") == 0 ||
                std::strcmp(name, "ShotClockUpdateEvent") == 0 ||
                std::strcmp(name, "ShotClockShowEvent") == 0 ||
                std::strcmp(name, "ShotClockHideEvent") == 0;

            // Fallback for builds that do not deliver the final ScoreShowEvent
            // through a redirected call site.
            if (std::strcmp(name, "ScoreClockUpdateEvent") == 0) {
                g_gameplayStarted = true;
                g_scoreboardSuppressed = false;
                g_scoreboardVisible = true;
            }

            if (stateEvent && g_gameplayStarted) {
                PollState();
            }
        }
    }
    // Keep ScoreShowEvent available to the custom scoreboard lifecycle code
    // above, but do not forward it to the original Flash/APT main-screen
    // receiver in games whose native show-state wrappers are neutralized.
    if (g_customOverlayEnabled && g_game &&
        (g_game->version == GameVersion::Live2005 ||
        g_game->version == GameVersion::Live2006 ||
        g_game->version == GameVersion::Live2007 ||
        g_game->version == GameVersion::Live2008) &&
        mainScreen && name && std::strcmp(name, "ScoreShowEvent") == 0) {
        return 0;
    }

    // Live 08's pause flow restores the existing Flash main screen through
    // ShowOverlayManager itself, before its later ScoreShowEvent reaches this
    // hook. Preserve the pause/overlay manager, then hide only the native
    // scoreboard. Call the original directly for the synthetic hide so our
    // custom scoreboard lifecycle is not suppressed.
    if (g_customOverlayEnabled && g_game &&
        (g_game->version == GameVersion::Live2007 ||
            g_game->version == GameVersion::Live2008) &&
        name && std::strcmp(name, "ShowOverlayManager") == 0) {
        const int result = g_originalSendEvent(
            event, path, name, p0, p1, p2, p3, p4);
        g_originalSendEvent(event, "_level0.gMainScreen", "ScoreHideEvent",
            nullptr, nullptr, nullptr, nullptr, nullptr);
        return result;
    }

    return g_originalSendEvent(event, path, name, p0, p1, p2, p3, p4);
}

DWORD __fastcall HookGetOverlayData(
    DWORD* thisPtr, void*, int outputVector, int type)
{
    const auto original = reinterpret_cast<GetOverlayDataFn>(
        g_game->getOverlayData);
    const DWORD result = original(thisPtr, outputVector, type);
    if (type == 1)
        CaptureViolationPayload(reinterpret_cast<DWORD*>(outputVector));
    else if (type == 3)
        CaptureIntroPayload(reinterpret_cast<DWORD*>(outputVector));
    LogOverlayPayload(type, reinterpret_cast<DWORD*>(outputVector));
    // Fouls, timeouts, substitutions and other presentation state can change
    // between whole-second score-clock events.
    if (g_scoreboardVisible)
        PollState();
    return result;
}

bool RefreshRealtimeScoreboardState()
{
    if (!g_game)
        return false;

    __try {
        GdInfoCentral* info = GetGdInfoCentral();
        GdAI* gdAI = GetGdAI();
        if (!info || !gdAI || !IsGameClockValid(gdAI)) {
            const bool previousGameHadStarted = g_gameplayStarted;
            g_gameplayStarted = false;
            g_seenIntroOverlayHide = false;
            g_scoreboardVisible = false;
            g_lastState.homeTeamID = -1;
            g_lastState.awayTeamID = -1;
            g_lastState.homeTeamDBID = -1;
            g_lastState.awayTeamDBID = -1;
            g_lastState.quarter = INT_MIN;
            g_lastState.homeFouls = 0;
            g_lastState.awayFouls = 0;
            g_pendingFoulResetQuarter = INT_MIN;
            g_visibilityPreviousAwayScore = INT_MIN;
            g_visibilityPreviousHomeScore = INT_MIN;
            g_visibilityLastTick = 0;
            g_scoreboardShowRemaining = 0;
            // Clear payload identity only when a running game has genuinely
            // ended. During the pregame intro the clock is also invalid, so
            // clearing it unconditionally would allow GetOverlayData to replay
            // the same intro several times.
            if (previousGameHadStarted) {
                g_intro.payloadHash = 0;
                g_intro.active = false;
                g_introPresentationSuppressed = false;
                g_introTransitionHiddenAt = 0;
            }
            return false;
        }

        if (g_scoreboardSuppressed || !g_gameplayStarted)
            return false;

        g_scoreboardVisible = true;

        const auto getGameClock = reinterpret_cast<GetGameClockFn>(
            g_game->getGameClock);
        const auto isShotClockValid = reinterpret_cast<IsShotClockValidFn>(
            g_game->isShotClockValid);
        const auto getShotClock = reinterpret_cast<GetShotClockFn>(
            g_game->getShotClock);

        g_lastState.gameValid = 1;
        if (!g_lastState.clockUnitsPerSecond ||
            g_lastState.clockUnitsPerSecond == UINT_MAX) {
            if (g_game->getClockUnitsPerSecond) {
                const auto getClockUnitsPerSecond =
                    reinterpret_cast<GetGameClockFn>(
                        g_game->getClockUnitsPerSecond);
                g_lastState.clockUnitsPerSecond =
                    getClockUnitsPerSecond(info);
            }
            else {
                g_lastState.clockUnitsPerSecond = 60;
            }
            if (!g_lastState.clockUnitsPerSecond)
                g_lastState.clockUnitsPerSecond = 60;
        }
        g_lastState.gameRaw = getGameClock(info);
        const int previousQuarter = g_lastState.quarter;
        const int currentQuarter = GetQuarter(gdAI);
        g_lastState.quarter = currentQuarter;
        if (previousQuarter != INT_MIN &&
            currentQuarter != previousQuarter) {
            // Clear the presentation cache immediately. PollState will
            // replace these with the new quarter's authoritative values.
            g_lastState.homeFouls = 0;
            g_lastState.awayFouls = 0;
            g_pendingFoulResetQuarter = currentQuarter;
        }
        g_lastState.shotValid = isShotClockValid(info) ? 1 : 0;
        if (g_lastState.shotValid)
            g_lastState.shotRaw = getShotClock(info);

        if (g_game->getTeamScore) {
            g_lastState.homeScore = GetTeamScore(info, 0);
            g_lastState.awayScore = GetTeamScore(info, 1);
        }
        if (g_game->getTeamIDFromSide &&
            (g_lastState.homeTeamDBID < 0 ||
             g_lastState.awayTeamDBID < 0)) {
            const IDTeam homeTeam = GetTeamIDFromSide(0);
            const IDTeam awayTeam = GetTeamIDFromSide(1);
            g_lastState.homeTeamID = homeTeam.value;
            g_lastState.awayTeamID = awayTeam.value;
            g_lastState.homeTeamDBID = GetDatabaseTeamID(homeTeam);
            g_lastState.awayTeamDBID = GetDatabaseTeamID(awayTeam);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_gameplayStarted = false;
        g_scoreboardVisible = false;
        return false;
    }
}

bool ThemeVisibilityAllowsScoreboard(const ExtendedState& state)
{
    const scoreboardconfig::Config& config = scoreboardconfig::Get();
    const DWORD now = GetTickCount();
    if (g_visibilityLastTick) {
        const DWORD elapsed = now - g_visibilityLastTick;
        g_scoreboardShowRemaining = elapsed >= g_scoreboardShowRemaining ?
            0 : g_scoreboardShowRemaining - elapsed;
    }
    g_visibilityLastTick = now;

    if (g_visibilityPreviousAwayScore != INT_MIN &&
        (state.awayScore > g_visibilityPreviousAwayScore ||
         state.homeScore > g_visibilityPreviousHomeScore)) {
        g_scoreboardShowRemaining = config.showAfterScoreMilliseconds;
    }
    g_visibilityPreviousAwayScore = state.awayScore;
    g_visibilityPreviousHomeScore = state.homeScore;

    const unsigned int gameSeconds = state.clockUnitsPerSecond ?
        state.gameRaw / state.clockUnitsPerSecond : UINT_MAX;
    const bool lateGame =
        gameSeconds <= config.alwaysShowBelowSeconds;
    switch (config.visibilityMode) {
    case scoreboardconfig::VisibilityMode::AfterScore:
        return g_scoreboardShowRemaining > 0;
    case scoreboardconfig::VisibilityMode::LateGameOnly:
        return lateGame;
    case scoreboardconfig::VisibilityMode::AfterScoreAndLateGame:
        return lateGame || g_scoreboardShowRemaining > 0;
    default:
        return true;
    }
}

const char* SelectTeamName(const popup::TeamVisual* team,
                           scoreboardconfig::TeamNameFormat format,
                           char* fullName, size_t capacity,
                           const char* fallback)
{
    if (!team) return fallback;
    switch (format) {
    case scoreboardconfig::TeamNameFormat::City:
        return team->cityName;
    case scoreboardconfig::TeamNameFormat::Nickname:
        return team->teamName;
    case scoreboardconfig::TeamNameFormat::FullName:
        std::snprintf(fullName, capacity, "%s %s",
            team->cityName, team->teamName);
        return fullName;
    case scoreboardconfig::TeamNameFormat::ShortCode:
        return team->shortCode;
    default:
        return team->abbreviation;
    }
}

void RenderNativeScoreboard(IDirect3DDevice9* device)
{
    if (!g_customOverlayEnabled || !device || !RefreshRealtimeScoreboardState() ||
        g_lastState.gameValid != 1 ||
        g_lastState.homeScore < 0 || g_lastState.awayScore < 0 ||
        !g_lastState.clockUnitsPerSecond)
        return;

    popup::Load(g_customOverlayName);
    scoreboardconfig::Load(g_customOverlayName);
    if (!ThemeVisibilityAllowsScoreboard(g_lastState))
        return;
    const scoreboardconfig::Config& scoreboardSettings =
        scoreboardconfig::Get();
    const popup::TeamVisual* awayTeam =
        popup::FindTeam(g_lastState.awayTeamDBID);
    const popup::TeamVisual* homeTeam =
        popup::FindTeam(g_lastState.homeTeamDBID);

    scoreboard::Frame frame = {};
    frame.gameClockRaw = g_lastState.gameRaw;
    frame.clockUnitsPerSecond = g_lastState.clockUnitsPerSecond;
    frame.shotClockValid = g_lastState.shotValid != 0;
    frame.shotClockRaw = g_lastState.shotRaw;
    frame.awayScore = g_lastState.awayScore;
    frame.homeScore = g_lastState.homeScore;
    frame.quarter = g_lastState.quarter;
    frame.awayFouls = g_lastState.awayFouls;
    frame.homeFouls = g_lastState.homeFouls;
    frame.awayTimeouts = g_lastState.awayTimeouts;
    frame.homeTimeouts = g_lastState.homeTimeouts;
    frame.awayDatabaseTeamID = g_lastState.awayTeamDBID;
    frame.homeDatabaseTeamID = g_lastState.homeTeamDBID;
    frame.awayColor = awayTeam ? awayTeam->primaryColor :
        g_broadcast.awayColor;
    frame.homeColor = homeTeam ? homeTeam->primaryColor :
        g_broadcast.homeColor;
    frame.awaySecondaryColor = awayTeam ? awayTeam->secondaryColor :
        D3DCOLOR_XRGB(220, 220, 220);
    frame.homeSecondaryColor = homeTeam ? homeTeam->secondaryColor :
        D3DCOLOR_XRGB(220, 220, 220);
    frame.awayLogo = popup::GetLogoTexture(
        device, g_lastState.awayTeamDBID);
    if (g_loggedAwayLogoTeam != g_lastState.awayTeamDBID) {
        g_loggedAwayLogoTeam = g_lastState.awayTeamDBID;
        AppendDiagnostic(
            "Away logo databaseTeamID=%d texture=%p path=%s error=%s\n",
            g_lastState.awayTeamDBID, frame.awayLogo,
            awayTeam ? awayTeam->logoPath : "<no team entry>",
            frame.awayLogo ? "<none>" : popup::GetLastError());
    }
    frame.homeLogo = popup::GetLogoTexture(
        device, g_lastState.homeTeamDBID);
    if (g_loggedHomeLogoTeam != g_lastState.homeTeamDBID) {
        g_loggedHomeLogoTeam = g_lastState.homeTeamDBID;
        AppendDiagnostic(
            "Home logo databaseTeamID=%d texture=%p path=%s error=%s\n",
            g_lastState.homeTeamDBID, frame.homeLogo,
            homeTeam ? homeTeam->logoPath : "<no team entry>",
            frame.homeLogo ? "<none>" : popup::GetLastError());
    }
    char awayFullName[128] = {};
    char homeFullName[128] = {};
    frame.awayTeamName = SelectTeamName(awayTeam,
        scoreboardSettings.teamNameFormat,
        awayFullName, sizeof(awayFullName), g_broadcast.awayName);
    frame.homeTeamName = SelectTeamName(homeTeam,
        scoreboardSettings.teamNameFormat,
        homeFullName, sizeof(homeFullName), g_broadcast.homeName);
    scoreboard::Render(device, frame, g_customOverlayName);
}

float SmoothStep(float value)
{
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    return value * value * (3.0f - 2.0f * value);
}

bool CalculatePresentationAnimation(const scoreboardconfig::Config& config,
    DWORD startedAt, bool* active, float* x, float* y, float* opacity)
{
    const DWORD elapsed = GetTickCount() - startedAt;
    const DWORD enterEnd = config.enterMilliseconds;
    const DWORD holdEnd = enterEnd + config.holdMilliseconds;
    const DWORD total = holdEnd + config.exitMilliseconds;
    if (elapsed >= total) {
        if (active) *active = false;
        return false;
    }
    *x = 0.0f;
    *y = 0.0f;
    *opacity = 1.0f;
    if (elapsed < enterEnd && enterEnd > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed) / enterEnd);
        if (_stricmp(config.enterAnimation, "slide") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0) {
            *x = config.enterFromX * (1.0f - p);
            *y = config.enterFromY * (1.0f - p);
        }
        if (_stricmp(config.enterAnimation, "fade") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0)
            *opacity = p;
    }
    else if (elapsed >= holdEnd && config.exitMilliseconds > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed - holdEnd) /
            config.exitMilliseconds);
        if (_stricmp(config.exitAnimation, "slide") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0) {
            *x = config.exitToX * p;
            *y = config.exitToY * p;
        }
        if (_stricmp(config.exitAnimation, "fade") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0)
            *opacity = 1.0f - p;
    }
    return true;
}

void RenderViolationOverlay(IDirect3DDevice9* device)
{
    if (!g_customOverlayEnabled || !g_violation.active ||
        g_violationPresentationSuppressed || !device) return;
    scoreboardconfig::LoadViolation(g_customOverlayName);
    const scoreboardconfig::Config& config = scoreboardconfig::GetViolation();
    const DWORD now = g_violation.paused ? g_violation.pausedAt : GetTickCount();
    const DWORD elapsed = now - g_violation.startedAt;
    const DWORD enterEnd = config.enterMilliseconds;
    const DWORD holdEnd = enterEnd + config.holdMilliseconds;
    const DWORD total = holdEnd + config.exitMilliseconds;
    if (elapsed >= total) { g_violation.active = false; return; }

    float x = 0.0f, y = 0.0f, opacity = 1.0f;
    if (elapsed < enterEnd && enterEnd > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed) / enterEnd);
        if (_stricmp(config.enterAnimation, "slide") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0) {
            x = config.enterFromX * (1.0f - p);
            y = config.enterFromY * (1.0f - p);
        }
        if (_stricmp(config.enterAnimation, "fade") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0)
            opacity = p;
    }
    else if (elapsed >= holdEnd && config.exitMilliseconds > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed - holdEnd) /
            config.exitMilliseconds);
        if (_stricmp(config.exitAnimation, "slide") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0) {
            x = config.exitToX * p;
            y = config.exitToY * p;
        }
        if (_stricmp(config.exitAnimation, "fade") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0)
            opacity = 1.0f - p;
    }

    popup::Load(g_customOverlayName);
    const popup::TeamVisual* away = popup::FindTeam(g_lastState.awayTeamDBID);
    const popup::TeamVisual* home = popup::FindTeam(g_lastState.homeTeamDBID);
    const D3DCOLOR rgb = g_violation.teamColor & 0x00FFFFFFu;
    const bool awaySide = away &&
        (away->primaryColor & 0x00FFFFFFu) == rgb;
    const bool homeSide = home &&
        (home->primaryColor & 0x00FFFFFFu) == rgb;
    const popup::TeamVisual* team = awaySide ? away : homeSide ? home : nullptr;

    scoreboard::Frame frame = {};
    frame.violationTitle = g_violation.title;
    frame.violationPossession = g_violation.possession;
    frame.violationTeamColor = g_violation.teamColor;
    frame.violationTeamName = team ? team->teamName : "";
    char violationLogoPath[MAX_PATH] = {};
    if (team && team->shortCode[0])
        std::snprintf(violationLogoPath, sizeof(violationLogoPath),
            "teams\\%s.png", team->shortCode);
    frame.violationTeamLogo = violationLogoPath[0] ?
        popup::GetOverlayTexture(device, g_customOverlayName,
            "violation", violationLogoPath) : nullptr;
    scoreboard::RenderViolation(device, frame, g_customOverlayName,
        x, y, opacity);
}

void RenderPlayerFoulOverlay(IDirect3DDevice9* device)
{
    if (!g_customOverlayEnabled || !g_playerFoulLayoutAvailable ||
        !g_playerFoul.active || g_playerFoulPresentationSuppressed || !device)
        return;
    const scoreboardconfig::Config& config =
        scoreboardconfig::GetPlayerFoul();
    const DWORD elapsed = GetTickCount() - g_playerFoul.startedAt;
    const DWORD enterEnd = config.enterMilliseconds;
    const DWORD holdEnd = enterEnd + config.holdMilliseconds;
    const DWORD total = holdEnd + config.exitMilliseconds;
    if (elapsed >= total) { g_playerFoul.active = false; return; }

    float x = 0.0f, y = 0.0f, opacity = 1.0f;
    if (elapsed < enterEnd && enterEnd > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed) / enterEnd);
        if (_stricmp(config.enterAnimation, "slide") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0) {
            x = config.enterFromX * (1.0f - p);
            y = config.enterFromY * (1.0f - p);
        }
        if (_stricmp(config.enterAnimation, "fade") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0)
            opacity = p;
    }
    else if (elapsed >= holdEnd && config.exitMilliseconds > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed - holdEnd) /
            config.exitMilliseconds);
        if (_stricmp(config.exitAnimation, "slide") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0) {
            x = config.exitToX * p;
            y = config.exitToY * p;
        }
        if (_stricmp(config.exitAnimation, "fade") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0)
            opacity = 1.0f - p;
    }

    popup::Load(g_customOverlayName);
    const popup::TeamVisual* away = popup::FindTeam(g_lastState.awayTeamDBID);
    const popup::TeamVisual* home = popup::FindTeam(g_lastState.homeTeamDBID);
    const D3DCOLOR rgb = g_playerFoul.teamColor & 0x00FFFFFFu;
    // Player-stat value 13 is the game's two-character team abbreviation.
    // Match it directly against TEAMABR2 from scoreboard/teams.json.
    const popup::TeamVisual* team = popup::FindTeamByShortCode(
        g_playerFoul.logoId);
    if (!team)
        team = away && (away->primaryColor & 0x00FFFFFFu) == rgb ? away :
            home && (home->primaryColor & 0x00FFFFFFu) == rgb ? home : nullptr;

    scoreboard::Frame frame = {};
    frame.playerFirstName = g_playerFoul.firstName;
    frame.playerLastName = g_playerFoul.lastName;
    frame.playerJerseyNumber = g_playerFoul.jerseyNumber;
    frame.statValueCount = 15;
    // NBA Live 06 compatibility path: feed the custom jersey value through
    // an existing stat.raw slot so the 06 renderer does not depend on the
    // newer Frame::playerJerseyNumber binding implementation. Native raw14
    // is only the presentation package id, so replacing it in the custom
    // render frame is safe. 07/08 continue using player.jerseyNumber.
    if (g_game && g_game->version == GameVersion::Live2006)
        frame.statValues[14] = g_playerFoul.jerseyNumber;
    frame.statLabel1 = g_playerFoul.label1;
    frame.statValue1 = g_playerFoul.value1;
    frame.statLabel2 = g_playerFoul.label2;
    frame.statValue2 = g_playerFoul.value2;
    frame.statTeamName = team ? team->teamName : "";
    frame.statTeamColor = g_playerFoul.teamColor;
    frame.statPrimaryColor = team ? team->primaryColor :
        g_playerFoul.teamColor;
    frame.statSecondaryColor = team ? team->secondaryColor :
        g_playerFoul.teamColor;
    if (_stricmp(g_loggedStatTeamCode, g_playerFoul.logoId) != 0) {
        std::strncpy(g_loggedStatTeamCode, g_playerFoul.logoId,
            sizeof(g_loggedStatTeamCode) - 1);
        g_loggedStatTeamCode[sizeof(g_loggedStatTeamCode) - 1] = '\0';
        AppendDiagnostic(
            "Player-foul team mapping: payload TEAMABR2='%s' matched=%s "
            "databaseTeamID=%d primary=%u secondary=%u raw=%u.\n",
            g_playerFoul.logoId, team ? team->teamName : "<none>",
            team ? team->databaseTeamID : -1,
            static_cast<unsigned int>(frame.statPrimaryColor & 0x00FFFFFFu),
            static_cast<unsigned int>(frame.statSecondaryColor & 0x00FFFFFFu),
            static_cast<unsigned int>(frame.statTeamColor & 0x00FFFFFFu));
    }

    char logoPath[MAX_PATH] = {};
    char portraitPath[MAX_PATH] = {};
    if (g_playerFoul.logoId[0])
        std::snprintf(logoPath, sizeof(logoPath),
            "teams\\%s.png", g_playerFoul.logoId);
    if (g_playerFoul.portraitId[0] &&
        _stricmp(g_playerFoul.portraitId, "blank__") != 0)
        std::snprintf(portraitPath, sizeof(portraitPath),
            "portraits\\%s.png", g_playerFoul.portraitId);
    frame.statTeamLogo = logoPath[0] ? popup::GetOverlayTexture(device,
        g_customOverlayName, "stats", logoPath) : nullptr;
    frame.playerPortrait = portraitPath[0] ? popup::GetOverlayTexture(device,
        g_customOverlayName, "stats", portraitPath) : nullptr;
    scoreboard::RenderPlayerFoul(device, frame, g_customOverlayName,
        x, y, opacity);
}

void RenderGenericStatOverlay(IDirect3DDevice9* device)
{
    if (!g_customOverlayEnabled || !g_genericStat.active ||
        g_genericStatPresentationSuppressed || !device)
        return;
    const scoreboardconfig::Config& config = scoreboardconfig::GetStat();
    const DWORD elapsed = GetTickCount() - g_genericStat.startedAt;
    const DWORD enterEnd = config.enterMilliseconds;
    const DWORD holdEnd = enterEnd + config.holdMilliseconds;
    const DWORD total = holdEnd + config.exitMilliseconds;
    if (elapsed >= total) { g_genericStat.active = false; return; }

    float x = 0.0f, y = 0.0f, opacity = 1.0f;
    if (elapsed < enterEnd && enterEnd > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed) / enterEnd);
        if (_stricmp(config.enterAnimation, "slide") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0) {
            x = config.enterFromX * (1.0f - p);
            y = config.enterFromY * (1.0f - p);
        }
        if (_stricmp(config.enterAnimation, "fade") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0)
            opacity = p;
    }
    else if (elapsed >= holdEnd && config.exitMilliseconds > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed - holdEnd) /
            config.exitMilliseconds);
        if (_stricmp(config.exitAnimation, "slide") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0) {
            x = config.exitToX * p;
            y = config.exitToY * p;
        }
        if (_stricmp(config.exitAnimation, "fade") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0)
            opacity = 1.0f - p;
    }

    popup::Load(g_customOverlayName);
    const popup::TeamVisual* team = popup::FindTeamByShortCode(
        g_genericStat.teamCode);
    scoreboard::Frame frame = {};
    const char* genericStatFirstName = g_genericStat.count > 0 ?
        g_genericStat.values[0] : "";
    frame.playerFirstName = genericStatFirstName;
    frame.playerLastName = g_genericStat.count > 1 ?
        g_genericStat.values[1] : "";
    frame.playerJerseyNumber = g_genericStat.jerseyNumber;
    frame.statLabel1 = g_genericStat.count > 2 ?
        g_genericStat.values[2] : "";
    frame.statValue1 = g_genericStat.count > 3 ?
        g_genericStat.values[3] : "";
    frame.statLabel2 = g_genericStat.count > 4 ?
        g_genericStat.values[4] : "";
    frame.statValue2 = g_genericStat.count > 5 ?
        g_genericStat.values[5] : "";
    frame.statTeamName = team ? team->teamName : "";
    frame.statTeamColor = g_genericStat.teamColor;
    frame.statPrimaryColor = team ? team->primaryColor :
        g_genericStat.teamColor;
    frame.statSecondaryColor = team ? team->secondaryColor :
        g_genericStat.teamColor;
    frame.statValueCount = g_genericStat.count;
    for (int i = 0; i < g_genericStat.count && i < 15; ++i)
        frame.statValues[i] = g_genericStat.values[i];
    // NBA Live 06 compatibility path: expose the custom jersey number through
    // the renderer's already-existing stat.raw14 channel. This is render-only;
    // it does not modify the native FEOverlayStats payload or the 07/08 path.
    if (g_game && g_game->version == GameVersion::Live2006) {
        frame.statValues[14] = g_genericStat.jerseyNumber;
        if (frame.statValueCount < 15)
            frame.statValueCount = 15;
    }

    char logoPath[MAX_PATH] = {};
    char portraitPath[MAX_PATH] = {};
    if (g_genericStat.teamCode[0])
        std::snprintf(logoPath, sizeof(logoPath), "teams\\%s.png",
            g_genericStat.teamCode);
    if (g_genericStat.portraitId[0] &&
        _stricmp(g_genericStat.portraitId, "blank__") != 0)
        std::snprintf(portraitPath, sizeof(portraitPath),
            "portraits\\%s.png", g_genericStat.portraitId);
    frame.statTeamLogo = logoPath[0] ? popup::GetOverlayTexture(device,
        g_customOverlayName, "stats", logoPath) : nullptr;
    frame.playerPortrait = portraitPath[0] ? popup::GetOverlayTexture(device,
        g_customOverlayName, "stats", portraitPath) : nullptr;
    scoreboard::RenderStat(device, frame, g_customOverlayName,
        x, y, opacity);
}

void RenderPlayCallOverlay(IDirect3DDevice9* device)
{
    if (!g_customOverlayEnabled || !g_playCallLayoutAvailable ||
        !g_playCall.active || g_playCallPresentationSuppressed || !device)
        return;
    const scoreboardconfig::Config& config = scoreboardconfig::GetPlayCall();
    const DWORD elapsed = GetTickCount() - g_playCall.startedAt;
    const DWORD enterEnd = config.enterMilliseconds;
    const DWORD holdEnd = enterEnd + config.holdMilliseconds;
    const DWORD total = holdEnd + config.exitMilliseconds;
    if (elapsed >= total) { g_playCall.active = false; return; }

    float x = 0.0f, y = 0.0f, opacity = 1.0f;
    if (elapsed < enterEnd && enterEnd > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed) / enterEnd);
        if (_stricmp(config.enterAnimation, "slide") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0) {
            x = config.enterFromX * (1.0f - p);
            y = config.enterFromY * (1.0f - p);
        }
        if (_stricmp(config.enterAnimation, "fade") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0)
            opacity = p;
    }
    else if (elapsed >= holdEnd && config.exitMilliseconds > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed - holdEnd) /
            config.exitMilliseconds);
        if (_stricmp(config.exitAnimation, "slide") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0) {
            x = config.exitToX * p;
            y = config.exitToY * p;
        }
        if (_stricmp(config.exitAnimation, "fade") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0)
            opacity = 1.0f - p;
    }

    scoreboard::Frame frame = {};
    frame.playCallTeam = g_playCall.team;
    frame.playCallName = g_playCall.call;
    frame.playCallTeamColor = g_playCall.teamColor;
    frame.playCallValues[0] = g_playCall.team;
    frame.playCallValues[1] = g_playCall.call;
    frame.playCallValues[2] = g_playCall.rawColor;
    frame.playCallValues[3] = g_playCall.extra;
    scoreboard::RenderPlayCall(device, frame, g_customOverlayName,
        x, y, opacity);
}

void RenderIntroOverlay(IDirect3DDevice9* device)
{
    if (!g_customOverlayEnabled || !g_intro.active ||
        g_introPresentationSuppressed || !device) return;
    scoreboardconfig::LoadIntro(g_customOverlayName);
    const scoreboardconfig::Config& config = scoreboardconfig::GetIntro();
    const DWORD elapsed = GetTickCount() - g_intro.startedAt;
    const DWORD enterEnd = config.enterMilliseconds;
    const DWORD holdEnd = enterEnd + config.holdMilliseconds;
    const DWORD total = holdEnd + config.exitMilliseconds;
    if (elapsed >= total) { g_intro.active = false; return; }

    float x = 0.0f, y = 0.0f, opacity = 1.0f;
    if (elapsed < enterEnd && enterEnd > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed) / enterEnd);
        if (_stricmp(config.enterAnimation, "slide") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0) {
            x = config.enterFromX * (1.0f - p);
            y = config.enterFromY * (1.0f - p);
        }
        if (_stricmp(config.enterAnimation, "fade") == 0 ||
            _stricmp(config.enterAnimation, "slideFade") == 0)
            opacity = p;
    }
    else if (elapsed >= holdEnd && config.exitMilliseconds > 0) {
        const float p = SmoothStep(static_cast<float>(elapsed - holdEnd) /
            config.exitMilliseconds);
        if (_stricmp(config.exitAnimation, "slide") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0) {
            x = config.exitToX * p;
            y = config.exitToY * p;
        }
        if (_stricmp(config.exitAnimation, "fade") == 0 ||
            _stricmp(config.exitAnimation, "slideFade") == 0)
            opacity = 1.0f - p;
    }

    scoreboard::Frame frame = {};
    for (int i = 0; i < 15; ++i)
        frame.introValues[i] = g_intro.values[i];
    popup::Load(g_customOverlayName);
    const popup::TeamVisual* away = popup::FindTeamByShortCode(
        g_intro.values[12]);
    const popup::TeamVisual* home = popup::FindTeamByShortCode(
        g_intro.values[13]);
    frame.awayColor = away ? away->primaryColor : g_broadcast.awayColor;
    frame.homeColor = home ? home->primaryColor : g_broadcast.homeColor;
    frame.awaySecondaryColor = away ? away->secondaryColor : frame.awayColor;
    frame.homeSecondaryColor = home ? home->secondaryColor : frame.homeColor;
    frame.awayLogo = away ? popup::GetLogoTexture(device,
        away->databaseTeamID) : nullptr;
    frame.homeLogo = home ? popup::GetLogoTexture(device,
        home->databaseTeamID) : nullptr;
    scoreboard::RenderIntro(device, frame, g_customOverlayName,
        x, y, opacity);
}

void RenderStarting5Overlay(IDirect3DDevice9* device)
{
    if (!g_customOverlayEnabled || !g_starting5LayoutAvailable ||
        g_fullScreenTransitionActive || !device)
        return;
    if (!g_starting5.active && g_starting5Pending.active) {
        g_starting5 = g_starting5Pending;
        g_starting5.startedAt = GetTickCount();
        std::memset(&g_starting5Pending, 0, sizeof(g_starting5Pending));
    }
    if (!g_starting5.active) return;
    const scoreboardconfig::Config& config =
        scoreboardconfig::GetStarting5();
    float x, y, opacity;
    if (!CalculatePresentationAnimation(config, g_starting5.startedAt,
            &g_starting5.active, &x, &y, &opacity))
        return;

    popup::Load(g_customOverlayName);
    const popup::TeamVisual* team = popup::FindTeamByShortCode(
        g_starting5.values[10]);
    const bool awaySide = _stricmp(g_starting5.values[10],
        g_broadcast.awayLogoId) == 0;
    const bool homeSide = _stricmp(g_starting5.values[10],
        g_broadcast.homeLogoId) == 0;

    scoreboard::Frame frame = {};
    for (int i = 0; i < 11; ++i)
        frame.starting5Values[i] = g_starting5.values[i];
    frame.starting5TeamName = team ? team->teamName : "";
    frame.starting5Side = awaySide ? "away" : homeSide ? "home" : "unknown";
    const D3DCOLOR fallback = awaySide ? g_broadcast.awayColor :
        homeSide ? g_broadcast.homeColor : D3DCOLOR_XRGB(48, 48, 48);
    frame.starting5TeamColor = team ? team->primaryColor : fallback;
    frame.starting5PrimaryColor = frame.starting5TeamColor;
    frame.starting5SecondaryColor = team ? team->secondaryColor : fallback;
    frame.starting5TeamLogo = team ? popup::GetLogoTexture(device,
        team->databaseTeamID) : nullptr;
    char portraitPath[MAX_PATH] = {};
    for (int i = 0; i < 5; ++i) {
        portraitPath[0] = '\0';
        if (g_starting5.values[i][0])
            std::snprintf(portraitPath, sizeof(portraitPath),
                "portraits\\%s.png", g_starting5.values[i]);
        frame.starting5PlayerPortraits[i] = portraitPath[0] ?
            popup::GetOverlayTexture(device, g_customOverlayName,
                "starting5", portraitPath) : nullptr;
    }
    scoreboard::RenderStarting5(device, frame, g_customOverlayName,
        x, y, opacity);
}

void RenderOutroOverlay(IDirect3DDevice9* device)
{
    if (!g_customOverlayEnabled || !g_outroLayoutAvailable ||
        g_fullScreenTransitionActive || !g_outro.active || !device)
        return;
    const scoreboardconfig::Config& config = scoreboardconfig::GetOutro();
    float x, y, opacity;
    if (!CalculatePresentationAnimation(config, g_outro.startedAt,
            &g_outro.active, &x, &y, &opacity))
        return;

    popup::Load(g_customOverlayName);
    const popup::TeamVisual* away = popup::FindTeamByShortCode(
        g_outro.values[12]);
    const popup::TeamVisual* home = popup::FindTeamByShortCode(
        g_outro.values[13]);
    scoreboard::Frame frame = {};
    for (int i = 0; i < 15; ++i)
        frame.outroValues[i] = g_outro.values[i];
    frame.awayColor = away ? away->primaryColor : g_broadcast.awayColor;
    frame.homeColor = home ? home->primaryColor : g_broadcast.homeColor;
    frame.awaySecondaryColor = away ? away->secondaryColor : frame.awayColor;
    frame.homeSecondaryColor = home ? home->secondaryColor : frame.homeColor;
    frame.awayLogo = away ? popup::GetLogoTexture(device,
        away->databaseTeamID) : nullptr;
    frame.homeLogo = home ? popup::GetLogoTexture(device,
        home->databaseTeamID) : nullptr;
    scoreboard::RenderOutro(device, frame, g_customOverlayName,
        x, y, opacity);
}

void RenderLineupsOverlay(IDirect3DDevice9* device)
{
    if (!g_customOverlayEnabled || !g_lineupsLayoutAvailable ||
        g_fullScreenTransitionActive || !g_lineups.active || !device)
        return;
    const scoreboardconfig::Config& config = scoreboardconfig::GetLineups();
    float x, y, opacity;
    if (!CalculatePresentationAnimation(config, g_lineups.startedAt,
            &g_lineups.active, &x, &y, &opacity))
        return;

    popup::Load(g_customOverlayName);
    const popup::TeamVisual* away = popup::FindTeamByShortCode(
        g_lineups.values[10]);
    const popup::TeamVisual* home = popup::FindTeamByShortCode(
        g_lineups.values[12]);
    scoreboard::Frame frame = {};
    for (int i = 0; i < 14; ++i)
        frame.lineupsValues[i] = g_lineups.values[i];
    frame.awayColor = away ? away->primaryColor : g_broadcast.awayColor;
    frame.homeColor = home ? home->primaryColor : g_broadcast.homeColor;
    frame.awaySecondaryColor = away ? away->secondaryColor : frame.awayColor;
    frame.homeSecondaryColor = home ? home->secondaryColor : frame.homeColor;
    frame.awayLogo = away ? popup::GetLogoTexture(device,
        away->databaseTeamID) : nullptr;
    frame.homeLogo = home ? popup::GetLogoTexture(device,
        home->databaseTeamID) : nullptr;
    scoreboard::RenderLineups(device, frame, g_customOverlayName,
        x, y, opacity);
}

using OverlayRenderFn = void (*)(IDirect3DDevice9*);

void RenderConfiguredOverlays(IDirect3DDevice9* device)
{
    scoreboardconfig::Load(g_customOverlayName);
    scoreboardconfig::LoadViolation(g_customOverlayName);
    scoreboardconfig::LoadPlayCall(g_customOverlayName);
    scoreboardconfig::LoadIntro(g_customOverlayName);
    scoreboardconfig::LoadStarting5(g_customOverlayName);
    scoreboardconfig::LoadOutro(g_customOverlayName);
    scoreboardconfig::LoadLineups(g_customOverlayName);
    scoreboardconfig::LoadPlayerFoul(g_customOverlayName);

    struct RenderEntry {
        int z;
        int sequence;
        OverlayRenderFn render;
    };
    RenderEntry entries[] = {
        { scoreboardconfig::Get().overlayZ, 0, &RenderNativeScoreboard },
        { scoreboardconfig::GetViolation().overlayZ, 1,
            &RenderViolationOverlay },
        { scoreboardconfig::GetPlayCall().overlayZ, 2,
            &RenderPlayCallOverlay },
        { scoreboardconfig::GetIntro().overlayZ, 5,
            &RenderIntroOverlay },
        { scoreboardconfig::GetStarting5().overlayZ, 6,
            &RenderStarting5Overlay },
        { scoreboardconfig::GetOutro().overlayZ, 7,
            &RenderOutroOverlay },
        { scoreboardconfig::GetLineups().overlayZ, 8,
            &RenderLineupsOverlay },
        { scoreboardconfig::GetPlayerFoul().overlayZ, 3,
            &RenderPlayerFoulOverlay },
        { scoreboardconfig::GetStat().overlayZ, 4,
            &RenderGenericStatOverlay }
    };
    const int count = sizeof(entries) / sizeof(entries[0]);
    for (int i = 1; i < count; ++i) {
        const RenderEntry value = entries[i];
        int j = i - 1;
        while (j >= 0 && (entries[j].z > value.z ||
            (entries[j].z == value.z && entries[j].sequence > value.sequence))) {
            entries[j + 1] = entries[j];
            --j;
        }
        entries[j + 1] = value;
    }
    for (int i = 0; i < count; ++i)
        entries[i].render(device);
}

void CheckPopupHotReload(IDirect3DDevice9* device)
{
    const bool keyDown = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    bool reloadRequested = keyDown && !g_reloadKeyWasDown;
    const DWORD now = GetTickCount();
    if (now - g_reloadFileLastCheck >= 500) {
        g_reloadFileLastCheck = now;
        const ULONGLONG stamp = GetPopupReloadFileStamp();
        if (g_reloadFileStamp && stamp && stamp != g_reloadFileStamp)
            reloadRequested = true;
        g_reloadFileStamp = stamp;
    }
    if (reloadRequested) {
        // This runs on the Present/render thread, so cached D3D resources are
        // never released concurrently with scoreboard drawing.
        const bool themeLoaded = popup::Reload(g_customOverlayName);
        const bool fontLoaded = popupfont::Reload(device, g_customOverlayName);
        const bool scoreboardLoaded = scoreboardconfig::Reload(g_customOverlayName);
        const bool violationLoaded = scoreboardconfig::ReloadViolation(
            g_customOverlayName);
        const bool playCallLoaded = scoreboardconfig::ReloadPlayCall(
            g_customOverlayName);
        const bool introLoaded = scoreboardconfig::ReloadIntro(
            g_customOverlayName);
        const bool starting5Loaded = scoreboardconfig::ReloadStarting5(
            g_customOverlayName);
        const bool outroLoaded = scoreboardconfig::ReloadOutro(
            g_customOverlayName);
        const bool lineupsRequired = g_game &&
            g_game->version == GameVersion::Live2008;
        const bool lineupsLoaded = !lineupsRequired ||
            scoreboardconfig::ReloadLineups(g_customOverlayName);
        const bool playerFoulLoaded = scoreboardconfig::ReloadPlayerFoul(
            g_customOverlayName);
        const bool genericStatLoaded = scoreboardconfig::ReloadStat(
            g_customOverlayName, g_genericStat.subtypeKey,
            g_genericStat.playerPayload, g_genericStat.valueCase);
        g_playerFoulLayoutAvailable = playerFoulLoaded &&
            g_statsRequestHookInstalled;
        g_playCallLayoutAvailable = playCallLoaded &&
            g_originalPlayCallDataStore != nullptr;
        g_introLayoutAvailable = introLoaded &&
            g_originalIntroDataStore != nullptr;
        g_starting5LayoutAvailable = starting5Loaded &&
            g_originalPresentationDataStore != nullptr;
        g_outroLayoutAvailable = outroLoaded &&
            g_originalPresentationDataStore != nullptr;
        g_lineupsLayoutAvailable = lineupsRequired && lineupsLoaded &&
            g_originalPresentationDataStore != nullptr && g_game &&
            g_game->version == GameVersion::Live2008;
        if (!playCallLoaded) g_playCall.active = false;
        if (!introLoaded) g_intro.active = false;
        if (!starting5Loaded) g_starting5.active = false;
        if (!starting5Loaded) g_starting5Pending.active = false;
        if (!outroLoaded) g_outro.active = false;
        if (!lineupsLoaded) {
            g_lineups.active = false;
            g_lineupsTransitionHiddenAt = 0;
        }
        if (!playerFoulLoaded) g_playerFoul.active = false;
        if (!genericStatLoaded) g_genericStat.active = false;
        g_loggedAwayLogoTeam = INT_MIN;
        g_loggedHomeLogoTeam = INT_MIN;
        g_loggedStatTeamCode[0] = '\0';
        AppendDiagnostic(
            "Popup hot reload: teams=%s font=%s scoreboard=%s violation=%s "
            "playCall=%s intro=%s starting5=%s outro=%s lineups=%s playerFoul=%s "
            "stat=%s error=%s\n",
            themeLoaded ? "OK" : "FAILED",
            fontLoaded ? "OK" : "FAILED",
            scoreboardLoaded ? "OK" : "FAILED",
            violationLoaded ? "OK" : "FAILED",
            playCallLoaded ? "OK" : "FAILED",
            introLoaded ? "OK" : "FAILED",
            starting5Loaded ? "OK" : "FAILED",
            outroLoaded ? "OK" : "FAILED",
            lineupsLoaded ? "OK" : "FAILED",
            playerFoulLoaded ? "OK" : "FAILED",
            genericStatLoaded ? "OK" : "FAILED",
            themeLoaded && scoreboardLoaded && violationLoaded && playCallLoaded &&
                introLoaded && starting5Loaded && outroLoaded && lineupsLoaded &&
                playerFoulLoaded && genericStatLoaded ? "<none>" :
                (!themeLoaded ? popup::GetLastError() :
                    scoreboardconfig::GetLastError()));
    }
    const bool csvKeyDown=(GetAsyncKeyState(VK_F6)&0x8000)!=0;
    if (csvKeyDown && !g_boxScoreCsvKeyWasDown && g_game &&
        (g_game->version == GameVersion::Live2005 ||
         g_game->version == GameVersion::Live2006 ||
         g_game->version == GameVersion::Live2007 ||
         g_game->version == GameVersion::Live2008)) {
        const bool ok = g_game->version == GameVersion::Live2005
            ? ExportBoxScoreCsv05()
            : (g_game->version == GameVersion::Live2006
                ? ExportBoxScoreCsv06()
                : (g_game->version == GameVersion::Live2007
                    ? ExportBoxScoreCsv07() : ExportBoxScoreCsv08()));
        if (!ok) {
            AppendDiagnostic("F6 box score export requested before the live-player cache became valid.\n");
            if (g_game->version == GameVersion::Live2008)
                ExportLivePlayerManagerCsv08();
        }
    }
    g_boxScoreCsvKeyWasDown=csvKeyDown;
    g_reloadKeyWasDown = keyDown;
}

HRESULT WINAPI HookPresent(IDirect3DDevice9* device, const RECT* sourceRect,
    const RECT* destinationRect, HWND destinationWindow,
    const RGNDATA* dirtyRegion)
{
    if (!g_presentReached) {
        g_presentReached = true;
        D3DVIEWPORT9 viewport = {};
        device->GetViewport(&viewport);
        AppendDiagnostic(
            "Native scoreboard reached Present device=%p viewport=%ux%u.\n",
            device, viewport.Width, viewport.Height);
    }
    CheckPopupHotReload(device);
    if (SUCCEEDED(device->BeginScene())) {
        RenderConfiguredOverlays(device);
        device->EndScene();
    }
    return g_originalPresent(device, sourceRect, destinationRect,
        destinationWindow, dirtyRegion);
}

bool ReplaceVtableEntry(void** table, unsigned int index,
                        void* replacement, void** original)
{
    if (!table || !replacement || !original) return false;
    DWORD oldProtection = 0;
    if (!VirtualProtect(&table[index], sizeof(void*), PAGE_EXECUTE_READWRITE,
                        &oldProtection))
        return false;
    *original = table[index];
    table[index] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&table[index], sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &table[index], sizeof(void*));
    return true;
}

void HookDevice(IDirect3DDevice9* device)
{
    if (!device) return;
    void** table = *reinterpret_cast<void***>(device);
    if (table == g_hookedDeviceVtable) {
        AppendDiagnostic("D3D9 device=%p reuses hooked vtable=%p.\n",
            device, table);
        return;
    }

    void* originalPresent = nullptr;
    if (!ReplaceVtableEntry(table, 17, reinterpret_cast<void*>(&HookPresent),
                            &originalPresent)) {
        AppendDiagnostic("Failed to hook Present for device=%p vtable=%p.\n",
            device, table);
        return;
    }
    g_originalPresent = reinterpret_cast<PresentFn>(originalPresent);
    g_hookedDeviceVtable = table;
    AppendDiagnostic(
        "Hooked D3D9 device=%p vtable=%p Present=%p.\n",
        device, table, originalPresent);
}

class D3D9Proxy : public IDirect3D9 {
public:
    explicit D3D9Proxy(IDirect3D9* real) : real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
        { return real_->QueryInterface(riid, object); }
    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return real_->Release(); }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* initialize) override
        { return real_->RegisterSoftwareDevice(initialize); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override
        { return real_->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags,
        D3DADAPTER_IDENTIFIER9* identifier) override
        { return real_->GetAdapterIdentifier(adapter, flags, identifier); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter,
        D3DFORMAT format) override
        { return real_->GetAdapterModeCount(adapter, format); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT format,
        UINT mode, D3DDISPLAYMODE* displayMode) override
        { return real_->EnumAdapterModes(adapter, format, mode, displayMode); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter,
        D3DDISPLAYMODE* mode) override
        { return real_->GetAdapterDisplayMode(adapter, mode); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type,
        D3DFORMAT adapterFormat, D3DFORMAT backBufferFormat,
        BOOL windowed) override
        { return real_->CheckDeviceType(adapter, type, adapterFormat,
              backBufferFormat, windowed); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type,
        D3DFORMAT adapterFormat, DWORD usage, D3DRESOURCETYPE resourceType,
        D3DFORMAT checkFormat) override
        { return real_->CheckDeviceFormat(adapter, type, adapterFormat, usage,
              resourceType, checkFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter,
        D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
        D3DMULTISAMPLE_TYPE multiSampleType, DWORD* qualityLevels) override
        { return real_->CheckDeviceMultiSampleType(adapter, type, format,
              windowed, multiSampleType, qualityLevels); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter,
        D3DDEVTYPE type, D3DFORMAT adapterFormat, D3DFORMAT renderTargetFormat,
        D3DFORMAT depthStencilFormat) override
        { return real_->CheckDepthStencilMatch(adapter, type, adapterFormat,
              renderTargetFormat, depthStencilFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter,
        D3DDEVTYPE type, D3DFORMAT sourceFormat,
        D3DFORMAT targetFormat) override
        { return real_->CheckDeviceFormatConversion(adapter, type,
              sourceFormat, targetFormat); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type,
        D3DCAPS9* caps) override
        { return real_->GetDeviceCaps(adapter, type, caps); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override
        { return real_->GetAdapterMonitor(adapter); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type,
        HWND focusWindow, DWORD behaviorFlags,
        D3DPRESENT_PARAMETERS* parameters,
        IDirect3DDevice9** returnedDevice) override
    {
        const HRESULT result = real_->CreateDevice(adapter, type, focusWindow,
            behaviorFlags, parameters, returnedDevice);
        AppendDiagnostic("D3D9 proxy CreateDevice result=%08X device=%p\n",
            static_cast<unsigned int>(result),
            returnedDevice ? *returnedDevice : nullptr);
        if (SUCCEEDED(result) && returnedDevice && *returnedDevice)
            HookDevice(*returnedDevice);
        return result;
    }

private:
    IDirect3D9* real_;
};

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion)
{
    IDirect3D9* direct3D = g_originalDirect3DCreate9(sdkVersion);
    AppendDiagnostic("Intercepted Direct3DCreate9(%u), real=%p\n",
        sdkVersion, direct3D);
    return direct3D ? new D3D9Proxy(direct3D) : nullptr;
}

bool HookImportedFunctionInModule(HMODULE module, const char* dllName,
                                  const char* functionName,
                                  void* replacement, void** original)
{
    unsigned char* base = reinterpret_cast<unsigned char*>(module);
    if (!base) return false;
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;

    IMAGE_IMPORT_DESCRIPTOR* descriptor =
        reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* importedDll = reinterpret_cast<const char*>(
            base + descriptor->Name);
        if (_stricmp(importedDll, dllName) != 0) continue;

        IMAGE_THUNK_DATA* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->OriginalFirstThunk);
        IMAGE_THUNK_DATA* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->FirstThunk);

        // Some NBA Live 07 executables have a stripped OriginalFirstThunk.
        // Their FirstThunk already contains resolved function addresses, not
        // IMAGE_IMPORT_BY_NAME RVAs. Match the resolved export directly.
        if (!descriptor->OriginalFirstThunk) {
            HMODULE importedModule = GetModuleHandleA(importedDll);
            FARPROC importedFunction = importedModule
                ? GetProcAddress(importedModule, functionName)
                : nullptr;
            if (!importedFunction) return false;

            for (; addresses->u1.Function; ++addresses) {
                if (addresses->u1.Function !=
                    reinterpret_cast<uintptr_t>(importedFunction))
                    continue;
                DWORD oldProtection = 0;
                if (!VirtualProtect(&addresses->u1.Function,
                        sizeof(uintptr_t), PAGE_READWRITE, &oldProtection))
                    return false;
                if (!*original)
                    *original = reinterpret_cast<void*>(
                        addresses->u1.Function);
                addresses->u1.Function =
                    reinterpret_cast<uintptr_t>(replacement);
                DWORD ignored = 0;
                VirtualProtect(&addresses->u1.Function, sizeof(uintptr_t),
                    oldProtection, &ignored);
                return true;
            }
            return false;
        }

        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char*>(name->Name), functionName))
                continue;
            DWORD oldProtection = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(uintptr_t),
                                PAGE_READWRITE, &oldProtection))
                return false;
            if (!*original)
                *original = reinterpret_cast<void*>(addresses->u1.Function);
            addresses->u1.Function = reinterpret_cast<uintptr_t>(replacement);
            DWORD ignored = 0;
            VirtualProtect(&addresses->u1.Function, sizeof(uintptr_t),
                           oldProtection, &ignored);
            return true;
        }
    }
    return false;
}

bool HookImportedFunction(const char* dllName, const char* functionName,
                          void* replacement, void** original)
{
    bool hooked = false;
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32First(snapshot, &entry)) {
        do {
            __try {
                if (HookImportedFunctionInModule(entry.hModule, dllName,
                        functionName, replacement, original)) {
                    hooked = true;
                    AppendDiagnostic(
                        "Hooked %s!%s import in %s base=%p.\n",
                        dllName, functionName, entry.szModule,
                        entry.modBaseAddr);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                AppendDiagnostic(
                    "Skipped invalid import table in %s base=%p.\n",
                    entry.szModule, entry.modBaseAddr);
            }
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return hooked && *original != nullptr;
}

bool HookSharedD3D9DeviceVtable()
{
    char systemDirectory[MAX_PATH] = {};
    if (!GetSystemDirectoryA(systemDirectory, MAX_PATH))
        return false;

    char d3d9Path[MAX_PATH] = {};
    std::snprintf(d3d9Path, sizeof(d3d9Path), "%s\\d3d9.dll",
        systemDirectory);
    g_systemD3D9Module = LoadLibraryA(d3d9Path);
    if (!g_systemD3D9Module) {
        AppendDiagnostic("Failed to load system D3D9 from %s.\n", d3d9Path);
        return false;
    }

    const auto createDirect3D = reinterpret_cast<Direct3DCreate9Fn>(
        GetProcAddress(g_systemD3D9Module, "Direct3DCreate9"));
    if (!createDirect3D) {
        AppendDiagnostic("System D3D9 has no Direct3DCreate9 export.\n");
        return false;
    }

    HWND window = CreateWindowExA(0, "STATIC", "NBA Live D3D9 probe",
        WS_POPUP, 0, 0, 16, 16, nullptr, nullptr,
        GetModuleHandleA(nullptr), nullptr);
    if (!window) {
        AppendDiagnostic("Failed to create D3D9 probe window.\n");
        return false;
    }

    IDirect3D9* direct3D = createDirect3D(D3D_SDK_VERSION);
    if (!direct3D) {
        DestroyWindow(window);
        AppendDiagnostic("System Direct3DCreate9 failed for probe device.\n");
        return false;
    }

    D3DPRESENT_PARAMETERS parameters = {};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferFormat = D3DFMT_UNKNOWN;
    parameters.hDeviceWindow = window;

    IDirect3DDevice9* device = nullptr;
    HRESULT result = direct3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
        &parameters, &device);
    if (FAILED(result)) {
        result = direct3D->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
            &parameters, &device);
    }

    if (SUCCEEDED(result) && device) {
        HookDevice(device);
        AppendDiagnostic(
            "Installed shared D3D9 Present hook from probe device=%p.\n",
            device);
    }
    else {
        AppendDiagnostic("D3D9 probe CreateDevice failed result=%08X.\n",
            static_cast<unsigned int>(result));
    }

    if (device) device->Release();
    direct3D->Release();
    DestroyWindow(window);
    return SUCCEEDED(result) && g_originalPresent != nullptr;
}

DWORD WINAPI DeferredD3D9VtableHook(void*)
{
    // This runs only after DLL initialization has released the loader lock.
    // LoadLibrary/CreateWindow/CreateDevice are unsafe in a global ASI
    // constructor and previously produced debugger-dependent startup crashes.
    if (!HookSharedD3D9DeviceVtable())
        AppendDiagnostic("Native scoreboard D3D9 hook was not installed.\n");
    return 0;
}

void InitializeNativeScoreboard()
{
    void* original = nullptr;
    if (HookImportedFunction("d3d9.dll", "Direct3DCreate9",
            reinterpret_cast<void*>(&HookDirect3DCreate9), &original)) {
        g_originalDirect3DCreate9 =
            reinterpret_cast<Direct3DCreate9Fn>(original);
        AppendDiagnostic("Native scoreboard D3D9 hook installed.\n");
    }
    else {
        AppendDiagnostic(
            "No Direct3DCreate9 import hook; scheduling shared device vtable.\n");
        if (InterlockedCompareExchange(&g_d3d9ProbeScheduled, 1, 0) == 0) {
            HANDLE thread = CreateThread(nullptr, 0,
                &DeferredD3D9VtableHook, nullptr, 0, nullptr);
            if (thread) {
                CloseHandle(thread);
            }
            else {
                InterlockedExchange(&g_d3d9ProbeScheduled, 0);
                AppendDiagnostic(
                    "Failed to create deferred D3D9 hook thread.\n");
            }
        }
    }
}

unsigned int RedirectDirectCalls(uintptr_t target, void* replacement)
{
    HMODULE module = GetModuleHandleA(nullptr);
    if (!module) return 0;
    unsigned char* base = reinterpret_cast<unsigned char*>(module);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    unsigned int count = 0;
    for (unsigned int s = 0;
         s < nt->FileHeader.NumberOfSections;
         ++s, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        unsigned char* start = base + section->VirtualAddress;
        const size_t size = static_cast<size_t>(section->Misc.VirtualSize);
        for (size_t offset = 0; offset + 5 <= size; ++offset) {
            unsigned char* instruction = start + offset;
            if (instruction[0] != 0xE8) continue;
            const int32_t displacement =
                *reinterpret_cast<int32_t*>(instruction + 1);
            const uintptr_t destination =
                reinterpret_cast<uintptr_t>(instruction + 5) + displacement;
            if (destination != target) continue;
            patch::RedirectCall(
                reinterpret_cast<unsigned int>(instruction),
                replacement);
            ++count;
            offset += 4;
        }
    }
    return count;
}

uintptr_t GetDirectCallDestination(uintptr_t callAddress)
{
    if (!callAddress) return 0;
    const unsigned char* instruction =
        reinterpret_cast<const unsigned char*>(callAddress);
    if (instruction[0] != 0xE8) return 0;
    const int32_t displacement =
        *reinterpret_cast<const int32_t*>(instruction + 1);
    return callAddress + 5 + displacement;
}

void Initialize(const GameAddresses& game)
{
    g_game = &game;
    g_originalSendEvent = reinterpret_cast<SendEventFn>(game.sendEvent);
    LoadCustomOverlaySettings();

    if (g_customOverlayEnabled && game.version == GameVersion::Live2005) {
        patch::SetUChar(0x0054023F + 3, 0); // Generic show
        patch::SetUChar(0x0055AA7F + 3, 0); // JumpBallToss
        patch::SetUChar(0x0055AADF + 3, 0); // QuarterStart
        patch::SetUChar(0x0055AB0F + 3, 0); // UnPauseGame
    }

    // NBA Live 06 FEOverlayScore::Show sets its controller-visible flag at
    // [this+0x24] before sending ScoreShowEvent. Keep that flag cleared so a
    // later ShowOverlaysEvent (notably pause/resume) cannot resurrect the
    // native scoreboard. The immediate byte of C6 41 24 01 is at +3.
    if (g_customOverlayEnabled && game.version == GameVersion::Live2006)
        patch::SetUChar(0x0053AB8F + 3, 0);

    // Live 06 also has three overlay-manager ScoreShowEvent wrappers. Each
    // sets its own [this+0x24] visible byte before SendEvent reaches our hook.
    // The pause path uses the third wrapper (call logged at 0x0055B1CE), so
    // merely consuming ScoreShowEvent is too late: ShowOverlayManager sees the
    // byte and restores the native board. Keep all three wrapper flags clear.
    if (g_customOverlayEnabled && game.version == GameVersion::Live2006) {
        patch::SetUChar(0x0055B12F + 3, 0);
        patch::SetUChar(0x0055B18F + 3, 0);
        patch::SetUChar(0x0055B1BF + 3, 0);
    }

    if (g_customOverlayEnabled && game.version == GameVersion::Live2007) {
        patch::SetUChar(0x0054DC8F + 3, 0);
        patch::SetUChar(0x0054E1CF + 3, 0);
        patch::SetUChar(0x0054E232 + 3, 0);
        patch::SetUChar(0x0054E27F + 3, 0);
        patch::SetUChar(0x0055B73F + 3, 0);
        patch::SetUChar(0x0055B84D + 3, 0);
    }

    // NBA Live 08 uses +0x30 for the native scoreboard controller-visible
    // byte. Neutralize every known ScoreShowEvent wrapper, including the two
    // overlay-manager paths used while restoring overlays after pause.
    if (g_customOverlayEnabled && game.version == GameVersion::Live2008) {
        patch::SetUChar(0x005547BF + 3, 0);
        patch::SetUChar(0x00554D3F + 3, 0);
        patch::SetUChar(0x00554DA2 + 3, 0);
        patch::SetUChar(0x00554DEF + 3, 0);
        patch::SetUChar(0x0056C57F + 3, 0);
        patch::SetUChar(0x0056C6AD + 3, 0);
    }

    FILE* file = nullptr;

    char logPath[MAX_PATH] = {};
    BuildLogPath(
        logPath,
        sizeof(logPath),
        "score_events.log"
    );
    if (logPath[0]) {
        file = std::fopen(logPath, "w");
        if (file) {
            std::fprintf(file, "%s score.big event log\nSendEvent=%08X\n\n",
                game.name, static_cast<unsigned int>(game.sendEvent));
            std::fclose(file);
        }
    }

    char logPathext[MAX_PATH] = {};
    BuildLogPath(
        logPathext,
        sizeof(logPathext),
        "extended_score_state.log"
    );
    if (logPathext[0]) {
        file = std::fopen(logPathext, "w");
        if (file) {
            std::fprintf(file,
                "%s confirmed getter diagnostics\n"
                "gGdInfoCentral=%08X\nGetGameClock=%08X\n"
                "GetClockUnitsPerSecond=%08X\n"
                "IsShotClockValid=%08X\nGetShotClock=%08X\n"
                "GetTeamScore=%08X\nGetTeamIDFromSide=%08X\n"
                "GetTeamFouls=%08X\nGetTeamTimeoutsLeft=%08X\n\n",
                game.name,
                static_cast<unsigned int>(game.gdInfoCentralAddress),
                static_cast<unsigned int>(game.getGameClock),
                static_cast<unsigned int>(game.getClockUnitsPerSecond),
                static_cast<unsigned int>(game.isShotClockValid),
                static_cast<unsigned int>(game.getShotClock),
                static_cast<unsigned int>(game.getTeamScore),
                static_cast<unsigned int>(game.getTeamIDFromSide),
                static_cast<unsigned int>(game.getTeamFouls),
                static_cast<unsigned int>(game.getTeamTimeoutsLeft));
            std::fclose(file);
        }
    }

    char logPathPre[MAX_PATH] = {};
    BuildLogPath(
        logPathPre,
        sizeof(logPathPre),
        "presentation_payloads.log"
    );
    if (logPathPre[0] &&
        (game.version == GameVersion::Live2007 ||
         game.version == GameVersion::Live2008)) {
        file = std::fopen(logPathPre, "w");
        if (file) {
            std::fprintf(file,
                "%s Starting5/Outro/Lineups completed-payload log\n\n",
                game.name);
            std::fclose(file);
        }
    }

    AppendDiagnostic(
        "Custom overlay: enabled=%s name=%s\n"
        "main.ini=%s exists=%s\n"
        "scoreboard.json=%s exists=%s\n",
        g_customOverlayEnabled ? "yes" : "no", g_customOverlayName,
        g_customOverlayIniPath,
        GetFileAttributesA(g_customOverlayIniPath) != INVALID_FILE_ATTRIBUTES
            ? "yes" : "no",
        g_customOverlayScoreboardPath,
        GetFileAttributesA(g_customOverlayScoreboardPath) !=
            INVALID_FILE_ATTRIBUTES ? "yes" : "no");

    if (game.version == GameVersion::Live2008) {
        constexpr uintptr_t kBoxScoreInit=0x005C4DA0;
        constexpr uintptr_t calls[]={0x005D9AED,0x005DAE8D};
        bool valid=true; for(unsigned i=0;i<2;++i) if(GetDirectCallDestination(calls[i])!=kBoxScoreInit) valid=false;
        if(valid){ ResetLiveGameData08(); g_originalBoxScoreInit08=(BoxScoreInitFn)kBoxScoreInit; g_getBoxScoreGame08=(GetBoxScoreGameFn)0x004F2400; g_playerGameStatsGetStat08=(PlayerGameStatsGetStatFn)0x004F17D0;
            for(unsigned i=0;i<2;++i) patch::RedirectCall((unsigned int)calls[i],reinterpret_cast<void*>(&HookBoxScoreInit08));
            AppendDiagnostic("NBA Live 08 BoxScoreMgr capture installed: init=%08X calls=%08X,%08X getGame=%08X getStat=%08X CSV=F6 (live-player cache).\n",(unsigned)kBoxScoreInit,(unsigned)calls[0],(unsigned)calls[1],0x004F2400u,0x004F17D0u);
        } else AppendDiagnostic("NBA Live 08 BoxScoreMgr capture skipped: call validation failed.\n");
    }

    // NBA Live 07/08: suppress the native Starting5 and Outro presentation
    // movies at their exact per-game builder request calls. All four call
    // targets are validated before any redirect is installed. Payload stores
    // and event-state transitions remain native and continue normally.
    if (game.version == GameVersion::Live2007 ||
        game.version == GameVersion::Live2008) {
        uintptr_t nativePresentationRequest = 0;
        uintptr_t presentationDataStore = 0;
        uintptr_t starting5RequestCall = 0;
        uintptr_t starting5StoreCall = 0;
        uintptr_t outroRequestCall = 0;
        uintptr_t outroStoreCall = 0;

        if (game.version == GameVersion::Live2007) {
            nativePresentationRequest = 0x0054D7C0;
            presentationDataStore = 0x0051E070;
            starting5RequestCall = 0x00578838;
            starting5StoreCall = 0x0057884C;
            outroRequestCall = 0x00578F70;
            outroStoreCall = 0x00578F81;
        }
        else {
            // Confirmed against nbalive08.exe SHA-256:
            // 54e5a6b1f41b0e45783e7ffdf97bdf9a43daa4aaef1310efa89f1bae981f3161
            nativePresentationRequest = 0x0056CF50;
            presentationDataStore = 0x0053AAE0;
            starting5RequestCall = 0x00594BC5;
            starting5StoreCall = 0x00594BD9;
            outroRequestCall = 0x0059530D;
            outroStoreCall = 0x0059531F;
        }

        const uintptr_t starting5Target =
            GetDirectCallDestination(starting5RequestCall);
        const uintptr_t starting5StoreTarget =
            GetDirectCallDestination(starting5StoreCall);
        const uintptr_t outroTarget =
            GetDirectCallDestination(outroRequestCall);
        const uintptr_t outroStoreTarget =
            GetDirectCallDestination(outroStoreCall);

        if (starting5Target == nativePresentationRequest &&
            outroTarget == nativePresentationRequest &&
            starting5StoreTarget == presentationDataStore &&
            outroStoreTarget == presentationDataStore) {
            g_originalPresentationDataStore =
                reinterpret_cast<StoreOverlayDataFn>(presentationDataStore);
            patch::RedirectCall(
                static_cast<unsigned int>(starting5RequestCall),
                reinterpret_cast<void*>(&SuppressNativePresentationRequest));
            patch::RedirectCall(
                static_cast<unsigned int>(outroRequestCall),
                reinterpret_cast<void*>(&SuppressNativePresentationRequest));
            patch::RedirectCall(
                static_cast<unsigned int>(starting5StoreCall),
                reinterpret_cast<void*>(&HookStarting5DataStore));
            patch::RedirectCall(
                static_cast<unsigned int>(outroStoreCall),
                reinterpret_cast<void*>(&HookOutroDataStore));
            g_starting5LayoutAvailable =
                scoreboardconfig::LoadStarting5(g_customOverlayName);
            g_outroLayoutAvailable =
                scoreboardconfig::LoadOutro(g_customOverlayName);
            AppendDiagnostic(
                "%s native Starting5 and Outro disabled: "
                "requests=%08X,%08X target=%08X; payload stores="
                "%08X,%08X hooked for capture, original store=%08X and "
                "event lifecycle retained; layouts Starting5=%s Outro=%s.\n",
                game.name,
                static_cast<unsigned int>(starting5RequestCall),
                static_cast<unsigned int>(outroRequestCall),
                static_cast<unsigned int>(nativePresentationRequest),
                static_cast<unsigned int>(starting5StoreCall),
                static_cast<unsigned int>(outroStoreCall),
                static_cast<unsigned int>(presentationDataStore),
                g_starting5LayoutAvailable ? "yes" : "no",
                g_outroLayoutAvailable ? "yes" : "no");
        }
        else {
            AppendDiagnostic(
                "%s Starting5/Outro suppression/capture skipped: "
                "call-target validation failed (Starting5 request=%08X "
                "store=%08X; Outro request=%08X store=%08X; expected request="
                "%08X store=%08X).\n",
                game.name,
                static_cast<unsigned int>(starting5Target),
                static_cast<unsigned int>(starting5StoreTarget),
                static_cast<unsigned int>(outroTarget),
                static_cast<unsigned int>(outroStoreTarget),
                static_cast<unsigned int>(nativePresentationRequest),
                static_cast<unsigned int>(presentationDataStore));
        }
    }

    // NBA Live 08-only FEOverlayLineups. Suppress its native movie only when
    // lineups.json exists and both exact call targets still match the
    // confirmed executable. The native key-12 event lifecycle remains intact.
    if (g_customOverlayEnabled && game.version == GameVersion::Live2008) {
        constexpr uintptr_t kLineupsRequestCall = 0x0059589D;
        constexpr uintptr_t kLineupsStoreCall = 0x005958B1;
        constexpr uintptr_t kNativePresentationRequest = 0x0056CF50;
        constexpr uintptr_t kPresentationDataStore = 0x0053AAE0;
        const bool layoutLoaded =
            scoreboardconfig::ReloadLineups(g_customOverlayName);
        const uintptr_t requestTarget =
            GetDirectCallDestination(kLineupsRequestCall);
        const uintptr_t storeTarget =
            GetDirectCallDestination(kLineupsStoreCall);
        if (layoutLoaded && requestTarget == kNativePresentationRequest &&
            storeTarget == kPresentationDataStore) {
            g_originalPresentationDataStore =
                reinterpret_cast<StoreOverlayDataFn>(kPresentationDataStore);
            patch::RedirectCall(
                static_cast<unsigned int>(kLineupsRequestCall),
                reinterpret_cast<void*>(&SuppressNativePresentationRequest));
            patch::RedirectCall(
                static_cast<unsigned int>(kLineupsStoreCall),
                reinterpret_cast<void*>(&HookLineupsDataStore));
            g_lineupsLayoutAvailable = true;
            AppendDiagnostic(
                "NBA Live 08 native in-game lineups disabled: request=%08X "
                "target=%08X; payload store=%08X hooked, original store=%08X; "
                "key-12 lifecycle retained; layout=yes.\n",
                static_cast<unsigned int>(kLineupsRequestCall),
                static_cast<unsigned int>(kNativePresentationRequest),
                static_cast<unsigned int>(kLineupsStoreCall),
                static_cast<unsigned int>(kPresentationDataStore));
        }
        else {
            g_lineupsLayoutAvailable = false;
            AppendDiagnostic(
                "NBA Live 08 in-game lineups custom overlay unavailable; "
                "native retained: layout=%s request=%08X store=%08X "
                "expected request=%08X store=%08X.\n",
                layoutLoaded ? "yes" : "no",
                static_cast<unsigned int>(requestTarget),
                static_cast<unsigned int>(storeTarget),
                static_cast<unsigned int>(kNativePresentationRequest),
                static_cast<unsigned int>(kPresentationDataStore));
        }
    }

    const unsigned int sendEventCalls = RedirectDirectCalls(
        game.sendEvent, reinterpret_cast<void*>(&HookSendEvent));
    AppendDiagnostic("Redirected SendEvent calls: %u\n", sendEventCalls);

    char logPathOvr[MAX_PATH] = {};
    BuildLogPath(
        logPathOvr,
        sizeof(logPathOvr),
        "overlay_payloads.log"
    );

    if (game.getOverlayData) {
        if (logPathOvr[0]) {
            file = std::fopen(logPathOvr, "w");
            if (file) {
                std::fprintf(file,
                    "%s overlay payload log\nGetOverlayData=%08X\n\n",
                    game.name,
                    static_cast<unsigned int>(game.getOverlayData));
                std::fclose(file);
            }
        }

        const unsigned int overlayCalls = RedirectDirectCalls(
            game.getOverlayData,
            reinterpret_cast<void*>(&HookGetOverlayData));
        AppendDiagnostic("Redirected GetOverlayData calls: %u\n",
            overlayCalls);
    }

    // All three FEOverlayIntro builders (primary, alternate and indexed)
    // request overlays~intro.big and then store the completed 15-value vector.
    // Only replace those calls when intro.json exists and every call target is
    // valid. Otherwise the native intro remains completely untouched.
    if (g_customOverlayEnabled) {
        const bool introLoaded = scoreboardconfig::LoadIntro(g_customOverlayName);
        uintptr_t storeDestination = 0;
        bool valid = introLoaded;
        for (int i = 0; i < 3; ++i) {
            const uintptr_t requestTarget = GetDirectCallDestination(
                game.introRequestCalls[i]);
            const uintptr_t currentStoreTarget = GetDirectCallDestination(
                game.introDataStoreCalls[i]);
            if (!requestTarget || !currentStoreTarget ||
                (storeDestination && storeDestination != currentStoreTarget))
                valid = false;
            if (!storeDestination) storeDestination = currentStoreTarget;
        }
        if (!valid) {
            AppendDiagnostic(
                "Intro custom overlay unavailable; native retained: "
                "layout=%s calls=%s.\n",
                introLoaded ? "yes" : "no", valid ? "valid" : "invalid");
        }
        else {
            g_originalIntroDataStore =
                reinterpret_cast<StoreOverlayDataFn>(storeDestination);
            for (int i = 0; i < 3; ++i) {
                // The indexed builder also drives the game's native player
                // introductions. Keep its movie request intact until custom
                // starting-lineup/player-intro layouts are implemented.
                if (i < 2) {
                    patch::RedirectCall(
                        static_cast<unsigned int>(game.introRequestCalls[i]),
                        reinterpret_cast<void*>(&SuppressNativeIntroRequest));
                }
                patch::RedirectCall(
                    static_cast<unsigned int>(game.introDataStoreCalls[i]),
                    reinterpret_cast<void*>(&HookIntroDataStore));
            }
            g_introLayoutAvailable = true;
            AppendDiagnostic(
                "Native match-intro movie requests disabled: stores=%08X; "
                "primary/alternate requests suppressed, three stores hooked, "
                "indexed player-introduction request retained.\n",
                static_cast<unsigned int>(storeDestination));
        }
    }

    // FEOverlayPlayCall builders in every supported game first request
    // overlays~playcall.big and then store the completed four-value vector.
    // Suppress the movie only when playcall.json is available; otherwise the
    // game's original graphic remains untouched.
    if (g_customOverlayEnabled && game.playCallRequestCall &&
        game.playCallDataStoreCall) {
        const bool layoutLoaded =
            scoreboardconfig::LoadPlayCall(g_customOverlayName);
        const uintptr_t requestDestination = GetDirectCallDestination(
            game.playCallRequestCall);
        const uintptr_t storeDestination = GetDirectCallDestination(
            game.playCallDataStoreCall);
        if (!layoutLoaded || !requestDestination || !storeDestination) {
            AppendDiagnostic(
                "Play-call custom overlay unavailable; native retained: "
                "layout=%s request=%08X store=%08X.\n",
                layoutLoaded ? "yes" : "no",
                static_cast<unsigned int>(game.playCallRequestCall),
                static_cast<unsigned int>(game.playCallDataStoreCall));
        }
        else {
            patch::RedirectCall(
                static_cast<unsigned int>(game.playCallRequestCall),
                reinterpret_cast<void*>(&SuppressNativePlayCallRequest));
            g_originalPlayCallDataStore =
                reinterpret_cast<StoreOverlayDataFn>(storeDestination);
            patch::RedirectCall(
                static_cast<unsigned int>(game.playCallDataStoreCall),
                reinterpret_cast<void*>(&HookPlayCallDataStore));
            g_playCallLayoutAvailable = true;
            AppendDiagnostic(
                "Native play-call movie request disabled: "
                "request=%08X store=%08X target=%08X.\n",
                static_cast<unsigned int>(game.playCallRequestCall),
                static_cast<unsigned int>(game.playCallDataStoreCall),
                static_cast<unsigned int>(storeDestination));
        }
    }

    char logPathStat[MAX_PATH] = {};
    BuildLogPath(
        logPathStat,
        sizeof(logPathStat),
        "stat_payloads.log"
    );

    char logPathStatCat[MAX_PATH] = {};
    BuildLogPath(
        logPathStatCat,
        sizeof(logPathStatCat),
        "stat_catalog.log"
    );
    // FEOverlayStats builders in every supported game converge here after
    // completing their owned BBallString vectors. A request gets a custom
    // instance only when its subtype-specific or family layout exists;
    // unknown, invalid and unconfigured requests remain native.
    if (game.statsDataStore) {
        if (logPathStat[0]) {
            file = std::fopen(logPathStat, "w");
            if (file) {
                std::fprintf(file,
                    "%s completed FEOverlayStats payload log\n"
                    "Store=%08X\n\n",
                    game.name,
                    static_cast<unsigned int>(game.statsDataStore));
                std::fclose(file);
            }
        }

        std::memset(g_statCatalog, 0, sizeof(g_statCatalog));
        if (logPathStatCat[0]) {
            file = std::fopen(logPathStatCat, "w");
            if (file) {
                std::fprintf(file,
                    "%s deduplicated FEOverlayStats subtype catalog\n"
                    "One representative is recorded for each distinct "
                    "control/count/non-empty-slot layout.\n\n",
                    game.name);
                std::fclose(file);
            }
        }

        g_originalStatsDataStore =
            reinterpret_cast<StoreOverlayDataFn>(game.statsDataStore);

        // NBA Live 05 runtime-player/cache port.
        if (game.version == GameVersion::Live2005) {
            constexpr uintptr_t kGetPlayerPackage = 0x0051CEB0;
            constexpr uintptr_t kPlayerPackageCalls[] = {
                0x0057D4B9,
                0x0057DBEC, 0x0057DF30, 0x0057E283, 0x0057E60D,
                0x0057E973, 0x0057ECD6, 0x0057F08C, 0x0057F4C8,
                0x0057FB3E, 0x00580066, 0x005806D5, 0x00580BBF,
                0x00580EF6, 0x005812A6, 0x0058178D, 0x00581B73,
                0x00581E5C, 0x0058215C, 0x0058245C,
                0x0058AD5C, 0x0058B07C, 0x0058BBC6, 0x0058BF13,
                0x0058C3E4
            };
            bool valid = true;
            for (unsigned int i = 0;
                 valid && i < sizeof(kPlayerPackageCalls) / sizeof(kPlayerPackageCalls[0]);
                 ++i) {
                if (GetDirectCallDestination(kPlayerPackageCalls[i]) !=
                    kGetPlayerPackage)
                    valid = false;
            }
            if (valid) {
                ResetLiveGameData05();
                g_loggedBoxScoreReady05 = false;
                g_originalGetPlayerPackage =
                    reinterpret_cast<GetPlayerPackageFn>(kGetPlayerPackage);
                g_resolveRuntimePlayer05 =
                    reinterpret_cast<ResolveRuntimePlayerFn>(0x00494A20);
                g_getRuntimePlayerInt05 =
                    reinterpret_cast<PlayerQueryGetIntFn>(0x004B2960);

                for (unsigned int i = 0;
                     i < sizeof(kPlayerPackageCalls) / sizeof(kPlayerPackageCalls[0]);
                     ++i) {
                    patch::RedirectCall(
                        static_cast<unsigned int>(kPlayerPackageCalls[i]),
                        reinterpret_cast<void*>(&HookGetPlayerPackage));
                }

                constexpr uintptr_t kSubstitutionBuilderCall = 0x0058EEDD;
                constexpr uintptr_t kSubstitutionBuilder = 0x005817F0;
                if (GetDirectCallDestination(kSubstitutionBuilderCall) ==
                    kSubstitutionBuilder) {
                    g_originalSubstitutionBuilder05 =
                        reinterpret_cast<SubstitutionBuilderFn>(kSubstitutionBuilder);
                    patch::RedirectCall(
                        static_cast<unsigned int>(kSubstitutionBuilderCall),
                        reinterpret_cast<void*>(&HookSubstitutionBuilder05));
                    AppendDiagnostic(
                        "NBA Live 05 live-lineup tracker installed: substitutionBuilderCall=%08X target=%08X.\n",
                        static_cast<unsigned int>(kSubstitutionBuilderCall),
                        static_cast<unsigned int>(kSubstitutionBuilder));
                }
                else {
                    AppendDiagnostic(
                        "NBA Live 05 live-lineup tracker skipped: substitution builder call validation failed.\n");
                }

                AppendDiagnostic(
                    "NBA Live 05 featured-player/live-cache installed: packageResolver=%08X runtimeResolver=%08X intGetter=%08X manager=%08X fields=85/87/88/89 stats=00-10.\n",
                    static_cast<unsigned int>(kGetPlayerPackage),
                    0x00494A20u, 0x004B2960u, 0x00C49014u);
            }
            else {
                AppendDiagnostic(
                    "NBA Live 05 featured-player/live-cache skipped: player-package call validation failed.\n");
            }
        }

        // NBA Live 06 runtime-player/cache port. The manager/resolver/getter
        // layout remains the same 0x988 / 96-native-ID architecture used by
        // Live 07 and 08, with game-specific addresses.
        if (game.version == GameVersion::Live2006) {
            constexpr uintptr_t kGetPlayerPackage = 0x005126C0;
            constexpr uintptr_t kPlayerQueryGetInt = 0x004AE820;
            constexpr uintptr_t kPackageIdentityFieldCall = 0x005126F9;
            constexpr uintptr_t kPlayerPackageCalls[] = {
                0x0058B481, 0x0058BC2C,
                0x0058BF70, 0x0058C2C3, 0x0058C64D, 0x0058C9B3,
                0x0058CD16, 0x0058D0CC, 0x0058D508, 0x0058DBBE,
                0x0058E0E6, 0x0058E755, 0x0058EC3F, 0x0058EF76,
                0x0058F326, 0x0058F80D, 0x0058FBF3, 0x0058FEDC,
                0x005901DC, 0x005904DC,
                0x0059AC01, 0x0059AF1C, 0x0059BA6B, 0x0059BDB3,
                0x0059C284
            };
            bool valid = GetDirectCallDestination(kPackageIdentityFieldCall) ==
                kPlayerQueryGetInt;
            for (unsigned int i = 0;
                 valid && i < sizeof(kPlayerPackageCalls) / sizeof(kPlayerPackageCalls[0]);
                 ++i) {
                if (GetDirectCallDestination(kPlayerPackageCalls[i]) !=
                    kGetPlayerPackage)
                    valid = false;
            }
            if (valid) {
                ResetLiveGameData06();
                g_loggedBoxScoreReady06 = false;
                g_originalGetPlayerPackage =
                    reinterpret_cast<GetPlayerPackageFn>(kGetPlayerPackage);
                g_originalPlayerQueryGetInt =
                    reinterpret_cast<PlayerQueryGetIntFn>(kPlayerQueryGetInt);
                g_resolveRuntimePlayer06 =
                    reinterpret_cast<ResolveRuntimePlayerFn>(0x00493A60);
                g_getRuntimePlayerInt06 =
                    reinterpret_cast<PlayerQueryGetIntFn>(0x004AE820);
                patch::RedirectCall(
                    static_cast<unsigned int>(kPackageIdentityFieldCall),
                    reinterpret_cast<void*>(&HookFeaturedPlayerQueryGetInt));
                for (unsigned int i = 0;
                     i < sizeof(kPlayerPackageCalls) / sizeof(kPlayerPackageCalls[0]);
                     ++i) {
                    patch::RedirectCall(
                        static_cast<unsigned int>(kPlayerPackageCalls[i]),
                        reinterpret_cast<void*>(&HookGetPlayerPackage));
                }

                constexpr uintptr_t kSubstitutionBuilderCall = 0x005A3F4D;
                constexpr uintptr_t kSubstitutionBuilder = 0x0058F870;
                if (GetDirectCallDestination(kSubstitutionBuilderCall) ==
                    kSubstitutionBuilder) {
                    g_originalSubstitutionBuilder06 =
                        reinterpret_cast<SubstitutionBuilderFn>(kSubstitutionBuilder);
                    patch::RedirectCall(
                        static_cast<unsigned int>(kSubstitutionBuilderCall),
                        reinterpret_cast<void*>(&HookSubstitutionBuilder06));
                    AppendDiagnostic(
                        "NBA Live 06 live-lineup tracker installed: substitutionBuilderCall=%08X target=%08X.\n",
                        static_cast<unsigned int>(kSubstitutionBuilderCall),
                        static_cast<unsigned int>(kSubstitutionBuilder));
                }
                else {
                    AppendDiagnostic(
                        "NBA Live 06 live-lineup tracker skipped: substitution builder call validation failed.\n");
                }
                AppendDiagnostic(
                    "NBA Live 06 featured-player/live-cache installed: full player-package caller family, packageResolver=%08X runtimeResolver=%08X intGetter=%08X manager=%08X fields=85/87/88/89 stats=00-10.\n",
                    static_cast<unsigned int>(kGetPlayerPackage),
                    0x00493A60u, 0x004AE820u, 0x00CB1D30u);
            }
            else {
                AppendDiagnostic(
                    "NBA Live 06 featured-player/live-cache skipped: player-package call validation failed.\n");
            }
        }

        // NBA Live 07 runtime-player/cache port, structurally matched to Live 08.
        if (game.version == GameVersion::Live2007) {
            constexpr uintptr_t kGetPlayerPackage = 0x00530C70;
            constexpr uintptr_t kPlayerQueryGetInt = 0x004D68B0;
            constexpr uintptr_t kPackageIdentityFieldCall = 0x00530CA9;
            constexpr uintptr_t kPlayerPackageCalls[] = {
                // Shared/non-stat presentation callers that also feed the
                // featured-player package path.
                0x005698A1, 0x00569BBC, 0x0056A70B, 0x0056AA53,
                0x0056AF24, 0x0056C5B1,

                // FEOverlayStats player-package callers. Keep the entire
                // contiguous 07 family hooked; the original port stopped at
                // 0x0056F286, which left later player stat popups without a
                // pending jersey identity.
                0x0056CD5C, 0x0056D0A0, 0x0056D3F3, 0x0056D781,
                0x0056DB13, 0x0056DE96, 0x0056E28C, 0x0056E6D9,
                0x0056ED5E, 0x0056F286, 0x0056F8F5, 0x0056FE2F,
                0x00570166, 0x00570516, 0x005709FD, 0x00570DE3,
                0x005710CC, 0x005713CC, 0x005716CC
            };
            bool valid = GetDirectCallDestination(kPackageIdentityFieldCall) ==
                kPlayerQueryGetInt;
            for (unsigned int i = 0;
                 valid && i < sizeof(kPlayerPackageCalls) / sizeof(kPlayerPackageCalls[0]);
                 ++i) {
                if (GetDirectCallDestination(kPlayerPackageCalls[i]) !=
                    kGetPlayerPackage)
                    valid = false;
            }
            if (valid) {
                ResetLiveGameData07();
                g_loggedBoxScoreReady07 = false;
                g_originalGetPlayerPackage =
                    reinterpret_cast<GetPlayerPackageFn>(kGetPlayerPackage);
                g_originalPlayerQueryGetInt =
                    reinterpret_cast<PlayerQueryGetIntFn>(kPlayerQueryGetInt);
                g_resolveRuntimePlayer07 =
                    reinterpret_cast<ResolveRuntimePlayerFn>(0x004BB080);
                g_getRuntimePlayerInt07 =
                    reinterpret_cast<PlayerQueryGetIntFn>(0x004D68B0);
                patch::RedirectCall(
                    static_cast<unsigned int>(kPackageIdentityFieldCall),
                    reinterpret_cast<void*>(&HookFeaturedPlayerQueryGetInt));
                for (unsigned int i = 0;
                     i < sizeof(kPlayerPackageCalls) / sizeof(kPlayerPackageCalls[0]);
                     ++i) {
                    patch::RedirectCall(
                        static_cast<unsigned int>(kPlayerPackageCalls[i]),
                        reinterpret_cast<void*>(&HookGetPlayerPackage));
                }

                constexpr uintptr_t kSubstitutionBuilderCall = 0x0058050D;
                constexpr uintptr_t kSubstitutionBuilder = 0x00570A60;
                if (GetDirectCallDestination(kSubstitutionBuilderCall) ==
                    kSubstitutionBuilder) {
                    g_originalSubstitutionBuilder07 =
                        reinterpret_cast<SubstitutionBuilderFn>(kSubstitutionBuilder);
                    patch::RedirectCall(
                        static_cast<unsigned int>(kSubstitutionBuilderCall),
                        reinterpret_cast<void*>(&HookSubstitutionBuilder07));
                    AppendDiagnostic(
                        "NBA Live 07 live-lineup tracker installed: substitutionBuilderCall=%08X target=%08X.\n",
                        static_cast<unsigned int>(kSubstitutionBuilderCall),
                        static_cast<unsigned int>(kSubstitutionBuilder));
                }
                else {
                    AppendDiagnostic(
                        "NBA Live 07 live-lineup tracker skipped: substitution builder call validation failed.\n");
                }
                AppendDiagnostic(
                    "NBA Live 07 featured-player/live-cache installed: full player-package caller family, packageResolver=%08X runtimeResolver=%08X intGetter=%08X manager=%08X fields=85/87/88/89 stats=00-10.\n",
                    static_cast<unsigned int>(kGetPlayerPackage),
                    0x004BB080u, 0x004D68B0u, 0x00CEAA08u);
            }
            else {
                AppendDiagnostic(
                    "NBA Live 07 featured-player/live-cache skipped: player-package call validation failed.\n");
            }
        }

        // Every Live 08 player-stat builder resolves the player's package ID
        // through this shared function immediately before it stores the
        // completed 15-value payload. Intercept only the 16 confirmed player
        // builder call sites; team-stat builders and unrelated callers remain
        // untouched. The pointed DWORD is logged first so its identity can be
        // proven before jersey/stat getters are attached to it.
        if (game.version == GameVersion::Live2008) {
            constexpr uintptr_t kGetPlayerPackage = 0x00537D10;
            constexpr uintptr_t kPlayerQueryGetInt = 0x004DEA10;
            constexpr uintptr_t kPackageDeletedFieldCall = 0x00537D7C;
            constexpr uintptr_t kPlayerPackageCalls[] = {
                0x00587D4D, 0x00588131, 0x0058853E, 0x00588951,
                0x00588D64, 0x005891C5, 0x005896CA, 0x00589DD8,
                0x0058AAB2, 0x0058B04B, 0x0058B3C0, 0x0058B7D5,
                0x0058BD04, 0x0058C135, 0x0058C500, 0x0058C89E
            };
            bool valid = true;
            if (GetDirectCallDestination(kPackageDeletedFieldCall) !=
                kPlayerQueryGetInt) {
                valid = false;
            }
            for (unsigned int i = 0;
                 i < sizeof(kPlayerPackageCalls) /
                     sizeof(kPlayerPackageCalls[0]); ++i) {
                if (GetDirectCallDestination(kPlayerPackageCalls[i]) !=
                    kGetPlayerPackage) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                g_originalGetPlayerPackage =
                    reinterpret_cast<GetPlayerPackageFn>(kGetPlayerPackage);
                g_originalPlayerQueryGetInt =
                    reinterpret_cast<PlayerQueryGetIntFn>(kPlayerQueryGetInt);
                g_resolveRuntimePlayer08 =
                    reinterpret_cast<ResolveRuntimePlayerFn>(0x004C13B0);
                g_getRuntimePlayerInt08 =
                    reinterpret_cast<PlayerQueryGetIntFn>(0x004DEA10);
                patch::RedirectCall(
                    static_cast<unsigned int>(kPackageDeletedFieldCall),
                    reinterpret_cast<void*>(&HookFeaturedPlayerQueryGetInt));
                for (unsigned int i = 0;
                     i < sizeof(kPlayerPackageCalls) /
                         sizeof(kPlayerPackageCalls[0]); ++i) {
                    patch::RedirectCall(
                        static_cast<unsigned int>(kPlayerPackageCalls[i]),
                        reinterpret_cast<void*>(&HookGetPlayerPackage));
                }

                constexpr uintptr_t kSubstitutionBuilderCall = 0x005A0E1D;
                constexpr uintptr_t kSubstitutionBuilder = 0x0058BDD0;
                if (GetDirectCallDestination(kSubstitutionBuilderCall) ==
                    kSubstitutionBuilder) {
                    g_originalSubstitutionBuilder08 =
                        reinterpret_cast<SubstitutionBuilderFn>(
                            kSubstitutionBuilder);
                    patch::RedirectCall(
                        static_cast<unsigned int>(kSubstitutionBuilderCall),
                        reinterpret_cast<void*>(&HookSubstitutionBuilder08));
                    AppendDiagnostic(
                        "NBA Live 08 live-lineup tracker installed: "
                        "substitutionBuilderCall=%08X target=%08X.\n",
                        static_cast<unsigned int>(kSubstitutionBuilderCall),
                        static_cast<unsigned int>(kSubstitutionBuilder));
                }
                else {
                    AppendDiagnostic(
                        "NBA Live 08 live-lineup tracker skipped: "
                        "substitution builder call validation failed.\n");
                }

                AppendDiagnostic(
                    "NBA Live 08 featured-player identity/live-stat "
                    "diagnostics installed: 16 player-stat builders, "
                    "packageResolver=%08X runtimeResolver=%08X "
                    "intGetter=%08X fields=85/87/88/89 "
                    "stats=06/07/09/0C/0F/10.\n",
                    static_cast<unsigned int>(kGetPlayerPackage),
                    0x004C13B0u, 0x004DEA10u);
            }
            else {
                AppendDiagnostic(
                    "NBA Live 08 featured-player identity diagnostics "
                    "skipped: player-package call validation failed; game "
                    "flow left unchanged.\n");
            }
        }
        const bool playerFoulLayoutLoaded = g_customOverlayEnabled &&
            scoreboardconfig::LoadPlayerFoul(g_customOverlayName);
        g_playerFoulLayoutAvailable = false;

        if (g_customOverlayEnabled && game.statsRequestCall) {
            const uintptr_t requestDestination = GetDirectCallDestination(
                game.statsRequestCall);
            if (requestDestination) {
                g_originalStatsRequest =
                    reinterpret_cast<StatsRequestFn>(requestDestination);
                patch::RedirectCall(
                    static_cast<unsigned int>(game.statsRequestCall),
                    reinterpret_cast<void*>(&HookStatsNativeRequest));
                g_statsRequestHookInstalled = true;
                g_playerFoulLayoutAvailable = playerFoulLayoutLoaded;
                AppendDiagnostic(
                    "Player-foul native request hook installed: call=%08X "
                    "target=%08X.\n",
                    static_cast<unsigned int>(game.statsRequestCall),
                    static_cast<unsigned int>(requestDestination));
            }
            else {
                g_playerFoulLayoutAvailable = false;
                AppendDiagnostic(
                    "Player-foul fallback to native: invalid request call "
                    "%08X.\n",
                    static_cast<unsigned int>(game.statsRequestCall));
            }
        }
        const unsigned int statsCalls = RedirectDirectCalls(
            game.statsDataStore,
            reinterpret_cast<void*>(&HookStatsDataStore));
        AppendDiagnostic(
            "Redirected FEOverlayStats completed-store calls: %u "
            "target=%08X.\n",
            statsCalls, static_cast<unsigned int>(game.statsDataStore));
        AppendDiagnostic("Player-foul custom layout: %s.\n",
            g_playerFoulLayoutAvailable ? "enabled" :
                "unavailable; native overlay retained");
    }

    // Every supported FEOverlayViolation builder first submits the native
    // overlays~viol.big request, then stores its completed three-value
    // BBallString vector. Replace the request with a stack-compatible success
    // stub and capture the vector at the following store call.
    if (g_customOverlayEnabled && game.violationRequestCall &&
        game.violationDataStoreCall) {
        const uintptr_t requestDestination = GetDirectCallDestination(
            game.violationRequestCall);
        const uintptr_t storeDestination = GetDirectCallDestination(
            game.violationDataStoreCall);
        if (!requestDestination || !storeDestination) {
            AppendDiagnostic(
                "Native violation suppression skipped: invalid calls "
                "request=%08X store=%08X.\n",
                static_cast<unsigned int>(game.violationRequestCall),
                static_cast<unsigned int>(game.violationDataStoreCall));
        }
        else {
            patch::RedirectCall(
                static_cast<unsigned int>(game.violationRequestCall),
                reinterpret_cast<void*>(&SuppressNativeViolationRequest));

            g_originalViolationDataStore =
                reinterpret_cast<StoreOverlayDataFn>(storeDestination);
            patch::RedirectCall(
                static_cast<unsigned int>(game.violationDataStoreCall),
                reinterpret_cast<void*>(&HookViolationDataStore));
            AppendDiagnostic(
                "Native violation movie request disabled: "
                "request=%08X store=%08X target=%08X.\n",
                static_cast<unsigned int>(game.violationRequestCall),
                static_cast<unsigned int>(game.violationDataStoreCall),
                static_cast<unsigned int>(storeDestination));
        }
    }

    if (g_customOverlayEnabled)
        InitializeNativeScoreboard();
}

} // namespace

void InitializeShotClock()
{
    {
        switch (FM::GetEntryPoint()) {
        case 0xCD8005: // NBA Live 2005
            Initialize(NBA_LIVE_2005);
            break;
        case 0x40109F:
            if (patch::GetFloat(0xBD832C) == 1.3333334f) { // NBA Live 06
                Initialize(NBA_LIVE_2006);
            }
            else if (patch::GetFloat(0xBBBC3C) == 1.3333334f) { // NBA Live 07
                Initialize(NBA_LIVE_2007);
            }
            else if (patch::GetFloat(0xC3DF84) == 1.3333334f) {
                Initialize(NBA_LIVE_2008);
            }
            break;
        default:
            break;
        }
    }
}

void InitializeDebugLogging()
{
    InitializeDebugLoggingInternal();
}