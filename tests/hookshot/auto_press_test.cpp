// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's presses for the player on the game's menu (hookshot/src/coop_auto_press.h).
#include <doctest/doctest.h>

#include <cstdint>

#include "coop_auto_press.h"

using namespace CoopAutoPress;

TEST_CASE("auto press: the player's input is held for the whole run, and only then") {
    CHECK_FALSE(Running(0, 5000));            // (no presses)
    CHECK(Running(1000, 1000));               // (from their first ms)
    CHECK(Running(1000, 1000 + kAtMs[0]));    // (a pulse)
    CHECK(Running(1000, 1000 + 1500));        // (between pulses: a hover or click then moved the focus)
    CHECK(Running(1000, 1000 + kEndMs - 1));
    CHECK_FALSE(Running(1000, 1000 + kEndMs));
    CHECK_FALSE(Running(5000, 4000));         // (a clock before the start: not running)
}

TEST_CASE("auto press: the game gets only the pulses, never the player's buttons") {
    constexpr std::uint64_t kA = 1;
    int pulses = 0;
    std::uint64_t last = 0;
    for (std::uint64_t e = 0; e < kEndMs; ++e) {
        const std::uint64_t pad = Pad(e, kA);
        CHECK((pad == 0 || pad == kA));
        if (pad && !last) ++pulses;
        last = pad;
    }
    CHECK(pulses == static_cast<int>(kAtMs.size()));
    CHECK(Pad(kAtMs[0] - 1, kA) == 0);
    CHECK(Pad(kAtMs[0], kA) == kA);
    CHECK(Pad(kAtMs[0] + kHoldMs, kA) == 0);   // (let go: the next A is a new press)
    for (const std::uint64_t at : kAtMs) CHECK(at + kHoldMs <= kEndMs);   // (every pulse inside the run)
}
