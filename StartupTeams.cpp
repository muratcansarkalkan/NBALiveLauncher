#include "plugin-std.h"
#include "Games.h"

#include <cstdio>
#include <cstring>

using namespace plugin;

namespace
{
    void SetStartupTeam(unsigned int address, int teamNum)
    {
        char query[32];
        sprintf_s(query, sizeof(query), " TEAMNUM = %d ", teamNum);

        // Write the new query including the null terminator.
        for (size_t i = 0; i <= strlen(query); ++i)
            patch::SetChar(address + static_cast<unsigned int>(i), query[i]);
    }
}

void InitializeStartupTeams()
{
    // -1 means leave the game's original startup team unchanged.
    int homeTeamNum = GetPrivateProfileIntA(
        "STARTUP",
        "HOME_TEAMNUM",
        -1,
        ".\\main.ini"
    );

    int awayTeamNum = GetPrivateProfileIntA(
        "STARTUP",
        "AWAY_TEAMNUM",
        -1,
        ".\\main.ini"
    );

    // Nothing configured. Keep the original game behavior.
    if (homeTeamNum < 0 && awayTeamNum < 0)
        return;

    unsigned int homeAddress = 0;
    unsigned int awayAddress = 0;

    switch (FM::GetEntryPoint())
    {
    case 0xCD8005: // NBA Live 2005 1.0 NOCD
        homeAddress = 0x00B41010; // " CITYNAME = 'Detroit' "
        awayAddress = 0x00B41028; // " CITYNAME = 'LA Lakers' "
        break;

    case 0x40109F:
        if (patch::GetFloat(0xBD832C) == 1.3333334f)
        {
            // NBA Live 06 1.0 NOCD
            homeAddress = 0x00BAC954; // " TEAMNAME = 'Spurs' "
            awayAddress = 0x00BAC96C; // " TEAMNAME = 'Pistons' "
        }
        else if (patch::GetFloat(0xBBBC3C) == 1.3333334f)
        {
            // NBA Live 07 1.1 NOCD
            homeAddress = 0x00BC98C0; // " TEAMNAME = 'Heat' "
            awayAddress = 0x00BC98D4; // " TEAMNAME = 'Mavericks' "
        }
        else if (patch::GetFloat(0xC3DF84) == 1.3333334f)
        {
            // NBA Live 08 1.0 NOCD
            homeAddress = 0x00C5496C; // " TEAMNAME = 'Spurs' "
            awayAddress = 0x00C54984; // " TEAMNAME = 'Cavaliers' "
        }
        break;
    }

    if (!homeAddress || !awayAddress)
        return;

    if (homeTeamNum >= 0)
        SetStartupTeam(homeAddress, homeTeamNum);

    if (awayTeamNum >= 0)
        SetStartupTeam(awayAddress, awayTeamNum);
}