// SPDX-License-Identifier: GPL-3.0-or-later
// Row 83 / #245: the game behind another of our games asks one step more (hookshot/src/coop_back_window.h).
#include <doctest/doctest.h>

#include "coop_back_window.h"

using namespace CoopBackWindow;

namespace {
State Behind() {
    State s;
    s.front_is_ours = true;
    s.has_window = true;
    return s;
}
}  // namespace

TEST_CASE("back window: behind another game of ours: raised as the mode says") {
    CHECK(Decide(Mode::Both, Behind()) == Want{true, true});
    CHECK(Decide(Mode::Gpu, Behind()) == Want{true, false});
    CHECK(Decide(Mode::Cpu, Behind()) == Want{false, true});
    CHECK(Decide(Mode::Off, Behind()) == Want{});
}

TEST_CASE("back window: normal in front, alone, minimized or without a window") {
    for (Mode m : {Mode::Both, Mode::Gpu, Mode::Cpu}) {
        State front = Behind();
        front.in_front = true;
        CHECK(Decide(m, front) == Want{});
        State alone = Behind();
        alone.front_is_ours = false;   // a third app in front (browser, Discord), even with 2+ of our games behind it
        CHECK(Decide(m, alone) == Want{});
        State mini = Behind();
        mini.iconic = true;
        CHECK(Decide(m, mini) == Want{});
        State none = Behind();
        none.has_window = false;
        CHECK(Decide(m, none) == Want{});
    }
}

TEST_CASE("back window: two of our games, a third app in front: both normal; one of ours in front: only the other up") {
    State a, b;   // two games on one PC, both behind a browser: neither window is in front, the front is not ours
    a.has_window = b.has_window = true;
    CHECK(Decide(Mode::Both, a) == Want{});
    CHECK(Decide(Mode::Both, b) == Want{});
    a.in_front = true;          // game A comes to the front
    b.front_is_ours = true;     // so game B sees one of ours in front
    CHECK(Decide(Mode::Both, a) == Want{});
    CHECK(Decide(Mode::Both, b) == Want{true, true});
}

TEST_CASE("back window: LICHII_COOP_BACK_BOOST values") {
    CHECK(ModeFrom(nullptr) == kDefault);
    CHECK(ModeFrom("") == kDefault);
    CHECK(ModeFrom("off") == Mode::Off);
    CHECK(ModeFrom("0") == Mode::Off);
    CHECK(ModeFrom("gpu") == Mode::Gpu);
    CHECK(ModeFrom("cpu") == Mode::Cpu);
    CHECK(ModeFrom("both") == Mode::Both);
    CHECK(ModeFrom("1") == Mode::Both);
    CHECK(ModeFrom("loud") == kDefault);
}
