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
        ElementSelector.ItemsSource = EditableElements;
        ElementSelector.SelectedItem = "awayPanel";
        LoadControls();
        RebuildPreview();
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
            _theme = JsonSerializer.Deserialize<ScoreboardTheme>(
                File.ReadAllText(dialog.FileName), JsonOptions) ?? new();
            string popupPath = Path.Combine(_themeDirectory, "popup.json");
            _font = File.Exists(popupPath)
                ? JsonSerializer.Deserialize<PopupFontTheme>(
                    File.ReadAllText(popupPath), JsonOptions) ?? new()
                : new();
            LoadTeams();
            LoadControls();
            RebuildPreview();
            StatusText.Text = $"Loaded {_themeDirectory}";
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

    private void Save(bool reload)
    {
        if (_themeDirectory is null)
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
            File.WriteAllText(Path.Combine(_themeDirectory, "scoreboard.json"),
                JsonSerializer.Serialize(_theme, JsonOptions));
            File.WriteAllText(Path.Combine(_themeDirectory, "popup.json"),
                JsonSerializer.Serialize(_font, JsonOptions));
            if (reload)
                File.WriteAllText(Path.Combine(_themeDirectory, ".reload"),
                    DateTime.UtcNow.Ticks.ToString());
            StatusText.Text = reload
                ? "Saved. The running game will reload within 500 ms."
                : "Theme saved.";
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "Unable to save theme",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void LoadControls()
    {
        _loadingControls = true;
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
        _loadingControls = false;
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
        PreviewCanvas.Background = PackedBrush(
            _theme.BackgroundColor, _theme.BackgroundAlpha);
        PreviewCanvas.Children.Clear();
        AddBackgroundImage();
        TeamDefinition away = AwayTeamCombo.SelectedItem as TeamDefinition ??
            _teams.FirstOrDefault() ?? new() { Abbreviation = "AWY", TeamName = "Away", CityName = "Away", PrimaryColor = 3030876, SecondaryColor = 14013909 };
        TeamDefinition home = HomeTeamCombo.SelectedItem as TeamDefinition ??
            _teams.Skip(1).FirstOrDefault() ?? new() { Abbreviation = "HME", TeamName = "Home", CityName = "Home", PrimaryColor = 9969197, SecondaryColor = 16777215 };
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

        BackgroundImageBox.Text = _themeDirectory is null
            ? dialog.FileName
            : Path.GetRelativePath(_themeDirectory, dialog.FileName)
                .Replace(Path.DirectorySeparatorChar, '/');
        ShowBackgroundImageCheck.IsChecked = true;
        ApplyBehaviorFromControls();
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

    private FontFamily GetPreviewFontFamily()
    {
        try
        {
            if (_themeDirectory is not null &&
                !string.IsNullOrWhiteSpace(_font.FontFile))
            {
                string fontPath = Path.GetFullPath(Path.Combine(_themeDirectory,
                    _font.FontFile.Replace('/', Path.DirectorySeparatorChar)));
                if (File.Exists(fontPath))
                {
                    string directory = Path.GetDirectoryName(fontPath)! +
                        Path.DirectorySeparatorChar;
                    return new FontFamily(new Uri(directory, UriKind.Absolute),
                        $"./#{_font.FontFace.Trim()}");
                }
            }
        }
        catch { }
        try { return new FontFamily(_font.FontFace); }
        catch { return new FontFamily("Arial"); }
    }

    private TextBlock PreviewText(string text, double height, Brush brush)
    {
        FontFamily family = GetPreviewFontFamily();
        FontWeight weight = FontWeight.FromOpenTypeWeight(
            Math.Clamp(_font.FontWeight, 1, 999));

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
                double sourceHeight = Math.Max(1, _font.FontSourceHeight);
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
    }

    private void ApplyElement_Click(object sender, RoutedEventArgs e)
    {
        if (_selectedPrefix is not string prefix) return;
        Set(prefix, "X", Double(ElementXBox, Get(prefix, "X")));
        Set(prefix, "Y", Double(ElementYBox, Get(prefix, "Y")));
        Set(prefix, "Width", Double(ElementWidthBox, Get(prefix, "Width")));
        Set(prefix, "Height", Double(ElementHeightBox, Get(prefix, "Height")));
        RebuildPreview();
    }

    private double Get(string prefix, string suffix) =>
        (double)(_theme.GetType().GetProperty(ToProperty(prefix) + suffix)!.GetValue(_theme) ?? 0d);

    private void Set(string prefix, string suffix, double value) =>
        _theme.GetType().GetProperty(ToProperty(prefix) + suffix)!.SetValue(_theme, value);

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
