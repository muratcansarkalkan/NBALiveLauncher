#pragma once

#include <d3d9.h>

namespace scoreboardconfig {

enum class ScaleMode { Uniform, Fixed, PositionOnly };
enum class VisibilityMode { Always, AfterScore, LateGameOnly, AfterScoreAndLateGame };
enum class ShotClockVisibility { Always, UnderThreshold, Never };
enum class TeamNameFormat { Abbreviation, City, Nickname, FullName, ShortCode };
enum class PeriodFormat { Number, Ordinal, OrdinalQuarter, ShortQuarter, LongQuarter };
enum class IndicatorMode { None, Number, Text, Dots, Bars, Images };
enum class ElementType { Rectangle, Image, Text, Indicator };
enum class TextAlignment { Left, Center, Right };
enum class TextOverflow { Overflow, Fit };
enum class TextTransform { None, Uppercase, Lowercase, Capitalize, SmallCaps };
enum class FillType { Solid, LinearGradient };
enum class ImageFit { Contain, Stretch };

struct Rect {
    float x, y, width, height;
};

struct Element {
    char id[32];
    ElementType type;
    char binding[48];
    char text[96];
    char image[MAX_PATH];
    char font[32];
    Rect rect;
    int z;
    bool visible;
    bool locked;
    TextAlignment alignment;
    TextOverflow overflow;
    TextTransform textTransform;
    float smallCapsScale;
    float fontHeight;
    D3DCOLOR textColor;
    int opacity;
    FillType fillType;
    char fillBinding[48];
    D3DCOLOR fillColor;
    D3DCOLOR gradientStartColor;
    D3DCOLOR gradientEndColor;
    char gradientStartBinding[48];
    char gradientEndBinding[48];
    bool gradientHorizontal;
    ImageFit imageFit;
    bool tintEnabled;
    char tintBinding[48];
    D3DCOLOR tintColor;
};

constexpr int MAX_ELEMENTS = 64;

struct Config {
    Element elements[MAX_ELEMENTS];
    int elementCount;
    int referenceWidth;
    int referenceHeight;
    ScaleMode scaleMode;
    float offsetX;
    float offsetY;
    float width;
    float height;
    int overlayZ;
    D3DCOLOR backgroundColor;
    int backgroundAlpha;
    bool showBackgroundImage;
    char backgroundImage[MAX_PATH];
    int backgroundImageAlpha;
    bool showAccent;
    float accentHeight;

    VisibilityMode visibilityMode;
    unsigned int showAfterScoreMilliseconds;
    unsigned int alwaysShowBelowSeconds;

    ShotClockVisibility shotClockVisibility;
    unsigned int shotClockThreshold;
    unsigned int urgentShotClockThreshold;
    D3DCOLOR shotClockNormalColor;
    D3DCOLOR shotClockUrgentColor;

    bool showTeamNames;
    bool showAwayLogo;
    bool showHomeLogo;
    TeamNameFormat teamNameFormat;
    PeriodFormat periodFormat;

    IndicatorMode foulMode;
    IndicatorMode timeoutMode;
    int maximumTeamFouls;
    int maximumTimeouts;
    bool timeoutCountRemaining;
    char foulImage[MAX_PATH];
    char timeoutImage[MAX_PATH];

    bool showBonus;
    int bonusThreshold;
    int doubleBonusThreshold;
    char bonusText[32];
    char doubleBonusText[32];

    Rect awayPanel;
    Rect homePanel;
    Rect awayLogo;
    Rect homeLogo;
    Rect awayName;
    Rect homeName;
    Rect awayScore;
    Rect homeScore;
    Rect gameClock;
    Rect shotClock;
    Rect period;
    Rect awayFouls;
    Rect homeFouls;
    Rect awayTimeouts;
    Rect homeTimeouts;
    Rect awayBonus;
    Rect homeBonus;

    char enterAnimation[16];
    float enterFromX;
    float enterFromY;
    unsigned int enterMilliseconds;
    unsigned int holdMilliseconds;
    char exitAnimation[16];
    float exitToX;
    float exitToY;
    unsigned int exitMilliseconds;
    bool freezeWhilePaused;
};

const Config& Get();
const Config& GetViolation();
const Config& GetPlayerFoul();
const Config& GetStat();
bool Load(const char* themeName);
bool Reload(const char* themeName);
bool LoadViolation(const char* themeName);
bool ReloadViolation(const char* themeName);
bool LoadPlayerFoul(const char* themeName);
bool ReloadPlayerFoul(const char* themeName);
bool LoadStat(const char* themeName, const char* subtypeKey,
    bool playerPayload, int valueCase = 0);
bool ReloadStat(const char* themeName, const char* subtypeKey,
    bool playerPayload, int valueCase = 0);
const char* GetLastError();

} // namespace scoreboardconfig
