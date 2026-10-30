#pragma once

// NBA Live 06 (1.0 NOCD) — optional Title -> game auto-start.
//
// main.ini:
//   [STARTUP]
//   AUTO_START_GAME=1
//   AUTO_START_DELAY_MS=0
//
// With AUTO_START_GAME=1, if present, the engine loads custom BIG files from:
//   <game root>\assets\autostart\06\screens~bootup~splash.big
//   <game root>\assets\autostart\06\screens~bootup~health.big
// It also accepts splash.big and health.big as aliases.
//
// With AUTO_START_GAME=0, all original BIG files are used and auto-start
// hooks are off. No BIG or EXE files are edited on disk.
//
// BOOT_STRAIGHT_TO_TITLE is obsolete/ignored; auto-start is the only switch.
// BOOT_SELECTOR_TRACE=1 remains available for diagnostics.
void InitializeAutoStartGame();
