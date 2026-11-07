#include "plugin-std.h"
#include "GameSetup.h"
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
#include "GameStageInternal.h"
#include "Games.h"
#include <limits>
#include "PracticeSetupInternal.h"
#include <climits>
#include "ControllerSetupInternal.h"

using namespace plugin;

namespace custompaths {
bool gForcedStageEnabled = false;

PostseasonStage gForcedStage = STAGE_REGULAR;

bool gForceGlobalGameMode = false;

bool gForcedSeriesEnabled = false;

int gForcedSeriesGame = 0;

int gForcedSeriesLength = 7;

int gForcedHomeWins = 0;

int gForcedAwayWins = 0;

ForcedNativeGameMode gForcedNativeGameMode = NATIVE_MODE_OFF;

bool gPlayoffStateDebug = false;

bool gLoggedMissingGlobalOptions = false;

bool gLoggedMissingNativeModeOptions = false;

bool gLoggedUnsupportedNativeMode = false;

bool gRetryLive06PlayoffSeries = false;

bool gLoggedForcedScheduleGetter = false;

bool gLoggedForcedScheduleSelection = false;

// Live 2005 native playoff schedule anchor. Native direct calls to
// sub_542980 are redirected through Live05NativeScheduleQueryHook below.
// Selector 6 is scheduleplayoff.dbf; the query ID itself is the native
// interleaved playoff schedule ID. The strongest active-game path is the
// call at 0x00554B4A, where the engine extracts ID as ([game+4]>>14)&0x7FF.
bool gLive05PlayoffQueryValid = false;
short gLive05PlayoffQueryPeriod = 2;
short gLive05PlayoffQueryRound = -1;
short gLive05PlayoffQuerySlot = -1;
short gLive05PlayoffQuerySeriesGame = -1;
short gLive05PlayoffQueryScheduleId = -1;
uintptr_t gLive05PlayoffQueryCaller = 0;
int gLive05PlayoffQueryConfidence = 0;

void* gLastRejectedOptions = nullptr;

int gLastRejectedMode = 0;

int gLastRejectedRound = 0;

void* GetCurrentOptions(void* suppliedOptions)
    {
        if (suppliedOptions)
            return suppliedOptions;

        __try
        {
            if (gGameVersion == GAME_LIVE_2005)
            {
                unsigned char* audio = *reinterpret_cast<unsigned char**>(0x00C44A30);
                return audio ? *reinterpret_cast<void**>(audio + 0x28) : nullptr;
            }
            if (gGameVersion == GAME_LIVE_07)
            {
                void* game = *reinterpret_cast<void**>(0x00CEBBB0);
                return game
                    ? *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(game) + 0x0AC0)
                    : nullptr;
            }

            if (gGameVersion == GAME_LIVE_08)
            {
                void* game = *reinterpret_cast<void**>(0x00DA7A20);
                return game
                    ? *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(game) + 0x0B70)
                    : nullptr;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }

        return nullptr;
    }

const char* StageName(PostseasonStage stage)
    {
        switch (stage)
        {
        case STAGE_PLAYOFF: return "playoff";
        case STAGE_ECR1:    return "ECR1";
        case STAGE_WCR1:    return "WCR1";
        case STAGE_ECSF:    return "ECSF";
        case STAGE_WCSF:    return "WCSF";
        case STAGE_ECF:     return "ECF";
        case STAGE_WCF:     return "WCF";
        case STAGE_FINALS:  return "finals";
        default:            return "regular";
        }
    }

const char* ForcedNativeGameModeName(ForcedNativeGameMode mode)
    {
        switch (mode)
        {
        case NATIVE_MODE_ALLSTAR: return "ALLSTAR";
        case NATIVE_MODE_ROOKIE:  return "ROOKIE";
        default:                  return "OFF";
        }
    }

int StageRoundValue(PostseasonStage stage)
    {
        switch (stage)
        {
        case STAGE_ECR1:   return 0;
        case STAGE_WCR1:   return 1;
        case STAGE_ECSF:   return 2;
        case STAGE_WCSF:   return 3;
        case STAGE_ECF:    return 4;
        case STAGE_WCF:    return 5;
        case STAGE_FINALS: return 6;
        default:           return 0;
        }
    }

const char* GameVersionName()
    {
        switch (gGameVersion)
        {
        case GAME_LIVE_2005: return "2005";
        case GAME_LIVE_06:   return "06";
        case GAME_LIVE_07:   return "07";
        case GAME_LIVE_08:   return "08";
        default:             return "unknown";
        }
    }

short ReadScheduleShort(
        const Live06ScheduleGame& game,
        size_t offset)
    {
        short value = 0;
        std::memcpy(&value, game.bytes + offset, sizeof(value));
        return value;
    }

bool ReadLive06ScheduleGame(
        unsigned int gameId,
        Live06ScheduleGame& game)
    {
        if (gGameVersion != GAME_LIVE_06 ||
            gameId == 0 ||
            gameId > 0x7FFF)
        {
            return false;
        }

        // Native helper used by ScheduleInfo.GetGameInfoArray. It queries the
        // already-open schedule table by ID and returns the game's normalized
        // 44-byte record, so no DBF path discovery or extra disk I/O is needed.
        typedef void* (__stdcall* ReadScheduleGameFn)(
            void* output,
            short period,
            int id);
        ReadScheduleGameFn readScheduleGame =
            reinterpret_cast<ReadScheduleGameFn>(0x00552F20);

        std::memset(&game, 0, sizeof(game));
        __try
        {
            // PERIOD=2 is the playoff schedule, as confirmed by
            // scheduleplayoff.dbf.
            readScheduleGame(
                &game,
                2,
                static_cast<int>(gameId));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            char message[160] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_QUERY id=%u period=2 "
                "exception=yes.\n",
                gameId);
            WritePlayoffStateLog(message);
            return false;
        }

        const short returnedId = ReadScheduleShort(game, 0);
        if (returnedId == static_cast<short>(gameId))
            return true;

        const unsigned int* raw =
            reinterpret_cast<const unsigned int*>(game.bytes);
        char message[320] = {};
        sprintf_s(
            message,
            sizeof(message),
            "[CustomPaths] PLAYOFF_QUERY id=%u period=2 returnedId=%d "
            "played=%u home=%d away=%d score=%d-%d "
            "raw=%08X,%08X,%08X,%08X,%08X,%08X\n",
            gameId,
            returnedId,
            game.bytes[18],
            ReadScheduleShort(game, 20),
            ReadScheduleShort(game, 26),
            ReadScheduleShort(game, 22),
            ReadScheduleShort(game, 28),
            raw[0],
            raw[1],
            raw[2],
            raw[3],
            raw[4],
            raw[5]);
        WritePlayoffStateLog(message);
        return false;
    }


short ReadLive05ScheduleShort(
        const Live08ScheduleGame& game,
        size_t offset)
    {
        short value = 0;
        if (offset + sizeof(value) <= sizeof(game.bytes))
            std::memcpy(&value, game.bytes + offset, sizeof(value));
        return value;
    }

bool ReadLive05ScheduleGame(
        unsigned int gameId,
        Live08ScheduleGame& game)
    {
        if (gGameVersion != GAME_LIVE_2005 ||
            gameId == 0 || gameId > 0x7FFF)
        {
            return false;
        }

        // NBA Live 2005 native schedule path established by the deep IDA probe:
        //   sub_4F8BE0 = local schedule-helper constructor
        //   sub_542980 = query-by-ID wrapper (" ID = %hd ")
        //   sub_520510 = DBF row -> normalized schedule-record decoder
        //   sub_4D72C0 = local schedule-helper cleanup
        //
        // sub_4F8BE0 initializes eight schedule-table selectors. The 2005
        // descriptor mapping proves selector 6 routes to scheduleplayoff.dbf
        // (descriptor ID 15); selector 5 routes to scheduleseason.dbf.
        typedef void (__thiscall* ConstructScheduleHelperFn)(void* self);
        typedef void* (__thiscall* ReadScheduleGameFn)(
            void* self,
            void* output,
            int gameIdValue,
            int selector);
        typedef void (__thiscall* DestroyScheduleHelperFn)(void* self);

        ConstructScheduleHelperFn constructHelper =
            reinterpret_cast<ConstructScheduleHelperFn>(0x004F8BE0);
        ReadScheduleGameFn readScheduleGame =
            reinterpret_cast<ReadScheduleGameFn>(0x00542980);
        DestroyScheduleHelperFn destroyHelper =
            reinterpret_cast<DestroyScheduleHelperFn>(0x004D72C0);

        // Native callers place this helper in a local object beginning at
        // var_48. 0x48 bytes safely covers the observed local helper storage.
        alignas(4) unsigned char helper[0x48] = {};
        std::memset(&game, 0, sizeof(game));

        bool constructed = false;
        __try
        {
            constructHelper(helper);
            constructed = true;

            readScheduleGame(
                helper,
                &game,
                static_cast<int>(gameId),
                6);

            destroyHelper(helper);
            constructed = false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (constructed)
            {
                __try { destroyHelper(helper); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }

            char message[176] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_05_QUERY id=%u exception=yes.\n",
                gameId);
            WritePlayoffStateLog(message);
            return false;
        }

        // sub_520510 normalized output:
        // +00 TYPE, +02 SPECIAL, +04 ID, +0C PERIOD,
        // +2C PLAYED, +2D OVERTIME,
        // +2E TEAMHOME, +30 SCOREHOME,
        // +36 TEAMAWAY, +38 SCOREAWAY.
        const short returnedId = ReadLive05ScheduleShort(game, 0x04);
        const short homeTeam = ReadLive05ScheduleShort(game, 0x2E);
        const short awayTeam = ReadLive05ScheduleShort(game, 0x36);

        if (returnedId != static_cast<short>(gameId) ||
            homeTeam < 0 || awayTeam < 0 || homeTeam == awayTeam)
        {
            char message[224] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_05_QUERY id=%u returned=%d "
                "home=%d away=%d valid=no.\n",
                gameId,
                static_cast<int>(returnedId),
                static_cast<int>(homeTeam),
                static_cast<int>(awayTeam));
            WritePlayoffStateLog(message);
            return false;
        }

        return true;
    }

short ReadLive07ScheduleShort(
        const Live08ScheduleGame& game,
        size_t offset)
    {
        short value = 0;
        if (offset + sizeof(value) <= sizeof(game.bytes))
            std::memcpy(&value, game.bytes + offset, sizeof(value));
        return value;
    }

bool ReadLive07ScheduleGame(
        unsigned int gameId,
        Live08ScheduleGame& game)
    {
        if (gGameVersion != GAME_LIVE_07 ||
            gameId == 0 || gameId > 0x7FFF)
        {
            return false;
        }

        // NBA Live 07 equivalents from the IDA dump:
        //   sub_5CBFD0 = local schedule helper constructor
        //   sub_5F7DF0 = query-by-ID wrapper (" ID = %hd ")
        //   sub_5E3CC0 = normalized schedule record decoder
        //   sub_5CC080 = local helper destructor
        typedef void* (__thiscall* ConstructScheduleHelperFn)(void* self);
        typedef void* (__thiscall* ReadScheduleGameFn)(
            void* self,
            void* output,
            int gameIdValue,
            int selector);
        typedef void (__thiscall* DestroyScheduleHelperFn)(void* self);

        ConstructScheduleHelperFn constructHelper =
            reinterpret_cast<ConstructScheduleHelperFn>(0x005CBFD0);
        ReadScheduleGameFn readScheduleGame =
            reinterpret_cast<ReadScheduleGameFn>(0x005F7DF0);
        DestroyScheduleHelperFn destroyHelper =
            reinterpret_cast<DestroyScheduleHelperFn>(0x005CC080);

        alignas(4) unsigned char helper[0x28] = {};
        std::memset(&game, 0, sizeof(game));

        bool constructed = false;
        __try
        {
            constructHelper(helper);
            constructed = true;

            // Selector 2 is the postseason schedule path, matching the
            // corresponding Live 08 native reader.
            readScheduleGame(
                helper,
                &game,
                static_cast<int>(gameId),
                2);

            destroyHelper(helper);
            constructed = false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (constructed)
            {
                __try { destroyHelper(helper); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }

            char message[176] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_07_QUERY id=%u exception=yes.\n",
                gameId);
            WritePlayoffStateLog(message);
            return false;
        }

        // sub_5E3CC0 normalized output:
        // +00 TYPE, +02 SPECIAL, +04 ID,
        // +2C PLAYED, +2D OVERTIME,
        // +2E TEAMHOME, +30 SCOREHOME,
        // +36 TEAMAWAY, +38 SCOREAWAY.
        const short returnedId = ReadLive07ScheduleShort(game, 0x04);
        const short homeTeam = ReadLive07ScheduleShort(game, 0x2E);
        const short awayTeam = ReadLive07ScheduleShort(game, 0x36);

        if (returnedId != static_cast<short>(gameId) ||
            homeTeam < 0 || awayTeam < 0 || homeTeam == awayTeam)
        {
            char message[224] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_07_QUERY id=%u returned=%d "
                "home=%d away=%d valid=no.\n",
                gameId,
                static_cast<int>(returnedId),
                static_cast<int>(homeTeam),
                static_cast<int>(awayTeam));
            WritePlayoffStateLog(message);
            return false;
        }

        return true;
    }

short ReadLive08ScheduleShort(
        const Live08ScheduleGame& game,
        size_t offset)
    {
        short value = 0;
        if (offset + sizeof(value) <= sizeof(game.bytes))
            std::memcpy(&value, game.bytes + offset, sizeof(value));
        return value;
    }

bool ReadLive08ScheduleGame(
        unsigned int gameId,
        Live08ScheduleGame& game)
    {
        if (gGameVersion != GAME_LIVE_08 ||
            gameId == 0 || gameId > 0x7FFF)
        {
            return false;
        }

        typedef void* (__thiscall* ConstructScheduleHelperFn)(void* self);
        typedef void* (__thiscall* ReadScheduleGameFn)(
            void* self,
            void* output,
            int gameIdValue,
            int selector);
        typedef void (__thiscall* DestroyScheduleHelperFn)(void* self);

        ConstructScheduleHelperFn constructHelper =
            reinterpret_cast<ConstructScheduleHelperFn>(0x005E5BA0);
        ReadScheduleGameFn readScheduleGame =
            reinterpret_cast<ReadScheduleGameFn>(0x00614910);
        DestroyScheduleHelperFn destroyHelper =
            reinterpret_cast<DestroyScheduleHelperFn>(0x005E5C50);

        alignas(4) unsigned char helper[0x28] = {};
        std::memset(&game, 0, sizeof(game));

        bool constructed = false;
        __try
        {
            constructHelper(helper);
            constructed = true;

            // Native selector 2 is the playoff schedule table path used by
            // sub_614F70 when the active schedule context is postseason.
            readScheduleGame(
                helper,
                &game,
                static_cast<int>(gameId),
                2);

            destroyHelper(helper);
            constructed = false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (constructed)
            {
                __try { destroyHelper(helper); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }

            char message[176] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_08_QUERY id=%u exception=yes.\n",
                gameId);
            WritePlayoffStateLog(message);
            return false;
        }

        // sub_5FF380 normalized output layout:
        // +00 TYPE, +02 SPECIAL, +04 ID, +2C PLAYED,
        // +2E TEAMHOME, +30 SCOREHOME,
        // +36 TEAMAWAY, +38 SCOREAWAY.
        const short returnedId = ReadLive08ScheduleShort(game, 0x04);
        const short homeTeam = ReadLive08ScheduleShort(game, 0x2E);
        const short awayTeam = ReadLive08ScheduleShort(game, 0x36);

        if (returnedId != static_cast<short>(gameId) ||
            homeTeam < 0 || awayTeam < 0 || homeTeam == awayTeam)
        {
            char message[224] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] PLAYOFF_08_QUERY id=%u returned=%d "
                "home=%d away=%d valid=no.\n",
                gameId,
                static_cast<int>(returnedId),
                static_cast<int>(homeTeam),
                static_cast<int>(awayTeam));
            WritePlayoffStateLog(message);
            return false;
        }

        return true;
    }

bool IsReadableMemoryRange(const void* address, size_t size)
    {
        if (!address || size == 0)
            return false;

        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        const uintptr_t end = start + size;
        if (end < start)
            return false;

        uintptr_t cursor = start;
        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION information = {};
            if (!VirtualQuery(
                    reinterpret_cast<const void*>(cursor),
                    &information,
                    sizeof(information)))
            {
                return false;
            }

            if (information.State != MEM_COMMIT ||
                (information.Protect & PAGE_GUARD) != 0 ||
                (information.Protect & PAGE_NOACCESS) != 0)
            {
                return false;
            }

            const uintptr_t regionStart =
                reinterpret_cast<uintptr_t>(information.BaseAddress);
            const uintptr_t regionEnd = regionStart + information.RegionSize;
            if (regionEnd <= cursor)
                return false;
            cursor = regionEnd;
        }

        return true;
    }

bool IsLive06PlayoffIntegerSetting(unsigned int id)
    {
        return id == 0x0B || id == 0x0C ||
            id == 0x23 || id == 0x24 ||
            id == 0x3B || id == 0x3C || id == 0x3D;
    }

bool IsLive06PlayoffBooleanSetting(unsigned int id)
    {
        return id == 0x2A || id == 0x2B;
    }

int __fastcall Live06IntegerSettingGetterHook(
        void* settings,
        void*,
        unsigned int id,
        unsigned int source)
    {
        const uintptr_t caller =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        int value = 0;

        switch (source)
        {
        case 1:
            value = reinterpret_cast<int (__stdcall*)(unsigned int)>(
                0x004C4460)(id);
            break;
        case 2:
            value = reinterpret_cast<int (__stdcall*)(unsigned int)>(
                0x004C4610)(id);
            break;
        case 3:
            value = reinterpret_cast<int (__stdcall*)(unsigned int)>(
                0x004C46C0)(id);
            break;
        default:
            __try
            {
                value = *reinterpret_cast<const int*>(
                    reinterpret_cast<const unsigned char*>(settings) +
                    0xBC + id * sizeof(int));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                value = 0;
            }
            break;
        }

        if (gForcedSeriesEnabled &&
            gForcedStageEnabled &&
            gForceGlobalGameMode)
        {
            // These are the presentation-facing SettingsObject values.  In
            // an exhibition promoted to Playoff mode there is no live
            // schedule/bracket object, so callers otherwise keep receiving
            // the default "round one, game one" state.
            switch (id)
            {
            case 0x0B: value = gForcedHomeWins; break; // away losses
            case 0x0C: value = gForcedAwayWins; break; // away wins
            case 0x23: value = gForcedAwayWins; break; // home losses
            case 0x24: value = gForcedHomeWins; break; // home wins
            case 0x3B: value = (gForcedSeriesLength - 1) / 2; break;
            case 0x3C: value = StageRoundValue(gForcedStage); break;
            default: break;
            }
        }

        if (IsLive06PlayoffIntegerSetting(id))
        {
            LogLive06PlayoffAccessor(
                settings,
                caller,
                id,
                source,
                value,
                false);
        }
        return value;
    }

