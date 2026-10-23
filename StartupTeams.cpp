#include "plugin-std.h"
#include "Games.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

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
