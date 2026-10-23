# NBA Live 2005-08 Launcher

NBA Live Launcher is an all-in-one ASI plugin for **NBA Live 2005, NBA
Live 06, NBA Live 07, and NBA Live 08**.

It combines modern resolution and widescreen support, windowed mode,
enhanced anti-aliasing, custom broadcast graphics, a stadium server,
dynamic stadium displays, configurable game setup and controllers,
screenshots, debugging tools, memory improvements, and game-specific
fixes in a single plugin.

## Features

-   NBA Live 2005, NBA Live 06, NBA Live 07, and NBA Live 08 support
-   Modern resolution and widescreen support
-   Updated resolution text in the game's display menus
-   Custom frontend/menu resolution
-   Higher-resolution frontend/menu fonts
-   Windowed mode and intro video control
-   Native MSAA anti-aliasing
-   Native supersampling (SSAA) for improved edge quality, including
    court graphics
-   PNG and TGA screenshots with configurable output directory
-   Custom Direct3D 9 scoreboards and broadcast overlays
-   Expanded stat-overlay classifications and data bindings
-   Custom Starting 5 and Outro graphics in NBA Live 07 and NBA Live 08
-   Custom in-game lineup graphics in NBA Live 08
-   F5 live reload for custom popup packages
-   F6 CSV box-score export with player names
-   Stadium Server with arena-specific courts, backboards, Dornas,
    cameras, and configuration
-   Playoffs, Conference Finals, and NBA Finals stadium asset overrides
-   Dynamic stadium/jumbotron game data
-   NBA Live 2005 stadium lighting controls
-   Custom startup teams, jerseys, starting lineups, and arena selection
-   Keyboard/gamepad side, profile, and player-lock configuration
-   Optional fast filesystem wait path to reduce game loading delays
-   NBA Live 06 loading profiler and frontend input/navigation diagnostics
-   Forced playoff-stage/series presentation context for testing and custom setups
-   Additional `faces\` file-search path
-   Increased game memory for larger custom assets
-   Expanded debugger and crash diagnostics
-   Additional game-specific stability and rendering fixes

## Installation

The release package contains three directories:

``` text
all
2005
06
```

### 1. Install the NBA Live ASI Loader

Download the NO-CD fixed executable for the game you would prefer windowed mode. You need the NO-CD fixed executable as the plugin is designed for a certain version of an executable. NO-CD is also advised as original CD's of NBA Live 2005-08 series unfortunately do not run in Windows 10 and further.

Download ASI Loader and follow instructions in Installation section at its Readme file (Here: https://forums.nba-live.com/downloads.php?view=detail&df_id=13583).

Make sure you have the latest release from Releases.

Copy "d3d9.dll", "oledlg.dll" and "plugins" folders from the archive to your game's root folder. Otherwise, this plugin will not work.

### 2. Remove Previous Plugins

If you previously used the standalone Windowed Mode or Resolution
plugins, remove these files in "plugins" folder:

``` text
NBAWindowedMode.asi
NBALiveResolution.asi
```

Their functionality is already included in NBA Live Launcher.

### 3. Install the Common Files

Open the `all` directory and copy **all of its contents** into the main
directory of the NBA Live game you want to use.

The launcher supports:

-   NBA Live 2005
-   NBA Live 06
-   NBA Live 07
-   NBA Live 08

### 4. Install Game-Specific Files

If you are playing **NBA Live 2005**, copy the contents of the `2005`
directory into your NBA Live 2005 directory and overwrite files when
prompted.

If you are playing **NBA Live 06**, copy the contents of the `06`
directory into your NBA Live 06 directory and overwrite files when
prompted.

NBA Live 07 and NBA Live 08 only require the contents of the `all`
directory.

A compatible supported executable is required.

## Display Configuration

Display settings are configured through `main.ini`.

Example:

``` ini
[DISPLAY]
RES_X=1920
RES_Y=1080
WINDOWED=1
ANTI_ALIASING=1
MSAA_SAMPLES=4
SUPERSAMPLING=0
SUPERSAMPLE_PERCENT=150

