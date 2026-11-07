# Shared Playoff Presentation Bindings

Playoff presentation data is populated on every overlay Frame, not only the scoreboard.

Available text bindings in any overlay JSON:

- `playoff.roundText`
- `playoff.seriesText`
- `playoff.active` (`1` or `0`)
- `playoff.stage` (numeric GameSetup postseason-stage value)
- `playoff.seriesGame`
- `playoff.homeWins`
- `playoff.awayWins`

Available image bindings in any overlay JSON:

- `playoff.roundImage`
- `playoff.seriesLeaderLogo`

This applies to scoreboard, violation, player foul, generic stat/Team Leaders,
play call, intro, Starting 5, outro, and lineups render paths.

The round image remains a shared asset loaded from the theme's `scoreboard`
asset folder, so the same ECR1/WCR1/ECSF/WCSF/ECF/WCF/Finals art can be
reused in any presentation JSON without duplicating files.

Examples:

```json
{
  "type": "text",
  "binding": "playoff.roundText"
}
```

```json
{
  "type": "text",
  "binding": "playoff.seriesText"
}
```

```json
{
  "type": "image",
  "binding": "playoff.roundImage"
}
```

```json
{
  "type": "image",
  "binding": "playoff.seriesLeaderLogo"
}
```
