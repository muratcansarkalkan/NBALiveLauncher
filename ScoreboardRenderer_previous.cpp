#include "ScoreboardRenderer.h"
#include "PopupFont.h"
#include "PopupTheme.h"
#include "ScoreboardConfig.h"

#include <cstdio>
#include <cstring>
#include <cmath>

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

void DrawGradientRect(IDirect3DDevice9* device, float left, float top,
                      float right, float bottom, D3DCOLOR start,
                      D3DCOLOR end, bool horizontal)
{
    const D3DCOLOR tl = start;
    const D3DCOLOR tr = horizontal ? end : start;
    const D3DCOLOR bl = horizontal ? start : end;
    const D3DCOLOR br = end;
    const ScreenVertex vertices[4] = {
        { left, top, 0.0f, 1.0f, tl }, { right, top, 0.0f, 1.0f, tr },
        { left, bottom, 0.0f, 1.0f, bl }, { right, bottom, 0.0f, 1.0f, br }
    };
    device->SetTexture(0, nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices,
        sizeof(ScreenVertex));
}

void DrawFilledCircle(IDirect3DDevice9* device, float centerX,
                      float centerY, float radius, D3DCOLOR color)
{
    constexpr int SEGMENTS = 20;
    constexpr float TWO_PI = 6.28318530717958647692f;
    ScreenVertex vertices[SEGMENTS + 2] = {};
    vertices[0] = { centerX, centerY, 0.0f, 1.0f, color };
    for (int i = 0; i <= SEGMENTS; ++i) {
        const float angle = TWO_PI * static_cast<float>(i) /
            static_cast<float>(SEGMENTS);
        vertices[i + 1] = {
            centerX + std::cos(angle) * radius,
            centerY + std::sin(angle) * radius,
            0.0f, 1.0f, color
        };
    }
    device->SetTexture(0, nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, SEGMENTS, vertices,
        sizeof(ScreenVertex));
}

struct TextureVertex {
    float x, y, z, rhw;
    D3DCOLOR color;
    float u, v;
};

void DrawTexture(IDirect3DDevice9* device, IDirect3DTexture9* texture,
                 float left, float top, float right, float bottom,
                 D3DCOLOR color = 0xFFFFFFFFu)
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
        { left,  top,    0.0f, 1.0f, color, 0.0f, 0.0f },
        { right, top,    0.0f, 1.0f, color, 1.0f, 0.0f },
        { left,  bottom, 0.0f, 1.0f, color, 0.0f, 1.0f },
        { right, bottom, 0.0f, 1.0f, color, 1.0f, 1.0f }
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

