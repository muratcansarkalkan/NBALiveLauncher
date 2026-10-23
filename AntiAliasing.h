#pragma once

#include <d3d9.h>

// NBA Live 2005-08 D3D9 MSAA / supersampling.
void InitializeAntiAliasing();

// Optional native-resolution custom-overlay pass.
// Returns true only when SSAA is active and the supersampled game frame
// was resolved early to the native backbuffer.
bool BeginAntiAliasingNativeOverlayPass(IDirect3DDevice9* device);
void EndAntiAliasingNativeOverlayPass(IDirect3DDevice9* device);
float GetAntiAliasingNativeOverlayScale();