bool __fastcall Live06BooleanSettingGetterHook(
        void* settings,
        void*,
        unsigned int id,
        unsigned int source)
    {
        const uintptr_t caller =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        bool value = false;

        switch (source)
        {
        case 1:
            value = reinterpret_cast<bool (__stdcall*)(unsigned int)>(
                0x004C45A0)(id);
            break;
        case 2:
            value = false;
            break;
        case 3:
            value = true;
            break;
        default:
            __try
            {
                value = *reinterpret_cast<const unsigned char*>(
                    reinterpret_cast<const unsigned char*>(settings) +
                    0x7E + id) != 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                value = false;
            }
            break;
        }

        if (gForcedSeriesEnabled &&
            gForcedStageEnabled &&
            gForceGlobalGameMode)
        {
            if (id == 0x2A)
                value = gForcedAwayWins == (gForcedSeriesLength - 1) / 2;
            else if (id == 0x2B)
                value = gForcedHomeWins == (gForcedSeriesLength - 1) / 2;
        }

        if (IsLive06PlayoffBooleanSetting(id))
        {
            LogLive06PlayoffAccessor(
                settings,
                caller,
                id,
                source,
                value ? 1 : 0,
                true);
        }
        return value;
    }

int __stdcall Live06CommentaryParameterGetterHook(unsigned int index)
    {
        // Exact replacement for native 0x00449550. The original lazily fills
        // the 128-entry commentary parameter table with -1 before reading.
        unsigned char* initialized =
            reinterpret_cast<unsigned char*>(0x00CAF5B4);
        int* values = reinterpret_cast<int*>(0x00CAFA18);
        if (*initialized == 0)
        {
            for (unsigned int i = 0; i < 128; ++i)
                values[i] = -1;
            *initialized = 1;
        }

        const int value = index < 128 ? values[index] : -1;
        LogLive06CommentaryParameter(
            reinterpret_cast<uintptr_t>(_ReturnAddress()),
            index,
            value,
            false);
        return value;
    }

void __stdcall Live06CommentaryParameterSetterHook(
        unsigned int index,
        int value)
    {
        // Exact replacement for native 0x00449590.
        unsigned char* initialized =
            reinterpret_cast<unsigned char*>(0x00CAF5B4);
        int* values = reinterpret_cast<int*>(0x00CAFA18);
        if (*initialized == 0)
        {
            for (unsigned int i = 0; i < 128; ++i)
                values[i] = -1;
            *initialized = 1;
        }

        if (index < 128)
            values[index] = value;
        LogLive06CommentaryParameter(
            reinterpret_cast<uintptr_t>(_ReturnAddress()),
            index,
            value,
            true);
    }

#if defined(_M_IX86)
uintptr_t gLive06PregameContinuation = 0;

uintptr_t gLive06PlayoffCacheInitializer = 0x0043C410;

void ApplyForcedSeriesRulesForBuild(void* options)
    {
        if (!options || !gForcedSeriesEnabled || !gForcedStageEnabled ||
            !gForceGlobalGameMode)
            return;
        __try
        {
            unsigned char* rules = static_cast<unsigned char*>(options);
            const bool live08 = gGameVersion == GAME_LIVE_08;
            const size_t modeOffset = live08 ? 0x64 : 0x5C;
            const size_t roundOffset = live08 ? 0x168 : 0x158;
            const size_t lengthOffset = live08 ? 0x170 : 0x160;
            const size_t awayOffset = live08 ? 0x11D : 0x10D;
            const size_t homeOffset = live08 ? 0x11E : 0x10E;
            if (!IsReadableMemoryRange(rules, lengthOffset + sizeof(int)))
                return;
            int& mode = *reinterpret_cast<int*>(rules + modeOffset);
            if (mode < 0 || mode > 24)
                return;
            mode = mode == 4 ? 4 : 2;
            *reinterpret_cast<int*>(rules + roundOffset) = StageRoundValue(gForcedStage);
            *reinterpret_cast<int*>(rules + lengthOffset) = (gForcedSeriesLength - 1) / 2;
            rules[awayOffset] = gForcedAwayWins == (gForcedSeriesLength - 1) / 2 ? 1 : 0;
            rules[homeOffset] = gForcedHomeWins == (gForcedSeriesLength - 1) / 2 ? 1 : 0;
            if (gGameVersion == GAME_LIVE_2005)
            {
                // Native 2005 rules copies: 892033/89203F and 5DCF15/5DCF1E.
                *reinterpret_cast<unsigned char*>(0x00CBE934) = rules[homeOffset];
                *reinterpret_cast<unsigned char*>(0x00CBE935) = rules[awayOffset];
                unsigned char* info = *reinterpret_cast<unsigned char**>(0x00C49F04);
                if (IsReadableMemoryRange(info, 0x26))
                {
                    info[0x24] = rules[awayOffset];
                    info[0x25] = rules[homeOffset];
                }
                return;
            }
            if (live08)
            {
                // Native Live 08 rules copy at 0x009E421A..0x009E422C.
                *reinterpret_cast<unsigned char*>(0x00DF9C54) = rules[homeOffset];
                *reinterpret_cast<unsigned char*>(0x00DF9C55) = rules[awayOffset];
                return;
            }
            // Live 07 copies home/away clinch flags to bits 1/2 in the
            // gameplay rules object (native copy at 0x0092A327..0x0092A362).
            unsigned char* gameplayRules =
                *reinterpret_cast<unsigned char**>(0x00D3B188);
            if (IsReadableMemoryRange(gameplayRules, 0x58A))
                gameplayRules[0x589] = static_cast<unsigned char>(
                    (gameplayRules[0x589] & ~6u) |
                    (rules[0x10E] ? 2u : 0u) | (rules[0x10D] ? 4u : 0u));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog("[CustomPaths] Forced rules unreadable.\n");
        }
    }

void __stdcall ApplyLive06ForcedSpeechContext()
    {
        if (!gForcedSeriesEnabled || !gForcedStageEnabled ||
            !gForceGlobalGameMode)
            return;
        int round = 1;
        int slot = 5; // Native default: Eastern first-round bracket slot.
        switch (gForcedStage)
        {
        case STAGE_FINALS: round = 4; slot = 1; break;
        case STAGE_ECR1: round = 1; slot = 5; break;
        case STAGE_WCR1: round = 1; slot = 1; break;
        case STAGE_ECSF: round = 2; slot = 3; break;
        case STAGE_WCSF: round = 2; slot = 1; break;
        case STAGE_ECF: round = 3; slot = 2; break;
        case STAGE_WCF: round = 3; slot = 1; break;
        case STAGE_PLAYOFF: break;
        default: return;
        }
        if (gGameVersion == GAME_LIVE_2005)
        {
            // 2005 builds its speech context from live query results below;
            // it has no 06/07/08 cached-round address to update.
            ApplyForcedSeriesRulesForBuild(GetCurrentOptions(nullptr));
            return;
        }
        const bool live07 = gGameVersion == GAME_LIVE_07;
        const bool live08 = gGameVersion == GAME_LIVE_08;
        int* cachedRound = reinterpret_cast<int*>(live08 ? 0x00D137C4 : (live07 ? 0x00C567F4 : 0x00C3D234));
        int* cachedSlot = reinterpret_cast<int*>(live08 ? 0x00D137C8 : (live07 ? 0x00C567F8 : 0x00C3D238));
        int* cachedLength = reinterpret_cast<int*>(live08 ? 0x00D137CC : (live07 ? 0x00C567FC : 0x00C3D23C));
        int* cachedGame = reinterpret_cast<int*>(live08 ? 0x00D137D0 : (live07 ? 0x00C56800 : 0x00C3D240));
        if (live07 || live08)
        {
            ApplyForcedSeriesRulesForBuild(GetCurrentOptions(nullptr));
            __try
            {
                unsigned char* root = *reinterpret_cast<unsigned char**>(live08 ? 0x00DA3D08 : 0x00CE8378);
                if (IsReadableMemoryRange(root, 0x2C))
                    ApplyForcedSeriesRulesForBuild(*reinterpret_cast<void**>(root + 0x28));
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        const bool changed = *cachedRound != round || *cachedSlot != slot ||
            *cachedLength != gForcedSeriesLength || *cachedGame != gForcedSeriesGame;
        *cachedRound = round;
        *cachedSlot = slot;
        *cachedLength = gForcedSeriesLength;
        *cachedGame = gForcedSeriesGame;
        // The two cached win counts are indexed by bracket participants,
        // rather than a verified home/away ordering. Equal forced counts
        // are independent of that ordering (including the Finals G7 test).
        if (gForcedHomeWins == gForcedAwayWins)
        {
            *reinterpret_cast<int*>(live08 ? 0x00D137D4 : (live07 ? 0x00C56804 : 0x00CAF5C8)) = gForcedHomeWins;
            *reinterpret_cast<int*>(live08 ? 0x00D137D8 : (live07 ? 0x00C56808 : 0x00CAF5CC)) = gForcedAwayWins;
        }
        if (changed && gPlayoffStateDebug)
        {
            char message[256] = {};
            sprintf_s(message, sizeof(message),
                "[CustomPaths] PLAYOFF_SPEECH_CONTEXT game=%s stage=%s round=%d "
                "slot=%d length=%d game=%d winsApplied=%s\n",
                GameVersionName(), StageName(gForcedStage), round, slot, gForcedSeriesLength, gForcedSeriesGame,
                gForcedHomeWins == gForcedAwayWins ? "equal-counts" : "native-order-unverified");
            WritePlayoffStateLog(message);
        }
    }

// Redirected wrapper for native NBA Live 2005 ScheduleInfo::GetGame calls.
// Original ABI: __thiscall(self, output, short gameId, unsigned selector), RET 0Ch.
// __fastcall preserves ECX as self while consuming the same 12 stack bytes.
void* __fastcall Live05NativeScheduleQueryHook(
        void* object, void*, void* output, int gameIdValue, int selector)
    {
        typedef void* (__thiscall* QueryFn)(void*, void*, int, int);
        void* result = reinterpret_cast<QueryFn>(0x00542980)(
            object, output, gameIdValue, selector);

        if (!output || selector != 6 || gameIdValue < 1 || gameIdValue > 15 * 7)
            return result;

        __try
        {
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(output);
            short returnedId = 0, homeTeam = -1, awayTeam = -1;
            std::memcpy(&returnedId, bytes + 0x04, sizeof(returnedId));
            std::memcpy(&homeTeam, bytes + 0x2E, sizeof(homeTeam));
            std::memcpy(&awayTeam, bytes + 0x36, sizeof(awayTeam));

            if (returnedId != static_cast<short>(gameIdValue) ||
                homeTeam < 0 || awayTeam < 0 || homeTeam == awayTeam)
                return result;

            const uintptr_t returnAddress =
                reinterpret_cast<uintptr_t>(_ReturnAddress());

            // Prefer the two call paths proven to extract the schedule ID from
            // a live game object immediately before sub_542980. The 0x554B4A
            // path is the strongest active-game anchor. Other native playoff
            // lookups remain a low-confidence fallback if neither path fires.
            int confidence = 1;
            if (returnAddress == 0x00554B4Fu)
                confidence = 3;
            else if (returnAddress == 0x00543529u)
                confidence = 2;

            if (!gLive05PlayoffQueryValid ||
                confidence > gLive05PlayoffQueryConfidence ||
                (confidence == gLive05PlayoffQueryConfidence &&
                 (confidence >= 2 ||
                  gameIdValue == gLive05PlayoffQueryScheduleId)))
            {
                gLive05PlayoffQueryPeriod = 2;
                gLive05PlayoffQueryScheduleId =
                    static_cast<short>(gameIdValue);
                gLive05PlayoffQuerySlot = static_cast<short>(
                    ((gameIdValue - 1) % 15) + 1);
                gLive05PlayoffQuerySeriesGame = static_cast<short>(
                    ((gameIdValue - 1) / 15) + 1);
                gLive05PlayoffQueryCaller = returnAddress;
                gLive05PlayoffQueryConfidence = confidence;
                gLive05PlayoffQueryValid = true;

                // The options round is the presentation round (0=East R1,
                // 1=West R1, 2=ECSF, 3=WCSF, 4=ECF, 5=WCF, 6=Finals).
                void* options = GetCurrentOptions(nullptr);
                if (options)
                {
                    int round = -1;
                    std::memcpy(&round,
                        reinterpret_cast<unsigned char*>(options) + 0x0158,
                        sizeof(round));
                    gLive05PlayoffQueryRound = static_cast<short>(round);
                }

                if (gPlayoffStateDebug)
                {
                    char message[352] = {};
                    sprintf_s(message, sizeof(message),
                        "[CustomPaths] PLAYOFF_05_NATIVE_QUERY caller=%08X "
                        "selector=%d request=%d returned=%d played=%u "
                        "slot=%d seriesGame=%d round=%d "
                        "home=%d away=%d confidence=%d\n",
                        static_cast<unsigned int>(returnAddress),
                        selector, gameIdValue, static_cast<int>(returnedId),
                        static_cast<unsigned int>(bytes[0x2C]),
                        static_cast<int>(gLive05PlayoffQuerySlot),
                        static_cast<int>(gLive05PlayoffQuerySeriesGame),
                        static_cast<int>(gLive05PlayoffQueryRound),
                        static_cast<int>(homeTeam),
                        static_cast<int>(awayTeam), confidence);
                    WritePlayoffStateLog(message);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        return result;
    }

bool Live05ForcedSpeechEnabled()
    {
        return gGameVersion == GAME_LIVE_2005 && gForcedSeriesEnabled &&
            gForcedStageEnabled && gForceGlobalGameMode;
    }

short* __fastcall Live05SpeechPlayoffQueryHook(
        void* object, void*, short* output)
    {
        typedef short* (__thiscall* QueryFn)(void*, short*);
        short* result = reinterpret_cast<QueryFn>(0x00539C40)(object, output);
        if (output)
        {
            // Preserve the native query as the authoritative 2005 playoff
            // presentation anchor before any optional forced-stage rewrite.
            gLive05PlayoffQueryPeriod = output[0];
            gLive05PlayoffQueryRound = output[1];
            gLive05PlayoffQuerySlot = output[2];
            gLive05PlayoffQuerySeriesGame = output[3];
            gLive05PlayoffQueryScheduleId = output[4];
            gLive05PlayoffQueryValid =
                output[0] == 2 &&
                output[3] >= 1 && output[3] <= 7 &&
                output[4] >= 1 && output[4] <= 15 * 7;
        }
        if (!Live05ForcedSpeechEnabled() || !output)
            return result;
        // The native 10-byte result is period, round, conference-local
        // bracket slot, series game, schedule ID. Preserve period and ID.
        int round = 1;
        int slot = 5;
        switch (gForcedStage)
        {
        case STAGE_ECR1: round = 1; slot = 5; break;
        case STAGE_WCR1: round = 1; slot = 1; break;
        case STAGE_ECSF: round = 2; slot = 3; break;
        case STAGE_WCSF: round = 2; slot = 1; break;
        case STAGE_ECF: round = 3; slot = 2; break;
        case STAGE_WCF: round = 3; slot = 1; break;
        case STAGE_FINALS: round = 4; slot = 1; break;
        case STAGE_PLAYOFF: break;
        default: return result;
        }
        output[1] = static_cast<short>(round);
        output[2] = static_cast<short>(slot);
        output[3] = static_cast<short>(gForcedSeriesGame);
        if (gPlayoffStateDebug)
        {
            static int previousRound = -1, previousSlot = -1, previousGame = -1;
            if (previousRound != round || previousSlot != slot ||
                previousGame != gForcedSeriesGame)
            {
                char message[256] = {};
                sprintf_s(message, sizeof(message),
                    "[CustomPaths] PLAYOFF_SPEECH_CONTEXT game=2005 stage=%s "
                    "round=%d slot=%d seriesGame=%d length=%d winsApplied=%s\n",
                    StageName(gForcedStage), round, slot, gForcedSeriesGame,
                    gForcedSeriesLength, gForcedHomeWins == gForcedAwayWins
                        ? "equal-counts" : "native-order-unverified");
                WritePlayoffStateLog(message);
                previousRound = round;
                previousSlot = slot;
                previousGame = gForcedSeriesGame;
            }
        }
        return result;
    }

unsigned char* __fastcall Live05SpeechSeriesQueryHook(
        void* object, void*, unsigned char* output,
        unsigned int period, unsigned int roundAndSlot,
        unsigned int slotAndGame)
    {
        // Preserve the native hidden-result ABI: result pointer followed by
        // three packed arguments, with RET 10h at 0x004ECB25.
        typedef unsigned char* (__thiscall* QueryFn)(void*, unsigned char*,
            unsigned int, unsigned int, unsigned int);
        unsigned char* result = reinterpret_cast<QueryFn>(0x004EC880)(
            object, output, period, roundAndSlot, slotAndGame);
        if (!Live05ForcedSpeechEnabled() || !output)
            return result;
        // Native 54-byte series record: length +6, wins needed +8,
        // participant wins +0E/+10 (consumed by 44CE15..44CE2B).
        *reinterpret_cast<short*>(output + 6) = static_cast<short>(gForcedSeriesLength);
        *reinterpret_cast<short*>(output + 8) = static_cast<short>((gForcedSeriesLength + 1) / 2);
        if (gForcedHomeWins == gForcedAwayWins)
        {
            *reinterpret_cast<short*>(output + 0x0E) = static_cast<short>(gForcedHomeWins);
            *reinterpret_cast<short*>(output + 0x10) = static_cast<short>(gForcedAwayWins);
        }
        return result;
    }

void __declspec(naked) Live06PlayoffCacheInitializerHook()
    {
        __asm
        {
            call dword ptr [gLive06PlayoffCacheInitializer]
            pushfd
            pushad
            cld
            mov ebx, esp
            sub esp, 528
            and esp, 0FFFFFFF0h
            fxsave [esp]
            call ApplyLive06ForcedSpeechContext
            fxrstor [esp]
            mov esp, ebx
            popad
            popfd
            ret
        }
    }

void __declspec(naked) Live06PregameContextHook()
    {
        __asm
        {
            pushfd
            pushad
            cld
            mov ebx, esp
            sub esp, 528
            and esp, 0FFFFFFF0h
            fxsave [esp]
            call ApplyLive06ForcedSpeechContext
            fxrstor [esp]
            mov esp, ebx
            popad
            popfd
            jmp dword ptr [gLive06PregameContinuation]
        }
    }

bool InstallLive06PregameContextHook()
    {
        // 0x0045D240 builds event 164; its only direct caller is
        // 0x00461B69. 0x0045CD00 is a separate request-management routine,
        // ending at 0x0045D224, and cannot cover the first pregame event.
        // MOV AL,[00CAF5D0h] is one complete five-byte instruction.
        const bool live07 = gGameVersion == GAME_LIVE_07;
        const bool live08 = gGameVersion == GAME_LIVE_08;
        const bool live05 = gGameVersion == GAME_LIVE_2005;
        const size_t patchLength = live08 ? 7 : (live07 || live05 ? 6 : 5);
        unsigned char* target = reinterpret_cast<unsigned char*>(live05 ? 0x0045B5F0 : (live08 ? 0x0048ECE0 : (live07 ? 0x0047C430 : 0x0045D240)));
        const unsigned char expected[5] = { 0xA0, 0xD0, 0xF5, 0xCA, 0x00 };
        const unsigned char expected07[6] = { 0x81, 0xEC, 0xA4, 0x02, 0x00, 0x00 };
        // Two complete PUSH instructions, before the native SEH frame.
        const unsigned char expected08[7] = { 0x6A, 0xFF, 0x68, 0x71, 0x1C, 0xBF, 0x00 };
        const unsigned char expected05[6] = { 0x81, 0xEC, 0x5C, 0x01, 0x00, 0x00 };
        if (std::memcmp(target, live05 ? expected05 : (live08 ? expected08 : (live07 ? expected07 : expected)), patchLength) != 0)
            return false;
        unsigned char* trampoline = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, patchLength + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!trampoline)
            return false;
        std::memcpy(trampoline, target, patchLength);
        trampoline[patchLength] = 0xE9;
        const int back = static_cast<int>(
            reinterpret_cast<uintptr_t>(target + patchLength) -
            reinterpret_cast<uintptr_t>(trampoline + patchLength + 5));
        std::memcpy(trampoline + patchLength + 1, &back, sizeof(back));
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }
        gLive06PregameContinuation = reinterpret_cast<uintptr_t>(trampoline);
        unsigned char replacement[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
        const int forward = static_cast<int>(
            reinterpret_cast<uintptr_t>(Live06PregameContextHook) -
            reinterpret_cast<uintptr_t>(target + 5));
        std::memcpy(replacement + 1, &forward, sizeof(forward));
        std::memcpy(target, replacement, patchLength);
        DWORD ignored = 0;
        VirtualProtect(target, patchLength, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), trampoline, patchLength + 5);
        FlushInstructionCache(GetCurrentProcess(), target, patchLength);
        return true;
    }

#endif
void InitializeForcedStageSettings()
    {
        char iniPath[MAX_PATH] = {};
        const DWORD exeLength = GetModuleFileNameA(
            nullptr, iniPath, static_cast<DWORD>(sizeof(iniPath)));
        if (exeLength == 0 || exeLength >= sizeof(iniPath))
        {
            strcpy_s(iniPath, iniPath);
        }
        else
        {
            char* slash = strrchr(iniPath, '\\');
            if (slash)
                strcpy_s(slash + 1,
                    sizeof(iniPath) - static_cast<size_t>(slash + 1 - iniPath),
                    "main.ini");
            else
                strcpy_s(iniPath, "main.ini");
        }

        char value[32] = {};
        GetPrivateProfileStringA(
            "PLAYOFF",
            "FORCE_STAGE",
            "OFF",
            value,
            sizeof(value),
            iniPath);

        // Be tolerant of accidental surrounding whitespace or quotes.
        std::string normalizedStage(value);
        while (!normalizedStage.empty() &&
               std::isspace(static_cast<unsigned char>(normalizedStage.front())))
            normalizedStage.erase(normalizedStage.begin());
        while (!normalizedStage.empty() &&
               std::isspace(static_cast<unsigned char>(normalizedStage.back())))
            normalizedStage.pop_back();
        if (normalizedStage.size() >= 2 &&
            ((normalizedStage.front() == '"' && normalizedStage.back() == '"') ||
             (normalizedStage.front() == '\'' && normalizedStage.back() == '\'')))
        {
            normalizedStage =
                normalizedStage.substr(1, normalizedStage.size() - 2);
        }

        const char* stageValue = normalizedStage.c_str();

        if (_stricmp(stageValue, "PLAYOFF") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_PLAYOFF;
        }
        else if (_stricmp(stageValue, "ECR1") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_ECR1;
        }
        else if (_stricmp(stageValue, "WCR1") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_WCR1;
        }
        else if (_stricmp(stageValue, "ECSF") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_ECSF;
        }
        else if (_stricmp(stageValue, "WCSF") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_WCSF;
        }
        else if (_stricmp(stageValue, "ECF") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_ECF;
        }
        else if (_stricmp(stageValue, "WCF") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_WCF;
        }
        else if (_stricmp(stageValue, "FINALS") == 0)
        {
            gForcedStageEnabled = true;
            gForcedStage = STAGE_FINALS;
        }
        else
        {
            gForcedStageEnabled = false;
            gForcedStage = STAGE_REGULAR;
        }

        gForceGlobalGameMode =
            GetPrivateProfileIntA(
                "PLAYOFF",
                "GLOBAL_GAME_MODE",
                0,
                iniPath) != 0;

        const int requestedSeriesGame =
            GetPrivateProfileIntA(
                "PLAYOFF",
                "FORCE_SERIES_GAME",
                0,
                iniPath);
        const int requestedSeriesLength = GetPrivateProfileIntA(
            "PLAYOFF", "FORCE_SERIES_LENGTH", 7, iniPath);
        const bool validSeriesLength = requestedSeriesLength == 1 ||
            requestedSeriesLength == 3 || requestedSeriesLength == 5 ||
            requestedSeriesLength == 7;
        const int maximumPreGameWins = validSeriesLength
            ? (requestedSeriesLength - 1) / 2 : -1;
        const int requestedHomeWins =
            GetPrivateProfileIntA(
                "PLAYOFF",
                "FORCE_HOME_WINS",
                -1,
                iniPath);
        const int requestedAwayWins =
            GetPrivateProfileIntA(
                "PLAYOFF",
                "FORCE_AWAY_WINS",
                -1,
                iniPath);

        gForcedSeriesEnabled =
            validSeriesLength &&
            requestedSeriesGame >= 1 && requestedSeriesGame <= requestedSeriesLength &&
            requestedHomeWins >= 0 && requestedHomeWins <= maximumPreGameWins &&
            requestedAwayWins >= 0 && requestedAwayWins <= maximumPreGameWins &&
            requestedHomeWins + requestedAwayWins ==
                requestedSeriesGame - 1;

        if (gForcedSeriesEnabled)
        {
            gForcedSeriesGame = requestedSeriesGame;
            gForcedSeriesLength = requestedSeriesLength;
            gForcedHomeWins = requestedHomeWins;
            gForcedAwayWins = requestedAwayWins;
        }
        else
        {
            gForcedSeriesGame = 0;
            gForcedSeriesLength = 7;
            gForcedHomeWins = 0;
            gForcedAwayWins = 0;

            if (requestedSeriesGame != 0 ||
                requestedHomeWins != -1 ||
                requestedAwayWins != -1)
            {
                OutputDebugStringA(
                    "[CustomPaths] Ignored invalid forced-series values; "
                    "length must be 1/3/5/7, game must be within length, "
                    "neither team may have won the series, and total wins "
                    "must equal game-1.\n");
            }
        }

        gPlayoffStateDebug =
            GetPrivateProfileIntA(
                "PLAYOFF",
                "LOG_STATE",
                0,
                iniPath) != 0;

        char nativeMode[32] = {};
        GetPrivateProfileStringA(
            "GAME_MODE",
            "FORCE_MODE",
            "OFF",
            nativeMode,
            sizeof(nativeMode),
            iniPath);

        if (_stricmp(nativeMode, "ALLSTAR") == 0 ||
            _stricmp(nativeMode, "ALL_STAR") == 0)
        {
            gForcedNativeGameMode = NATIVE_MODE_ALLSTAR;
        }
        else if (_stricmp(nativeMode, "ROOKIE") == 0 ||
                 _stricmp(nativeMode, "ROOKIE_CHALLENGE") == 0)
        {
            gForcedNativeGameMode = NATIVE_MODE_ROOKIE;
        }
        else
        {
            gForcedNativeGameMode = NATIVE_MODE_OFF;
        }

        // A native All-Star event and a forced playoff state are mutually
        // exclusive. Give the explicit event selection priority so playoff
        // arena assets and tournament state are not mixed into the test.
        if (gForcedNativeGameMode != NATIVE_MODE_OFF &&
            (gForcedStageEnabled || gForceGlobalGameMode))
        {
            gForcedStageEnabled = false;
            gForcedStage = STAGE_REGULAR;
            gForceGlobalGameMode = false;
            gForcedSeriesEnabled = false;
            OutputDebugStringA(
                "[CustomPaths] FORCE_MODE overrides [PLAYOFF] forcing.\n");
        }

        // A series score is meaningful only when the complete native playoff
        // presentation state is being forced. Do not leave a partial request
        // active, because that could combine a regular exhibition with
        // playoff clinching flags.
        if (gForcedSeriesEnabled &&
            (!gForcedStageEnabled || !gForceGlobalGameMode))
        {
            gForcedSeriesEnabled = false;
            gForcedSeriesGame = 0;
            gForcedHomeWins = 0;
            gForcedAwayWins = 0;
            OutputDebugStringA(
                "[CustomPaths] FORCE_SERIES_GAME requires FORCE_STAGE and "
                "GLOBAL_GAME_MODE=1; forced series ignored.\n");
        }

        if (gForcedStageEnabled)
        {
            char message[192] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] Forced stage=%s scope=%s.\n",
                StageName(gForcedStage),
                gForceGlobalGameMode ? "global" : "stadium-only");
            OutputDebugStringA(message);
        }

        if (gForcedSeriesEnabled)
        {
            char message[224] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] Forced series game=%d homeWins=%d "
                "awayWins=%d.\n",
                gForcedSeriesGame,
                gForcedHomeWins,
                gForcedAwayWins);
            OutputDebugStringA(message);
        }

        if (gPlayoffStateDebug)
        {
            OutputDebugStringA(
                "[CustomPaths] Read-only playoff-state logging enabled.\n");
        }


        if (gForcedNativeGameMode != NATIVE_MODE_OFF)
        {
            char message[160] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] Requested native game mode=%s.\n",
                ForcedNativeGameModeName(gForcedNativeGameMode));
            OutputDebugStringA(message);
        }
    }

