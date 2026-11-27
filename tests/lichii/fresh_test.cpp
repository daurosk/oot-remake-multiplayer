// SPDX-License-Identifier: GPL-3.0-or-later
// When a reading is too old to use (lichii/hook/coop_fresh.h, hud-fresh-0121).
#include <doctest/doctest.h>

#include <cstdint>

#include "coop_fresh.h"

TEST_CASE("fresh: a game stepped a frame every 1.5 s keeps each frame's reading (testkit-0120 kit-in-play)") {
    // Written at present 100, read 1500 ms later on the next frame, before that frame's present.
    CHECK_FALSE(CoopFresh::Stale(1500, 1000, 100, 100));
    // ... or just after it (one present, the reading of that present not yet written).
    CHECK_FALSE(CoopFresh::Stale(1500, 1000, 100, 101));
    // A long hitch: no presents at all for 5 s.
    CHECK_FALSE(CoopFresh::Stale(5000, 1000, 100, 100));
}

TEST_CASE("fresh: a load or a menu (presents go on, the reading stops) drops it as before") {
    CHECK(CoopFresh::Stale(1000, 1000, 100, 160));   // a second of presents at 60 fps
    CHECK(CoopFresh::Stale(1200, 1000, 100, 102));
    CHECK_FALSE(CoopFresh::Stale(999, 1000, 100, 160));   // younger than a second: never stale
}

TEST_CASE("fresh: the present count") {
    const std::uint64_t was = CoopFresh::Presents();
    CoopFresh::Present();
    CoopFresh::Present();
    CHECK(CoopFresh::Presents() == was + 2);
    CHECK_FALSE(CoopFresh::Stale(0, 1000, was + 5, was + 2));   // (a reading "from the future": never stale)
    CHECK_FALSE(CoopFresh::Stale(5000, 1000, was + 5, was + 2));
}