void DrawTextureStretched(IDirect3DDevice9* device,
                          IDirect3DTexture9* texture,
                          float left, float top, float right, float bottom,
                          D3DCOLOR color)
{
    if (!texture) return;
    const TextureVertex vertices[4] = {
        { left,  top,    0.0f, 1.0f, color, 0.0f, 0.0f },
        { right, top,    0.0f, 1.0f, color, 1.0f, 0.0f },
        { left,  bottom, 0.0f, 1.0f, color, 0.0f, 1.0f },
        { right, bottom, 0.0f, 1.0f, color, 1.0f, 1.0f }
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

scoreboardconfig::Rect ToScreen(const scoreboardconfig::Rect& source,
                                float originX, float originY, float scale)
{
    scoreboardconfig::Rect result = {
        originX + source.x * scale,
        originY + source.y * scale,
        source.width * scale,
        source.height * scale
    };
    return result;
}

void FormatPeriod(int zeroBasedQuarter,
                  scoreboardconfig::PeriodFormat format,
                  char* output, size_t capacity)
{
    if (zeroBasedQuarter >= 4) {
        const int overtime = zeroBasedQuarter - 3;
        if (overtime == 1) std::snprintf(output, capacity, "OT");
        else std::snprintf(output, capacity, "%dOT", overtime);
        return;
    }
    const int quarter = zeroBasedQuarter + 1;
    const char* ordinal = quarter == 1 ? "1st" : quarter == 2 ?
        "2nd" : quarter == 3 ? "3rd" : "4th";
    switch (format) {
    case scoreboardconfig::PeriodFormat::Number:
        std::snprintf(output, capacity, "%d", quarter); break;
    case scoreboardconfig::PeriodFormat::OrdinalQuarter:
        std::snprintf(output, capacity, "%s Qtr", ordinal); break;
    case scoreboardconfig::PeriodFormat::ShortQuarter:
        std::snprintf(output, capacity, "Q%d", quarter); break;
    case scoreboardconfig::PeriodFormat::LongQuarter:
        std::snprintf(output, capacity, "%s Quarter", ordinal); break;
    default:
        std::snprintf(output, capacity, "%s", ordinal); break;
    }
}

void DrawBoundText(IDirect3DDevice9* device, const char* text,
                   const scoreboardconfig::Rect& rectangle,
                   float height, D3DCOLOR color, int alignment,
                   bool fit = false)
{
    // The rectangle is an alignment box, not a font-size ceiling. Keeping
    // the requested height allows the editor's score/clock/shot-clock font
    // controls to grow text beyond the original default element bounds.
    float actualHeight = height > 0.0f ? height : rectangle.height;
    if (fit && rectangle.height > 0.0f && actualHeight > rectangle.height)
        actualHeight = rectangle.height;
    if (fit && rectangle.width > 0.0f) {
        const float measured = popupfont::Measure(text, actualHeight);
        if (measured > rectangle.width && measured > 0.0f)
            actualHeight *= rectangle.width / measured;
    }
    const float y = rectangle.y +
        (rectangle.height - actualHeight) * 0.5f;
    if (alignment < 0)
        popupfont::DrawLeft(device, text, rectangle.x, y,
            actualHeight, color);
    else if (alignment > 0)
        popupfont::DrawRight(device, text,
            rectangle.x + rectangle.width, y, actualHeight, color);
    else
        popupfont::DrawCentered(device, text,
            rectangle.x + rectangle.width * 0.5f,
            y, actualHeight, color);
}

D3DCOLOR WithOpacity(D3DCOLOR color, int opacity)
{
    if (opacity < 0) opacity = 0;
    if (opacity > 255) opacity = 255;
    return (static_cast<D3DCOLOR>(opacity) << 24) | (color & 0x00FFFFFFu);
}

D3DCOLOR ResolveLayerColor(const char* binding, D3DCOLOR fallback,
                           const scoreboard::Frame& frame, int opacity)
{
    D3DCOLOR color = fallback;
    if (binding && std::strcmp(binding, "away.primaryColor") == 0)
        color = frame.awayColor;
    else if (binding && std::strcmp(binding, "home.primaryColor") == 0)
        color = frame.homeColor;
    else if (binding && std::strcmp(binding, "away.secondaryColor") == 0)
        color = frame.awaySecondaryColor;
    else if (binding && std::strcmp(binding, "home.secondaryColor") == 0)
        color = frame.homeSecondaryColor;
    return WithOpacity(color, opacity);
}

void DrawIndicator(IDirect3DDevice9* device,
                   scoreboardconfig::IndicatorMode mode,
                   const scoreboardconfig::Rect& rectangle,
                   int value, int maximum, const char* label,
                   IDirect3DTexture9* image, D3DCOLOR color,
                   float textHeight);

bool ResolveLayerText(const scoreboardconfig::Element& element,
                      const scoreboardconfig::Config& config,
                      const scoreboard::Frame& frame,
                      char* output, size_t capacity,
                      float* defaultHeight,
                      const popupfont::Style& style,
                      D3DCOLOR* color)
{
    const char* b = element.binding;
    *color = style.scoreColor;
    *defaultHeight = style.teamNameHeight;
    if (!b[0]) { std::snprintf(output, capacity, "%s", element.text); return true; }
    if (std::strcmp(b, "away.score") == 0) {
        std::snprintf(output, capacity, "%d", frame.awayScore);
        *defaultHeight = style.scoreHeight; return true;
    }
    if (std::strcmp(b, "home.score") == 0) {
        std::snprintf(output, capacity, "%d", frame.homeScore);
        *defaultHeight = style.scoreHeight; return true;
    }
    if (std::strcmp(b, "game.clock") == 0) {
        const unsigned int tenths = (frame.gameClockRaw * 10u +
            frame.clockUnitsPerSecond - 1u) / frame.clockUnitsPerSecond;
        if (tenths < 600u) std::snprintf(output, capacity, "%u.%u",
            tenths / 10u, tenths % 10u);
        else { const unsigned int seconds = frame.gameClockRaw /
            frame.clockUnitsPerSecond; std::snprintf(output, capacity,
            "%u:%02u", seconds / 60u, seconds % 60u); }
        *defaultHeight = style.clockHeight; *color = style.clockColor; return true;
    }
    if (std::strcmp(b, "game.shotClock") == 0) {
        const unsigned int seconds = (frame.shotClockRaw +
            frame.clockUnitsPerSecond - 1u) / frame.clockUnitsPerSecond;
        bool show = frame.shotClockValid && config.shotClockVisibility !=
            scoreboardconfig::ShotClockVisibility::Never;
        if (show && config.shotClockVisibility ==
            scoreboardconfig::ShotClockVisibility::UnderThreshold)
            show = seconds <= config.shotClockThreshold;
        if (!show) return false;
        std::snprintf(output, capacity, "%u", seconds);
        *defaultHeight = style.shotClockHeight;
        *color = seconds <= config.urgentShotClockThreshold ?
            config.shotClockUrgentColor : config.shotClockNormalColor;
        return true;
    }
    if (std::strcmp(b, "game.period") == 0) {
        FormatPeriod(frame.quarter, config.periodFormat, output, capacity);
        *defaultHeight = style.periodHeight; *color = style.clockColor; return true;
    }
    if (std::strcmp(b, "away.name") == 0 ||
        std::strcmp(b, "home.name") == 0) {
        std::snprintf(output, capacity, "%s",
            b[0] == 'a' ? frame.awayTeamName : frame.homeTeamName);
        return true;
    }
    const bool awayBonus = std::strcmp(b, "away.bonus") == 0;
    const bool homeBonus = std::strcmp(b, "home.bonus") == 0;
    if (awayBonus || homeBonus) {
        const int fouls = awayBonus ? frame.homeFouls : frame.awayFouls;
        if (!config.showBonus || fouls < config.bonusThreshold) return false;
        const char* value = config.doubleBonusThreshold > 0 &&
            fouls >= config.doubleBonusThreshold ? config.doubleBonusText :
            config.bonusText;
        std::snprintf(output, capacity, "%s", value);
        *defaultHeight = style.bonusHeight; return true;
    }
    std::snprintf(output, capacity, "%s", element.text);
    return true;
}

bool RenderGenericElements(IDirect3DDevice9* device,
                           const scoreboardconfig::Config& config,
                           const scoreboard::Frame& frame,
                           float originX, float originY, float scale)
{
    if (config.elementCount <= 0) return false;
    int order[scoreboardconfig::MAX_ELEMENTS];
    for (int i = 0; i < config.elementCount; ++i) order[i] = i;
    for (int i = 1; i < config.elementCount; ++i) {
        const int value = order[i]; int j = i - 1;
        while (j >= 0 && config.elements[order[j]].z > config.elements[value].z) {
            order[j + 1] = order[j]; --j;
        }
        order[j + 1] = value;
    }
    const bool fontReady = popupfont::Begin(device, "TEST");
    const popupfont::Style& style = popupfont::GetStyle();
    for (int n = 0; n < config.elementCount; ++n) {
        const scoreboardconfig::Element& e = config.elements[order[n]];
        if (!e.visible) continue;
        const scoreboardconfig::Rect r = ToScreen(e.rect, originX, originY, scale);
        if (e.type == scoreboardconfig::ElementType::Rectangle) {
            if (e.fillType == scoreboardconfig::FillType::LinearGradient)
                DrawGradientRect(device, r.x, r.y, r.x + r.width, r.y + r.height,
                    ResolveLayerColor(e.gradientStartBinding,
                        e.gradientStartColor, frame, e.opacity),
                    ResolveLayerColor(e.gradientEndBinding,
                        e.gradientEndColor, frame, e.opacity),
                    e.gradientHorizontal);
            else DrawFilledRect(device, r.x, r.y, r.x + r.width, r.y + r.height,
                ResolveLayerColor(e.fillBinding, e.fillColor, frame, e.opacity));
        }
        else if (e.type == scoreboardconfig::ElementType::Image) {
            IDirect3DTexture9* texture = nullptr;
            if (std::strcmp(e.binding, "away.logo") == 0) texture = frame.awayLogo;
            else if (std::strcmp(e.binding, "home.logo") == 0) texture = frame.homeLogo;
            else if (e.image[0]) texture = popup::GetThemeTexture(device, e.image);
            const D3DCOLOR tint = D3DCOLOR_ARGB(
                e.opacity < 0 ? 0 : e.opacity > 255 ? 255 : e.opacity,
                255, 255, 255);
            if (std::strcmp(e.id, "backgroundImage") == 0)
                DrawTextureStretched(device, texture, r.x, r.y,
                    r.x + r.width, r.y + r.height, tint);
            else
                DrawTexture(device, texture, r.x, r.y,
                    r.x + r.width, r.y + r.height, tint);
        }
        else if (e.type == scoreboardconfig::ElementType::Text && fontReady) {
            char text[128]; float height; D3DCOLOR color;
            if (!ResolveLayerText(e, config, frame, text, sizeof(text),
                    &height, style, &color)) continue;
            if (e.textColor != D3DCOLOR_XRGB(255, 255, 255) || !e.binding[0])
                color = e.textColor;
            if (e.fontHeight > 0.0f) height = e.fontHeight;
            const int alignment = e.alignment == scoreboardconfig::TextAlignment::Left ?
                -1 : e.alignment == scoreboardconfig::TextAlignment::Right ? 1 : 0;
            DrawBoundText(device, text, r, height * scale,
                WithOpacity(color, e.opacity), alignment,
                e.overflow == scoreboardconfig::TextOverflow::Fit);
        }
        else if (e.type == scoreboardconfig::ElementType::Indicator && fontReady) {
            const bool foul = std::strstr(e.binding, "fouls") != nullptr;
            const bool away = std::strncmp(e.binding, "away.", 5) == 0;
            int value = foul ? (away ? frame.awayFouls : frame.homeFouls) :
                (away ? frame.awayTimeouts : frame.homeTimeouts);
            if (!foul && !config.timeoutCountRemaining)
                value = config.maximumTimeouts - value;
            const scoreboardconfig::IndicatorMode mode = foul ?
                config.foulMode : config.timeoutMode;
            IDirect3DTexture9* image = mode == scoreboardconfig::IndicatorMode::Images ?
                popup::GetThemeTexture(device, foul ? config.foulImage :
                    config.timeoutImage) : nullptr;
            DrawIndicator(device, mode, r, value,
                foul ? config.maximumTeamFouls : config.maximumTimeouts,
                foul ? "F" : "TO", image, WithOpacity(style.scoreColor, e.opacity),
                (e.fontHeight > 0.0f ? e.fontHeight :
                    (foul ? style.foulHeight : style.timeoutHeight)) * scale);
        }
    }
    return true;
}

void DrawIndicator(IDirect3DDevice9* device,
                   scoreboardconfig::IndicatorMode mode,
                   const scoreboardconfig::Rect& rectangle,
                   int value, int maximum, const char* label,
                   IDirect3DTexture9* image, D3DCOLOR color,
                   float textHeight)
{
    if (mode == scoreboardconfig::IndicatorMode::None || value < 0) return;
    if (value > maximum && maximum > 0) value = maximum;
    if (mode == scoreboardconfig::IndicatorMode::Number ||
        mode == scoreboardconfig::IndicatorMode::Text) {
        char text[32];
        if (mode == scoreboardconfig::IndicatorMode::Text)
            std::snprintf(text, sizeof(text), "%s %d", label, value);
        else
            std::snprintf(text, sizeof(text), "%d", value);
        DrawBoundText(device, text, rectangle,
            textHeight, color, 0);
        return;
    }
    if (maximum <= 0) return;
    const float spacing = 2.0f;
    const float itemWidth =
        (rectangle.width - spacing * (maximum - 1)) / maximum;
    for (int i = 0; i < maximum; ++i) {
        const float left = rectangle.x + i * (itemWidth + spacing);
        const float dot = itemWidth < rectangle.height ?
            itemWidth : rectangle.height;
        if (i >= value) {
            if (mode == scoreboardconfig::IndicatorMode::Dots)
                DrawFilledCircle(device, left + dot * 0.5f,
                    rectangle.y + rectangle.height * 0.5f,
                    dot * 0.5f, D3DCOLOR_ARGB(85, 255, 255, 255));
            else
                DrawFilledRect(device, left, rectangle.y,
                    left + itemWidth, rectangle.y + rectangle.height,
                    D3DCOLOR_ARGB(85, 255, 255, 255));
            continue;
        }
        if (mode == scoreboardconfig::IndicatorMode::Images && image)
            DrawTexture(device, image, left, rectangle.y,
                left + itemWidth, rectangle.y + rectangle.height);
        else if (mode == scoreboardconfig::IndicatorMode::Dots) {
            DrawFilledCircle(device, left + dot * 0.5f,
                rectangle.y + rectangle.height * 0.5f,
                dot * 0.5f, color);
        }
        else
            DrawFilledRect(device, left, rectangle.y,
                left + itemWidth, rectangle.y + rectangle.height, color);
    }
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
    // NBA Live may leave sampler 0 configured for point filtering. That is
    // especially visible when a reference-resolution scoreboard is reduced
    // to 640x480: logos become grainy and the GDI glyph atlas stair-steps.
    // The state block restores the game's original sampler state afterward.
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const scoreboardconfig::Config& config = scoreboardconfig::Get();
    float scale = 1.0f;
    if (config.scaleMode == scoreboardconfig::ScaleMode::Uniform) {
        const float scaleX = static_cast<float>(viewport.Width) /
            config.referenceWidth;
        const float scaleY = static_cast<float>(viewport.Height) /
            config.referenceHeight;
        scale = scaleX < scaleY ? scaleX : scaleY;
    }
    const float width = config.width * scale;
    const float height = config.height * scale;
    const float left = (static_cast<float>(viewport.Width) - width) * 0.5f +
        config.offsetX * (config.scaleMode ==
            scoreboardconfig::ScaleMode::Uniform ? scale : 1.0f);
    const float top = config.offsetY * (config.scaleMode ==
        scoreboardconfig::ScaleMode::Uniform ? scale : 1.0f);
    const float right = left + width;
    const float bottom = top + height;

    // New themes render entirely from their ordered elements[] collection.
    // Themes without elements[] continue through the legacy path below.
    if (RenderGenericElements(device, config, frame, left, top, scale)) {
        if (stateBlock) {
            stateBlock->Apply();
            stateBlock->Release();
        }
        return;
    }

    const scoreboardconfig::Rect awayPanel =
        ToScreen(config.awayPanel, left, top, scale);
    const scoreboardconfig::Rect homePanel =
        ToScreen(config.homePanel, left, top, scale);
    int backgroundAlpha = config.backgroundAlpha;
    if (backgroundAlpha < 0) backgroundAlpha = 0;
    if (backgroundAlpha > 255) backgroundAlpha = 255;
    const D3DCOLOR backgroundColor =
        (static_cast<D3DCOLOR>(backgroundAlpha) << 24) |
        (config.backgroundColor & 0x00FFFFFFu);
    DrawFilledRect(device, left, top, right, bottom, backgroundColor);
    if (config.showBackgroundImage && config.backgroundImage[0]) {
        IDirect3DTexture9* backgroundImage =
            popup::GetThemeTexture(device, config.backgroundImage);
        int imageAlpha = config.backgroundImageAlpha;
        if (imageAlpha < 0) imageAlpha = 0;
        if (imageAlpha > 255) imageAlpha = 255;
        DrawTextureStretched(device, backgroundImage,
            left, top, right, bottom,
            D3DCOLOR_ARGB(imageAlpha, 255, 255, 255));
    }
    DrawFilledRect(device, awayPanel.x, awayPanel.y,
        awayPanel.x + awayPanel.width, awayPanel.y + awayPanel.height,
        frame.awayColor);
    DrawFilledRect(device, homePanel.x, homePanel.y,
        homePanel.x + homePanel.width, homePanel.y + homePanel.height,
        frame.homeColor);
    if (config.showAccent && config.accentHeight > 0.0f) {
        const float accentHeight = config.accentHeight * scale;
        DrawFilledRect(device, awayPanel.x,
            awayPanel.y + awayPanel.height - accentHeight,
            awayPanel.x + awayPanel.width,
            awayPanel.y + awayPanel.height, frame.awaySecondaryColor);
        DrawFilledRect(device, homePanel.x,
            homePanel.y + homePanel.height - accentHeight,
            homePanel.x + homePanel.width,
            homePanel.y + homePanel.height, frame.homeSecondaryColor);
    }

    scoreboardconfig::Rect awayLogo =
        ToScreen(config.awayLogo, left, top, scale);
    if (config.showAwayLogo)
        DrawTexture(device, frame.awayLogo,
            awayLogo.x, awayLogo.y,
            awayLogo.x + awayLogo.width,
            awayLogo.y + awayLogo.height);
    scoreboardconfig::Rect homeLogo =
        ToScreen(config.homeLogo, left, top, scale);
    if (config.showHomeLogo)
        DrawTexture(device, frame.homeLogo, homeLogo.x, homeLogo.y,
            homeLogo.x + homeLogo.width, homeLogo.y + homeLogo.height);

    const unsigned int shotSeconds =
        (frame.shotClockRaw + frame.clockUnitsPerSecond - 1u) /
        frame.clockUnitsPerSecond;
    const D3DCOLOR white = D3DCOLOR_XRGB(255, 255, 255);
    if (popupfont::Begin(device, "TEST")) {
        const popupfont::Style& style = popupfont::GetStyle();
        char awayScore[16];
        char homeScore[16];
        char clock[16];
        char shotClock[16];
        std::sprintf(awayScore, "%d", frame.awayScore);
        std::sprintf(homeScore, "%d", frame.homeScore);
        const unsigned int totalTenths =
            (frame.gameClockRaw * 10u + frame.clockUnitsPerSecond - 1u) /
            frame.clockUnitsPerSecond;
        if (totalTenths < 600u)
            std::sprintf(clock, "%u.%u", totalTenths / 10u,
                totalTenths % 10u);
        else {
            const unsigned int totalSeconds =
                frame.gameClockRaw / frame.clockUnitsPerSecond;
            std::sprintf(clock, "%u:%02u", totalSeconds / 60u,
                totalSeconds % 60u);
        }
        std::sprintf(shotClock, "%u", shotSeconds);
        DrawBoundText(device, awayScore,
            ToScreen(config.awayScore, left, top, scale),
            style.scoreHeight * scale, style.scoreColor, 1);
        DrawBoundText(device, homeScore,
            ToScreen(config.homeScore, left, top, scale),
            style.scoreHeight * scale, style.scoreColor, 1);
        DrawBoundText(device, clock,
            ToScreen(config.gameClock, left, top, scale),
            style.clockHeight * scale, style.clockColor, 0);

        bool showShotClock = frame.shotClockValid &&
            config.shotClockVisibility !=
                scoreboardconfig::ShotClockVisibility::Never;
        if (showShotClock && config.shotClockVisibility ==
                scoreboardconfig::ShotClockVisibility::UnderThreshold)
            showShotClock = shotSeconds <= config.shotClockThreshold;
        if (showShotClock) {
            D3DCOLOR shotColor = shotSeconds <=
                config.urgentShotClockThreshold ?
                config.shotClockUrgentColor : config.shotClockNormalColor;
            DrawBoundText(device, shotClock,
                ToScreen(config.shotClock, left, top, scale),
                style.shotClockHeight * scale, shotColor, 1);
        }

        char period[32];
        FormatPeriod(frame.quarter, config.periodFormat,
            period, sizeof(period));
        DrawBoundText(device, period,
            ToScreen(config.period, left, top, scale),
            style.periodHeight * scale, style.clockColor, 0);
        if (config.showTeamNames) {
            DrawBoundText(device, frame.awayTeamName,
                ToScreen(config.awayName, left, top, scale),
                style.teamNameHeight * scale, style.scoreColor, 0);
            DrawBoundText(device, frame.homeTeamName,
                ToScreen(config.homeName, left, top, scale),
                style.teamNameHeight * scale, style.scoreColor, 0);
        }

        int awayTimeouts = frame.awayTimeouts;
        int homeTimeouts = frame.homeTimeouts;
        if (!config.timeoutCountRemaining) {
            awayTimeouts = config.maximumTimeouts - awayTimeouts;
            homeTimeouts = config.maximumTimeouts - homeTimeouts;
        }
        IDirect3DTexture9* foulImage =
            config.foulMode == scoreboardconfig::IndicatorMode::Images ?
            popup::GetThemeTexture(device, config.foulImage) : nullptr;
        IDirect3DTexture9* timeoutImage =
            config.timeoutMode == scoreboardconfig::IndicatorMode::Images ?
            popup::GetThemeTexture(device, config.timeoutImage) : nullptr;
        DrawIndicator(device, config.foulMode,
            ToScreen(config.awayFouls, left, top, scale),
            frame.awayFouls, config.maximumTeamFouls,
            "F", foulImage, style.scoreColor, style.foulHeight * scale);
        DrawIndicator(device, config.foulMode,
            ToScreen(config.homeFouls, left, top, scale),
            frame.homeFouls, config.maximumTeamFouls,
            "F", foulImage, style.scoreColor, style.foulHeight * scale);
        DrawIndicator(device, config.timeoutMode,
            ToScreen(config.awayTimeouts, left, top, scale),
            awayTimeouts, config.maximumTimeouts,
            "TO", timeoutImage, style.scoreColor,
            style.timeoutHeight * scale);
        DrawIndicator(device, config.timeoutMode,
            ToScreen(config.homeTimeouts, left, top, scale),
            homeTimeouts, config.maximumTimeouts,
            "TO", timeoutImage, style.scoreColor,
            style.timeoutHeight * scale);

        if (config.showBonus && frame.homeFouls >= config.bonusThreshold) {
            const char* bonus = config.doubleBonusThreshold > 0 &&
                frame.homeFouls >= config.doubleBonusThreshold ?
                config.doubleBonusText : config.bonusText;
            DrawBoundText(device, bonus,
                ToScreen(config.awayBonus, left, top, scale),
                style.bonusHeight * scale, style.scoreColor, 0);
        }
        if (config.showBonus && frame.awayFouls >= config.bonusThreshold) {
            const char* bonus = config.doubleBonusThreshold > 0 &&
                frame.awayFouls >= config.doubleBonusThreshold ?
                config.doubleBonusText : config.bonusText;
            DrawBoundText(device, bonus,
                ToScreen(config.homeBonus, left, top, scale),
                style.bonusHeight * scale, style.scoreColor, 0);
        }
    }
    else {
        DrawInteger(device, static_cast<unsigned int>(frame.awayScore),
            left + 236.0f * scale, top + 17.0f * scale,
            16.0f, 34.0f, 5.0f, white);
        DrawInteger(device, static_cast<unsigned int>(frame.homeScore),
            left + 422.0f * scale, top + 17.0f * scale,
            16.0f, 34.0f, 5.0f, white);
        DrawGameClock(device, frame.gameClockRaw,
            frame.clockUnitsPerSecond,
            left + 310.0f * scale, top + 10.0f * scale, white);
        if (frame.shotClockValid)
            DrawInteger(device, shotSeconds, left + 364.0f * scale,
                top + 43.0f, 8.0f, 17.0f, 2.0f,
                D3DCOLOR_XRGB(255, 210, 64));
    }

    if (stateBlock) {
        stateBlock->Apply();
        stateBlock->Release();
    }
}

} // namespace scoreboard
