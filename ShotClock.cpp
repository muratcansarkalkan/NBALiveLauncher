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

#pragma comment(lib, "d3d9.lib")

using namespace plugin;

namespace {

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

const GameAddresses* g_game = nullptr;
SendEventFn g_originalSendEvent = nullptr;
CRITICAL_SECTION g_logLock;
bool g_logReady = false;
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

struct PlayerFoulState {
    char firstName[64];
    char lastName[64];
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
StoreOverlayDataFn g_originalStatsDataStore = nullptr;
StatsRequestFn g_originalStatsRequest = nullptr;

bool __stdcall SuppressNativeViolationRequest(void*)
{
    // Matches sub_55B020's one stack argument and returns AL=1 so
    // sub_597270 continues into its normal payload-storage branch.
    return true;
}

bool __fastcall HookStatsNativeRequest(void* thisPtr, void*, void* request)
{
    if (g_suppressCurrentStatsRequest)
        return true;
    return g_originalStatsRequest ? g_originalStatsRequest(thisPtr, request) : false;
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
        "popups\\%s\\scoreboard\\scoreboard.json", g_customOverlayName);
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
        "popups\\%s\\.reload", g_customOverlayName);
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
    if (!g_logReady) return;
    EnterCriticalSection(&g_logLock);
    FILE* file = std::fopen("extended_score_state.log", "a");
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
    if (!g_logReady) return;
    EnterCriticalSection(&g_logLock);
    FILE* file = std::fopen("score_events.log", "a");
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
        if (g_violationPresentationSuppressed)
            g_violationTransitionHiddenAt = g_violation.startedAt;
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

void LogCompletedStatPayload(DWORD* payload)
{
    if (!g_logReady || !payload)
        return;

    __try {
        const int count = static_cast<int>(payload[3]);
        if (count < 0 || count > 128)
            return;

        const BBallString* values = reinterpret_cast<const BBallString*>(
            payload[0]);
        if (count > 0 && !values)
            return;

        FILE* file = nullptr;
        EnterCriticalSection(&g_logLock);
        __try {
            file = std::fopen("stat_payloads.log", "a");
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
        }
        __finally {
            if (file) std::fclose(file);
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
    if (g_customOverlayEnabled && g_playerFoulLayoutAvailable && payload) {
        __try {
            const int count = static_cast<int>(payload[3]);
            const BBallString* values = reinterpret_cast<const BBallString*>(
                payload[0]);
            playerFoul = count >= 15 && values &&
                payload[6] == 1 && payload[7] == 10;
            if (playerFoul) {
                const unsigned int hash = HashOverlayPayload(0, values, count);
                CopyText(g_playerFoul.firstName,
                    sizeof(g_playerFoul.firstName), values[0].sharedstring);
                CopyText(g_playerFoul.lastName,
                    sizeof(g_playerFoul.lastName), values[1].sharedstring);
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
                if (g_playerFoulPresentationSuppressed)
                    g_playerFoulTransitionHiddenAt = g_playerFoul.startedAt;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            playerFoul = false;
            g_playerFoul.active = false;
        }
    }

    // sub_584890 still performs all native payload storage/bookkeeping. Its
    // internal movie request is the only operation conditionally suppressed.
    g_suppressCurrentStatsRequest = playerFoul;
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
    if (!g_logReady || type < 0 || type > MAX_OVERLAY_TYPE || !vector)
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

        FILE* file = nullptr;
        EnterCriticalSection(&g_logLock);
        __try {
            file = std::fopen("overlay_payloads.log", "a");
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
            g_violationPresentationSuppressed = true;
            g_violation.active = false;
            g_violation.paused = false;
            g_violation.pausedAt = 0;
            g_violationTransitionHiddenAt = 0;
            g_playerFoulPresentationSuppressed = true;
            g_playerFoul.active = false;
            g_playerFoulTransitionHiddenAt = 0;
        }
        else if (std::strcmp(name, "HideOverlaysEvent") == 0) {
            if (!g_violationPresentationSuppressed && g_violation.active)
                g_violationTransitionHiddenAt = GetTickCount();
            g_violationPresentationSuppressed = true;
            if (!g_playerFoulPresentationSuppressed && g_playerFoul.active)
                g_playerFoulTransitionHiddenAt = GetTickCount();
            g_playerFoulPresentationSuppressed = true;
        }
        else if (std::strcmp(name, "ShowOverlaysEvent") == 0) {
            if (g_violationPresentationSuppressed && g_violation.active &&
                g_violationTransitionHiddenAt) {
                g_violation.startedAt +=
                    GetTickCount() - g_violationTransitionHiddenAt;
            }
            g_violationPresentationSuppressed = false;
            g_violationTransitionHiddenAt = 0;
            if (g_playerFoulPresentationSuppressed && g_playerFoul.active &&
                g_playerFoulTransitionHiddenAt)
                g_playerFoul.startedAt +=
                    GetTickCount() - g_playerFoulTransitionHiddenAt;
            g_playerFoulPresentationSuppressed = false;
            g_playerFoulTransitionHiddenAt = 0;
        }
        else if (resumeEvent) {
            // A pause killed the previous instance; resume merely allows the
            // next native violation request to create a new custom one.
            g_violationPresentationSuppressed = false;
            g_violationTransitionHiddenAt = 0;
            g_playerFoulPresentationSuppressed = false;
            g_playerFoulTransitionHiddenAt = 0;
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

using OverlayRenderFn = void (*)(IDirect3DDevice9*);

void RenderConfiguredOverlays(IDirect3DDevice9* device)
{
    scoreboardconfig::Load(g_customOverlayName);
    scoreboardconfig::LoadViolation(g_customOverlayName);
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
        { scoreboardconfig::GetPlayerFoul().overlayZ, 2,
            &RenderPlayerFoulOverlay }
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
        const bool playerFoulLoaded = scoreboardconfig::ReloadPlayerFoul(
            g_customOverlayName);
        g_playerFoulLayoutAvailable = playerFoulLoaded &&
            g_statsRequestHookInstalled;
        if (!playerFoulLoaded) g_playerFoul.active = false;
        g_loggedAwayLogoTeam = INT_MIN;
        g_loggedHomeLogoTeam = INT_MIN;
        g_loggedStatTeamCode[0] = '\0';
        AppendDiagnostic(
            "Popup hot reload: teams=%s font=%s scoreboard=%s violation=%s playerFoul=%s error=%s\n",
            themeLoaded ? "OK" : "FAILED",
            fontLoaded ? "OK" : "FAILED",
            scoreboardLoaded ? "OK" : "FAILED",
            violationLoaded ? "OK" : "FAILED",
            playerFoulLoaded ? "OK" : "FAILED",
            themeLoaded && scoreboardLoaded && violationLoaded &&
                playerFoulLoaded ? "<none>" :
                (!themeLoaded ? popup::GetLastError() :
                    scoreboardconfig::GetLastError()));
    }
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

    InitializeCriticalSection(&g_logLock);
    g_logReady = true;

    FILE* file = std::fopen("score_events.log", "w");
    if (file) {
        std::fprintf(file, "%s score.big event log\nSendEvent=%08X\n\n",
            game.name, static_cast<unsigned int>(game.sendEvent));
        std::fclose(file);
    }

    file = std::fopen("extended_score_state.log", "w");
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

    const unsigned int sendEventCalls = RedirectDirectCalls(
        game.sendEvent, reinterpret_cast<void*>(&HookSendEvent));
    AppendDiagnostic("Redirected SendEvent calls: %u\n", sendEventCalls);

    if (game.getOverlayData) {
        file = std::fopen("overlay_payloads.log", "w");
        if (file) {
            std::fprintf(file,
                "%s overlay payload log\nGetOverlayData=%08X\n\n",
                game.name,
                static_cast<unsigned int>(game.getOverlayData));
            std::fclose(file);
        }

        const unsigned int overlayCalls = RedirectDirectCalls(
            game.getOverlayData,
            reinterpret_cast<void*>(&HookGetOverlayData));
        AppendDiagnostic("Redirected GetOverlayData calls: %u\n",
            overlayCalls);
    }

    // Live 06 FEOverlayStats builders all converge here after completing
    // their owned BBallString vectors. The player-foul subtype (1/10) gets a
    // custom instance only when its layout exists; all other stat requests
    // continue to the native overlay unchanged.
    if (game.statsDataStore) {
        file = std::fopen("stat_payloads.log", "w");
        if (file) {
            std::fprintf(file,
                "%s completed FEOverlayStats payload log\n"
                "Store=%08X\n\n",
                game.name,
                static_cast<unsigned int>(game.statsDataStore));
            std::fclose(file);
        }

        g_originalStatsDataStore =
            reinterpret_cast<StoreOverlayDataFn>(game.statsDataStore);
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
