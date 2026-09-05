using System.Text.Json.Serialization;

namespace NBALiveScoreboardEditor;

public sealed class ScoreboardTheme
{
    public List<OverlayElement> Elements { get; set; } = [];
    public OverlayAnimation Animation { get; set; } = new();
    public int ReferenceWidth { get; set; } = 1366;
    public int ReferenceHeight { get; set; } = 768;
    public string ScaleMode { get; set; } = "uniform";
    public double OffsetX { get; set; }
    public double OffsetY { get; set; } = 18;
    public double ScoreboardWidth { get; set; } = 620;
    public double ScoreboardHeight { get; set; } = 68;
    public int OverlayZ { get; set; } = 10;
    public int BackgroundColor { get; set; } = 987672;
    public int BackgroundAlpha { get; set; } = 232;
    public bool ShowBackgroundImage { get; set; }
    public string BackgroundImage { get; set; } = "images/background.png";
    public int BackgroundImageAlpha { get; set; } = 255;
    public bool ShowAccent { get; set; } = true;
    public double AccentHeight { get; set; } = 6;
    public string VisibilityMode { get; set; } = "always";
    public int ShowAfterScoreMilliseconds { get; set; } = 5000;
    public int AlwaysShowBelowSeconds { get; set; } = 120;
    public string ShotClockVisibility { get; set; } = "always";
    public int ShotClockThreshold { get; set; } = 10;
    public int UrgentShotClockThreshold { get; set; } = 10;
    public int ShotClockNormalColor { get; set; } = 16764480;
    public int ShotClockUrgentColor { get; set; } = 16724016;
    public bool ShowTeamNames { get; set; }
    public bool ShowAwayLogo { get; set; } = true;
    public bool ShowHomeLogo { get; set; } = true;
    public string TeamNameFormat { get; set; } = "abbreviation";
    public string PeriodFormat { get; set; } = "ordinal";
    public string FoulMode { get; set; } = "none";
    public string TimeoutMode { get; set; } = "none";
    public int MaximumTeamFouls { get; set; } = 5;
    public int MaximumTimeouts { get; set; } = 6;
    public string TimeoutCountMode { get; set; } = "remaining";
    public string FoulImage { get; set; } = "images/foul.png";
    public string TimeoutImage { get; set; } = "images/timeout.png";
    public bool ShowBonus { get; set; }
    public int BonusThreshold { get; set; } = 5;
    public int DoubleBonusThreshold { get; set; }
    public string BonusText { get; set; } = "BONUS";
    public string DoubleBonusText { get; set; } = "BONUS+";

    public double AwayPanelX { get; set; } public double AwayPanelY { get; set; }
    public double AwayPanelWidth { get; set; } = 186; public double AwayPanelHeight { get; set; } = 68;
    public double HomePanelX { get; set; } = 434; public double HomePanelY { get; set; }
    public double HomePanelWidth { get; set; } = 186; public double HomePanelHeight { get; set; } = 68;
    public double AwayLogoX { get; set; } = 12; public double AwayLogoY { get; set; } = 5;
    public double AwayLogoWidth { get; set; } = 162; public double AwayLogoHeight { get; set; } = 48;
    public double HomeLogoX { get; set; } = 446; public double HomeLogoY { get; set; } = 5;
    public double HomeLogoWidth { get; set; } = 162; public double HomeLogoHeight { get; set; } = 48;
    public double AwayNameX { get; set; } = 12; public double AwayNameY { get; set; } = 49;
    public double AwayNameWidth { get; set; } = 162; public double AwayNameHeight { get; set; } = 15;
    public double HomeNameX { get; set; } = 446; public double HomeNameY { get; set; } = 49;
    public double HomeNameWidth { get; set; } = 162; public double HomeNameHeight { get; set; } = 15;
    public double AwayScoreX { get; set; } = 190; public double AwayScoreY { get; set; } = 14;
    public double AwayScoreWidth { get; set; } = 46; public double AwayScoreHeight { get; set; } = 36;
    public double HomeScoreX { get; set; } = 376; public double HomeScoreY { get; set; } = 14;
    public double HomeScoreWidth { get; set; } = 46; public double HomeScoreHeight { get; set; } = 36;
    public double GameClockX { get; set; } = 248; public double GameClockY { get; set; } = 7;
    public double GameClockWidth { get; set; } = 124; public double GameClockHeight { get; set; } = 30;
    public double ShotClockX { get; set; } = 334; public double ShotClockY { get; set; } = 41;
    public double ShotClockWidth { get; set; } = 30; public double ShotClockHeight { get; set; } = 18;
    public double PeriodX { get; set; } = 256; public double PeriodY { get; set; } = 42;
    public double PeriodWidth { get; set; } = 68; public double PeriodHeight { get; set; } = 18;
    public double AwayFoulsX { get; set; } = 12; public double AwayFoulsY { get; set; } = 49;
    public double AwayFoulsWidth { get; set; } = 80; public double AwayFoulsHeight { get; set; } = 15;
    public double HomeFoulsX { get; set; } = 528; public double HomeFoulsY { get; set; } = 49;
    public double HomeFoulsWidth { get; set; } = 80; public double HomeFoulsHeight { get; set; } = 15;
    public double AwayTimeoutsX { get; set; } = 94; public double AwayTimeoutsY { get; set; } = 49;
    public double AwayTimeoutsWidth { get; set; } = 80; public double AwayTimeoutsHeight { get; set; } = 15;
    public double HomeTimeoutsX { get; set; } = 446; public double HomeTimeoutsY { get; set; } = 49;
    public double HomeTimeoutsWidth { get; set; } = 80; public double HomeTimeoutsHeight { get; set; } = 15;
    public double AwayBonusX { get; set; } = 188; public double AwayBonusY { get; set; } = 51;
    public double AwayBonusWidth { get; set; } = 58; public double AwayBonusHeight { get; set; } = 13;
    public double HomeBonusX { get; set; } = 374; public double HomeBonusY { get; set; } = 51;
    public double HomeBonusWidth { get; set; } = 58; public double HomeBonusHeight { get; set; } = 13;
}