void ApplyForcedNativeGameMode(void* options)
    {
        if (gForcedNativeGameMode == NATIVE_MODE_OFF)
            return;

        // The confirmed OptionsSettings values currently apply only to 06.
        if (gGameVersion != GAME_LIVE_06)
        {
            if (!gLoggedUnsupportedNativeMode)
            {
                OutputDebugStringA(
                    "[CustomPaths] FORCE_MODE is currently mapped only for "
                    "NBA Live 06; request ignored.\n");
                gLoggedUnsupportedNativeMode = true;
            }
            return;
        }

        if (!options)
        {
            if (!gLoggedMissingNativeModeOptions)
            {
                OutputDebugStringA(
                    "[CustomPaths] Native game mode requested, but the "
                    "OptionsSettings pointer is not available yet.\n");
                gLoggedMissingNativeModeOptions = true;
            }
            return;
        }

        gLoggedMissingNativeModeOptions = false;

        __try
        {
            unsigned char* bytes =
                reinterpret_cast<unsigned char*>(options);
            int* mode = reinterpret_cast<int*>(bytes + 0x5C);
            int* round = reinterpret_cast<int*>(bytes + 0x158);
            const int originalMode = *mode;
            const int originalRound = *round;

            if (originalMode < 0 || originalMode > 32 ||
                originalRound < 0 || originalRound > 32)
            {
                OutputDebugStringA(
                    "[CustomPaths] Rejected native game-mode write because "
                    "OptionsSettings is not initialized.\n");
                return;
            }

            const int targetMode =
                gForcedNativeGameMode == NATIVE_MODE_ALLSTAR ? 16 : 17;
            const int targetRound = 7;
            if (originalMode == targetMode && originalRound == targetRound)
                return;

            *mode = targetMode;
            *round = targetRound;

            char message[256] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] Applied NBA Live 06 native mode test: "
                "options=%08X mode %d->%d round %d->%d event=%s.\n",
                static_cast<unsigned int>(
                    reinterpret_cast<uintptr_t>(options)),
                originalMode,
                targetMode,
                originalRound,
                targetRound,
                ForcedNativeGameModeName(gForcedNativeGameMode));
            OutputDebugStringA(message);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            OutputDebugStringA(
                "[CustomPaths] Failed to apply native game mode.\n");
        }
    }

void ApplyGlobalForcedStage(void* options)
    {
        if (!gForcedStageEnabled || !gForceGlobalGameMode)
            return;

        if (!options)
        {
            if (!gLoggedMissingGlobalOptions)
            {
                OutputDebugStringA(
                    "[CustomPaths] Global forced stage requested, but the "
                    "game-options pointer is not available yet.\n");
                gLoggedMissingGlobalOptions = true;
            }
            return;
        }

        gLoggedMissingGlobalOptions = false;

        const unsigned int modeOffset =
            gGameVersion == GAME_LIVE_08 ? 0x64 : 0x5C;
        const unsigned int roundOffset =
            gGameVersion == GAME_LIVE_08 ? 0x168 : 0x158;

        __try
        {
            int* mode = reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(options) + modeOffset);
            int* round = reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(options) + roundOffset);

            const int originalMode = *mode;
            const int originalRound = *round;

            // Do not write into a pointer that has not reached a plausible
            // initialized OptionsSettings state yet.
            if (originalMode < 0 || originalMode > 16 ||
                originalRound < 0 || originalRound > 32)
            {
                if (options != gLastRejectedOptions ||
                    originalMode != gLastRejectedMode ||
                    originalRound != gLastRejectedRound)
                {
                    gLastRejectedOptions = options;
                    gLastRejectedMode = originalMode;
                    gLastRejectedRound = originalRound;

                    char message[224] = {};
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] Rejected global stage write: "
                        "options=%08X mode=%d round=%d.\n",
                        static_cast<unsigned int>(
                            reinterpret_cast<uintptr_t>(options)),
                        originalMode,
                        originalRound);
                    OutputDebugStringA(message);
                }
                return;
            }

            gLastRejectedOptions = nullptr;

            // Preserve an existing Franchise Playoff mode. Exhibition and
            // other modes are promoted to the standard Playoff mode.
            const int targetMode = originalMode == 4 ? 4 : 2;
            const int targetRound = StageRoundValue(gForcedStage);

            // The stadium setter can be called many times for one game. Avoid
            // touching stable state after the requested values are present.
            if (originalMode == targetMode && originalRound == targetRound)
                return;

            *mode = targetMode;
            *round = targetRound;

            char message[256] = {};
            sprintf_s(
                message,
                sizeof(message),
                "[CustomPaths] Applied global stage once: options=%08X "
                "mode %d->%d round %d->%d stage=%s.\n",
                static_cast<unsigned int>(
                    reinterpret_cast<uintptr_t>(options)),
                originalMode,
                targetMode,
                originalRound,
                targetRound,
                StageName(gForcedStage));
            OutputDebugStringA(message);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            OutputDebugStringA(
                "[CustomPaths] Failed to apply global forced stage.\n");
        }
    }

unsigned int ForcedStageBracketSlot(
        PostseasonStage stage,
        unsigned int currentGame)
    {
        switch (stage)
        {
        case STAGE_WCR1:
        case STAGE_ECR1:
        {
            // First-round slots 1-4 are West and 5-8 are East. Preserve an
            // already-selected matchup when it belongs to the requested side.
            const unsigned int firstSlot = stage == STAGE_WCR1 ? 1u : 5u;
            const unsigned int lastSlot = firstSlot + 3u;
            if (currentGame > 0u && currentGame <= 15u * 7u)
            {
                const unsigned int currentSlot =
                    ((currentGame - 1u) % 15u) + 1u;
                if (currentSlot >= firstSlot && currentSlot <= lastSlot)
                    return currentSlot;
            }
            return firstSlot;
        }
        case STAGE_WCSF:
        case STAGE_ECSF:
        {
            // Schedule slots 9/10 are West, 11/12 East. Preserve a
            // matching native matchup; use its conference's first slot
            // when launching without a matching schedule selection.
            const unsigned int firstSlot = stage == STAGE_WCSF ? 9u : 11u;
            if (currentGame > 0u && currentGame <= 15u * 7u)
            {
                const unsigned int currentSlot = ((currentGame - 1u) % 15u) + 1u;
                if (currentSlot == firstSlot || currentSlot == firstSlot + 1u)
                    return currentSlot;
            }
            return firstSlot;
        }
        case STAGE_WCF:
            return 13u;
        case STAGE_ECF:
            return 14u;
        case STAGE_FINALS:
            return 15u;
        case STAGE_PLAYOFF:
        {
            if (currentGame > 0u && currentGame <= 15u * 7u)
            {
                const unsigned int currentSlot =
                    ((currentGame - 1u) % 15u) + 1u;
                if (currentSlot <= 12u)
                    return currentSlot;
            }
            return 1u;
        }
        default:
            return 0u;
        }
    }

unsigned int ForcedScheduleGameId(unsigned int currentGame)
    {
        const unsigned int bracketSlot =
            ForcedStageBracketSlot(gForcedStage, currentGame);
        if (bracketSlot == 0u ||
            gForcedSeriesGame < 1 ||
            gForcedSeriesGame > 7)
        {
            return 0u;
        }

        return bracketSlot +
            15u * static_cast<unsigned int>(gForcedSeriesGame - 1);
    }

short __cdecl Live06ForcedCurrentGameGetterHook()
    {
        unsigned int currentGame = 0u;
        __try
        {
            unsigned char* scheduleInfo =
                *reinterpret_cast<unsigned char**>(0x00CB2AE0);
            if (scheduleInfo)
            {
                currentGame =
                    *reinterpret_cast<unsigned short*>(scheduleInfo + 0x64);
            }

            const unsigned int forcedGame =
                ForcedScheduleGameId(currentGame);
            if (forcedGame != 0u)
            {
                // Keep the backing object synchronized once it exists, while
                // still allowing native consumers to receive the forced ID
                // before ScheduleInfo has been constructed.
                if (scheduleInfo)
                {
                    *reinterpret_cast<unsigned short*>(scheduleInfo + 0x64) =
                        static_cast<unsigned short>(forcedGame);
                }

                if (!gLoggedForcedScheduleGetter)
                {
                    char message[224] = {};
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] PLAYOFF_SCHEDULE_GET forced=%u "
                        "original=%u stage=%s game=%d service=%s\n",
                        forcedGame,
                        currentGame,
                        StageName(gForcedStage),
                        gForcedSeriesGame,
                        scheduleInfo ? "ready" : "unavailable");
                    WritePlayoffStateLog(message);
                    gLoggedForcedScheduleGetter = true;
                }

                return static_cast<short>(forcedGame);
            }

            return static_cast<short>(currentGame);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

void __fastcall Live06ScheduleSelectionHook(
        void* scheduleInfo,
        void*,
        int selection,
        void* selectionContext)
    {
        typedef void (__thiscall* ScheduleSelectionFn)(
            void*,
            int,
            void*);
        ScheduleSelectionFn selectScheduleGame =
            reinterpret_cast<ScheduleSelectionFn>(0x004F0250);

        // Preserve the complete native schedule-selection process first.
        selectScheduleGame(
            scheduleInfo,
            selection,
            selectionContext);

        if (!scheduleInfo ||
            !gForcedSeriesEnabled ||
            !gForcedStageEnabled ||
            !gForceGlobalGameMode)
        {
            return;
        }

        __try
        {
            unsigned char* bytes =
                reinterpret_cast<unsigned char*>(scheduleInfo);
            const unsigned int originalGame =
                *reinterpret_cast<unsigned short*>(bytes + 0x64);
            const unsigned int forcedGame =
                ForcedScheduleGameId(originalGame);
            if (forcedGame == 0u)
                return;

            *reinterpret_cast<unsigned short*>(bytes + 0x64) =
                static_cast<unsigned short>(forcedGame);

            // This object becomes the global ScheduleInfo singleton during
            // initialization. Keep the global object synchronized whenever it
            // is already published.
            unsigned char* globalSchedule =
                *reinterpret_cast<unsigned char**>(0x00CB2AE0);
            if (globalSchedule)
            {
                *reinterpret_cast<unsigned short*>(globalSchedule + 0x64) =
                    static_cast<unsigned short>(forcedGame);
            }

            if (!gLoggedForcedScheduleSelection)
            {
                char message[240] = {};
                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] PLAYOFF_SCHEDULE_SELECT forced=%u "
                    "original=%u stage=%s game=%d object=%08X global=%s\n",
                    forcedGame,
                    originalGame,
                    StageName(gForcedStage),
                    gForcedSeriesGame,
                    static_cast<unsigned int>(
                        reinterpret_cast<uintptr_t>(scheduleInfo)),
                    globalSchedule ? "ready" : "unavailable");
                WritePlayoffStateLog(message);
                gLoggedForcedScheduleSelection = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WritePlayoffStateLog(
                "[CustomPaths] PLAYOFF_SCHEDULE_SELECT exception=yes.\n");
        }
    }

