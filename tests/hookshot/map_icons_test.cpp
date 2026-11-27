// SPDX-License-Identifier: GPL-3.0-or-later
// The others on the game's own map icons (hookshot/src/coop_map_icons.h): the clip's inverse, the plan, and the steps
// against a made-up game memory (the UI manager, the map screen, its layout, the copies and their part layouts).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "coop_map_icons.h"
#include "coop_native_ui.h"

namespace {

namespace M = CoopMapIcons;
namespace T = CoopNativeTags;
constexpr std::uint64_t kBase = 0x80020000;
// The pause tab's route (coop_pause_tab.h): kPaneTable (layout + 0x40) -> table, kPaneByName (table, name) -> own pane,
// kPaneText (layout, "part/pane", &ref, 1, 0).
constexpr std::uint64_t kPaneTable = 0x43c040, kOwnPane = 0x43e300, kPathText = 0xa67d30;

struct FakeGame final : T::Game {
    // A pretend game in play: its play UI is up, so its listed screens are live (CoopNativeUi::ScreenLive, which
    // ScreenByVtable asks since party-hud-crash-c126; as native_tags_test.cpp's FakeGame).
    FakeGame() {
        CoopNativeUi::SetSceneLoading(false);
        CoopNativeUi::SetRunningScreens(std::vector<std::uint64_t>{}, true);
    }
    std::map<std::uint64_t, std::uint8_t> mem;
    std::map<std::pair<std::uint64_t, std::string>, std::uint64_t> panes;   // (layout, name) -> pane
    std::map<std::uint64_t, int> shown;                                     // pane -> last kPaneShow
    std::vector<std::pair<std::string, std::string>> texts;                 // (path, text) set
    std::map<std::uint64_t, std::uint64_t> tables;                          // layout -> its pane table
    std::vector<std::uint64_t> touched;                                     // panes shown, hidden or marked
    std::uint64_t next = 0xc0000000;

    std::uint64_t MainBase() const override { return kBase; }
    bool Read(std::uint64_t a, void* out, std::size_t n) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = mem.find(a + i);
            o[i] = it == mem.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(std::uint64_t a, const void* in, std::size_t n) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < n; ++i) mem[a + i] = p[i];
        return true;
    }
    std::string Str(std::uint64_t a) {
        std::string s;
        for (char ch; Read(a, &ch, 1) && ch != 0; ++a) s.push_back(ch);
        return s;
    }
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x) override {
        if (f == kBase + T::addr::kMalloc) return 0x90000000;
        if (f == kBase + T::addr::kPaneByName) {
            const auto it = panes.find({x[0], Str(x[1])});
            return it == panes.end() ? 0 : it->second;
        }
        if (f == kBase + kPaneTable) {
            const auto it = tables.find(x[0] - 0x40);
            return it == tables.end() ? 0 : it->second;
        }
        if (f == kBase + kOwnPane) {
            const auto it = panes.find({x[0], Str(x[1])});
            return it == panes.end() ? 0 : it->second;
        }
        if (f == kBase + T::addr::kPaneShow) shown[x[0]] = static_cast<int>(x[1]);
        if (f == kBase + T::addr::kPaneShow || f == kBase + T::addr::kPaneMarkB) touched.push_back(x[0]);
        if (f == kBase + kPathText) {
            std::uint64_t text = 0;
            Read(x[2], &text, 8);
            texts.emplace_back(Str(x[1]), Str(text));
        }
        return 0;
    }
    template <typename V>
    void Put(std::uint64_t a, V v) { Write(a, &v, sizeof v); }
    std::uint64_t Pane(std::uint64_t layout, const std::string& name) {
        const std::uint64_t p = next += 0x1000;
        panes[{layout, name}] = p;
        return p;
    }
    // A pane table with an empty parts list (sentinel +0x128, first +0x130).
    std::uint64_t Table() {
        const std::uint64_t t = next += 0x1000;
        Put<std::uint64_t>(t + 0x130, t + 0x128);
        return t;
    }
    // The part pane `name` in `table` (its own pane) and its layout: a node of the table's parts list (next +8, the
    // part's name at +0x148), itself a pane table.
    std::uint64_t Part(std::uint64_t table, const std::string& name) {
        Pane(table, name);
        const std::uint64_t node = Table(), text = next += 0x1000;
        Write(text, name.c_str(), name.size() + 1);
        Put<std::uint64_t>(node + 0x148, text);
        std::uint64_t first = 0;
        Read(table + 0x130, &first, 8);
        Put<std::uint64_t>(node + 8, first);
        Put<std::uint64_t>(table + 0x130, node);
        return node;
    }
};

