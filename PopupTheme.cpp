#include "PopupTheme.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace popup {
namespace {

struct LogoTexture {
    int databaseTeamID;
    IDirect3DTexture9* texture;
};

std::vector<TeamVisual> g_teams;
std::vector<LogoTexture> g_logos;
IDirect3DDevice9* g_textureDevice = nullptr;
bool g_loadAttempted = false;
char g_loadedTheme[64] = {};
char g_lastError[512] = {};

void SetLastError(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(g_lastError, sizeof(g_lastError), format, arguments);
    va_end(arguments);
    g_lastError[sizeof(g_lastError) - 1] = '\0';
}

typedef HRESULT (WINAPI *D3DXCreateTextureFromFileExAFn)(
    IDirect3DDevice9*, const char*, UINT, UINT, UINT, DWORD,
    D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR,
    void*, PALETTEENTRY*, IDirect3DTexture9**);

D3DXCreateTextureFromFileExAFn GetTextureLoader()
{
    static bool attempted = false;
    static D3DXCreateTextureFromFileExAFn function = nullptr;
    if (attempted) return function;
    attempted = true;

    // Games in this series may ship with different legacy D3DX runtimes.
    for (int version = 43; version >= 24 && !function; --version) {
        char moduleName[32];
        std::sprintf(moduleName, "d3dx9_%d.dll", version);
        HMODULE module = GetModuleHandleA(moduleName);
        if (!module) module = LoadLibraryA(moduleName);
        if (module) {
            function = reinterpret_cast<D3DXCreateTextureFromFileExAFn>(
                GetProcAddress(module, "D3DXCreateTextureFromFileExA"));
        }
    }
    return function;
}

void ReleaseTextures()
{
    for (size_t i = 0; i < g_logos.size(); ++i) {
        if (g_logos[i].texture)
            g_logos[i].texture->Release();
    }
    g_logos.clear();
    g_textureDevice = nullptr;
}

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
    if (size < 0 || size > 4 * 1024 * 1024) {
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

size_t SkipWhitespace(const std::string& text, size_t position)
{
    while (position < text.size() &&
           (text[position] == ' ' || text[position] == '\t' ||
            text[position] == '\r' || text[position] == '\n'))
        ++position;
    return position;
}

bool FindValue(const std::string& object, const char* key, size_t* position)
{
    const std::string token = std::string("\"") + key + "\"";
    size_t found = object.find(token);
    if (found == std::string::npos) return false;
    found = object.find(':', found + token.size());
    if (found == std::string::npos) return false;
    *position = SkipWhitespace(object, found + 1);
    return *position < object.size();
}

bool ReadInteger(const std::string& object, const char* key,
                 unsigned long* value)
{
    size_t position = 0;
    if (!FindValue(object, key, &position)) return false;
    char* end = nullptr;
    *value = std::strtoul(object.c_str() + position, &end, 10);
    return end != object.c_str() + position;
}

bool ReadString(const std::string& object, const char* key,
                char* destination, size_t capacity)
{
    if (!destination || !capacity) return false;
    destination[0] = '\0';
    size_t position = 0;
    if (!FindValue(object, key, &position) || object[position] != '"')
        return false;
    ++position;
    size_t output = 0;
    while (position < object.size() && object[position] != '"') {
        char character = object[position++];
        if (character == '\\' && position < object.size()) {
            const char escaped = object[position++];
            if (escaped == 'n') character = '\n';
            else if (escaped == 'r') character = '\r';
            else if (escaped == 't') character = '\t';
            else character = escaped;
        }
        if (output + 1 < capacity) destination[output++] = character;
    }
    destination[output] = '\0';
    return position < object.size();
}

bool ExtractObject(const std::string& text, size_t start,
                   std::string* object, size_t* next)
{
    const size_t opening = text.find('{', start);
    if (opening == std::string::npos) return false;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = opening; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) {
            *object = text.substr(opening, i - opening + 1);
            *next = i + 1;
            return true;
        }
    }
    return false;
}

D3DCOLOR PackedRgb(unsigned long value, D3DCOLOR fallback)
{
    if (value > 0xFFFFFFul) return fallback;
    return 0xFF000000u | static_cast<D3DCOLOR>(value);
}

