// Tests for the dungeon rooms (lichii/hook/coop_rooms.cpp): which room of a
// dungeon a position is in (issue 145). Synthetic points only: the real
// table is made from the player's own game with the puppet pack.
#include <doctest/doctest.h>

#include <vector>

#include "coop_rooms.h"

namespace {

using CoopRooms::Point;
using CoopRooms::Table;
using CoopRooms::Tracker;

// Two rooms side by side on one floor, a third right above room 1.
Table Two() {
    return Table(std::vector<Point>{
        {"D", 1, 0.0f, 0.0f, 0.0f},
        {"D", 1, 4.0f, 0.0f, 0.0f},
        {"D", 2, 10.0f, 0.0f, 0.0f},
        {"D", 3, 0.0f, 6.0f, 0.0f},
        {"E", 7, 0.0f, 0.0f, 0.0f},
    });
}

}  // namespace

TEST_CASE("rooms: Parse reads 'scene room x y z' lines, skips comments and bad lines") {
    int bad = -1;
    const auto pts = CoopRooms::Parse(
        "# a comment\n"
        "D 1 0.5 -2 3.25\n"
        "\n"
        "E 7 1 2 3   # trailing comment\r\n"
        "D x 1 2 3\n"
        "D 1 1 2\n"
        "D 1 1 2 3 4\n"
        "D -1 1 2 3\n"
        "F 0 4 5 6",
        &bad);
    REQUIRE(pts.size() == 3);
    CHECK(pts[0].scene == "D");
    CHECK(pts[0].room == 1);
    CHECK(pts[0].x == doctest::Approx(0.5f));
    CHECK(pts[0].y == doctest::Approx(-2.0f));
    CHECK(pts[0].z == doctest::Approx(3.25f));
    CHECK(pts[1].scene == "E");
    CHECK(pts[2].scene == "F");
    CHECK(pts[2].z == doctest::Approx(6.0f));
    CHECK(bad == 4);
    const Table t(pts);
    CHECK(t.size() == 3);
    CHECK(t.Has("E"));
    CHECK_FALSE(t.Has("G"));
    CHECK(Table().empty());
}

TEST_CASE("rooms: the nearest placement's room, height counted twice, nothing far off") {
    const Table t = Two();
    const auto g = t.Nearest("D", {1, 0, 0});
    REQUIRE(g);
    CHECK(g->room == 1);
    CHECK(g->distance == doctest::Approx(1.0f));
    CHECK(t.Nearest("D", {9, 0, 0})->room == 2);
    // 2.5 up from room 1 is 5 weighted; 3.5 below room 3 is 7: still room 1.
    CHECK(t.Nearest("D", {0, 2.5f, 0})->room == 1);
    CHECK(t.Nearest("D", {0, 3.5f, 0})->room == 3);
    // Another scene's points do not count; a scene with none is not known.
    CHECK(t.Nearest("E", {1, 0, 0})->room == 7);
    CHECK_FALSE(t.Nearest("KakarikoVillage", {0, 0, 0}));
    CHECK_FALSE(t.Nearest("D", {40, 0, 0}));
    float to = 0.0f;
    t.Nearest("D", {6, 0, 0}, 2, &to);
    CHECK(to == doctest::Approx(4.0f));
}

TEST_CASE("rooms: a tracked player changes room only once clearly in the new one") {
    Tracker t(Two());
    CHECK(t.Update(1, "D", {4, 0, 0}) == 1);
    // At 7.5 room 2 is nearer (2.5 against 3.5), but by less than kMargin.
    CHECK(t.Update(1, "D", {7.5f, 0, 0}) == 1);
    CHECK(t.Update(1, "D", {8.5f, 0, 0}) == 2);  // 1.5 against 4.5
    CHECK(t.Update(1, "D", {6.5f, 0, 0}) == 2);  // back in the door: stays
    // Another key has its own room; a fresh one takes the nearest at once.
    CHECK(t.Update(2, "D", {6.5f, 0, 0}) == 1);
    // Out of every room, or in another scene: forgotten, then fresh.
    CHECK_FALSE(t.Update(1, "D", {60, 0, 0}));
    CHECK(t.Update(1, "D", {6.5f, 0, 0}) == 1);
    CHECK(t.Update(1, "E", {0, 0, 0}) == 7);
    // ForgetUnless drops the keys not kept: key 2 is fresh again.
    CHECK(t.Update(2, "D", {8.5f, 0, 0}) == 2);
    t.ForgetUnless([](std::uint32_t key) { return key == 1; });
    CHECK(t.Update(2, "D", {6.5f, 0, 0}) == 1);
}
