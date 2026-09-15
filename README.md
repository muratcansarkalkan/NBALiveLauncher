# NBA Live 2005-08 Launcher

NBA Live Launcher is a single ASI plugin for **NBA Live 2005, NBA Live 06, NBA Live 07, and NBA Live 08**. It combines the previous Resolution/Widescreen and Windowed Mode plugins with custom broadcast overlays and additional game improvements.

## Features

- Modern resolution and widescreen support
- Custom frontend/menu resolution
- Windowed mode and intro video control
- Custom Direct3D 9 scoreboards and broadcast overlays
- NBA Live Overlay Theme Editor
- Custom startup home and away teams
- Additional `faces\` file-search path for all four games
- Per-stadium Dorna ad count and timing configuration
- Increased temporary-list capacity for NBA Live 2005

## Installation

The release package contains three directories:

```text
all
2005
06
```

### 1. Install the NBA Live ASI Loader

NBA Live Launcher requires the **NBA Live ASI Loader**.

Install the ASI Loader into the main directory of the NBA Live game you want to use before installing NBA Live Launcher.

Follow the installation instructions provided with the ASI Loader.

### 2. Remove Previous Plugins

If you previously used the standalone Windowed Mode or Resolution plugins, remove these files:

```text
NBAWindowedMode.asi
NBALiveResolution.asi
```

Their functionality is already included in NBA Live Launcher.

### 3. Install the Common Files

Open the `all` directory and copy **all of its contents** into the main directory of the NBA Live game you want to use.

The launcher supports:

- NBA Live 2005
- NBA Live 06
- NBA Live 07
- NBA Live 08

### 4. Install Game-Specific Files

If you are playing **NBA Live 2005**, copy the contents of the `2005` directory into your NBA Live 2005 directory and overwrite files when prompted.

If you are playing **NBA Live 06**, copy the contents of the `06` directory into your NBA Live 06 directory and overwrite files when prompted.

NBA Live 07 and NBA Live 08 only require the contents of the `all` directory.

A compatible supported executable is required.

## Display Configuration

Display settings are configured through `main.ini`.

Example:

```ini
[DISPLAY]
RES_X=1920
RES_Y=1080
WINDOWED=1

[BOOTUP]
INTRO=1
```

`RES_X` and `RES_Y` set the startup/frontend resolution. You can still choose your preferred gameplay resolution from the normal in-game resolution options.

- `WINDOWED=1` enables windowed mode.
- `WINDOWED=0` uses fullscreen mode.
- `INTRO=1` enables intro videos.
- `INTRO=0` disables intro videos.

## Custom Scoreboards and Broadcast Overlays

Custom popup packages are stored under:

```text
popups\<package name>\
```

Custom overlays are configured in `main.ini`:

```ini
[OVERLAY]
CUSTOM_OVERLAY=1
CUSTOM_OVERLAY_NAME=TEST
```

`CUSTOM_OVERLAY=1` enables custom overlays. Set it to `0` to use the game's original scoreboard and overlays.

`CUSTOM_OVERLAY_NAME` selects the popup package to load.

For example:

```ini
CUSTOM_OVERLAY_NAME=TNT07
```

loads:

```text
popups\TNT07\
```

Restart the game after changing `CUSTOM_OVERLAY` or `CUSTOM_OVERLAY_NAME`.

The custom overlay system supports scoreboards, violations, play calls, player-foul graphics, player and team statistics, and pregame matchup graphics. Unsupported or unconfigured overlays fall back to the game's original graphics.

Users can create their own scoreboards and popups using the included **NBA Live Overlay Theme Editor**.

See [CustomScoreboards.md](CustomScoreboards.md) for information about creating custom popup packages, layouts, bindings, fonts, images, animations, supported overlays, and the Theme Editor.

Press **F5** in game to reload the current popup package while editing.

## Startup Teams

The initial matchup can be overridden through `main.ini`:

```ini
[STARTUP]
HOME_TEAMNUM=-1
AWAY_TEAMNUM=-1
```

Set either value to a valid database `TEAMNUM`.

A value of `-1` leaves that team's original startup selection unchanged.

## Custom Faces Folder

NBA Live Launcher adds:

```text
faces\
```

to the game's file-search paths in NBA Live 2005, NBA Live 06, NBA Live 07, and NBA Live 08.

This allows supported files to be loaded from a dedicated `faces` directory.

## Stadium Dorna Configuration

Dorna behavior can be customized for individual stadiums using JSON files stored in:

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
- `period` controls the complete Dorna cycle period.
- `transition_period` controls transition timing.

If a stadium configuration or individual setting is missing, the game uses its original value.

## Sample Stadium

The release package includes the **Boston Celtics stadium from NBA Live 08** as a sample.

It is included as an example of the stadium-related functionality available with NBA Live Launcher.

## NBA Live 2005 Memory Improvement

NBA Live 2005 includes an additional memory-related patch that increases a known temporary-list capacity from **118 to 512 entries**.

The experimental NBA Live 06 memory allocator expansion is currently disabled.

## Supported Games

- NBA Live 2005
- NBA Live 06
- NBA Live 07
- NBA Live 08

## Building

NBA Live Launcher is a **32-bit ASI plugin**.

The project uses the Visual Studio `v141_xp` toolset and the FIFAM/NBA Live ASI development headers.

Open `NBALiveLauncher.sln`, configure the required local include paths, and build the Win32 configuration.

The NBA Live Overlay Theme Editor is a separate .NET application.

## Credits

- Dmitri — coding assistance and FIFAM ASI Loader development
- wiscard_rush — UI components
- JuicyShaqMeat — UI components
- iceman — widescreen intro videos for NBA Live 2005 and NBA Live 06

## License

See `LICENSE`.