using Microsoft.Win32;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace NBALiveScoreboardEditor;

public sealed class EditorSettings
{
    public string PreviewBackgroundPath { get; set; } = "";
    public string PreviewBackgroundFit { get; set; } = "fill";
}

public partial class MainWindow : Window
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        WriteIndented = true
    };
    private static readonly string[] EditableElements =
    [
        "awayPanel", "homePanel",
        "awayLogo", "homeLogo",
        "awayName", "homeName",
        "awayScore", "homeScore",
        "gameClock", "shotClock", "period",
        "awayFouls", "homeFouls",
        "awayTimeouts", "homeTimeouts",
        "awayBonus", "homeBonus"
    ];

    private ScoreboardTheme _theme = new();
    private PopupFontTheme _font = new();
    private readonly List<TeamDefinition> _teams = [];
    private string? _themeDirectory;
    private string? _activeDocumentPath;
    private FrameworkElement? _selected;
    private string? _selectedPrefix;
    private bool _dragging;
    private bool _draggingScoreboard;
    private Point _dragStart;
    private Point _scoreboardDragStart;
    private double _elementStartX;
    private double _elementStartY;
    private double _scoreboardStartOffsetX;
    private double _scoreboardStartOffsetY;
    private bool _loadingControls;
    private bool _loadingPreviewSettings;
    private EditorSettings _editorSettings = new();

    private static string EditorSettingsPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "NBALiveScoreboardEditor", "editor-settings.json");

    public MainWindow()
    {
        InitializeComponent();
        VisibilityCombo.ItemsSource = new[] { "always", "afterScore", "lateGameOnly", "afterScoreAndLateGame" };
        ShotVisibilityCombo.ItemsSource = new[] { "always", "underThreshold", "never" };
        TeamFormatCombo.ItemsSource = new[] { "abbreviation", "city", "nickname", "fullName", "shortCode" };
        PeriodFormatCombo.ItemsSource = new[] { "number", "ordinal", "ordinalQuarter", "shortQuarter", "longQuarter" };
        FoulModeCombo.ItemsSource = TimeoutModeCombo.ItemsSource =
            new[] { "none", "number", "text", "dots", "bars", "images" };
        ScaleModeCombo.ItemsSource = new[] { "uniform", "fixed", "positionOnly" };
        LayoutScaleModeCombo.ItemsSource = new[] { "uniform", "fixed", "positionOnly" };
        PreviewAspectCombo.ItemsSource = new[] { "4:3", "16:9", "16:10" };
        PreviewAspectCombo.SelectedItem = "16:9";
        PreviewBackgroundFitCombo.ItemsSource = new[] { "fill", "contain", "stretch" };
        ElementSelector.ItemsSource = EditableElements;
        ElementAlignmentCombo.ItemsSource = new[] { "left", "center", "right" };
        ElementTypeCombo.ItemsSource = new[] { "rectangle", "image", "text", "indicator" };
        ElementImageFitCombo.ItemsSource = new[] { "contain", "stretch" };
        ElementOverflowCombo.ItemsSource = new[] { "overflow", "fit" };
        ElementFillTypeCombo.ItemsSource = new[] { "solid", "linearGradient" };
        GradientDirectionCombo.ItemsSource = new[] { "vertical", "horizontal" };
        LoadEditorSettings();
        EnsureElements();
        ElementSelector.SelectedItem = "awayPanel";
        LoadControls();
        RebuildPreview();
    }

    private void LoadEditorSettings()
    {
        _loadingPreviewSettings = true;
        try
        {
            if (File.Exists(EditorSettingsPath))
                _editorSettings = JsonSerializer.Deserialize<EditorSettings>(
                    File.ReadAllText(EditorSettingsPath), JsonOptions) ?? new();
        }
        catch
        {
            _editorSettings = new();
        }
        PreviewBackgroundPathBox.Text = _editorSettings.PreviewBackgroundPath;
        PreviewBackgroundFitCombo.SelectedItem =
            _editorSettings.PreviewBackgroundFit;
        if (PreviewBackgroundFitCombo.SelectedItem is null)
            PreviewBackgroundFitCombo.SelectedItem = "fill";
        ApplyPreviewBackground(false);
        _loadingPreviewSettings = false;
    }

    private void SaveEditorSettings()
    {
        string? directory = Path.GetDirectoryName(EditorSettingsPath);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        File.WriteAllText(EditorSettingsPath,
            JsonSerializer.Serialize(_editorSettings, JsonOptions));
    }

    private void ApplyPreviewBackground(bool persist)
    {
        string path = PreviewBackgroundPathBox.Text.Trim();
        string fit = PreviewBackgroundFitCombo.SelectedItem as string ?? "fill";
        PreviewBackground.Stretch = fit switch
        {
            "contain" => Stretch.Uniform,
            "stretch" => Stretch.Fill,
            _ => Stretch.UniformToFill
        };

        if (path.Length == 0)
        {
            PreviewBackground.Source = new BitmapImage(
                new Uri("pack://application:,,,/Assets/broadcast-court.png",
                    UriKind.Absolute));
        }
        else if (File.Exists(path))
        {
            BitmapImage image = new();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.UriSource = new Uri(Path.GetFullPath(path), UriKind.Absolute);
            image.EndInit();
            image.Freeze();
            PreviewBackground.Source = image;
        }
        else
        {
            StatusText.Text = $"Reference background not found: {path}";
            return;
        }

        _editorSettings.PreviewBackgroundPath = path;
        _editorSettings.PreviewBackgroundFit = fit;
        if (persist) SaveEditorSettings();
    }

    private void BrowsePreviewBackground_Click(object sender, RoutedEventArgs e)
    {
        OpenFileDialog dialog = new()
        {
            Title = "Select editor reference background",
            Filter = "Image files|*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff|All files|*.*"
        };
        string current = PreviewBackgroundPathBox.Text.Trim();
        if (File.Exists(current))
            dialog.InitialDirectory = Path.GetDirectoryName(current);
        if (dialog.ShowDialog(this) != true) return;
        PreviewBackgroundPathBox.Text = dialog.FileName;
        ApplyPreviewBackground(true);
        StatusText.Text = $"Reference background: {dialog.FileName}";
    }

    private void ApplyPreviewBackground_Click(object sender, RoutedEventArgs e)
    {
        ApplyPreviewBackground(true);
    }

    private void ResetPreviewBackground_Click(object sender, RoutedEventArgs e)
    {
        PreviewBackgroundPathBox.Text = "";
        PreviewBackgroundFitCombo.SelectedItem = "fill";
        ApplyPreviewBackground(true);
        StatusText.Text = "Using built-in reference background.";
    }

    private void PreviewBackgroundFit_Changed(object sender,
        SelectionChangedEventArgs e)
    {
        if (_loadingPreviewSettings || !IsLoaded) return;
        ApplyPreviewBackground(true);
    }

    private void OpenTheme_Click(object sender, RoutedEventArgs e)
    {
        OpenFileDialog dialog = new()
        {
            Title = "Open a popup scoreboard layout",
            Filter = "NBA Live scoreboard layout|scoreboard.json|JSON files|*.json"
        };
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            _themeDirectory = Path.GetDirectoryName(dialog.FileName)!;
            _activeDocumentPath = Path.GetFullPath(dialog.FileName);
            _theme = JsonSerializer.Deserialize<ScoreboardTheme>(
                File.ReadAllText(dialog.FileName), JsonOptions) ?? new();
            EnsureElements();
            string popupPath = Path.Combine(_themeDirectory, "popup.json");
            _font = File.Exists(popupPath)
                ? JsonSerializer.Deserialize<PopupFontTheme>(
                    File.ReadAllText(popupPath), JsonOptions) ?? new()
                : new();
            _font.Fonts ??= [];
            LoadTeams();
            LoadControls();
            RebuildPreview();
            UpdateDocumentCaption();
            StatusText.Text = $"Loaded {_activeDocumentPath}";
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "Unable to open theme",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void LoadTeams()
    {
        _teams.Clear();
        string path = Path.Combine(_themeDirectory!, "teams.json");
        if (!File.Exists(path)) return;
        using JsonDocument document = JsonDocument.Parse(File.ReadAllText(path));
        if (!document.RootElement.TryGetProperty("teams", out JsonElement teams)) return;
        foreach (JsonProperty property in teams.EnumerateObject())
        {
            TeamDefinition? team = property.Value.Deserialize<TeamDefinition>(JsonOptions);
            if (team is not null) _teams.Add(team);
        }
        _teams.Sort((a, b) => a.TeamNumber.CompareTo(b.TeamNumber));
        AwayTeamCombo.ItemsSource = _teams;
        HomeTeamCombo.ItemsSource = _teams;
        if (_teams.Count > 0)
        {
            AwayTeamCombo.SelectedIndex = 0;
            HomeTeamCombo.SelectedIndex = Math.Min(7, _teams.Count - 1);
        }
    }

    private void Save_Click(object sender, RoutedEventArgs e) => Save(false);
    private void SaveReload_Click(object sender, RoutedEventArgs e) => Save(true);
    private void SaveAs_Click(object sender, RoutedEventArgs e)
    {
        if (_themeDirectory is null)
        {
            MessageBox.Show(this, "Open a theme first.");
            return;
        }

        SaveFileDialog dialog = new()
        {
            Title = "Save scoreboard layout as",
            Filter = "JSON files|*.json",
            InitialDirectory = _themeDirectory,
            FileName = _activeDocumentPath is null
                ? "scoreboard.json"
                : Path.GetFileName(_activeDocumentPath),
            AddExtension = true,
            DefaultExt = ".json",
            OverwritePrompt = true
        };

        if (dialog.ShowDialog(this) != true) return;
        Save(false, Path.GetFullPath(dialog.FileName));
    }

    private void Save(bool reload, string? destinationPath = null)
    {
        string? savePath = destinationPath ?? _activeDocumentPath;
        if (_themeDirectory is null || savePath is null)
        {
            MessageBox.Show(this, "Open a theme first.");
            return;
        }
        try
        {
            ApplyLayoutFromControls();
            ApplyBehaviorFromControls();
            ApplyFontFromControls();
            RebuildPreview();
            string destinationDirectory = Path.GetDirectoryName(savePath)!;
            Directory.CreateDirectory(destinationDirectory);
            File.WriteAllText(savePath,
                JsonSerializer.Serialize(_theme, JsonOptions));
            File.WriteAllText(Path.Combine(destinationDirectory, "popup.json"),
                JsonSerializer.Serialize(_font, JsonOptions));

            _activeDocumentPath = savePath;
            _themeDirectory = destinationDirectory;
            UpdateDocumentCaption();

            if (reload)
                File.WriteAllText(Path.Combine(destinationDirectory, ".reload"),
                    DateTime.UtcNow.Ticks.ToString());
            bool gameLoadsActiveDocument =
                string.Equals(Path.GetFileName(savePath), "scoreboard.json",
                    StringComparison.OrdinalIgnoreCase);
            StatusText.Text = reload && gameLoadsActiveDocument
                ? "Saved scoreboard.json. The running game will reload within 500 ms."
                : reload
                    ? $"Saved {Path.GetFileName(savePath)}. The game currently loads scoreboard.json."
                    : $"Saved {savePath}.";
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "Unable to save theme",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void UpdateDocumentCaption()
    {
        Title = _activeDocumentPath is null
            ? "NBA Live Scoreboard Theme Editor"
            : $"{Path.GetFileName(_activeDocumentPath)} - NBA Live Scoreboard Theme Editor";
    }

    private void LoadControls()
    {
        _loadingControls = true;
        SyncLegacyPresentationFromLayers();
        VisibilityCombo.SelectedItem = _theme.VisibilityMode;
        BackgroundColorBox.Text = _theme.BackgroundColor.ToString();
        BackgroundAlphaBox.Text = _theme.BackgroundAlpha.ToString();
        ShowBackgroundImageCheck.IsChecked = _theme.ShowBackgroundImage;
        BackgroundImageBox.Text = _theme.BackgroundImage;
        BackgroundImageAlphaBox.Text = _theme.BackgroundImageAlpha.ToString();
        ShowAccentCheck.IsChecked = _theme.ShowAccent;
        AccentHeightBox.Text = _theme.AccentHeight.ToString();
        AfterScoreBox.Text = _theme.ShowAfterScoreMilliseconds.ToString();
        LateGameBox.Text = _theme.AlwaysShowBelowSeconds.ToString();
        ShotVisibilityCombo.SelectedItem = _theme.ShotClockVisibility;
        ShotThresholdBox.Text = _theme.ShotClockThreshold.ToString();
        UrgentThresholdBox.Text = _theme.UrgentShotClockThreshold.ToString();
        NormalShotColorBox.Text = _theme.ShotClockNormalColor.ToString();
        UrgentShotColorBox.Text = _theme.ShotClockUrgentColor.ToString();
        ShowTeamNamesCheck.IsChecked = _theme.ShowTeamNames;
        ShowAwayLogoCheck.IsChecked = _theme.ShowAwayLogo;
        ShowHomeLogoCheck.IsChecked = _theme.ShowHomeLogo;
        TeamFormatCombo.SelectedItem = _theme.TeamNameFormat;
        PeriodFormatCombo.SelectedItem = _theme.PeriodFormat;
        FoulModeCombo.SelectedItem = _theme.FoulMode;
        TimeoutModeCombo.SelectedItem = _theme.TimeoutMode;
        ShowBonusCheck.IsChecked = _theme.ShowBonus;
        BonusThresholdBox.Text = _theme.BonusThreshold.ToString();
        ScaleModeCombo.SelectedItem = _theme.ScaleMode;
        ReferenceWidthBox.Text = _theme.ReferenceWidth.ToString();
        ReferenceHeightBox.Text = _theme.ReferenceHeight.ToString();
        LayoutScaleModeCombo.SelectedItem = _theme.ScaleMode;
        ScoreboardOffsetXBox.Text = _theme.OffsetX.ToString("0.##");
        ScoreboardOffsetYBox.Text = _theme.OffsetY.ToString("0.##");
        ScoreboardWidthBox.Text = _theme.ScoreboardWidth.ToString("0.##");
        ScoreboardHeightBox.Text = _theme.ScoreboardHeight.ToString("0.##");
        FontFileBox.Text = _font.FontFile;
        FontFaceBox.Text = _font.FontFace;
        FontWeightBox.Text = _font.FontWeight.ToString();
        FontSpacingBox.Text = _font.CharacterSpacing.ToString();
        ScoreHeightBox.Text = _font.ScoreHeight.ToString();
        ClockHeightBox.Text = _font.ClockHeight.ToString();
        ShotHeightBox.Text = _font.ShotClockHeight.ToString();
        TeamNameHeightBox.Text = _font.TeamNameHeight.ToString();
        PeriodHeightBox.Text = _font.PeriodHeight.ToString();
        FoulHeightBox.Text = _font.FoulHeight.ToString();
        TimeoutHeightBox.Text = _font.TimeoutHeight.ToString();
        BonusHeightBox.Text = _font.BonusHeight.ToString();
        RefreshLayerLists();
        RefreshFontPresetList();
        _loadingControls = false;
    }

    private void RefreshFontPresetList()
    {
        string current = ElementFontCombo.Text;
        ElementFontCombo.ItemsSource = new[] { "" }
            .Concat(_font.Fonts.Keys.OrderBy(x => x))
            .ToList();
        ElementFontCombo.Text = current;
    }

    private void EnsureElements()
    {
        _theme.Elements ??= [];
        if (_theme.Elements.Count > 0)
        {
            foreach (OverlayElement image in
                _theme.Elements.Where(x => x.Type == "image" &&
                    string.IsNullOrWhiteSpace(x.ImageFit)))
                image.ImageFit = image.Id == "backgroundImage"
                    ? "stretch" : "contain";
            return;
        }
        OverlayElement Make(string id, string type, string binding,
            double x, double y, double width, double height, int z,
            string fillBinding = "") => new()
        {
            Id = id, Type = type, Binding = binding, X = x, Y = y,
            Width = width, Height = height, Z = z,
            Fill = new OverlayFill { Binding = fillBinding }
        };
        _theme.Elements =
        [
            new OverlayElement { Id="background", Type="rectangle", Width=_theme.ScoreboardWidth, Height=_theme.ScoreboardHeight, Z=0,
                Opacity=_theme.BackgroundAlpha, Fill=new OverlayFill { Color=_theme.BackgroundColor } },
            new OverlayElement { Id="backgroundImage", Type="image", Image=_theme.BackgroundImage,
                Width=_theme.ScoreboardWidth, Height=_theme.ScoreboardHeight, Z=1,
                Visible=_theme.ShowBackgroundImage, Opacity=_theme.BackgroundImageAlpha,
                ImageFit="stretch" },
            Make("awayPanel", "rectangle", "", _theme.AwayPanelX, _theme.AwayPanelY, _theme.AwayPanelWidth, _theme.AwayPanelHeight, 10, "away.primaryColor"),
            Make("homePanel", "rectangle", "", _theme.HomePanelX, _theme.HomePanelY, _theme.HomePanelWidth, _theme.HomePanelHeight, 10, "home.primaryColor"),
            Make("awayAccent", "rectangle", "", _theme.AwayPanelX, _theme.AwayPanelY + _theme.AwayPanelHeight - _theme.AccentHeight, _theme.AwayPanelWidth, _theme.AccentHeight, 15, "away.secondaryColor"),
            Make("homeAccent", "rectangle", "", _theme.HomePanelX, _theme.HomePanelY + _theme.HomePanelHeight - _theme.AccentHeight, _theme.HomePanelWidth, _theme.AccentHeight, 15, "home.secondaryColor"),
            Make("awayLogo", "image", "away.logo", _theme.AwayLogoX, _theme.AwayLogoY, _theme.AwayLogoWidth, _theme.AwayLogoHeight, 20),
            Make("homeLogo", "image", "home.logo", _theme.HomeLogoX, _theme.HomeLogoY, _theme.HomeLogoWidth, _theme.HomeLogoHeight, 20),
            Make("awayName", "text", "away.name", _theme.AwayNameX, _theme.AwayNameY, _theme.AwayNameWidth, _theme.AwayNameHeight, 30),
            Make("homeName", "text", "home.name", _theme.HomeNameX, _theme.HomeNameY, _theme.HomeNameWidth, _theme.HomeNameHeight, 30),
            Make("awayScore", "text", "away.score", _theme.AwayScoreX, _theme.AwayScoreY, _theme.AwayScoreWidth, _theme.AwayScoreHeight, 30),
            Make("homeScore", "text", "home.score", _theme.HomeScoreX, _theme.HomeScoreY, _theme.HomeScoreWidth, _theme.HomeScoreHeight, 30),
            Make("gameClock", "text", "game.clock", _theme.GameClockX, _theme.GameClockY, _theme.GameClockWidth, _theme.GameClockHeight, 30),
            Make("shotClock", "text", "game.shotClock", _theme.ShotClockX, _theme.ShotClockY, _theme.ShotClockWidth, _theme.ShotClockHeight, 30),
            Make("period", "text", "game.period", _theme.PeriodX, _theme.PeriodY, _theme.PeriodWidth, _theme.PeriodHeight, 30),
            Make("awayFouls", "indicator", "away.fouls", _theme.AwayFoulsX, _theme.AwayFoulsY, _theme.AwayFoulsWidth, _theme.AwayFoulsHeight, 30),
            Make("homeFouls", "indicator", "home.fouls", _theme.HomeFoulsX, _theme.HomeFoulsY, _theme.HomeFoulsWidth, _theme.HomeFoulsHeight, 30),
            Make("awayTimeouts", "indicator", "away.timeouts", _theme.AwayTimeoutsX, _theme.AwayTimeoutsY, _theme.AwayTimeoutsWidth, _theme.AwayTimeoutsHeight, 30),
            Make("homeTimeouts", "indicator", "home.timeouts", _theme.HomeTimeoutsX, _theme.HomeTimeoutsY, _theme.HomeTimeoutsWidth, _theme.HomeTimeoutsHeight, 30),
            Make("awayBonus", "text", "away.bonus", _theme.AwayBonusX, _theme.AwayBonusY, _theme.AwayBonusWidth, _theme.AwayBonusHeight, 30),
            Make("homeBonus", "text", "home.bonus", _theme.HomeBonusX, _theme.HomeBonusY, _theme.HomeBonusWidth, _theme.HomeBonusHeight, 30)
        ];
        _theme.Elements.First(x => x.Id == "awayAccent").Visible = _theme.ShowAccent;
        _theme.Elements.First(x => x.Id == "homeAccent").Visible = _theme.ShowAccent;
        _theme.Elements.First(x => x.Id == "awayLogo").Visible = _theme.ShowAwayLogo;
        _theme.Elements.First(x => x.Id == "homeLogo").Visible = _theme.ShowHomeLogo;
        _theme.Elements.First(x => x.Id == "awayName").Visible = _theme.ShowTeamNames;
        _theme.Elements.First(x => x.Id == "homeName").Visible = _theme.ShowTeamNames;
        _theme.Elements.First(x => x.Id == "awayBonus").Visible = _theme.ShowBonus;
        _theme.Elements.First(x => x.Id == "homeBonus").Visible = _theme.ShowBonus;
        _theme.Elements.First(x => x.Id == "awayScore").Alignment = "right";
        _theme.Elements.First(x => x.Id == "homeScore").Alignment = "right";
        _theme.Elements.First(x => x.Id == "shotClock").Alignment = "right";
        _theme.Elements.First(x => x.Id == "awayName").Overflow = "fit";
        _theme.Elements.First(x => x.Id == "homeName").Overflow = "fit";
    }

    private void RefreshLayerLists()
    {
        string? selected = _selectedPrefix;
        var ordered = _theme.Elements.OrderByDescending(x => x.Z).ToList();
        LayersList.ItemsSource = ordered;
        ElementSelector.ItemsSource = ordered.Select(x => x.Id).ToList();
        if (selected is not null) ElementSelector.SelectedItem = selected;
    }

    private void ApplyBehavior_Click(object sender, RoutedEventArgs e)
    {
        ApplyBehaviorFromControls();
        RebuildPreview();
    }

    private void ApplyLayout_Click(object sender, RoutedEventArgs e)
    {
        ApplyLayoutFromControls();
        RebuildPreview();
    }

    private void ApplyLayoutFromControls()
    {
        _theme.ReferenceWidth = Math.Max(1,
            Int(ReferenceWidthBox, _theme.ReferenceWidth));
        _theme.ReferenceHeight = Math.Max(1,
            Int(ReferenceHeightBox, _theme.ReferenceHeight));
        _theme.ScaleMode = LayoutScaleModeCombo.SelectedItem as string ??
            _theme.ScaleMode;
        ScaleModeCombo.SelectedItem = _theme.ScaleMode;
        _theme.OffsetX = Double(ScoreboardOffsetXBox, _theme.OffsetX);
        _theme.OffsetY = Double(ScoreboardOffsetYBox, _theme.OffsetY);
        _theme.ScoreboardWidth = Math.Max(1,
            Double(ScoreboardWidthBox, _theme.ScoreboardWidth));
        _theme.ScoreboardHeight = Math.Max(1,
            Double(ScoreboardHeightBox, _theme.ScoreboardHeight));
    }

    private void ApplyBehaviorFromControls()
    {
        _theme.VisibilityMode = VisibilityCombo.SelectedItem as string ?? "always";
        _theme.BackgroundColor = Int(BackgroundColorBox, 987672);
        _theme.BackgroundAlpha = Math.Clamp(Int(BackgroundAlphaBox, 232), 0, 255);
        _theme.ShowBackgroundImage = ShowBackgroundImageCheck.IsChecked == true;
        _theme.BackgroundImage = BackgroundImageBox.Text.Trim();
        _theme.BackgroundImageAlpha = Math.Clamp(
            Int(BackgroundImageAlphaBox, 255), 0, 255);
        _theme.ShowAccent = ShowAccentCheck.IsChecked == true;
        _theme.AccentHeight = Math.Max(0, Double(AccentHeightBox, 6));
        _theme.ShowAfterScoreMilliseconds = Int(AfterScoreBox, 5000);
        _theme.AlwaysShowBelowSeconds = Int(LateGameBox, 120);
        _theme.ShotClockVisibility = ShotVisibilityCombo.SelectedItem as string ?? "always";
        _theme.ShotClockThreshold = Int(ShotThresholdBox, 10);
        _theme.UrgentShotClockThreshold = Int(UrgentThresholdBox, 10);
        _theme.ShotClockNormalColor = Int(NormalShotColorBox, 16764480);
        _theme.ShotClockUrgentColor = Int(UrgentShotColorBox, 16724016);
        _theme.ShowTeamNames = ShowTeamNamesCheck.IsChecked == true;
        _theme.ShowAwayLogo = ShowAwayLogoCheck.IsChecked == true;
        _theme.ShowHomeLogo = ShowHomeLogoCheck.IsChecked == true;
        _theme.TeamNameFormat = TeamFormatCombo.SelectedItem as string ?? "abbreviation";
        _theme.PeriodFormat = PeriodFormatCombo.SelectedItem as string ?? "ordinal";
        _theme.FoulMode = FoulModeCombo.SelectedItem as string ?? "none";
        _theme.TimeoutMode = TimeoutModeCombo.SelectedItem as string ?? "none";
        _theme.ShowBonus = ShowBonusCheck.IsChecked == true;
        _theme.BonusThreshold = Int(BonusThresholdBox, 5);
        _theme.ScaleMode = ScaleModeCombo.SelectedItem as string ?? "uniform";
        // Once elements[] exists, layer properties are authoritative. Mirror
        // them back into the legacy fields instead of overwriting layer edits
        // whenever Save calls this method.
        SyncLegacyPresentationFromLayers();
    }

    private void SyncLegacyPresentationFromLayers()
    {
        if (_theme.Elements.Count == 0) return;

        OverlayElement? background = Layer("background");
        if (background is not null)
        {
            _theme.BackgroundColor = background.Fill.Color;
            _theme.BackgroundAlpha = background.Opacity;
        }

        OverlayElement? image = Layer("backgroundImage");
        if (image is not null)
        {
            _theme.ShowBackgroundImage = image.Visible;
            _theme.BackgroundImage = image.Image;
            _theme.BackgroundImageAlpha = image.Opacity;
        }

        _theme.ShowAccent =
            Layer("awayAccent")?.Visible == true ||
            Layer("homeAccent")?.Visible == true;
        _theme.ShowAwayLogo = Layer("awayLogo")?.Visible == true;
        _theme.ShowHomeLogo = Layer("homeLogo")?.Visible == true;
        _theme.ShowTeamNames =
            Layer("awayName")?.Visible == true ||
            Layer("homeName")?.Visible == true;
        _theme.ShowBonus =
            Layer("awayBonus")?.Visible == true ||
            Layer("homeBonus")?.Visible == true;
    }

    private void ApplyFont_Click(object sender, RoutedEventArgs e)
    {
        ApplyFontFromControls();
        RebuildPreview();
        ShowAppliedFontStatus();
    }

    private void FontTextChanged(object sender, TextChangedEventArgs e)
    {
        if (_loadingControls || !IsLoaded) return;
        ApplyFontFromControls();
        RebuildPreview();
        ShowAppliedFontStatus();
    }

    private void ApplyFontFromControls()
    {
        _font.FontFile = FontFileBox.Text.Trim();
        _font.FontFace = FontFaceBox.Text.Trim();
        _font.FontWeight = Int(FontWeightBox, 600);
        _font.CharacterSpacing = Int(FontSpacingBox, 1);
        _font.ScoreHeight = Int(ScoreHeightBox, 34);
        _font.ClockHeight = Int(ClockHeightBox, 28);
        _font.ShotClockHeight = Int(ShotHeightBox, 17);
        _font.TeamNameHeight = Int(TeamNameHeightBox, 15);
        _font.PeriodHeight = Int(PeriodHeightBox, 18);
        _font.FoulHeight = Int(FoulHeightBox, 15);
        _font.TimeoutHeight = Int(TimeoutHeightBox, 15);
        _font.BonusHeight = Int(BonusHeightBox, 13);
    }

    private void ShowAppliedFontStatus()
    {
        StatusText.Text =
            $"Font preview: score {_font.ScoreHeight}, " +
            $"clock {_font.ClockHeight}, shot {_font.ShotClockHeight}, " +
            $"team {_font.TeamNameHeight}, period {_font.PeriodHeight}, " +
            $"foul {_font.FoulHeight}, timeout {_font.TimeoutHeight}, " +
            $"bonus {_font.BonusHeight}";
    }

    private void RebuildPreview()
    {
        UpdatePreviewStage();
        PreviewCanvas.Width = Math.Max(1, _theme.ScoreboardWidth);
        PreviewCanvas.Height = Math.Max(1, _theme.ScoreboardHeight);
        PreviewCanvas.Background = _theme.Elements.Count > 0
            ? Brushes.Transparent
            : PackedBrush(_theme.BackgroundColor, _theme.BackgroundAlpha);
        PreviewCanvas.Children.Clear();
        if (_theme.Elements.Count == 0) AddBackgroundImage();
        TeamDefinition away = AwayTeamCombo.SelectedItem as TeamDefinition ??
            _teams.FirstOrDefault() ?? new() { Abbreviation = "AWY", TeamName = "Away", CityName = "Away", PrimaryColor = 3030876, SecondaryColor = 14013909 };
        TeamDefinition home = HomeTeamCombo.SelectedItem as TeamDefinition ??
            _teams.Skip(1).FirstOrDefault() ?? new() { Abbreviation = "HME", TeamName = "Home", CityName = "Home", PrimaryColor = 9969197, SecondaryColor = 16777215 };
        if (_theme.Elements.Count > 0)
        {
            RebuildLayerPreview(away, home);
            RestoreElementSelection();
            return;
        }
        AddRectangle("awayPanel", PackedBrush(away.PrimaryColor));
        AddRectangle("homePanel", PackedBrush(home.PrimaryColor));
        if (_theme.ShowAccent && _theme.AccentHeight > 0)
        {
            AddAccent(away, true);
            AddAccent(home, false);
        }
        if (_theme.ShowAwayLogo) AddImage("awayLogo", away);
        if (_theme.ShowHomeLogo) AddImage("homeLogo", home);
        if (_theme.ShowTeamNames)
        {
            AddText("awayName", FormatTeam(away), _font.TeamNameHeight);
            AddText("homeName", FormatTeam(home), _font.TeamNameHeight);
        }
        AddText("awayScore", AwayScoreBox.Text, _font.ScoreHeight);
        AddText("homeScore", HomeScoreBox.Text, _font.ScoreHeight);
        AddText("gameClock", ClockBox.Text, _font.ClockHeight);
        if (ShouldShowShotClock())
        {
            int shot = int.TryParse(ShotBox.Text, out int value) ? value : 24;
            AddText("shotClock", ShotBox.Text, _font.ShotClockHeight,
                PackedBrush(shot <= _theme.UrgentShotClockThreshold
                    ? _theme.ShotClockUrgentColor : _theme.ShotClockNormalColor));
        }
        AddText("period", FormatPeriod(), _font.PeriodHeight);
        AddIndicator("awayFouls", _theme.FoulMode,
            Int(AwayFoulsBox, 0), "F", _font.FoulHeight);
        AddIndicator("homeFouls", _theme.FoulMode,
            Int(HomeFoulsBox, 0), "F", _font.FoulHeight);
        AddIndicator("awayTimeouts", _theme.TimeoutMode,
            Int(AwayTimeoutsBox, 0), "TO", _font.TimeoutHeight);
        AddIndicator("homeTimeouts", _theme.TimeoutMode,
            Int(HomeTimeoutsBox, 0), "TO", _font.TimeoutHeight);
        if (_theme.ShowBonus && Int(HomeFoulsBox, 0) >= _theme.BonusThreshold)
            AddText("awayBonus", _theme.BonusText, _font.BonusHeight);
        if (_theme.ShowBonus && Int(AwayFoulsBox, 0) >= _theme.BonusThreshold)
            AddText("homeBonus", _theme.BonusText, _font.BonusHeight);
        RestoreElementSelection();
    }

    private void RebuildLayerPreview(TeamDefinition away, TeamDefinition home)
    {
        foreach (OverlayElement layer in _theme.Elements.OrderBy(x => x.Z))
        {
            if (!layer.Visible) continue;
            Border border = CreateBorder(layer.Id);
            border.Opacity = Math.Clamp(layer.Opacity, 0, 255) / 255.0;
            if (layer.Type == "rectangle")
            {
                int Resolve(string binding, int fallback) => binding switch
                {
                    "away.primaryColor" => away.PrimaryColor,
                    "away.secondaryColor" => away.SecondaryColor,
                    "home.primaryColor" => home.PrimaryColor,
                    "home.secondaryColor" => home.SecondaryColor,
                    _ => fallback
                };
                int start = Resolve(layer.Fill.StartBinding, layer.Fill.StartColor);
                int end = Resolve(layer.Fill.EndBinding, layer.Fill.EndColor);
                border.Background = layer.Fill.Type == "linearGradient"
                    ? new LinearGradientBrush(PackedBrush(start).Color,
                        PackedBrush(end).Color,
                        layer.Fill.Direction == "horizontal" ? 0 : 90)
                    : PackedBrush(Resolve(layer.Fill.Binding, layer.Fill.Color));
            }
            else if (layer.Type == "image")
            {
                string? path = layer.Binding switch
                {
                    "away.logo" => TeamLogoPath(away),
                    "home.logo" => TeamLogoPath(home),
                    _ when _themeDirectory is not null && layer.Image.Length > 0 =>
                        Path.GetFullPath(Path.Combine(_themeDirectory,
                            layer.Image.Replace('/', Path.DirectorySeparatorChar))),
                    _ => null
                };
                if (path is not null && File.Exists(path))
                    border.Child = new Image { Source = new BitmapImage(
                        new Uri(path, UriKind.Absolute)), Stretch =
                        layer.ImageFit == "stretch" ? Stretch.Fill : Stretch.Uniform };
            }
            else
            {
                string value = ResolvePreviewBinding(layer.Binding, away, home,
                    layer.Text);
                double height = layer.FontHeight > 0 ? layer.FontHeight :
                    DefaultFontHeight(layer.Binding);
                TextBlock text = PreviewText(value, height,
                    PackedBrush(layer.TextColor), layer.Font);
                text.TextAlignment = layer.Alignment switch
                {
                    "left" => TextAlignment.Left,
                    "right" => TextAlignment.Right,
                    _ => TextAlignment.Center
                };
                // Fill the layer's complete width so TextAlignment describes
                // alignment inside the same box used by the game renderer.
                text.HorizontalAlignment = HorizontalAlignment.Stretch;
                border.Child = layer.Overflow == "fit"
                    ? new Viewbox { Stretch = Stretch.Uniform, Child = text }
                    : text;
            }
            Panel.SetZIndex(border, layer.Z);
            AddToCanvas(border, layer.Id);
        }
    }

    private string? TeamLogoPath(TeamDefinition team) =>
        _themeDirectory is null || string.IsNullOrWhiteSpace(team.Logo) ? null :
        Path.Combine(_themeDirectory, team.Logo.Replace('/', Path.DirectorySeparatorChar));

    private string ResolvePreviewBinding(string binding, TeamDefinition away,
        TeamDefinition home, string fallback) => binding switch
    {
        "away.score" => AwayScoreBox.Text, "home.score" => HomeScoreBox.Text,
        "game.clock" => ClockBox.Text, "game.shotClock" => ShotBox.Text,
        "game.period" => FormatPeriod(), "away.name" => FormatTeam(away),
        "home.name" => FormatTeam(home), "away.fouls" => AwayFoulsBox.Text,
        "home.fouls" => HomeFoulsBox.Text,
        "away.timeouts" => AwayTimeoutsBox.Text,
        "home.timeouts" => HomeTimeoutsBox.Text,
        "away.bonus" or "home.bonus" => _theme.BonusText,
        _ => fallback
    };

    private double DefaultFontHeight(string binding) => binding switch
    {
        "away.score" or "home.score" => _font.ScoreHeight,
        "game.clock" => _font.ClockHeight, "game.shotClock" => _font.ShotClockHeight,
        "game.period" => _font.PeriodHeight,
        "away.fouls" or "home.fouls" => _font.FoulHeight,
        "away.timeouts" or "home.timeouts" => _font.TimeoutHeight,
        "away.bonus" or "home.bonus" => _font.BonusHeight,
        _ => _font.TeamNameHeight
    };

    private void UpdatePreviewStage()
    {
        const double stageWidth = 1366.0;
        string aspect = PreviewAspectCombo.SelectedItem as string ?? "16:9";
        double stageHeight = aspect switch
        {
            "4:3" => Math.Round(stageWidth * 3.0 / 4.0),
            "16:10" => Math.Round(stageWidth * 10.0 / 16.0),
            _ => Math.Round(stageWidth * 9.0 / 16.0)
        };

        PreviewStage.Width = stageWidth;
        PreviewStage.Height = stageHeight;
        PreviewBackground.Width = stageWidth;
        PreviewBackground.Height = stageHeight;

        double scale = 1.0;
        if (_theme.ScaleMode == "uniform")
        {
            double referenceWidth = Math.Max(1, _theme.ReferenceWidth);
            double referenceHeight = Math.Max(1, _theme.ReferenceHeight);
            scale = Math.Min(stageWidth / referenceWidth,
                stageHeight / referenceHeight);
        }
        PreviewCanvas.RenderTransform = new ScaleTransform(scale, scale);
        double scaledWidth = _theme.ScoreboardWidth * scale;
        double offsetScale = _theme.ScaleMode == "uniform" ? scale : 1.0;
        Canvas.SetLeft(PreviewCanvas,
            (stageWidth - scaledWidth) * 0.5 + _theme.OffsetX * offsetScale);
        Canvas.SetTop(PreviewCanvas, _theme.OffsetY * offsetScale);
    }

    private void AddRectangle(string prefix, Brush brush)
    {
        Border border = CreateBorder(prefix);
        border.Background = brush;
        AddToCanvas(border, prefix);
    }

    private void AddAccent(TeamDefinition team, bool away)
    {
        double panelX = away ? _theme.AwayPanelX : _theme.HomePanelX;
        double panelY = away ? _theme.AwayPanelY : _theme.HomePanelY;
        double panelWidth = away ? _theme.AwayPanelWidth : _theme.HomePanelWidth;
        double panelHeight = away ? _theme.AwayPanelHeight : _theme.HomePanelHeight;
        Border accent = new()
        {
            Width = Math.Max(1, panelWidth),
            Height = Math.Max(1, _theme.AccentHeight),
            Background = PackedBrush(team.SecondaryColor),
            IsHitTestVisible = false
        };
        Canvas.SetLeft(accent, panelX);
        Canvas.SetTop(accent,
            panelY + panelHeight - _theme.AccentHeight);
        PreviewCanvas.Children.Add(accent);
    }

    private void AddImage(string prefix, TeamDefinition team)
    {
        Border border = CreateBorder(prefix);
        string? path = _themeDirectory is null ? null :
            Path.Combine(_themeDirectory, team.Logo.Replace('/', Path.DirectorySeparatorChar));
        if (path is not null && File.Exists(path))
        {
            border.Child = new Image
            {
                Source = new BitmapImage(new Uri(path)),
                Stretch = Stretch.Uniform,
                IsHitTestVisible = false
            };
        }
        else
            border.Child = PreviewText(team.Abbreviation, 18, Brushes.White);
        AddToCanvas(border, prefix);
    }

    private void AddText(string prefix, string text, double height, Brush? brush = null)
    {
        Border border = CreateBorder(prefix);
        border.Child = PreviewText(text, height, brush ?? Brushes.White);
        AddToCanvas(border, prefix);
    }

    private void AddBackgroundImage()
    {
        if (!_theme.ShowBackgroundImage || _themeDirectory is null ||
            string.IsNullOrWhiteSpace(_theme.BackgroundImage))
            return;

        string path = Path.GetFullPath(Path.Combine(_themeDirectory,
            _theme.BackgroundImage.Replace('/', Path.DirectorySeparatorChar)));
        if (!File.Exists(path)) return;

        try
        {
            BitmapImage bitmap = new();
            bitmap.BeginInit();
            bitmap.CacheOption = BitmapCacheOption.OnLoad;
            bitmap.UriSource = new Uri(path, UriKind.Absolute);
            bitmap.EndInit();
            bitmap.Freeze();

            Image image = new()
            {
                Source = bitmap,
                Width = Math.Max(1, _theme.ScoreboardWidth),
                Height = Math.Max(1, _theme.ScoreboardHeight),
                Stretch = Stretch.Fill,
                Opacity = _theme.BackgroundImageAlpha / 255.0,
                IsHitTestVisible = false
            };
            Canvas.SetLeft(image, 0);
            Canvas.SetTop(image, 0);
            Panel.SetZIndex(image, int.MinValue);
            PreviewCanvas.Children.Add(image);
        }
        catch { }
    }

    private void BrowseBackgroundImage_Click(object sender, RoutedEventArgs e)
    {
        OpenFileDialog dialog = new()
        {
            Title = "Select scoreboard background image",
            Filter = "Image files|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files|*.*"
        };
        if (_themeDirectory is not null)
            dialog.InitialDirectory = _themeDirectory;
        if (dialog.ShowDialog(this) != true) return;

        string selectedPath = _themeDirectory is null
            ? dialog.FileName
            : Path.GetRelativePath(_themeDirectory, dialog.FileName)
                .Replace(Path.DirectorySeparatorChar, '/');
        BackgroundImageBox.Text = selectedPath;
        ShowBackgroundImageCheck.IsChecked = true;

        OverlayElement? image = Layer("backgroundImage");
        if (image is not null)
        {
            image.Image = selectedPath;
            image.Visible = true;
            image.Opacity = Math.Clamp(
                Int(BackgroundImageAlphaBox, image.Opacity), 0, 255);
        }
        _theme.BackgroundImage = selectedPath;
        _theme.ShowBackgroundImage = true;
        RebuildPreview();
    }

    private void AddIndicator(string prefix, string mode, int value,
        string label, double textHeight)
    {
        if (mode == "none") return;
        string text = mode switch
        {
            "text" => $"{label} {value}",
            "dots" => new string('●', Math.Max(0, value)),
            "bars" => new string('▮', Math.Max(0, value)),
            "images" => new string('◆', Math.Max(0, value)),
            _ => value.ToString()
        };
        bool textMode = mode is "number" or "text";
        AddText(prefix, text,
            textMode ? textHeight : Get(prefix, "Height"));
    }

    private FontDefinition? FontPreset(string? fontId) =>
        !string.IsNullOrWhiteSpace(fontId) &&
        _font.Fonts.TryGetValue(fontId, out FontDefinition? preset)
            ? preset : null;

    private FontFamily GetPreviewFontFamily(string? fontId = null)
    {
        try
        {
            FontDefinition? preset = FontPreset(fontId);
            string fontFile = preset?.FontFile ?? _font.FontFile;
            string fontFace = preset?.FontFace ?? _font.FontFace;
            if (_themeDirectory is not null &&
                !string.IsNullOrWhiteSpace(fontFile))
            {
                string fontPath = Path.GetFullPath(Path.Combine(_themeDirectory,
                    fontFile.Replace('/', Path.DirectorySeparatorChar)));
                if (File.Exists(fontPath))
                {
                    string directory = Path.GetDirectoryName(fontPath)! +
                        Path.DirectorySeparatorChar;
                    return new FontFamily(new Uri(directory, UriKind.Absolute),
                        $"./#{fontFace.Trim()}");
                }
            }
        }
        catch { }
        try { return new FontFamily(FontPreset(fontId)?.FontFace ?? _font.FontFace); }
        catch { return new FontFamily("Arial"); }
    }

    private TextBlock PreviewText(string text, double height, Brush brush,
        string? fontId = null)
    {
        FontDefinition? preset = FontPreset(fontId);
        FontFamily family = GetPreviewFontFamily(fontId);
        FontWeight weight = FontWeight.FromOpenTypeWeight(
            Math.Clamp(preset?.FontWeight ?? _font.FontWeight, 1, 999));

        // PopupFont.cpp renders a GDI atlas relative to tmHeight + four
        // padding pixels. WPF FontSize is an em size, so using `height`
        // directly makes the editor preview noticeably larger than the
        // in-game glyphs. Convert the requested atlas height through the
        // typeface's line-height metric to preview the same visible size.
        double previewFontSize = height;
        try
        {
            Typeface typeface = new(family, FontStyles.Normal, weight,
                FontStretches.Normal);
            if (typeface.TryGetGlyphTypeface(out GlyphTypeface glyph))
            {
                double sourceHeight = Math.Max(1,
                    preset?.FontSourceHeight ?? _font.FontSourceHeight);
                double atlasLineRatio = glyph.Height + 4.0 / sourceHeight;
                if (atlasLineRatio > 0)
                    previewFontSize = height / atlasLineRatio;
            }
        }
        catch { }

        TextBlock block = new()
        {
            Text = text,
            Foreground = brush,
            FontFamily = family,
            FontSize = Math.Max(5, previewFontSize),
            FontWeight = weight,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            TextAlignment = TextAlignment.Center,
            IsHitTestVisible = false
        };
        return block;
    }

    private Border CreateBorder(string prefix)
    {
        Border border = new()
        {
            Tag = prefix,
            BorderBrush = Brushes.Transparent,
            BorderThickness = new Thickness(1),
            Cursor = Cursors.SizeAll
        };
        border.MouseLeftButtonDown += Element_MouseLeftButtonDown;
        return border;
    }

    private void AddToCanvas(FrameworkElement element, string prefix)
    {
        element.Width = Math.Max(1, Get(prefix, "Width"));
        element.Height = Math.Max(1, Get(prefix, "Height"));
        Canvas.SetLeft(element, Get(prefix, "X"));
        Canvas.SetTop(element, Get(prefix, "Y"));
        PreviewCanvas.Children.Add(element);
    }

    private void Element_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        FrameworkElement element = (FrameworkElement)sender;
        if (element.Tag is not string prefix) return;
        SelectElement(element, prefix);
        if (Layer(prefix)?.Locked == true) { e.Handled = true; return; }
        _dragging = true;
        _dragStart = e.GetPosition(PreviewCanvas);
        _elementStartX = Canvas.GetLeft(_selected!);
        _elementStartY = Canvas.GetTop(_selected!);
        PreviewCanvas.CaptureMouse();
        e.Handled = true;
    }

    private void PreviewCanvas_MouseLeftButtonDown(
        object sender, MouseButtonEventArgs e)
    {
        if (!ReferenceEquals(e.OriginalSource, PreviewCanvas)) return;

        _draggingScoreboard = true;
        _scoreboardDragStart = e.GetPosition(PreviewStage);
        _scoreboardStartOffsetX = _theme.OffsetX;
        _scoreboardStartOffsetY = _theme.OffsetY;
        PreviewCanvas.CaptureMouse();
        e.Handled = true;
    }

    private void Canvas_MouseMove(object sender, MouseEventArgs e)
    {
        if (_draggingScoreboard && e.LeftButton == MouseButtonState.Pressed)
        {
            Point current = e.GetPosition(PreviewStage);
            double scale = 1.0;
            if (_theme.ScaleMode == "uniform")
                scale = Math.Min(
                    PreviewStage.Width / Math.Max(1, _theme.ReferenceWidth),
                    PreviewStage.Height / Math.Max(1, _theme.ReferenceHeight));
            double offsetScale = _theme.ScaleMode == "uniform"
                ? Math.Max(scale, 0.0001) : 1.0;
            _theme.OffsetX = _scoreboardStartOffsetX +
                (current.X - _scoreboardDragStart.X) / offsetScale;
            _theme.OffsetY = _scoreboardStartOffsetY +
                (current.Y - _scoreboardDragStart.Y) / offsetScale;
            ScoreboardOffsetXBox.Text = _theme.OffsetX.ToString("0.##");
            ScoreboardOffsetYBox.Text = _theme.OffsetY.ToString("0.##");
            UpdatePreviewStage();
            return;
        }

        if (!_dragging || _selected is null || _selectedPrefix is null ||
            e.LeftButton != MouseButtonState.Pressed) return;

        Point elementPosition = e.GetPosition(PreviewCanvas);
        double x = Math.Round(
            _elementStartX + elementPosition.X - _dragStart.X, 1);
        double y = Math.Round(
            _elementStartY + elementPosition.Y - _dragStart.Y, 1);
        Canvas.SetLeft(_selected, x);
        Canvas.SetTop(_selected, y);
        Set(_selectedPrefix, "X", x);
        Set(_selectedPrefix, "Y", y);
        ShowElementValues();
    }

    private void Canvas_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (_draggingScoreboard)
        {
            _draggingScoreboard = false;
            PreviewCanvas.ReleaseMouseCapture();
            return;
        }
        _dragging = false;
        PreviewCanvas.ReleaseMouseCapture();
    }

    private void ElementSelector_SelectionChanged(
        object sender, SelectionChangedEventArgs e)
    {
        if (ElementSelector.SelectedItem is not string prefix) return;
        SelectElement(FindElement(prefix), prefix, false);
    }

    private FrameworkElement? FindElement(string prefix) =>
        PreviewCanvas.Children.OfType<FrameworkElement>()
            .FirstOrDefault(element => element.Tag as string == prefix);

    private void SelectElement(FrameworkElement? element, string prefix,
        bool updateSelector = true)
    {
        if (_selected is Border old) old.BorderBrush = Brushes.Transparent;
        _selected = element;
        _selectedPrefix = prefix;
        if (_selected is Border selected) selected.BorderBrush = Brushes.Lime;
        if (updateSelector && ElementSelector.SelectedItem as string != prefix)
            ElementSelector.SelectedItem = prefix;
        ShowElementValues();
    }

    private void RestoreElementSelection()
    {
        if (_selectedPrefix is null) return;
        _selected = FindElement(_selectedPrefix);
        if (_selected is Border selected)
            selected.BorderBrush = Brushes.Lime;
        ShowElementValues();
    }

    private void ShowElementValues()
    {
        if (_selectedPrefix is not string prefix) return;
        SelectedElementLabel.Text = _selected is null
            ? $"{prefix} (currently hidden)"
            : prefix;
        ElementXBox.Text = Get(prefix, "X").ToString("0.##");
        ElementYBox.Text = Get(prefix, "Y").ToString("0.##");
        ElementWidthBox.Text = Get(prefix, "Width").ToString("0.##");
        ElementHeightBox.Text = Get(prefix, "Height").ToString("0.##");
        OverlayElement? layer = Layer(prefix);
        if (layer is null) return;
        ElementZBox.Text = layer.Z.ToString();
        ElementTypeCombo.SelectedItem = layer.Type;
        ElementBindingBox.Text = layer.Binding;
        ElementTextBox.Text = layer.Text;
        ElementFontCombo.Text = layer.Font;
        ElementImageBox.Text = layer.Image;
        ElementImageFitCombo.SelectedItem = layer.ImageFit;
        ElementOpacityBox.Text = layer.Opacity.ToString();
        ElementVisibleCheck.IsChecked = layer.Visible;
        ElementLockedCheck.IsChecked = layer.Locked;
        ElementAlignmentCombo.SelectedItem = layer.Alignment;
        ElementOverflowCombo.SelectedItem = layer.Overflow;
        ElementFontHeightBox.Text = layer.FontHeight.ToString("0.##");
        ElementTextColorBox.Text = layer.TextColor.ToString();
        ElementFillTypeCombo.SelectedItem = layer.Fill.Type;
        ElementFillBindingBox.Text = layer.Fill.Binding;
        ElementFillColorBox.Text = layer.Fill.Color.ToString();
        GradientStartBindingBox.Text = layer.Fill.StartBinding;
        GradientStartColorBox.Text = layer.Fill.StartColor.ToString();
        GradientEndBindingBox.Text = layer.Fill.EndBinding;
        GradientEndColorBox.Text = layer.Fill.EndColor.ToString();
        GradientDirectionCombo.SelectedItem = layer.Fill.Direction;
    }

    private void ApplyElement_Click(object sender, RoutedEventArgs e)
    {
        if (_selectedPrefix is not string prefix) return;
        Set(prefix, "X", Double(ElementXBox, Get(prefix, "X")));
        Set(prefix, "Y", Double(ElementYBox, Get(prefix, "Y")));
        Set(prefix, "Width", Double(ElementWidthBox, Get(prefix, "Width")));
        Set(prefix, "Height", Double(ElementHeightBox, Get(prefix, "Height")));
        OverlayElement? layer = Layer(prefix);
        if (layer is not null)
        {
            layer.Z = Int(ElementZBox, layer.Z);
            layer.Type = ElementTypeCombo.SelectedItem as string ?? layer.Type;
            layer.Binding = ElementBindingBox.Text.Trim();
            layer.Text = ElementTextBox.Text;
            layer.Font = ElementFontCombo.Text.Trim();
            layer.Image = ElementImageBox.Text.Trim();
            layer.ImageFit = ElementImageFitCombo.SelectedItem as string ?? "contain";
            layer.Opacity = Math.Clamp(Int(ElementOpacityBox, layer.Opacity), 0, 255);
            layer.Visible = ElementVisibleCheck.IsChecked == true;
            layer.Locked = ElementLockedCheck.IsChecked == true;
            layer.Alignment = ElementAlignmentCombo.SelectedItem as string ?? "center";
            layer.Overflow = ElementOverflowCombo.SelectedItem as string ?? "overflow";
            layer.FontHeight = Double(ElementFontHeightBox, layer.FontHeight);
            layer.TextColor = Int(ElementTextColorBox, layer.TextColor);
            layer.Fill.Type = ElementFillTypeCombo.SelectedItem as string ?? "solid";
            layer.Fill.Binding = ElementFillBindingBox.Text.Trim();
            layer.Fill.Color = Int(ElementFillColorBox, layer.Fill.Color);
            layer.Fill.StartBinding = GradientStartBindingBox.Text.Trim();
            layer.Fill.StartColor = Int(GradientStartColorBox, layer.Fill.StartColor);
            layer.Fill.EndBinding = GradientEndBindingBox.Text.Trim();
            layer.Fill.EndColor = Int(GradientEndColorBox, layer.Fill.EndColor);
            layer.Fill.Direction = GradientDirectionCombo.SelectedItem as string ?? "vertical";
        }
        RefreshLayerLists();
        RebuildPreview();
    }

    private OverlayElement? Layer(string id) =>
        _theme.Elements.FirstOrDefault(x => x.Id == id);

    private double Get(string prefix, string suffix)
    {
        OverlayElement? layer = Layer(prefix);
        if (layer is not null) return suffix switch
        {
            "X" => layer.X, "Y" => layer.Y, "Width" => layer.Width,
            "Height" => layer.Height, _ => 0
        };
        return (double)(_theme.GetType().GetProperty(
            ToProperty(prefix) + suffix)!.GetValue(_theme) ?? 0d);
    }

    private void Set(string prefix, string suffix, double value)
    {
        OverlayElement? layer = Layer(prefix);
        if (layer is not null)
        {
            if (suffix == "X") layer.X = value;
            else if (suffix == "Y") layer.Y = value;
            else if (suffix == "Width") layer.Width = value;
            else if (suffix == "Height") layer.Height = value;
            return;
        }
        _theme.GetType().GetProperty(ToProperty(prefix) + suffix)!.SetValue(_theme, value);
    }

    private void LayersList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LayersList.SelectedItem is OverlayElement layer)
            SelectElement(FindElement(layer.Id), layer.Id);
    }

    private void ChangeLayerZ(int delta)
    {
        OverlayElement? layer = _selectedPrefix is null ? null : Layer(_selectedPrefix);
        if (layer is null) return;
        layer.Z += delta;
        RefreshLayerLists(); RebuildPreview();
    }
    private void LayerUp_Click(object sender, RoutedEventArgs e) => ChangeLayerZ(1);
    private void LayerDown_Click(object sender, RoutedEventArgs e) => ChangeLayerZ(-1);

    private string UniqueLayerId(string seed)
    {
        int number = 1; string id = seed;
        while (_theme.Elements.Any(x => x.Id == id)) id = seed + number++;
        return id;
    }

    private void AddLayer(string type)
    {
        string id = UniqueLayerId(type);
        OverlayElement layer = new() { Id=id, Type=type, X=20, Y=20,
            Width=120, Height=40, Z=_theme.Elements.Count == 0 ? 0 :
                _theme.Elements.Max(x => x.Z) + 1 };
        if (type == "text") layer.Text = "TEXT";
        if (type == "image")
        {
            layer.Image = "images/image.png";
            layer.ImageFit = "contain";
        }
        _theme.Elements.Add(layer); _selectedPrefix = id;
        RefreshLayerLists(); RebuildPreview();
    }
    private void AddRectangleLayer_Click(object sender, RoutedEventArgs e) => AddLayer("rectangle");
    private void AddImageLayer_Click(object sender, RoutedEventArgs e) => AddLayer("image");
    private void AddTextLayer_Click(object sender, RoutedEventArgs e) => AddLayer("text");

    private void DuplicateLayer_Click(object sender, RoutedEventArgs e)
    {
        OverlayElement? source = _selectedPrefix is null ? null : Layer(_selectedPrefix);
        if (source is null) return;
        OverlayElement copy = JsonSerializer.Deserialize<OverlayElement>(
            JsonSerializer.Serialize(source, JsonOptions), JsonOptions)!;
        copy.Id = UniqueLayerId(source.Id + "Copy"); copy.X += 8; copy.Y += 8;
        copy.Z++; _theme.Elements.Add(copy); _selectedPrefix = copy.Id;
        RefreshLayerLists(); RebuildPreview();
    }

    private void DeleteLayer_Click(object sender, RoutedEventArgs e)
    {
        OverlayElement? layer = _selectedPrefix is null ? null : Layer(_selectedPrefix);
        if (layer is null) return;
        _theme.Elements.Remove(layer); _selectedPrefix = null; _selected = null;
        RefreshLayerLists(); RebuildPreview();
    }

    private static string ToProperty(string prefix) =>
        char.ToUpperInvariant(prefix[0]) + prefix[1..];

    private string FormatTeam(TeamDefinition team) => _theme.TeamNameFormat switch
    {
        "city" => team.CityName,
        "nickname" => team.TeamName,
        "fullName" => $"{team.CityName} {team.TeamName}",
        "shortCode" => team.ShortCode,
        _ => team.Abbreviation
    };

    private string FormatPeriod()
    {
        int quarter = Math.Max(1, Int(QuarterBox, 1));
        if (quarter > 4) return quarter == 5 ? "OT" : $"{quarter - 4}OT";
        string ordinal = quarter switch { 1 => "1st", 2 => "2nd", 3 => "3rd", _ => "4th" };
        return _theme.PeriodFormat switch
        {
            "number" => quarter.ToString(), "ordinalQuarter" => $"{ordinal} Qtr",
            "shortQuarter" => $"Q{quarter}", "longQuarter" => $"{ordinal} Quarter", _ => ordinal
        };
    }

    private bool ShouldShowShotClock()
    {
        if (_theme.ShotClockVisibility == "never") return false;
        if (_theme.ShotClockVisibility != "underThreshold") return true;
        return Int(ShotBox, 24) <= _theme.ShotClockThreshold;
    }

    private static SolidColorBrush PackedBrush(int packed, int alpha = 255) => new(
        Color.FromArgb((byte)Math.Clamp(alpha, 0, 255),
                       (byte)((packed >> 16) & 255),
                       (byte)((packed >> 8) & 255),
                       (byte)(packed & 255)));

    private void PreviewChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loadingControls && IsLoaded) RebuildPreview();
    }

    private void PreviewTextChanged(object sender, TextChangedEventArgs e)
    {
        if (!_loadingControls && IsLoaded) RebuildPreview();
    }

    private static int Int(TextBox box, int fallback) =>
        int.TryParse(box.Text, out int value) ? value : fallback;
    private static double Double(TextBox box, double fallback) =>
        double.TryParse(box.Text, out double value) ? value : fallback;
}
