#include "ScoreboardRenderer.h"

#include <cstdio>
#include <cstring>

namespace scoreboard {
namespace {

struct ScreenVertex {
    float x, y, z, rhw;
    D3DCOLOR color;
};

void DrawFilledRect(IDirect3DDevice9* device, float left, float top,
                    float right, float bottom, D3DCOLOR color)
{
    const ScreenVertex vertices[4] = {
        { left,  top,    0.0f, 1.0f, color },
        { right, top,    0.0f, 1.0f, color },
        { left,  bottom, 0.0f, 1.0f, color },
        { right, bottom, 0.0f, 1.0f, color }
    };
    device->SetTexture(0, nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices,
        sizeof(ScreenVertex));
}

struct TextureVertex {
    float x, y, z, rhw;
    D3DCOLOR color;
    float u, v;
};

void DrawTexture(IDirect3DDevice9* device, IDirect3DTexture9* texture,
                 float left, float top, float right, float bottom)
{
    if (!texture) return;
    D3DSURFACE_DESC description = {};
    if (FAILED(texture->GetLevelDesc(0, &description)) ||
        !description.Width || !description.Height)
        return;

    const float availableWidth = right - left;
    const float availableHeight = bottom - top;
    const float imageAspect = static_cast<float>(description.Width) /
        static_cast<float>(description.Height);
    const float boxAspect = availableWidth / availableHeight;
    if (imageAspect > boxAspect) {
        const float height = availableWidth / imageAspect;
        top += (availableHeight - height) * 0.5f;
        bottom = top + height;
    }
    else {
        const float width = availableHeight * imageAspect;
        left += (availableWidth - width) * 0.5f;
        right = left + width;
    }

    const TextureVertex vertices[4] = {
        { left,  top,    0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f },
        { right, top,    0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f },
        { left,  bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f },
        { right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f }
    };
    device->SetTexture(0, texture);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices,
        sizeof(TextureVertex));
}

void DrawSevenSegmentDigit(IDirect3DDevice9* device, int digit,
                           float x, float y, float width, float height,
                           float thickness, D3DCOLOR color)
{
    static const unsigned char masks[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66,
        0x6D, 0x7D, 0x07, 0x7F, 0x6F
    };
    if (digit < 0 || digit > 9) return;
    const unsigned char mask = masks[digit];
    const float middle = y + height * 0.5f;
    if (mask & 0x01)
        DrawFilledRect(device, x, y, x + width, y + thickness, color);
    if (mask & 0x02)
        DrawFilledRect(device, x + width - thickness, y,
            x + width, middle, color);
    if (mask & 0x04)
        DrawFilledRect(device, x + width - thickness, middle,
            x + width, y + height, color);
    if (mask & 0x08)
        DrawFilledRect(device, x, y + height - thickness,
            x + width, y + height, color);
    if (mask & 0x10)
        DrawFilledRect(device, x, middle, x + thickness,
            y + height, color);
    if (mask & 0x20)
        DrawFilledRect(device, x, y, x + thickness, middle, color);
    if (mask & 0x40)
        DrawFilledRect(device, x, middle - thickness * 0.5f,
            x + width, middle + thickness * 0.5f, color);
}

float DrawInteger(IDirect3DDevice9* device, unsigned int value,
                  float right, float y, float digitWidth, float digitHeight,
                  float spacing, D3DCOLOR color)
{
    char text[16];
    std::sprintf(text, "%u", value);
    const size_t length = std::strlen(text);
    const float total = length * digitWidth + (length - 1) * spacing;
    float x = right - total;
    for (size_t i = 0; i < length; ++i) {
        DrawSevenSegmentDigit(device, text[i] - '0', x, y,
            digitWidth, digitHeight, 3.0f, color);
        x += digitWidth + spacing;
    }
    return right - total;
}

float ClockCharacterWidth(char character, float digitWidth)
{
    if (character == ':') return 7.0f;
    if (character == '.') return 6.0f;
    return digitWidth;
}

void DrawClockText(IDirect3DDevice9* device, const char* text,
                   float centerX, float y, D3DCOLOR color)
{
    const float digitWidth = 13.0f;
    const float digitHeight = 28.0f;
    const float spacing = 4.0f;
    float total = 0.0f;
    for (const char* p = text; *p; ++p)
        total += ClockCharacterWidth(*p, digitWidth) + spacing;
    total -= spacing;

    float x = centerX - total * 0.5f;
    for (const char* p = text; *p; ++p) {
        if (*p == ':') {
            DrawFilledRect(device, x + 2.0f, y + 8.0f,
                x + 5.0f, y + 11.0f, color);
            DrawFilledRect(device, x + 2.0f, y + 19.0f,
                x + 5.0f, y + 22.0f, color);
        }
        else if (*p == '.') {
            DrawFilledRect(device, x + 1.0f, y + digitHeight - 4.0f,
                x + 5.0f, y + digitHeight, color);
        }
        else {
            DrawSevenSegmentDigit(device, *p - '0', x, y,
                digitWidth, digitHeight, 3.0f, color);
        }
        x += ClockCharacterWidth(*p, digitWidth) + spacing;
    }
}

void DrawGameClock(IDirect3DDevice9* device, unsigned int rawClock,
                   unsigned int unitsPerSecond, float centerX, float y,
                   D3DCOLOR color)
{
    char text[16];
    const unsigned int totalTenths =
        (rawClock * 10u + unitsPerSecond - 1u) / unitsPerSecond;
    if (totalTenths < 600u) {
        std::sprintf(text, "%u.%u",
            totalTenths / 10u, totalTenths % 10u);
    }
    else {
        const unsigned int totalSeconds = rawClock / unitsPerSecond;
        std::sprintf(text, "%u:%02u",
            totalSeconds / 60u, totalSeconds % 60u);
    }
    DrawClockText(device, text, centerX, y, color);
}

} // namespace

