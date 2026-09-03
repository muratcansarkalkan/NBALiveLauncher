#include "PopupFont.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "gdi32.lib")

namespace popupfont {
namespace {

constexpr int FIRST_CHARACTER = 32;
constexpr int LAST_CHARACTER = 126;
constexpr int CHARACTER_COUNT = LAST_CHARACTER - FIRST_CHARACTER + 1;
constexpr int ATLAS_COLUMNS = 16;
constexpr int CELL_WIDTH = 64;
constexpr int CELL_HEIGHT = 72;
constexpr int ATLAS_WIDTH = ATLAS_COLUMNS * CELL_WIDTH;
constexpr int ATLAS_ROWS =
    (CHARACTER_COUNT + ATLAS_COLUMNS - 1) / ATLAS_COLUMNS;
constexpr int ATLAS_HEIGHT = ATLAS_ROWS * CELL_HEIGHT;
constexpr int MAX_FONT_ATLASES = 16;

struct Glyph {
    float u0, v0, u1, v1;
    float width;
    float height;
    float advance;
};

struct FontVertex {
    float x, y, z, rhw;
    D3DCOLOR color;
    float u, v;
};

struct AtlasState {
    char id[32];
    IDirect3DTexture9* texture;
    Glyph glyphs[CHARACTER_COUNT];
    float sourceHeight;
    float spacing;
    char privateFontPath[MAX_PATH];
};

AtlasState g_atlases[MAX_FONT_ATLASES] = {};
int g_atlasCount = 0;
AtlasState* g_activeAtlas = nullptr;
IDirect3DDevice9* g_device = nullptr;
bool g_attempted = false;
char g_theme[64] = {};

#define g_atlas (g_activeAtlas->texture)
#define g_glyphs (g_activeAtlas->glyphs)
#define g_sourceHeight (g_activeAtlas->sourceHeight)
#define g_spacing (g_activeAtlas->spacing)
#define g_privateFontPath (g_activeAtlas->privateFontPath)

Style g_style = {
    34.0f, 28.0f, 17.0f,
    15.0f, 18.0f, 15.0f, 15.0f, 13.0f,
    1.0f,
    D3DCOLOR_XRGB(255, 255, 255),
    D3DCOLOR_XRGB(255, 255, 255),
    D3DCOLOR_XRGB(255, 210, 64)
};

std::string GetGameDirectory()
{
    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return std::string();
    char* slash = std::strrchr(path, '\\');
    if (slash) *slash = '\0';
    return path;
}

bool ReadFile(const char* path, std::string* output)
{
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0 || size > 1024 * 1024) {
        std::fclose(file);
        return false;
    }
    output->resize(static_cast<size_t>(size));
    const bool okay = size == 0 ||
        std::fread(&(*output)[0], 1, static_cast<size_t>(size), file) ==
            static_cast<size_t>(size);
    std::fclose(file);
    return okay;
}

size_t FindJsonValue(const std::string& json, const char* key)
{
    const std::string token = std::string("\"") + key + "\"";
    size_t searchFrom = 0;
    size_t bestValue = std::string::npos;
    int bestDepth = 0x7FFFFFFF;
    while (true) {
        size_t position = json.find(token, searchFrom);
        if (position == std::string::npos) return bestValue;
        size_t colon = position + token.size();
        while (colon < json.size() &&
               (json[colon] == ' ' || json[colon] == '\t' ||
                json[colon] == '\r' || json[colon] == '\n')) ++colon;
        if (colon < json.size() && json[colon] == ':') {
            int depth = 0; bool quoted = false;
            for (size_t i = 0; i < position; ++i) {
                if (json[i] == '"' && (i == 0 || json[i - 1] != '\\'))
                    quoted = !quoted;
                if (!quoted) {
                    if (json[i] == '{' || json[i] == '[') ++depth;
                    else if (json[i] == '}' || json[i] == ']') --depth;
                }
            }
            size_t value = colon + 1;
            while (value < json.size() &&
                   (json[value] == ' ' || json[value] == '\t' ||
                    json[value] == '\r' || json[value] == '\n')) ++value;
            if (depth < bestDepth) {
                bestDepth = depth;
                bestValue = value;
                if (depth <= 1) return bestValue;
            }
        }
        searchFrom = position + token.size();
    }
}

size_t MatchingBrace(const std::string& text, size_t start)
{
    int depth = 0; bool quoted = false;
    for (size_t i = start; i < text.size(); ++i) {
        if (text[i] == '"' && (i == 0 || text[i - 1] != '\\')) quoted = !quoted;
        if (quoted) continue;
        if (text[i] == '{') ++depth;
        else if (text[i] == '}' && --depth == 0) return i;
    }
    return std::string::npos;
}

std::string JsonObject(const std::string& json, const char* key)
{
    const size_t start = FindJsonValue(json, key);
    if (start == std::string::npos || start >= json.size() || json[start] != '{')
        return std::string();
    const size_t end = MatchingBrace(json, start);
    return end == std::string::npos ? std::string() :
        json.substr(start, end - start + 1);
}

void ReadJsonString(const std::string& json, const char* key,
                    char* output, size_t capacity, const char* fallback)
{
    std::strncpy(output, fallback ? fallback : "", capacity - 1);
    output[capacity - 1] = '\0';
    size_t position = FindJsonValue(json, key);
    if (position == std::string::npos || position >= json.size() ||
        json[position] != '"') return;
    ++position;
    size_t length = 0;
    while (position < json.size() && json[position] != '"') {
        char character = json[position++];
        if (character == '\\' && position < json.size())
            character = json[position++];
        if (length + 1 < capacity) output[length++] = character;
    }
    output[length] = '\0';
}

long ReadJsonInteger(const std::string& json, const char* key, long fallback)
{
    const size_t position = FindJsonValue(json, key);
    if (position == std::string::npos || position >= json.size())
        return fallback;
    char* end = nullptr;
    const long value = std::strtol(json.c_str() + position, &end, 10);
    return end == json.c_str() + position ? fallback : value;
}

D3DCOLOR ReadJsonColor(const std::string& json, const char* key,
                       D3DCOLOR fallback)
{
    const size_t position = FindJsonValue(json, key);
    if (position == std::string::npos || position >= json.size())
        return fallback;
    char* end = nullptr;
    const unsigned long value = std::strtoul(
        json.c_str() + position, &end, 10);
    if (end == json.c_str() + position || value > 0xFFFFFFul)
        return fallback;
    return 0xFF000000u | static_cast<D3DCOLOR>(value);
}

void ReleaseAtlases()
{
    for (int i = 0; i < g_atlasCount; ++i) {
        if (g_atlases[i].texture) g_atlases[i].texture->Release();
        if (g_atlases[i].privateFontPath[0])
            RemoveFontResourceExA(g_atlases[i].privateFontPath,
                FR_PRIVATE, nullptr);
    }
    std::memset(g_atlases, 0, sizeof(g_atlases));
    g_atlasCount = 0;
    g_activeAtlas = nullptr;
    g_device = nullptr;
}

bool BuildAtlas(IDirect3DDevice9* device, const char* themeName,
                const char* fontId)
{
    const std::string themeDirectory =
        GetGameDirectory() + "\\popups\\" + themeName;
    std::string configuration;
    ReadFile((themeDirectory + "\\popup.json").c_str(), &configuration);

    const bool isDefault = !fontId || !*fontId ||
        std::strcmp(fontId, "default") == 0;
    std::string fontConfiguration = configuration;
    if (!isDefault) {
        const std::string fonts = JsonObject(configuration, "fonts");
        fontConfiguration = JsonObject(fonts, fontId);
        if (fontConfiguration.empty()) return false;
    }

    char fontFile[MAX_PATH] = {};
    char fontFace[LF_FACESIZE] = {};
    ReadJsonString(fontConfiguration, "fontFile", fontFile,
        sizeof(fontFile), isDefault ? "fonts/scoreboard.ttf" : "");
    ReadJsonString(fontConfiguration, "fontFace", fontFace,
        sizeof(fontFace), isDefault ? "Arial" : "");
    const int fontHeight = static_cast<int>(ReadJsonInteger(
        fontConfiguration, "fontSourceHeight", 48));
    const int fontWeight = static_cast<int>(ReadJsonInteger(
        fontConfiguration, "fontWeight", FW_SEMIBOLD));
    g_spacing = static_cast<float>(ReadJsonInteger(
        fontConfiguration, "characterSpacing", 1));
    if (isDefault) g_style.characterSpacing = g_spacing;
    if (isDefault) {
    g_style.scoreHeight = static_cast<float>(ReadJsonInteger(
        configuration, "scoreHeight", 34));
    g_style.clockHeight = static_cast<float>(ReadJsonInteger(
        configuration, "clockHeight", 28));
    g_style.shotClockHeight = static_cast<float>(ReadJsonInteger(
        configuration, "shotClockHeight", 17));
    g_style.teamNameHeight = static_cast<float>(ReadJsonInteger(
        configuration, "teamNameHeight", 15));
    g_style.periodHeight = static_cast<float>(ReadJsonInteger(
        configuration, "periodHeight", 18));
    g_style.foulHeight = static_cast<float>(ReadJsonInteger(
        configuration, "foulHeight", 15));
    g_style.timeoutHeight = static_cast<float>(ReadJsonInteger(
        configuration, "timeoutHeight", 15));
    g_style.bonusHeight = static_cast<float>(ReadJsonInteger(
        configuration, "bonusHeight", 13));
    g_style.scoreColor = ReadJsonColor(configuration, "scoreColor",
        D3DCOLOR_XRGB(255, 255, 255));
    g_style.clockColor = ReadJsonColor(configuration, "clockColor",
        D3DCOLOR_XRGB(255, 255, 255));
    g_style.shotClockColor = ReadJsonColor(
        configuration, "shotClockColor",
        D3DCOLOR_XRGB(255, 210, 64));
    }

    std::string fontPath = themeDirectory + "\\" + fontFile;
    for (size_t i = 0; i < fontPath.size(); ++i)
        if (fontPath[i] == '/') fontPath[i] = '\\';
    if (GetFileAttributesA(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::strncpy(g_privateFontPath, fontPath.c_str(), MAX_PATH - 1);
        AddFontResourceExA(g_privateFontPath, FR_PRIVATE, nullptr);
    }

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = ATLAS_WIDTH;
    bitmapInfo.bmiHeader.biHeight = -ATLAS_HEIGHT;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        dc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HFONT font = CreateFontA(
        -fontHeight, 0, 0, 0, fontWeight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fontFace);
    if (!bitmap || !font || !pixels) {
        if (font) DeleteObject(font);
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        return false;
    }

    HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    HGDIOBJ previousFont = SelectObject(dc, font);
    std::memset(pixels, 0, ATLAS_WIDTH * ATLAS_HEIGHT * 4);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, OPAQUE);
    TEXTMETRICA metrics = {};
    GetTextMetricsA(dc, &metrics);
    g_sourceHeight = static_cast<float>(metrics.tmHeight + 4);

    for (int index = 0; index < CHARACTER_COUNT; ++index) {
        const char character = static_cast<char>(FIRST_CHARACTER + index);
        const int column = index % ATLAS_COLUMNS;
        const int row = index / ATLAS_COLUMNS;
        const int x = column * CELL_WIDTH;
        const int y = row * CELL_HEIGHT;
        SIZE size = {};
        GetTextExtentPoint32A(dc, &character, 1, &size);
        TextOutA(dc, x + 2, y + 2, &character, 1);
        Glyph& glyph = g_glyphs[index];
        const int imageWidth = size.cx + 4 < CELL_WIDTH ?
            size.cx + 4 : CELL_WIDTH;
        const int imageHeight = metrics.tmHeight + 4 < CELL_HEIGHT ?
            metrics.tmHeight + 4 : CELL_HEIGHT;
        glyph.u0 = static_cast<float>(x) / ATLAS_WIDTH;
        glyph.v0 = static_cast<float>(y) / ATLAS_HEIGHT;
        glyph.u1 = static_cast<float>(x + imageWidth) / ATLAS_WIDTH;
        glyph.v1 = static_cast<float>(y + imageHeight) / ATLAS_HEIGHT;
        glyph.width = static_cast<float>(imageWidth);
        glyph.height = static_cast<float>(imageHeight);
        glyph.advance = static_cast<float>(size.cx);
    }

    HRESULT result = device->CreateTexture(
        ATLAS_WIDTH, ATLAS_HEIGHT, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &g_atlas, nullptr);
    if (SUCCEEDED(result)) {
        D3DLOCKED_RECT locked = {};
        result = g_atlas->LockRect(0, &locked, nullptr, 0);
        if (SUCCEEDED(result)) {
            const unsigned char* source =
                static_cast<const unsigned char*>(pixels);
            for (int y = 0; y < ATLAS_HEIGHT; ++y) {
                DWORD* destination = reinterpret_cast<DWORD*>(
                    static_cast<unsigned char*>(locked.pBits) +
                    y * locked.Pitch);
                for (int x = 0; x < ATLAS_WIDTH; ++x) {
                    const unsigned char intensity = source[
                        (y * ATLAS_WIDTH + x) * 4];
                    destination[x] =
                        (static_cast<DWORD>(intensity) << 24) | 0x00FFFFFFu;
                }
            }
            g_atlas->UnlockRect(0);
        }
    }

    SelectObject(dc, previousFont);
    SelectObject(dc, previousBitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    if (FAILED(result)) {
        if (g_atlas) { g_atlas->Release(); g_atlas = nullptr; }
    }
    return SUCCEEDED(result);
}

void DrawGlyph(IDirect3DDevice9* device, const Glyph& glyph,
               float x, float y, float scale, D3DCOLOR color)
{
    const float width = glyph.width * scale;
    const float height = glyph.height * scale;
    const FontVertex vertices[4] = {
        { x,         y,          0.0f, 1.0f, color, glyph.u0, glyph.v0 },
        { x + width, y,          0.0f, 1.0f, color, glyph.u1, glyph.v0 },
        { x,         y + height, 0.0f, 1.0f, color, glyph.u0, glyph.v1 },
        { x + width, y + height, 0.0f, 1.0f, color, glyph.u1, glyph.v1 }
    };
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices,
        sizeof(FontVertex));
}

} // namespace

