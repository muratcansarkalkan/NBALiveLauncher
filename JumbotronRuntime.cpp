#include "plugin-std.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

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

        97,         // GdAI::GetQuarter vtable slot
        98          // GdAI::IsGameClockValid vtable slot
    };


    static const GameAddresses* gGame = nullptr;

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

        "gJumboPeriod_RMRuntime"
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
        FILE* f = nullptr;

        if (fopen_s(&f, "JumboRuntime.log", "a") != 0 || !f)
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

    struct JumbotronState
    {
        int homeScore;
        int awayScore;

        int gameMinutes;
        int gameSeconds;

        int shotClock;

        // User-facing period: 1..4, >=5 means OT.
        int period;

        bool gameClockValid;
        bool shotClockValid;
    };

    static JumbotronState gState = {};
    static JumbotronState gLastLoggedState = {};
    static bool gHaveState = false;
    static bool gHaveLoggedState = false;

    // One persistent Vector4 per material/channel.
    // x = U offset, y = V offset, z/w = full native scale/intensity.
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


    static void SetScoreChannels(
        int score,
        Channel hundreds,
        Channel tens,
        Channel ones)
    {
        if (score < 0)
        {
            SetChannelSlot(hundreds, kSlotBlank);
            SetChannelSlot(tens, kSlotBlank);
            SetChannelSlot(ones, kSlotBlank);
            return;
        }

        score = ClampInt(score, 0, 999);

        const int h = (score / 100) % 10;
        const int t = (score / 10) % 10;
        const int o = score % 10;

        SetChannelSlot(
            hundreds,
            score >= 100 ? h : kSlotBlank
        );

        SetChannelSlot(
            tens,
            score >= 10 ? t : kSlotBlank
        );

        SetChannelSlot(
            ones,
            o
        );
    }


    static void BuildChannelSlots(const JumbotronState& state)
    {
        SetScoreChannels(
            state.homeScore,
            Channel::HomeHundreds,
            Channel::HomeTens,
            Channel::HomeOnes
        );

        SetScoreChannels(
            state.awayScore,
            Channel::AwayHundreds,
            Channel::AwayTens,
            Channel::AwayOnes
        );


        if (state.gameClockValid)
        {
            const int minutes =
                ClampInt(state.gameMinutes, 0, 99);

            const int seconds =
                ClampInt(state.gameSeconds, 0, 59);

            SetChannelSlot(
                Channel::GameMinTens,
                minutes >= 10
                    ? (minutes / 10) % 10
                    : kSlotBlank
            );

            SetChannelSlot(
                Channel::GameMinOnes,
                minutes % 10
            );

            SetChannelSlot(
                Channel::GameSecTens,
                (seconds / 10) % 10
            );

            SetChannelSlot(
                Channel::GameSecOnes,
                seconds % 10
            );
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
            const int shot =
                ClampInt(state.shotClock, 0, 99);

            SetChannelSlot(
                Channel::ShotTens,
                shot >= 10
                    ? (shot / 10) % 10
                    : kSlotBlank
            );

            SetChannelSlot(
                Channel::ShotOnes,
                shot % 10
            );
        }
        else
        {
            SetChannelSlot(Channel::ShotTens, kSlotBlank);
            SetChannelSlot(Channel::ShotOnes, kSlotBlank);
        }


        if (state.period >= 1 && state.period <= 4)
        {
            SetChannelSlot(
                Channel::Period,
                state.period
            );
        }
        else if (state.period >= 5)
        {
            SetChannelSlot(
                Channel::Period,
                kSlotOT
            );
        }
        else
        {
            SetChannelSlot(
                Channel::Period,
                kSlotBlank
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


            out.homeScore =
                getTeamScore(info, 0);

            out.awayScore =
                getTeamScore(info, 1);


            out.gameClockValid =
                isGameClockValid(gdAI);

            unsigned int clockUnitsPerSecond = 60u;

            if (gGame->getClockUnitsPerSecond)
            {
                auto getClockUnitsPerSecond =
                    reinterpret_cast<GetClockUnitsPerSecondFn>(
                        gGame->getClockUnitsPerSecond
                    );

                clockUnitsPerSecond =
                    getClockUnitsPerSecond(info);

                if (!clockUnitsPerSecond)
                    clockUnitsPerSecond = 60u;
            }


            const unsigned int gameRaw =
                getGameClock(info);

            const unsigned int gameTotalSeconds =
                gameRaw / clockUnitsPerSecond;

            out.gameMinutes =
                static_cast<int>(gameTotalSeconds / 60u);

            out.gameSeconds =
                static_cast<int>(gameTotalSeconds % 60u);


            out.shotClockValid =
                isShotClockValid(info);

            if (out.shotClockValid)
            {
                const unsigned int shotRaw =
                    getShotClock(info);

                // Match the proven scoreboard path: shot clock rounds up.
                const unsigned int shotSeconds =
                    (shotRaw + clockUnitsPerSecond - 1u) /
                    clockUnitsPerSecond;

                out.shotClock =
                    static_cast<int>(shotSeconds);
            }
            else
            {
                out.shotClock = 0;
            }


            // GetQuarter is zero-based in the existing 05-08 scoreboard
            // implementation: raw 0 = 1st period.
            const int rawQuarter =
                getQuarter(gdAI);

            out.period =
                rawQuarter >= 0
                    ? rawQuarter + 1
                    : 0;

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
            "state home=%d away=%d game=%d:%02d valid=%d "
            "shot=%d valid=%d period=%d",
            gState.homeScore,
            gState.awayScore,
            gState.gameMinutes,
            gState.gameSeconds,
            gState.gameClockValid ? 1 : 0,
            gState.shotClock,
            gState.shotClockValid ? 1 : 0,
            gState.period
        );

        gLastLoggedState = gState;
        gHaveLoggedState = true;
    }


    static void RefreshStateForDraw()
    {
        // All 13 materials are normally drawn within the same system-timer
        // tick. This prevents 13 duplicate gameplay getter passes while still
        // keeping replay-aware game/shot clocks responsive.
        const DWORD now = GetTickCount();

        if (gHaveStateReadTick && now == gLastStateReadTick)
            return;

        gLastStateReadTick = now;
        gHaveStateReadTick = true;

        JumbotronState next = {};

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
    // All 13 wrappers enter the same JumboDraw implementation.
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

        reinterpret_cast<void*>(&DrawPeriod)
    };


    // =====================================================================
    // Runtime clones / publication
    // =====================================================================

    alignas(16) static uint8_t
        gRuntimeClones[kChannelCount][kRenderMethodRuntimeSize] = {};

    static void* gDrawHandlerTables[kChannelCount][1] = {};
    static RMExportRecord gExports[kChannelCount] = {};

    // Backward-compatible temporary alias so a leftover [JumboTest]
    // material does not crash while converting the stadium to 13 channels.
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
            "%s Jumbotron 13-channel init: state=%u loader=%p",
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
