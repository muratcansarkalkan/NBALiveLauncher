# NBA Live 07 Starting Lineup and Outro

Layouts are loaded from:

```text
assets\popups\<CUSTOM_OVERLAY_NAME>\starting5\starting5.json
assets\popups\<CUSTOM_OVERLAY_NAME>\outro\outro.json
```

Starting-lineup portrait images are optional and use:

```text
assets\popups\<CUSTOM_OVERLAY_NAME>\starting5\portraits\<PLAYERID>.png
```

## Starting Lineup bindings

```text
starting5.player1Id ... starting5.player5Id
starting5.player1Name ... starting5.player5Name
starting5.player1Portrait ... starting5.player5Portrait
starting5.teamCode
starting5.teamName
starting5.teamLogo
starting5.side
starting5.teamColor
starting5.primaryColor
starting5.secondaryColor
starting5.raw0 ... starting5.raw10
```

`starting5.side` resolves to `away`, `home`, or `unknown` by comparing the
payload team code with the persistent Intro team codes.

## Outro bindings

```text
outro.homeHeading
outro.awayCity
outro.awayNickname
outro.awayRecord
outro.homeCity
outro.homeNickname
outro.homeRecord
outro.extra
outro.arena
outro.location
outro.awayScore
outro.homeScore
outro.awayTeamCode
outro.homeTeamCode
outro.leagueCode
outro.awayLogo
outro.homeLogo
outro.awayPrimaryColor
outro.awaySecondaryColor
outro.homePrimaryColor
outro.homeSecondaryColor
outro.raw0 ... outro.raw14
```

Both layouts support the same elements, templates, fonts, colors, gradients,
stroke, shadow, scaling, animation, and `overlayZ` options as the other custom
overlays.
