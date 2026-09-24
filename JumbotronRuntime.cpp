#include "plugin-std.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <climits>

using namespace plugin;

namespace jumbotron
{
    // =====================================================================
    // Per-game addresses
    // =====================================================================

    struct GameAddresses
    {
        const char* name;

        uintptr_t runRMConstructors;
        uintptr_t runRMConstructorsCall;
        uintptr_t getLoaderContext;
        uintptr_t publishRuntime;
        uintptr_t rmState;

        uintptr_t scrollTextureDimRuntime;
        uintptr_t scrollTextureDimDraw;

        uintptr_t gdAI;
        uintptr_t gdInfoCentral;
        uintptr_t getGameClock;
        uintptr_t getClockUnitsPerSecond;
        uintptr_t isShotClockValid;
        uintptr_t getShotClock;
        uintptr_t getTeamScore;
        uintptr_t getTeamIDFromSide;
        uintptr_t getTeamFouls;
        uintptr_t getTeamTimeoutsLeft;

        uintptr_t runtimePlayerManager;
        uintptr_t resolveRuntimePlayer;
        uintptr_t playerQueryGetInt;
        uintptr_t substitutionBuilderCall;
        uintptr_t substitutionBuilder;

        unsigned int getQuarterSlot;
        unsigned int isGameClockValidSlot;
    };


    // NBA Live 2005 renderer values recovered directly from nba2005.exe:
    //
    // gScrollTextureDim_RMRuntime string   00BAA1A0
    // static initializer                   00B265B0
    // RM constructor registration          006E7F22
    // ScrollTextureDim runtime             00C28734
    // runtime +00 draw table               00C28730
    // ScrollTextureDim::Draw               009A6430
    // InitVariables                        009A6250
    // UnInitVariables                      009A6A20
    // InitExternalVariables                009A6120
    // PublishRuntime                       006E8029
    // RunRMConstructors                    006DB06F
    // RunRMConstructors call               006DD891
    // RM initialized/state byte            00C56CD9
    // DynamicLoader/context getter         006CF478
    //
    static constexpr GameAddresses NBA_LIVE_05 =
    {
        "NBA Live 2005",

        0x006DB06F, // RunRMConstructors
        0x006DD891, // call RunRMConstructors
        0x006CF478, // DynamicLoader/context getter
        0x006E8029, // PublishRuntime
        0x00C56CD9, // RM initialized/state byte

        0x00C28734, // gScrollTextureDim RenderMethodRuntime
        0x009A6430, // ScrollTextureDim::Draw

        0x00C49F1C, // gGdAI
        0x00C49F04, // gGdInfoCentral
        0x005E1280, // GetGameClock
        0x00000000, // GetClockUnitsPerSecond; 05 uses 60
        0x005DDAA0, // IsShotClockValid
        0x005E12C0, // GetShotClock
        0x005DDD20, // GetTeamScore
        0x005DD750, // GetTeamIDFromSide
        0x005DD900, // GetTeamFouls
        0x005DD910, // GetTeamTimeoutsLeft

        0x00C49014, // runtime player manager
        0x00494A20, // ResolveRuntimePlayer
        0x004B2960, // PlayerQueryGetInt
        0x0058EEDD, // substitution builder call
        0x005817F0, // substitution builder

        88,         // GdAI::GetQuarter vtable slot
        89          // GdAI::IsGameClockValid vtable slot
    };


    // Proven NBA Live 06 values.
    static constexpr GameAddresses NBA_LIVE_06 =
    {
        "NBA Live 06",

        0x006F1C47, // RunRMConstructors
        0x006F4448, // call RunRMConstructors
        0x006E6050, // DynamicLoader/context getter
        0x006FEA4D, // PublishRuntime
        0x00CBFC41, // RM initialized/state byte

        0x00C8C114, // gScrollTextureDim RenderMethodRuntime
        0x009EF090, // ScrollTextureDim::Draw

        0x00CB2C8C, // gGdAI
        0x00CB2C74, // gGdInfoCentral
        0x005EE070, // GetGameClock
        0x00000000, // GetClockUnitsPerSecond; 05/06 use 60
        0x005E9B70, // IsShotClockValid
        0x005EE0B0, // GetShotClock
        0x005E9E50, // GetTeamScore
        0x005E97C0, // GetTeamIDFromSide
        0x005E99B0, // GetTeamFouls
        0x005E99C0, // GetTeamTimeoutsLeft

        0x00CB1D30, // runtime player manager
        0x00493A60, // ResolveRuntimePlayer
        0x004AE820, // PlayerQueryGetInt
        0x005A3F4D, // substitution builder call
        0x0058F870, // substitution builder

        91,         // GdAI::GetQuarter vtable slot
        92          // GdAI::IsGameClockValid vtable slot
    };


    // NBA Live 07 renderer values recovered directly from nbalive07.exe:
    //
    // gScrollTextureDim_RMRuntime string   00C322B4
    // static initializer                   00BAF0C0
    // RM constructor registration          004339C0
    // ScrollTextureDim runtime             00CA519C
    // runtime +00 draw table               00CA5198
    // ScrollTextureDim::Draw               00A0F040
    // InitVariables                        00A0EE60
    // UnInitVariables                      00A0F5D0
    // InitExternalVariables                00A0ED30
    // PublishRuntime                       00433AC7
    // RunRMConstructors                    0042B1FF
    // RunRMConstructors call               0042DA00
    // RM initialized/state byte            00CC96E9
    // DynamicLoader/context getter         00808B84
    //
    static constexpr GameAddresses NBA_LIVE_07 =
    {
        "NBA Live 07",

        0x0042B1FF, // RunRMConstructors
        0x0042DA00, // call RunRMConstructors
        0x00808B84, // DynamicLoader/context getter
        0x00433AC7, // PublishRuntime
        0x00CC96E9, // RM initialized/state byte

        0x00CA519C, // gScrollTextureDim RenderMethodRuntime
        0x00A0F040, // ScrollTextureDim::Draw

        0x00CEB8A4, // gGdAI
        0x00CEB888, // gGdInfoCentral
        0x00641A20, // GetGameClock
        0x0063CC70, // GetClockUnitsPerSecond
        0x0063CC80, // IsShotClockValid
        0x00641A60, // GetShotClock
        0x0063CFC0, // GetTeamScore
        0x0063C7F0, // GetTeamIDFromSide
        0x0063CA60, // GetTeamFouls
        0x0063CA80, // GetTeamTimeoutsLeft

        0x00CEAA08, // runtime player manager
        0x004BB080, // ResolveRuntimePlayer
        0x004D68B0, // PlayerQueryGetInt
        0x0058050D, // substitution builder call
        0x00570A60, // substitution builder

        96,         // GdAI::GetQuarter vtable slot
        97          // GdAI::IsGameClockValid vtable slot
    };


    // NBA Live 08 renderer values recovered directly from nbalive08.exe:
    //
    // gScrollTextureDim_RMRuntime string   00CB6EFC
    // static initializer                   00C31880
    // RM registration thunk                004361F0 -> 00E21ACA
    // ScrollTextureDim runtime             00D60E2C
    // runtime +00 draw table               00D60E28
    // ScrollTextureDim::Draw               00A487E0
    // InitVariables                        00A48600
    // UnInitVariables                      00A48D70
    // InitExternalVariables                00A484D0
    // PublishRuntime thunk                 004362F7 -> 00E21B03
    // RunRMConstructors thunk              0042DA2F -> 00E1DE1F
    // RunRMConstructors call               00E1F2F5
    // RM initialized/state byte            00D85079
    // DynamicLoader/context getter         00837CE2
    //
    static constexpr GameAddresses NBA_LIVE_08 =
    {
        "NBA Live 08",

        0x0042DA2F, // RunRMConstructors thunk
        0x00E1F2F5, // call RunRMConstructors
        0x00837CE2, // DynamicLoader/context getter
        0x004362F7, // PublishRuntime thunk
        0x00D85079, // RM initialized/state byte

        0x00D60E2C, // gScrollTextureDim RenderMethodRuntime
        0x00A487E0, // ScrollTextureDim::Draw

        0x00DA741C, // gGdAI
        0x00DA7404, // gGdInfoCentral
        0x00661130, // GetGameClock
        0x0065C420, // GetClockUnitsPerSecond
        0x0065C430, // IsShotClockValid
        0x00661170, // GetShotClock
        0x0065C710, // GetTeamScore
        0x0065BFD0, // GetTeamIDFromSide
        0x0065C220, // GetTeamFouls
        0x0065C230, // GetTeamTimeoutsLeft

        0x00DA63C8, // runtime player manager
        0x004C13B0, // ResolveRuntimePlayer
        0x004DEA10, // PlayerQueryGetInt
        0x005A0E1D, // substitution builder call
        0x0058BDD0, // substitution builder

        97,         // GdAI::GetQuarter vtable slot
        98          // GdAI::IsGameClockValid vtable slot
    };


