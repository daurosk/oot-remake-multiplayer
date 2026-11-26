// SPDX-License-Identifier: GPL-3.0-or-later
// Row 83 / #245: the game behind another of our games asks one step more (hookshot/src/coop_back_window.h).
#include <doctest/doctest.h>

#include "coop_back_window.h"

using namespace CoopBackWindow;

namespace {
State Behind() {
    State s;
    s.other_game = true;
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
        alone.other_game = false;   // a solo game behind a browser takes nothing from the window in front
        CHECK(Decide(m, alone) == Want{});
        State mini = Behind();
        mini.iconic = true;
        CHECK(Decide(m, mini) == Want{});
        State none = Behind();
        none.has_window = false;
        CHECK(Decide(m, none) == Want{});
    }
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
