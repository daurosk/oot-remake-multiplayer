// Tests for the co-op HUD's place names, hearts and layout (lichii/hook/coop_hud_places.h,
// coop_hud_hearts.h, coop_hud_layout.h).
// <ostream> before doctest: it prints std::string_view, which MSVC's
// <string_view> can only do with <ostream> in.
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "coop_hud_hearts.h"
#include "coop_hud_layout.h"
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

TEST_CASE("layout: player blocks start below the game's own top-left HUD") {
    using CoopHudLayout::BlocksTop;
    using CoopHudLayout::kBlocksTop;
    using CoopHudLayout::kGameHudBottom;
    // Clear of the hearts, magic bar and d-pad item row, with room to spare.
    CHECK(kBlocksTop >= kGameHudBottom + 0.05);
    CHECK(kBlocksTop < 0.5);  // still in the top half, not over the chat
    // 16:9: a fraction of the height.
    CHECK(BlocksTop(1280, 720) == doctest::Approx(0.38 * 720));
    CHECK(BlocksTop(1920, 1080) == doctest::Approx(0.38 * 1080));
    // Ultrawide window: the 16:9 picture fills the height, pillarboxed.
    CHECK(BlocksTop(2560, 1080) == doctest::Approx(0.38 * 1080));
    // Tall window: the picture is letterboxed, the blocks move with it.
    CHECK(BlocksTop(1280, 1000) == doctest::Approx(140 + 0.38 * 720));
    CHECK(BlocksTop(0, 720) == 0.0);
}

namespace {
// A pane at (tx, ty) in the layout's units (y up from the middle) at `scale`, as the game's global matrix holds it.
CoopHudLayout::HudPiece PaneAt(float tx, float ty, float w, float h, float alpha = 1.0f, float scale = 1.0f) {
    float m[12] = {};
    m[0] = scale, m[4] = scale, m[8] = 1.0f, m[9] = tx, m[10] = ty;
    return CoopHudLayout::Centred(m, alpha, w, h);
}
// The d-pad's bottom slot (257.387 square at -1764,166 in Pa_GuideCross_00), the part at (tx, ty).
CoopHudLayout::HudPiece DpadAt(float tx, float ty, float alpha = 1.0f, float scale = 1.0f) {
    auto p = PaneAt(tx, ty, 257.387f, 257.387f, alpha, scale);
    p.x0 += -1764, p.x1 += -1764, p.y0 += 166, p.y1 += 166;
    return p;
}
// The bottom of a slot's box from the top of the 2160-unit layout, as a fraction.
double SlotBottom(double ty, double scale = 1.0) {
    return (1080.0 - (ty + scale * (166.0 - 257.387 / 2))) / 2160.0;
}
}  // namespace

TEST_CASE("layout row 141: the game's HUD bottom from its panes, the d-pad's bottom slot the lowest") {
    using CoopHudLayout::HudBottom;
    // PlayerStatus_00 as laid out with one heart row and no magic bar: hearts at -1140,896 (1300 x 212), the d-pad
    // part at 132,318: its bottom slot ends at 0.336 of the height, the hearts at 0.134.
    const auto hearts = PaneAt(-1140, 896, 1300, 212);
    const auto one_row = HudBottom({hearts, DpadAt(132, 318)});
    REQUIRE(one_row);
    CHECK(*one_row == doctest::Approx(SlotBottom(318)));
    CHECK(*one_row == doctest::Approx(0.3355).epsilon(0.002));
    // Two heart rows and the magic bar push N_HeartPos_00 / N_MpGauge_00 down: the d-pad with them. The fixed top
    // (0.38) would be over its bottom slot: the owner's screenshot. The blocks now start under it.
    const auto lower = HudBottom({hearts, PaneAt(-1560, 760, 1256, 155.2f), DpadAt(132, 180)});
    REQUIRE(lower);
    CHECK(*lower == doctest::Approx(SlotBottom(180)));
    CHECK(*lower > CoopHudLayout::kBlocksTop);
    CHECK(CoopHudLayout::TopFraction(lower) == doctest::Approx(*lower + CoopHudLayout::kGap));
    // The Stone of Agony's icon under the d-pad counts while shown, not while faded out.
    CHECK(*HudBottom({DpadAt(132, 318), PaneAt(-1632, 294, 200, 200)}) == doctest::Approx((1080.0 - 194.0) / 2160.0));
    CHECK(*HudBottom({DpadAt(132, 318), PaneAt(-1632, 294, 200, 200, 0.0f)}) == doctest::Approx(SlotBottom(318)));
    // The d-pad hidden (alpha 0): the magic bar's bottom.
    CHECK(*HudBottom({hearts, PaneAt(-1560, 760, 1256, 155.2f), DpadAt(132, 180, 0.0f)}) ==
          doctest::Approx((1080.0 - (760.0 - 77.6)) / 2160.0));
    // A smaller HUD (its scale nodes at 0.75): the slot's offset and size scale with it.
    CHECK(*HudBottom({DpadAt(100, 300, 1.0f, 0.75f)}) == doctest::Approx(SlotBottom(300, 0.75)));
}

