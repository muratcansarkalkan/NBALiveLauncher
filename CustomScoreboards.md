# Custom Scoreboards and Broadcast Overlays

This system replaces NBA Live's original in-game scoreboard and selected
broadcast graphics with configurable Direct3D 9 overlays. Layouts are stored as
JSON and can be edited with the included **NBA Live Overlay Theme Editor**.

## Supported games

- NBA Live 2005
- NBA Live 06
- NBA Live 07
- NBA Live 08

The same overlay package can be used by all four games. Game-specific addresses,
payload formats, scoreboard lifecycle behavior, pauses, transitions and native
overlay suppression are handled by the ASI plugin.

## Available custom overlays

| Overlay | Status | Layout location |
|---|---|---|
| Scoreboard | Supported | `scoreboard\scoreboard.json` |
| Violations | Supported | `violation\violation.json` |
| Play calls | Supported | `playcall\playcall.json` |
| Player-foul graphic | Supported | `stats\player_foul.json` |
| Player statistics | Supported | `stats\player.json`, `player_1.json`–`player_5.json` |
| Team statistics | Supported | `stats\team.json`, `team_1.json`–`team_3.json` |
| Subtype-specific statistics | Supported | `stats\<subtype>.json` |
| Pregame matchup intro | Supported | `intro\intro.json` |
| Native player introductions | Preserved | Custom layouts planned later |
| Starting lineups | Payloads identified; custom renderer planned | — |
| FSS and other special overlays | Planned | — |

Unknown, incomplete or unsupported statistical payloads continue using the
game's original overlay. The native graphic is suppressed only when a suitable
custom layout has loaded successfully.

## Installation

Add the supplied C++ files to the ASI project and compile it as a 32-bit Release
build. Build the editor with Visual Studio 2022 and the .NET 8 SDK.

The game directory must contain `main.ini`:

```ini
[OVERLAY]
; 0 uses the game's original scoreboard and overlays.
; 1 enables the selected custom overlay package.
CUSTOM_OVERLAY=1

; Directory name beneath popups. An invalid or missing name falls back to TEST.
CUSTOM_OVERLAY_NAME=TNT07
```

The package is selected when the ASI initializes. Restart the game after
changing `CUSTOM_OVERLAY` or `CUSTOM_OVERLAY_NAME`.

## Package structure

Recommended structure:

```text
NBA Live 2005\
├── main.ini
├── NBALiveLauncher.asi
└── popups\
    └── TNT07\
        ├── .reload
        ├── scoreboard\
        │   ├── scoreboard.json
        │   ├── popup.json
        │   ├── fonts\
        │   ├── images\
        │   └── teams\
        ├── violation\
        │   ├── violation.json
        │   ├── popup.json
        │   ├── images\
        │   └── teams\
        ├── playcall\
        │   ├── playcall.json
        │   ├── popup.json
        │   └── images\
        ├── stats\
        │   ├── player.json
        │   ├── player_1.json ... player_5.json
        │   ├── player_foul.json
        │   ├── team.json
        │   ├── team_1.json ... team_3.json
        │   ├── popup.json
        │   ├── fonts\
        │   ├── images\
        │   ├── portraits\
        │   └── teams\
        └── intro\
            ├── intro.json
            ├── popup.json
            ├── fonts\
            ├── images\
            └── teams\
```

Each overlay directory may use its own fonts and assets. This permits a team
logo, background or font used by the scoreboard to differ from the corresponding
asset used by a stat or intro graphic.

## Theme editor

Open any supported JSON layout in the editor. The top-right **Screen** selector
switches between Scoreboard, Stats, Intro, Violation and Playcall. The Stats
subtype selector exposes the player/team fallback layouts and their value-count
variants.

The editor supports:

- Dragging the entire overlay or individual elements
- Exact X, Y, width and height entry
- Multiple overlays in one preview
- Overlay-level and element-level Z-order
- 4:3, 16:9 and 16:10 preview resolutions
- Built-in or user-selected reference backgrounds
- Simulated scoreboard, violation, play-call, stat and intro data
- Save and Save As
- Save + Reload in Game
- Hidden and locked elements that remain selectable from the layer list

## Element types

Layouts use an ordered `elements` array. Every element has an ID, type, bounds,
visibility, lock state, opacity and Z-order.

Available types:

- `rectangle` — solid or linear-gradient shape
- `image` — external PNG/image or a dynamic logo/portrait binding
- `text` — static text or dynamically bound game text
- `indicator` — fouls or timeouts rendered as text, numbers, dots, bars or images