void ApplyForcedSeriesState(void* options)
    {
        if (!gForcedSeriesEnabled ||
            !gForcedStageEnabled ||
            !gForceGlobalGameMode)
        {
            return;
        }

#if defined(_M_IX86)
        if (gGameVersion == GAME_LIVE_2005 ||
            gGameVersion == GAME_LIVE_07 || gGameVersion == GAME_LIVE_08)
        {
            ApplyForcedSeriesRulesForBuild(options);
            return; // Schedule service layout is not shared with Live 06.
        }
#endif
        // Remaining schedule/global addresses below are specific to Live 06.
        if (gGameVersion != GAME_LIVE_06 || !options)
            return;

        __try
        {
            unsigned char* bytes =
                reinterpret_cast<unsigned char*>(options);
            const unsigned char awayCanWin =
                gForcedAwayWins == (gForcedSeriesLength - 1) / 2 ? 1 : 0;
            const unsigned char homeCanWin =
                gForcedHomeWins == (gForcedSeriesLength - 1) / 2 ? 1 : 0;

            // RulesSettings fields consumed by gameplay setup.
            *reinterpret_cast<int*>(bytes + 0x160) =
                (gForcedSeriesLength - 1) / 2;
            bytes[0x10D] = awayCanWin;
            bytes[0x10E] = homeCanWin;

            // Mirror the same values into the live rules globals in case the
            // initial RulesSettings copy has already occurred.
            *reinterpret_cast<unsigned char*>(0x00D212D5) = awayCanWin;
            *reinterpret_cast<unsigned char*>(0x00D212D4) = homeCanWin;

            unsigned char* scheduleInfo =
                *reinterpret_cast<unsigned char**>(0x00CB2AE0);
            if (!scheduleInfo)
            {
                OutputDebugStringA(
                    "[CustomPaths] Forced series requested, but the Live 06 "
                    "schedule service is not available yet.\n");
                return;
            }

            unsigned short* currentGamePointer =
                reinterpret_cast<unsigned short*>(scheduleInfo + 0x64);
            const unsigned int originalGame = *currentGamePointer;
            const unsigned int bracketSlot =
                ForcedStageBracketSlot(gForcedStage, originalGame);
            if (bracketSlot == 0u)
                return;

            const unsigned int targetGame =
                ForcedScheduleGameId(originalGame);
            if (targetGame == 0u)
                return;
            *currentGamePointer = static_cast<unsigned short>(targetGame);

            if (originalGame != targetGame)
            {
                char message[256] = {};
                sprintf_s(
                    message,
                    sizeof(message),
                    "[CustomPaths] Applied forced series: stage=%s "
                    "game=%d score(home-away)=%d-%d schedule %u->%u "
                    "slot=%u homeCanWin=%u awayCanWin=%u.\n",
                    StageName(gForcedStage),
                    gForcedSeriesGame,
                    gForcedHomeWins,
                    gForcedAwayWins,
                    originalGame,
                    targetGame,
                    bracketSlot,
                    homeCanWin,
                    awayCanWin);
                OutputDebugStringA(message);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            OutputDebugStringA(
                "[CustomPaths] Failed to apply forced playoff-series state.\n");
        }
    }

PostseasonStage GetPostseasonStage(void* suppliedOptions)
    {
        if (gForcedNativeGameMode != NATIVE_MODE_OFF)
            return STAGE_REGULAR;

        if (gForcedStageEnabled)
            return gForcedStage;

        void* options = GetCurrentOptions(suppliedOptions);
        if (!options)
            return STAGE_REGULAR;

        const unsigned int modeOffset =
            gGameVersion == GAME_LIVE_08 ? 0x64 : 0x5C;
        const unsigned int roundOffset =
            gGameVersion == GAME_LIVE_08 ? 0x168 : 0x158;

        __try
        {
            const int mode = *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(options) + modeOffset);
            const int round = *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(options) + roundOffset);

            // These are the same fields used by the games' native playoff-logo
            // selection. The bracket enumerates East then West, with the NBA
            // Finals last. A normal exhibition uses the round sentinel 16.
            if (mode != 2 && mode != 4)
                return STAGE_REGULAR;

            if (round == 6)
                return STAGE_FINALS;
            if (round == 4)
                return STAGE_ECF;
            if (round == 5)
                return STAGE_WCF;
            if (round == 2)
                return STAGE_ECSF;
            if (round == 3)
                return STAGE_WCSF;
            if (round == 0)
                return STAGE_ECR1;
            if (round == 1)
                return STAGE_WCR1;

            return STAGE_REGULAR;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return STAGE_REGULAR;
        }
    }

PostseasonStage PrepareGameContext(void* suppliedOptions)
{
        void* options = GetCurrentOptions(suppliedOptions);
        // Earlier FEUber/bracket probes were useful for eliminating the
        // frontend objects, but they are not part of the gameplay handoff and
        // produced large logs. RulesSettings is now decoded directly above.
        ApplyForcedNativeGameMode(options);
        ApplyGlobalForcedStage(options);
        ApplyForcedSeriesState(options);
        LogPlayoffStateSnapshot(options);
        return GetPostseasonStage(options);
}

bool InstallLive06AccessorEntryHook(
        uintptr_t address,
        const unsigned char* expected,
        void* hook)
    {
        const size_t patchLength = 5;
        unsigned char* target =
            reinterpret_cast<unsigned char*>(address);

        __try
        {
            if (std::memcmp(target, expected, patchLength) != 0)
                return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(
                target,
                patchLength,
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        target[0] = 0xE9;
        *reinterpret_cast<int32_t*>(target + 1) =
            static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(hook) - (address + 5));

        DWORD ignored = 0;
        VirtualProtect(target, patchLength, oldProtect, &ignored);
        FlushInstructionCache(
            GetCurrentProcess(),
            target,
            patchLength);
        return true;
    }

void InstallLive06PlayoffAccessorHooks()
    {
        const unsigned char expectedInteger[5] = {
            0x8B, 0x44, 0x24, 0x08, 0x48
        };
        const unsigned char expectedBoolean[5] = {
            0x8B, 0x54, 0x24, 0x08, 0x32
        };

        const bool integerInstalled = InstallLive06AccessorEntryHook(
            0x004EA3D0,
            expectedInteger,
            reinterpret_cast<void*>(&Live06IntegerSettingGetterHook));
        const bool booleanInstalled = InstallLive06AccessorEntryHook(
            0x004EA420,
            expectedBoolean,
            reinterpret_cast<void*>(&Live06BooleanSettingGetterHook));

        if (integerInstalled && booleanInstalled)
        {
            WritePlayoffStateLog(
                "[CustomPaths] Forced playoff SettingsObject accessors "
                "installed.\n");
        }
        else
        {
            WritePlayoffStateLog(
                "[CustomPaths] Forced playoff SettingsObject accessor "
                "validation failed.\n");
        }
    }

void InitializeGameContextHooks()
{
        InitializePlayoffStateLog();

#if defined(_M_IX86)
        if (gGameVersion == GAME_LIVE_2005)
        {
            if (gPlayoffStateDebug)
                WritePlayoffStateLog(InstallLive06SpeechDispatchTrace()
                    ? "[CustomPaths] Live 2005 speech-event trace installed (70DDB0).\n"
                    : "[CustomPaths] Live 2005 speech-event trace validation failed.\n");
            // Intercept every direct native sub_542980 call and cache only
            // selector-6 (scheduleplayoff.dbf) records. The hook ranks the
            // proven live-game call paths above generic schedule lookups.
            const unsigned int scheduleQueryCalls[] = {
                0x005432E9, 0x00543524, 0x005543E8, 0x00554B4A,
                0x0055ACBD, 0x00572EFB, 0x005916F4
            };
            const size_t scheduleQueryCallCount =
                sizeof(scheduleQueryCalls) / sizeof(scheduleQueryCalls[0]);
            const bool scheduleQueryValid = ValidateCallSites(
                scheduleQueryCalls, scheduleQueryCallCount, 0x00542980);
            if (scheduleQueryValid)
            {
                for (size_t i = 0; i < scheduleQueryCallCount; ++i)
                    patch::RedirectCall(
                        scheduleQueryCalls[i], Live05NativeScheduleQueryHook);
            }
            if (gPlayoffStateDebug)
            {
                WritePlayoffStateLog(scheduleQueryValid
                    ? "[CustomPaths] Live 2005 native schedule-query hooks installed "
                      "(query=542980 selector=6).\n"
                    : "[CustomPaths] Live 2005 native schedule-query hook "
                      "validation failed.\n");
            }

            if (Live05ForcedSpeechEnabled())
            {
                // Forced speech presentation still uses the older 539C40
                // five-short query. Keep these hooks only for forced-mode
                // rewriting; native series detection no longer depends on it.
                const unsigned int queryCalls[] = {
                    0x0043BF88, 0x0044CDE5, 0x0045BB15
                };
                const bool queryValid =
                    ValidateCallSites(queryCalls, 3, 0x00539C40);
                if (queryValid)
                {
                    for (size_t i = 0; i < 3; ++i)
                        patch::RedirectCall(
                            queryCalls[i], Live05SpeechPlayoffQueryHook);
                }
                const unsigned int seriesCalls[] = { 0x0044CE02, 0x0045BB35 };
                const bool valid = queryValid &&
                    ValidateCallSites(seriesCalls, 2, 0x004EC880);
                const bool installed = valid && InstallLive06PregameContextHook();
                if (installed)
                {
                    for (size_t i = 0; i < 2; ++i)
                        patch::RedirectCall(seriesCalls[i], Live05SpeechSeriesQueryHook);
                }
                WritePlayoffStateLog(installed
                    ? "[CustomPaths] Live 2005 forced speech-context hooks installed "
                      "(pregame=45B5F0 speechQuery=539C40 series=4EC880).\n"
                    : "[CustomPaths] Live 2005 forced speech-context validation failed.\n");
            }
        }
        if (gGameVersion == GAME_LIVE_07 || gGameVersion == GAME_LIVE_08)
        {
            const bool live08 = gGameVersion == GAME_LIVE_08;
            if (gPlayoffStateDebug)
                WritePlayoffStateLog(InstallLive06SpeechDispatchTrace()
                    ? "[CustomPaths] Live 07/08 speech-event trace installed.\n"
                    : "[CustomPaths] Live 07/08 speech-event trace validation failed.\n");
            if (gForcedSeriesEnabled && gForcedStageEnabled && gForceGlobalGameMode)
            {
                // 08's init thunk jumps to relocated native code at E2F38C.
                const unsigned int calls[] = { live08 ? 0x006632FDu : 0x00643BC6u };
                const unsigned int initializer = live08 ? 0x004642B0u : 0x004617F0u;
                const bool initValid = ValidateCallSites(calls, 1, initializer);
                const bool pregameInstalled = initValid && InstallLive06PregameContextHook();
                if (pregameInstalled)
                {
                    gLive06PlayoffCacheInitializer = initializer;
                    patch::RedirectCall(calls[0], Live06PlayoffCacheInitializerHook);
                }
                WritePlayoffStateLog(pregameInstalled
                    ? (live08
                        ? "[CustomPaths] Live 08 forced speech-context hooks installed (init=4642B0 pregame=48ECE0).\n"
                        : "[CustomPaths] Live 07 forced speech-context hooks installed (init=4617F0 pregame=47C430).\n")
                    : "[CustomPaths] Live 07/08 forced speech-context validation failed.\n");
            }
        }
        // Install independently of force settings, allowing an unmodified
        // real playoff launch to supply the comparison baseline.
        if (gGameVersion == GAME_LIVE_06 && gPlayoffStateDebug)
        {
            WritePlayoffStateLog(InstallLive06SpeechDispatchTrace()
                ? "[CustomPaths] Playoff speech-event dispatch trace installed.\n"
                : "[CustomPaths] Playoff speech-event dispatch trace validation "
                  "or installation failed.\n");
            WritePlayoffStateLog(InstallLive06SetupHandoffTrace()
                ? "[CustomPaths] Playoff GameSetup handoff trace installed.\n"
                : "[CustomPaths] Playoff GameSetup handoff trace validation "
                  "or installation failed.\n");
            const unsigned int introCall[] = { 0x0046018F };
            const bool valid = ValidateCallSites(introCall, 1, 0x00434610);
            if (valid)
                patch::RedirectCall(introCall[0], Live06IntroEventTraceHook);
            WritePlayoffStateLog(valid
                ? "[CustomPaths] Playoff live-intro argument trace installed.\n"
                : "[CustomPaths] Playoff live-intro argument trace validation "
                  "failed.\n");
        }
#endif

        if (gGameVersion == GAME_LIVE_06 &&
            gForcedSeriesEnabled &&
            gForcedStageEnabled &&
            gForceGlobalGameMode)
        {
#if defined(_M_IX86)
            const unsigned int cacheInitCalls[] = { 0x005EFE82 };
            const bool cacheInitValid = ValidateCallSites(
                cacheInitCalls, 1, 0x0043C410);
            const bool pregameInstalled = InstallLive06PregameContextHook();
            if (cacheInitValid)
                patch::RedirectCall(cacheInitCalls[0], Live06PlayoffCacheInitializerHook);
            WritePlayoffStateLog(cacheInitValid && pregameInstalled
                ? "[CustomPaths] Forced playoff speech-context hooks installed "
                  "(pregame builder=0045D240).\n"
                : "[CustomPaths] Forced playoff speech-context hooks incomplete: "
                  "initialization or pregame validation failed.\n");
#endif
            // Unlike a real playoff launch, a promoted exhibition has no
            // ScheduleInfo object.  The game's SettingsObject accessors are
            // still used by presentation/announcer code, so force the series
            // values at that boundary and record exactly which consumers ask
            // for them.
            InstallLive06PlayoffAccessorHooks();

            const unsigned char expectedCommentaryParameter[5] = {
                0xA0, 0xB4, 0xF5, 0xCA, 0x00
            };
            const bool commentaryGetterInstalled =
                InstallLive06AccessorEntryHook(
                    0x00449550,
                    expectedCommentaryParameter,
                    reinterpret_cast<void*>(
                        &Live06CommentaryParameterGetterHook));
            const bool commentarySetterInstalled =
                InstallLive06AccessorEntryHook(
                    0x00449590,
                    expectedCommentaryParameter,
                    reinterpret_cast<void*>(
                        &Live06CommentaryParameterSetterHook));
            WritePlayoffStateLog(
                commentaryGetterInstalled && commentarySetterInstalled
                    ? "[CustomPaths] Playoff commentary-parameter trace "
                      "installed.\n"
                    : "[CustomPaths] Playoff commentary-parameter trace "
                      "validation failed.\n");

            // ScheduleInfo::GetGame is the presentation-facing accessor used
            // after the frontend/gameplay handoff. Hooking it lets announcer
            // and tournament presentation receive the requested schedule ID
            // even before the ScheduleInfo singleton has been constructed.
            const unsigned char expectedCurrentGameGetter[5] = {
                0xA1, 0xE0, 0x2A, 0xCB, 0x00
            };
            const bool installed = InstallLive06AccessorEntryHook(
                0x004E4380,
                expectedCurrentGameGetter,
                reinterpret_cast<void*>(
                    &Live06ForcedCurrentGameGetterHook));
            WritePlayoffStateLog(
                installed
                    ? "[CustomPaths] Forced playoff schedule getter "
                      "installed.\n"
                    : "[CustomPaths] Forced playoff schedule getter "
                      "validation failed.\n");

            const size_t selectionCallCount =
                sizeof(kLive06ScheduleSelectionCalls) /
                sizeof(kLive06ScheduleSelectionCalls[0]);
            const bool selectionHooksValid = ValidateCallSites(
                kLive06ScheduleSelectionCalls,
                selectionCallCount,
                0x004F0250);
            if (selectionHooksValid)
            {
                for (size_t i = 0; i < selectionCallCount; ++i)
                {
                    patch::RedirectCall(
                        kLive06ScheduleSelectionCalls[i],
                        Live06ScheduleSelectionHook);
                }
            }
            WritePlayoffStateLog(
                selectionHooksValid
                    ? "[CustomPaths] Forced playoff schedule-selection "
                      "hooks installed.\n"
                    : "[CustomPaths] Forced playoff schedule-selection "
                      "hook validation failed.\n");
        }

        // The temporary generic SettingsObject accessor trace is no longer
        // installed. It established that only PLAYOFF_LENGTH is consumed by
        // the normal settings-copy path; the live clinching flags reside in
        // RulesSettings and are logged without executable entry hooks.

    
}

}

// Startup teams, jerseys, lineups and venue selection.


using namespace plugin;

namespace
{
    // NBA Live 05 1.0 NOCD. Verified in the supplied 2005 PC executable.
    // All eight E8 calls directly target 005189F0; team layout matches 06-08.
    constexpr uintptr_t kEnsureDefaultJersey05 = 0x005189F0;
    constexpr uintptr_t kJerseyEnsureCalls05[] = {
        0x0053CE73, // native team jersey getter
        0x0053CEE3, // team/scene preparation
        0x0055916B, // Participants5on5 getter
        0x00559A58, // scene game setup: one team
        0x00559A6C, // scene game setup: other team
        0x00559E90, // explicit jersey setter path
        0x005ABBD9, // FE GetJerseyVal wrapper 1
        0x005AE7D9  // FE GetJerseyVal wrapper 2
    };

    // NBA Live 07. Verified against jersey backend pass 3.
    constexpr uintptr_t kEnsureDefaultJersey07 = 0x005499D0;
    constexpr uintptr_t kJerseyEnsureCalls07[] = {
        0x0055A363, // direct team jersey getter
        0x00563DEB, // Participants5on5 GetJersey
        0x005641D3, // team/scene preparation
        0x005642C8, // home/away game setup jersey resolution
        0x005642DC, // home/away game setup jersey resolution
        0x00564580, // explicit selection: only patches when initially -1
        0x005A1FE9, // FE GetJerseyVal
        0x005A5529  // secondary FE getter
    };

    // NBA Live 08 1.0 NOCD. Verified in nbalive08.exe (SHA-256 recorded
    // in accompanying verification report). Same team-object fields as 06/07.
    constexpr uintptr_t kEnsureDefaultJersey08 = 0x0056A840;
    constexpr uintptr_t kJerseyEnsureCalls08[] = {
        0x00577403, // native team jersey getter
        0x00577413, // native team/scene preparation
        0x0057D16B, // Participants5on5 getter
        0x0057D1F8, // scene home/away jersey resolution
        0x0057D20C, // scene home/away jersey resolution
        0x0057D360, // explicit jersey setter, preserves downstream selection
        0x005B4FF9, // first FE GetJerseyVal wrapper
        0x005B9E09  // second FE GetJerseyVal wrapper
    };

    constexpr uintptr_t kGetJersey = 0x00575980;
    constexpr uintptr_t kEnsureDefaultJersey = 0x00536570;
    constexpr uintptr_t kFrontendEnsureCall = 0x005B7A29; // GetJerseyVal -> EnsureDefaultJersey
    constexpr uintptr_t kHomeSceneGetJerseyCall = 0x0061E307; // push 1; mov ecx,edi; call GetJersey
    constexpr uintptr_t kAwaySceneGetJerseyCall = 0x0061E83D; // push 0; mov ecx,edi; call GetJersey

