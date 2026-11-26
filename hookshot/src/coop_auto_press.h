// SPDX-License-Identifier: GPL-3.0-or-later
// Presses co-op makes for the player on the game's own menu (the title's Continue, its save list's newest save, then
// Load, after a join made in the game: coop_title.cpp OnPad). One rule for all of them: while they run, the game gets
// only their A pulses. The player's pad, keys and mouse are held off until they end: a pad press is dropped (OnPad
// returns the pulses only), and the mouse neither points nor clicks in the game's menus (hos_coop_menu_mouse_held,
// Longshot patch 0030's oot_menu.c). Longshot's menu mouse moves the game's focus to the button under the cursor, so a
// hover during the presses moved them onto another button (a player's report, 2026-11-24: Continue clicked with
// the mouse after a public-lobby join, then a crash).
//
// Logic only (tests/hookshot/auto_press_test.cpp).
#pragma once

#include <array>
#include <cstdint>

namespace CoopAutoPress {

// An A at each of these times (ms after the presses began), held this long; done at kEndMs.
inline constexpr std::array<std::uint64_t, 4> kAtMs{400, 2900, 5400, 7900};   // (the last again: Load came late once)
inline constexpr std::uint64_t kHoldMs = 100, kEndMs = 9000;

// The presses began at `t0_ms` (0: none). True while they run at `now_ms`: the player's input is held off.
inline bool Running(std::uint64_t t0_ms, std::uint64_t now_ms) {
    return t0_ms != 0 && now_ms >= t0_ms && now_ms - t0_ms < kEndMs;
}

// The buttons the game gets `elapsed_ms` into the presses: `a` during a pulse, else nothing (never the player's).
inline std::uint64_t Pad(std::uint64_t elapsed_ms, std::uint64_t a) {
    for (const std::uint64_t at : kAtMs) {
        if (elapsed_ms >= at && elapsed_ms < at + kHoldMs) return a;
    }
    return 0;
}

}  // namespace CoopAutoPress
