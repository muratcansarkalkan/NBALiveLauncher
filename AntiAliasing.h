#pragma once

// NBA Live 06 native D3D9 MSAA.
// Hooks Direct3DCreate9 once, then intercepts every CreateDevice and Reset.
void InitializeAntiAliasing();
