#pragma once

#include <cstdint>
#include <climits>

enum class GameVersion { Live2005, Live2006, Live2007, Live2008 };

struct GameAddresses {
    GameVersion version;
    const char* name;
    uintptr_t sendEvent;
    uintptr_t gdAIAddress;
    uintptr_t gdInfoCentralAddress;
    uintptr_t getGameClock;
    uintptr_t getClockUnitsPerSecond;
    uintptr_t isShotClockValid;
    uintptr_t getShotClock;
    uintptr_t getTeamScore;
    uintptr_t getTeamIDFromSide;
    uintptr_t getTeamFouls;
    uintptr_t getTeamTimeoutsLeft;
    uintptr_t getOverlayData;
    unsigned int getQuarterSlot;
    unsigned int isGameClockValidSlot;
};

constexpr unsigned int INVALID_SLOT = UINT_MAX;

extern const GameAddresses NBA_LIVE_2005;
extern const GameAddresses NBA_LIVE_2006;
extern const GameAddresses NBA_LIVE_2007;
extern const GameAddresses NBA_LIVE_2008;
#pragma once