    static const GameAddresses* gGame = nullptr;

    bool gDebugLoggingEnabled = false;

    void InitializeDebugLogging()
    {
        char exePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return;

        char* slash = std::strrchr(exePath, '\\');
        if (!slash)
            return;

        *(slash + 1) = '\0';

        char iniPath[MAX_PATH] = {};
        std::snprintf(iniPath, sizeof(iniPath), "%smain.ini", exePath);

        gDebugLoggingEnabled =
            GetPrivateProfileIntA("DEBUG", "CONSOLE", 0, iniPath) != 0;

        if (!gDebugLoggingEnabled)
            return;

        char logsPath[MAX_PATH] = {};
        std::snprintf(logsPath, sizeof(logsPath), "%slogs", exePath);
        CreateDirectoryA(logsPath, nullptr);
    }

    bool BuildDebugLogPath(
        char* outPath,
        size_t outSize,
        const char* fileName)
    {
        if (!outPath || !outSize || !fileName || !gDebugLoggingEnabled)
        {
            if (outPath && outSize)
                outPath[0] = '\0';
            return false;
        }

        char exePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (!length || length >= MAX_PATH)
        {
            outPath[0] = '\0';
            return false;
        }

        char* slash = std::strrchr(exePath, '\\');
        if (!slash)
        {
            outPath[0] = '\0';
            return false;
        }

        *(slash + 1) = '\0';

        const int written =
            std::snprintf(outPath, outSize, "%slogs\\%s", exePath, fileName);

        if (written < 0 || static_cast<size_t>(written) >= outSize)
        {
            outPath[0] = '\0';
            return false;
        }

        return true;
    }


    static const GameAddresses* DetectGame()
    {
        const uintptr_t entryPoint =
            FM::GetEntryPoint();

        if (entryPoint == 0xCD8005)
            return &NBA_LIVE_05;

        if (entryPoint != 0x40109F)
            return nullptr;

        if (patch::GetFloat(0xBD832C) == 1.3333334f)
            return &NBA_LIVE_06;

        if (patch::GetFloat(0xBBBC3C) == 1.3333334f)
            return &NBA_LIVE_07;

        if (patch::GetFloat(0xC3DF84) == 1.3333334f)
            return &NBA_LIVE_08;

        return nullptr;
    }


    static constexpr size_t kRenderMethodRuntimeSize =
        0x28;


    // =====================================================================
    // Atlas layout
    // =====================================================================
    // One texture, one horizontal row, 16 equal cells:
    //
    //   0..9  = digits 0..9
    //   10    = blank
    //   11    = OT
    //   12..15 reserved
    //
    // Every dynamic material uses the same 1/16 U-step.

    static constexpr int kAtlasColumns = 16;
    static constexpr float kAtlasCellWidth =
        1.0f / static_cast<float>(kAtlasColumns);

    static constexpr int kSlotBlank = 10;
    static constexpr int kSlotOT = 11;


    // =====================================================================
    // Logical jumbotron channels
    // =====================================================================

    enum class Channel : int
    {
        HomeHundreds = 0,
        HomeTens,
        HomeOnes,
        AwayHundreds,
        AwayTens,
        AwayOnes,
        GameMinTens,
        GameMinOnes,
        GameSecTens,
        GameSecOnes,
        ShotTens,
        ShotOnes,
        Period,
        HomeTimeouts,
        AwayTimeouts,
        HomeTeamFoulsTens,
        HomeTeamFoulsOnes,
        AwayTeamFoulsTens,
        AwayTeamFoulsOnes,
        HomeCourt1NumberTens,
        HomeCourt1NumberOnes,
        HomeCourt1PointsTens,
        HomeCourt1PointsOnes,
        HomeCourt1Fouls,
        HomeCourt2NumberTens,
        HomeCourt2NumberOnes,
        HomeCourt2PointsTens,
        HomeCourt2PointsOnes,
        HomeCourt2Fouls,
        HomeCourt3NumberTens,
        HomeCourt3NumberOnes,
        HomeCourt3PointsTens,
        HomeCourt3PointsOnes,
        HomeCourt3Fouls,
        HomeCourt4NumberTens,
        HomeCourt4NumberOnes,
        HomeCourt4PointsTens,
        HomeCourt4PointsOnes,
        HomeCourt4Fouls,
        HomeCourt5NumberTens,
        HomeCourt5NumberOnes,
        HomeCourt5PointsTens,
        HomeCourt5PointsOnes,
        HomeCourt5Fouls,
        AwayCourt1NumberTens,
        AwayCourt1NumberOnes,
        AwayCourt1PointsTens,
        AwayCourt1PointsOnes,
        AwayCourt1Fouls,
        AwayCourt2NumberTens,
        AwayCourt2NumberOnes,
        AwayCourt2PointsTens,
        AwayCourt2PointsOnes,
        AwayCourt2Fouls,
        AwayCourt3NumberTens,
        AwayCourt3NumberOnes,
        AwayCourt3PointsTens,
        AwayCourt3PointsOnes,
        AwayCourt3Fouls,
        AwayCourt4NumberTens,
        AwayCourt4NumberOnes,
        AwayCourt4PointsTens,
        AwayCourt4PointsOnes,
        AwayCourt4Fouls,
        AwayCourt5NumberTens,
        AwayCourt5NumberOnes,
        AwayCourt5PointsTens,
        AwayCourt5PointsOnes,
        AwayCourt5Fouls,

        Count
    };

    static constexpr int kChannelCount =
        static_cast<int>(Channel::Count);