Example:

```json
{
  "id": "homeName",
  "type": "text",
  "binding": "home.name",
  "x": 450,
  "y": 12,
  "width": 130,
  "height": 26,
  "z": 20,
  "visible": true,
  "alignment": "right",
  "overflow": "fit",
  "font": "teamName",
  "fontHeight": 22,
  "textColor": 16777215
}
```

Colors are decimal RGB values. For example, white is `16777215` (`0xFFFFFF`).
Opacity uses values from `0` through `255`.

## Layers, colors and gradients

All panels, accents and decorative shapes can be ordinary JSON elements. They
do not need to be hardcoded in C++.

Rectangle fills support:

```json
"fill": {
  "type": "linearGradient",
  "startBinding": "home.primaryColor",
  "endBinding": "home.secondaryColor",
  "direction": "horizontal"
}
```

Available dynamic color bindings include:

```text
away.primaryColor       home.primaryColor
away.secondaryColor     home.secondaryColor
violation.teamColor     playcall.teamColor
stat.teamColor          stat.primaryColor
stat.secondaryColor
```

An image may also be tinted with a fixed color or one of these bindings. White
mask images with transparency work especially well as recolorable shapes.

## Images

Image elements support:

- `contain` — preserve aspect ratio and fit inside the element
- `stretch` — fill the complete element bounds
- Opacity
- Fixed tint
- Dynamic team-color tint
- Arbitrary background and decorative images
- Dynamic team logos and player portraits

Common image bindings:

```text
away.logo              home.logo
violation.teamLogo     stat.teamLogo
player.portrait        intro.awayLogo
intro.homeLogo
```

Use paths relative to the current overlay directory:

```json
{
  "id": "background",
  "type": "image",
  "image": "images/background.png",
  "imageFit": "stretch"
}
```

## Text

Text elements support:

- Left, center or right alignment
- Overflow beyond the element bounds
- Automatic fitting inside the element bounds
- Per-element font selection and height
- Uppercase, lowercase, capitalization and small-caps transforms
- Per-element color and opacity

Set `overflow` to:

- `overflow` — preserve the requested font size even if text exceeds its box
- `fit` — shrink the text until it fits the box

Text transforms:

```text
none  uppercase  lowercase  capitalize  smallCaps
```

For small caps, `smallCapsScale` controls the height of converted lowercase
characters.

## Fonts

Each overlay directory can contain its own `popup.json` and font files:

```json
{
  "fonts": {
    "default": {
      "fontFile": "fonts/scoreboard.ttf",
      "fontFace": "Roboto Condensed",
      "fontSourceHeight": 48,
      "fontWeight": 700,
      "characterSpacing": 1
    },
    "teamName": {
      "fontFile": "fonts/team-name.ttf",
      "fontFace": "Teko",
      "fontSourceHeight": 64,
      "fontWeight": 600,
      "characterSpacing": 0
    }
  }
}
```

Select a font on an element with `"font": "teamName"`. An empty font name uses
`default`. `fontFace` must match the font's internal family name, which may not
be the same as its filename.

The game loads local TTF files directly. The editor also loads theme-local font
files; if the same filename is overwritten with a different font while the
editor is running, restart the editor or use a new filename to avoid WPF font
caching.

## Scoreboard features

The scoreboard supports:

- Home and away team panels, names, logos and scores
- Game clock with tenths below one minute
- Shot clock always visible, threshold-only or disabled
- Separate normal and urgent shot-clock colors
- Team names as abbreviation, city, nickname, full name or two-letter code
- Multiple quarter formats with correct overtime handling
- Fouls and timeouts as numbers, text, dots, bars or images
- Remaining or used timeout counts
- BONUS and optional double-bonus text
- Independent home/away logo visibility
- Always-visible, after-score, late-game or hybrid visibility
- Configurable post-score duration and late-game clock threshold
- Uniform resolution scaling, fixed pixels or position-only behavior
- Fully editable backgrounds and primary/secondary-color accents

Main scoreboard bindings:

```text
away.name          home.name
away.logo          home.logo
away.score         home.score
away.fouls         home.fouls
away.timeouts      home.timeouts
away.bonus         home.bonus
game.clock         game.shotClock
game.period
```

## Violation overlay

Violation payloads expose:

```text
violation.title
violation.possession
violation.teamName
violation.teamColor
violation.teamLogo
```

