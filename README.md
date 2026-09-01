# NBA Live 2005-08 Launcher

NBA Live 2005-08 Launcher combines the **Resolution & Widescreen
Update**, **Windowed Mode**, and the new **native in-game scoreboard**
into a single ASI plugin for NBA Live 2005, NBA Live 06, NBA Live 07,
and NBA Live 08.

The launcher is intended to provide a single base plugin for modern
display support and additional game enhancements instead of requiring
separate ASI plugins.

## Features

### Resolution and Widescreen

-   Adds widescreen and modern resolution support without requiring an
    external resolution application.
-   Adjusts the game's aspect ratio according to the selected width and
    height instead of using a fixed aspect ratio.
-   Allows a custom frontend/menu resolution through `main.ini`.
-   Keeps the normal in-game resolution selector available under
    **Options -\> Detail Settings**.
-   Adjusts widescreen UI rendering and mouse input where supported.
-   Includes widescreen UI components for NBA Live 2005 and NBA Live 06.
-   Adjusts loading bars, player creation/edit areas, movies, and other
    display elements for widescreen resolutions.
-   Allows intro videos to be enabled or disabled.

### Windowed Mode

-   Allows NBA Live 2005-08 to run in windowed mode without an external
    windowed-mode application.
-   Windowed mode can be enabled or disabled from the same `main.ini`
    used by the resolution plugin.

### Native Scoreboard

-   Adds a Direct3D 9 in-game scoreboard rendered directly by the
    plugin.
-   Reads live game data from the game rather than using manually
    synchronized values.
-   Displays the current game clock and shot clock.
-   Displays home and away scores.
-   Reads additional game state including team fouls and timeouts.
-   Tracks the game's overlay events so the scoreboard can appear and
    disappear with gameplay rather than remaining permanently on screen.
-   Supports NBA Live 2005, NBA Live 06, NBA Live 07, and NBA Live 08.
-   Uses a deferred Direct3D 9 hook fallback to avoid performing unsafe
    D3D initialization while the ASI is being loaded.

The scoreboard is currently part of the launcher itself and does not
require a separate configuration entry.

## Supported Games

The plugin targets the specific supported game executables used by the
original projects. The only games that are supported are NBA Live 2005-08.
A compatible **NO-CD fixed executable** is required.
The original CD releases are also problematic on modern versions of
Windows.

## Installation

1.  Install the NBA Live ASI Loader and follow its installation
    instructions.
2.  Make sure the required ASI Loader files, including `d3d9.dll`,
    `oledlg.dll`, and the `plugins` folder, are installed in the game's
    root directory.
3.  Copy the launcher release contents to the game's root directory.
    This includes the launcher ASI and, when supplied by the release,
    the `assets`, `movies`, `plugins`, and `main.ini` files/folders.
4.  Remove older standalone copies of the plugins to avoid loading the
    same patches twice. In particular, remove old `NBALiveResolution`,
    `NBAWindowedMode`, or equivalent debug ASI files from the `plugins`
    folder when using the combined launcher.
5.  Configure `main.ini`.

## Configuration

The launcher uses one `main.ini` file in the game's root directory.

Example:

``` ini
[DISPLAY]
RES_X=1920
RES_Y=1080
WINDOWED=1

[BOOTUP]
INTRO=1
```

### `RES_X` and `RES_Y`

`RES_X` and `RES_Y` control the resolution used when the game starts and
for the frontend/menu.

This is important because the original games can otherwise use a 640x480
frontend, which is undesirable on modern displays.

These values do **not** remove the normal in-game resolution selection.
Once in the game, you can still navigate to:

**Options -\> Detail Settings**

and select one of the available resolution entries.

Example:

``` ini
RES_X=1920
RES_Y=1080
```

### `WINDOWED`

Set:

``` ini
WINDOWED=1
```

to enable windowed mode.

Set:

``` ini
WINDOWED=0
```

to use fullscreen mode.

### `INTRO`

Set:

``` ini
INTRO=1
```

to enable intro videos.

Set:

``` ini
INTRO=0
```

to disable them.

## Resolution Reference

NBA Live 2005, 06, and 07 expose 10 resolution entries. NBA Live 08
exposes 16 entries. The launcher replaces the original entries with the
following resolutions.

| Resolution    | 2005         | 2006         | 2007         | 2008         |
|---------------|--------------|--------------|--------------|--------------|
| 640x480x16    | 640x480x32   | 640x480x32   | 640x480x32   | 640x480x32   |
| 640x480x32    | 800x600x32   | 800x600x32   | RES_XxRES_Yx32*   | RES_XxRES_Yx32*   |
| 800x600x16    | 1024x768x32  | 1024x768x32  | 1024x768x32  | 1024x768x32  |
| 800x600x32    | 1280x720x32  | 1280x720x32  | 1280x720x32  | 1280x720x32  |
| 1024x768x16   | 1280x1024x32 | 1280x1024x32 | 1280x1024x32 | 1280x1024x32 |
| 1024x768x32   | 1366x768x32  | 1366x768x32  | 1366x768x32  | 1366x768x32  |
| 1280x720x16   | -            | -            | -            | 1440x900x32  |
| 1280x720x32   | -            | -            | -            | 1600x900x32  |
| 1280x1024x16  | 1440x900x32  | 1440x900x32  | 1440x900x32  | 1600x1200x32 |
| 1280x1024x32  | 1600x900x32  | 1600x900x32  | 1600x900x32  | 1680x1050x32 |
| 1440x900x16   | -            | -            | -            | 1920x1080x32 |
| 1440x900x32   | -            | -            | -            | 2560x1440x32 |
| 1600x1200x16  | 1920x1080x32 | 1920x1080x32 | 1920x1080x32 | 3440x1440x32 |
| 1600x1200x32  | 2560x1440x32 | 2560x1440x32 | 2560x1440x32 | 3840x1080x32 |
| 1680x1050x16  | -            | -            | -            | 3840x1200x32 |
| 1680x1050x32  | -            | -            | -            | 3840x1600x32 |

