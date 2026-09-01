#include "ScoreboardConfig.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace scoreboardconfig {
namespace {

Config g_config = {};
bool g_loaded = false;
char g_theme[64] = {};
char g_lastError[512] = {};

void SetDefaults(Config* c)
{
    std::memset(c, 0, sizeof(*c));
    c->referenceWidth = 1366;
    c->referenceHeight = 768;
    c->scaleMode = ScaleMode::Uniform;
    c->offsetX = 0.0f;
    c->offsetY = 18.0f;
    c->width = 620.0f;
    c->height = 68.0f;
    c->visibilityMode = VisibilityMode::Always;
    c->showAfterScoreMilliseconds = 5000;
    c->alwaysShowBelowSeconds = 120;
    c->shotClockVisibility = ShotClockVisibility::Always;
    c->shotClockThreshold = 10;
    c->urgentShotClockThreshold = 10;
    c->shotClockNormalColor = D3DCOLOR_XRGB(255, 210, 64);
    c->shotClockUrgentColor = D3DCOLOR_XRGB(255, 48, 48);
    c->showTeamNames = false;
    c->showAwayLogo = true;
    c->showHomeLogo = true;
    c->teamNameFormat = TeamNameFormat::Abbreviation;
    c->periodFormat = PeriodFormat::Ordinal;
    c->foulMode = IndicatorMode::None;
    c->timeoutMode = IndicatorMode::None;
    c->maximumTeamFouls = 5;
    c->maximumTimeouts = 6;
    c->timeoutCountRemaining = true;
    std::strcpy(c->foulImage, "images/foul.png");
    std::strcpy(c->timeoutImage, "images/timeout.png");
    c->showBonus = false;
    c->bonusThreshold = 5;
    c->doubleBonusThreshold = 0;
    std::strcpy(c->bonusText, "BONUS");
    std::strcpy(c->doubleBonusText, "BONUS+");

    c->awayPanel = { 0, 0, 186, 68 };
    c->homePanel = { 434, 0, 186, 68 };
    c->awayLogo = { 12, 5, 162, 48 };
    c->homeLogo = { 446, 5, 162, 48 };
    c->awayName = { 12, 49, 162, 15 };
    c->homeName = { 446, 49, 162, 15 };
    c->awayScore = { 190, 14, 46, 36 };
    c->homeScore = { 376, 14, 46, 36 };
    c->gameClock = { 248, 7, 124, 30 };
    c->shotClock = { 334, 41, 30, 18 };
    c->period = { 256, 42, 68, 18 };
    c->awayFouls = { 12, 49, 80, 15 };
    c->homeFouls = { 528, 49, 80, 15 };
    c->awayTimeouts = { 94, 49, 80, 15 };
    c->homeTimeouts = { 446, 49, 80, 15 };
    c->awayBonus = { 188, 51, 58, 13 };
    c->homeBonus = { 374, 51, 58, 13 };
}

std::string GameDirectory()
{
    char path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
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
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0 || size > 1024 * 1024) {
        std::fclose(file);
        return false;
    }
    output->resize(static_cast<size_t>(size));
    bool okay = size == 0 ||
        std::fread(&(*output)[0], 1, static_cast<size_t>(size), file) ==
            static_cast<size_t>(size);
    std::fclose(file);
    return okay;
}

size_t ValuePosition(const std::string& json, const char* key)
{
    std::string token = std::string("\"") + key + "\"";
    size_t p = json.find(token);
    if (p == std::string::npos) return p;
    p = json.find(':', p + token.size());
    if (p == std::string::npos) return p;
    do { ++p; } while (p < json.size() &&
        (json[p] == ' ' || json[p] == '\t' ||
         json[p] == '\r' || json[p] == '\n'));
    return p;
}

long Integer(const std::string& json, const char* key, long fallback)
{
    size_t p = ValuePosition(json, key);
    if (p == std::string::npos) return fallback;
    char* end = nullptr;
    long value = std::strtol(json.c_str() + p, &end, 10);
    return end == json.c_str() + p ? fallback : value;
}

float Number(const std::string& json, const char* key, float fallback)
{
    size_t p = ValuePosition(json, key);
    if (p == std::string::npos) return fallback;
    char* end = nullptr;
    double value = std::strtod(json.c_str() + p, &end);
    return end == json.c_str() + p ? fallback : static_cast<float>(value);
}

