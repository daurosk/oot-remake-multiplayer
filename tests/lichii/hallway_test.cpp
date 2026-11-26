// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "coop_hallway.h"

namespace {

using CoopHallway::Other;
using CoopHallway::Vec;

constexpr Vec kWestInside{-56.4f, 124.5f, -30.0f};
constexpr Vec kEastInside{56.6f, 124.5f, -30.0f};
constexpr Vec kWestEye{-56.5f, 124.5f, -6.0f};  // in front of the west door, shooting up at its eye
constexpr Vec kEastEye{56.5f, 124.5f, -6.0f};

Other In(std::uint8_t number, const char* name, const Vec& at, bool can_move = true) {
    Other o;
    o.number = number;
    o.name = name;
    o.position = at;
    o.scene = std::string(CoopHallway::kScene);
    o.can_move = can_move;
    return o;
}

} // namespace

TEST_CASE("hallway: the two hallways' boxes, and nothing outside the Forest Temple") {
    CHECK(CoopHallway::HallwayAt("ForestTemple", kWestInside) == 0);
    CHECK(CoopHallway::HallwayAt("ForestTemple", kEastInside) == 1);
    CHECK(CoopHallway::HallwayAt("ForestTemple", Vec{-56.4f, 127.5f, -50.0f}) == 0);  // turned floor, far end
    CHECK_FALSE(CoopHallway::HallwayAt("ForestTemple", kWestEye));                    // before its door
    CHECK_FALSE(CoopHallway::HallwayAt("ForestTemple", Vec{-56.4f, 124.5f, -60.0f}));  // the room beyond
    CHECK_FALSE(CoopHallway::HallwayAt("ForestTemple", Vec{0.0f, 124.5f, -30.0f}));    // the main hall
    CHECK_FALSE(CoopHallway::HallwayAt("WaterTemple", kWestInside));
    CHECK(CoopHallway::HallwayOfShooter(kWestEye) == 0);
    CHECK(CoopHallway::HallwayOfShooter(kEastEye) == 1);
    CHECK(CoopHallway::IsTurn("ForestTemple_Room11_RoomSpin"));  // what the eye asks
    CHECK(CoopHallway::IsTurn("ForestTemple_Room22_RoomSpin"));
    CHECK(CoopHallway::IsTurn("Dm_ForestTemple_0005"));
    CHECK(CoopHallway::IsTurn("Dm_ForestTemple_0007"));
    CHECK_FALSE(CoopHallway::IsTurn("Dm_ForestTemple_0010"));
}

TEST_CASE("hallway: a turn is refused while a player who can move is in that hallway") {
    const std::vector<Other> saria{In(2, "Saria", kWestInside)};
    auto v = CoopHallway::Decide("ForestTemple_Room11_RoomSpin", "ForestTemple", kWestEye, saria);
    CHECK(v.refuse);
    CHECK(v.hallway == 0);
    REQUIRE(v.who.size() == 1);
    CHECK(v.who[0] == "Saria");
    CHECK(CoopHallway::OurLine(v) == "The west hallway can't turn: Saria is in it.");
    CHECK(CoopHallway::TheirLine(v) == "Saria, step out of the west hallway: I want to turn it.");

    // The other hallway's eye: Saria is not in that one.
    CHECK_FALSE(CoopHallway::Decide("Dm_ForestTemple_0007", "ForestTemple", kEastEye, saria).refuse);
    // Nobody inside: turns.
    CHECK_FALSE(CoopHallway::Decide("Dm_ForestTemple_0005", "ForestTemple", kWestEye,
                                    std::vector<Other>{In(2, "Saria", kWestEye)})
                    .refuse);
    // Not a turn, or not the Forest Temple: never refused.
    CHECK_FALSE(CoopHallway::Decide("Dm_ForestTemple_0010", "ForestTemple", kWestEye, saria).refuse);
    CHECK_FALSE(CoopHallway::Decide("Dm_ForestTemple_0005", "WaterTemple", kWestEye, saria).refuse);
}

TEST_CASE("hallway: a player who cannot move never blocks it, two inside are both named") {
    // Down, spectating or away (paused, in a menu, loading...): the turn goes on.
    CHECK_FALSE(CoopHallway::Decide("Dm_ForestTemple_0005", "ForestTemple", kWestEye,
                                    std::vector<Other>{In(2, "Saria", kWestInside, false)})
                    .refuse);
    // Another scene's player with the same coordinates: not in this hallway.
    Other far = In(3, "Mido", kWestInside);
    far.scene = "KokiriForest";
    CHECK_FALSE(CoopHallway::Decide("Dm_ForestTemple_0005", "ForestTemple", kWestEye, std::vector<Other>{far}).refuse);
    // Two inside, by number; one with no name yet.
    const auto v = CoopHallway::Decide("Dm_ForestTemple_0005", "ForestTemple", kWestEye,
                                       std::vector<Other>{In(4, "", kWestInside), In(2, "Saria", kWestInside)});
    REQUIRE(v.refuse);
    CHECK(CoopHallway::OurLine(v) == "The west hallway can't turn: Saria and player 4 are in it.");
}

TEST_CASE("hallway: the lines are said once every 4 s per hallway") {
    CoopHallway::Lines lines;
    const auto v = CoopHallway::Decide("Dm_ForestTemple_0005", "ForestTemple", kWestEye,
                                       std::vector<Other>{In(2, "Saria", kWestInside)});
    lines.Refused(v, 1'000'000);
    lines.Refused(v, 2'000'000);  // shot again at once: not said again
    CHECK(lines.TakeOurs().size() == 1);
    CHECK(lines.TakeChats().size() == 1);
    lines.Refused(v, 5'100'000);
    CHECK(lines.TakeOurs().size() == 1);
    CoopHallway::Verdict open;  // no refusal: nothing said
    lines.Refused(open, 20'000'000);
    CHECK(lines.TakeOurs().empty());
}
