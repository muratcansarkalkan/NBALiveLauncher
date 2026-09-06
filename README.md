# NBA Live 2005-08 Launcher

NBA Live Launcher is a single ASI plugin for **NBA Live 2005, NBA Live
06, NBA Live 07, and NBA Live 08**. It combines the previous
Resolution/Widescreen and Windowed Mode plugins with the custom
scoreboard and broadcast overlay system.

## Features

-   Modern resolution and widescreen support
-   Custom frontend/menu resolution
-   Windowed mode
-   Intro video enable/disable option
-   Custom Direct3D 9 scoreboard and broadcast popups
-   Custom violation, play-call, player/team statistics, player-foul,
    and pregame matchup graphics
-   Included Scoreboard Theme Editor for creating and editing popup
    layouts
-   Support for NBA Live 2005-08 from the same plugin

For detailed popup documentation, see
**[CustomScoreboards.md](CustomScoreboards.md)**.

## Installation

1.  Make sure the NBA Live ASI Loader is installed.
2.  **Remove the previous standalone plugins if they are installed:**
    -   `NBAWindowedMode.asi`
    -   `NBALiveResolution.asi`
3.  Place the supplied files directly into the **game directory** of the
    NBA Live 2005-08 game you want to play.
4.  Edit `main.ini` to configure the launcher.

Do not keep the old Windowed Mode or Resolution ASI files installed
alongside NBA Live Launcher. Their functionality is already included.

## Configuration

Example `main.ini`:

``` ini
[DISPLAY]
RES_X=1920
RES_Y=1080
WINDOWED=1

[BOOTUP]
INTRO=1

[OVERLAY]
CUSTOM_OVERLAY=1
CUSTOM_OVERLAY_NAME=TEST
```

### Display

`RES_X` and `RES_Y` set the startup/frontend resolution. You can still
select your preferred gameplay resolution from the normal in-game
resolution options.

Set `WINDOWED=1` to enable windowed mode, or `WINDOWED=0` for
fullscreen.

Set `INTRO=1` to enable intro videos, or `INTRO=0` to disable them.

## Custom Scoreboards and Popups

Custom popup packages are stored inside the `popups` directory.

Enable custom popups with:

``` ini
CUSTOM_OVERLAY=1
```

Disable them and use the game's original overlays with:

``` ini
CUSTOM_OVERLAY=0
```

Choose your popup package by editing:

``` ini
CUSTOM_OVERLAY_NAME=TEST
```

For example, if the package is:

``` text
popups/TNT07/
```

use:

``` ini
CUSTOM_OVERLAY_NAME=TNT07
```

Restart the game after changing `CUSTOM_OVERLAY` or
`CUSTOM_OVERLAY_NAME`.

Users can create new popups of their own by reviewing
**[CustomScoreboards.md](CustomScoreboards.md)**. It documents the
package structure, JSON layouts, supported overlays, bindings, fonts,
images, animations, and the Theme Editor.

Currently supported custom graphics include scoreboards, violations,
play calls, player-foul graphics, player and team statistics, and
pregame matchup intros. Unsupported or unconfigured graphics fall back
to the game's original overlays.

## Popup Editing

The included **Scoreboard Theme Editor** can be used to create and edit
supported popup layouts visually.

Press **F5** in game to reload the current popup package while editing.
Changes to `main.ini` require restarting the game.

See **[CustomScoreboards.md](CustomScoreboards.md)** for full
documentation.

## Supported Games

-   NBA Live 2005
-   NBA Live 06
-   NBA Live 07
-   NBA Live 08

A compatible supported executable and the NBA Live ASI Loader are
required.

## Credits

-   Dmitri --- coding assistance and FIFAM ASI Loader development
-   wiscard_rush --- UI components
-   JuicyShaqMeat --- UI components
-   iceman --- widescreen intro videos for NBA Live 2005 and NBA Live 06

## License

See `LICENSE`.
