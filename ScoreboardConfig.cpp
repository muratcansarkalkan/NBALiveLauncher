#include "ScoreboardConfig.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace scoreboardconfig {
namespace {

Config g_config = {};
Config g_violationConfig = {};
bool g_loaded = false;
bool g_violationLoaded = false;
char g_theme[64] = {};
char g_violationTheme[64] = {};
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
    c->backgroundColor = D3DCOLOR_XRGB(15, 18, 24);
    c->backgroundAlpha = 232;
    c->showBackgroundImage = false;
    std::strcpy(c->backgroundImage, "images/background.png");
    c->backgroundImageAlpha = 255;
    c->showAccent = true;
    c->accentHeight = 6.0f;
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
    std::strcpy(c->enterAnimation, "slideFade");
    c->enterFromX = -80.0f;
    c->enterFromY = 0.0f;
    c->enterMilliseconds = 250;
    c->holdMilliseconds = 2500;
    std::strcpy(c->exitAnimation, "fade");
    c->exitToX = 0.0f;
    c->exitToY = 0.0f;
    c->exitMilliseconds = 200;
    c->freezeWhilePaused = true;
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
    size_t searchFrom = 0;
    while (true) {
        size_t p = json.find(token, searchFrom);
        if (p == std::string::npos) return p;

        // A token is a property name only when the next non-whitespace
        // character is ':'.  Editor-generated elements can contain values
        // such as "id": "backgroundImage" before the top-level
        // "backgroundImage" property.  The old search mistook that value for
        // the property name and read the following field instead.
        size_t colon = p + token.size();
        while (colon < json.size() &&
            (json[colon] == ' ' || json[colon] == '\t' ||
             json[colon] == '\r' || json[colon] == '\n'))
            ++colon;

        if (colon < json.size() && json[colon] == ':') {
            p = colon;
            do { ++p; } while (p < json.size() &&
                (json[p] == ' ' || json[p] == '\t' ||
                 json[p] == '\r' || json[p] == '\n'));
            return p;
        }

        searchFrom = p + token.size();
    }
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

size_t MatchingDelimiter(const std::string& text, size_t start,
                         char open, char close)
{
    int depth = 0;
    bool quoted = false;
    for (size_t i = start; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '"' && (i == 0 || text[i - 1] != '\\')) quoted = !quoted;
        if (quoted) continue;
        if (ch == open) ++depth;
        else if (ch == close && --depth == 0) return i;
    }
    return std::string::npos;
}

std::string ObjectValue(const std::string& json, const char* key)
{
    size_t p = ValuePosition(json, key);
    if (p == std::string::npos || p >= json.size() || json[p] != '{')
        return std::string();
    size_t end = MatchingDelimiter(json, p, '{', '}');
    return end == std::string::npos ? std::string() :
        json.substr(p, end - p + 1);
}