bool ParseTeams(const std::string& json, const std::string& themeDirectory)
{
    size_t teamsPosition = json.find("\"teams\"");
    if (teamsPosition == std::string::npos) return false;

    size_t position = json.find('{', teamsPosition);
    if (position == std::string::npos) return false;
    ++position;

    while (position < json.size()) {
        size_t teamNumberPosition = json.find("\"TEAMNUM\"", position);
        if (teamNumberPosition == std::string::npos) break;

        const size_t objectStart = json.rfind('{', teamNumberPosition);
        if (objectStart == std::string::npos) break;
        std::string object;
        size_t next = 0;
        if (!ExtractObject(json, objectStart, &object, &next)) break;

        unsigned long teamNumber = 0;
        unsigned long primary = 0;
        unsigned long secondary = 0;
        if (!ReadInteger(object, "TEAMNUM", &teamNumber)) {
            position = next;
            continue;
        }

        TeamVisual team = {};
        team.databaseTeamID = static_cast<int>(teamNumber);
        ReadString(object, "TEAMNAME", team.teamName,
            sizeof(team.teamName));
        ReadString(object, "CITYNAME", team.cityName,
            sizeof(team.cityName));
        ReadString(object, "ABBREV", team.abbreviation,
            sizeof(team.abbreviation));
        ReadString(object, "TEAMABR2", team.shortCode,
            sizeof(team.shortCode));
        char relativeLogo[MAX_PATH] = {};
        ReadString(object, "logo", relativeLogo, sizeof(relativeLogo));
        if (relativeLogo[0]) {
            std::string path = themeDirectory + "\\" + relativeLogo;
            for (size_t i = 0; i < path.size(); ++i)
                if (path[i] == '/') path[i] = '\\';
            std::strncpy(team.logoPath, path.c_str(), MAX_PATH - 1);
        }
        if (ReadInteger(object, "PRIRGB", &primary))
            team.primaryColor = PackedRgb(primary,
                D3DCOLOR_XRGB(46, 63, 92));
        else
            team.primaryColor = D3DCOLOR_XRGB(46, 63, 92);
        if (ReadInteger(object, "SECRGB", &secondary))
            team.secondaryColor = PackedRgb(secondary,
                D3DCOLOR_XRGB(220, 220, 220));
        else
            team.secondaryColor = D3DCOLOR_XRGB(220, 220, 220);

        g_teams.push_back(team);
        position = next;
    }
    return !g_teams.empty();
}

} // namespace

bool Load(const char* themeName)
{
    if (!themeName || !*themeName) {
        SetLastError("Theme name is empty.");
        return false;
    }
    if (g_loadAttempted && std::strcmp(g_loadedTheme, themeName) == 0)
        return !g_teams.empty();

    ReleaseTextures();
    g_teams.clear();
    g_loadAttempted = true;
    std::strncpy(g_loadedTheme, themeName, sizeof(g_loadedTheme) - 1);

    const std::string gameDirectory = GetGameDirectory();
    if (gameDirectory.empty()) {
        SetLastError("Could not resolve the game directory.");
        return false;
    }
    const std::string themeDirectory =
        gameDirectory + "\\popups\\" + themeName;
    const std::string jsonPath = themeDirectory + "\\teams.json";
    std::string json;
    if (!ReadFile(jsonPath.c_str(), &json)) {
        SetLastError("Could not read %s", jsonPath.c_str());
        return false;
    }
    if (!ParseTeams(json, themeDirectory)) {
        SetLastError("No valid team records in %s", jsonPath.c_str());
        return false;
    }
    g_lastError[0] = '\0';
    return true;
}

bool Reload(const char* themeName)
{
    ReleaseTextures();
    g_teams.clear();
    g_loadAttempted = false;
    g_loadedTheme[0] = '\0';
    return Load(themeName);
}

const TeamVisual* FindTeam(int databaseTeamID)
{
    for (size_t i = 0; i < g_teams.size(); ++i)
        if (g_teams[i].databaseTeamID == databaseTeamID)
            return &g_teams[i];
    return nullptr;
}

IDirect3DTexture9* GetLogoTexture(
    IDirect3DDevice9* device, int databaseTeamID)
{
    if (!device) return nullptr;
    if (device != g_textureDevice) {
        ReleaseTextures();
        g_textureDevice = device;
    }
    for (size_t i = 0; i < g_logos.size(); ++i)
        if (g_logos[i].databaseTeamID == databaseTeamID)
            return g_logos[i].texture;

    const TeamVisual* team = FindTeam(databaseTeamID);
    if (!team) {
        SetLastError("No teams.json entry for databaseTeamID=%d.",
            databaseTeamID);
        return nullptr;
    }
    if (!team->logoPath[0]) {
        SetLastError("Team %d has no logo path.", databaseTeamID);
        return nullptr;
    }
    if (GetFileAttributesA(team->logoPath) == INVALID_FILE_ATTRIBUTES) {
        SetLastError("Logo file does not exist: %s", team->logoPath);
        LogoTexture missing = { databaseTeamID, nullptr };
        g_logos.push_back(missing);
        return nullptr;
    }
    const D3DXCreateTextureFromFileExAFn loadTexture = GetTextureLoader();
    if (!loadTexture) {
        SetLastError("No usable d3dx9_24.dll through d3dx9_43.dll found.");
        return nullptr;
    }

    IDirect3DTexture9* texture = nullptr;
    const HRESULT result = loadTexture(
        device, team->logoPath, 0xFFFFFFFFu, 0xFFFFFFFFu,
        1, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
        0x00000003u, 0x00000003u, 0, nullptr, nullptr, &texture);
    LogoTexture cached = { databaseTeamID,
        SUCCEEDED(result) ? texture : nullptr };
    g_logos.push_back(cached);
    if (FAILED(result))
        SetLastError("D3DX failed to load %s (HRESULT=%08X).",
            team->logoPath, static_cast<unsigned int>(result));
    else
        g_lastError[0] = '\0';
    return cached.texture;
}

const char* GetLastError()
{
    return g_lastError;
}

void Shutdown()
{
    ReleaseTextures();
    g_teams.clear();
    g_loadAttempted = false;
    g_loadedTheme[0] = '\0';
    g_lastError[0] = '\0';
}

} // namespace popup