void Render(IDirect3DDevice9* device, const Frame& frame)
{
    if (!device || !frame.clockUnitsPerSecond ||
        frame.homeScore < 0 || frame.awayScore < 0)
        return;

    D3DVIEWPORT9 viewport = {};
    if (FAILED(device->GetViewport(&viewport))) return;

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)))
        stateBlock->Capture();

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

    const float width = viewport.Width >= 900 ? 620.0f : 520.0f;
    const float height = 68.0f;
    const float left = (static_cast<float>(viewport.Width) - width) * 0.5f;
    const float top = 18.0f;
    const float right = left + width;
    const float bottom = top + height;
    const float teamWidth = width * 0.30f;
    const float scoreWidth = width * 0.10f;
    const float centerLeft = left + teamWidth + scoreWidth;
    const float centerRight = right - teamWidth - scoreWidth;

    DrawFilledRect(device, left, top, right, bottom,
        D3DCOLOR_ARGB(232, 15, 18, 24));
    DrawFilledRect(device, left, top, left + teamWidth, bottom,
        frame.awayColor);
    DrawFilledRect(device, right - teamWidth, top, right, bottom,
        frame.homeColor);
    const float accentHeight = 6.0f;
    DrawFilledRect(device, left, bottom - accentHeight,
        left + teamWidth, bottom, frame.awaySecondaryColor);
    DrawFilledRect(device, right - teamWidth, bottom - accentHeight,
        right, bottom, frame.homeSecondaryColor);
    DrawFilledRect(device, centerLeft, top, centerRight, bottom,
        D3DCOLOR_ARGB(255, 24, 28, 36));

    const unsigned int shotSeconds =
        (frame.shotClockRaw + frame.clockUnitsPerSecond - 1u) /
        frame.clockUnitsPerSecond;
    const D3DCOLOR white = D3DCOLOR_XRGB(255, 255, 255);
    DrawInteger(device, static_cast<unsigned int>(frame.awayScore),
        centerLeft - 12.0f, top + 17.0f, 16.0f, 34.0f, 5.0f, white);
    DrawInteger(device, static_cast<unsigned int>(frame.homeScore),
        right - teamWidth - 12.0f, top + 17.0f,
        16.0f, 34.0f, 5.0f, white);
    DrawGameClock(device, frame.gameClockRaw, frame.clockUnitsPerSecond,
        (centerLeft + centerRight) * 0.5f, top + 10.0f, white);
    if (frame.shotClockValid)
        DrawInteger(device, shotSeconds, centerRight - 8.0f,
            top + 43.0f, 8.0f, 17.0f, 2.0f,
            D3DCOLOR_XRGB(255, 210, 64));

    // Team logos are aspect-fitted and preserve PNG transparency.
    DrawTexture(device, frame.awayLogo,
        left + 12.0f, top + 7.0f,
        left + teamWidth - 12.0f, bottom - accentHeight - 5.0f);
    DrawTexture(device, frame.homeLogo,
        right - teamWidth + 12.0f, top + 7.0f,
        right - 12.0f, bottom - accentHeight - 5.0f);

    if (stateBlock) {
        stateBlock->Apply();
        stateBlock->Release();
    }
}

} // namespace scoreboard