void ParseElements(const std::string& json, Config* c)
{
    c->elementCount = 0;
    size_t p = ValuePosition(json, "elements");
    if (p == std::string::npos || p >= json.size() || json[p] != '[') return;
    const size_t arrayEnd = MatchingDelimiter(json, p, '[', ']');
    if (arrayEnd == std::string::npos) return;
    ++p;
    while (p < arrayEnd && c->elementCount < MAX_ELEMENTS) {
        p = json.find('{', p);
        if (p == std::string::npos || p >= arrayEnd) break;
        const size_t end = MatchingDelimiter(json, p, '{', '}');
        if (end == std::string::npos || end > arrayEnd) break;
        const std::string object = json.substr(p, end - p + 1);
        Element& e = c->elements[c->elementCount];
        std::memset(&e, 0, sizeof(e));
        String(object, "id", e.id, sizeof(e.id), "element");
        const std::string type = StringValue(object, "type", "rectangle");
        e.type = type == "image" ? ElementType::Image :
            type == "text" ? ElementType::Text :
            type == "indicator" ? ElementType::Indicator :
            ElementType::Rectangle;
        String(object, "binding", e.binding, sizeof(e.binding), "");
        String(object, "text", e.text, sizeof(e.text), "");
        String(object, "image", e.image, sizeof(e.image), "");
        String(object, "font", e.font, sizeof(e.font), "");
        e.rect.x = Number(object, "x", 0.0f);
        e.rect.y = Number(object, "y", 0.0f);
        e.rect.width = Number(object, "width", 100.0f);
        e.rect.height = Number(object, "height", 30.0f);
        e.z = Integer(object, "z", c->elementCount);
        e.visible = Boolean(object, "visible", true);
        e.locked = Boolean(object, "locked", false);
        const std::string imageFit = StringValue(object, "imageFit",
            std::strcmp(e.id, "backgroundImage") == 0 ? "stretch" : "contain");
        e.imageFit = imageFit == "stretch" ? ImageFit::Stretch : ImageFit::Contain;
        const std::string alignment = StringValue(object, "alignment", "center");
        e.alignment = alignment == "left" ? TextAlignment::Left :
            alignment == "right" ? TextAlignment::Right : TextAlignment::Center;
        e.overflow = StringValue(object, "overflow", "overflow") == "fit" ?
            TextOverflow::Fit : TextOverflow::Overflow;
        e.fontHeight = Number(object, "fontHeight", 0.0f);
        e.textColor = Color(object, "textColor", D3DCOLOR_XRGB(255, 255, 255));
        e.opacity = Integer(object, "opacity", 255);

        const std::string fill = ObjectValue(object, "fill");
        e.fillType = StringValue(fill, "type", "solid") == "linearGradient" ?
            FillType::LinearGradient : FillType::Solid;
        String(fill, "binding", e.fillBinding, sizeof(e.fillBinding), "");
        e.fillColor = Color(fill, "color", D3DCOLOR_XRGB(255, 255, 255));
        e.gradientStartColor = Color(fill, "startColor", e.fillColor);
        e.gradientEndColor = Color(fill, "endColor", D3DCOLOR_XRGB(0, 0, 0));
        String(fill, "startBinding", e.gradientStartBinding,
            sizeof(e.gradientStartBinding), "");
        String(fill, "endBinding", e.gradientEndBinding,
            sizeof(e.gradientEndBinding), "");
        e.gradientHorizontal =
            StringValue(fill, "direction", "vertical") == "horizontal";
        ++c->elementCount;
        p = end + 1;
    }
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
    c->backgroundColor = Color(json,
        "backgroundColor", c->backgroundColor);
    c->backgroundAlpha = Integer(json,
        "backgroundAlpha", c->backgroundAlpha);
    c->showBackgroundImage = Boolean(json,
        "showBackgroundImage", c->showBackgroundImage);
    String(json, "backgroundImage", c->backgroundImage,
        sizeof(c->backgroundImage), "images/background.png");
    c->backgroundImageAlpha = Integer(json,
        "backgroundImageAlpha", c->backgroundImageAlpha);
    c->showAccent = Boolean(json, "showAccent", c->showAccent);
    c->accentHeight = Number(json, "accentHeight", c->accentHeight);

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
    ParseElements(json, c);

    const std::string animation = ObjectValue(json, "animation");
    const std::string enter = ObjectValue(animation, "enter");
    const std::string exit = ObjectValue(animation, "exit");
    String(enter, "type", c->enterAnimation,
        sizeof(c->enterAnimation), "slideFade");
    c->enterFromX = Number(enter, "fromX", c->enterFromX);
    c->enterFromY = Number(enter, "fromY", c->enterFromY);
    c->enterMilliseconds = Integer(enter, "duration", c->enterMilliseconds);
    c->holdMilliseconds = Integer(animation,
        "holdMilliseconds", c->holdMilliseconds);
    String(exit, "type", c->exitAnimation,
        sizeof(c->exitAnimation), "fade");
    c->exitToX = Number(exit, "toX", c->exitToX);
    c->exitToY = Number(exit, "toY", c->exitToY);
    c->exitMilliseconds = Integer(exit, "duration", c->exitMilliseconds);
    c->freezeWhilePaused = Boolean(animation,
        "freezeWhilePaused", c->freezeWhilePaused);
}

} // namespace

const Config& Get() { return g_config; }
const Config& GetViolation() { return g_violationConfig; }

bool Load(const char* themeName)
{
    if (g_loaded && std::strcmp(g_theme, themeName) == 0) return true;
    Config next;
    SetDefaults(&next);
    std::string path = GameDirectory() + "\\popups\\" +
        themeName + "\\scoreboard\\scoreboard.json";
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

bool LoadViolation(const char* themeName)
{
    if (g_violationLoaded &&
        std::strcmp(g_violationTheme, themeName) == 0) return true;
    Config next;
    SetDefaults(&next);
    next.width = 420.0f;
    next.height = 80.0f;
    next.offsetY = 90.0f;
    std::string path = GameDirectory() + "\\popups\\" + themeName +
        "\\violation\\violation.json";
    std::string json;
    if (!ReadFile(path.c_str(), &json)) {
        std::snprintf(g_lastError, sizeof(g_lastError),
            "Could not read %s", path.c_str());
        g_violationConfig = next;
        g_violationLoaded = true;
        std::strncpy(g_violationTheme, themeName,
            sizeof(g_violationTheme) - 1);
        return false;
    }
    Parse(json, &next);
    g_violationConfig = next;
    g_violationLoaded = true;
    std::strncpy(g_violationTheme, themeName,
        sizeof(g_violationTheme) - 1);
    g_lastError[0] = '\0';
    return true;
}

bool ReloadViolation(const char* themeName)
{
    g_violationLoaded = false;
    g_violationTheme[0] = '\0';
    return LoadViolation(themeName);
}

const char* GetLastError() { return g_lastError; }

} // namespace scoreboardconfig