constexpr std::uint64_t kScreen = 0xa0002000, kLayout = 0xa0008000;

// The map screen with its layout, the clip at the identity (the layout's middle) and `copies` copies made by the pack.
void SetUp(FakeGame& g, std::size_t copies, std::uint64_t layout = kLayout) {
    g.Put<std::uint64_t>(kBase + T::addr::kUiManagerGot, 0xa0000f00);
    g.Put<std::uint64_t>(0xa0000f00, 0xa0000000);
    g.Put<std::uint32_t>(0xa0000000 + T::addr::kUiCount, 1);
    g.Put<std::uint64_t>(0xa0000000 + T::addr::kUiScreens, 0xa0001000);
    g.Put<std::uint64_t>(0xa0001000, kScreen);
    g.Put<std::uint64_t>(kScreen, kBase + T::kMapMenuVtable);
    g.Put<std::uint64_t>(kScreen + T::addr::kScreenLayout, layout);
    const std::uint64_t clip = g.Pane(layout, M::kClipPane);
    const std::uint64_t table = g.tables[layout] = g.Table();
    const std::uint64_t game_icon = g.Part(table, "Pa_MapIcon_00");
    g.Pane(g.Part(game_icon, "Pa_Player_00"), "N_PlayerRotate_00");
    const float id[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    g.Write(clip + M::kPaneMatrix, id, sizeof id);
    for (std::size_t i = 0; i < copies; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, M::kCopyName, static_cast<int>(i));
        const std::uint64_t icon = g.Part(table, name);
        for (const char* n : {"N_Door_00", "N_Player_00", "N_Entrance_00", "N_Ruto_00"}) g.Pane(icon, n);
        const std::uint64_t player = g.Part(icon, "Pa_Player_00");
        g.Pane(player, "N_PlayerRotate_00");
        g.Pane(player, "N_Type_01");
        const std::uint64_t entrance = g.Part(icon, "Pa_Entrance_00");
        g.Pane(entrance, "T_Location_00");
        g.Pane(entrance, "N_Mini_00");
    }
}

M::Mark At(std::uint8_t n, float x, float y, std::string name = "") {
    M::Mark m;
    m.number = n, m.x = x, m.y = y, m.name = std::move(name);
    return m;
}

}  // namespace

