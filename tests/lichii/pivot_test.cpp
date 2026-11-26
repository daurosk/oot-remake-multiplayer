// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cmath>

#include "coop_pivot.h"

using CoopPivot::Follower;

TEST_CASE("pivot: directions are eighths of a lap, 45 degrees each (run st-pivot3: a push to yaw -45 read flag 7)") {
    CHECK(CoopPivot::YawOfDir(0) == doctest::Approx(0.0f));
    CHECK(CoopPivot::YawOfDir(7) == doctest::Approx(-45.0f));
    CHECK(CoopPivot::YawOfDir(1) == doctest::Approx(45.0f));
    CHECK(CoopPivot::YawOfDir(5) == doctest::Approx(-135.0f));
    CHECK(std::fabs(CoopPivot::YawOfDir(4)) == doctest::Approx(180.0f));
    CHECK(CoopPivot::YawOfDir(8) == doctest::Approx(0.0f));
    CHECK(CoopPivot::Short(170.0f, -170.0f) == doctest::Approx(20.0f));  // across the seam, the short way
    CHECK(CoopPivot::Short(-170.0f, 170.0f) == doctest::Approx(-20.0f));
}

TEST_CASE("pivot: another player's push turns ours the short way at the game's speed, once it has rested") {
    Follower f;
    CHECK_FALSE(f.Look(1000, 0.0f, 0));
    CHECK_FALSE(f.Look(1100, 0.0f, 7));  // the flag says one push on, but our floor was seen still only now
    const auto t = f.Look(1700, 0.0f, 7);  // still 0.6 s
    REQUIRE(t);
    CHECK(t->from == doctest::Approx(0.0f));
    CHECK(t->to == doctest::Approx(-45.0f));
    CHECK(t->ms == 1400);
    CHECK(t->At(1700 + 700) == doctest::Approx(-22.5f));
    CHECK(t->At(9000) == doctest::Approx(-45.0f));
    // While it is driven no second turn starts, and our writes are not taken for anything else.
    CHECK_FALSE(f.Look(2000, -10.0f, 7));
    CHECK(f.Driving());
    CHECK_FALSE(f.Look(3200, -45.0f, 7));  // done, at the direction
    CHECK_FALSE(f.Driving());
    CHECK_FALSE(f.Look(4000, -45.0f, 7));
    // Two pushes at once (a late flag): one turn of 90 degrees, twice as long.
    const auto two = f.Look(4100, -45.0f, 5);
    REQUIRE(two);
    CHECK(two->to == doctest::Approx(-135.0f));
    CHECK(two->ms == 2800);
}

TEST_CASE("pivot: our own player's push is never fought, however slowly it starts (run st-pivot3 v4)") {
    Follower f;
    CHECK_FALSE(f.Look(1000, 0.0f, 0));
    CHECK_FALSE(f.Look(1700, 0.0f, 0));
    // The grab: the floor creeps (0.2 degrees a look), the flag still 0: a moving floor is never turned.
    float yaw = 0.0f;
    for (std::uint64_t ms = 1733; ms < 2500; ms += 33) {
        yaw -= 0.2f;
        CHECK_FALSE(f.Look(ms, yaw, 0));
    }
    // Held part way (the player stops pushing): still, but not at a resting place.
    CHECK_FALSE(f.Look(2600, -20.0f, 0));
    CHECK_FALSE(f.Look(3600, -20.0f, 0));
    // The push ends at -45 with the flag 7: nothing to do.
    CHECK_FALSE(f.Look(4000, -45.0f, 7));
    CHECK_FALSE(f.Look(5000, -45.0f, 7));
    CHECK_FALSE(f.Driving());
}

TEST_CASE("pivot: no direction known, a new room, a broken read") {
    Follower f;
    CHECK_FALSE(f.Look(1000, 0.0f, std::nullopt));
    CHECK_FALSE(f.Look(2000, 0.0f, std::nullopt));
    CHECK_FALSE(f.Look(2100, NAN, 2));
    CHECK_FALSE(f.Look(2200, 0.0f, 2));
    REQUIRE(f.Look(2900, 0.0f, 2));
    f.Reset();
    CHECK_FALSE(f.Driving());
    CHECK_FALSE(f.Look(3000, 90.0f, 2));  // the room loaded again at the direction
    CHECK_FALSE(f.Look(4000, 90.0f, 2));
}
