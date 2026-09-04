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
    int quarter;
    int awayFouls;
    int homeFouls;
    int awayTimeouts;
    int homeTimeouts;
    int awayDatabaseTeamID;
    int homeDatabaseTeamID;
    D3DCOLOR awayColor;
    D3DCOLOR homeColor;
    D3DCOLOR awaySecondaryColor;
    D3DCOLOR homeSecondaryColor;
    IDirect3DTexture9* awayLogo;
    IDirect3DTexture9* homeLogo;
    const char* awayTeamName;
    const char* homeTeamName;
    const char* violationTitle;
    const char* violationPossession;
    const char* violationTeamName;
    D3DCOLOR violationTeamColor;
    IDirect3DTexture9* violationTeamLogo;
};

void Render(IDirect3DDevice9* device, const Frame& frame,
            const char* overlayName);
void RenderViolation(IDirect3DDevice9* device, const Frame& frame,
    const char* overlayName, float animationOffsetX,
    float animationOffsetY, float animationOpacity);

} // namespace scoreboard