bool Boolean(const std::string& json, const char* key, bool fallback)
{
    size_t p = ValuePosition(json, key);
    if (p == std::string::npos) return fallback;
    if (json.compare(p, 4, "true") == 0) return true;
    if (json.compare(p, 5, "false") == 0) return false;
    return fallback;
}

void String(const std::string& json, const char* key,
            char* output, size_t capacity, const char* fallback)
{
    std::strncpy(output, fallback ? fallback : "", capacity - 1);
    output[capacity - 1] = '\0';
    size_t p = ValuePosition(json, key);
    if (p == std::string::npos || p >= json.size() || json[p] != '"')
        return;
    ++p;
    size_t length = 0;
    while (p < json.size() && json[p] != '"') {
        char ch = json[p++];
        if (ch == '\\' && p < json.size()) ch = json[p++];
        if (length + 1 < capacity) output[length++] = ch;
    }
    output[length] = '\0';
}

std::string StringValue(const std::string& json, const char* key,
                        const char* fallback)
{
    char value[128] = {};
    String(json, key, value, sizeof(value), fallback);
    return value;
}

D3DCOLOR Color(const std::string& json, const char* key, D3DCOLOR fallback)
{
    long value = Integer(json, key, -1);
    if (value < 0 || value > 0xFFFFFF) return fallback;
    return 0xFF000000u | static_cast<D3DCOLOR>(value);
}

Rect ReadRect(const std::string& json, const char* name, const Rect& fallback)
{
    std::string prefix(name);
    Rect r = fallback;
    r.x = Number(json, (prefix + "X").c_str(), r.x);
    r.y = Number(json, (prefix + "Y").c_str(), r.y);
    r.width = Number(json, (prefix + "Width").c_str(), r.width);
    r.height = Number(json, (prefix + "Height").c_str(), r.height);
    return r;
}

IndicatorMode Indicator(const std::string& value)
{
    if (value == "number") return IndicatorMode::Number;
    if (value == "text") return IndicatorMode::Text;
    if (value == "dots") return IndicatorMode::Dots;
    if (value == "bars") return IndicatorMode::Bars;
    if (value == "images") return IndicatorMode::Images;
    return IndicatorMode::None;
}

