#pragma once
// Legacy Main.cpp entrypoint: teams, jerseys, lineups and venue.
void InitializeStartupTeams();

// Lightweight presentation-facing postseason snapshot.
// Round/stage is available for native and forced playoff games.
// Series win counts are currently marked valid when the launcher's forced
// series state is active.
enum GameSetupPostseasonStage {
    GAME_SETUP_STAGE_REGULAR = 0,
    GAME_SETUP_STAGE_PLAYOFF = 1,
    GAME_SETUP_STAGE_ECF = 2,
    GAME_SETUP_STAGE_WCF = 3,
    GAME_SETUP_STAGE_FINALS = 4,
    GAME_SETUP_STAGE_ECSF = 5,
    GAME_SETUP_STAGE_WCSF = 6,
    GAME_SETUP_STAGE_ECR1 = 7,
    GAME_SETUP_STAGE_WCR1 = 8
};

struct GameSetupPostseasonPresentation {
    bool active;
    int stage;
    bool seriesValid;
    int seriesGame;
    int seriesLength;
    int homeWins;
    int awayWins;
};

bool GetGameSetupPostseasonPresentation(
    GameSetupPostseasonPresentation* presentation);

