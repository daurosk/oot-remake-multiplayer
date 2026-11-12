// Tests for the co-op HUD's place names and hearts (lichii/hook/coop_hud_places.h,
// coop_hud_hearts.h).
// <ostream> before doctest: it prints std::string_view, which MSVC's
// <string_view> can only do with <ostream> in.
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "coop_hud_hearts.h"
#include "coop_hud_places.h"

using CoopHudHearts::Quarters;
using CoopHudPlaces::Name;
using CoopHudPlaces::Parse;
using CoopHudPlaces::Readable;
using CoopHudPlaces::SceneBase;

TEST_CASE("places.json: a flat object of strings") {
    const auto p = Parse("\xEF\xBB\xBF{\n \"KokiriForest\": \"Kokiri Forest\",\n"
                         " \"Market\": \"Castle \\\"Town\\\" \\u00e9\\ud83d\\ude00\"\n}\n");
    REQUIRE(p);
    CHECK(p->size() == 2);
    CHECK(p->at("KokiriForest") == "Kokiri Forest");
    CHECK(p->at("Market") == "Castle \"Town\" \xC3\xA9\xF0\x9F\x98\x80");
    CHECK(Parse("{}"));
    CHECK_FALSE(Parse(""));
    CHECK_FALSE(Parse("[]"));
    CHECK_FALSE(Parse("{\"a\": 1}"));
    CHECK_FALSE(Parse("{\"a\": \"b\""));
    CHECK_FALSE(Parse("{\"a\" \"b\"}"));
}

TEST_CASE("place name: the game's name by scene, then by its base, then readable") {
    const CoopHudPlaces::Places p{{"KokiriForest", "Kokiri Forest"}, {"GohmaStage", "Hall"}};
    CHECK(SceneBase("KokiriForest_Child") == "KokiriForest");
    CHECK(SceneBase("KakarikoVillage_Adult_Alternative") == "KakarikoVillage");
    CHECK(SceneBase("ZoraDomain_Alternative") == "ZoraDomain");
    CHECK(SceneBase("_Child") == "_Child");
    CHECK(Name(p, "KokiriForest_Adult") == "Kokiri Forest");
    CHECK(Name(p, "GohmaStage") == "Hall");
    CHECK(Name(p, "SageRoom") == "Sage Room");
    CHECK(Name({}, "DeathMountainTrail_Child") == "Death Mountain Trail");
    CHECK(Readable("Demo_ShootingLocation") == "Demo Shooting Location");
}

TEST_CASE("hearts: quarters per heart, rounded up, clamped") {
    CHECK(Quarters(12, 12) == std::vector<int>{4, 4, 4});
    CHECK(Quarters(9, 12) == std::vector<int>{4, 4, 1});
    CHECK(Quarters(0, 12) == std::vector<int>{0, 0, 0});
    CHECK(Quarters(20, 12) == std::vector<int>{4, 4, 4});
    CHECK(Quarters(-3, 8) == std::vector<int>{0, 0});
    CHECK(Quarters(5, 14) == std::vector<int>{4, 1, 0, 0});
    CHECK(Quarters(5, 0).empty());
    CHECK(Quarters(9999, 9999).size() == CoopHudHearts::kMostHearts);
}
