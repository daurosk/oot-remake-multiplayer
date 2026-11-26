// SPDX-License-Identifier: GPL-3.0-or-later
// One cursor model for co-op's own lists (hookshot/src/coop_menu_nav.h, menu-polish-01113).
#include <doctest/doctest.h>

#include <cstdint>

#include "coop_menu_nav.h"

using namespace CoopMenuNav;

namespace {
constexpr std::uint64_t kA = 1, kUp = 2, kDown = 4, kDirs = kUp | kDown;
}  // namespace

TEST_CASE("menu nav: a press moves once, a held direction repeats after the delay") {
    Repeat r;
    std::uint64_t rep = 0;
    CHECK(r.Presses(kDown, 1000, kDirs, &rep) == kDown);   // (the press: at once)
    CHECK(rep == 0);                                       // (a fresh press, not a repeat)
    CHECK(r.Presses(kDown, 1100, kDirs) == 0);
    CHECK(r.Presses(kDown, 1399, kDirs) == 0);             // (not yet: the delay)
    CHECK(r.Presses(kDown, 1400, kDirs, &rep) == kDown);   // (the first repeat)
    CHECK(rep == kDown);
    CHECK(r.Presses(kDown, 1450, kDirs) == 0);
    CHECK(r.Presses(kDown, 1490, kDirs) == kDown);         // (then every 90 ms)
    CHECK(r.Presses(kDown, 1580, kDirs) == kDown);
    CHECK(r.Presses(0, 1600, kDirs) == 0);                 // (let go: nothing)
    CHECK(r.Presses(0, 2600, kDirs) == 0);
}

TEST_CASE("menu nav: buttons outside the repeat bits are presses only") {
    Repeat r;
    CHECK(r.Presses(kA, 0, kDirs) == kA);
    CHECK(r.Presses(kA, 1000, kDirs) == 0);   // (A held never repeats)
    CHECK(r.Presses(0, 1100, kDirs) == 0);
    CHECK(r.Presses(kA, 1200, kDirs) == kA);
}

TEST_CASE("menu nav: another direction starts its own delay, letting one of two go stops the repeat") {
    Repeat r;
    CHECK(r.Presses(kDown, 0, kDirs) == kDown);
    CHECK(r.Presses(kDown, 400, kDirs) == kDown);
    CHECK(r.Presses(kDown | kUp, 450, kDirs) == kUp);   // (Up pressed too: a press of its own)
    CHECK(r.Presses(kDown | kUp, 500, kDirs) == 0);     // (and its own delay)
    CHECK(r.Presses(kDown, 900, kDirs) == 0);           // (Up let go: Down waits for a press again)
    CHECK(r.Presses(kDown, 2000, kDirs) == 0);
    CHECK(r.Presses(0, 2100, kDirs) == 0);
    CHECK(r.Presses(kDown, 2200, kDirs) == kDown);
}

TEST_CASE("menu nav: Forget takes what is held as already pressed") {
    Repeat r;
    r.Presses(kDown, 0, kDirs);
    r.Forget(kDown | kA);   // (the pad given to another list with Down and A still held)
    CHECK(r.Presses(kDown | kA, 10, kDirs) == 0);
    CHECK(r.Presses(kDown | kA, 5000, kDirs) == 0);   // (no repeat of the old hold either)
    CHECK(r.Presses(kA, 5100, kDirs) == 0);
    CHECK(r.Presses(kA | kDown, 5200, kDirs) == kDown);
    CHECK(r.Presses(kA | kDown, 5600, kDirs) == kDown);   // (a new press repeats again)
}