The custom violation follows game transitions and pause behavior. Its native
`overlays~viol.big` request is bypassed only when the custom layout is ready.

## Play-call overlay

Play-call bindings:

```text
playcall.team
playcall.call
playcall.teamColor
playcall.raw0 ... playcall.raw3
```

The raw bindings expose the original four-value payload for unusual layouts.

## Statistical overlays

Common stat bindings:

```text
player.firstName       player.lastName
player.fullName        player.portrait
stat.label1            stat.value1
stat.label2            stat.value2
stat.teamName          stat.teamLogo
stat.teamColor         stat.primaryColor
stat.secondaryColor    stat.raw0 ... stat.raw14
```

The renderer first looks for a subtype-specific file such as
`player_rebounds.json` or `team_leaders.json`. If it is absent, it selects a
generic player/team layout based on the number of populated value groups:

```text
player_1.json ... player_5.json
team_1.json ... team_3.json
```

It then falls back to `player.json` or `team.json`. If no valid custom layout
exists, the original game graphic remains enabled.

Known player categories include points, assists, rebounds, steals, blocks,
biographical information, school, stat lines, injuries, fouls, shooting and
substitutions. Known team categories include scoring runs, makes/misses,
timeouts, leaders, shooting, rebounds, turnovers, bench scoring, positional
leaders and related breakdowns. Availability varies slightly by game version.

## Pregame intro

Intro bindings:

```text
intro.homeHeading
intro.awayCity          intro.awayNickname
intro.awayRecord        intro.homeCity
intro.homeNickname      intro.homeRecord
intro.liveFromHeading   intro.arena
intro.location          intro.awayTeamCode
intro.homeTeamCode      intro.leagueCode
intro.awayLogo          intro.homeLogo
intro.raw0 ... intro.raw14
```

The match-intro payload is copied into owned memory and retained during the
game, so records, arena, location and team identity remain available to future
overlays. Duplicate reads do not restart the animation.

Primary and alternate native match-intro requests are replaced. The indexed
native player-introduction path remains enabled until custom player-introduction
and lineup layouts are implemented.

## Animation

Violation, play-call, stat and intro layouts support enter, hold and exit
animation phases:

```json
"animation": {
  "enter": {
    "type": "slideFade",
    "fromX": -80,
    "fromY": 0,
    "duration": 250
  },
  "holdMilliseconds": 2500,
  "exit": {
    "type": "fade",
    "toX": 0,
    "toY": 0,
    "duration": 200
  },
  "freezeWhilePaused": true
}
```

Animation types are `none`, `slide`, `fade` and `slideFade`. Coordinates use
the overlay's reference-canvas units.

## Resolution behavior

Each layout declares a reference canvas and scaling mode:

```json
"referenceWidth": 1366,
"referenceHeight": 768,
"scaleMode": "uniform",
"offsetX": 0,
"offsetY": 18
```

- `uniform` scales size and position proportionally while preserving aspect
  ratio.
- `fixed` retains pixel dimensions.
- `positionOnly` retains element size while adapting anchored placement.

Linear texture sampling is enabled for downscaled logos, images and font
atlases. Very small output resolutions still contain fewer physical pixels and
cannot look identical to high-resolution output.

## Live reloading

Press **F5** in game to reload the package. **Save + Reload in Game** writes the
package's `.reload` marker, which the ASI checks approximately every 500 ms on
the render thread.

Layout, fonts, images, colors and animation settings are refreshed. Changes to
`main.ini` require a game restart.

## Diagnostics

Useful logs created beside the game executable include:

```text
extended_score_state.log
score_events.log
overlay_payloads.log
stat_payloads.log
stat_catalog.log
```

`extended_score_state.log` reports the selected package, resolved files,
installed hooks, team/logo matching and accepted custom payloads. If an overlay
does not appear, first confirm that its JSON exists at the reported path and
that its initialization line says the custom layout was enabled.

## Current limitations

- The editor preview uses WPF while the game uses Direct3D 9; minor font-metric
  differences can remain.
- Starting lineups and player-introduction cards currently remain native.
- FSS and unidentified overlay families are not yet custom-rendered.
- Changing the selected overlay package in `main.ini` requires restarting the
  game.
- A replaced TTF using the same filename may remain cached by the editor until
  restart.

These limitations do not prevent the original game graphics from being used;
unsupported or unconfigured overlay types deliberately fall back to native
behavior.