    // Native NBA Live 2005-08 venue entrypoints and registration sites.
    // Verified from the exact supplied PC executable of each game.
    struct ArenaBuild
    {
        const char* tag;
        uintptr_t getVenueTeamId;
        uintptr_t setStadiumLocation;
        uintptr_t gameSingleton;
        uintptr_t getGameManager;
        uintptr_t getGameMode;
        const uintptr_t* gameGetVenueCalls;
        size_t gameCallCount;
        uintptr_t frontendGetter[2];
        uintptr_t frontendRegistration[2];
    };

    constexpr uintptr_t kGameCalls05[] = {0x0060C0F8}; // Proven-working Play Now path
    constexpr uintptr_t kGameCalls06[] = {0x0061EDA4, 0x0062DD45};
    constexpr uintptr_t kGameCalls07[] = {0x00655B8C, 0x0065666C, 0x006647A5};
    constexpr uintptr_t kGameCalls08[] = {0x00676599, 0x00677292, 0x0068732F};

    const ArenaBuild kArena05 = {
        "NBA05", 0x00517740, 0x0053CC00, 0x00C49D34,
        0x004C1B50, 0x004C1B70,
        kGameCalls05, sizeof(kGameCalls05) / sizeof(kGameCalls05[0]),
        {0x005ABBF0, 0x005AE7F0}, {0x005C6ED5, 0x005C8E15}
    };
    const ArenaBuild kArena06 = {
        "NBA06", 0x0050E270, 0x00536010, 0x00CB2A2C,
        0x004B8870, 0x004B8890,
        kGameCalls06, sizeof(kGameCalls06) / sizeof(kGameCalls06[0]),
        {0x005B7A40, 0x005BA250}, {0x005D4EF5, 0x005D6A35}
    };
    const ArenaBuild kArena07 = {
        "NBA07", 0x0052C7C0, 0x00548AC0, 0x00CEB6F4,
        0x004E4510, 0x004E4530,
        kGameCalls07, sizeof(kGameCalls07) / sizeof(kGameCalls07[0]),
        {0x005A2000, 0x005A5540}, {0x005B9E55, 0x005BCAB5}
    };
    const ArenaBuild kArena08 = {
        "NBA08", 0x00532830, 0x0054DD60, 0x00DA7214,
        0x004E8C20, 0x004E8C40,
        kGameCalls08, sizeof(kGameCalls08) / sizeof(kGameCalls08[0]),
        {0x005B5010, 0x005B9E20}, {0x005D2525, 0x005D5225}
    };

    // Starting five: per-side default active roster positions (0..11).
    // 12..14 are injured reserves and cannot be selected as starters.
    // These positions refer to the native *default lineup array order*,
    // which is normally ROSTERPOS order. See README for this limitation.
    struct StartingFiveBuild
    {
        const char* tag;
        uintptr_t initializer;
        const uintptr_t* callers;
        size_t count;
    };

    constexpr uintptr_t kLineupCalls05[] = {
        0x004F2DF3, 0x004F2E06, 0x004F2E53, 0x0051776B,
        0x0051778E, 0x005177DB, 0x0056D6E7, 0x0056D6FB,
        0x0056D96E, 0x005ABC39, 0x005ABC7B, 0x005AE839,
        0x005AE87B
    };
    constexpr uintptr_t kLineupCalls06[] = {
        0x0050F2E3, 0x0050F2F6, 0x0050F343, 0x00535F4B,
        0x00535F6E, 0x00535FBB, 0x0053726A, 0x00588E87,
        0x00588E9B, 0x0058910E, 0x005B7A89, 0x005B7ACB,
        0x005BA299, 0x005BA2DB
    };
    constexpr uintptr_t kLineupCalls07[] = {
        0x00564153, 0x00564166, 0x005641B3, 0x005689C5,
        0x00568A01, 0x00568A43, 0x00568A81, 0x00568ABD,
        0x00568C0E, 0x00568C34, 0x00568D2B, 0x00568D4E,
        0x00568D9B, 0x00568EDA, 0x0057B147, 0x0057B15B,
        0x0057B3CE, 0x005A2049, 0x005A208B, 0x005A5589,
        0x005A55CB
    };
    constexpr uintptr_t kLineupCalls08[] = {
        0x005345B3, 0x005345DB, 0x00534643, 0x0054DBBA,
        0x0054DCCB, 0x0054DD0B, 0x0054FA0A, 0x00580771,
        0x00580785, 0x005B5059, 0x005B9E69
    };

    const StartingFiveBuild kStarting05 = {
        "NBA05", 0x004CDFA0, kLineupCalls05,
        sizeof(kLineupCalls05) / sizeof(kLineupCalls05[0])
    };
    const StartingFiveBuild kStarting06 = {
        "NBA06", 0x004EA9E0, kLineupCalls06,
        sizeof(kLineupCalls06) / sizeof(kLineupCalls06[0])
    };
    const StartingFiveBuild kStarting07 = {
        "NBA07", 0x0055A370, kLineupCalls07,
        sizeof(kLineupCalls07) / sizeof(kLineupCalls07[0])
    };
    const StartingFiveBuild kStarting08 = {
        "NBA08", 0x0051B870, kLineupCalls08,
        sizeof(kLineupCalls08) / sizeof(kLineupCalls08[0])
    };

    struct StartingFiveSelection
    {
        bool configured = false;
        int rosterPos[5] = {-1, -1, -1, -1, -1};
    };
    // Explicit constant initializers avoid depending on cross-translation-unit
    // startup constructor ordering when the launcher starts installing hooks.
    StartingFiveSelection gHomeStartingFive = {false, {-1, -1, -1, -1, -1}};
    StartingFiveSelection gAwayStartingFive = {false, {-1, -1, -1, -1, -1}};
    bool gStartingFiveTrace = false;

    int gArenaTeam = -1;
    bool gArenaForce = false;
    bool gArenaTrace = false;

    int gHomeJersey = -1;
    int gAwayJersey = -1;
    int gHomeTeam = -1;
    int gAwayTeam = -1;

    using GetJerseyFn = int(__thiscall*)(void*, int);

