#pragma once

#include <d3d9.h>

namespace scoreboard {

struct Frame {
    unsigned int gameClockRaw;
    unsigned int clockUnitsPerSecond;
    bool shotClockValid;
    unsigned int shotClockRaw;
    int awayScore;
    int homeScore;
    int awayDatabaseTeamID;
    int homeDatabaseTeamID;
    D3DCOLOR awayColor;
    D3DCOLOR homeColor;
    D3DCOLOR awaySecondaryColor;
    D3DCOLOR homeSecondaryColor;
    IDirect3DTexture9* awayLogo;
    IDirect3DTexture9* homeLogo;
};

void Render(IDirect3DDevice9* device, const Frame& frame);

} // namespace scoreboard