public sealed class OverlayAnimation
{
    public AnimationTransition Enter { get; set; } = new()
    {
        Type = "slideFade", FromX = -80, Duration = 250
    };
    public int HoldMilliseconds { get; set; } = 2500;
    public AnimationTransition Exit { get; set; } = new()
    {
        Type = "fade", Duration = 200
    };
    public bool FreezeWhilePaused { get; set; } = true;
}

public sealed class AnimationTransition
{
    public string Type { get; set; } = "none";
    public double FromX { get; set; }
    public double FromY { get; set; }
    public double ToX { get; set; }
    public double ToY { get; set; }
    public int Duration { get; set; }
}

public sealed class OverlayElement
{
    public string Id { get; set; } = "element";
    public string Type { get; set; } = "rectangle";
    public string Binding { get; set; } = "";
    public string Text { get; set; } = "";
    public string Image { get; set; } = "";
    public string Font { get; set; } = "";
    public double X { get; set; }
    public double Y { get; set; }
    public double Width { get; set; } = 100;
    public double Height { get; set; } = 30;
    public int Z { get; set; }
    public bool Visible { get; set; } = true;
    public bool Locked { get; set; }
    public string ImageFit { get; set; } = "";
    public bool TintEnabled { get; set; }
    public string TintBinding { get; set; } = "";
    public int TintColor { get; set; } = 16777215;
    public string Alignment { get; set; } = "center";
    public string Overflow { get; set; } = "overflow";
    public string TextTransform { get; set; } = "none";
    public double SmallCapsScale { get; set; } = 0.75;
    public double FontHeight { get; set; }
    public int TextColor { get; set; } = 16777215;
    public int Opacity { get; set; } = 255;
    public OverlayFill Fill { get; set; } = new();

    [JsonIgnore] public string DisplayName => $"{Z,3}  {Id}";
}

public sealed class OverlayFill
{
    public string Type { get; set; } = "solid";
    public string Binding { get; set; } = "";
    public int Color { get; set; } = 16777215;
    public int StartColor { get; set; } = 16777215;
    public int EndColor { get; set; }
    public string StartBinding { get; set; } = "";
    public string EndBinding { get; set; } = "";
    public string Direction { get; set; } = "vertical";
}

public sealed class PopupFontTheme
{
    public Dictionary<string, FontDefinition> Fonts { get; set; } = [];
    public string FontFile { get; set; } = "fonts/scoreboard.ttf";
    public string FontFace { get; set; } = "Arial";
    public int FontSourceHeight { get; set; } = 48;
    public int FontWeight { get; set; } = 600;
    public int CharacterSpacing { get; set; } = 1;
    public int ScoreHeight { get; set; } = 34;
    public int ClockHeight { get; set; } = 28;
    public int ShotClockHeight { get; set; } = 17;
    public int TeamNameHeight { get; set; } = 15;
    public int PeriodHeight { get; set; } = 18;
    public int FoulHeight { get; set; } = 15;
    public int TimeoutHeight { get; set; } = 15;
    public int BonusHeight { get; set; } = 13;
    public int ScoreColor { get; set; } = 16777215;
    public int ClockColor { get; set; } = 16777215;
    public int ShotClockColor { get; set; } = 16764480;
}

public sealed class FontDefinition
{
    public string FontFile { get; set; } = "fonts/scoreboard.ttf";
    public string FontFace { get; set; } = "Arial";
    public int FontSourceHeight { get; set; } = 48;
    public int FontWeight { get; set; } = 600;
    public int CharacterSpacing { get; set; } = 1;
}

public sealed class TeamDefinition
{
    [JsonPropertyName("TEAMNUM")] public int TeamNumber { get; set; }
    [JsonPropertyName("TEAMNAME")] public string TeamName { get; set; } = "Team";
    [JsonPropertyName("CITYNAME")] public string CityName { get; set; } = "City";
    [JsonPropertyName("ABBREV")] public string Abbreviation { get; set; } = "TM";
    [JsonPropertyName("TEAMABR2")] public string ShortCode { get; set; } = "";
    [JsonPropertyName("PRIRGB")] public int PrimaryColor { get; set; }
    [JsonPropertyName("SECRGB")] public int SecondaryColor { get; set; }
    [JsonPropertyName("logo")] public string Logo { get; set; } = "";
    [JsonIgnore] public string DisplayName => $"{TeamNumber}: {CityName} {TeamName}";
}
