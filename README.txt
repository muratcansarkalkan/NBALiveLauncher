Starting 5 runtime identity enrichment (NBA Live 07/08)

New bindings:
  starting5.player1FirstName ... starting5.player5FirstName
  starting5.player1LastName  ... starting5.player5LastName
  starting5.player1FullName  ... starting5.player5FullName
  starting5.player1Number    ... starting5.player5Number

Alias also supported:
  starting5.player1JerseyNumber ... starting5.player5JerseyNumber

Existing bindings are unchanged:
  starting5.player1Id ... player5Id
  starting5.player1Name ... player5Name  (EA abbreviated name, e.g. R. Foye)

The five display names are resolved against the current live 24-player cache.
No hard-coded player/name table is used. Unresolved enriched fields stay empty.

Additional Starting 5 team binding:
  starting5.cityName  - CITYNAME from the resolved TeamVisual/teams.json entry.

Starting 5 early-cache refresh fix
----------------------------------
Starting 5 presentation payloads in NBA Live 07/08 can arrive before normal
scoreboard polling has populated the 24-player live cache. ShotClock.cpp now
performs an on-demand state read and live-cache refresh before resolving the
five abbreviated Starting 5 names. Debug output includes:
  Starting5 live-cache refresh: version=07/08 refreshed=yes/no valid=yes/no ...

Starting 5 direct identity update
---------------------------------
NBA Live 07/08 Starting 5 player identity now uses the payload team and fixed
starter roster-slot order 4,3,2,1,0 (PG,SG,SF,PF,C) as the primary mapping.
This avoids abbreviated-name collisions such as M. Williams.

New text bindings:
  starting5.player1Position ... starting5.player5Position

Existing first/last/full-name and jersey-number bindings are populated from the
directly mapped runtime player. Same-team abbreviated-name matching is retained
only as a safety fallback when the expected roster slot is unavailable.


Season comparison shared stat family
------------------------------------
season_assists, season_blocks, season_rebounds, and season_steals now share
stats\season_comparison.json when no exact subtype JSON exists.

Resolution order:
  exact subtype -> season_comparison.json -> team_N.json -> team.json

Observed payload:
  raw0  title
  raw4/raw5  Season/value
  raw8/raw9  Tonight/value
  raw12 packed team color
  raw13 team code

stat.teamLogo and the existing stat team-color/name bindings resolve normally
from the team code.