[BOOTUP]
INTRO=1
```

`RES_X` and `RES_Y` set the startup/frontend resolution. You can still
choose your preferred gameplay resolution from the normal in-game
resolution options.

The resolution text displayed by the game's own menus has also been
updated for the expanded resolution support.

-   `WINDOWED=1` enables windowed mode.
-   `WINDOWED=0` uses fullscreen mode.
-   `INTRO=1` enables intro videos.
-   `INTRO=0` disables intro videos.

### Anti-Aliasing

NBA Live Launcher provides native anti-aliasing support for all four
supported games. External NVIDIA or AMD control-panel overrides are no
longer required.

MSAA can be enabled with:

``` ini
[DISPLAY]
ANTI_ALIASING=1
MSAA_SAMPLES=4
```

`MSAA_SAMPLES` controls the requested multisample level. Common values
include `2`, `4`, `8`, and `16`. The final available level depends on
the hardware, backbuffer format, and display mode.

### Supersampling

Supersampling renders the game internally at a higher resolution and
downsamples the result to the selected output resolution. This improves
hard and jagged edges throughout the rendered scene and is especially
useful for court graphics and other surfaces that are not fully improved
by conventional MSAA.

Enable it with:

``` ini
[DISPLAY]
SUPERSAMPLING=1
SUPERSAMPLE_PERCENT=150
```

Supported supersampling percentages are:

-   `125`
-   `150`
-   `175`
-   `200`

When supersampling is enabled, the current renderer uses SSAA instead of
MSAA for that run.

## Frontend Font Improvements

NBA Live Launcher improves font rasterization in the frontend so menu
text remains sharper at modern resolutions.

This applies to **frontend/menu fonts only**. In-game fonts are
unchanged.

## Screenshots

NBA Live Launcher includes a replacement Direct3D 9 screenshot system
for NBA Live 2005, NBA Live 06, NBA Live 07, and NBA Live 08.

Configure it in `main.ini`:

``` ini
[SCREENSHOT]
FORMAT=png
DIRECTORY=
```

Supported formats:

-   `png`
-   `tga`

If `DIRECTORY` is empty, screenshots are saved to the normal NBA Live
screenshot directory under the user's Documents folder.

A custom directory can also be specified:

``` ini
[SCREENSHOT]
FORMAT=png
DIRECTORY=C:\NBA Screenshots
```

Relative screenshot directories are resolved under the user's Documents
directory.

## Custom Scoreboards and Broadcast Overlays

Custom popup packages are stored under:

``` text
popups\<package name>\
```

Configure the active package in `main.ini`:

``` ini
[OVERLAY]
CUSTOM_OVERLAY=1
CUSTOM_OVERLAY_NAME=TEST
```

`CUSTOM_OVERLAY=1` enables custom overlays. Set it to `0` to use the
game's original scoreboard and overlays.

`CUSTOM_OVERLAY_NAME` selects the popup package to load.

For example:

``` ini
CUSTOM_OVERLAY_NAME=TNT07
```

loads:

``` text
popups\TNT07\
```

Restart the game after changing `CUSTOM_OVERLAY` or
`CUSTOM_OVERLAY_NAME`.

Press **F5** during the game to reload the current custom popup package
while editing.

### Supported Broadcast Graphics

The custom broadcast system includes support for scoreboards,
violations, play calls, player-foul graphics, player and team
statistics, and other supported in-game graphics.

The stat-overlay system includes expanded classifications and additional
data bindings, allowing themes to display more player and team
information. Player jersey numbers can also be used by supported stat
and foul graphics.

Starting 5 packages can also use enriched live-player data, including
first name, last name, full name, jersey number, position, player portrait,
team name/city, side, team colors, and team logo. Two-team stat graphics
(such as Team Leaders) can bind separate left/right team logos instead of
assuming the first displayed player is always Home and the second is Away.

Additional presentation support is game-specific:

-   **NBA Live 07 and NBA Live 08:** custom Starting 5 graphics
-   **NBA Live 07 and NBA Live 08:** custom Outro graphics
-   **NBA Live 08:** custom in-game lineup graphics

Unsupported or unconfigured graphics can continue to use the game's
original presentation.

For detailed instructions on creating and editing scoreboards, stat
overlays, Starting 5 graphics, fonts, images, bindings, animations, and
other popup content, see the **Scoreboard Theme Editor User Guide**:

https://github.com/muratcansarkalkan/ScoreboardThemeEditor/blob/main/USER_GUIDE.md

## Box Score Export

Press **F6** during a game to export the current box score in CSV
format.

The export includes resolved **player names** together with the
available player and game statistical data, making the CSV easier to
inspect and use outside the game.

## Stadium Server

NBA Live Launcher includes a stadium server for loading arena-specific
assets and properties without replacing the game's base files.

Stadium content can include:

-   Courts
-   Backboards
-   Dornas
-   Cameras
-   Stadium-specific configuration
-   Dynamic stadium/jumbotron displays
-   NBA Live 2005 stadium lighting properties

The stadium override/configuration system is available across the
supported games. Custom stadium camera loading is currently supported in
**NBA Live 2005, NBA Live 06, and NBA Live 07**.

### Event-Specific Stadium Assets

Courts, backboards, and Dornas can have separate versions for special
postseason events:

-   Playoffs
-   Eastern Conference Finals
-   Western Conference Finals
-   NBA Finals

This allows a stadium to automatically use event-specific court
graphics, backboards, and Dorna content while retaining its normal
regular-season assets.

For custom backboards, place the required `.bbd` files in the stadium's
`backboard` directory. Event-specific backboards can then be placed in
the corresponding Playoffs, Conference Finals, or NBA Finals override
path.

The same event-specific override concept is available for court and
Dorna content.

For detailed stadium model setup, folder conventions, materials, dynamic
displays, playoff assets, and EBO editing, see **NBA Live EBO Tools**:

https://github.com/muratcansarkalkan/EBOTools/

## Stadium Configuration

Per-stadium JSON configuration can be stored under:

``` text
assets\stadia\<stadium-abbreviation>.json
```

### Dorna Configuration

Example:

``` json
{
  "dorna": {
    "ad_count": 12,
    "period": 8.6,
    "transition_period": 0.6
  }
}
```

-   `ad_count` controls the number of Dorna advertisements.
-   `period` controls the complete Dorna cycle period.
-   `transition_period` controls transition timing.

If a stadium configuration or individual setting is missing, the game
uses its original value.

### Custom Stadium Cameras

A stadium can specify a custom camera file:

``` json
{
  "camera": "assets/camera/boston.mgd"
}
```

When no explicit camera is configured, supported games can use the
stadium ID to look for a matching camera under:

``` text
assets\camera\<stadium-id>.mgd
```

Custom stadium cameras are currently supported in NBA Live 2005, NBA
Live 06, and NBA Live 07.

### NBA Live 2005 Stadium Lighting

NBA Live 2005 supports additional per-stadium lighting/material
adjustments.

Available controls include:

-   Brightness
-   Contrast
-   Saturation
-   Gamma

Example:

``` json
{
  "stadiumLighting": {
    "brightness": 1.0,
    "contrast": 1.0,
    "saturation": 1.0,
    "gamma": 1.0
  }
}
```

Stadium lighting configuration is currently **NBA Live 2005 only**.

## Dynamic Stadium Displays

Compatible stadium models can display live game information using
dynamic runtime materials.

Supported data includes information such as:

-   Home and away scores
-   Game clock
-   Shot clock
-   Period and overtime
-   Home and away timeouts
-   Home and away team fouls
-   On-court player jersey numbers
-   Player points
-   Player fouls

This allows compatible arena scoreboards and jumbotrons to display live
game and player data instead of relying only on static textures.

NBA Live Launcher provides the runtime data. For information about
setting up the required materials and model structure, see **NBA Live
EBO Tools**:

https://github.com/muratcansarkalkan/EBOTools/

## Included Arenas

The release package includes **four configured arenas** with Dorna,
backboard, and camera content for:

-   NBA Live 2005
-   NBA Live 06
-   NBA Live 07

These provide ready-to-use examples of the Stadium Server functionality.

## Startup Game Configuration

The initial Play Now setup can be overridden through `main.ini`. These
settings are available in NBA Live 2005, NBA Live 06, NBA Live 07, and
NBA Live 08.

``` ini
[STARTUP]
HOME_TEAMNUM=-1
AWAY_TEAMNUM=-1
HOME_JERSEY_ID=-1
AWAY_JERSEY_ID=-1
HOME_STARTING5=
AWAY_STARTING5=
ARENA_TEAMNUM=-1
ARENA_FORCE=0
```

`HOME_TEAMNUM` and `AWAY_TEAMNUM` use database `TEAMNUM` values. A value
of `-1` leaves the game's original team selection unchanged.

`HOME_JERSEY_ID` and `AWAY_JERSEY_ID` select the initial jersey ID for
each side. A value of `-1` leaves the normal jersey-selection path
unchanged. The launcher only supplies the configured jersey when the
team does not already have an explicit jersey choice, so a later manual
frontend selection is not overwritten.

`HOME_STARTING5` and `AWAY_STARTING5` accept five distinct comma-separated
positions from `0` through `11`, for example:

``` ini
HOME_STARTING5=0,1,2,3,4
AWAY_STARTING5=1,0,2,4,3
```

These values refer to positions in EA's native default 12-player lineup
array. They are not guaranteed to be identical to DBF `ROSTERPOS` values
for every custom roster. The selected five players are moved to the front
of the active lineup while the remaining native order is preserved.

`ARENA_TEAMNUM` selects the venue associated with a team. By default the
launcher only supplies the arena when the native venue has not already
been resolved. Set `ARENA_FORCE=1` to replace an already resolved venue.

Optional diagnostics are available with `STARTING5_TRACE=1` and
`ARENA_TRACE=1`. Starting-five diagnostics are written to
`StartupTeams.log`.

## Controller Setup

NBA Live Launcher can preconfigure keyboard and gamepad assignment before
gameplay, including side assignment, controller profile selection, and
player lock. The feature uses the games' native controller/configuration
paths rather than replacing gameplay input handling.

Enable it in `main.ini`:

``` ini
[CONTROLLERS]
ENABLED=1
USE_DOCUMENTS_CONFIGS=1

