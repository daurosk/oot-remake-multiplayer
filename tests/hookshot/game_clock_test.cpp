// SPDX-License-Identifier: GPL-3.0-or-later
// The game's clock (hookshot/src/coop_game_clock.h): the module's waits on the game's menus count the game's frames.
#include <doctest/doctest.h>

#include <cstdint>

#include "coop_auto_press.h"
#include "coop_game_clock.h"
#include "coop_pause_tab.h"

using CoopGameClock::Clock;
using CoopGameClock::Counted;
using CoopGameClock::kMostPerFrameMs;

TEST_CASE("game clock: the wall clock until the first frame, then on from it") {
    Clock c;
    CHECK_FALSE(c.Seen());
    CHECK(c.Now(5000) == 5000);   // (an exe that never calls the frame site: as before)
    c.Frame(6000);
    CHECK(c.Seen());
    CHECK(c.Now(6000) == 6000);   // (no jump: a stamp taken before the first frame stays right)
}

TEST_CASE("game clock: a running game keeps the wall clock's pace (every wait as long as it was)") {
    Clock c;
    std::uint64_t wall = 1000;
    c.Frame(wall);
    for (int i = 0; i < 30 * 25; ++i) {   // (25 s at 30 frames a second, 33 or 34 ms each)
        wall += (i % 3 == 0) ? 34 : 33;
        c.Frame(wall);
    }
    CHECK(c.Now(wall) == wall);
    CHECK(c.Now(wall + 20) == wall + 20);   // (between frames: on from the last)
}

TEST_CASE("game clock: a held game does not use a wait up (the bridge's hold, a stall, the emulator's pause)") {
    Clock c;
    c.Frame(1000);
    const std::uint64_t start = c.Now(1000);
    // Held for 60 s of wall clock: at most one frame's worth counts.
    CHECK(c.Now(61000) == 1000 + kMostPerFrameMs);
    // The bridge steps one frame each 2 s: 25 s of wall clock is 12 frames, 1.2 s of game.
    std::uint64_t wall = 1000;
    for (int i = 0; i < 12; ++i) {
        wall += 2000;
        c.Frame(wall);
    }
    CHECK(c.Now(wall) - start == 12 * kMostPerFrameMs);
    CHECK(c.Now(wall) - start < CoopPauseTab::kToTitleMs);   // (the to-title did not give up: test-bridge-c127-4)
    CHECK(c.Now(wall) - start < CoopAutoPress::kEndMs);
}

TEST_CASE("game clock: never goes back (unsigned waits cannot wrap)") {
    Clock c;
    c.Frame(1000);
    std::uint64_t last = 0;
    const std::uint64_t walls[] = {1000, 1050, 1300, 1310, 1200, 1400, 9000, 9001, 9050, 9050};
    for (const std::uint64_t w : walls) {
        const std::uint64_t before = c.Now(w);
        CHECK(before >= last);
        c.Frame(w);
        CHECK(c.Now(w) >= before);   // (the frame took on what the reads between saw)
        last = c.Now(w);
    }
}

TEST_CASE("game clock: a player's event holds the to-title's wait instead of using it up") {
    // Asked at 1000; an event (a 60 s talk) from 3000 to 63000 holds it; the menu then needs 5 s.
    const std::uint64_t start = 1000, held = 60000;
    CHECK(Counted(start, held, 63000) == 2000);
    CHECK(Counted(start, held, 68000) == 7000);
    CHECK(Counted(start, held, 68000) < CoopPauseTab::kToTitleMs);   // (the wall: 67 s, given up before)
    CHECK(Counted(start, 0, start + CoopPauseTab::kToTitleMs + 1) > CoopPauseTab::kToTitleMs);   // (no event: as before)
    CHECK(Counted(start, 0, start) == 0);
    CHECK(Counted(start, 0, 500) == 0);       // (a clock behind the start: nothing counted, no wrap)
    CHECK(Counted(start, 9000, 5000) == 0);   // (held more than ran: nothing, no wrap)
}
