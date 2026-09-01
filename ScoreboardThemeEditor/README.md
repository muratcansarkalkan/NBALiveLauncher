# NBA Live Scoreboard Theme Editor

Open `popups\TEST\scoreboard.json`, select preview teams, and drag the known
scoreboard elements directly on the canvas. Exact bounds, behavior rules, font
settings, simulated game data, and asset-backed team previews are supported.

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