    static const char* const kRuntimeNames[kChannelCount] =
    {
        "gJumboHomeHundreds_RMRuntime",
        "gJumboHomeTens_RMRuntime",
        "gJumboHomeOnes_RMRuntime",
        "gJumboAwayHundreds_RMRuntime",
        "gJumboAwayTens_RMRuntime",
        "gJumboAwayOnes_RMRuntime",
        "gJumboGameMinTens_RMRuntime",
        "gJumboGameMinOnes_RMRuntime",
        "gJumboGameSecTens_RMRuntime",
        "gJumboGameSecOnes_RMRuntime",
        "gJumboShotTens_RMRuntime",
        "gJumboShotOnes_RMRuntime",
        "gJumboPeriod_RMRuntime",
        "gJumboHomeTimeouts_RMRuntime",
        "gJumboAwayTimeouts_RMRuntime",
        "gJumboHomeTeamFoulsTens_RMRuntime",
        "gJumboHomeTeamFoulsOnes_RMRuntime",
        "gJumboAwayTeamFoulsTens_RMRuntime",
        "gJumboAwayTeamFoulsOnes_RMRuntime",
        "gJumboHomeCourt1NumberTens_RMRuntime",
        "gJumboHomeCourt1NumberOnes_RMRuntime",
        "gJumboHomeCourt1PointsTens_RMRuntime",
        "gJumboHomeCourt1PointsOnes_RMRuntime",
        "gJumboHomeCourt1Fouls_RMRuntime",
        "gJumboHomeCourt2NumberTens_RMRuntime",
        "gJumboHomeCourt2NumberOnes_RMRuntime",
        "gJumboHomeCourt2PointsTens_RMRuntime",
        "gJumboHomeCourt2PointsOnes_RMRuntime",
        "gJumboHomeCourt2Fouls_RMRuntime",
        "gJumboHomeCourt3NumberTens_RMRuntime",
        "gJumboHomeCourt3NumberOnes_RMRuntime",
        "gJumboHomeCourt3PointsTens_RMRuntime",
        "gJumboHomeCourt3PointsOnes_RMRuntime",
        "gJumboHomeCourt3Fouls_RMRuntime",
        "gJumboHomeCourt4NumberTens_RMRuntime",
        "gJumboHomeCourt4NumberOnes_RMRuntime",
        "gJumboHomeCourt4PointsTens_RMRuntime",
        "gJumboHomeCourt4PointsOnes_RMRuntime",
        "gJumboHomeCourt4Fouls_RMRuntime",
        "gJumboHomeCourt5NumberTens_RMRuntime",
        "gJumboHomeCourt5NumberOnes_RMRuntime",
        "gJumboHomeCourt5PointsTens_RMRuntime",
        "gJumboHomeCourt5PointsOnes_RMRuntime",
        "gJumboHomeCourt5Fouls_RMRuntime",
        "gJumboAwayCourt1NumberTens_RMRuntime",
        "gJumboAwayCourt1NumberOnes_RMRuntime",
        "gJumboAwayCourt1PointsTens_RMRuntime",
        "gJumboAwayCourt1PointsOnes_RMRuntime",
        "gJumboAwayCourt1Fouls_RMRuntime",
        "gJumboAwayCourt2NumberTens_RMRuntime",
        "gJumboAwayCourt2NumberOnes_RMRuntime",
        "gJumboAwayCourt2PointsTens_RMRuntime",
        "gJumboAwayCourt2PointsOnes_RMRuntime",
        "gJumboAwayCourt2Fouls_RMRuntime",
        "gJumboAwayCourt3NumberTens_RMRuntime",
        "gJumboAwayCourt3NumberOnes_RMRuntime",
        "gJumboAwayCourt3PointsTens_RMRuntime",
        "gJumboAwayCourt3PointsOnes_RMRuntime",
        "gJumboAwayCourt3Fouls_RMRuntime",
        "gJumboAwayCourt4NumberTens_RMRuntime",
        "gJumboAwayCourt4NumberOnes_RMRuntime",
        "gJumboAwayCourt4PointsTens_RMRuntime",
        "gJumboAwayCourt4PointsOnes_RMRuntime",
        "gJumboAwayCourt4Fouls_RMRuntime",
        "gJumboAwayCourt5NumberTens_RMRuntime",
        "gJumboAwayCourt5NumberOnes_RMRuntime",
        "gJumboAwayCourt5PointsTens_RMRuntime",
        "gJumboAwayCourt5PointsOnes_RMRuntime",
        "gJumboAwayCourt5Fouls_RMRuntime",
    };


    // =====================================================================
    // Runtime / renderer layouts
    // =====================================================================

    struct RMExportRecord
    {
        uint32_t unused00;
        uint32_t unused04;
        uint32_t unused08;

        const char* exportName;    // +0x0C
        void* runtime;             // +0x10

        uint32_t unused14;
        uint32_t unused18;
        uint32_t unused1C;
        uint32_t unused20;
    };

    static_assert(sizeof(RMExportRecord) == 0x24);


    struct ExternalBinding
    {
        uint32_t unknown00;
        uint32_t unknown04;
        uint32_t unknown08;
        void* data;                // +0x0C
        uint32_t flags;            // +0x10
    };

    static_assert(sizeof(ExternalBinding) == 0x14);


    struct GdAI
    {
        void** __vtable;
    };

    struct GdInfoCentral;


    using RunRMConstructorsFn =
        void(__cdecl*)();

    using GetLoaderContextFn =
        void* (__cdecl*)();

    using PublishRuntimeFn =
        void(__thiscall*)(
            RMExportRecord* self,
            void* loaderContext
        );

    using ScrollTextureDimDrawFn =
        void(__cdecl*)(
            void* drawContext,
            void* drawData
        );

    using GetGameClockFn =
        unsigned int(__thiscall*)(GdInfoCentral*);

    using GetClockUnitsPerSecondFn =
        unsigned int(__thiscall*)(GdInfoCentral*);

    using IsShotClockValidFn =
        bool(__thiscall*)(GdInfoCentral*);

    using GetShotClockFn =
        unsigned int(__thiscall*)(GdInfoCentral*);

    using GetTeamScoreFn =
        int(__thiscall*)(GdInfoCentral*, int);

    using GetQuarterFn =
        int(__thiscall*)(GdAI*);

    using IsGameClockValidFn =
        bool(__thiscall*)(GdAI*);


    // =====================================================================
    // Logging
    // =====================================================================

    static void Log(const char* format, ...)
    {
        if (!gDebugLoggingEnabled)
            return;

        char logPath[MAX_PATH] = {};
        if (!BuildDebugLogPath(
                logPath,
                sizeof(logPath),
                "JumboRuntime.log"))
        {
            return;
        }

        FILE* f = nullptr;
        if (fopen_s(&f, logPath, "a") != 0 || !f)
            return;

        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);

