# NBA Live Scoreboard Theme Editor

Open `popups\TEST\scoreboard.json`, select preview teams, and drag the known
scoreboard elements directly on the canvas. Exact bounds, behavior rules, font
settings, simulated game data, and asset-backed team previews are supported.

The Font tab independently controls score, game clock, shot clock, team name,
period, numeric/text foul, numeric/text timeout, and BONUS heights. Dot, bar,
and image indicator sizes use their draggable element bounds instead.

The Preview selector shows the scoreboard over a bundled generic broadcast
background at 4:3, 16:9, or 16:10. The stage uses the same centered placement,
reference resolution, offsets, and uniform/fixed scale rules as the game.

The editor automatically migrates legacy named scoreboard properties into an
ordered `elements[]` collection. The Layers tab can add rectangles, images,
and text; duplicate/delete layers; and change their Z-order. Hidden and locked
layers remain selectable from the list.

The Element tab controls bounds, opacity, visibility, locking, alignment,
`overflow` versus `fit`, solid fills, and two-color horizontal/vertical
gradients. Primary and secondary team-color panels are ordinary layers.

Useful bindings include:

```text
away.primaryColor       home.primaryColor
away.secondaryColor     home.secondaryColor
away.logo                home.logo
away.name                home.name
away.score               home.score
away.fouls               home.fouls
away.timeouts            home.timeouts
away.bonus               home.bonus
game.clock               game.shotClock
game.period
```

Use `type: "indicator"` for foul or timeout bindings. Gradient endpoints can
each use a fixed decimal RGB color or one of the four team-color bindings.

## Build

Open `ScoreboardThemeEditor.csproj` in Visual Studio 2022 with the .NET 8 SDK,
or run:

```powershell
dotnet build -c Release
```

## Live workflow

1. Run NBA Live with the updated ASI plugin.
2. Pause the game.
3. Edit the theme.
4. Click **Save + Reload in Game**.
5. Resume.

The editor updates `.reload`; the plugin checks it every 500 ms from the D3D9
render thread. `F5` remains available as a manual reload method.

The first release edits the main scoreboard. The same canvas/property model is
intended to add `stat.json`, `violation.json`, `playcall.json`, `intro.json`,
and lineup layouts next.
