# NBA Live 2005-08 Launcher

NBA Live Launcher is an all-in-one ASI plugin for **NBA Live 2005, NBA Live 06, NBA Live 07, and NBA Live 08**.

It combines modern display support, windowed mode, custom broadcast overlays, screenshots, anti-aliasing, debugging tools, stadium enhancements, memory improvements, game fixes, and other features into a single plugin.

## Features

- NBA Live 2005, 06, 07 and 08 support
- Modern resolution and widescreen support
- Custom frontend/menu resolution
- Improved menu font resolution
- Windowed mode
- MSAA anti-aliasing
- PNG/TGA screenshots with configurable output directory
- Custom scoreboards and broadcast overlays
- Editable Starting 5, Outro and in-game lineup graphics
- Player/team statistics and foul popups
- Player jersey-number support in stat/foul popups
- Dynamic stadium/jumbotron game data
- CSV box-score export
- Debug console and crash diagnostics
- Custom startup teams
- Additional `faces\` file-search path
- Per-stadium Dorna configuration
- Expanded memory for larger custom assets
- Several game-specific crash and rendering fixes

---

# Installation

The release package contains three directories:

```text
all
2005
06
```

## 1. Install the NBA Live ASI Loader

**NBA Live Launcher requires the NBA Live ASI Loader.**

Install the ASI Loader into the main directory of the NBA Live game you want to use before installing NBA Live Launcher.

Follow the installation instructions included with the ASI Loader.

## 2. Remove Previous Plugins

If you previously used the standalone Windowed Mode or Resolution plugins, remove:

```text
NBAWindowedMode.asi
NBALiveResolution.asi
```

Their functionality is already included in NBA Live Launcher.

## 3. Install the Common Files

Copy **all contents of the `all` directory** into the main directory of the NBA Live game you want to play.

This applies to:

- NBA Live 2005
- NBA Live 06
- NBA Live 07
- NBA Live 08

## 4. Install Game-Specific Files

### NBA Live 2005

After installing `all`, copy the contents of:

```text
2005
```

into the NBA Live 2005 directory and **overwrite files when prompted**.

### NBA Live 06

After installing `all`, copy the contents of:

```text
06
```

into the NBA Live 06 directory and **overwrite files when prompted**.

### NBA Live 07 / NBA Live 08

No additional game-specific package is required. Install the contents of `all`.

## Sample Stadium

The package includes the **Boston Celtics stadium from NBA Live 08** as a sample of the launcher's stadium functionality.

---

# Configuration

NBA Live Launcher is configured through `main.ini` in the game directory.

## Display

```ini
[DISPLAY]
RES_X=1920
RES_Y=1080
WINDOWED=1
ANTI_ALIASING=1
MSAA_SAMPLES=4
```

### Resolution

`RES_X` and `RES_Y` control the startup/frontend resolution.

For example:

```ini
RES_X=1920
RES_Y=1080
```

The normal in-game resolution selector remains available, so the gameplay resolution can still be selected from the game's options.

### Windowed Mode

```ini
WINDOWED=1
```

enables windowed mode.

Use:

```ini
WINDOWED=0
```

for fullscreen.

### Anti-Aliasing

Enable MSAA with:

```ini
ANTI_ALIASING=1
```

and select the requested sample count with:

```ini
MSAA_SAMPLES=4
```

Supported requested values include:

```text
2
4
8
16
```

Actual MSAA availability depends on the graphics hardware and display mode.

---

# Screenshots

NBA Live Launcher replaces the original screenshot system with a Direct3D 9 implementation.

Screenshots can be saved as **PNG** or **TGA**, work with the launcher's anti-aliasing implementation, and can be written to a configurable directory.

Configure the screenshot system in `main.ini`:

```ini
[SCREENSHOT]
FORMAT=png
DIRECTORY=
```

Supported formats:

```ini
FORMAT=png
```

or:

```ini
FORMAT=tga
```

If `DIRECTORY` is empty, screenshots are saved to the appropriate game's Screenshots directory under Documents.

For example:

```text
Documents\NBA LIVE 2005\Screenshots
Documents\NBA LIVE 06\Screenshots
Documents\NBA LIVE 07\Screenshots
Documents\NBA LIVE 08\Screenshots
```

A custom directory can also be specified:

```ini
DIRECTORY=C:\NBA Screenshots
```

Screenshots are automatically numbered to prevent existing screenshots from being overwritten.

---

# Custom Scoreboards and Broadcast Overlays

NBA Live Launcher includes a custom Direct3D 9 broadcast graphics system.

Custom packages are stored under:

```text
popups\<package name>\
```

Enable custom overlays in `main.ini`:

```ini
[OVERLAY]
CUSTOM_OVERLAY=1
CUSTOM_OVERLAY_NAME=TEST
```

`CUSTOM_OVERLAY` controls whether the custom overlay system is enabled:

```ini
CUSTOM_OVERLAY=1
```

enables custom overlays.

```ini
CUSTOM_OVERLAY=0
```

uses the game's original overlays.

`CUSTOM_OVERLAY_NAME` selects the package:

```ini
CUSTOM_OVERLAY_NAME=TNT07
```

loads:

```text
popups\TNT07\
```

Restart the game after changing `CUSTOM_OVERLAY` or `CUSTOM_OVERLAY_NAME`.

Press **F5** while in game to reload the current popup package during development.

## Supported Graphics

The custom overlay system supports graphics including:

- Scoreboard
- Violations
- Play calls
- Player fouls
- Player statistics
- Team statistics
- Starting 5
- Outro graphics
- In-game lineups

NBA Live 07 and NBA Live 08 now support full custom editing of the **Starting 5, Outro and in-game lineup** presentation graphics.

Player-stat and player-foul graphics can also display the featured player's **jersey number**.

## Creating Your Own Scoreboards and Popups

The **NBA Live Scoreboard Theme Editor** provides the visual editing environment for creating and modifying custom broadcast packages.

For complete instructions covering the editor, elements, bindings, layouts, images, fonts, animations and custom popup creation, read the:

[Scoreboard Theme Editor User Guide](https://github.com/muratcansarkalkan/ScoreboardThemeEditor/blob/main/USER_GUIDE.md?utm_source=chatgpt.com)

---

# Box Score Export

Press:

```text
F6
```

during a game to export the current box score as a **CSV file**.

The exported box score contains live player/game statistics gathered from the current game.

---

## Dynamic Stadium Displays

Compatible stadium models can display live game information using dynamic runtime materials.

Supported stadium/jumbotron information includes data such as:

- Home and away scores
- Game clock
- Shot clock
- Period / overtime
- Timeouts
- Team fouls
- Player jersey numbers
- Player points
- Player fouls

This allows compatible stadium models to contain functional scoreboards and statistical displays instead of static textures.

For information about creating and editing compatible stadium models, dynamic materials, jumbotron displays, and other stadium features, see [NBA Live EBO Tools](https://github.com/muratcansarkalkan/EBOTools/?utm_source=chatgpt.com).

---

# Stadium Dorna Configuration

Dorna behavior can be configured individually for stadiums using:

```text
assets\stadia\<stadium-abbreviation>.json
```

Example:

```json
{
  "dorna": {
    "ad_count": 12,
    "period": 8.6,
    "transition_period": 0.6
  }
}
```

- `ad_count` controls the number of Dorna advertisements.
- `period` controls the complete cycle.
- `transition_period` controls transition timing.

Missing settings use the game's original values.

---

# Debugger

NBA Live Launcher includes an optional external debug console intended primarily for mod development and troubleshooting.

Enable it through `main.ini`:

```ini
[DEBUG]
CONSOLE=1
```

Additional debugging options include:

```ini
[DEBUG]
CONSOLE=1
FILES=1
FAILED_FILES=1
FILE_CALLERS=1
CRASHES=1
ASSET_FILES=1
ANIMBANK=0
```

These options can provide information about file activity, failed file loads, asset requests and crashes.

Crash diagnostics can include information such as the exception location, operation, CPU state, stack information and relevant resource context.

For normal gameplay, the debugger can be disabled with:

```ini
CONSOLE=0
```

---

# Startup Teams

The initial matchup can be overridden through `main.ini`:

```ini
[STARTUP]
HOME_TEAMNUM=-1
AWAY_TEAMNUM=-1
```

Set either value to a valid database `TEAMNUM`.

`-1` leaves that team's normal startup selection unchanged.

---

# Custom Faces Folder

NBA Live Launcher adds:

```text
faces\
```

to the file-search paths of NBA Live 2005, 06, 07 and 08.

This provides a dedicated location for supported custom face files.

---

# Memory Improvements

NBA Live Launcher increases available in-game memory/resources to improve support for substantially larger custom assets, including more detailed stadiums.

NBA Live 2005 also increases a known temporary resource-list capacity from:

```text
118
```

to:

```text
512
```

entries.

---

# Additional Fixes and Improvements

The launcher also includes several fixes for the original games:

### NBA Live 2005

- Fixed the **Lounge not displaying** correctly.
- Increased resource capacity for larger custom assets.

### NBA Live 07

- Fixed player clipping issues.
- Fixed a longstanding crash that could occur when quitting an active game and returning to the main menu.

### NBA Live 08

- Fixed player clipping issues.

### Frontend

- Improved **menu/frontend font resolution** for modern display resolutions.
- This change applies to frontend fonts and does not replace the in-game font rendering system.

---

# Changelog

## Latest Update

### Added

- MSAA anti-aliasing.
- New Direct3D 9 screenshot system.
- PNG screenshot support.
- Configurable screenshot output directory.
- Dynamic game data for stadium/jumbotron models.
- Debug console and crash diagnostics.
- F6 CSV box-score export.
- Player jersey-number binding for stat and foul popups.
- Player-number support in the Scoreboard Theme Editor.
- Full custom Starting 5 graphics for NBA Live 07 and 08.
- Full custom Outro graphics for NBA Live 07 and 08.
- Full custom in-game lineup graphics for NBA Live 07 and 08.
- Increased game memory for larger custom stadiums and other assets.
- Higher-resolution frontend/menu font rendering.

### Fixed

- Fixed the Lounge not displaying correctly in NBA Live 2005.
- Fixed player clipping in NBA Live 07 and NBA Live 08.
- Fixed an NBA Live 07 crash when quitting an in-progress game and returning to the main menu.

### Improved

- Screenshot capture now works with the launcher's MSAA implementation.
- Screenshot filenames are automatically numbered.
- Stat and foul popup data has been expanded.
- Stadium models can now function as live arena displays instead of relying entirely on static textures.
- Debugging information for custom assets and game crashes has been expanded.

---

# Supported Games

- NBA Live 2005
- NBA Live 06
- NBA Live 07
- NBA Live 08

A compatible game executable and the NBA Live ASI Loader are required.

---

# Building

NBA Live Launcher is a **32-bit ASI plugin**.

The project uses the Visual Studio `v141_xp` toolset and the FIFAM/NBA Live ASI development headers.

Open:

```text
NBALiveLauncher.sln
```

configure the required local include paths and build the Win32 configuration.

The **NBA Live Scoreboard Theme Editor** is maintained as a separate application.

---

# Credits

- Dmitri — coding assistance and FIFAM ASI Loader development
- wiscard_rush — UI components
- JuicyShaqMeat — UI components
- iceman — widescreen intro videos for NBA Live 2005 and NBA Live 06

# License

See `LICENSE`.