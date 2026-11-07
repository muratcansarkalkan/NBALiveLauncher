#pragma once

// Detects NBA Live 2005/06/07/08 and installs the corresponding hooks.
void InitializeShotClock();
void InitializeDebugLogging();
// Read-only snapshot for the NBA Live 06 bench-actor identity probe.
// The DB ID (runtime field 0x88) is distinct from the native runtime ID.
// onCourt/courtSlot are inferred by ShotClock's lineup tracker, not verified
// authoritative gameplay state. Do not use them alone to move actors.
struct Live06BenchPlayerSnapshot {
    int side = -1;             // 0 home, 1 away
    int originalRosterSlot = -1; // runtime field 0x89; NOT mutable seat assignment
    int databasePlayerId = -1;
    unsigned int nativePlayerId = 0;
    int courtSlot = -1;
    bool trackedOnCourt = false;
    unsigned int snapshotAgeMs = 0;
    unsigned int refreshedAtTick = 0; // ShotClock player-cache generation
    // Advances only after a valid substitution pair has updated the lineup.
    // This is a presentation-event signal, NOT a verified engine commit.
    unsigned int lineupEventRevision = 0;
    unsigned int lineupEventTick = 0;
};

// Returns the number of database-ID matches (0, 1, or >1).
// Output is only valid with exactly one match. No refresh/game function calls.
int ShotClockLookupLive06BenchPlayerByDbId(int databasePlayerId,
    Live06BenchPlayerSnapshot* output);
