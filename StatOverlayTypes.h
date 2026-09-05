#pragma once

#include "ShotClockGames.h"
#include <cstdint>

namespace statoverlay {

struct Subtype {
    const char* key;
    bool playerPayload;
    bool supported;
};

inline bool IsTeamSubtype(unsigned int subtype)
{
    if (subtype == 18 || (subtype >= 20 && subtype <= 26) ||
        (subtype >= 29 && subtype <= 53))
        return subtype != 27 && subtype != 28;
    return false;
}

inline const char* Key(GameVersion game, unsigned int subtype)
{
    switch (subtype) {
    case 0: return "player_assists";
    case 1: return "player_blocks";
    case 2: return "player_points_quarter";
    case 3: return "player_rebounds";
    case 4: return "player_steals";
    case 5:
        return game == GameVersion::Live2005 ||
            game == GameVersion::Live2006 ?
            "player_school" : "player_points_tonight";
    case 6: return "player_half_split";
    case 7: return "player_stat_line";
    case 8: return "player_bio";
    case 9: return "player_five_stats";
    case 10: return "player_foul";
    case 11: return "player_fouled_out";
    case 12: return "player_injury";
    case 13: return "player_injury_update";
    case 14: return "substitution";
    case 15: return "shooting_three_pointers";
    case 16: return "shooting_field_goals";
    case 17: return "shooting_free_throws";
    case 18: return "team_scoring_run";
    case 20: return "team_hustle_points";
    case 21: return "team_makes_quarter";
    case 22: return "team_misses_quarter";
    case 23: return "team_makes_game";
    case 24: return "team_misses_game";
    case 25: return "team_timeout";
    case 26: return "team_leaders";
    case 29: return "season_assists";
    case 30: return "season_blocks";
    case 31: return "season_rebounds";
    case 32: return "season_steals";
    case 33: return "team_three_pointers";
    case 34: return "team_bench_scoring";
    case 35: return "team_blocks";
    case 36: return "team_field_goals";
    case 37: return "team_field_goals_quarter";
    case 38: return "team_turnovers_quarter";
    case 39: return "team_rebounds";
    case 40: return "team_steals";
    case 41: return "team_turnovers";
    case 42: return "team_three_pointers_detail";
    case 43: return "team_field_goals_detail";
    case 44: return "team_stat_44";
    case 45: return "team_rebound_breakdown";
    case 46: return "team_defensive_rebounds";
    case 47: return "team_offensive_rebounds";
    case 48: return "team_turnovers_half";
    case 49: return "leaders_centers";
    case 50: return "leaders_point_guards";
    case 51: return "leaders_power_forwards";
    case 52: return "leaders_shooting_guards";
    case 53: return "leaders_small_forwards";
    case 55: return "one_on_one_end";
    default: return "unidentified";
    }
}

inline bool IsSupported(GameVersion game, unsigned int control18,
                        unsigned int subtype)
{
    switch (game) {
    case GameVersion::Live2005:
        if (control18 == 1)
            return subtype <= 17 || subtype == 55;
        return control18 == 0 && IsTeamSubtype(subtype) && subtype != 44;

    case GameVersion::Live2006:
        if (control18 == 1)
            return subtype <= 17;
        return control18 == 0 && IsTeamSubtype(subtype) && subtype != 44;

    case GameVersion::Live2007:
        if (control18 == 1)
            return (subtype <= 16 && subtype != 8) || subtype == 55;
        return control18 == 0 && IsTeamSubtype(subtype) && subtype != 44;

    case GameVersion::Live2008:
        if (control18 == 1)
            return subtype <= 14 || subtype == 55;
        return control18 == 0 &&
            ((subtype >= 15 && subtype <= 17) || IsTeamSubtype(subtype));
    }
    return false;
}

inline Subtype Resolve(GameVersion game, unsigned int control18,
                       unsigned int control1C)
{
    const bool supported = IsSupported(game, control18, control1C);
    Subtype result = {
        supported ? Key(game, control1C) : "unidentified",
        control18 == 1,
        supported
    };
    return result;
}

} // namespace statoverlay