TEST_CASE("layout row 141: no HUD reading (all faded, nothing found, a bad matrix) keeps the fixed top") {
    using CoopHudLayout::HudBottom;
    using CoopHudLayout::TopFraction;
    CHECK_FALSE(HudBottom({}));
    CHECK_FALSE(HudBottom({DpadAt(132, 318, 0.0f), PaneAt(-1140, 896, 1300, 212, 0.0f)}));   // a cutscene: faded out
    auto nan = DpadAt(132, 318);
    nan.m[10] = std::nanf("");
    CHECK_FALSE(HudBottom({nan}));
    CHECK_FALSE(HudBottom({DpadAt(132, 318, 1.0f, 0.0f)}));   // a scale of 0: not placed yet
    CHECK(TopFraction(std::nullopt) == CoopHudLayout::kBlocksTop);
    CHECK(TopFraction(0.95) == CoopHudLayout::kBlocksTop);    // not a believable HUD
    CHECK(TopFraction(0.01) == CoopHudLayout::kBlocksTop);
    CHECK(TopFraction(0.30) == doctest::Approx(0.32));
}

TEST_CASE("layout row 141: the last reading holds while the HUD cannot be read") {
    CoopHudLayout::Hold hold;
    CHECK_FALSE(hold.Update(std::nullopt));          // nothing read yet: the fixed top
    CHECK(*hold.Update(0.34) == doctest::Approx(0.34));
    CHECK(*hold.Update(std::nullopt) == doctest::Approx(0.34));   // a menu or cutscene: the blocks stay
    CHECK(*hold.Update(0.44) == doctest::Approx(0.44));           // a heart container later: the new bottom
}

TEST_CASE("layout row 141: the HUD's top in any window: 16:9, ultrawide, 4:3, small") {
    using CoopHudLayout::BlocksTop;
    const double top = 0.44 + CoopHudLayout::kGap;
    CHECK(BlocksTop(1280, 720, 0.44) == doctest::Approx(top * 720));
    CHECK(BlocksTop(1920, 1080, 0.44) == doctest::Approx(top * 1080));
    CHECK(BlocksTop(2560, 1080, 0.44) == doctest::Approx(top * 1080));          // ultrawide: pillarboxed
    CHECK(BlocksTop(1024, 768, 0.44) == doctest::Approx(96 + top * 576));       // 4:3: letterboxed
    CHECK(BlocksTop(320, 180, 0.44) == doctest::Approx(top * 180));             // a small window
    CHECK(BlocksTop(1280, 720, std::nullopt) == doctest::Approx(0.38 * 720));   // unknown: as before
}

TEST_CASE("layout row 141: blocks that would run into the chat are left out, room kept for the '+N' line") {
    using CoopHudLayout::BlocksThatFit;
    const std::vector<double> h{100, 100, 100};
    CHECK(BlocksThatFit(300, h, 700, 25) == 3);    // all fit (no '+N' line needed)
    CHECK(BlocksThatFit(300, h, 600, 25) == 3);    // ending at the floor exactly: fits
    CHECK(BlocksThatFit(300, h, 599, 25) == 2);    // 2 and the '+1' line: 525
    CHECK(BlocksThatFit(300, h, 520, 25) == 1);    // 2 + the line would pass 520
    CHECK(BlocksThatFit(300, h, 320, 25) == 0);    // only the '+3' line
    CHECK(BlocksThatFit(300, {}, 100, 25) == 0);
}

TEST_CASE("hud: the player list's size: Auto is full blocks for a few players, compact rows past them") {
    using CoopHudLayout::ListSize;
    CHECK(CoopHudLayout::ParseListSize("") == ListSize::kAuto);
    CHECK(CoopHudLayout::ParseListSize("Compact") == ListSize::kCompact);
    CHECK(CoopHudLayout::ParseListSize("full") == ListSize::kFull);
    CHECK(CoopHudLayout::ParseListSize("off") == ListSize::kOff);
    CHECK(CoopHudLayout::ParseListSize("something") == ListSize::kAuto);
    CHECK_FALSE(CoopHudLayout::CompactFor(ListSize::kAuto, CoopHudLayout::kFullMost));
    CHECK(CoopHudLayout::CompactFor(ListSize::kAuto, CoopHudLayout::kFullMost + 1));
    CHECK(CoopHudLayout::CompactFor(ListSize::kCompact, 1));
    CHECK_FALSE(CoopHudLayout::CompactFor(ListSize::kFull, 15));
    CHECK(std::string(CoopHudLayout::ListSizeName(ListSize::kCompact)) == "Compact");
}

TEST_CASE("hud: the players in our own area are listed first, the order otherwise kept") {
    std::vector<std::pair<int, bool>> players{{2, false}, {3, true}, {4, false}, {5, true}};
    CoopHudLayout::HereFirst(players, [](const auto& p) { return p.second; });
    CHECK(players[0].first == 3);
    CHECK(players[1].first == 5);
    CHECK(players[2].first == 2);
    CHECK(players[3].first == 4);
}