TEST_CASE("menu nav: Step skips lines that cannot be picked and stops at the ends") {
    // lines 0 heading, 1, 2 note, 3, 4 heading
    const auto pick = [](int i) { return i == 1 || i == 3; };
    CHECK(Step(1, 1, 5, false, pick) == 3);    // (over the note)
    CHECK(Step(3, 1, 5, false, pick) == 3);    // (only a heading below: stays)
    CHECK(Step(1, -1, 5, false, pick) == 1);   // (only a heading above: stays)
    CHECK(Step(3, 1, 5, true, pick) == 1);     // (a fresh press on a list that goes round)
    CHECK(Step(1, -1, 5, true, pick) == 3);
    CHECK(Step(1, 1, 0, false, pick) == 1);    // (no lines)
    CHECK(Step(1, 0, 5, false, pick) == 1);
    CHECK(CanStep(1, 1, 5, pick));
    CHECK_FALSE(CanStep(3, 1, 5, pick));       // (a held Down at the last line: no repeat sent, no going round)
    const auto only = [](int i) { return i == 2; };
    CHECK(Step(2, 1, 5, true, only) == 2);     // (the one pickable line: round to itself, stays)
}

TEST_CASE("menu nav: a mask of lines, as the - page keeps them") {
    const std::uint32_t mask = 0b10110;   // lines 1, 2, 4
    CHECK(StepMask(mask, 1, 1) == 2);
    CHECK(StepMask(mask, 2, 1) == 4);
    CHECK(StepMask(mask, 4, 1) == 4);
    CHECK(StepMask(mask, 4, 1, true) == 1);
    CHECK(StepMask(mask, 1, -1) == 1);
    CHECK(StepMask(0, 0, 1) == 0);
}

TEST_CASE("menu nav: the typing keys go round on a press, stop at the ends when held, the letter wheel always goes round") {
    int key = 3;   // (4 keys: Letter 0, Delete 1, Cancel 2, OK 3)
    std::size_t letter = 0;
    CHECK_FALSE(TypingKeysStep(1, 0, true, 4, 0, 26, &key, &letter));   // (held Down at the last key: stays)
    CHECK(key == 3);
    CHECK(TypingKeysStep(1, 0, false, 4, 0, 26, &key, &letter));        // (a fresh Down: round to the first)
    CHECK(key == 0);
    CHECK_FALSE(TypingKeysStep(-1, 0, true, 4, 0, 26, &key, &letter));  // (held Up at the first: stays)
    CHECK(TypingKeysStep(-1, 0, false, 4, 0, 26, &key, &letter));       // (a fresh Up: round to the last)
    CHECK(key == 3);
    CHECK(TypingKeysStep(-1, 0, true, 4, 0, 26, &key, &letter));        // (held, not at the end: moves)
    CHECK(key == 2);
    key = 0;
    CHECK(TypingKeysStep(0, -1, true, 4, 0, 26, &key, &letter));        // (the letter wheel, held: round)
    CHECK(letter == 25);
    CHECK(TypingKeysStep(0, 1, true, 4, 0, 26, &key, &letter));
    CHECK(letter == 0);
    key = 1;
    CHECK_FALSE(TypingKeysStep(0, 1, false, 4, 0, 26, &key, &letter));  // (Left/Right off the Letter key: nothing)
    CHECK(letter == 0);
}

TEST_CASE("menu nav: another's list read at its page's refresh, trusted only while no move went to it since") {
    // (the Mods tab's list on the - page: its view is built at the refresh, never at each pad read, review-01112-h)
    ListReading r;
    CHECK_FALSE(r.Trusted(0));                       // (nothing read yet)
    CHECK_FALSE(r.CanRepeat(1, 0));
    // lines: 0 heading, 1, 2, 3 note, 4; line 2 picked; read with 5 moves sent before the view was built
    r.Fill(5, {false, true, true, false, true}, 2);
    CHECK(r.Trusted(5));
    CHECK(r.CanRepeat(1, 5));                        // (down: over the note to 4)
    CHECK(r.CanRepeat(-1, 5));                       // (up: 1)
    CHECK_FALSE(r.CanRepeat(1, 6));                  // (a move went to it since: stale, no repeat until read again)
    r.Fill(6, {false, true, true, false, true}, 4);
    CHECK_FALSE(r.CanRepeat(1, 6));                  // (at its last line: a held Down never goes round)
    CHECK(r.CanRepeat(-1, 6));
    r.Fill(6, {false, true, true, false, true}, 1);
    CHECK_FALSE(r.CanRepeat(-1, 6));                 // (at its first pickable line: the heading above is not one)
    r.Fill(7, {true, true}, -1);
    CHECK_FALSE(r.Trusted(7));                       // (no line picked)
}
