# NBA Live 08 In-Game Lineups

Place the layout at:

```text
assets\popups\<theme>\lineups\lineups.json
```

This overlay is available only in NBA Live 08. If the layout is missing or
the executable call sites do not validate, the native lineup overlay remains
enabled.

## Text bindings

```text
lineups.awayPlayer1
lineups.awayPlayer2
lineups.awayPlayer3
lineups.awayPlayer4
lineups.awayPlayer5
lineups.homePlayer1
lineups.homePlayer2
lineups.homePlayer3
lineups.homePlayer4
lineups.homePlayer5
lineups.awayTeamCode
lineups.awayTeamName
lineups.homeTeamCode
lineups.homeTeamName
lineups.raw0 ... lineups.raw13
```

## Image bindings

```text
lineups.awayLogo
lineups.homeLogo
```

## Color bindings

```text
lineups.awayPrimaryColor
lineups.awaySecondaryColor
lineups.homePrimaryColor
lineups.homeSecondaryColor
```

The game payload contains display names but not player IDs. This release does
not expose automatic player portraits for the in-game lineup overlay.

The overlay is permanently ended when `HideOverlaysEvent` starts a full-screen
transition. A later `ShowOverlaysEvent` does not revive that instance.