[KEYBOARD]
ENABLED=1
SIDE=HOME
CONFIG=keyboard.kfg
PLAYER_LOCK=-1

[JOYSTICK_1]
ENABLED=1
NATIVE_SLOT=0
SIDE=AWAY
CONFIG=gamepad.jfg
CONFIG_DEVICE_INDEX=AUTO
PLAYER_LOCK=-1
```

`SIDE` accepts `HOME`, `AWAY`, `NEUTRAL`, or `UNCHANGED`.

Keyboard configurations use `.kfg`; gamepad configurations use `.jfg`.
With `USE_DOCUMENTS_CONFIGS=1`, profiles are resolved from the game's
`Documents\<NBA LIVE game>\configs` directory. Set it to `0` to use
`interface\configs` under the installation directory.

For joystick side assignment, `NATIVE_SLOT` must identify a unique native
user slot from `0` through `6`; slot `7` is reserved for the keyboard.
`CONFIG_DEVICE_INDEX=AUTO` is recommended so the launcher can use the
verified native loader mapping.

`PLAYER_LOCK=-1` leaves player lock unchanged. Values `0` through `4`
select a player-lock position for a controller assigned to Home or Away.

Controller setup writes diagnostic information to `ControllerSetup.log`.

### NBA Live 06 Frontend Device Navigation

NBA Live 06 also includes an optional frontend input/navigation layer in
`DeviceNavigation.ini`. It can bypass the two verified frontend input
gates and optionally remap existing native frontend actions. This is
primarily useful for controller-driven frontend experiments and custom
launcher/menu workflows.

Basic configuration:

``` ini
[DEVICE_NAVIGATION]
ENABLE=1
GATE_MODE=BYPASS
REMAP_BUTTONS=0
```

Use `GATE_MODE=NATIVE` to leave the original checks untouched. Additional
trace options in this file are intended for development and diagnostics.

## Forced Game and Playoff Context

For testing presentation packages and special-event behavior, the launcher
can force a postseason stage independently of the normal season/playoff
frontend flow.

``` ini
[PLAYOFF]
FORCE_STAGE=OFF
GLOBAL_GAME_MODE=0
FORCE_SERIES_GAME=0
FORCE_SERIES_LENGTH=7
FORCE_HOME_WINS=-1
FORCE_AWAY_WINS=-1
LOG_STATE=0
```

`FORCE_STAGE` accepts `OFF`, `PLAYOFF`, `ECSF`, `WCSF`, `ECF`, `WCF`, or
`FINALS`. When `GLOBAL_GAME_MODE=0`, the forced stage is limited to the
stadium/event-asset context. `GLOBAL_GAME_MODE=1` also promotes the game
into the corresponding playoff presentation context where supported.

A forced series state is valid only when the requested wins add up to
`FORCE_SERIES_GAME - 1`, neither side has already clinched the series,
and the length is `1`, `3`, `5`, or `7`. Series forcing requires both a
forced stage and `GLOBAL_GAME_MODE=1`.

`LOG_STATE=1` enables additional playoff-state diagnostics in
`logs\playoff_state.log`.

The native special-event mode can also be selected for testing:

``` ini
[GAME_MODE]
FORCE_MODE=OFF
```

Supported values are `OFF`, `ALLSTAR`, and `ROOKIE`
(`ROOKIE_CHALLENGE` is also accepted). Native special-event forcing takes
priority over forced playoff mode so the two contexts are not mixed.

## Custom Faces Folder

NBA Live Launcher adds:

``` text
faces\
```

to the game's file-search paths in NBA Live 2005, NBA Live 06, NBA Live
07, and NBA Live 08.

This allows supported files to be loaded from a dedicated `faces`
directory.

## Memory Improvements

NBA Live Launcher expands several game memory/resource limits to provide
more room for larger custom stadiums and other modded assets.

The current launcher includes memory improvements for NBA Live 2005, NBA
Live 06, NBA Live 07, and NBA Live 08.

NBA Live 2005 also increases a known temporary resource-list capacity
from **118 to 512 entries**.

## Loading Performance and Profiling

The launcher includes an optional filesystem wait optimization for all four
supported games:

``` ini
[LOADING]
FAST_FILE_WAIT=1
```

The optimization changes only the verified filesystem completion-wait path.
It keeps the game's completion checks and callback pump intact and does not
apply a global `Sleep`, timer-resolution, or FPS patch. If the required hook
cannot be validated for the running executable, the launcher leaves the
original path unchanged.

NBA Live 06 also includes a detailed loading profiler for development work.
It can capture native asset-entry order and intercepted file API timings to
CSV/log files under `logs\`.

``` ini
[DEBUG]
CONSOLE=0