        fprintf(f, "\n");
        fclose(f);
    }



    // =====================================================================
    // Shared live jumbotron state
    // =====================================================================

    struct IDTeam
    {
        int value;
    };

    struct LivePlayer
    {
        int nativePlayerId;
        int databasePlayerId;
        int jerseyNumber;
        int currentPosition;
        int initialPosition;
        int courtSlot;
        int fouls;
        int points;
        bool onCourt;
        bool valid;
    };

    struct LiveTeam
    {
        int teamId;
        int playerCount;
        LivePlayer players[12];
    };

    struct LiveGame
    {
        LiveTeam teams[2];
        int homeTeamIndex;
        int awayTeamIndex;
        DWORD refreshedAt;
        bool valid;
    };

    struct DisplayPlayer
    {
        int jerseyNumber;
        int points;
        int fouls;
        bool valid;
    };

    struct JumbotronState
    {
        int homeScore;
        int awayScore;

        int gameMinutes;
        int gameSeconds;

        int shotClock;

        // User-facing period: 1..4, >=5 means OT.
        int period;

        int homeTimeouts;
        int awayTimeouts;
        int homeFouls;
        int awayFouls;

        DisplayPlayer homePlayers[5];
        DisplayPlayer awayPlayers[5];

        bool gameClockValid;
        bool shotClockValid;
    };

    using GetTeamIDFromSideFn =
        IDTeam* (__stdcall*)(IDTeam*, int);

    using GetTeamValueFn =
        int(__stdcall*)(int);

    using PlayerQueryGetIntFn =
        int(__thiscall*)(void*, int);

    using ResolveRuntimePlayerFn =
        void* (__cdecl*)(const DWORD*, DWORD*);

    using SubstitutionBuilderFn =
        bool(__thiscall*)(void*, DWORD*, DWORD*);

    static JumbotronState gState = {};
    static JumbotronState gLastLoggedState = {};
    static bool gHaveState = false;
    static bool gHaveLoggedState = false;

    static LiveGame gLiveGame = {};
    static bool gOnCourt[2][12] = {};
    static int gCourtSlot[2][12] = {};
    static bool gLineupInitialized = false;

    static SubstitutionBuilderFn gOriginalSubstitutionBuilder = nullptr;

    alignas(16) static float gChannelUv[kChannelCount][4] = {};

    static DWORD gLastStateReadTick = 0;
    static bool gHaveStateReadTick = false;


    static int ClampInt(int value, int low, int high)
    {
        if (value < low) return low;
        if (value > high) return high;
        return value;
    }


    static void SetChannelSlot(Channel channel, int slot)
    {
        const int index = static_cast<int>(channel);

        slot = ClampInt(slot, 0, kAtlasColumns - 1);

        gChannelUv[index][0] =
            static_cast<float>(slot) * kAtlasCellWidth;

        gChannelUv[index][1] = 0.0f;
        gChannelUv[index][2] = 1.0f;
        gChannelUv[index][3] = 1.0f;
    }


    static void BlankAllChannels()
    {
        for (int i = 0; i < kChannelCount; ++i)
            SetChannelSlot(static_cast<Channel>(i), kSlotBlank);
    }


    static int GetDatabaseTeamID(IDTeam team)
    {
        return team.value > 0 ? team.value - 1 : -1;
    }


    static IDTeam GetTeamIDFromSide(int side)
    {
        IDTeam result = {};
        if (!gGame || !gGame->getTeamIDFromSide)
            return result;

        auto function = reinterpret_cast<GetTeamIDFromSideFn>(
            gGame->getTeamIDFromSide
        );

        function(&result, side);
        return result;
    }


    static int GetOptionalTeamValue(uintptr_t address, IDTeam team)
    {
        if (!address || team.value <= 0)
            return -1;

        auto function = reinterpret_cast<GetTeamValueFn>(address);
        return function(team.value);
    }


    static void InitializeLiveLineup()
    {
        if (gLineupInitialized)
            return;

        std::memset(gOnCourt, 0, sizeof(gOnCourt));

        for (int side = 0; side < 2; ++side)
        {
            for (int slot = 0; slot < 12; ++slot)
                gCourtSlot[side][slot] = -1;

            for (int slot = 0; slot < 5; ++slot)
            {
                gOnCourt[side][slot] = true;
                gCourtSlot[side][slot] = slot;
            }
        }

        gLineupInitialized = true;
    }


    static void ResetLiveGame()
    {
        std::memset(&gLiveGame, 0, sizeof(gLiveGame));
        std::memset(gOnCourt, 0, sizeof(gOnCourt));
        for (int side = 0; side < 2; ++side)
            for (int slot = 0; slot < 12; ++slot)
                gCourtSlot[side][slot] = -1;
        gLineupInitialized = false;
        gLiveGame.homeTeamIndex = -1;
        gLiveGame.awayTeamIndex = -1;
    }


    static bool FindLivePlayerLocation(
        DWORD nativePlayerId,
        int& sideOut,
        int& slotOut)
    {
        if (!nativePlayerId || nativePlayerId == 0xFFFFFFFFu)
            return false;

        for (int side = 0; side < 2; ++side)
        {
            for (int slot = 0; slot < 12; ++slot)
            {
                const LivePlayer& p =
                    gLiveGame.teams[side].players[slot];

                if (p.valid &&
                    static_cast<DWORD>(p.nativePlayerId) == nativePlayerId)
                {
                    sideOut = side;
                    slotOut = slot;
                    return true;
                }
            }
        }

        return false;
    }


    static void ApplySubstitutionToLiveLineup(
        DWORD incomingId,
        DWORD outgoingId)
    {
        if (!gLiveGame.valid)
            return;

        InitializeLiveLineup();

        int inSide = -1;
        int inSlot = -1;
        int outSide = -1;
        int outSlot = -1;

        const bool haveIn =
            FindLivePlayerLocation(incomingId, inSide, inSlot);

        const bool haveOut =
            FindLivePlayerLocation(outgoingId, outSide, outSlot);

        if (!haveIn || !haveOut || inSide != outSide)
            return;

        const int inheritedCourtSlot =
            gCourtSlot[outSide][outSlot];

        if (inheritedCourtSlot < 0 || inheritedCourtSlot > 4)
            return;

        gOnCourt[outSide][outSlot] = false;
        gCourtSlot[outSide][outSlot] = -1;
        gOnCourt[inSide][inSlot] = true;
        gCourtSlot[inSide][inSlot] = inheritedCourtSlot;

        gLiveGame.teams[outSide].players[outSlot].onCourt = false;
        gLiveGame.teams[outSide].players[outSlot].courtSlot = -1;
        gLiveGame.teams[inSide].players[inSlot].onCourt = true;
        gLiveGame.teams[inSide].players[inSlot].courtSlot = inheritedCourtSlot;
    }


    static bool __fastcall HookSubstitutionBuilder(
        void* thisPtr,
        void*,
        DWORD* incomingPlayerId,
        DWORD* outgoingPlayerId)
    {
        DWORD incomingId = 0;
        DWORD outgoingId = 0;

        __try
        {
            if (incomingPlayerId) incomingId = *incomingPlayerId;
            if (outgoingPlayerId) outgoingId = *outgoingPlayerId;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            incomingId = 0;
            outgoingId = 0;
        }

        const bool result =
            gOriginalSubstitutionBuilder
                ? gOriginalSubstitutionBuilder(
                    thisPtr,
                    incomingPlayerId,
                    outgoingPlayerId
                )
                : true;

        if (incomingId && outgoingId)
            ApplySubstitutionToLiveLineup(incomingId, outgoingId);

        return result;
    }


    static bool FillLiveRuntimePlayerData(
        LivePlayer& out,
        DWORD nativePlayerId,
        void* player)
    {
        if (!player || !gGame || !gGame->playerQueryGetInt ||
            nativePlayerId == 0 || nativePlayerId == 0xFFFFFFFFu)
        {
            return false;
        }

        auto getInt = reinterpret_cast<PlayerQueryGetIntFn>(
            gGame->playerQueryGetInt
        );

        __try
        {
            std::memset(&out, 0, sizeof(out));
            out.courtSlot = -1;
            out.nativePlayerId = static_cast<int>(nativePlayerId);
            out.databasePlayerId = getInt(player, 0x88);
            out.jerseyNumber = getInt(player, 0x85);
            out.currentPosition = getInt(player, 0x87);
            out.initialPosition = getInt(player, 0x89);
            out.fouls = getInt(player, 0x09);
            out.points = getInt(player, 0x0F);
            out.valid = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out.valid = false;
            return false;
        }
    }


    static bool RefreshLiveGame(
        int homeTeamId,
        int awayTeamId)
    {
        if (!gGame || !gGame->runtimePlayerManager ||
            !gGame->resolveRuntimePlayer || !gGame->playerQueryGetInt)
        {
            return false;
        }

        __try
        {
            void* manager =
                *reinterpret_cast<void**>(
                    gGame->runtimePlayerManager
                );

            if (!manager)
            {
                ResetLiveGame();
                return false;
            }

            DWORD* nativeIds =
                reinterpret_cast<DWORD*>(
                    static_cast<unsigned char*>(manager) + 0x08
                );

            LiveGame next = {};
            next.homeTeamIndex = 0;
            next.awayTeamIndex = 1;
            next.teams[0].teamId = homeTeamId;
            next.teams[1].teamId = awayTeamId;

            int resolved[2] = { 0, 0 };
            InitializeLiveLineup();

            auto resolvePlayer =
                reinterpret_cast<ResolveRuntimePlayerFn>(
                    gGame->resolveRuntimePlayer
                );

            for (int managerSlot = 0; managerSlot < 24; ++managerSlot)
            {
                const DWORD nativeId = nativeIds[managerSlot];
                if (nativeId == 0 || nativeId == 0xFFFFFFFFu)
                    continue;

                DWORD resolverType = 0;
                void* player =
                    resolvePlayer(&nativeId, &resolverType);

                if (!player)
                    continue;

                const int side = managerSlot < 12 ? 0 : 1;
                LivePlayer temp = {};
                if (!FillLiveRuntimePlayerData(temp, nativeId, player))
                    continue;

                int rosterSlot = temp.initialPosition;
                if (rosterSlot < 0 || rosterSlot >= 12)
                    rosterSlot = managerSlot % 12;

                temp.onCourt = gOnCourt[side][rosterSlot];
                temp.courtSlot = gCourtSlot[side][rosterSlot];
                next.teams[side].players[rosterSlot] = temp;
                ++resolved[side];
            }

            next.teams[0].playerCount = resolved[0];
            next.teams[1].playerCount = resolved[1];
            next.refreshedAt = GetTickCount();
            next.valid = resolved[0] > 0 && resolved[1] > 0;
            gLiveGame = next;
            return next.valid;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ResetLiveGame();
            return false;
        }
    }


    static bool GetDisplayPlayer(
        int side,
        int courtSlot,
        DisplayPlayer& out)
    {
        std::memset(&out, 0, sizeof(out));
        out.jerseyNumber = INT_MIN;
        out.points = INT_MIN;
        out.fouls = INT_MIN;

        if (!gLiveGame.valid || side < 0 || side > 1 ||
            courtSlot < 0 || courtSlot > 4)
        {
            return false;
        }

        for (int slot = 0; slot < 12; ++slot)
        {
            const LivePlayer& p = gLiveGame.teams[side].players[slot];
            if (!p.valid || !p.onCourt || p.courtSlot != courtSlot)
                continue;

            out.jerseyNumber = p.jerseyNumber;
            out.points = p.points;
            out.fouls = p.fouls;
            out.valid = true;
            return true;
        }

        return false;
    }


    static void SetThreeDigitChannels(
        int value,
        Channel hundreds,
        Channel tens,
        Channel ones)
    {
        if (value < 0)
        {
            SetChannelSlot(hundreds, kSlotBlank);
            SetChannelSlot(tens, kSlotBlank);
            SetChannelSlot(ones, kSlotBlank);
            return;
        }

        value = ClampInt(value, 0, 999);

        const int h = (value / 100) % 10;
        const int t = (value / 10) % 10;
        const int o = value % 10;

        SetChannelSlot(hundreds, value >= 100 ? h : kSlotBlank);
        SetChannelSlot(tens, value >= 10 ? t : kSlotBlank);
        SetChannelSlot(ones, o);
    }


    static void SetTwoDigitChannels(
        int value,
        Channel tens,
        Channel ones,
        bool blankTensForSingleDigit = true)
    {
        if (value < 0)
        {
            SetChannelSlot(tens, kSlotBlank);
            SetChannelSlot(ones, kSlotBlank);
            return;
        }

        value = ClampInt(value, 0, 99);
        const int t = (value / 10) % 10;
        const int o = value % 10;

        SetChannelSlot(
            tens,
            blankTensForSingleDigit && value < 10 ? kSlotBlank : t
        );

        SetChannelSlot(ones, o);
    }


    static void SetJerseyChannels(
        int jerseyNumber,
        Channel tens,
        Channel ones)
    {
        if (jerseyNumber == -1)
        {
            SetChannelSlot(tens, 0);
            SetChannelSlot(ones, 0);
            return;
        }

        SetTwoDigitChannels(jerseyNumber, tens, ones, true);
    }


    static void SetSingleDigitChannel(
        int value,
        Channel channel)
    {
        if (value < 0)
        {
            SetChannelSlot(channel, kSlotBlank);
            return;
        }

        SetChannelSlot(channel, ClampInt(value, 0, 9));
    }


    static void BuildChannelSlots(const JumbotronState& state)
    {
        SetThreeDigitChannels(
            state.homeScore,
            Channel::HomeHundreds,
            Channel::HomeTens,
            Channel::HomeOnes
        );

        SetThreeDigitChannels(
            state.awayScore,
            Channel::AwayHundreds,
            Channel::AwayTens,
            Channel::AwayOnes
        );

        if (state.gameClockValid)
        {
            const int minutes = ClampInt(state.gameMinutes, 0, 99);
            const int seconds = ClampInt(state.gameSeconds, 0, 59);
            SetChannelSlot(
                Channel::GameMinTens,
                minutes >= 10 ? (minutes / 10) % 10 : kSlotBlank
            );
            SetChannelSlot(Channel::GameMinOnes, minutes % 10);
            SetChannelSlot(Channel::GameSecTens, (seconds / 10) % 10);
            SetChannelSlot(Channel::GameSecOnes, seconds % 10);
        }
        else
        {
            SetChannelSlot(Channel::GameMinTens, kSlotBlank);
            SetChannelSlot(Channel::GameMinOnes, kSlotBlank);
            SetChannelSlot(Channel::GameSecTens, kSlotBlank);
            SetChannelSlot(Channel::GameSecOnes, kSlotBlank);
        }

        if (state.shotClockValid)
        {
            const int shot = ClampInt(state.shotClock, 0, 99);
            SetChannelSlot(
                Channel::ShotTens,
                shot >= 10 ? (shot / 10) % 10 : kSlotBlank
            );
            SetChannelSlot(Channel::ShotOnes, shot % 10);
        }
        else
        {
            SetChannelSlot(Channel::ShotTens, kSlotBlank);
            SetChannelSlot(Channel::ShotOnes, kSlotBlank);
        }

        if (state.period >= 1 && state.period <= 4)
            SetChannelSlot(Channel::Period, state.period);
        else if (state.period >= 5)
            SetChannelSlot(Channel::Period, kSlotOT);
        else
            SetChannelSlot(Channel::Period, kSlotBlank);

        SetSingleDigitChannel(state.homeTimeouts, Channel::HomeTimeouts);
        SetSingleDigitChannel(state.awayTimeouts, Channel::AwayTimeouts);
        SetTwoDigitChannels(
            state.homeFouls,
            Channel::HomeTeamFoulsTens,
            Channel::HomeTeamFoulsOnes,
            true
        );
        SetTwoDigitChannels(
            state.awayFouls,
            Channel::AwayTeamFoulsTens,
            Channel::AwayTeamFoulsOnes,
            true
        );

        const Channel homeNumberTens[5] = {
            Channel::HomeCourt1NumberTens,
            Channel::HomeCourt2NumberTens,
            Channel::HomeCourt3NumberTens,
            Channel::HomeCourt4NumberTens,
            Channel::HomeCourt5NumberTens,
        };
        const Channel homeNumberOnes[5] = {
            Channel::HomeCourt1NumberOnes,
            Channel::HomeCourt2NumberOnes,
            Channel::HomeCourt3NumberOnes,
            Channel::HomeCourt4NumberOnes,
            Channel::HomeCourt5NumberOnes,
        };
        const Channel homePointsTens[5] = {
            Channel::HomeCourt1PointsTens,
            Channel::HomeCourt2PointsTens,
            Channel::HomeCourt3PointsTens,
            Channel::HomeCourt4PointsTens,
            Channel::HomeCourt5PointsTens,
        };
        const Channel homePointsOnes[5] = {
            Channel::HomeCourt1PointsOnes,
            Channel::HomeCourt2PointsOnes,
            Channel::HomeCourt3PointsOnes,
            Channel::HomeCourt4PointsOnes,
            Channel::HomeCourt5PointsOnes,
        };
        const Channel homeFouls[5] = {
            Channel::HomeCourt1Fouls,
            Channel::HomeCourt2Fouls,
            Channel::HomeCourt3Fouls,
            Channel::HomeCourt4Fouls,
            Channel::HomeCourt5Fouls,
        };
        const Channel awayNumberTens[5] = {
            Channel::AwayCourt1NumberTens,
            Channel::AwayCourt2NumberTens,
            Channel::AwayCourt3NumberTens,
            Channel::AwayCourt4NumberTens,
            Channel::AwayCourt5NumberTens,
        };
        const Channel awayNumberOnes[5] = {
            Channel::AwayCourt1NumberOnes,
            Channel::AwayCourt2NumberOnes,
            Channel::AwayCourt3NumberOnes,
            Channel::AwayCourt4NumberOnes,
            Channel::AwayCourt5NumberOnes,
        };
        const Channel awayPointsTens[5] = {
            Channel::AwayCourt1PointsTens,
            Channel::AwayCourt2PointsTens,
            Channel::AwayCourt3PointsTens,
            Channel::AwayCourt4PointsTens,
            Channel::AwayCourt5PointsTens,
        };
        const Channel awayPointsOnes[5] = {
            Channel::AwayCourt1PointsOnes,
            Channel::AwayCourt2PointsOnes,
            Channel::AwayCourt3PointsOnes,
            Channel::AwayCourt4PointsOnes,
            Channel::AwayCourt5PointsOnes,
        };
        const Channel awayFouls[5] = {
            Channel::AwayCourt1Fouls,
            Channel::AwayCourt2Fouls,
            Channel::AwayCourt3Fouls,
            Channel::AwayCourt4Fouls,
            Channel::AwayCourt5Fouls,
        };

        for (int i = 0; i < 5; ++i)
        {
            SetJerseyChannels(
                state.homePlayers[i].jerseyNumber,
                homeNumberTens[i],
                homeNumberOnes[i]
            );
            SetTwoDigitChannels(
                state.homePlayers[i].points,
                homePointsTens[i],
                homePointsOnes[i],
                true
            );
            SetSingleDigitChannel(
                state.homePlayers[i].fouls,
                homeFouls[i]
            );

            SetJerseyChannels(
                state.awayPlayers[i].jerseyNumber,
                awayNumberTens[i],
                awayNumberOnes[i]
            );
            SetTwoDigitChannels(
                state.awayPlayers[i].points,
                awayPointsTens[i],
                awayPointsOnes[i],
                true
            );
            SetSingleDigitChannel(
                state.awayPlayers[i].fouls,
                awayFouls[i]
            );
        }
    }


    static bool ReadLiveState(JumbotronState& out)
    {
        __try
        {
            auto* info =
                *reinterpret_cast<GdInfoCentral**>(
                    gGame->gdInfoCentral
                );

            auto* gdAI =
                *reinterpret_cast<GdAI**>(
                    gGame->gdAI
                );

            if (!info || !gdAI || !gdAI->__vtable)
                return false;

            auto getGameClock =
                reinterpret_cast<GetGameClockFn>(
                    gGame->getGameClock
                );

            auto isShotClockValid =
                reinterpret_cast<IsShotClockValidFn>(
                    gGame->isShotClockValid
                );

            auto getShotClock =
                reinterpret_cast<GetShotClockFn>(
                    gGame->getShotClock
                );

            auto getTeamScore =
                reinterpret_cast<GetTeamScoreFn>(
                    gGame->getTeamScore
                );

            auto getQuarter =
                reinterpret_cast<GetQuarterFn>(
                    gdAI->__vtable[gGame->getQuarterSlot]
                );

            auto isGameClockValid =
                reinterpret_cast<IsGameClockValidFn>(
                    gdAI->__vtable[gGame->isGameClockValidSlot]
                );

            out.homeScore = getTeamScore(info, 0);
            out.awayScore = getTeamScore(info, 1);

            out.gameClockValid = isGameClockValid(gdAI);

            unsigned int clockUnitsPerSecond = 60u;
            if (gGame->getClockUnitsPerSecond)
            {
                auto getClockUnitsPerSecond =
                    reinterpret_cast<GetClockUnitsPerSecondFn>(
                        gGame->getClockUnitsPerSecond
                    );
                clockUnitsPerSecond = getClockUnitsPerSecond(info);
                if (!clockUnitsPerSecond)
                    clockUnitsPerSecond = 60u;
            }

            const unsigned int gameRaw = getGameClock(info);
            const unsigned int gameTotalSeconds = gameRaw / clockUnitsPerSecond;
            out.gameMinutes = static_cast<int>(gameTotalSeconds / 60u);
            out.gameSeconds = static_cast<int>(gameTotalSeconds % 60u);

            out.shotClockValid = isShotClockValid(info);
            if (out.shotClockValid)
            {
                const unsigned int shotRaw = getShotClock(info);
                const unsigned int shotSeconds =
                    (shotRaw + clockUnitsPerSecond - 1u) /
                    clockUnitsPerSecond;
                out.shotClock = static_cast<int>(shotSeconds);
            }
            else
            {
                out.shotClock = 0;
            }

            const int rawQuarter = getQuarter(gdAI);
            out.period = rawQuarter >= 0 ? rawQuarter + 1 : 0;

            const IDTeam homeTeam = GetTeamIDFromSide(0);
            const IDTeam awayTeam = GetTeamIDFromSide(1);

            out.homeFouls =
                GetOptionalTeamValue(gGame->getTeamFouls, homeTeam);
            out.awayFouls =
                GetOptionalTeamValue(gGame->getTeamFouls, awayTeam);
            out.homeTimeouts =
                GetOptionalTeamValue(gGame->getTeamTimeoutsLeft, homeTeam);
            out.awayTimeouts =
                GetOptionalTeamValue(gGame->getTeamTimeoutsLeft, awayTeam);

            const int homeTeamId =
                homeTeam.value > 0 ? GetDatabaseTeamID(homeTeam) : 0;
            const int awayTeamId =
                awayTeam.value > 0 ? GetDatabaseTeamID(awayTeam) : 1;

            RefreshLiveGame(homeTeamId, awayTeamId);

            for (int i = 0; i < 5; ++i)
            {
                GetDisplayPlayer(0, i, out.homePlayers[i]);
                GetDisplayPlayer(1, i, out.awayPlayers[i]);
            }

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }


    static void LogStateIfChanged()
    {
        if (!gHaveState)
            return;

        if (gHaveLoggedState &&
            std::memcmp(
                &gState,
                &gLastLoggedState,
                sizeof(gState)
            ) == 0)
        {
            return;
        }

        Log(
            "state home=%d away=%d game=%d:%02d valid=%d shot=%d valid=%d period=%d homeTO=%d awayTO=%d homeF=%d awayF=%d",
            gState.homeScore,
            gState.awayScore,
            gState.gameMinutes,
            gState.gameSeconds,
            gState.gameClockValid ? 1 : 0,
            gState.shotClock,
            gState.shotClockValid ? 1 : 0,
            gState.period,
            gState.homeTimeouts,
            gState.awayTimeouts,
            gState.homeFouls,
            gState.awayFouls
        );

        gLastLoggedState = gState;
        gHaveLoggedState = true;
    }


    static void RefreshStateForDraw()
    {
        const DWORD now = GetTickCount();

        if (gHaveStateReadTick && now == gLastStateReadTick)
            return;

        gLastStateReadTick = now;
        gHaveStateReadTick = true;

        JumbotronState next = {};
        for (int i = 0; i < 5; ++i)
        {
            next.homePlayers[i].jerseyNumber = INT_MIN;
            next.homePlayers[i].points = INT_MIN;
            next.homePlayers[i].fouls = INT_MIN;
            next.awayPlayers[i].jerseyNumber = INT_MIN;
            next.awayPlayers[i].points = INT_MIN;
            next.awayPlayers[i].fouls = INT_MIN;
        }
        next.homeTimeouts = -1;
        next.awayTimeouts = -1;
        next.homeFouls = -1;
        next.awayFouls = -1;

        if (!ReadLiveState(next))
        {
            gHaveState = false;
            BlankAllChannels();
            return;
        }

        gState = next;
        gHaveState = true;

        BuildChannelSlots(gState);
        LogStateIfChanged();
    }


    // =====================================================================
    // Shared custom draw
    // =====================================================================

    static bool gLoggedFirstDraw[kChannelCount] = {};


    static void JumboDraw(
        Channel channel,
        void* drawContext,
        void* drawData)
    {
        auto nativeDraw =
            reinterpret_cast<ScrollTextureDimDrawFn>(
                gGame->scrollTextureDimDraw
            );

        if (!drawData)
        {
            nativeDraw(drawContext, drawData);
            return;
        }

        auto* externalBindings =
            *reinterpret_cast<ExternalBinding**>(
                reinterpret_cast<uint8_t*>(drawData) + 0x04
            );

        if (!externalBindings)
        {
            nativeDraw(drawContext, drawData);
            return;
        }


        RefreshStateForDraw();

        const int channelIndex =
            static_cast<int>(channel);

        ExternalBinding& uvBinding =
            externalBindings[3];

        void* const oldData =
            uvBinding.data;

        const uint32_t oldFlags =
            uvBinding.flags;


        // Jumbo materials own external #3 completely.
        // Clear bit 0: one Vector4, not a per-instance array.
        // Set bit 1: dirty/change indicator for the native draw path.
        uvBinding.data =
            gChannelUv[channelIndex];

        uvBinding.flags =
            (oldFlags & ~1u) | 2u;


        if (!gLoggedFirstDraw[channelIndex])
        {
            Log(
                "first draw channel=%d runtime=%s U=%.6f",
                channelIndex,
                kRuntimeNames[channelIndex],
                gChannelUv[channelIndex][0]
            );

            gLoggedFirstDraw[channelIndex] = true;
        }


        nativeDraw(
            drawContext,
            drawData
        );


        uvBinding.data =
            oldData;

        uvBinding.flags =
            oldFlags;
    }


    // =====================================================================
    // One tiny draw wrapper per logical runtime.
    // All wrappers enter the same JumboDraw implementation.
    // =====================================================================

#define DEFINE_JUMBO_DRAW_WRAPPER(functionName, channelName)              \
    static void __cdecl functionName(void* drawContext, void* drawData)  \
    {                                                                    \
        JumboDraw(Channel::channelName, drawContext, drawData);          \
    }

    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeHundreds, HomeHundreds)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeTens, HomeTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeOnes, HomeOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayHundreds, AwayHundreds)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayTens, AwayTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayOnes, AwayOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawGameMinTens, GameMinTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawGameMinOnes, GameMinOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawGameSecTens, GameSecTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawGameSecOnes, GameSecOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawShotTens, ShotTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawShotOnes, ShotOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawPeriod, Period)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeTimeouts, HomeTimeouts)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayTimeouts, AwayTimeouts)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeTeamFoulsTens, HomeTeamFoulsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeTeamFoulsOnes, HomeTeamFoulsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayTeamFoulsTens, AwayTeamFoulsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayTeamFoulsOnes, AwayTeamFoulsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt1NumberTens, HomeCourt1NumberTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt1NumberOnes, HomeCourt1NumberOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt1PointsTens, HomeCourt1PointsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt1PointsOnes, HomeCourt1PointsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt1Fouls, HomeCourt1Fouls)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt2NumberTens, HomeCourt2NumberTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt2NumberOnes, HomeCourt2NumberOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt2PointsTens, HomeCourt2PointsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt2PointsOnes, HomeCourt2PointsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt2Fouls, HomeCourt2Fouls)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt3NumberTens, HomeCourt3NumberTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt3NumberOnes, HomeCourt3NumberOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt3PointsTens, HomeCourt3PointsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt3PointsOnes, HomeCourt3PointsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt3Fouls, HomeCourt3Fouls)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt4NumberTens, HomeCourt4NumberTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt4NumberOnes, HomeCourt4NumberOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt4PointsTens, HomeCourt4PointsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt4PointsOnes, HomeCourt4PointsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt4Fouls, HomeCourt4Fouls)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt5NumberTens, HomeCourt5NumberTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt5NumberOnes, HomeCourt5NumberOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt5PointsTens, HomeCourt5PointsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt5PointsOnes, HomeCourt5PointsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawHomeCourt5Fouls, HomeCourt5Fouls)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt1NumberTens, AwayCourt1NumberTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt1NumberOnes, AwayCourt1NumberOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt1PointsTens, AwayCourt1PointsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt1PointsOnes, AwayCourt1PointsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt1Fouls, AwayCourt1Fouls)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt2NumberTens, AwayCourt2NumberTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt2NumberOnes, AwayCourt2NumberOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt2PointsTens, AwayCourt2PointsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt2PointsOnes, AwayCourt2PointsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt2Fouls, AwayCourt2Fouls)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt3NumberTens, AwayCourt3NumberTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt3NumberOnes, AwayCourt3NumberOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt3PointsTens, AwayCourt3PointsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt3PointsOnes, AwayCourt3PointsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt3Fouls, AwayCourt3Fouls)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt4NumberTens, AwayCourt4NumberTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt4NumberOnes, AwayCourt4NumberOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt4PointsTens, AwayCourt4PointsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt4PointsOnes, AwayCourt4PointsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt4Fouls, AwayCourt4Fouls)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt5NumberTens, AwayCourt5NumberTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt5NumberOnes, AwayCourt5NumberOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt5PointsTens, AwayCourt5PointsTens)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt5PointsOnes, AwayCourt5PointsOnes)
    DEFINE_JUMBO_DRAW_WRAPPER(DrawAwayCourt5Fouls, AwayCourt5Fouls)