bool Begin(IDirect3DDevice9* device, const char* themeName)
{
    if (!device || !themeName || !*themeName) return false;
    if (g_device != device || std::strcmp(g_theme, themeName) != 0) {
        Shutdown();
        g_attempted = true;
        std::strncpy(g_theme, themeName, sizeof(g_theme) - 1);
        g_activeAtlas = &g_atlases[0];
        g_atlasCount = 1;
        std::strcpy(g_activeAtlas->id, "default");
        if (!BuildAtlas(device, themeName, "default")) {
            ReleaseAtlases();
            return false;
        }
        g_device = device;
    }
    g_activeAtlas = g_atlasCount > 0 ? &g_atlases[0] : nullptr;
    return g_atlas != nullptr;
}

bool SelectFont(IDirect3DDevice9* device, const char* themeName,
                const char* fontId)
{
    if (!Begin(device, themeName)) return false;
    if (!fontId || !*fontId || std::strcmp(fontId, "default") == 0)
        return true;

    for (int i = 1; i < g_atlasCount; ++i) {
        if (std::strcmp(g_atlases[i].id, fontId) == 0) {
            g_activeAtlas = &g_atlases[i];
            return g_activeAtlas->texture != nullptr;
        }
    }
    if (g_atlasCount >= MAX_FONT_ATLASES) return true;

    AtlasState* candidate = &g_atlases[g_atlasCount];
    std::memset(candidate, 0, sizeof(*candidate));
    std::strncpy(candidate->id, fontId, sizeof(candidate->id) - 1);
    g_activeAtlas = candidate;
    if (!BuildAtlas(device, themeName, fontId)) {
        if (candidate->privateFontPath[0])
            RemoveFontResourceExA(candidate->privateFontPath,
                FR_PRIVATE, nullptr);
        std::memset(candidate, 0, sizeof(*candidate));
        g_activeAtlas = &g_atlases[0];
        return true;
    }
    ++g_atlasCount;
    return true;
}