TEST_CASE("map icons: the clip's inverse takes a layout point into its frame (scaled, moved, turned)") {
    const float id[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    auto inv = M::Inverse(id);
    REQUIRE(inv);
    auto p = M::ToLocal(*inv, 1920, 1080);   // the layout's middle
    CHECK(p[0] == doctest::Approx(0.0f));
    CHECK(p[1] == doctest::Approx(0.0f));
    p = M::ToLocal(*inv, 2020, 980);         // right 100, up 100 (y down in, y up out)
    CHECK(p[0] == doctest::Approx(100.0f));
    CHECK(p[1] == doctest::Approx(100.0f));
    // scale 2 at (1536, -690) (the minimap's corner): a point there is the frame's 0, 100 further is 50
    const float sm[12] = {2, 0, 0, 0, 2, 0, 0, 0, 1, 1536, -690, 0};
    inv = M::Inverse(sm);
    REQUIRE(inv);
    p = M::ToLocal(*inv, 1920 + 1536 + 100, 1080 + 690);
    CHECK(p[0] == doctest::Approx(50.0f));
    CHECK(p[1] == doctest::Approx(0.0f));
    // turned a quarter counter-clockwise: the layout's up is the frame's right
    const float tm[12] = {0, 1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0};
    inv = M::Inverse(tm);
    REQUIRE(inv);
    p = M::ToLocal(*inv, 1920, 1080 - 100);
    CHECK(p[0] == doctest::Approx(100.0f));
    CHECK(p[1] == doctest::Approx(0.0f));
    const float zero[12] = {};
    CHECK_FALSE(M::Inverse(zero));
    float nan[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    nan[9] = std::nanf("");
    CHECK_FALSE(M::Inverse(nan));
}

TEST_CASE("map icons: one copy per player, at most 15, past the edge left out, turned their way, P<n> unnamed") {
    const float id[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const auto inv = *M::Inverse(id);
    M::Frame f;
    for (int i = 1; i <= 17; ++i) f.marks.push_back(At(static_cast<std::uint8_t>(i), 100.0f * i, 100.0f, "N" + std::to_string(i)));
    f.marks[0].clamped = true;                 // past the map's edge: the game's clip would cut it
    f.marks[1].angle = -1.57079632679f;        // facing up the screen: not turned
    f.marks[2].angle = 0.0f;                   // facing right: a quarter clockwise (-pi/2)
    f.marks[3].name.clear();
    f.marks[4].floor_delta = 1;                // another floor: the name, no arrow
    const auto plan = M::Plan(f, inv);
    REQUIRE(plan.size() == M::kCopies);
    CHECK(plan[0].name == "N2");
    CHECK(plan[0].copy == 0);
    REQUIRE(plan[0].turn);
    CHECK(*plan[0].turn == doctest::Approx(0.0f));
    REQUIRE(plan[1].turn);
    CHECK(*plan[1].turn == doctest::Approx(-1.57079632679f));
    CHECK(plan[2].name == "P4");
    CHECK_FALSE(plan[3].arrow);
    CHECK(plan[4].arrow);
    CHECK_FALSE(plan[4].turn);                 // no facing known: not turned
    CHECK(plan[14].name == "N16");             // the 17th has no copy
    CHECK(plan[1].at[0] == doctest::Approx(300.0f - 1920.0f));
    CHECK(plan[1].at[1] == doctest::Approx(1080.0f - 100.0f));
    CHECK(M::Plan(M::Frame{}, inv).empty());   // solo: nothing
}

TEST_CASE("map icons: overlapping arrows show only the name of the one nearer to us") {
    const float id[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const auto inv = *M::Inverse(id);
    M::Frame f;
    f.marks = {At(2, 1000, 1000, "far"), At(3, 1010, 1000, "near"), At(4, 1500, 1000, "alone")};
    f.self = std::array<float, 2>{1100.0f, 1000.0f};
    auto plan = M::Plan(f, inv);
    REQUIRE(plan.size() == 3);
    CHECK_FALSE(plan[0].name_shown);
    CHECK(plan[1].name_shown);
    CHECK(plan[2].name_shown);
    f.self.reset();   // our place not known: the lower number keeps its name
    plan = M::Plan(f, inv);
    CHECK(plan[0].name_shown);
    CHECK_FALSE(plan[1].name_shown);
}

TEST_CASE("map icons: the steps show a copy per player with the name on its label, hide the rest, then hide all") {
    FakeGame g;
    SetUp(g, M::kCopies);
    M::Icons icons;
    M::Frame f;
    f.marks = {At(2, 2020, 980, "Saria"), At(3, 1000, 500, "Mido")};
    REQUIRE(icons.Step(g, f));
    CHECK(icons.Up());
    CHECK(icons.shown() == 2);
    const std::uint64_t c0 = icons.copy(0), c1 = icons.copy(1);
    CHECK(g.shown[c0] == 1);
    CHECK(g.shown[c1] == 1);
    float xy[2] = {};
    g.Read(c0 + T::addr::kPaneXY, xy, sizeof xy);
    CHECK(xy[0] == doctest::Approx(100.0f));
    CHECK(xy[1] == doctest::Approx(100.0f));
    REQUIRE(g.texts.size() == 2);
    CHECK(g.texts[0].second == "Saria");
    CHECK(g.texts[0].first == "Pa_CoopMapIcon_00/Pa_Entrance_00/T_Location_00");
    CHECK(g.texts[1].second == "Mido");
    // the arrow turned at kPaneTurn (no facing given here: not turned); the state says what is shown
    const std::string js = M::Json(icons.Snapshot(g));
    CHECK(js.find("\"up\":true") != std::string::npos);
    CHECK(js.find("\"game_arrow\":true") != std::string::npos);
    CHECK(js.find("\"name\":\"Saria\",\"name_set\":true") != std::string::npos);
    // the icon types not ours hidden (in the two copies used), the arrow and the label shown, the other look hidden
    int doors = 0, players = 0, minis = 0;
    for (const auto& [key, pane] : g.panes) {
        const auto it = g.shown.find(pane);
        if (it == g.shown.end()) continue;
        doors += key.second == "N_Door_00" && it->second == 0;
        players += key.second == "N_Player_00" && it->second == 1;
        minis += key.second == "N_Mini_00" && it->second == 0;
    }
    CHECK(doors == 2);
    CHECK(players == 2);
    CHECK(minis == 2);
    // Mido leaves: his copy is hidden again, Saria's name is not set again
    f.marks.pop_back();
    REQUIRE(icons.Step(g, f));
    CHECK(icons.shown() == 1);
    CHECK(g.shown[c1] == 0);
    CHECK(g.texts.size() == 2);
    icons.HideAll(g);
    CHECK(g.shown[c0] == 0);
    CHECK(icons.shown() == 0);
}

TEST_CASE("map icons: a game without the pack's copies has nothing touched and says so") {
    FakeGame g;
    SetUp(g, 0);
    M::Icons icons;
    M::Frame f;
    f.marks = {At(2, 2020, 980, "Saria")};
    CHECK_FALSE(icons.Step(g, f));
    CHECK_FALSE(icons.Up());
    CHECK(icons.why().find("Pa_CoopMapIcon_00") != std::string::npos);
    CHECK(g.shown.empty());
    CHECK(g.texts.empty());
}

TEST_CASE("map icons: a facing turns the copy's arrow at kPaneTurn, read back in the state") {
    FakeGame g;
    SetUp(g, 1);
    M::Icons icons;
    icons.TurnAt(0x68);   // (the value under trial: LICHII_COOP_MAP_TURN_AT=68; kPaneTurn stays 0 until proven)
    M::Frame f;
    f.marks = {At(2, 2020, 980, "Saria")};
    f.marks[0].angle = 0.0f;   // facing right on the screen: a quarter turn clockwise from up
    REQUIRE(icons.Step(g, f));
    const M::State st = icons.Snapshot(g);
    REQUIRE(st.shown.size() == 1);
    REQUIRE(st.shown[0].turn.has_value());
    REQUIRE(st.shown[0].wrote_turn.has_value());
    CHECK(*st.shown[0].turn == doctest::Approx(*st.shown[0].wrote_turn));
    CHECK(*st.shown[0].turn == doctest::Approx(-1.5707963f));
    CHECK(st.turn_at == 0x68);
}

TEST_CASE("map icons: no turn place known (kPaneTurn 0, or TURN_AT=0): the arrow's turn is never written") {
    FakeGame g;
    SetUp(g, 1);
    M::Icons icons;
    CHECK(M::kPaneTurn == 0);
    M::Frame f;
    f.marks = {At(2, 2020, 980, "Saria")};
    f.marks[0].angle = 0.0f;
    REQUIRE(icons.Step(g, f));
    M::State st = icons.Snapshot(g);
    REQUIRE(st.shown.size() == 1);
    CHECK_FALSE(st.shown[0].wrote_turn.has_value());
    icons.TurnAt(0x68);
    icons.TurnAt(0);      // (LICHII_COOP_MAP_TURN_AT=0 switches a trial off again)
    REQUIRE(icons.Step(g, f));
    st = icons.Snapshot(g);
    CHECK_FALSE(st.shown[0].wrote_turn.has_value());
}

TEST_CASE("map icons: a new layout at the same screen address (a load) forgets the old panes, no call into them") {
    FakeGame g;
    SetUp(g, 2);
    M::Icons icons;
    M::Frame f;
    f.marks = {At(2, 2020, 980, "Saria")};
    REQUIRE(icons.Step(g, f));
    const std::uint64_t old_copy = icons.copy(0);
    std::vector<std::uint64_t> old_panes;
    for (const auto& [key, pane] : g.panes) old_panes.push_back(pane);
    // the load: the screen's memory reused by the new screen, with a new layout and new panes
    SetUp(g, 2, 0xa0400000);
    g.touched.clear();
    REQUIRE(icons.Step(g, f));
    CHECK(icons.copy(0) != old_copy);
    for (const std::uint64_t t : g.touched) {
        CHECK(std::find(old_panes.begin(), old_panes.end(), t) == old_panes.end());
    }
    // Forget (the glue calls it at a load) drops everything with no game call
    g.touched.clear();
    icons.Forget();
    CHECK(icons.shown() == 0);
    CHECK(g.touched.empty());
}

// A layout object at `at` named `name` as LayoutName reads it (layout+0xf8 -> resource, +0x18 -> the name).
static void NamedLayout(FakeGame& g, std::uint64_t at, const std::string& name) {
    const std::uint64_t res = g.next += 0x1000, text = g.next += 0x1000;
    g.Write(text, name.c_str(), name.size() + 1);
    g.Put<std::uint64_t>(res + 0x18, text);
    g.Put<std::uint64_t>(at + 0xf8, res);
}

TEST_CASE("map icons: the turn probe reads the game's own arrows fresh, solo, with nothing shown, written or kept") {
    FakeGame g;
    SetUp(g, 0);   // (no pack copies needed: the probe reads the game's own Pa_MapIcon_00)
    // the template part's arrow: words +0x60.. = 0, 1, 2, ...
    const std::uint64_t table = g.tables[kLayout];
    std::uint64_t icon = 0, player = 0, name_at = 0;
    g.Read(table + 0x130, &icon, 8);   // (no copies: the parts list holds Pa_MapIcon_00 only, its own Pa_Player_00)
    g.Read(icon + 0x148, &name_at, 8);
    REQUIRE(g.Str(name_at) == "Pa_MapIcon_00");
    g.Read(icon + 0x130, &player, 8);
    const std::uint64_t turn = g.panes[{player, "N_PlayerRotate_00"}];
    REQUIRE(turn != 0);
    for (int i = 0; i < 8; ++i) g.Put<float>(turn + 0x60 + 4 * i, static_cast<float>(i));
    // one run-time copy held by the screen: a part object whose layout (+0x40) is a PaMapIcon_00, holding a part
    // object whose layout is a PaMapIconPlayer_00 with its own N_PlayerRotate_00 (+0x68 = 1.5)
    const std::uint64_t run = 0xb0000000, run_player = 0xb0010000;
    NamedLayout(g, run + 0x40, "PaMapIcon_00");
    NamedLayout(g, run_player + 0x40, "PaMapIconPlayer_00");
    g.Put<std::uint64_t>(run + 0x200, run_player);
    g.Put<std::uint64_t>(kScreen + 0x100, run);
    const std::uint64_t run_turn = g.Pane(run_player + 0x40, "N_PlayerRotate_00");
    g.Put<float>(run_turn + 0x68, 1.5f);
    g.Put<float>(run_turn + 0x28, 0.5f);    // probe v2: the copy's global matrix m[0], m[1] (a turned arrow)
    g.Put<float>(run_turn + 0x2c, -0.25f);
    g.Put<float>(run_turn + 0xbc, 9.0f);    // the last raw word
    M::Icons icons;
    const M::State st = icons.ProbeArrow(g);
    CHECK(st.probe);
    CHECK(st.table);
    CHECK(st.game_arrow);
    CHECK(st.game_arrow_words[2] == doctest::Approx(2.0f));
    REQUIRE(st.run_arrow_words.size() == 1);
    CHECK(st.run_arrow_words[0][2] == doctest::Approx(1.5f));
    // probe v2: raw +0x28..+0xbc, [0] the template's, [1] the copy's; +0x60 is raw index 14
    REQUIRE(st.arrow_raw.size() == 2);
    CHECK(st.arrow_raw[0][14 + 2] == doctest::Approx(2.0f));
    CHECK(st.arrow_raw[1][0] == doctest::Approx(0.5f));
    CHECK(st.arrow_raw[1][1] == doctest::Approx(-0.25f));
    CHECK(st.arrow_raw[1][16] == doctest::Approx(1.5f));
    CHECK(st.arrow_raw[1][M::State::kRawWords - 1] == doctest::Approx(9.0f));
    CHECK(st.shown.empty());
    CHECK(g.touched.empty());
    CHECK(g.texts.empty());
    CHECK_FALSE(icons.Up());
    CHECK(icons.shown() == 0);
    const std::string js = M::Json(st);
    CHECK(js.find("\"probe\":true") != std::string::npos);
    CHECK(js.find("\"run_arrow_words\":[[0.0000,0.0000,1.5000,") != std::string::npos);
    CHECK(js.find("\"arrow_raw\":[[") != std::string::npos);
    CHECK(js.find(",[0.5000,-0.2500,") != std::string::npos);
    // no map screen: says so, reads nothing more
    g.Put<std::uint64_t>(kScreen, 0);
    CHECK(icons.ProbeArrow(g).why == "probe: no map screen");
}
