# Changelog

## v0.5

### Game Setup and Controllers

- Expanded startup configuration from team selection to full Play Now setup across NBA Live 2005, NBA Live 06, NBA Live 07, and NBA Live 08.
- Added configurable Home/Away jersey IDs while preserving later manual frontend jersey choices.
- Added configurable Home/Away starting fives using five positions from EA's native default 12-player lineup array.
- Added configurable arena selection with optional forced venue replacement.
- Added native keyboard/gamepad side assignment, `.kfg` / `.jfg` profile loading, and synchronization with the games' native selected-profile state.
- Added per-controller player-lock configuration.
- Added NBA Live 06 frontend device-navigation gate control, existing-action remapping, and input diagnostics for controller-driven frontend/custom-menu work.

### Loading and Frontend Timing

- Added `FAST_FILE_WAIT` for NBA Live 2005–08 to reduce unnecessary filesystem completion polling delay while retaining native completion checks and callback handling.
- The loading optimization does not globally change `Sleep`, timer resolution, or gameplay FPS, and fails closed when the verified hook cannot be installed.
- Added an NBA Live 06 loading profiler with automatic capture, manual F9 start/stop, `AUTO_START`, CSV event output, status reporting, and detailed logs.
- Corrected frontend/APT timing paths that could make menu animations advance too quickly on very fast or uncapped frontend loops.

### Widescreen and Rendering

- Reworked perspective projection to use the real viewport aspect ratio while preserving vertical FOV and the game's near/far clipping planes.
- Expanded conical-frustum culling for widescreen so geometry visible in the wider horizontal field is not incorrectly discarded.
- Improved high-resolution frontend and resolution-slot handling where required by the individual games.

### Broadcast Graphics

- Expanded Starting 5 bindings with player first name, last name, full name, jersey number, position, portrait, team name/city, side, team colors, and team logo.
- Added separate left/right team-logo bindings for two-team stat graphics such as Team Leaders, allowing logos to follow the actual displayed players instead of assuming a fixed Home/Away order.
- Preserved existing raw Starting 5 and lineup payload bindings for compatibility with older themes.

### Playoff and Special-Event Context

- Added forced postseason-stage selection for Playoffs, Conference Semifinals, Conference Finals, and NBA Finals testing.
- Added optional global playoff game-mode promotion and validated forced series game/score context.
- Added playoff-state logging and presentation/commentary diagnostics.
- Added optional native All-Star and Rookie Challenge mode forcing for presentation testing.

### Stability and Diagnostics

- Expanded runtime validation and diagnostics around controller, loading, playoff, frontend, resource, and crash-related hooks.
- New hooks are designed to fail closed when the expected executable bytes or runtime state do not match.

## v0.3

The previous v0.3 changelog remains documented in `README.md`, covering the Stadium Server, SSAA/MSAA, expanded broadcast graphics, box-score export, screenshots, memory expansion, debugger improvements, and game-specific fixes introduced in that release.
