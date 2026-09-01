#pragma once

#include <d3d9.h>

namespace popupfont {

struct Style {
    float scoreHeight;
    float clockHeight;
    float shotClockHeight;
    float teamNameHeight;
    float periodHeight;
    float foulHeight;
    float timeoutHeight;
    float bonusHeight;
    float characterSpacing;
    D3DCOLOR scoreColor;
    D3DCOLOR clockColor;
    D3DCOLOR shotClockColor;
};

// Creates a GDI-backed glyph atlas using the active popup's TTF file.
// Returns false only when even the Windows fallback font cannot be created.
bool Begin(IDirect3DDevice9* device, const char* themeName);
bool Reload(IDirect3DDevice9* device, const char* themeName);
const Style& GetStyle();

float Measure(const char* text, float height);
void DrawLeft(IDirect3DDevice9* device, const char* text,
              float x, float y, float height, D3DCOLOR color);
void DrawCentered(IDirect3DDevice9* device, const char* text,
                  float centerX, float y, float height, D3DCOLOR color);
void DrawRight(IDirect3DDevice9* device, const char* text,
               float right, float y, float height, D3DCOLOR color);

void Shutdown();

} // namespace popupfont