bool Reload(IDirect3DDevice9* device, const char* themeName)
{
    Shutdown();
    return Begin(device, themeName);
}

const Style& GetStyle()
{
    return g_style;
}

float Measure(const char* text, float height)
{
    if (!text || !*text || !g_activeAtlas || !g_atlas ||
        g_sourceHeight <= 0.0f)
        return 0.0f;
    const float scale = height / g_sourceHeight;
    float width = 0.0f;
    bool first = true;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        if (*p < FIRST_CHARACTER || *p > LAST_CHARACTER) continue;
        if (!first) width += g_spacing * scale;
        width += g_glyphs[*p - FIRST_CHARACTER].advance * scale;
        first = false;
    }
    return width;
}

void DrawLeft(IDirect3DDevice9* device, const char* text,
              float x, float y, float height, D3DCOLOR color)
{
    if (!device || !text || !g_activeAtlas || !g_atlas ||
        g_sourceHeight <= 0.0f) return;
    const float scale = height / g_sourceHeight;
    device->SetTexture(0, g_atlas);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    bool first = true;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        if (*p < FIRST_CHARACTER || *p > LAST_CHARACTER) continue;
        if (!first) x += g_spacing * scale;
        const Glyph& glyph = g_glyphs[*p - FIRST_CHARACTER];
        DrawGlyph(device, glyph, x, y, scale, color);
        x += glyph.advance * scale;
        first = false;
    }
}

void DrawCentered(IDirect3DDevice9* device, const char* text,
                  float centerX, float y, float height, D3DCOLOR color)
{
    DrawLeft(device, text, centerX - Measure(text, height) * 0.5f,
        y, height, color);
}

void DrawRight(IDirect3DDevice9* device, const char* text,
               float right, float y, float height, D3DCOLOR color)
{
    DrawLeft(device, text, right - Measure(text, height),
        y, height, color);
}

void Shutdown()
{
    ReleaseAtlases();
    g_attempted = false;
    g_theme[0] = '\0';
}

} // namespace popupfont
