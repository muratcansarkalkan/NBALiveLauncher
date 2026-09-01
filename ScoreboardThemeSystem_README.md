# NBA Live JSON Scoreboard Theme System

This package contains the updated NBA Live 2005–08 D3D9 scoreboard source,
the first `TEST` JSON theme, and the standalone WPF theme editor.

## Additions to the ASI project

Add these new files to the existing Visual Studio C++ project:

- `ScoreboardConfig.cpp`
- `ScoreboardConfig.h`
- `PopupTheme.cpp` / `.h`
- `PopupFont.cpp` / `.h`
- `ScoreboardRenderer.cpp` / `.h`
- `ShotClock.cpp` / `.h`
- `ShotClockGames.cpp` / `.h`

The runtime reads:

```text
popups\TEST\teams.json
popups\TEST\popup.json
popups\TEST\scoreboard.json
```

`F5` still reloads the theme. The editor's **Save + Reload in Game** button
updates `popups\TEST\.reload`, which the plugin detects within 500 ms.

## Implemented behavior

- Always/on-score/late-game/hybrid scoreboard visibility
- Configurable basket display duration and late-game threshold
- Always/threshold/disabled shot clock
- Normal and urgent shot-clock colors
- Abbreviation/city/nickname/full-name/short-code team text
- Numeric, ordinal, short, and long period formats with overtime handling
- Fouls and timeouts as numbers, text, dots, bars, or images
- Remaining/used timeout interpretation
- BONUS and optional double-BONUS thresholds
- Uniform responsive scaling or fixed-size positioning
- Drag-editable positions and dimensions for known scoreboard elements

## Editor build

### Build both projects

Place `build_all.py` and `build_all.bat` in the root of the existing ASI
repository, beside its single `.sln` or `.vcxproj`. Double-click
`build_all.bat` to build both the Release ASI plugin and the WPF editor. The
solution platform (`Win32`, `x86`, etc.) is detected from the project itself.

The script finds Visual Studio 2022 through `vswhere`, uses MSBuild for the
C++ project, and uses the .NET 8 SDK for the editor. For repositories with
multiple C++ solutions, specify the intended target manually:

```powershell
py build_all.py --cpp NBALiveLauncher.sln
```

Use `--clean`, `--configuration Debug`, or `--platform x64` when needed.

Open `ScoreboardThemeEditor\ScoreboardThemeEditor.csproj` in Visual Studio 2022
with the .NET 8 SDK and build Release, or run:

```powershell
dotnet build ScoreboardThemeEditor\ScoreboardThemeEditor.csproj -c Release
```

The editor is source-only in this package because the generation environment
does not include the Windows .NET SDK. JSON and XAML files were validated.