void Parse(const std::string& json, Config* c)
{
    c->referenceWidth = Integer(json, "referenceWidth", c->referenceWidth);
    c->referenceHeight = Integer(json, "referenceHeight", c->referenceHeight);
    std::string scale = StringValue(json, "scaleMode", "uniform");
    c->scaleMode = scale == "fixed" ? ScaleMode::Fixed :
        scale == "positionOnly" ? ScaleMode::PositionOnly : ScaleMode::Uniform;
    c->offsetX = Number(json, "offsetX", c->offsetX);
    c->offsetY = Number(json, "offsetY", c->offsetY);
    c->width = Number(json, "scoreboardWidth", c->width);
    c->height = Number(json, "scoreboardHeight", c->height);

    std::string visibility = StringValue(json, "visibilityMode", "always");
    c->visibilityMode = visibility == "afterScore" ? VisibilityMode::AfterScore :
        visibility == "lateGameOnly" ? VisibilityMode::LateGameOnly :
        visibility == "afterScoreAndLateGame" ? VisibilityMode::AfterScoreAndLateGame :
        VisibilityMode::Always;
    c->showAfterScoreMilliseconds = Integer(json,
        "showAfterScoreMilliseconds", c->showAfterScoreMilliseconds);
    c->alwaysShowBelowSeconds = Integer(json,
        "alwaysShowBelowSeconds", c->alwaysShowBelowSeconds);

    std::string shot = StringValue(json, "shotClockVisibility", "always");
    c->shotClockVisibility = shot == "underThreshold" ?
        ShotClockVisibility::UnderThreshold : shot == "never" ?
        ShotClockVisibility::Never : ShotClockVisibility::Always;
    c->shotClockThreshold = Integer(json,
        "shotClockThreshold", c->shotClockThreshold);
    c->urgentShotClockThreshold = Integer(json,
        "urgentShotClockThreshold", c->urgentShotClockThreshold);
    c->shotClockNormalColor = Color(json,
        "shotClockNormalColor", c->shotClockNormalColor);
    c->shotClockUrgentColor = Color(json,
        "shotClockUrgentColor", c->shotClockUrgentColor);

    c->showTeamNames = Boolean(json, "showTeamNames", c->showTeamNames);
    c->showAwayLogo = Boolean(json, "showAwayLogo", c->showAwayLogo);
    c->showHomeLogo = Boolean(json, "showHomeLogo", c->showHomeLogo);
    std::string team = StringValue(json, "teamNameFormat", "abbreviation");
    c->teamNameFormat = team == "city" ? TeamNameFormat::City :
        team == "nickname" ? TeamNameFormat::Nickname :
        team == "fullName" ? TeamNameFormat::FullName :
        team == "shortCode" ? TeamNameFormat::ShortCode :
        TeamNameFormat::Abbreviation;
    std::string period = StringValue(json, "periodFormat", "ordinal");
    c->periodFormat = period == "number" ? PeriodFormat::Number :
        period == "ordinalQuarter" ? PeriodFormat::OrdinalQuarter :
        period == "shortQuarter" ? PeriodFormat::ShortQuarter :
        period == "longQuarter" ? PeriodFormat::LongQuarter :
        PeriodFormat::Ordinal;

    c->foulMode = Indicator(StringValue(json, "foulMode", "none"));
    c->timeoutMode = Indicator(StringValue(json, "timeoutMode", "none"));
    c->maximumTeamFouls = Integer(json,
        "maximumTeamFouls", c->maximumTeamFouls);
    c->maximumTimeouts = Integer(json,
        "maximumTimeouts", c->maximumTimeouts);
    c->timeoutCountRemaining = StringValue(json,
        "timeoutCountMode", "remaining") != "used";
    String(json, "foulImage", c->foulImage,
        sizeof(c->foulImage), "images/foul.png");
    String(json, "timeoutImage", c->timeoutImage,
        sizeof(c->timeoutImage), "images/timeout.png");

    c->showBonus = Boolean(json, "showBonus", c->showBonus);
    c->bonusThreshold = Integer(json, "bonusThreshold", c->bonusThreshold);
    c->doubleBonusThreshold = Integer(json,
        "doubleBonusThreshold", c->doubleBonusThreshold);
    String(json, "bonusText", c->bonusText,
        sizeof(c->bonusText), "BONUS");
    String(json, "doubleBonusText", c->doubleBonusText,
        sizeof(c->doubleBonusText), "BONUS+");

#define READ_RECT(field) c->field = ReadRect(json, #field, c->field)
    READ_RECT(awayPanel); READ_RECT(homePanel);
    READ_RECT(awayLogo); READ_RECT(homeLogo);
    READ_RECT(awayName); READ_RECT(homeName);
    READ_RECT(awayScore); READ_RECT(homeScore);
    READ_RECT(gameClock); READ_RECT(shotClock); READ_RECT(period);
    READ_RECT(awayFouls); READ_RECT(homeFouls);
    READ_RECT(awayTimeouts); READ_RECT(homeTimeouts);
    READ_RECT(awayBonus); READ_RECT(homeBonus);
#undef READ_RECT
}

} // namespace

const Config& Get() { return g_config; }

bool Load(const char* themeName)
{
    if (g_loaded && std::strcmp(g_theme, themeName) == 0) return true;
    Config next;
    SetDefaults(&next);
    std::string path = GameDirectory() + "\\popups\\" +
        themeName + "\\scoreboard.json";
    std::string json;
    if (!ReadFile(path.c_str(), &json)) {
        std::snprintf(g_lastError, sizeof(g_lastError),
            "Could not read %s", path.c_str());
        g_config = next;
        g_loaded = true;
        std::strncpy(g_theme, themeName, sizeof(g_theme) - 1);
        return false;
    }
    Parse(json, &next);
    g_config = next;
    g_loaded = true;
    std::strncpy(g_theme, themeName, sizeof(g_theme) - 1);
    g_lastError[0] = '\0';
    return true;
}

bool Reload(const char* themeName)
{
    g_loaded = false;
    g_theme[0] = '\0';
    return Load(themeName);
}

const char* GetLastError() { return g_lastError; }

} // namespace scoreboardconfig