    bool WriteRelativeCall(uintptr_t site, uintptr_t expectedDestination, const void* replacement)
    {
        // Refuse to alter unexpected binaries; verify the original call target.
        const uint8_t* original = reinterpret_cast<const uint8_t*>(site);
        if (original[0] != 0xE8)
            return false;

        int32_t oldDisplacement = 0;
        std::memcpy(&oldDisplacement, original + 1, sizeof(oldDisplacement));
        const uintptr_t originalDestination = site + 5 + static_cast<intptr_t>(oldDisplacement);
        if (originalDestination != expectedDestination)
            return false;

        const intptr_t distance = reinterpret_cast<uintptr_t>(replacement) - (site + 5);
        if (distance < (std::numeric_limits<int32_t>::min)() ||
            distance > (std::numeric_limits<int32_t>::max)())
            return false;

        DWORD oldProtection = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site), 5, PAGE_EXECUTE_READWRITE, &oldProtection))
            return false;

        const int32_t displacement = static_cast<int32_t>(distance);
        std::memcpy(reinterpret_cast<void*>(site + 1), &displacement, sizeof(displacement));
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
        DWORD unused = 0;
        VirtualProtect(reinterpret_cast<void*>(site), 5, oldProtection, &unused);
        return true;
    }

    using EnsureDefaultJerseyFn = void(__thiscall*)(void*);

    // Apply a configured default only when the object was uninitialized on
    // entry. In particular, never undo an explicit jersey selection made in FE.
    bool ApplyInitialJersey(void* team, int jerseyBefore, const char* game)
    {
        if (!team || jerseyBefore != -1)
            return false;

        auto* bytes = static_cast<uint8_t*>(team);
        const uint8_t homeFlag = *(bytes + 0xA0); // 1 = home, 0 = away
        if (homeFlag > 1)
            return false;

        const int configured = homeFlag ? gHomeJersey : gAwayJersey;
        const int expectedTeam = homeFlag ? gHomeTeam : gAwayTeam;
        if (configured < 0)
            return false;

        const int actualTeam = *reinterpret_cast<const int*>(bytes + 0xA8);
        if (expectedTeam >= 0 && actualTeam != expectedTeam)
            return false;

        *reinterpret_cast<int*>(bytes + 0x98) = configured;
        char message[160];
        sprintf_s(message, sizeof(message),
                  "[StartupTeams] %s initial jersey applied: side=%s team=%d jersey=%d\n",
                  game, homeFlag ? "home" : "away", actualTeam, configured);
        OutputDebugStringA(message);
        return true;
    }

    using GetVenueTeamIdFn = int(__thiscall*)(void*);
    using SetStadiumLocFn = void(__thiscall*)(void*, int);
    using GetGameManagerFn = void*(__cdecl*)();
    using GetGameModeFn = int(__thiscall*)(void*);
    // APT binding calls take (scriptReturnValue, GameParticipants*) on the stack.
    using FrontendGetVenueFn = void(__cdecl*)(void*, void*);

    // FE exposes GetStadiumLocVal via a PUSH of an absolute callback address.
    // This edits only that immediate and preserves the original game's wrapper.
    // Critically, no 5-byte detour or guessed trampoline is required.
    bool WriteFrontendCallback(uintptr_t pushSite, uintptr_t expectedCallback,
                               const void* replacement)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(pushSite);
        if (bytes[0] != 0x68)  // PUSH imm32
            return false;
        uint32_t target = 0;
        std::memcpy(&target, bytes + 1, sizeof(target));
        if (target != expectedCallback)
            return false;
        const auto newTarget = reinterpret_cast<uintptr_t>(replacement);
        if (newTarget > (std::numeric_limits<uint32_t>::max)())
            return false;
        DWORD oldProtection = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(pushSite), 5,
                            PAGE_EXECUTE_READWRITE, &oldProtection))
            return false;
        const uint32_t newValue = static_cast<uint32_t>(newTarget);
        std::memcpy(reinterpret_cast<void*>(pushSite + 1), &newValue,
                    sizeof(newValue));
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<void*>(pushSite), 5);
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(pushSite), 5, oldProtection, &ignored);
        return true;
    }

    // Set the selected venue before the native FE getter populates the APT value.
    // Using the native setter also updates the 3-point-line property in game state.
    // Neither the user's teams nor jerseys are changed here.
    bool ApplyArenaSelection(void* participants, const ArenaBuild& build,
                             const char* phase)
    {
        if (!participants || gArenaTeam < 0)
            return false;
        auto* home = *reinterpret_cast<uint8_t**>(
            static_cast<uint8_t*>(participants) + 0x0C);
        if (!home)
            return false;

        const int before = *reinterpret_cast<int*>(home + 0x9C);
        if (before < -1 || before == gArenaTeam ||
            (!gArenaForce && before != -1))
            return false;

        // The engine reserves a special -2 venue for mode 3. Leave it native.
        const void* manager = reinterpret_cast<GetGameManagerFn>(
            build.getGameManager)();
        const int mode = manager
            ? reinterpret_cast<GetGameModeFn>(build.getGameMode)(
                const_cast<void*>(manager))
            : -1;
        if (mode == 3)
            return false;

        // At very early frontend startup the services may not be initialized.
        // In that case defer to the later FE call or gameplay fallback.
        const auto* global = *reinterpret_cast<uint8_t* const*>(
            build.gameSingleton);
        const void* teamServices = global
            ? *reinterpret_cast<void* const*>(global + 0x28) : nullptr;
        const void* gameState = global
            ? *reinterpret_cast<void* const*>(global + 0x58) : nullptr;
        if (!teamServices || !gameState)
        {
            if (gArenaTrace)
            {
                char s[220];
                sprintf_s(s, sizeof(s),
                    "[StartupTeams] %s arena deferred %s: services=%d state=%d\n",
                    build.tag, phase, teamServices ? 1 : 0, gameState ? 1 : 0);
                OutputDebugStringA(s);
            }
            return false;
        }

        reinterpret_cast<SetStadiumLocFn>(build.setStadiumLocation)(
            participants, gArenaTeam);
        char s[240];
        sprintf_s(s, sizeof(s),
            "[StartupTeams] %s arena applied %s: homeTeam=%d oldVenue=%d newVenue=%d force=%d\n",
            build.tag, phase, *reinterpret_cast<int*>(home + 0xA8),
            before, gArenaTeam, gArenaForce ? 1 : 0);
        OutputDebugStringA(s);
        return true;
    }

    int ResolveArenaGameplay(void* participants, const ArenaBuild& build)
    {
        const bool applied = ApplyArenaSelection(participants, build, "gameplay");
        const int venue = reinterpret_cast<GetVenueTeamIdFn>(
            build.getVenueTeamId)(participants);
        if (gArenaTrace)
        {
            char s[200];
            sprintf_s(s, sizeof(s),
                "[StartupTeams] %s arena gameplay result: venue=%d applied=%d\n",
                build.tag, venue, applied ? 1 : 0);
            OutputDebugStringA(s);
        }
        return venue;
    }

    void FrontendArenaGet(void* result, void* participants,
                          const ArenaBuild& build, unsigned getterNumber)
    {
        const bool applied = ApplyArenaSelection(participants, build, "frontend");
        // Let EA populate its own binding so the name resolves from its database.
        reinterpret_cast<FrontendGetVenueFn>(build.frontendGetter[getterNumber])(
            result, participants);
        if (gArenaTrace)
        {
            // FE binding getters can run every rendered frame; avoid log floods.
            static unsigned traces[2] = {};
            if (getterNumber < 2 && traces[getterNumber]++ < 12)
            {
                char s[180];
                sprintf_s(s, sizeof(s),
                    "[StartupTeams] %s arena FE getter=%u applied=%d\n",
                    build.tag, getterNumber + 1, applied ? 1 : 0);
                OutputDebugStringA(s);
            }
        }
    }

    // Native venue getter: thiscall ECX=participants, no stack arguments.
    // fastcall's unused EDX maintains the same stack ABI.
    int __fastcall ResolveArena05(void* p, void*) { return ResolveArenaGameplay(p, kArena05); }
    int __fastcall ResolveArena06(void* p, void*) { return ResolveArenaGameplay(p, kArena06); }
    int __fastcall ResolveArena07(void* p, void*) { return ResolveArenaGameplay(p, kArena07); }
    int __fastcall ResolveArena08(void* p, void*) { return ResolveArenaGameplay(p, kArena08); }

    // APT frontend bindings: two stack arguments, cdecl (caller cleans up).
    // These wrappers preserve the exact native callback behavior after applying
    // the configured arena before its defaulting logic can run.
    void __cdecl FrontendArena05A(void* out, void* p) { FrontendArenaGet(out, p, kArena05, 0); }
    void __cdecl FrontendArena05B(void* out, void* p) { FrontendArenaGet(out, p, kArena05, 1); }
    void __cdecl FrontendArena06A(void* out, void* p) { FrontendArenaGet(out, p, kArena06, 0); }
    void __cdecl FrontendArena06B(void* out, void* p) { FrontendArenaGet(out, p, kArena06, 1); }
    void __cdecl FrontendArena07A(void* out, void* p) { FrontendArenaGet(out, p, kArena07, 0); }
    void __cdecl FrontendArena07B(void* out, void* p) { FrontendArenaGet(out, p, kArena07, 1); }
    void __cdecl FrontendArena08A(void* out, void* p) { FrontendArenaGet(out, p, kArena08, 0); }
    void __cdecl FrontendArena08B(void* out, void* p) { FrontendArenaGet(out, p, kArena08, 1); }

    void InstallArenaOverride(const ArenaBuild& build,
                              const void* gameHook,
                              const void* frontendHookA,
                              const void* frontendHookB)
    {
        gArenaTeam = GetPrivateProfileIntA(
            "STARTUP", "ARENA_TEAMNUM", -1, ".\\main.ini");
        gArenaForce = GetPrivateProfileIntA(
            "STARTUP", "ARENA_FORCE", 0, ".\\main.ini") != 0;
        gArenaTrace = GetPrivateProfileIntA(
            "STARTUP", "ARENA_TRACE", 0, ".\\main.ini") != 0;
        if (gArenaTeam < 0 && !gArenaTrace)
            return;

        unsigned gameCount = 0;
        for (size_t i = 0; i < build.gameCallCount; ++i)
            gameCount += WriteRelativeCall(build.gameGetVenueCalls[i],
                build.getVenueTeamId, gameHook) ? 1u : 0u;

        // Keep EA's frontend selection synchronized whenever arena overrides
        // are configured. Gameplay still has its existing fallback hook.
        unsigned feCount = 0;
        feCount += WriteFrontendCallback(
            build.frontendRegistration[0], build.frontendGetter[0],
            frontendHookA) ? 1u : 0u;
        feCount += WriteFrontendCallback(
            build.frontendRegistration[1], build.frontendGetter[1],
            frontendHookB) ? 1u : 0u;

        char s[220];
        sprintf_s(s, sizeof(s),
            "[StartupTeams] %s arena hooks: gameplay=%u/%zu FE=%u/2 requested=%d force=%d trace=%d\n",
            build.tag, gameCount, build.gameCallCount, feCount,
            gArenaTeam, gArenaForce ? 1 : 0,
            gArenaTrace ? 1 : 0);
        OutputDebugStringA(s);
    }
    // Starting-five diagnostic file: written alongside .\\main.ini (the
    // game's current working directory), rather than to a debugger only.
    // Always emit a one-line startup marker, even if INI parsing fails, so the
    // presence/absence of this file distinguishes a missing hook installation
    // from an incorrect INI path.
    void StartingFiveLog(const char* line, bool forceToFile = false)
    {
        OutputDebugStringA(line);
        if (!gStartingFiveTrace && !forceToFile)
            return;
        HANDLE f = CreateFileA(".\\StartupTeams.log", FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return;
        DWORD written = 0;
        const DWORD len = static_cast<DWORD>(strlen(line));
        WriteFile(f, line, len, &written, nullptr);
        CloseHandle(f);
    }

    // Avoid automatically undoing manual lineup changes on future getters.
    // Identity is team-object pointer + team number. Reset the slot when a
    // new native initialization begins with an uninitialized live array.
    struct StartingFiveAppliedRecord { void* team; int teamNumber; };
    StartingFiveAppliedRecord gAppliedLineups[32] = {};
    unsigned gAppliedLineupCount = 0;
    unsigned gStartingFiveHookCalls = 0;

    bool StartingFiveWasApplied(void* team, int teamNumber)
    {
        for (unsigned i = 0; i < gAppliedLineupCount; ++i)
            if (gAppliedLineups[i].team == team &&
                gAppliedLineups[i].teamNumber == teamNumber)
                return true;
        return false;
    }

    void StartingFiveClearApplied(void* team)
    {
        for (unsigned i = 0; i < gAppliedLineupCount;)
        {
            if (gAppliedLineups[i].team == team)
                gAppliedLineups[i] = gAppliedLineups[--gAppliedLineupCount];
            else
                ++i;
        }
    }

    void StartingFiveMarkApplied(void* team, int teamNumber)
    {
        if (StartingFiveWasApplied(team, teamNumber))
            return;
        if (gAppliedLineupCount < 32)
            gAppliedLineups[gAppliedLineupCount++] = {team, teamNumber};
    }

    // The numeric positions here index EA's *native default 12-player array*;
    // they have not yet been proven equal to DBF ROSTERPOS on every roster.
    void ReadStartingFiveSelection(const char* key, StartingFiveSelection& output)
    {
        output = StartingFiveSelection{};
        char value[128] = {};
        GetPrivateProfileStringA("STARTUP", key, "", value,
            static_cast<DWORD>(sizeof(value)), ".\\main.ini");
        if (!value[0])
            return;

        const char* cursor = value;
        bool seen[12] = {};
        int parsed[5] = {};
        bool valid = true;
        for (int i = 0; i < 5; ++i)
        {
            while (*cursor == ' ' || *cursor == '\t') ++cursor;
            if (*cursor < '0' || *cursor > '9') { valid = false; break; }
            char* end = nullptr;
            const long v = std::strtol(cursor, &end, 10);
            if (end == cursor || v < 0 || v >= 12 || seen[v])
            { valid = false; break; }
            parsed[i] = static_cast<int>(v);
            seen[v] = true;
            cursor = end;
            while (*cursor == ' ' || *cursor == '\t') ++cursor;
            if (i < 4)
            {
                if (*cursor != ',') { valid = false; break; }
                ++cursor;
            }
            else if (*cursor != '\0') valid = false;
        }
        if (!valid)
        {
            char msg[250];
            sprintf_s(msg, sizeof(msg),
                "[StartupTeams] invalid %s='%s' (requires five distinct positions 0..11)\r\n",
                key, value);
            StartingFiveLog(msg, true);
            return;
        }
        output.configured = true;
        for (int i = 0; i < 5; ++i)
            output.rosterPos[i] = parsed[i];
    }

    // Report why an initialization was skipped, capped to avoid flooding.
    void StartingFiveStatus(const StartingFiveBuild& build, void* team,
        int actualTeam, int expectedTeam, int side, int count,
        int current, int defaults, const char* status)
    {
        if (!gStartingFiveTrace || ++gStartingFiveHookCalls > 120)
            return;
        char msg[320];
        sprintf_s(msg, sizeof(msg),
            "[StartupTeams] %s lineup: %s ptr=%p side=%d team=%d expected=%d "
            "count=%d live0=%d default0=%d\r\n",
            build.tag, status, team, side, actualTeam, expectedTeam,
            count, current, defaults);
        StartingFiveLog(msg);
    }

    void ApplyStartingFiveAfterInitialization(void* team,
                                              const StartingFiveBuild& build)
    {
        if (!team) return;
        auto* bytes = static_cast<uint8_t*>(team);
        const int side = *(bytes + 0xA0);
        const int actualTeam = *reinterpret_cast<const int*>(bytes + 0xA8);
        const int count = *reinterpret_cast<const int*>(bytes + 0xA4);
        const int live0 = *reinterpret_cast<const int*>(bytes + 0x08);
        const int default0 = *reinterpret_cast<const int*>(bytes + 0x38);
        if (side > 1)
        {
            StartingFiveStatus(build, team, actualTeam, -1, side, count,
                live0, default0, "invalid-side");
            return;
        }
        const int expectedTeam = side ? gHomeTeam : gAwayTeam;
        const StartingFiveSelection& cached = side ? gHomeStartingFive : gAwayStartingFive;
        const char* configKey = side ? "HOME_STARTING5" : "AWAY_STARTING5";

        // In the NBA06 runtime the INI parser reports home=1, away=1 during
        // installation, but the first native lineup hook later observes
        // configured=false on both cached structs. The writer has not yet
        // been identified; keep an independent, function-local recovery cache.
        // This avoids repeating INI file I/O and log spam on later getters.
        static StartingFiveSelection recoveredBySide[2] = {
            {false, {-1, -1, -1, -1, -1}},
            {false, {-1, -1, -1, -1, -1}}
        };
        static bool recoveryReported[2] = {false, false};
        const StartingFiveSelection* selected = &cached;
        if (!cached.configured)
        {
            StartingFiveSelection& recovered = recoveredBySide[side];
            if (!recovered.configured)
                ReadStartingFiveSelection(configKey, recovered);
            if (recovered.configured)
            {
                selected = &recovered;
                if (gStartingFiveTrace && !recoveryReported[side])
                {
                    recoveryReported[side] = true;
                    char msg[320];
                    sprintf_s(msg, sizeof(msg),
                        "[StartupTeams] %s lineup: CONFIG_RECOVERED key=%s "
                        "side=%d team=%d cachedHome=%d cachedAway=%d "
                        "positions=%d,%d,%d,%d,%d\r\n",
                        build.tag, configKey, side, actualTeam,
                        gHomeStartingFive.configured ? 1 : 0,
                        gAwayStartingFive.configured ? 1 : 0,
                        recovered.rosterPos[0], recovered.rosterPos[1],
                        recovered.rosterPos[2], recovered.rosterPos[3],
                        recovered.rosterPos[4]);
                    StartingFiveLog(msg);
                }
            }
        }
        if (!selected->configured)
        {
            StartingFiveStatus(build, team, actualTeam, expectedTeam, side,
                count, live0, default0, "side-not-configured-after-reload");
            return;
        }
        if (expectedTeam >= 0 && actualTeam != expectedTeam)
        {
            StartingFiveStatus(build, team, actualTeam, expectedTeam, side,
                count, live0, default0, "team-mismatch");
            return;
        }
        if (count < 5 || count > 12)
        {
            StartingFiveStatus(build, team, actualTeam, expectedTeam, side,
                count, live0, default0, "invalid-roster-count");
            return;
        }
        if (StartingFiveWasApplied(team, actualTeam))
        {
            StartingFiveStatus(build, team, actualTeam, expectedTeam, side,
                count, live0, default0, "previously-applied");
            return;
        }

        auto* live = reinterpret_cast<int32_t*>(bytes + 0x08);
        const auto* defaults = reinterpret_cast<const int32_t*>(bytes + 0x38);
        bool selectedPosition[12] = {};
        for (int i = 0; i < 5; ++i)
        {
            const int position = selected->rosterPos[i];
            if (position >= count || defaults[position] < 0)
            {
                StartingFiveStatus(build, team, actualTeam, expectedTeam, side,
                    count, live0, default0, "requested-position-missing");
                return;
            }
            selectedPosition[position] = true;
        }
        for (int i = 0; i < count; ++i)
        {
            if (defaults[i] < 0 || live[i] != defaults[i])
            {
                StartingFiveStatus(build, team, actualTeam, expectedTeam, side,
                    count, live0, default0, "lineup-already-modified");
                return;
            }
            for (int j = i + 1; j < count; ++j)
                if (defaults[i] == defaults[j])
                {
                    StartingFiveStatus(build, team, actualTeam, expectedTeam, side,
                        count, live0, default0, "duplicate-native-player");
                    return;
                }
        }

        int32_t reordered[12] = {};
        int n = 0;
        for (int i = 0; i < 5; ++i)
            reordered[n++] = defaults[selected->rosterPos[i]];
        for (int i = 0; i < count; ++i)
            if (!selectedPosition[i]) reordered[n++] = defaults[i];
        if (n != count) return;
        for (int i = 0; i < count; ++i)
            live[i] = reordered[i];
        StartingFiveMarkApplied(team, actualTeam);

        char msg[280];
        sprintf_s(msg, sizeof(msg),
            "[StartupTeams] %s starting five APPLIED side=%s team=%d "
            "positions=%d,%d,%d,%d,%d count=%d ptr=%p "
            "nativePlayers=%d,%d,%d,%d,%d\r\n",
            build.tag, side ? "home" : "away", actualTeam,
            selected->rosterPos[0], selected->rosterPos[1],
            selected->rosterPos[2], selected->rosterPos[3],
            selected->rosterPos[4], count, team,
            reordered[0], reordered[1], reordered[2],
            reordered[3], reordered[4]);
        StartingFiveLog(msg);
    }

    void InitializeStartingFive(void* team, const StartingFiveBuild& build)
    {
        if (!team)
        {
            StartingFiveStatus(build, team, -1, -1, -1, -1, -1, -1,
                "null-team");
            return;
        }
        const int32_t before = *reinterpret_cast<const int32_t*>(
            static_cast<const uint8_t*>(team) + 0x08);
        if (before == -1)
            StartingFiveClearApplied(team);
        reinterpret_cast<void(__thiscall*)(void*)>(build.initializer)(team);
        // Unlike the first implementation, attempt a one-time override even
        // if another native caller initialized the lineup earlier.
        // ApplyStartingFiveAfterInitialization itself checks live==default.
        ApplyStartingFiveAfterInitialization(team, build);
    }

    void __fastcall StartingFive05(void* team, void*) { InitializeStartingFive(team, kStarting05); }
    void __fastcall StartingFive06(void* team, void*) { InitializeStartingFive(team, kStarting06); }
    void __fastcall StartingFive07(void* team, void*) { InitializeStartingFive(team, kStarting07); }
    void __fastcall StartingFive08(void* team, void*) { InitializeStartingFive(team, kStarting08); }

    void InstallStartingFive(const StartingFiveBuild& build, const void* replacement)
    {
        ReadStartingFiveSelection("HOME_STARTING5", gHomeStartingFive);
        ReadStartingFiveSelection("AWAY_STARTING5", gAwayStartingFive);
        gStartingFiveTrace = GetPrivateProfileIntA(
            "STARTUP", "STARTING5_TRACE", 0, ".\\main.ini") != 0;

        char msg[260];
        sprintf_s(msg, sizeof(msg),
            "[StartupTeams] %s starting-five INI: trace=%d home=%d away=%d "
            "homeTeam=%d awayTeam=%d\r\n",
            build.tag, gStartingFiveTrace ? 1 : 0,
            gHomeStartingFive.configured ? 1 : 0,
            gAwayStartingFive.configured ? 1 : 0,
            gHomeTeam, gAwayTeam);
        StartingFiveLog(msg, true);
        if (!gHomeStartingFive.configured && !gAwayStartingFive.configured)
            return;

        unsigned installed = 0;
        for (size_t i = 0; i < build.count; ++i)
        {
            if (WriteRelativeCall(build.callers[i], build.initializer, replacement))
                ++installed;
            else if (gStartingFiveTrace)
            {
                sprintf_s(msg, sizeof(msg),
                    "[StartupTeams] %s lineup hook failed at %08X\r\n",
                    build.tag, static_cast<unsigned>(build.callers[i]));
                StartingFiveLog(msg);
            }
        }
        sprintf_s(msg, sizeof(msg),
            "[StartupTeams] %s starting five hooks: %u/%u\r\n",
            build.tag, installed, static_cast<unsigned>(build.count));
        StartingFiveLog(msg, true);
    }

    // Hook only the FE GetJerseyVal -> EnsureDefaultJersey callsite.
    // Calling convention: original __thiscall(ECX=TeamSelection*), no arguments.
    void __fastcall FrontendEnsureDefaultJersey(void* team, void*)
    {
        int before = team ? *reinterpret_cast<int*>(static_cast<uint8_t*>(team) + 0x98) : 0;
        reinterpret_cast<EnsureDefaultJerseyFn>(kEnsureDefaultJersey)(team);
        ApplyInitialJersey(team, before, "NBA06");
    }

    // Both scene call sites normally call __thiscall GetJersey(ECX=participants,
    // [ESP+4]=side), which pops the side argument. __fastcall with the unused
    // EDX parameter preserves that same ABI at the patched call site.
    int __fastcall SceneGetJerseyInitial(void* participants, void*, int side)
    {
        void* team = nullptr;
        int before = 0;
        if (participants && (side == 0 || side == 1))
        {
            auto* base = static_cast<uint8_t*>(participants);
            team = *reinterpret_cast<void**>(base + 0x08 + side * 4);
            if (team)
                before = *reinterpret_cast<int*>(static_cast<uint8_t*>(team) + 0x98);
        }

        int result = reinterpret_cast<GetJerseyFn>(kGetJersey)(participants, side);
        if (ApplyInitialJersey(team, before, "NBA06"))
            result = *reinterpret_cast<int*>(static_cast<uint8_t*>(team) + 0x98);
        return result;
    }

    // NBA Live 05 initializer: __thiscall(ECX=TeamSelection*), no arguments.
    // The original performs the standard DB-based lookup, then our configured
    // value replaces that default only when the entry was unselected (-1).
    void __fastcall EnsureDefaultJersey05(void* team, void*)
    {
        const int before = team
            ? *reinterpret_cast<const int*>(static_cast<const uint8_t*>(team) + 0x98)
            : 0;
        reinterpret_cast<EnsureDefaultJerseyFn>(kEnsureDefaultJersey05)(team);
        ApplyInitialJersey(team, before, "NBA05");
    }

    // The 07 default initializer has __thiscall ECX=team with no stack args.
    // A fastcall wrapper accepts ECX and the unused EDX without changing stack ABI.
    void __fastcall EnsureDefaultJersey07(void* team, void*)
    {
        const int before = team
            ? *reinterpret_cast<const int*>(static_cast<const uint8_t*>(team) + 0x98)
            : 0;
        reinterpret_cast<EnsureDefaultJerseyFn>(kEnsureDefaultJersey07)(team);
        ApplyInitialJersey(team, before, "NBA07");
    }

    // 08 has the same __thiscall ECX=team, no-argument initializer ABI.
    // Only assign our default when the field was -1 on entry. The original
    // engine initializer still runs, preserving its normal side effects.
    void __fastcall EnsureDefaultJersey08(void* team, void*)
    {
        const int before = team
            ? *reinterpret_cast<const int*>(static_cast<const uint8_t*>(team) + 0x98)
            : 0;
        reinterpret_cast<EnsureDefaultJerseyFn>(kEnsureDefaultJersey08)(team);
        ApplyInitialJersey(team, before, "NBA08");
    }

    void Install05JerseyOverrides()
    {
        gHomeJersey = GetPrivateProfileIntA("STARTUP", "HOME_JERSEY_ID", -1, ".\\main.ini");
        gAwayJersey = GetPrivateProfileIntA("STARTUP", "AWAY_JERSEY_ID", -1, ".\\main.ini");
        if (gHomeJersey < 0 && gAwayJersey < 0)
            return;

        unsigned installed = 0;
        for (uintptr_t site : kJerseyEnsureCalls05)
            installed += WriteRelativeCall(site, kEnsureDefaultJersey05,
                                           reinterpret_cast<const void*>(&EnsureDefaultJersey05)) ? 1u : 0u;

        char message[180];
        sprintf_s(message, sizeof(message),
                  "[StartupTeams] NBA05 initial jersey hooks: %u/%zu (IDs: %d, %d)\n",
                  installed, sizeof(kJerseyEnsureCalls05) / sizeof(kJerseyEnsureCalls05[0]),
                  gHomeJersey, gAwayJersey);
        OutputDebugStringA(message);
    }

    void Install08JerseyOverrides()
    {
        gHomeJersey = GetPrivateProfileIntA("STARTUP", "HOME_JERSEY_ID", -1, ".\\main.ini");
        gAwayJersey = GetPrivateProfileIntA("STARTUP", "AWAY_JERSEY_ID", -1, ".\\main.ini");
        if (gHomeJersey < 0 && gAwayJersey < 0)
            return;

        unsigned installed = 0;
        for (uintptr_t site : kJerseyEnsureCalls08)
            installed += WriteRelativeCall(site, kEnsureDefaultJersey08,
                                           reinterpret_cast<const void*>(&EnsureDefaultJersey08)) ? 1u : 0u;

        char message[180];
        sprintf_s(message, sizeof(message),
                  "[StartupTeams] NBA08 initial jersey hooks: %u/%zu (IDs: %d, %d)\n",
                  installed, sizeof(kJerseyEnsureCalls08) / sizeof(kJerseyEnsureCalls08[0]),
                  gHomeJersey, gAwayJersey);
        OutputDebugStringA(message);
    }

    void Install07JerseyOverrides()
    {
        gHomeJersey = GetPrivateProfileIntA("STARTUP", "HOME_JERSEY_ID", -1, ".\\main.ini");
        gAwayJersey = GetPrivateProfileIntA("STARTUP", "AWAY_JERSEY_ID", -1, ".\\main.ini");
        if (gHomeJersey < 0 && gAwayJersey < 0)
            return;

        unsigned installed = 0;
        for (uintptr_t site : kJerseyEnsureCalls07)
            installed += WriteRelativeCall(site, kEnsureDefaultJersey07,
                                           reinterpret_cast<const void*>(&EnsureDefaultJersey07)) ? 1u : 0u;

        char message[180];
        sprintf_s(message, sizeof(message),
                  "[StartupTeams] NBA07 initial jersey hooks: %u/%zu (IDs: %d, %d)\n",
                  installed, sizeof(kJerseyEnsureCalls07) / sizeof(kJerseyEnsureCalls07[0]),
                  gHomeJersey, gAwayJersey);
        OutputDebugStringA(message);
    }

    void SetStartupTeam(unsigned int address, int teamNum)
    {
        char query[32];
        sprintf_s(query, sizeof(query), " TEAMNUM = %d ", teamNum);
        for (size_t i = 0; i <= strlen(query); ++i)
            patch::SetChar(address + static_cast<unsigned int>(i), query[i]);
    }

    void Install06JerseyOverrides()
    {
        gHomeJersey = GetPrivateProfileIntA("STARTUP", "HOME_JERSEY_ID", -1, ".\\main.ini");
        gAwayJersey = GetPrivateProfileIntA("STARTUP", "AWAY_JERSEY_ID", -1, ".\\main.ini");

        if (gHomeJersey < 0 && gAwayJersey < 0)
            return;

        const bool frontendInstalled = WriteRelativeCall(
            kFrontendEnsureCall, kEnsureDefaultJersey, reinterpret_cast<const void*>(&FrontendEnsureDefaultJersey));
        const bool homeInstalled = WriteRelativeCall(
            kHomeSceneGetJerseyCall, kGetJersey, reinterpret_cast<const void*>(&SceneGetJerseyInitial));
        const bool awayInstalled = WriteRelativeCall(
            kAwaySceneGetJerseyCall, kGetJersey, reinterpret_cast<const void*>(&SceneGetJerseyInitial));
        char message[200];
        sprintf_s(message, sizeof(message),
                  "[StartupTeams] NBA06 initial jersey hooks: FE=%d scene-home=%d scene-away=%d (IDs: %d, %d)\n",
                  frontendInstalled ? 1 : 0, homeInstalled ? 1 : 0,
                  awayInstalled ? 1 : 0, gHomeJersey, gAwayJersey);
        OutputDebugStringA(message);
    }
}