For example, to select **1920x1080** in NBA Live 06, select the original
**1600x1200x16** entry in Detail Settings.

The text displayed by the original Detail Settings interface may still
show the game's original resolution label even though the launcher has
replaced the underlying resolution.

No additional executable hex edit is required for aspect ratio
adjustment.

## Video Notes

### NBA Live 2005 and NBA Live 06

The widescreen implementation currently supports two expected movie
sizes:

-   4:3 videos: **640x480**
-   16:9 videos: **1920x1088**

The launcher selects the appropriate widescreen movie resources
according to the configured aspect ratio.

### NBA Live 07 and NBA Live 08

The games fit video playback to the screen by default, so the same
restrictions do not apply.

## Widescreen UI Files

NBA Live 2005 and NBA Live 06 include additional widescreen-adjusted UI
components. When a widescreen aspect ratio is selected, the launcher can
copy the appropriate supplied UI files into `sgsm` without replacing
files that already exist there.

The corresponding asset directories are:

``` text
assets/05WSUI
assets/06WSUI
```

## Building on Windows

The project is built as a **32-bit ASI plugin**.

Requirements:

-   Visual Studio 2017 or Visual Studio 2022
-   **Desktop development with C++**
-   **Game development with C++**
-   **C++ Windows XP Support for VS 2017 (v141)**
-   Windows 8.1 SDK
-   The development files required by the NBA Live/FIFAM ASI Loader

Open `NBALiveLauncher.sln`, verify the include/library paths for your
local ASI Loader development environment, select the Win32/x86
configuration, and build the project.

The resulting plugin uses the `.asi` extension.

## Project Structure

The launcher keeps the three systems separated internally while using a
single entry point:

``` text
Main.cpp
    -> Resolution / widescreen initialization
    -> Windowed-mode initialization
    -> Scoreboard initialization

GameLive2005.cpp
GameLive06.cpp
GameLive07.cpp
GameLive08.cpp
    Resolution and widescreen patches for each game

AdjustWSUI.cpp
    NBA Live 2005/06 widescreen UI file handling

EnableWindowed.cpp
    Windowed-mode patches

ShotClock.cpp
    Game-state hooks, overlay-event handling, Direct3D 9 hook,
    and native scoreboard rendering
```

Keeping a single launcher entry point prevents the individual components
from depending on cross-file global-constructor initialization order.

## Troubleshooting

### Windowed mode does not activate

Make sure `main.ini` is in the game's root directory and contains:

``` ini
[DISPLAY]
WINDOWED=1
```

Also make sure Windows has not saved the file as `main.ini.txt`.

### The game still shows an old resolution name

The launcher replaces the underlying resolution values. The original
menu text may still display the old resolution label.

### The menu starts at an unwanted resolution

Set `RES_X` and `RES_Y` under `[DISPLAY]`. These values control the
startup/frontend resolution independently of the resolution you later
select for gameplay.

### The plugin does not load

Check that:

-   You are using a supported executable.
-   The ASI Loader is correctly installed.
-   The launcher ASI is inside the correct `plugins` folder.
-   An older standalone Resolution or Windowed Mode ASI is not being
    loaded at the same time.

## Release Notes

### Launcher v0.1

-   Merged NBA Live 2005-08 Resolution & Widescreen Update and NBA Live
    2005-08 Windowed Mode into one launcher project.
-   Added the native Direct3D 9 scoreboard.
-   Added live game clock and shot-clock display.
-   Added live score data and supporting team/game-state reads.
-   Consolidated display configuration into `main.ini`.
-   Preserved frontend/menu custom resolution while retaining the normal
    in-game resolution selector.

### Resolution & Widescreen History

-   **v1.0:** Initial release.
-   **v1.01:** Added widescreen UI and custom resolution support.
-   **v1.02:** Added widescreen-adjusted UI components for NBA Live 2005
    and NBA Live 06 without modifying the original game files.
-   **v1.03:** Fixed loading-bar, player edit-zone, and movie
    scaling/position. Added widescreen videos for NBA Live 2005/06 and
    intro enable/disable functionality. Credits to iceman for
    assistance.
-   **v1.04:** Updated the NBA Live 2005 transition screen and added
    intro-video enable/disable behavior after standby mode at the main
    menu.

## Credits

-   **Dmitri** --- coding assistance and FIFAM ASI Loader development
-   **wiscard_rush** --- UI components
-   **JuicyShaqMeat** --- UI components
-   **iceman** --- widescreen intro videos for NBA Live 2005 and NBA
    Live 06

## License

See `LICENSE` for the project's license.
