#pragma once

#include <d3d9.h>

namespace popup {

struct TeamVisual {
    int databaseTeamID;
    char teamName[64];
    char cityName[64];
    char abbreviation[16];
    char shortCode[16];
    char logoPath[MAX_PATH];
    D3DCOLOR primaryColor;
    D3DCOLOR secondaryColor;
};

// Loads <game directory>\popups\<themeName>\teams.json.
// It is safe to call repeatedly; the file is read only once per theme.
bool Load(const char* themeName);
bool Reload(const char* themeName);
const TeamVisual* FindTeam(int databaseTeamID);

// Loaded lazily from TeamVisual::logoPath and cached per D3D device.
IDirect3DTexture9* GetLogoTexture(
    IDirect3DDevice9* device, int databaseTeamID);

// Human-readable reason for the most recent theme/logo loading failure.
const char* GetLastError();

void Shutdown();

} // namespace popup