void InitializeStartupTeams()
{
    // Diagnostic startup marker: if missing, this module did not execute
    // or the process cannot write into its current working directory.
    char cwd[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, cwd);
    char startupMessage[MAX_PATH + 180];
    sprintf_s(startupMessage, sizeof(startupMessage),
        "[StartupTeams] InitializeStartupTeams entered; cwd=%s; entry=%08X\r\n",
        cwd, static_cast<unsigned>(FM::GetEntryPoint()));
    StartingFiveLog(startupMessage, true);
    gHomeTeam = GetPrivateProfileIntA("STARTUP", "HOME_TEAMNUM", -1, ".\\main.ini");
    gAwayTeam = GetPrivateProfileIntA("STARTUP", "AWAY_TEAMNUM", -1, ".\\main.ini");

    unsigned int homeAddress = 0;
    unsigned int awayAddress = 0;
    bool nba05 = false;
    bool nba06 = false;
    bool nba07 = false;
    bool nba08 = false;

    switch (FM::GetEntryPoint())
    {
    case 0xCD8005: // NBA Live 2005 1.0 NOCD
        nba05 = true;
        homeAddress = 0x00B41010;
        awayAddress = 0x00B41028;
        break;
    case 0x40109F:
        if (patch::GetFloat(0xBD832C) == 1.3333334f)
        {
            nba06 = true;
            homeAddress = 0x00BAC954;
            awayAddress = 0x00BAC96C;
        }
        else if (patch::GetFloat(0xBBBC3C) == 1.3333334f)
        {
            nba07 = true;
            homeAddress = 0x00BC98C0;
            awayAddress = 0x00BC98D4;
        }
        else if (patch::GetFloat(0xC3DF84) == 1.3333334f)
        {
            nba08 = true;
            homeAddress = 0x00C5496C;
            awayAddress = 0x00C54984;
        }
        break;
    }

    if (!homeAddress || !awayAddress)
        return;

    if (gHomeTeam >= 0)
        SetStartupTeam(homeAddress, gHomeTeam);
    if (gAwayTeam >= 0)
        SetStartupTeam(awayAddress, gAwayTeam);

    if (nba05)
    {
        Install05JerseyOverrides();
        InstallStartingFive(kStarting05, reinterpret_cast<const void*>(&StartingFive05));
        InstallArenaOverride(kArena05, reinterpret_cast<const void*>(&ResolveArena05),
            reinterpret_cast<const void*>(&FrontendArena05A),
            reinterpret_cast<const void*>(&FrontendArena05B));
    }
    else if (nba06)
    {
        Install06JerseyOverrides();
        InstallStartingFive(kStarting06, reinterpret_cast<const void*>(&StartingFive06));
        InstallArenaOverride(kArena06, reinterpret_cast<const void*>(&ResolveArena06),
            reinterpret_cast<const void*>(&FrontendArena06A),
            reinterpret_cast<const void*>(&FrontendArena06B));
    }
    else if (nba07)
    {
        Install07JerseyOverrides();
        InstallStartingFive(kStarting07, reinterpret_cast<const void*>(&StartingFive07));
        InstallArenaOverride(kArena07, reinterpret_cast<const void*>(&ResolveArena07),
            reinterpret_cast<const void*>(&FrontendArena07A),
            reinterpret_cast<const void*>(&FrontendArena07B));
    }
    else if (nba08)
    {
        Install08JerseyOverrides();
        InstallStartingFive(kStarting08, reinterpret_cast<const void*>(&StartingFive08));
        InstallArenaOverride(kArena08, reinterpret_cast<const void*>(&ResolveArena08),
            reinterpret_cast<const void*>(&FrontendArena08A),
            reinterpret_cast<const void*>(&FrontendArena08B));
    }
}

namespace practicesetup {
constexpr const char* kIniFile = ".\\main.ini";
    bool IsReadableAddress(const void* ptr, size_t size) {
        if (!ptr || size == 0)
            return false;

        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        const DWORD protect = mbi.Protect & 0xFF;
        if (protect == PAGE_NOACCESS || protect == PAGE_GUARD)
            return false;

        const auto start =
            reinterpret_cast<uintptr_t>(ptr);
        const auto end =
            start + size;
        const auto regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) +
            mbi.RegionSize;

        return end >= start && end <= regionEnd;
    }

PracticeProbe06 g_practice06;

bool PracticeApplyJersey06(const char* phase, void* participant) {
        if (!g_practice06.enabled || g_practice06.jerseyId < 0)
            return false;
        char message[352] = {};
        if (!IsReadableAddress(participant, 0x14)) {
            sprintf_s(message, sizeof(message),
                "[PracticeSetup] %s jersey override SKIPPED: selection object unreadable.\n",
                phase);
            PracticeLog06(message);
            return false;
        }
        const auto* bytes = static_cast<const unsigned char*>(participant);
        int actualPlayer = -1;
        int previousJersey = -1;
        memcpy(&actualPlayer, bytes + 0x08, sizeof(actualPlayer));
        memcpy(&previousJersey, bytes + 0x0C, sizeof(previousJersey));
        // Avoid changing a selection object belonging to another player.
        if (actualPlayer != g_practice06.dbId) {
            sprintf_s(message, sizeof(message),
                "[PracticeSetup] %s jersey override SKIPPED: playerId=%d expected=%d.\n",
                phase, actualPlayer, g_practice06.dbId);
            PracticeLog06(message);
            return false;
        }
        MEMORY_BASIC_INFORMATION info = {};
        auto* field = static_cast<unsigned char*>(participant) + 0x0C;
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (VirtualQuery(field, &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            !(info.Protect & writable)) {
            sprintf_s(message, sizeof(message),
                "[PracticeSetup] %s jersey override SKIPPED: +0C is not writable.\n", phase);
            PracticeLog06(message);
            return false;
        }
        const int requested = g_practice06.jerseyId;
        memcpy(field, &requested, sizeof(requested));
        int readback = -1;
        memcpy(&readback, field, sizeof(readback));
        sprintf_s(message, sizeof(message),
            "[PracticeSetup] %s jersey override: playerId=%d jersey %d->%d readback=%d match=%s\n",
            phase, actualPlayer, previousJersey, requested, readback,
            readback == requested ? "yes" : "NO");
        PracticeLog06(message);
        return readback == requested;
    }

void __fastcall PracticePlayerCallHook06(void* feObject, void*, int originalNativeId) {
        // ECX = Practice FE object, one stack integer, RET 4.
        // Captured from the original game CALL: this identifies which native
        // frontend/session constructor or reset path reached the setter.
        const void* callerRet = _ReturnAddress();
        // This call runs during construction, the APT SetPlayer action, and
        // SetPracticeKind's player reset. Apply the same selected player after
        // each of these original FE actions to avoid losing the override.
        const int appliedId = (g_practice06.enabled && g_practice06.kind == 0 &&
                               g_practice06.dbId >= 0)
            ? g_practice06.dbId : originalNativeId;
        reinterpret_cast<PracticeSetPlayerFn06>(kPracticeNativePlayerSetter06)(
            feObject, appliedId);
        // The native call may reset jersey to its preferred Practice uniform.
        PracticeApplyJersey06("FE after SetPlayer", feObject);
        ++g_practice06.frontendCalls;

        int player = -1;
        int jersey = -1;
        int kind = -1;
        char message[400] = {};
        if (IsReadableAddress(feObject, 0x14)) {
            const auto* p = static_cast<const unsigned char*>(feObject);
            memcpy(&player, p + 0x08, sizeof(player));
            memcpy(&jersey, p + 0x0C, sizeof(jersey));
            memcpy(&kind, p + 0x10, sizeof(kind));
            sprintf_s(message, sizeof(message),
                "[PracticeSetup] FE player refresh #%lu callerRet=%p object=%p inputPlayerId=%d appliedPlayerId=%d readbackPlayer=%d jersey=%d kind=%d match=%s\n",
                g_practice06.frontendCalls, callerRet, feObject, originalNativeId,
                appliedId, player, jersey, kind,
                player == appliedId ? "yes" : "NO");
        } else {
            sprintf_s(message, sizeof(message),
                "[PracticeSetup] FE player refresh #%lu callerRet=%p object=%p inputPlayerId=%d appliedPlayerId=%d; object unreadable\n",
                g_practice06.frontendCalls, callerRet, feObject, originalNativeId,
                appliedId);
        }
        g_practice06.lastFePlayer = player;
        g_practice06.lastFeJersey = jersey;
        g_practice06.lastFeKind = kind;
        PracticeLog06(message);
    }

void __fastcall PracticeBootModeCallHook06(void* feModeManager, void*, int originalMode) {
        const bool override = g_practice06.enabled && g_practice06.autoSelectMode &&
            g_practice06.kind == 0 && g_practice06.dbId >= 0 &&
            originalMode == kExhibitionMode06;
        const int selectedMode = override ? kIndividualPracticeMode06 : originalMode;
        char message[384] = {};
        sprintf_s(message, sizeof(message),
            "[PracticeSetup] FE initial mode request: manager=%p original=%d selected=%d override=%s\n",
            feModeManager, originalMode, selectedMode, override ? "yes" : "no");
        PracticeLog06(message);

        // Run EA's original mode/session-object construction on its own thread.
        reinterpret_cast<PracticeBootModeFn06>(kNativeModeConstructor06)(
            feModeManager, selectedMode);

        if (IsReadableAddress(feModeManager, 0x14)) {
            const auto* manager = static_cast<const unsigned char*>(feModeManager);
            int activeMode = -1, pendingMode = -1;
            void* session = nullptr;
            memcpy(&activeMode, manager + 0x08, sizeof(activeMode));
            memcpy(&pendingMode, manager + 0x0C, sizeof(pendingMode));
            memcpy(&session, manager + 0x10, sizeof(session));
            sprintf_s(message, sizeof(message),
                "[PracticeSetup] FE initial mode readback: current=%d pending=%d session=%p expected=%d match=%s\n",
                activeMode, pendingMode, session, selectedMode,
                (activeMode == selectedMode && session) ? "yes" : "NO");
            PracticeLog06(message);
        }
        else {
            PracticeLog06("[PracticeSetup] FE initial mode readback: manager unreadable.\n");
        }
    }

bool InstallPracticeBootModeHook06() {
        const uintptr_t site = kPracticeBootModeCall06;
        if (!IsReadableAddress(reinterpret_cast<const void*>(site), 5)) {
            PracticeLog06("[PracticeSetup] FE initial-mode hook skipped: unreadable call site.\n");
            return false;
        }
        const auto* instruction = reinterpret_cast<const unsigned char*>(site);
        if (instruction[0] != 0xE8 ||
            *reinterpret_cast<const unsigned char*>(site - 2) != 0x6A ||
            *reinterpret_cast<const unsigned char*>(site - 1) != 0x05) {
            PracticeLog06("[PracticeSetup] FE initial-mode hook skipped: opcode signature mismatch.\n");
            return false;
        }
        int32_t originalDisplacement = 0;
        memcpy(&originalDisplacement, instruction + 1, sizeof(originalDisplacement));
        if (static_cast<intptr_t>(site + 5) + originalDisplacement !=
            static_cast<intptr_t>(kNativeModeConstructor06)) {
            PracticeLog06("[PracticeSetup] FE initial-mode hook skipped: original target mismatch.\n");
            return false;
        }
        const intptr_t relative = reinterpret_cast<intptr_t>(&PracticeBootModeCallHook06) -
            static_cast<intptr_t>(site + 5);
        if (relative < INT32_MIN || relative > INT32_MAX) {
            PracticeLog06("[PracticeSetup] FE initial-mode hook skipped: rel32 out of range.\n");
            return false;
        }
        DWORD previousProtection = 0;
        auto* writable = reinterpret_cast<unsigned char*>(site);
        if (!VirtualProtect(writable, 5, PAGE_EXECUTE_READWRITE, &previousProtection)) {
            PracticeLog06("[PracticeSetup] FE initial-mode hook skipped: VirtualProtect failed.\n");
            return false;
        }
        const int32_t displacement = static_cast<int32_t>(relative);
        memcpy(writable + 1, &displacement, sizeof(displacement));
        FlushInstructionCache(GetCurrentProcess(), writable, 5);
        DWORD unused = 0;
        VirtualProtect(writable, 5, previousProtection, &unused);
        PracticeLog06("[PracticeSetup] FE initial-mode override installed (original Exhibition 5 -> Practice 10).\n");
        return true;
    }

unsigned char __fastcall PracticePrepareCallHook06(
        void* runtimeSetup, void*, void* participant, void* stagingRecord)
    {
        // Native __thiscall: ECX=runtimeSetup; two stack pointers; RET 8.
        // The compiler's __fastcall wrapper likewise cleans two stack args.
        PracticeDescribeParticipant06("BE preparation BEFORE", participant);
        // Covers FE-bypassed startup and any native jersey reset performed
        // after the FE hook. EA's BE preparation consumes this selected ID.
        PracticeApplyJersey06("BE before preparation", participant);
        const unsigned char result = reinterpret_cast<PracticePrepareFn06>(
            kPracticePrepareTarget06)(runtimeSetup, participant, stagingRecord);
        PracticeDescribeParticipant06("BE preparation AFTER", participant);
        if (IsReadableAddress(stagingRecord, 0x14)) {
            const auto* bytes = static_cast<const unsigned char*>(stagingRecord);
            unsigned int w[3] = {};
            memcpy(w, bytes + 0x08, sizeof(w));
            char message[320] = {};
            sprintf_s(message, sizeof(message),
                "[PracticeSetup] BE staging record=%p +08=%u +0C=%u +10=%u nativeResult=%u (raw, not assumed player IDs)\n",
                stagingRecord, w[0], w[1], w[2], static_cast<unsigned>(result));
            PracticeLog06(message);
        }
        return result;
    }

bool InstallPracticePrepareHook06() {
        const uintptr_t site = kPracticePrepareCallsite06;
        if (!IsReadableAddress(reinterpret_cast<const void*>(site), 5)) {
            PracticeLog06("[PracticeSetup] BE preparation hook skipped: unreadable callsite.\n");
            return false;
        }
        const auto* bytes = reinterpret_cast<const unsigned char*>(site);
        if (bytes[0] != 0xE8) {
            PracticeLog06("[PracticeSetup] BE preparation hook skipped: opcode mismatch.\n");
            return false;
        }
        int32_t oldRel = 0;
        memcpy(&oldRel, bytes + 1, sizeof(oldRel));
        if (site + 5 + static_cast<intptr_t>(oldRel) != kPracticePrepareTarget06) {
            PracticeLog06("[PracticeSetup] BE preparation hook skipped: target mismatch.\n");
            return false;
        }
        const intptr_t rel = reinterpret_cast<intptr_t>(&PracticePrepareCallHook06) -
            static_cast<intptr_t>(site + 5);
        if (rel < INT32_MIN || rel > INT32_MAX) {
            PracticeLog06("[PracticeSetup] BE preparation hook skipped: wrapper out of range.\n");
            return false;
        }
        DWORD originalProtection = 0;
        auto* writable = reinterpret_cast<unsigned char*>(site);
        if (!VirtualProtect(writable, 5, PAGE_EXECUTE_READWRITE, &originalProtection)) {
            PracticeLog06("[PracticeSetup] BE preparation hook skipped: VirtualProtect failed.\n");
            return false;
        }
        const int32_t newRel = static_cast<int32_t>(rel);
        memcpy(writable + 1, &newRel, sizeof(newRel));
        FlushInstructionCache(GetCurrentProcess(), writable, 5);
        DWORD ignored = 0;
        VirtualProtect(writable, 5, originalProtection, &ignored);
        PracticeLog06("[PracticeSetup] BE preparation diagnostic installed at 005EF462 (execution not established).\n");
        return true;
    }

bool VerifyPracticeCall06(uintptr_t site) {
        if (!IsReadableAddress(reinterpret_cast<const void*>(site), 5))
            return false;
        const auto* bytes = reinterpret_cast<const unsigned char*>(site);
        if (bytes[0] != 0xE8)
            return false;
        int32_t displacement = 0;
        memcpy(&displacement, bytes + 1, sizeof(displacement));
        return site + 5 + static_cast<intptr_t>(displacement) ==
            kPracticeNativePlayerSetter06;
    }

bool InstallPracticePlayerCall06(uintptr_t site) {
        const auto target = reinterpret_cast<intptr_t>(&PracticePlayerCallHook06);
        const auto relative = target - static_cast<intptr_t>(site + 5);
        if (relative < INT32_MIN || relative > INT32_MAX)
            return false;
        DWORD previous = 0;
        auto* bytes = reinterpret_cast<unsigned char*>(site);
        if (!VirtualProtect(bytes, 5, PAGE_EXECUTE_READWRITE, &previous))
            return false;
        const int32_t displacement = static_cast<int32_t>(relative);
        memcpy(bytes + 1, &displacement, sizeof(displacement));
        FlushInstructionCache(GetCurrentProcess(), bytes, 5);
        DWORD ignored = 0;
        VirtualProtect(bytes, 5, previous, &ignored);
        return true;
    }

void InitializePracticeProbe06() {
        g_practice06 = {};
        if (FM::GetEntryPoint() != 0x40109F ||
            patch::GetFloat(0xBD832C) != 1.3333334f)
            return; // Not the verified NBA Live 06 binary.

        g_practice06.enabled = GetPrivateProfileIntA(
            "PRACTICE", "PROFILE_ENABLED", 0, kIniFile) == 1;
        if (!g_practice06.enabled)
            return;
        g_practice06.kind = GetPrivateProfileIntA(
            "PRACTICE", "KIND", -1, kIniFile);
        g_practice06.dbId = GetPrivateProfileIntA(
            "PRACTICE", "PLAYER_DB_ID", -1, kIniFile);
        g_practice06.jerseyId = GetPrivateProfileIntA(
            "PRACTICE", "JERSEY_ID", -1, kIniFile);
        g_practice06.useKeyboard = GetPrivateProfileIntA(
            "PRACTICE", "USE_KEYBOARD", 0, kIniFile) == 1;
        g_practice06.autoSelectMode = GetPrivateProfileIntA(
            "PRACTICE", "AUTO_SELECT_MODE", 0, kIniFile) == 1;
        if (g_practice06.kind != 0 || g_practice06.dbId < 0 ||
            g_practice06.jerseyId < -1 || g_practice06.jerseyId > 32767) {
            PracticeLog06("[PracticeSetup] Invalid or unsupported Practice profile; no native hooks installed.\n");
            return;
        }
        char message[400] = {};
        sprintf_s(message, sizeof(message),
            "[PracticeSetup] NBA06 profile parsed: kind=%d playerDbId=%d jerseyId=%d useKeyboard=%d. Practice FE native setter receives PLAYER_DB_ID directly.\n",
            g_practice06.kind, g_practice06.dbId, g_practice06.jerseyId,
            g_practice06.useKeyboard ? 1 : 0);
        PracticeLog06(message);
        PracticeLog06("[PracticeSetup] FE player override enabled; PLAYER_NATIVE_ID is no longer read. JERSEY_ID=-1 keeps EA default.\n");
        if (g_practice06.autoSelectMode)
            PracticeLog06("[PracticeSetup] Experimental FE initial-mode selection requested. This does not automatically enter gameplay.\n");

        // Native direct calls to the Practice player/jersey setter:
        // 0x0055967C = Practice participant constructor (default Wade)
        // 0x00559650 = APT SetPlayer callback
        // 0x005759BC = Practice subtype reset
        // 0x00571277 = separate Practice session participant constructor
        // 0x00536255 = native default-player helper (default Wade)
        // Do not confuse a patched site with proof that its path executes.
        constexpr uintptr_t sites[] = {
            0x0055967C, 0x00559650, 0x005759BC,
            0x00571277, 0x00536255
        };
        for (const auto site : sites) {
            if (!VerifyPracticeCall06(site)) {
                sprintf_s(message, sizeof(message),
                    "[PracticeSetup] Original call mismatch at %08lX; aborting FE hook installation.\n",
                    static_cast<unsigned long>(site));
                PracticeLog06(message);
                return;
            }
        }
        for (const auto site : sites) {
            if (!InstallPracticePlayerCall06(site)) {
                sprintf_s(message, sizeof(message),
                    "[PracticeSetup] FE hook FAILED at %08lX; partial hooks may remain.\n",
                    static_cast<unsigned long>(site));
                PracticeLog06(message);
                return;
            }
        }
        PracticeLog06("[PracticeSetup] Practice setter direct call sites INSTALLED: 5/5 (execution not established).\n");
        // Do not change normal boot behavior unless AUTO_SELECT_MODE=1.
        if (g_practice06.autoSelectMode)
            InstallPracticeBootModeHook06();
        // Trace a second independent native path: actual Practice session preparation.
        // This runs before the Practice scene and player actor setup, if reached.
        InstallPracticePrepareHook06();

        // The backend path is a virtual method, not a direct callsite.
        // Its ABI is __thiscall (ECX + one stack arg, RET 4). Read-only trace.
        const uintptr_t vtableEntry = 0x00BC6C38 + 0x0C;
        auto* entry = reinterpret_cast<void**>(vtableEntry);
        if (!IsReadableAddress(entry, sizeof(void*)) ||
            *entry != reinterpret_cast<void*>(kPracticeSceneStart06)) {
            PracticeLog06("[PracticeSetup] Backend vtable signature mismatch; backend trace disabled.\n");
            return;
        }
        DWORD previous = 0;
        if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &previous)) {
            PracticeLog06("[PracticeSetup] Backend vtable VirtualProtect failed.\n");
            return;
        }
        *entry = reinterpret_cast<void*>(&PracticeSceneCallHook06);
        DWORD ignored = 0;
        VirtualProtect(entry, sizeof(void*), previous, &ignored);
        PracticeLog06("[PracticeSetup] Backend practice-scene virtual callback instrumented.\n");
        PracticeLog06("[PracticeSetup] Direct headless Practice launch NOT enabled; requires FE dispatcher and verified player handoff into gameplay.\n");
    }

}