[LOADING_PROFILE]
ENABLED=1
AUTO_CAPTURE=1
AUTO_START=0
MAX_EVENTS=65536
MAX_SECONDS=120
```

With automatic capture enabled, profiling starts at the verified 5-on-5
loading boundary and stops at pregame intro setup. With automatic capture
disabled, **F9** can start and stop a capture manually. `AUTO_START=1` starts
a capture as soon as the profiler is initialized and F9 can still stop it.

The profiler is currently a verified NBA Live 06 development feature;
`FAST_FILE_WAIT` is implemented separately for NBA Live 2005–08.

## Debugger

NBA Live Launcher includes an optional debugging console intended
primarily for mod development and troubleshooting.

Enable it through `main.ini`:

``` ini
[DEBUG]
CONSOLE=1
FILES=1
FAILED_FILES=1
FILE_CALLERS=1
CRASHES=1
ASSET_FILES=1
ANIMBANK=0
LIVE07_CLEANUP_FIX=1
```

Available diagnostics include file activity, failed file loads, callers,
asset loading, animation-bank diagnostics, crash information, and
additional game-specific runtime diagnostics.

Extensive file logging can reduce performance and is generally intended
for troubleshooting rather than normal gameplay.

The debugger also includes additional NBA Live 07 diagnostics and
cleanup protection used to address runtime errors found during
development.

Set:

``` ini
[DEBUG]
CONSOLE=0
```

for normal gameplay when debugging output is not needed.

## Changelog

### v0.5

#### Game Setup and Controllers

-   Expanded startup configuration from team selection to full Play Now
    setup across NBA Live 2005–08.
-   Added configurable Home/Away jersey IDs without overriding later manual
    frontend jersey selections.
-   Added configurable Home/Away starting fives using five positions from
    EA's native default lineup array.
-   Added configurable arena selection and optional forced venue replacement.
-   Added native keyboard/gamepad side assignment.
-   Added `.kfg` / `.jfg` controller-profile loading and synchronization with
    the games' native selected-profile state.
-   Added per-controller player-lock configuration.
-   Added NBA Live 06 frontend device-navigation gate control, native action
    remapping, and diagnostic tracing.

#### Loading and Frontend Timing

-   Added `FAST_FILE_WAIT` for NBA Live 2005, NBA Live 06, NBA Live 07, and
    NBA Live 08 to reduce unnecessary filesystem completion polling delay.
-   Preserved native completion checks/callback handling; the optimization
    does not globally alter `Sleep`, timer resolution, or gameplay FPS.
-   Added an NBA Live 06 loading profiler with automatic/manual capture, F9
    control, CSV event output, and detailed log/status files.
-   Corrected frontend/APT timing paths that could advance menu animations too
    quickly on very fast or uncapped frontend loops.

#### Widescreen and Rendering

-   Reworked perspective projection to use the actual viewport aspect ratio
    while preserving vertical field of view and native near/far planes.
-   Expanded conical-frustum culling for widescreen so objects visible at the
    wider horizontal FOV are not incorrectly culled.
-   Improved high-resolution frontend behavior and resolution handling in the
    supported games, including logical resolution ordering where required.

#### Broadcast Graphics

-   Expanded Starting 5 runtime bindings with player first/last/full names,
    jersey numbers, positions, portraits, team identity, side, colors, and
    team logo.
-   Added separate left/right team-logo bindings for two-team stat graphics
    such as Team Leaders, so logo selection follows the actual displayed
    players rather than a fixed Home/Away order.
-   Preserved the existing raw Starting 5 and lineup payload bindings for
    compatibility with older themes.

#### Playoff and Special-Event Context

-   Added forced postseason-stage selection for Playoffs, Conference
    Semifinals, Conference Finals, and NBA Finals testing.
-   Added optional global playoff game-mode promotion and forced series
    game/score context with validation.
-   Added playoff-state logging and additional presentation/commentary
    diagnostics.
-   Added optional native All-Star and Rookie Challenge mode forcing for
    presentation testing.

#### Stability and Diagnostics

-   Expanded crash/resource diagnostics and validation around newly hooked
    runtime paths.
-   Added fail-closed validation for controller, loading, playoff, and frontend
    hooks so unsupported/changing executable bytes leave the original path
    intact.

### v0.3

#### Stadium Server

-   Added the Stadium Server for arena-specific assets and
    configuration.
-   Added custom stadium properties/configuration.
-   Added arena-specific court, backboard, Dorna, and camera support.
-   Added event-specific court, backboard, and Dorna overrides for:
    -   Playoffs
    -   Eastern Conference Finals
    -   Western Conference Finals
    -   NBA Finals
-   Added support for custom `.bbd` backboards and event-specific
    backboard variants.
-   Added NBA Live 2005 stadium lighting controls for brightness,
    contrast, saturation, and gamma.
-   Added four configured arenas with Dorna, backboard, and camera
    content for NBA Live 2005, NBA Live 06, and NBA Live 07.
-   Expanded dynamic stadium/jumbotron runtime data for compatible arena
    models.

#### Graphics and Display

-   Added supersampling anti-aliasing (SSAA) support to NBA Live 2005,
    NBA Live 06, NBA Live 07, and NBA Live 08.
-   Added native MSAA support to all four games.
-   Improved anti-aliasing of hard edges, particularly court graphics
    and surfaces that are not fully handled by conventional MSAA.
-   Removed the need to force supersampling through NVIDIA or AMD driver
    control panels.
-   Added updated resolution text to the game's display menus.
-   Added higher-resolution frontend/menu font rendering.
-   In-game font rendering remains unchanged.

#### Broadcast Graphics

-   Expanded custom stat-overlay support and classifications.
-   Added more data bindings for player and team stat graphics.
-   Added player jersey-number support to compatible stat and foul
    popups.
-   Expanded Scoreboard Theme Editor support for the new popup data.
-   Added full custom Starting 5 support for NBA Live 07 and NBA Live
    08.
-   Added full custom Outro support for NBA Live 07 and NBA Live 08.
-   Added custom in-game lineup graphics for NBA Live 08.
-   Improved popup lifecycle and transition handling.

#### Box Scores

-   Improved the F6 CSV box-score export.
-   Box-score exports now include resolved player names instead of
    relying only on IDs/numeric data.

#### Screenshots

-   Added a replacement Direct3D 9 screenshot system for all four games.
-   Added PNG screenshot support.
-   Added TGA screenshot support.
-   Added configurable screenshot output directories.
-   Improved screenshot capture with the launcher's Direct3D rendering
    enhancements.

#### Memory and Mod Support

-   Expanded game memory/resource limits for NBA Live 2005, NBA Live 06,
    NBA Live 07, and NBA Live 08.
-   Increased the NBA Live 2005 temporary resource-list capacity from
    118 to 512 entries.
-   Improved support for larger custom stadium data and other modded
    assets.
-   Optimized stadium-specific asset overrides so they do not need to be
    registered as general global search paths.

#### Debugging and Fixes

-   Expanded the debugging console and file/resource diagnostics.
-   Added additional NBA Live 07 runtime diagnostics and cleanup
    protection.
-   Fixed the Lounge not displaying correctly in NBA Live 2005.
-   Fixed player clipping issues in NBA Live 07 and NBA Live 08.
-   Fixed an NBA Live 07 crash when quitting an in-progress game to the
    main menu.
-   Fixed additional NBA Live 07 runtime errors found through the
    expanded debugging system.

## Supported Games

-   NBA Live 2005
-   NBA Live 06
-   NBA Live 07
-   NBA Live 08

## Building

NBA Live Launcher is a **32-bit ASI plugin**.

The project uses the Visual Studio `v141_xp` toolset and the FIFAM/NBA
Live ASI development headers.

Open `NBALiveLauncher.sln`, configure the required local include paths,
and build the Win32 configuration.

The Scoreboard Theme Editor is a separate .NET application.

## Credits

-   Dmitri --- coding assistance and FIFAM ASI Loader development
-   wiscard_rush --- UI components
-   JuicyShaqMeat --- UI components
-   iceman --- widescreen intro videos for NBA Live 2005 and NBA Live 06

## License

See `LICENSE`.
