// SPDX-License-Identifier: GPL-3.0-or-later
// CoopLifeGauge (row 238, hearts-238-01112-2): when to ask the game's own heart and magic gauge refresh after co-op
// wrote life, max life or magic. The refresh itself is a game call (coop_hook.cpp, AddLifeGaugeCaller).
#include <doctest/doctest.h>

#include "coop_life_gauge.h"

using CoopLifeGauge::kDelayMs;
using CoopLifeGauge::kMostWaitMs;

TEST_CASE("life gauge: the flags it shows are life, max life and magic only") {
    CHECK(CoopLifeGauge::Shown(0xfbe01da1u));  // PlayerStatus.MaxLife
    CHECK(CoopLifeGauge::Shown(0x31ab5580u));  // PlayerStatus.Life
    CHECK(CoopLifeGauge::Shown(0x12d63a97u));  // PlayerStatus.MPMax
    CHECK(CoopLifeGauge::Shown(0x76b18200u));  // PlayerStatus.MP
    CHECK_FALSE(CoopLifeGauge::Shown(0xe753508bu));  // a capacity (bombs)
    CHECK_FALSE(CoopLifeGauge::Shown(0u));
}

TEST_CASE("life gauge: asked once, a moment after the last write") {
    CoopLifeGauge::Refresher r;
    r.SetInPlay(true);
    CHECK_FALSE(r.Due(100));  // nothing wanted
    r.Want(100);
    CHECK_FALSE(r.Due(100 + kDelayMs - 1));  // the setter's queue and Link's write-back first
    r.Want(200);                             // a later write moves it
    CHECK_FALSE(r.Due(100 + kDelayMs));
    CHECK(r.Due(200 + kDelayMs));
    CHECK_FALSE(r.Due(5000));  // once
}

TEST_CASE("life gauge: off play (the title, a load) it waits until Link is back") {
    CoopLifeGauge::Refresher r;
    r.Want(100);
    CHECK_FALSE(r.Due(1000));
    r.SetInPlay(true);
    CHECK(r.Due(1000));
}

TEST_CASE("life gauge: it waits for MaxLife and our Link's max to agree, but not for ever") {
    CoopLifeGauge::Refresher r;
    r.SetInPlay(true);
    r.SetSettled(false);  // the flag lowered, Link not yet (or the other way)
    r.Want(100);
    CHECK_FALSE(r.Due(100 + kDelayMs));
    r.SetSettled(true);  // the walk lowered Link
    CHECK(r.Due(100 + kDelayMs + 16));
    r.SetSettled(false);  // they never agree (a hold the game undoes): asked anyway after the longest wait
    r.Want(10000);
    CHECK_FALSE(r.Due(10000 + kMostWaitMs - 1));
    CHECK(r.Due(10000 + kMostWaitMs));
}

TEST_CASE("life gauge: writes without end do not hold the refresh off past the longest wait") {
    CoopLifeGauge::Refresher r;
    r.SetInPlay(true);
    std::uint64_t now = 1000;
    bool asked = false;
    for (; now < 1000 + 2 * kMostWaitMs && !asked; now += 16) {
        r.Want(now);  // a write every frame (a hold the game keeps putting back)
        asked = r.Due(now);
    }
    CHECK(asked);
    CHECK(now <= 1000 + kMostWaitMs + 16);
}