bool GetGameSetupPostseasonPresentation(
    GameSetupPostseasonPresentation* presentation)
{
    if (!presentation)
        return false;

    std::memset(presentation, 0, sizeof(*presentation));

    // For Live 06, the diagnostic/game-handoff path already captured the
    // actual OptionsSettings pointer. Reuse that exact verified context
    // instead of asking GetCurrentOptions(nullptr), which has no Live 06
    // global resolver.
    void* options = nullptr;
    if (custompaths::gGameVersion == custompaths::GAME_LIVE_06)
        options = custompaths::gLastPlayoffSnapshotOptions;

    const custompaths::PostseasonStage stage =
        custompaths::GetPostseasonStage(options);

    presentation->stage = static_cast<int>(stage);
    presentation->active = stage != custompaths::STAGE_REGULAR;

    if (!presentation->active)
        return true;

    // Explicit launcher-forced series state remains authoritative.
    if (custompaths::gForcedSeriesEnabled)
    {
        presentation->seriesValid = true;
        presentation->seriesGame = custompaths::gForcedSeriesGame;
        presentation->seriesLength = custompaths::gForcedSeriesLength;
        presentation->homeWins = custompaths::gForcedHomeWins;
        presentation->awayWins = custompaths::gForcedAwayWins;
        return true;
    }

    if (custompaths::gGameVersion == custompaths::GAME_LIVE_2005)
    {
        __try
        {
            if (!custompaths::gLive05PlayoffQueryValid)
                return true;

            const unsigned int currentGame =
                static_cast<unsigned int>(
                    custompaths::gLive05PlayoffQueryScheduleId);
            const int nativeSeriesGame =
                static_cast<int>(custompaths::gLive05PlayoffQuerySeriesGame);

            if (currentGame == 0u || currentGame > 15u * 7u ||
                nativeSeriesGame < 1 || nativeSeriesGame > 7)
            {
                return true;
            }

            custompaths::Live08ScheduleGame current = {};
            if (!custompaths::ReadLive05ScheduleGame(currentGame, current))
                return true;

            const short homeTeam =
                custompaths::ReadLive05ScheduleShort(current, 0x2E);
            const short awayTeam =
                custompaths::ReadLive05ScheduleShort(current, 0x36);

            if (homeTeam < 0 || awayTeam < 0 || homeTeam == awayTeam)
                return true;

            unsigned int homeWins = 0;
            unsigned int awayWins = 0;
            unsigned int historyGames = 0;
            bool exactSeriesScore = true;

            // scheduleplayoff.dbf uses the same 15-series interleave: each
            // previous game of the current matchup is exactly 15 IDs earlier.
            unsigned int previousGame = currentGame;
            while (exactSeriesScore && previousGame > 15u)
            {
                previousGame -= 15u;

                custompaths::Live08ScheduleGame prior = {};
                if (!custompaths::ReadLive05ScheduleGame(previousGame, prior))
                {
                    exactSeriesScore = false;
                    break;
                }

                const short priorHome =
                    custompaths::ReadLive05ScheduleShort(prior, 0x2E);
                const short priorHomeScore =
                    custompaths::ReadLive05ScheduleShort(prior, 0x30);
                const short priorAway =
                    custompaths::ReadLive05ScheduleShort(prior, 0x36);
                const short priorAwayScore =
                    custompaths::ReadLive05ScheduleShort(prior, 0x38);

                const bool sameTeams =
                    (priorHome == homeTeam && priorAway == awayTeam) ||
                    (priorHome == awayTeam && priorAway == homeTeam);

                if (prior.bytes[0x2C] == 0 ||
                    !sameTeams ||
                    priorHomeScore == priorAwayScore)
                {
                    exactSeriesScore = false;
                    break;
                }

                const short winner =
                    priorHomeScore > priorAwayScore ? priorHome : priorAway;

                if (winner == homeTeam)
                    ++homeWins;
                else if (winner == awayTeam)
                    ++awayWins;
                else
                {
                    exactSeriesScore = false;
                    break;
                }

                ++historyGames;
            }

            if (historyGames + 1u !=
                static_cast<unsigned int>(nativeSeriesGame))
            {
                exactSeriesScore = false;
            }

            if (!exactSeriesScore)
                return true;

            presentation->seriesValid = true;
            presentation->seriesGame = nativeSeriesGame;
            presentation->seriesLength = 7;
            presentation->homeWins = static_cast<int>(homeWins);
            presentation->awayWins = static_cast<int>(awayWins);

            if (custompaths::gPlayoffStateDebug)
            {
                static int lastLoggedGame05 = -1;
                static int lastLoggedHomeWins05 = -1;
                static int lastLoggedAwayWins05 = -1;
                if (lastLoggedGame05 != nativeSeriesGame ||
                    lastLoggedHomeWins05 != static_cast<int>(homeWins) ||
                    lastLoggedAwayWins05 != static_cast<int>(awayWins))
                {
                    char message[320] = {};
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] PLAYOFF_05_SERIES game=%d "
                        "scheduleId=%u round=%d slot=%d caller=%08X confidence=%d "
                        "homeTeam=%d awayTeam=%d "
                        "homeWins=%u awayWins=%u exact=yes\n",
                        nativeSeriesGame,
                        currentGame,
                        static_cast<int>(custompaths::gLive05PlayoffQueryRound),
                        static_cast<int>(custompaths::gLive05PlayoffQuerySlot),
                        static_cast<unsigned int>(custompaths::gLive05PlayoffQueryCaller),
                        custompaths::gLive05PlayoffQueryConfidence,
                        static_cast<int>(homeTeam),
                        static_cast<int>(awayTeam),
                        homeWins,
                        awayWins);
                    custompaths::WritePlayoffStateLog(message);
                    lastLoggedGame05 = nativeSeriesGame;
                    lastLoggedHomeWins05 = static_cast<int>(homeWins);
                    lastLoggedAwayWins05 = static_cast<int>(awayWins);
                }
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
    }

    if (custompaths::gGameVersion == custompaths::GAME_LIVE_07)
    {
        __try
        {
            const int cachedSlot =
                *reinterpret_cast<const int*>(0x00C567F8);
            const int cachedLength =
                *reinterpret_cast<const int*>(0x00C567FC);
            const int cachedSeriesGame =
                *reinterpret_cast<const int*>(0x00C56800);

            if (cachedSlot < 1 || cachedSlot > 15 ||
                cachedSeriesGame < 1 || cachedSeriesGame > 7)
            {
                return true;
            }

            const unsigned int currentGame =
                static_cast<unsigned int>(cachedSlot) +
                static_cast<unsigned int>(cachedSeriesGame - 1) * 15u;

            custompaths::Live08ScheduleGame current = {};
            if (!custompaths::ReadLive07ScheduleGame(currentGame, current))
                return true;

            const short homeTeam =
                custompaths::ReadLive07ScheduleShort(current, 0x2E);
            const short awayTeam =
                custompaths::ReadLive07ScheduleShort(current, 0x36);

            if (homeTeam < 0 || awayTeam < 0 || homeTeam == awayTeam)
                return true;

            unsigned int homeWins = 0;
            unsigned int awayWins = 0;
            unsigned int historyGames = 0;
            bool exactSeriesScore = true;

            unsigned int previousGame = currentGame;
            while (exactSeriesScore && previousGame > 15u)
            {
                previousGame -= 15u;

                custompaths::Live08ScheduleGame prior = {};
                if (!custompaths::ReadLive07ScheduleGame(previousGame, prior))
                {
                    exactSeriesScore = false;
                    break;
                }

                const short priorHome =
                    custompaths::ReadLive07ScheduleShort(prior, 0x2E);
                const short priorHomeScore =
                    custompaths::ReadLive07ScheduleShort(prior, 0x30);
                const short priorAway =
                    custompaths::ReadLive07ScheduleShort(prior, 0x36);
                const short priorAwayScore =
                    custompaths::ReadLive07ScheduleShort(prior, 0x38);

                const bool sameTeams =
                    (priorHome == homeTeam && priorAway == awayTeam) ||
                    (priorHome == awayTeam && priorAway == homeTeam);

                if (prior.bytes[0x2C] == 0 ||
                    !sameTeams ||
                    priorHomeScore == priorAwayScore)
                {
                    exactSeriesScore = false;
                    break;
                }

                const short winner =
                    priorHomeScore > priorAwayScore ? priorHome : priorAway;

                if (winner == homeTeam)
                    ++homeWins;
                else if (winner == awayTeam)
                    ++awayWins;
                else
                {
                    exactSeriesScore = false;
                    break;
                }

                ++historyGames;
            }

            if (historyGames + 1u !=
                static_cast<unsigned int>(cachedSeriesGame))
            {
                exactSeriesScore = false;
            }

            if (!exactSeriesScore)
                return true;

            presentation->seriesValid = true;
            presentation->seriesGame = cachedSeriesGame;
            presentation->seriesLength =
                cachedLength == 1 || cachedLength == 3 ||
                cachedLength == 5 || cachedLength == 7
                    ? cachedLength
                    : 7;
            presentation->homeWins = static_cast<int>(homeWins);
            presentation->awayWins = static_cast<int>(awayWins);

            if (custompaths::gPlayoffStateDebug)
            {
                static int lastLoggedGame07 = -1;
                static int lastLoggedHomeWins07 = -1;
                static int lastLoggedAwayWins07 = -1;
                if (lastLoggedGame07 != cachedSeriesGame ||
                    lastLoggedHomeWins07 != static_cast<int>(homeWins) ||
                    lastLoggedAwayWins07 != static_cast<int>(awayWins))
                {
                    char message[256] = {};
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] PLAYOFF_07_SERIES game=%d "
                        "scheduleId=%u homeTeam=%d awayTeam=%d "
                        "homeWins=%u awayWins=%u exact=yes\n",
                        cachedSeriesGame,
                        currentGame,
                        static_cast<int>(homeTeam),
                        static_cast<int>(awayTeam),
                        homeWins,
                        awayWins);
                    custompaths::WritePlayoffStateLog(message);
                    lastLoggedGame07 = cachedSeriesGame;
                    lastLoggedHomeWins07 = static_cast<int>(homeWins);
                    lastLoggedAwayWins07 = static_cast<int>(awayWins);
                }
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
    }

    if (custompaths::gGameVersion == custompaths::GAME_LIVE_08)
    {
        __try
        {
            const int cachedSlot =
                *reinterpret_cast<const int*>(0x00D137C8);
            const int cachedLength =
                *reinterpret_cast<const int*>(0x00D137CC);
            const int cachedSeriesGame =
                *reinterpret_cast<const int*>(0x00D137D0);

            if (cachedSlot < 1 || cachedSlot > 15 ||
                cachedSeriesGame < 1 || cachedSeriesGame > 7)
            {
                return true;
            }

            // scheduleplayoff.dbf interleaves the 15 bracket series. The
            // same series advances by 15 IDs for each successive game.
            const unsigned int currentGame =
                static_cast<unsigned int>(cachedSlot) +
                static_cast<unsigned int>(cachedSeriesGame - 1) * 15u;

            custompaths::Live08ScheduleGame current = {};
            if (!custompaths::ReadLive08ScheduleGame(currentGame, current))
                return true;

            const short homeTeam =
                custompaths::ReadLive08ScheduleShort(current, 0x2E);
            const short awayTeam =
                custompaths::ReadLive08ScheduleShort(current, 0x36);

            if (homeTeam < 0 || awayTeam < 0 || homeTeam == awayTeam)
                return true;

            unsigned int homeWins = 0;
            unsigned int awayWins = 0;
            unsigned int historyGames = 0;
            bool exactSeriesScore = true;

            unsigned int previousGame = currentGame;
            while (exactSeriesScore && previousGame > 15u)
            {
                previousGame -= 15u;

                custompaths::Live08ScheduleGame prior = {};
                if (!custompaths::ReadLive08ScheduleGame(previousGame, prior))
                {
                    exactSeriesScore = false;
                    break;
                }

                const short priorHome =
                    custompaths::ReadLive08ScheduleShort(prior, 0x2E);
                const short priorHomeScore =
                    custompaths::ReadLive08ScheduleShort(prior, 0x30);
                const short priorAway =
                    custompaths::ReadLive08ScheduleShort(prior, 0x36);
                const short priorAwayScore =
                    custompaths::ReadLive08ScheduleShort(prior, 0x38);

                const bool sameTeams =
                    (priorHome == homeTeam && priorAway == awayTeam) ||
                    (priorHome == awayTeam && priorAway == homeTeam);

                if (prior.bytes[0x2C] == 0 ||
                    !sameTeams ||
                    priorHomeScore == priorAwayScore)
                {
                    exactSeriesScore = false;
                    break;
                }

                const short winner =
                    priorHomeScore > priorAwayScore ? priorHome : priorAway;

                if (winner == homeTeam)
                    ++homeWins;
                else if (winner == awayTeam)
                    ++awayWins;
                else
                {
                    exactSeriesScore = false;
                    break;
                }

                ++historyGames;
            }

            if (historyGames + 1u !=
                static_cast<unsigned int>(cachedSeriesGame))
            {
                exactSeriesScore = false;
            }

            if (!exactSeriesScore)
                return true;

            presentation->seriesValid = true;
            presentation->seriesGame = cachedSeriesGame;
            presentation->seriesLength =
                cachedLength == 1 || cachedLength == 3 ||
                cachedLength == 5 || cachedLength == 7
                    ? cachedLength
                    : 7;
            presentation->homeWins = static_cast<int>(homeWins);
            presentation->awayWins = static_cast<int>(awayWins);

            if (custompaths::gPlayoffStateDebug)
            {
                static int lastLoggedGame = -1;
                static int lastLoggedHomeWins = -1;
                static int lastLoggedAwayWins = -1;
                if (lastLoggedGame != cachedSeriesGame ||
                    lastLoggedHomeWins != static_cast<int>(homeWins) ||
                    lastLoggedAwayWins != static_cast<int>(awayWins))
                {
                    char message[256] = {};
                    sprintf_s(
                        message,
                        sizeof(message),
                        "[CustomPaths] PLAYOFF_08_SERIES game=%d "
                        "scheduleId=%u homeTeam=%d awayTeam=%d "
                        "homeWins=%u awayWins=%u exact=yes\n",
                        cachedSeriesGame,
                        currentGame,
                        static_cast<int>(homeTeam),
                        static_cast<int>(awayTeam),
                        homeWins,
                        awayWins);
                    custompaths::WritePlayoffStateLog(message);
                    lastLoggedGame = cachedSeriesGame;
                    lastLoggedHomeWins = static_cast<int>(homeWins);
                    lastLoggedAwayWins = static_cast<int>(awayWins);
                }
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
    }

    if (custompaths::gGameVersion != custompaths::GAME_LIVE_06)
        return true;

    // Native Live 06 playoff series state. This deliberately mirrors the
    // already-proven PLAYOFF_SERIES diagnostic path: scheduleplayoff.dbf is
    // represented by 15 interleaved bracket slots, so each successive game
    // in the same series advances by 15 GAMENUM values.
    __try
    {
        unsigned char* scheduleInfo =
            *reinterpret_cast<unsigned char**>(0x00CB2AE0);
        if (!scheduleInfo)
            return true;

        const unsigned int currentGame =
            *reinterpret_cast<unsigned short*>(scheduleInfo + 0x64);

        const bool validPlayoffGame =
            currentGame > 0u && currentGame <= 15u * 7u;
        if (!validPlayoffGame)
            return true;

        const unsigned int seriesGame =
            ((currentGame - 1u) / 15u) + 1u;

        custompaths::Live06ScheduleGame current = {};
        if (!custompaths::ReadLive06ScheduleGame(currentGame, current))
            return true;

        const short homeTeam =
            custompaths::ReadScheduleShort(current, 20);
        const short awayTeam =
            custompaths::ReadScheduleShort(current, 26);

        if (homeTeam < 0 || awayTeam < 0 || homeTeam == awayTeam)
            return true;

        unsigned int homeWins = 0;
        unsigned int awayWins = 0;
        unsigned int historyGames = 0;
        bool exactSeriesScore = true;

        unsigned int previousGame = currentGame;
        while (exactSeriesScore && previousGame > 15u)
        {
            previousGame -= 15u;

            custompaths::Live06ScheduleGame prior = {};
            if (!custompaths::ReadLive06ScheduleGame(previousGame, prior))
            {
                exactSeriesScore = false;
                break;
            }

            const short priorHome =
                custompaths::ReadScheduleShort(prior, 20);
            const short priorAway =
                custompaths::ReadScheduleShort(prior, 26);
            const short priorHomeScore =
                custompaths::ReadScheduleShort(prior, 22);
            const short priorAwayScore =
                custompaths::ReadScheduleShort(prior, 28);

            const bool sameTeams =
                (priorHome == homeTeam && priorAway == awayTeam) ||
                (priorHome == awayTeam && priorAway == homeTeam);

            // +18 is PLAYED in the game's normalized schedule record.
            if (prior.bytes[18] == 0 ||
                !sameTeams ||
                priorHomeScore == priorAwayScore)
            {
                exactSeriesScore = false;
                break;
            }

            const short winner =
                priorHomeScore > priorAwayScore ? priorHome : priorAway;

            if (winner == homeTeam)
                ++homeWins;
            else if (winner == awayTeam)
                ++awayWins;
            else
            {
                exactSeriesScore = false;
                break;
            }

            ++historyGames;
        }

        // Before Game N there must be exactly N-1 completed games.
        if (historyGames + 1u != seriesGame)
            exactSeriesScore = false;

        if (!exactSeriesScore)
            return true;

        presentation->seriesValid = true;
        presentation->seriesGame = static_cast<int>(seriesGame);
        presentation->seriesLength = 7;
        presentation->homeWins = static_cast<int>(homeWins);
        presentation->awayWins = static_cast<int>(awayWins);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return true;
    }
}

// Main.cpp calls this legacy entrypoint at the same point as before.
// Practice remains independent of CONTROLLERS.ENABLED.
void InitializeControllerSetup() {
    LiveControllerSetup::InitializeConfiguration(&practicesetup::InitializePracticeProbe06);
}
