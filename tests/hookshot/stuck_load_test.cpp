// SPDX-License-Identifier: GPL-3.0-or-later
// A load that never ends (hookshot/src/coop_stuck_load.h, black-01112).
#include <doctest/doctest.h>

#include <cstdint>

#include "coop_stuck_load.h"

using namespace CoopStuckLoad;

namespace {

constexpr std::uint64_t kFrame = 16;

// Runs `ms` of frames with sample `s` from `at`; returns the steps other than none, in order, and moves `at` on.
struct Run {
    Watch w;
    std::uint64_t at = 1000;
    int stuck = 0, ended = 0, unclear = 0;
    void For(std::uint64_t ms, Sample s) {
        for (std::uint64_t end = at + ms; at < end; at += kFrame) {
            switch (w.Frame(s, at)) {
            case Step::stuck: ++stuck; break;
            case Step::ended: ++ended; break;
            case Step::unclear: ++unclear; break;
            case Step::none: break;
            }
        }
    }
};

const Sample kTitle{true, false, false};
const Sample kLoading{false, true, false};
const Sample kPlay{false, false, true};
const Sample kNoLink{false, false, false};   // a cutscene without Link, a movie, the staff roll

}  // namespace

TEST_CASE("stuck load: a scene load that never ends is stuck once (row 242: the world stays black)") {
    Run r;
    r.For(5'000, kTitle);
    r.For(kStuckMs - kFrame, kLoading);
    CHECK(r.stuck == 0);
    r.For(10 * 60'000, kLoading);   // (ten more minutes: still once)
    CHECK(r.stuck == 1);
    CHECK(r.w.Marked());
    CHECK(r.unclear == 0);
}

TEST_CASE("stuck load: normal loads (seconds) and long play never count") {
    Run r;
    r.For(5'000, kTitle);
    for (int i = 0; i < 50; ++i) {   // (50 scene changes, 40 s each at worst: a slow disk)
        r.For(40'000, kLoading);
        r.For(60'000, kPlay);
    }
    CHECK(r.stuck == 0);
    CHECK(r.ended == 0);
    CHECK(r.unclear == 0);
}

TEST_CASE("stuck load: Link-less scenes are not a load (New Game intro, Deku Tree, staff roll, title demo, collapse, "
          "warps)") {
    Run r;
    r.For(5'000, kTitle);
    r.For(3'000, kLoading);
    r.For(30 * 60'000, kNoLink);   // (half an hour of cutscene: the staff roll and more)
    CHECK(r.stuck == 0);
    CHECK(r.unclear == 1);   // (logged once, never asked about)
    r.For(1'000, kPlay);
    r.For(30 * 60'000, kNoLink);
    CHECK(r.unclear == 2);   // (once a stretch)
    CHECK(r.stuck == 0);
    CHECK_FALSE(r.w.Marked());
}

TEST_CASE("stuck load: the boot before the first title never counts (a fresh install's first start)") {
    Run r;
    r.For(10 * 60'000, kLoading);   // (the game's own start: shader caches made for minutes)
    r.For(10 * 60'000, kNoLink);    // (the boot movie)
    CHECK(r.stuck == 0);
    CHECK(r.unclear == 0);
    r.For(1'000, kTitle);
    r.For(kStuckMs + 1'000, kLoading);
    CHECK(r.stuck == 1);
}

TEST_CASE("stuck load: the title stops the time, a load that ends after all takes its record back") {
    Run r;
    r.For(1'000, kTitle);
    r.For(kStuckMs - 10'000, kLoading);
    r.For(1'000, kTitle);   // (back to the title: the time starts again)
    r.For(kStuckMs - 10'000, kLoading);
    CHECK(r.stuck == 0);
    r.For(20'000, kLoading);
    CHECK(r.stuck == 1);
    r.For(10'000, kNoLink);   // (the load ended, no Link yet: still marked)
    CHECK(r.ended == 0);
    r.For(1'000, kPlay);      // (the world came after all: a slow PC, not a stuck load)
    CHECK(r.ended == 1);
    CHECK_FALSE(r.w.Marked());
}

TEST_CASE("stuck load: quitting to the title from a black screen keeps the record") {
    Run r;
    r.For(1'000, kTitle);
    r.For(kStuckMs + 1'000, kLoading);
    REQUIRE(r.stuck == 1);
    r.For(5'000, kTitle);
    CHECK(r.w.Marked());
    CHECK(r.ended == 0);
    r.For(kStuckMs + 1'000, kLoading);   // (stuck again: the record is there already)
    CHECK(r.stuck == 1);
}

TEST_CASE("stuck load: LoadMs is the current load's running time") {
    Watch w;
    CHECK(w.Frame(kTitle, 100) == Step::none);
    CHECK(w.Frame(kLoading, 600) == Step::none);
    CHECK(w.Frame(kLoading, 900) == Step::none);
    CHECK(w.LoadMs() == 300);
    CHECK(w.Frame(kPlay, 1'000) == Step::none);
    CHECK(w.LoadMs() == 0);
}

// review-01111-a-2: the module's frame stops while the game does not run; such a gap is never a load's time.
TEST_CASE("stuck load: a pause mid-load (the emulator's pause) is not stuck") {
    Run r;
    r.For(1'000, kTitle);
    r.For(60'000, kLoading);
    r.at += 10 * 60'000;   // (paused ten minutes: no frames)
    r.For(30'000, kLoading);
    CHECK(r.stuck == 0);
    r.For(5'000, kPlay);   // (the load ends)
    CHECK(r.stuck == 0);
    CHECK(r.ended == 0);
}

TEST_CASE("stuck load: the window in the background (pause on unfocus), again and again, is not stuck") {
    Run r;
    r.For(1'000, kTitle);
    for (int i = 0; i < 20; ++i) {   // (4 s of frames, then 3 minutes away: 20 times)
        r.For(4'000, kLoading);
        r.at += 3 * 60'000;
    }
    CHECK(r.stuck == 0);   // (80 s of frames + 19 capped gaps of 1 s)
    CHECK(r.w.LoadMs() < kStuckMs);
}

TEST_CASE("stuck load: the PC asleep in a cutscene or a load is neither stuck nor unclear") {
    Run r;
    r.For(1'000, kTitle);
    r.For(30'000, kNoLink);
    r.at += 8 * 3'600'000;   // (eight hours asleep)
    r.For(30'000, kNoLink);
    CHECK(r.unclear == 0);
    r.For(30'000, kLoading);
    r.at += 8 * 3'600'000;
    r.For(30'000, kLoading);
    CHECK(r.stuck == 0);
}

TEST_CASE("stuck load: a pause does not hide a real stuck load (2 minutes of frames still count)") {
    Run r;
    r.For(1'000, kTitle);
    r.For(60'000, kLoading);
    r.at += 10 * 60'000;
    CHECK(r.stuck == 0);
    r.For(61'000, kLoading);   // (60 s + 1 s capped gap + 61 s: over 2 minutes of the game running)
    CHECK(r.stuck == 1);
}

TEST_CASE("stuck load: a clock that goes back adds nothing") {
    Watch w;
    CHECK(w.Frame(kTitle, 5'000) == Step::none);
    CHECK(w.Frame(kLoading, 6'000) == Step::none);
    CHECK(w.Frame(kLoading, 1'000) == Step::none);
    CHECK(w.LoadMs() == 0);
}