#undef DEFINE_JUMBO_DRAW_WRAPPER


    static void* const kDrawWrappers[kChannelCount] =
    {
        reinterpret_cast<void*>(&DrawHomeHundreds),
        reinterpret_cast<void*>(&DrawHomeTens),
        reinterpret_cast<void*>(&DrawHomeOnes),
        reinterpret_cast<void*>(&DrawAwayHundreds),
        reinterpret_cast<void*>(&DrawAwayTens),
        reinterpret_cast<void*>(&DrawAwayOnes),
        reinterpret_cast<void*>(&DrawGameMinTens),
        reinterpret_cast<void*>(&DrawGameMinOnes),
        reinterpret_cast<void*>(&DrawGameSecTens),
        reinterpret_cast<void*>(&DrawGameSecOnes),
        reinterpret_cast<void*>(&DrawShotTens),
        reinterpret_cast<void*>(&DrawShotOnes),
        reinterpret_cast<void*>(&DrawPeriod),
        reinterpret_cast<void*>(&DrawHomeTimeouts),
        reinterpret_cast<void*>(&DrawAwayTimeouts),
        reinterpret_cast<void*>(&DrawHomeTeamFoulsTens),
        reinterpret_cast<void*>(&DrawHomeTeamFoulsOnes),
        reinterpret_cast<void*>(&DrawAwayTeamFoulsTens),
        reinterpret_cast<void*>(&DrawAwayTeamFoulsOnes),
        reinterpret_cast<void*>(&DrawHomeCourt1NumberTens),
        reinterpret_cast<void*>(&DrawHomeCourt1NumberOnes),
        reinterpret_cast<void*>(&DrawHomeCourt1PointsTens),
        reinterpret_cast<void*>(&DrawHomeCourt1PointsOnes),
        reinterpret_cast<void*>(&DrawHomeCourt1Fouls),
        reinterpret_cast<void*>(&DrawHomeCourt2NumberTens),
        reinterpret_cast<void*>(&DrawHomeCourt2NumberOnes),
        reinterpret_cast<void*>(&DrawHomeCourt2PointsTens),
        reinterpret_cast<void*>(&DrawHomeCourt2PointsOnes),
        reinterpret_cast<void*>(&DrawHomeCourt2Fouls),
        reinterpret_cast<void*>(&DrawHomeCourt3NumberTens),
        reinterpret_cast<void*>(&DrawHomeCourt3NumberOnes),
        reinterpret_cast<void*>(&DrawHomeCourt3PointsTens),
        reinterpret_cast<void*>(&DrawHomeCourt3PointsOnes),
        reinterpret_cast<void*>(&DrawHomeCourt3Fouls),
        reinterpret_cast<void*>(&DrawHomeCourt4NumberTens),
        reinterpret_cast<void*>(&DrawHomeCourt4NumberOnes),
        reinterpret_cast<void*>(&DrawHomeCourt4PointsTens),
        reinterpret_cast<void*>(&DrawHomeCourt4PointsOnes),
        reinterpret_cast<void*>(&DrawHomeCourt4Fouls),
        reinterpret_cast<void*>(&DrawHomeCourt5NumberTens),
        reinterpret_cast<void*>(&DrawHomeCourt5NumberOnes),
        reinterpret_cast<void*>(&DrawHomeCourt5PointsTens),
        reinterpret_cast<void*>(&DrawHomeCourt5PointsOnes),
        reinterpret_cast<void*>(&DrawHomeCourt5Fouls),
        reinterpret_cast<void*>(&DrawAwayCourt1NumberTens),
        reinterpret_cast<void*>(&DrawAwayCourt1NumberOnes),
        reinterpret_cast<void*>(&DrawAwayCourt1PointsTens),
        reinterpret_cast<void*>(&DrawAwayCourt1PointsOnes),
        reinterpret_cast<void*>(&DrawAwayCourt1Fouls),
        reinterpret_cast<void*>(&DrawAwayCourt2NumberTens),
        reinterpret_cast<void*>(&DrawAwayCourt2NumberOnes),
        reinterpret_cast<void*>(&DrawAwayCourt2PointsTens),
        reinterpret_cast<void*>(&DrawAwayCourt2PointsOnes),
        reinterpret_cast<void*>(&DrawAwayCourt2Fouls),
        reinterpret_cast<void*>(&DrawAwayCourt3NumberTens),
        reinterpret_cast<void*>(&DrawAwayCourt3NumberOnes),
        reinterpret_cast<void*>(&DrawAwayCourt3PointsTens),
        reinterpret_cast<void*>(&DrawAwayCourt3PointsOnes),
        reinterpret_cast<void*>(&DrawAwayCourt3Fouls),
        reinterpret_cast<void*>(&DrawAwayCourt4NumberTens),
        reinterpret_cast<void*>(&DrawAwayCourt4NumberOnes),
        reinterpret_cast<void*>(&DrawAwayCourt4PointsTens),
        reinterpret_cast<void*>(&DrawAwayCourt4PointsOnes),
        reinterpret_cast<void*>(&DrawAwayCourt4Fouls),
        reinterpret_cast<void*>(&DrawAwayCourt5NumberTens),
        reinterpret_cast<void*>(&DrawAwayCourt5NumberOnes),
        reinterpret_cast<void*>(&DrawAwayCourt5PointsTens),
        reinterpret_cast<void*>(&DrawAwayCourt5PointsOnes),
        reinterpret_cast<void*>(&DrawAwayCourt5Fouls)
    };


    // =====================================================================
    // Runtime clones / publication
    // =====================================================================

    alignas(16) static uint8_t
        gRuntimeClones[kChannelCount][kRenderMethodRuntimeSize] = {};

    static void* gDrawHandlerTables[kChannelCount][1] = {};
    static RMExportRecord gExports[kChannelCount] = {};

    // Backward-compatible temporary alias so a leftover [JumboTest]
    // material does not crash while converting the stadium to the expanded channel set.
    static RMExportRecord gJumboTestExport = {};

    static bool gRuntimeClonesReady = false;


    static void BuildRuntimeClones()
    {
        if (gRuntimeClonesReady)
            return;

        BlankAllChannels();

        const void* nativeRuntime =
            reinterpret_cast<const void*>(
                gGame->scrollTextureDimRuntime
            );


        for (int i = 0; i < kChannelCount; ++i)
        {
            std::memcpy(
                gRuntimeClones[i],
                nativeRuntime,
                kRenderMethodRuntimeSize
            );

            gDrawHandlerTables[i][0] =
                kDrawWrappers[i];

            *reinterpret_cast<void**>(
                gRuntimeClones[i] + 0x00
            ) = gDrawHandlerTables[i];


            gExports[i].exportName =
                kRuntimeNames[i];

            gExports[i].runtime =
                gRuntimeClones[i];
        }


        gJumboTestExport.exportName =
            "gJumboTest_RMRuntime";

        gJumboTestExport.runtime =
            gRuntimeClones[
                static_cast<int>(Channel::HomeOnes)
            ];


        gRuntimeClonesReady = true;

        Log(
            "%s: built %d Jumbo runtime clones from native %p",
            gGame->name,
            kChannelCount,
            reinterpret_cast<void*>(gGame->scrollTextureDimRuntime)
        );
    }


    static void PublishAll(const char* stage)
    {
        BuildRuntimeClones();

        auto getLoader =
            reinterpret_cast<GetLoaderContextFn>(
                gGame->getLoaderContext
            );

        void* loaderContext =
            getLoader();

        const uint8_t state =
            *reinterpret_cast<uint8_t*>(
                gGame->rmState
            );

        Log(
            "[%s] state=%u loader=%p",
            stage,
            static_cast<unsigned>(state),
            loaderContext
        );


        if (!loaderContext)
        {
            Log(
                "[%s] DynamicLoader not available yet.",
                stage
            );

            return;
        }


        auto publish =
            reinterpret_cast<PublishRuntimeFn>(
                gGame->publishRuntime
            );


        for (int i = 0; i < kChannelCount; ++i)
        {
            publish(
                &gExports[i],
                loaderContext
            );

            Log(
                "[%s] published %s -> %p",
                stage,
                gExports[i].exportName,
                gExports[i].runtime
            );
        }


        publish(
            &gJumboTestExport,
            loaderContext
        );

        Log(
            "[%s] published compatibility alias %s -> %p",
            stage,
            gJumboTestExport.exportName,
            gJumboTestExport.runtime
        );
    }


    static void __cdecl RunRMConstructorsHook()
    {
        Log(
            "RunRMConstructorsHook entered."
        );

        reinterpret_cast<RunRMConstructorsFn>(
            gGame->runRMConstructors
        )();

        PublishAll(
            "after-native-rm"
        );

        Log(
            "RunRMConstructorsHook finished."
        );
    }


    void Initialize()
    {
        InitializeDebugLogging();
        gGame = DetectGame();

        if (!gGame)
            return;

        Log(
            "========================================"
        );

        Log(
            "Jumbotron runtime selected: %s",
            gGame->name
        );

        BuildRuntimeClones();

        if (gGame->substitutionBuilderCall && gGame->substitutionBuilder)
        {
            const uintptr_t callAddress =
                gGame->substitutionBuilderCall;

            const unsigned char* instruction =
                reinterpret_cast<const unsigned char*>(callAddress);

            if (instruction[0] == 0xE8)
            {
                const int32_t displacement =
                    *reinterpret_cast<const int32_t*>(instruction + 1);

                const uintptr_t destination =
                    callAddress + 5 + displacement;

                if (destination == gGame->substitutionBuilder)
                {
                    gOriginalSubstitutionBuilder =
                        reinterpret_cast<SubstitutionBuilderFn>(
                            gGame->substitutionBuilder
                        );

                    patch::RedirectCall(
                        static_cast<unsigned int>(callAddress),
                        reinterpret_cast<void*>(&HookSubstitutionBuilder)
                    );

                    Log(
                        "Installed substitution builder hook at %08X -> %08X.",
                        static_cast<unsigned>(callAddress),
                        static_cast<unsigned>(gGame->substitutionBuilder)
                    );
                }
                else
                {
                    Log(
                        "Skipped substitution builder hook: call %08X points to %08X, expected %08X.",
                        static_cast<unsigned>(callAddress),
                        static_cast<unsigned>(destination),
                        static_cast<unsigned>(gGame->substitutionBuilder)
                    );
                }
            }
        }

        const uint8_t state =
            *reinterpret_cast<uint8_t*>(
                gGame->rmState
            );

        auto getLoader =
            reinterpret_cast<GetLoaderContextFn>(
                gGame->getLoaderContext
            );

        void* loaderContext =
            getLoader();


        Log(
            "----------------------------------------"
        );

        Log(
            "%s Jumbotron extended-channel init: state=%u loader=%p",
            gGame->name,
            static_cast<unsigned>(state),
            loaderContext
        );


        if (loaderContext)
        {
            PublishAll(
                "plugin-init"
            );
        }


        if (state == 0)
        {
            patch::RedirectCall(
                gGame->runRMConstructorsCall,
                RunRMConstructorsHook
            );

            Log(
                "Installed RunRMConstructors hook at %08X.",
                static_cast<unsigned>(
                    gGame->runRMConstructorsCall
                )
            );
        }
        else
        {
            PublishAll(
                "late-plugin-init"
            );
        }
    }
}


void InitializeJumbotronRuntime()
{
    jumbotron::Initialize();
}
