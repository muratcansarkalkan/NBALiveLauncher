#pragma once

#include <array>
#include <string>

namespace LiveControllerSetup {
    enum class DeviceKind { Auto, Keyboard, Gamepad };
    enum class Side { Unchanged = -1, Neutral = 0, Home = 1, Away = 2 };

    struct Slot {
        bool enabled = false;
        DeviceKind device = DeviceKind::Auto;
        Side side = Side::Unchanged;
        int gamepadIndex = -1; // -1: auto; 0..3 only a launcher-side preference
        int playerLock = -1; // -1: EA default; modes 0..4 via native player-lock API
        std::string configFilename;
    };

    struct Settings {
        bool enabled = false;
        std::array<Slot, 8> slots{};
    };

    const Settings& GetSettings();
}

void InitializeControllerSetup();
