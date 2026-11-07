# Dynamic playoff scoreboard bindings

This patch adds presentation bindings driven by GameSetup's postseason context.

## Text bindings

- `playoff.roundText`
- `playoff.seriesText`

`playoff.roundText` values:
- NBA Playoffs
- Eastern Conference Semifinals
- Western Conference Semifinals
- Eastern Conference Finals
- Western Conference Finals
- NBA Finals

`playoff.seriesText` is currently populated when the launcher's forced-series
state is valid, for example:
- `Series tied 1-1`
- `ATL leads 3-0`
- `BOS leads 2-1`

The series line is intentionally blank for native playoff games when this
source set does not expose trustworthy native current-series win counts.

## Image bindings

- `playoff.roundImage`
- `playoff.seriesLeaderLogo`

Round image lookup uses these optional scoreboard-relative files:

- `scoreboard/images/playoffs.png`
- `scoreboard/images/ecsf.png`
- `scoreboard/images/wcsf.png`
- `scoreboard/images/ecf.png`
- `scoreboard/images/wcf.png`
- `scoreboard/images/finals.png`

Missing files simply produce no texture.

`playoff.seriesLeaderLogo` uses the already-loaded home/away team logo and is
blank when the series is tied or unavailable.

## JSON examples

Text:

```json
{
  "id": "playoffSeries",
  "type": "text",
  "binding": "playoff.seriesText",
  "x": 500,
  "y": 80,
  "width": 300,
  "height": 30,
  "alignment": "center"
}
```

Round image:

```json
{
  "id": "playoffRoundLogo",
  "type": "image",
  "binding": "playoff.roundImage",
  "x": 610,
  "y": 20,
  "width": 120,
  "height": 50,
  "imageFit": "contain"
}
```

Series-leading team logo:

```json
{
  "id": "seriesLeaderLogo",
  "type": "image",
  "binding": "playoff.seriesLeaderLogo",
  "x": 720,
  "y": 20,
  "width": 48,
  "height": 48,
  "imageFit": "contain"
}
```
