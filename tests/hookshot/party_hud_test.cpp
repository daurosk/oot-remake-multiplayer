// SPDX-License-Identifier: GPL-3.0-or-later
// The other players' hearts on the game's HUD (hookshot/src/coop_party_hud.h) on a pretend game.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop_party_hud.h"

namespace party = CoopPartyHud;
namespace ui = CoopNativeUi;

namespace {

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kManager = 0x96000000, kScreens = 0x97000000, kStatus = 0x93000000, kLayout = 0x94000000;
constexpr std::uint64_t kBlock = 0x9a000000, kPanes = 0x9b000000, kAnims = 0x9c000000;
constexpr std::uint64_t kParts = 0x9d000000, kBoxes = 0x9e000000;   // our parts' layouts, their text panes
constexpr std::uint64_t kAttrs = 0x1234;                             // a text pane's own message attributes
constexpr std::uint64_t kSource = 0x9f000000, kLive = 0xa0000000;   // the game's heart part layout; our run-time memory
constexpr std::uint64_t kLabelSource = 0x9f800000;                   // the pack's text part layout (its name lines)
// The game's MenuOverlay_00 (no pack: its Pa_Category_00 = PaMenuCategory_00 is the name lines' source).
constexpr std::uint64_t kOverlay = 0x93100000, kOverlayLayout = 0x94800000, kOverlayPanes = 0x95800000;
constexpr std::uint64_t kLayoutType = 0x95000000, kBareSource = 0x9f400000;

struct FakeGame final : party::Game {
    // A pretend game in play: the game's play UI is up (patch 0018's HUD screen ran since the last load), so its
    // listed screens are live (CoopNativeUi::ScreenLive, party-hud-crash-c126).
    FakeGame() {
        CoopNativeUi::SetSceneLoading(false);
        CoopNativeUi::SetRunningScreens(std::vector<std::uint64_t>{}, true);
    }
    std::map<std::uint64_t, std::uint8_t> mem;
    std::map<std::string, std::uint64_t> panes, anims;   // made as asked, when `pack` has them
    std::map<std::uint64_t, int> shown;                  // pane -> 0/1
    std::map<std::uint64_t, float> frames;               // animation -> the frame it is stopped at
    std::map<std::uint64_t, float> alphas;               // pane -> its own alpha (kPaneAlpha)
    std::map<std::string, std::string> texts;            // text pane path -> text (as the game's set-message call)
    std::map<std::uint64_t, std::string> parts, boxes;   // part layout -> its part's name; text pane -> its path
    int pane_text_calls = 0;                             // f_8045b970 (no path: sets nothing on our parts)
    bool text_panes = true;                              // the parts have T_Text_00 and T_Text_01
    bool pack = true;
    int pack_slots = 3;                                  // party_hud.py SLOTS
    std::int32_t gauge_last = 100;
    int calls = 0;
    // The game's own heart part (Pa_HeartGauge_00 under N_ScaleL_00): run-time gauges can be made from it (R2/R3).
    bool game_gauge = false;
    std::uint64_t next_new = kLive;
    std::map<std::uint64_t, std::uint64_t> clone_roots;   // clone root -> clone
    std::map<std::uint64_t, std::string> clone_panes;      // a pane in a clone -> its name
    std::map<std::uint64_t, std::string> clone_names;      // clone root -> the name it was made with
    std::map<std::uint64_t, std::uint64_t> clone_sources;  // clone root -> the part layout it was made from
    std::map<std::uint64_t, int> heart_set;                // controller -> its last state (f_833dca10)
    std::map<std::uint64_t, int> gauge_frame;              // part heart -> its last frame (f_833dcb60)
    int made = 0, attached = 0, unlinked = 0;
    bool menu_overlay = false;                             // MenuOverlay_00 runs (Pa_Category_00 + T_BtnL_00)
    std::uint64_t hud_layout = 0;                          // the layout Hud() put on the status screen
    std::map<std::uint64_t, std::uint64_t> attached_to;    // clone root -> the HUD layout it was attached in
    bool Live(std::uint64_t f, const std::array<std::uint64_t, 5>& x, std::uint64_t& out) {
        out = 0;
        if (f == At(party::addr::kNew)) return out = next_new, next_new += 0x10000, true;
        if (f == At(party::addr::kClonePart)) {
            clone_names[x[0] + 0x1000] = Str(x[2]);
            clone_sources[x[0] + 0x1000] = x[1];
            return ++made, clone_roots[x[0] + 0x1000] = x[0], true;
        }
        if (f == At(party::addr::kPaneTable) && clone_roots.count(x[0] - 0x40 + 0x1000)) return out = x[0] - 0x40 + 0x1000, true;
        if (f == At(party::addr::kTablePane) && clone_roots.count(x[0])) {
            const std::string name = Str(x[1]);
            for (const auto& [p, n] : clone_panes) if (n == name && p / 0x10000 == x[0] / 0x10000) return out = p, true;
            out = x[0] + 0x100 * (clone_panes.size() % 200 + 1);
            clone_panes[out] = name;
            if ((clone_sources[x[0]] == kLabelSource || clone_sources[x[0]] == kBareSource) &&
                (name == "T_Text_00" || name == "T_Text_01")) {
                // (a text pane of a cloned name line: set as the pack's own, under the clone's name)
                boxes[out] = clone_names[x[0]] + "/" + name;
                const std::uint32_t flags = party::addr::kTextPaneKind << 22;
                Write(out + party::addr::kPaneFlags, &flags, sizeof flags);
                Put(out + party::addr::kPaneMsgAttrs, kAttrs);
            }
            return true;
        }
        if (f == At(party::addr::kAttach)) return attached_to[x[1]] = hud_layout, ++attached, true;
        if (f == At(party::addr::kGaugeMake)) return true;
        if (f == At(party::addr::kGaugeSetUp)) {   // 20 whole hearts and the part heart, each with its animations
            for (int i = 0; i <= party::kHearts; ++i) {
                const std::uint64_t c = x[0] + 0x2000 + 0x100 * static_cast<std::uint64_t>(i);
                Put(c + party::addr::kHeartBase, c + 0x80);
                Put(c + party::addr::kHeartGauge, c + 0x90);
                Put(c + 0x90 + party::addr::kAnimPlayer, c + 0xa0);   // (its last frame: gauge_last)
                Put(i < party::kHearts ? x[0] + party::addr::kGaugeWhole + 8 * static_cast<std::uint64_t>(i)
                                       : x[0] + party::addr::kGaugePart, c);
            }
            return true;
        }
        if (f == At(party::addr::kGaugeUnlink)) return ++unlinked, true;
        if (f == At(party::addr::kHeartSet)) return heart_set[x[0]] = static_cast<int>(x[1]), true;
        if (f == At(party::addr::kGaugeFrame)) return gauge_frame[x[0]] = static_cast<int>(x[1]), true;
        return false;
    }

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
        for (char c = 0; Read(a++, &c, 1) && c != 0;) s.push_back(c);
        return s;
    }
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x) override {
        ++calls;
        if (f == At(ui::addr::kMalloc)) return kBlock;
        if (std::uint64_t out = 0; game_gauge && Live(f, x, out)) return out;
        if (f == At(ui::addr::kPaneByName)) {
            const std::string name = Str(x[1]);
            if (x[0] == kOverlayLayout) {   // (MenuOverlay_00: its category part and its own top-level text)
                if (name == "Pa_Category_00") return Put(kOverlayPanes + party::addr::kPartLayout, kBareSource), kOverlayPanes;
                return name == "T_BtnL_00" ? kOverlayPanes + 0x100 : 0;
            }
            for (const auto& [root, made_as] : clone_names) {   // (our run-time parts, while they hang in that layout)
                if (made_as == name && attached_to.count(root) && attached_to[root] == x[0]) return root;
            }
            if (game_gauge && (name == "Pa_HeartGauge_00" || name == "N_ScaleL_00")) {
                if (panes.count(name) == 0) panes[name] = kPanes + 0x1000 * panes.size();
                if (name == "Pa_HeartGauge_00") Put(panes[name] + party::addr::kPartLayout, kSource);
                return panes[name];
            }
            if (!pack || name.rfind("Pa_Coop", 0) != 0) return 0;
            for (const char* slotted : {"Pa_CoopHeart_", "Pa_CoopGauge_", "Pa_CoopName_"}) {
                const std::size_t n = std::string(slotted).size();
                if (name.rfind(slotted, 0) == 0 && std::stoi(name.substr(n)) >= pack_slots) return 0;
            }
            if (panes.count(name) == 0) panes[name] = kPanes + 0x1000 * panes.size();
            for (const char* line : {"Pa_CoopName_", "Pa_CoopMore_", "Pa_CoopSelf_"}) {   // (PaEnemyLocation_00)
                if (name.rfind(line, 0) == 0) Put(panes[name] + party::addr::kPartLayout, kLabelSource);
            }
            return panes[name];
        }
        if (f == At(party::addr::kAnimFind)) {
            const std::string name = Str(x[1]);
            if (!pack || name.find("IsDoubleDefense") != std::string::npos) return 0;
            if (anims.count(name) == 0) {
                const std::uint64_t at = kAnims + 0x100 * anims.size();
                anims[name] = at;
                Put(at + party::addr::kAnimPlayer, at + 0x80);
            }
            return anims[name];
        }
        if (f == At(party::addr::kAnimLastFrame)) {
            return static_cast<std::uint64_t>(static_cast<std::uint32_t>(gauge_last));
        }
        if (f == At(party::addr::kPaneShow)) {
            shown[x[0]] = static_cast<int>(x[1]);
            return 0;
        }
        if (f == At(ui::addr::kSetPaneText)) {   // (a pane of the layout itself only: a path matches nothing)
            ++pane_text_calls;
            return 0;
        }
        if (f == At(party::addr::kPathFind)) {   // "Part/Pane": the part's layout out, the name in it
            const std::string path = Str(x[0]);
            const auto slash = path.find('/');
            if (!pack || slash == std::string::npos) return 0;
            const std::string part = path.substr(0, slash);
            if (panes.count(part) == 0) return 0;
            std::uint64_t at = 0;
            for (const auto& [l, name] : parts) at = name == part ? l : at;
            if (at == 0) at = kParts + 0x1000 * (parts.size() + 1), parts[at] = part;
            Put(x[2], at);
            return x[0] + slash + 1;
        }
        if (f == At(party::addr::kPaneTable)) return parts.count(x[0] - 0x40) ? x[0] - 0x40 + 0x800 : 0;
        if (f == At(party::addr::kTablePane)) {
            const std::string name = Str(x[1]);
            const auto part = parts.find(x[0] - 0x800);
            if (!text_panes || part == parts.end() || (name != "T_Text_00" && name != "T_Text_01")) return 0;
            const std::string path = part->second + "/" + name;
            for (const auto& [box, p] : boxes) if (p == path) return box;
            const std::uint64_t box = kBoxes + 0x1000 * boxes.size();
            boxes[box] = path;
            const std::uint32_t flags = party::addr::kTextPaneKind << 22;
            Write(box + party::addr::kPaneFlags, &flags, sizeof flags);
            Put(box + party::addr::kPaneMsgAttrs, kAttrs);
            return box;
        }
        if (f == At(party::addr::kPaneSetMsg)) {   // the text box keeps the pointer and the length
            ui::Ref r;
            Read(x[1], &r, sizeof r);
            if (boxes.count(x[0]) == 0 || static_cast<std::uint64_t>(r.end) != kAttrs) return 0;
            texts[boxes[x[0]]] = Str(r.text).substr(0, r.bytes);
            return 1;
        }
        return 0;
    }
    std::uint64_t Main() const override { return kMain; }
    std::optional<std::uint64_t> CallFloat(std::uint64_t f, std::uint64_t x0, float s0) override {
        ++calls;
        if (f == At(party::addr::kPaneAlpha)) {
            alphas[x0] = s0;
            return 0;
        }
        if (f != At(party::addr::kAnimStopAt)) return std::nullopt;
        frames[x0] = s0;
        return 0;
    }
    void Put(std::uint64_t a, std::uint64_t v) { Write(a, &v, sizeof v); }
    void Hud(std::uint64_t layout = kLayout) {
        Put(At(ui::addr::kUiManagerGot), kManager - 8);
        Put(kManager - 8, kManager);
        const std::uint32_t count = menu_overlay ? 2 : 1;
        Write(kManager + ui::addr::kUiCount, &count, sizeof count);
        Put(kManager + ui::addr::kUiScreens, kScreens);
        Put(kScreens, kStatus);
        Put(kStatus, At(party::addr::kStatusVtable));
        Put(kStatus + ui::addr::kScreenLayout, layout);
        hud_layout = layout;
        if (menu_overlay) {   // (both layouts of one layout type, as the game's)
            Put(kScreens + 8, kOverlay);
            Put(kOverlay, 0x84000000);
            Put(kOverlay + ui::addr::kScreenLayout, kOverlayLayout);
            Put(layout, kLayoutType);
            Put(kOverlayLayout, kLayoutType);
        }
    }
    float Alpha(const std::string& pane) { return alphas.at(panes.at(pane)); }
    bool Shown(const std::string& pane) { return panes.count(pane) != 0 && shown[panes[pane]] == 1; }
    float Frame(const std::string& anim) { return frames.at(anims.at(anim)); }
    std::array<float, 2> Where(const std::string& pane) {
        std::array<float, 2> at{};
        Read(panes.at(pane) + party::addr::kPaneXY, at.data(), sizeof at);
        return at;
    }
};

std::vector<int> Quarters(int life, int max) {
    std::vector<int> out;
    for (int i = 0; i < (max + 3) / 4; ++i) out.push_back(std::clamp(life - 4 * i, 0, 4));
    return out;
}

std::string HeartName(int slot, int i) {
    char name[32];
    std::snprintf(name, sizeof name, "Pa_CoopHeart_%d_%02d", slot, i);
    return name;
}

}  // namespace

TEST_CASE("party hud: a player's hearts as the game sets its own") {
    for (int max = 12; max <= 80; max += 4) {
        for (int life = 0; life <= max; ++life) {
            const party::Plan p = party::PlanHearts(Quarters(life, max));
            CHECK(p.hearts == max / 4);
            int quarters = 0;
            for (int i = 0; i < party::kHearts; ++i) {
                const int state = p.whole[static_cast<std::size_t>(i)];
                CHECK(state == (i >= max / 4 ? 0 : i < life / 4 ? 2 : 1));
                quarters += state == 2 ? 4 : 0;
            }
            CHECK(quarters + p.part_quarters == life);
            CHECK(p.part_at == (life % 4 == 0 ? -1 : life / 4));
        }
    }
    CHECK(party::PlanHearts(std::vector<int>(40, 4)).hearts == party::kHearts);   // never past the copies
    CHECK(party::PlanHearts({}) == party::Plan{});
}

TEST_CASE("party hud: the part heart's frame on a long and on a short animation") {
    CHECK(party::GaugeFrame(1, 100) == 25.0f);
    CHECK(party::GaugeFrame(3, 100) == 75.0f);
    CHECK(party::GaugeFrame(1, 3) == 0.0f);    // the game's damage path: up to 1/4 -> 0 ... more than 3/4 -> 3
    CHECK(party::GaugeFrame(3, 3) == 2.0f);
    CHECK(party::GaugeFrame(4, 3) == 3.0f);
    CHECK(party::GaugeFrame(2, -1) == 0.0f);
}

TEST_CASE("party hud: the blocks stay on the picture") {
    // Every block that fits under a tall game HUD (15 others, two rows each): every heart inside the 3840 x 2160
    // layout, the rest in the "+N" line under them.
    float top = party::TopFrom(0.2);
    CHECK(top == doctest::Approx(-0.2f * 2160.0f - party::kBlockGap));
    CHECK(party::TopFrom(std::nullopt) == party::kDefaultTop);
    const std::size_t fit = party::BlocksThatFit(top, std::vector<int>(party::kSlots, party::kHearts), party::kSlots);
    CHECK(fit >= 3);
    CHECK(fit < static_cast<std::size_t>(party::kSlots));
    for (std::size_t s = 0; s < fit; ++s) {
        for (int i = 0; i < party::kHearts; ++i) {
            const auto at = party::HeartAt(top, i);
            CHECK(at[0] > 72.0f * party::kScale);
            CHECK(at[0] < 3840.0f);
            CHECK(at[1] < 0.0f);
            CHECK(at[1] - 72.0f * party::kScale > -2160.0f);
        }
        CHECK(party::HeartAt(top, 10)[0] == party::HeartAt(top, 0)[0]);   // the second row under the first
        CHECK(party::HeartAt(top, 10)[1] == doctest::Approx(party::HeartAt(top, 0)[1] - party::kStepY));
        top -= party::BlockHeight(party::kHearts);
    }
    CHECK(top - party::kLabelHeight > -2160.0f);   // and the "+N" line under them
    CHECK(party::BlockHeight(3) < party::BlockHeight(11));
    // Small blocks (3 hearts, one row): more of them fit; all 15 when they do, no "+N" then.
    top = party::TopFrom(0.2);
    CHECK(party::BlocksThatFit(top, std::vector<int>(party::kSlots, 3), party::kSlots) > fit);
    CHECK(party::BlocksThatFit(top, std::vector<int>(4, 3), party::kSlots) == 4);
    CHECK(party::BlocksThatFit(top, std::vector<int>(4, 3), 3) == 3);   // (a pack of 3 slots)
    CHECK(party::BlocksThatFit(top, {}, 3) == 0);
    // A game HUD reaching far down (a bottom at 90 %): at least one full block and the "+N" line on the picture.
    float low = party::TopFrom(0.9);
    CHECK(party::BlocksThatFit(low, std::vector<int>(5, party::kHearts), party::kSlots) >= 1);
    low -= party::BlockHeight(party::kHearts);
    CHECK(low - party::kLabelHeight >= -2160.0f);
    CHECK(party::MoreText(0).empty());
    CHECK(party::MoreText(2) == "+2");
}

TEST_CASE("party hud: an older pack hides the hearts (nothing drawn instead)") {
    FakeGame game;
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<std::string> log;
    const std::vector<party::Block> one = {{"A", Quarters(12, 12)}};
    CHECK_FALSE(hud.Step(game, one, std::nullopt, &log));   // no HUD yet
    game.Hud();
    game.pack = false;
    CHECK_FALSE(hud.Step(game, one, std::nullopt, &log));
    CHECK_FALSE(hud.native());
    REQUIRE(log.size() == 2);
    CHECK(log[0].find("no run-time gauges") != std::string::npos);
    CHECK(log[1].find("older pack") != std::string::npos);
    CHECK(log[1].find("hidden") != std::string::npos);
    const int calls = game.calls;
    CHECK_FALSE(hud.Step(game, one, std::nullopt, &log));   // asked once per HUD
    CHECK(game.calls == calls);
}

TEST_CASE("party hud: shows, sets and hides the copies") {
    FakeGame game;
    game.Hud();
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<std::string> log;
    // Player 1: 5 hearts, 2 and 3/4 filled. Player 2: 12 hearts, all full.
    std::vector<party::Block> blocks = {{"Saria  Kokiri Forest", Quarters(11, 20)}, {"Ruto", Quarters(48, 48)}};
    REQUIRE(hud.Step(game, blocks, 0.15, &log));
    CHECK(hud.native());
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("its own heart parts") != std::string::npos; }));
    CHECK(game.pane_text_calls == 0);   // (f_8045b970 takes no path: never asked for our parts' panes)
    CHECK(game.texts["Pa_CoopName_0/T_Text_00"] == "Saria  Kokiri Forest");
    CHECK(game.texts["Pa_CoopName_0/T_Text_01"] == "Saria  Kokiri Forest");
    CHECK(game.texts["Pa_CoopName_1/T_Text_00"] == "Ruto");
    CHECK(game.Shown("Pa_CoopName_0"));
    CHECK_FALSE(game.Shown("Pa_CoopName_2"));
    CHECK_FALSE(game.Shown("Pa_CoopMore_0"));
    const float top = party::TopFrom(0.15);
    for (int i = 0; i < party::kHearts; ++i) {
        const std::string name = HeartName(0, i);
        CHECK(game.Shown(name) == (i < 5));
        if (i >= 5) continue;
        CHECK(game.Frame(name + "/BaseState") == 1.0f);
        CHECK(game.Frame(name + "/HeartState") == (i < 2 ? 1.0f : 0.0f));
        CHECK(game.Where(name) == party::HeartAt(top, i));
    }
    CHECK(game.Shown("Pa_CoopGauge_0"));                                   // the part heart on the third heart
    CHECK(game.Where("Pa_CoopGauge_0") == party::HeartAt(top, 2));
    CHECK(game.Frame("Pa_CoopGauge_0/HeartGauge") == 75.0f);
    CHECK_FALSE(game.Shown("Pa_CoopGauge_1"));
    CHECK(game.Shown(HeartName(1, 11)));
    CHECK(game.Where(HeartName(1, 0)) == party::HeartAt(top - party::BlockHeight(5), 0));   // under the first block
    CHECK_FALSE(game.Shown(HeartName(2, 0)));

    const int calls = game.calls;
    REQUIRE(hud.Step(game, blocks, 0.15, &log));   // nothing changed: nothing set again
    CHECK(game.calls == calls);

    blocks[0].hearts = Quarters(12, 20);           // healed to 3 whole hearts: the part heart goes
    REQUIRE(hud.Step(game, blocks, 0.15, &log));
    CHECK_FALSE(game.Shown("Pa_CoopGauge_0"));
    CHECK(game.Frame(HeartName(0, 2) + "/HeartState") == 1.0f);

    blocks.erase(blocks.begin());                  // player 1 leaves: player 2 moves up, slot 1 hidden
    REQUIRE(hud.Step(game, blocks, 0.15, &log));
    CHECK(game.texts["Pa_CoopName_0/T_Text_00"] == "Ruto");
    CHECK(game.Shown(HeartName(0, 11)));
    CHECK_FALSE(game.Shown(HeartName(1, 0)));
    CHECK_FALSE(game.Shown("Pa_CoopName_1"));

    REQUIRE(hud.Step(game, {}, 0.15, &log));       // alone: nothing of ours on the HUD
    for (const auto& [name, pane] : game.panes) CHECK(game.shown[pane] == 0);

    REQUIRE(hud.Step(game, party::TestBlocks(0), std::nullopt, &log));   // four players: three blocks and "+1"
    CHECK(game.texts["Pa_CoopMore_0/T_Text_00"] == "+1");
    CHECK(game.Shown("Pa_CoopMore_0"));
    CHECK(game.Shown(HeartName(0, 19)));

    game.Hud(kLayout + 0x1000);                    // the HUD made again: found and set again
    game.shown.clear();
    REQUIRE(hud.Step(game, party::TestBlocks(0), std::nullopt, &log));
    CHECK(game.Shown(HeartName(0, 19)));
}

TEST_CASE("party hud: low life, the glowing heart, played frames") {
    CHECK_FALSE(party::LowLife(0, 12));            // down: not low
    CHECK(party::LowLife(4, 12));                  // 1 heart of 3
    CHECK_FALSE(party::LowLife(5, 20));
    CHECK(party::LowLife(6, 40));                  // 1 1/2 of 10
    CHECK(party::LowLife(8, 60));
    CHECK(party::LowLife(11, 80));
    CHECK_FALSE(party::LowLife(12, 80));
    CHECK(party::GlowHeart(-1, 10, 5) == -1);      // first sight: no glow
    CHECK(party::GlowHeart(10, 10, 5) == -1);
    CHECK(party::GlowHeart(12, 11, 5) == 2);       // hurt from 3 hearts: the third lost a quarter
    CHECK(party::GlowHeart(12, 8, 5) == 2);        // a whole heart lost: the third, now empty
    CHECK(party::GlowHeart(8, 9, 5) == 2);         // healed a quarter: the third
    CHECK(party::GlowHeart(19, 20, 5) == 4);       // healed full: the last
    CHECK(party::GlowHeart(20, 0, 5) == 0);
    CHECK(party::PlayedFrame(0, 20, false) == 0.0f);
    CHECK(party::PlayedFrame(500, 20, false) == 15.0f);
    CHECK(party::PlayedFrame(5000, 20, false) == 20.0f);   // held at its end
    CHECK(party::PlayedFrame(1000, 20, true) == 9.0f);     // round again: 30 % 21
    CHECK(party::PlayedFrame(1000, 0, true) == 0.0f);
}

TEST_CASE("party hud: a hit glows on the heart that lost, the pinch plays at low life") {
    FakeGame game;
    game.Hud();
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<party::Block> blocks = {{"Saria", Quarters(12, 20)}};
    REQUIRE(hud.Step(game, blocks, 0.15, nullptr, 1000));
    CHECK(game.Frame(HeartName(0, 2) + "/GlowState") == 0.0f);   // first sight: no glow
    blocks[0].hearts = Quarters(4, 20);                          // hit to 1 heart: low
    REQUIRE(hud.Step(game, blocks, 0.15, nullptr, 2000));
    CHECK(game.Frame(HeartName(0, 1) + "/GlowState") == party::kGlowState);   // the heart that lost (now empty)
    CHECK(game.Frame(HeartName(0, 2) + "/GlowState") == 0.0f);
    CHECK(game.Frame(HeartName(0, 0) + "/Pinch") == party::PlayedFrame(2000, game.gauge_last, true));   // its own clock
    REQUIRE(hud.Step(game, blocks, 0.15, nullptr, 2500));
    CHECK(game.Frame(HeartName(0, 1) + "/Glow") == 15.0f);
    REQUIRE(hud.Step(game, blocks, 0.15, nullptr, 2000 + 60000));   // long after: the glow over
    CHECK(game.Frame(HeartName(0, 1) + "/GlowState") == 0.0f);
    blocks[0].hearts = Quarters(20, 20);                         // healed: no pinch
    REQUIRE(hud.Step(game, blocks, 0.15, nullptr, 70000));
    CHECK(game.Frame(HeartName(0, 0) + "/Pinch") == 0.0f);
}

TEST_CASE("party hud: double defence on every heart, and off") {
    FakeGame game;
    game.Hud();
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<party::Block> blocks = {{"Saria", Quarters(10, 12)}};
    blocks[0].double_defence = true;
    REQUIRE(hud.Step(game, blocks, 0.15));
    CHECK(game.Frame(HeartName(0, 0) + "/DefenceBorn") == static_cast<float>(game.gauge_last));
    CHECK(game.Frame("Pa_CoopGauge_0/DefenceBorn") == static_cast<float>(game.gauge_last));
    blocks[0].double_defence = false;
    REQUIRE(hud.Step(game, blocks, 0.15));
    CHECK(game.Frame(HeartName(0, 0) + "/DefenceBorn") == 0.0f);
}

TEST_CASE("party hud: a player not connected is dimmed, and lit again") {
    FakeGame game;
    game.Hud();
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<party::Block> blocks = {{"Saria", Quarters(11, 20)}, {"Ruto", Quarters(12, 12), true}};
    REQUIRE(hud.Step(game, blocks, 0.15));
    CHECK(game.Alpha("Pa_CoopName_0") == 1.0f);
    CHECK(game.Alpha(HeartName(0, 0)) == 1.0f);
    CHECK(game.Alpha("Pa_CoopGauge_0") == 1.0f);
    CHECK(game.Alpha("Pa_CoopName_1") == party::kDimAlpha);
    CHECK(game.Alpha(HeartName(1, 2)) == party::kDimAlpha);
    blocks[1].dim = false;                          // back: lit again
    REQUIRE(hud.Step(game, blocks, 0.15));
    CHECK(game.Alpha("Pa_CoopName_1") == 1.0f);
    CHECK(game.Alpha(HeartName(1, 2)) == 1.0f);
}

TEST_CASE("party hud: the test players go through every fill") {
    const auto a = party::TestBlocks(0), b = party::TestBlocks(party::kTestStepMs * 3);
    REQUIRE(a.size() == 4);
    CHECK(a[0].hearts.size() == 20);
    CHECK(a[2].hearts == std::vector<int>{4, 4, 4});
    CHECK(a[1].text.rfind(std::string(16, 'W') + " (talking)  Inside Ganondorf's Castle ", 0) == 0);   // the longest line
    CHECK(b[2].hearts == std::vector<int>{4, 4, 1});
    CHECK(party::TestBlocks(party::kTestStepMs * 12)[2].hearts == std::vector<int>{0, 0, 0});
    CHECK(party::TestBlocks(party::kTestStepMs * 13)[2].hearts == std::vector<int>{4, 4, 4});
}

namespace {
// Row 326: N_ScaleL_00 as Longshot's hos_wide_ui_tick places it on a picture narrower than 16:9 (ratio r, the HUD's
// size k: the canvas drawn k smaller, the corner panes moved up by 1080 (1/k - 1) and in by 1920 (r / (k 16/9) - 1)).
std::array<float, 12> NarrowMatrix(float r, float k) {
    const float ey = 1080.0f * (1.0f / k - 1.0f), ex = 1920.0f * (r / (k * 16.0f / 9.0f) - 1.0f);
    return {1, 0, 0, 0, 1, 0, 0, 0, 1, -1920.0f - ex, 1080.0f + ey, 0};
}
// GameHudBottom of a HUD whose lowest point is `below` units under N_ScaleL_00's top (coop_hud_layout.h HudBottom:
// a fraction of 2160 down from the canvas's 16:9 top, +1080)
double BottomOf(const std::array<float, 12>& m, float below) {
    return (1080.0 - (m[10] - below)) / 2160.0;
}
}  // namespace

TEST_CASE("party hud: row 326, the blocks go under the game's hearts on a narrower picture (split screen halves)") {
    // the 16:9 frame: everything as before row 326
    const std::array<float, 12> wide{1, 0, 0, 0, 1, 0, 0, 0, 1, -1920.0f, 1080.0f, 0};
    const party::ParentFrame w = party::FrameFrom(wide.data());
    CHECK(w.left == -1920.0f);
    CHECK(w.top == 1080.0f);
    CHECK(party::PictureHeight(w) == 2160.0f);
    CHECK(party::PictureWidth(w) == 3840.0f);
    CHECK(party::LabelRoom(w) == doctest::Approx(party::kLabelRoom));
    for (const double f : {0.05, 0.2, 0.3, 0.62, 0.9}) {
        CHECK(party::TopFrom(f, false, w) == party::TopFrom(f));
        CHECK(party::TopFrom(f, true, w) == party::TopFrom(f, true));
    }
    CHECK(party::TopFrom(-0.1, false, w) == party::kDefaultTop);   // (not on the picture: as before)
    CHECK(party::TopFrom(1.1, false, w) == party::kDefaultTop);
    // a matrix that is not believable: the 16:9 frame
    const std::array<float, 12> zero{};
    CHECK(party::FrameFrom(zero.data()).top == 1080.0f);
    std::array<float, 12> bad = wide;
    bad[10] = std::nanf("");
    CHECK(party::FrameFrom(bad.data()).top == 1080.0f);

    // 8:9 halves (HOS_ASPECT=1920:2160, split-fill-301-2 05:31) at a few HUD sizes: the first block's top is kBlockGap
    // under the HUD's bottom in N_ScaleL_00's frame, wherever Longshot moved it. Before row 326 it was higher by the
    // move up (the owner's shot: the party name bar over the own hearts' second row).
    for (const float k : {0.5f, 0.6f, 0.75f, 1.0f}) {
        const auto m = NarrowMatrix(8.0f / 9.0f, k);
        const party::ParentFrame f = party::FrameFrom(m.data());
        CHECK(f.top == doctest::Approx(1080.0f / k));
        const float below = 0.2f * 2160.0f;   // (two rows of hearts and the magic bar)
        const double bottom = BottomOf(m, below);
        CHECK(party::TopFrom(bottom, false, f) == doctest::Approx(-below - party::kBlockGap));
        if (k < 1.0f) {
            CHECK(party::TopFrom(bottom) > -below);   // (the old reading: over the game's own hearts)
        }
        // the picture is taller in the parent's units: as many blocks fit as on a 16:9 picture, or more
        CHECK(party::PictureHeight(f) == doctest::Approx(2160.0f / k));
        const float top = party::TopFrom(bottom, false, f);
        CHECK(party::BlocksThatFit(top, std::vector<int>(party::kSlots, party::kHearts), party::kSlots, f) >=
              party::BlocksThatFit(party::TopFrom(0.2), std::vector<int>(party::kSlots, party::kHearts), party::kSlots));
        // a line is at most 30% of this picture's width
        CHECK(party::LabelRoom(f) == doctest::Approx(0.3f * 3840.0f * (8.0f / 9.0f) / (k * 16.0f / 9.0f)));
    }
}

TEST_CASE("party hud: row 326, Step places the blocks in N_ScaleL_00's frame as it is now") {
    FakeGame game;
    game.Hud();
    game.game_gauge = true;   // (N_ScaleL_00 found)
    party::Hud hud;
    std::vector<std::string> log;
    const std::vector<party::Block> blocks = {{"Saria", Quarters(11, 20)}};
    REQUIRE(hud.Step(game, blocks, 0.15, &log, 0));
    CHECK(game.Where(HeartName(0, 0)) == party::HeartAt(party::TopFrom(0.15), 0));   // (no matrix yet: 16:9)
    REQUIRE(game.panes.count("N_ScaleL_00"));
    const auto m = NarrowMatrix(8.0f / 9.0f, 0.6f);
    game.Write(game.panes["N_ScaleL_00"] + party::addr::kPaneMatrix, m.data(), sizeof m);
    const double bottom = BottomOf(m, 0.2f * 2160.0f);
    log.clear();
    REQUIRE(hud.Step(game, blocks, bottom, &log, 0));
    const party::ParentFrame f = party::FrameFrom(m.data());
    const auto want = party::HeartAt(-0.2f * 2160.0f - party::kBlockGap, 0);
    CHECK(game.Where(HeartName(0, 0))[0] == doctest::Approx(want[0]));
    CHECK(game.Where(HeartName(0, 0))[1] == doctest::Approx(want[1]));
    CHECK(game.Where(HeartName(0, 0)) == party::HeartAt(party::TopFrom(bottom, false, f), 0));
    bool said = false;
    for (const auto& l : log) said = said || l.find("frame: N_ScaleL_00 at (") != std::string::npos;
    CHECK(said);
}

TEST_CASE("party hud: our own voice line goes first on the game's text part, the blocks one line lower") {
    FakeGame game;
    game.Hud();
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<std::string> log;
    const std::vector<party::Block> blocks = {{"Saria", Quarters(11, 20)}};
    REQUIRE(hud.Step(game, blocks, 0.15, &log, 0, "You (talking)"));
    CHECK(game.texts["Pa_CoopSelf_0/T_Text_00"] == "You (talking)");
    CHECK(game.texts["Pa_CoopSelf_0/T_Text_01"] == "You (talking)");
    CHECK(game.Shown("Pa_CoopSelf_0"));
    const float top = party::TopFrom(0.15, true);
    CHECK(top == party::TopFrom(0.15, false));   // (our line takes the first place; only the stack's lowest start moves up)
    CHECK(party::TopFrom(0.95, true) > party::TopFrom(0.95, false));
    CHECK(game.Where("Pa_CoopSelf_0") ==
          std::array<float, 2>{party::LabelX("You (talking)"), top - party::kLabelHeight / 2.0f});
    // the blocks start under it
    CHECK(game.Where(HeartName(0, 0)) == party::HeartAt(top - (party::kLabelHeight + party::kBlockGap), 0));
    CHECK(game.Where("Pa_CoopName_0")[1] == top - (party::kLabelHeight + party::kBlockGap) - party::kLabelHeight / 2.0f);

    REQUIRE(hud.Step(game, blocks, 0.15, &log, 0, "You (muted)"));   // the voice state changes: the line's text does
    CHECK(game.texts["Pa_CoopSelf_0/T_Text_00"] == "You (muted)");

    REQUIRE(hud.Step(game, blocks, 0.15, &log, 0, ""));             // no voice: no line, the blocks back up
    CHECK_FALSE(game.Shown("Pa_CoopSelf_0"));
    CHECK(game.Where(HeartName(0, 0)) == party::HeartAt(party::TopFrom(0.15), 0));

    REQUIRE(hud.Step(game, {}, 0.15, &log, 0, "You"));              // nobody else: no row of our own either
    CHECK_FALSE(game.Shown("Pa_CoopSelf_0"));
}

namespace {
bool Said(const std::vector<std::string>& log, const std::string& what) {
    return std::any_of(log.begin(), log.end(), [&](const std::string& l) { return l.find(what) != std::string::npos; });
}
}  // namespace

TEST_CASE("party hud: every line's text panes are found by path and the log says so (row 291)") {
    FakeGame game;
    game.Hud();
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<std::string> log;
    REQUIRE(hud.Step(game, party::TestBlocks(0), std::nullopt, &log, 0, party::TestOwn()));
    for (const char* part : {"Pa_CoopName_0", "Pa_CoopName_1", "Pa_CoopName_2", "Pa_CoopMore_0", "Pa_CoopSelf_0"}) {
        CHECK(Said(log, std::string("text ") + part + "/T_Text_00: found"));
        CHECK(Said(log, std::string("text ") + part + "/T_Text_01: found"));
        CHECK(Said(log, std::string("text ") + part + ": set on 2 text pane(s)"));
        CHECK(game.texts[std::string(part) + "/T_Text_00"] == game.texts[std::string(part) + "/T_Text_01"]);
    }
    CHECK(game.texts["Pa_CoopSelf_0/T_Text_00"] == "You (talking)");
    CHECK(game.texts["Pa_CoopName_0/T_Text_00"] == "Twenty 80/80");
    CHECK(game.texts["Pa_CoopName_2/T_Text_00"] == "Three 12/12");   // the dimmed one has its name too
    CHECK(game.texts["Pa_CoopMore_0/T_Text_00"] == "+1");
    CHECK(game.Alpha("Pa_CoopName_2") == doctest::Approx(party::kDimAlpha));
    CHECK(game.pane_text_calls == 0);

    // The width the game drew it, logged once: here the game draws Pa_CoopName_0's T_Text_00 200 wide.
    std::uint64_t box = 0;
    for (const auto& [b, path] : game.boxes) box = path == "Pa_CoopName_0/T_Text_00" ? b : box;
    const float drawn = 200.0f;
    game.Write(box + party::addr::kPaneTextWidth, &drawn, sizeof drawn);
    hud.Step(game, party::TestBlocks(0), std::nullopt, &log, 0, party::TestOwn());
    hud.Step(game, party::TestBlocks(0), std::nullopt, &log, 0, party::TestOwn());
    CHECK(Said(log, "text Pa_CoopName_0 drawn 200 wide"));

    // A new text on a line: set again on both panes, the pointer the text box keeps holds the new words.
    REQUIRE(hud.Step(game, party::TestBlocks(party::kTestStepMs), std::nullopt, &log, 0, "You (muted)"));
    CHECK(game.texts["Pa_CoopName_0/T_Text_01"] == "Twenty 79/80");
    CHECK(game.texts["Pa_CoopSelf_0/T_Text_01"] == "You (muted)");
}

TEST_CASE("party hud: parts without text panes: the hearts still show, the log says why no names") {
    FakeGame game;
    game.Hud();
    game.text_panes = false;
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<std::string> log;
    REQUIRE(hud.Step(game, {{"Saria", Quarters(12, 12)}}, std::nullopt, &log));
    CHECK(game.Shown(HeartName(0, 0)));
    CHECK(Said(log, "text Pa_CoopName_0/T_Text_00: not found (pane not found)"));
    CHECK(Said(log, "text Pa_CoopName_0: not set (no text pane found)"));
    CHECK(game.texts.empty());
}

TEST_CASE("party hud: the slots the pack has, the rest in +N (row R3)") {
    for (const int slots : {1, 3, 5}) {
        FakeGame game;
        game.pack_slots = slots;
        game.Hud();
        party::Hud hud;
        hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
        std::vector<std::string> log;
        std::vector<party::Block> blocks;
        for (int i = 0; i < 6; ++i) blocks.push_back({"P" + std::to_string(i), Quarters(12, 12)});
        REQUIRE(hud.Step(game, blocks, 0.15, &log));
        CHECK(Said(log, "(its own heart parts, " + std::to_string(slots) + " slots)"));
        for (int s = 0; s < slots; ++s) CHECK(game.Shown(HeartName(s, 0)));
        CHECK(game.panes.count(HeartName(slots, 0)) == 0);
        CHECK(game.texts["Pa_CoopMore_0/T_Text_00"] == "+" + std::to_string(6 - slots));
    }
}

// party-hud-crash-c126 (reports/party-hud-crash-c126-4.md): the game's part clone, called from the module outside a
// screen's own set-up, crashed every game that ran it. The module's HUD makes none: the pack's slots show, the rest
// go to "+N" (0.11.12's rule), and without a pack nothing is shown; not one clone call, with the game's gauge there.
TEST_CASE("party hud: run-time clones are off by default: the pack's slots and +N, no clone call") {
    FakeGame game;
    game.Hud();
    game.game_gauge = true;
    party::Hud hud;   // (the module's: AllowRunTimeClones never called)
    std::vector<party::Block> five;
    for (const char* n : {"A", "B", "C", "D", "E"}) five.push_back({n, Quarters(12, 12)});
    std::vector<std::string> log;
    REQUIRE(hud.Step(game, five, std::nullopt, &log));
    CHECK(game.made == 0);
    CHECK(game.attached == 0);
    CHECK(Said(log, "run-time gauges are off (the game's part clone crashes outside a screen's own set-up)"));
    CHECK_FALSE(Said(log, "run-time name lines can be made"));
    CHECK(game.Shown(HeartName(2, 0)));
    CHECK(game.texts["Pa_CoopMore_0/T_Text_00"] == "+2");
    // No pack (as COOP_TEST_PARTY_HUD_NO_PACK): nothing of ours, hidden, still no clone.
    FakeGame bare;
    bare.Hud();
    bare.pack = false;
    bare.game_gauge = true;
    party::Hud hud2;
    hud2.IgnorePack(false);
    std::vector<std::string> log2;
    CHECK_FALSE(hud2.Step(bare, five, std::nullopt, &log2));
    CHECK(bare.made == 0);
    CHECK(Said(log2, "the other players' hearts are hidden (nothing drawn instead)"));
}

TEST_CASE("party hud: no pack: a run-time gauge per player, none in solo, forgotten with the HUD (R2)") {
    FakeGame game;
    game.Hud();
    game.pack = false;
    game.game_gauge = true;
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<std::string> log;
    CHECK(hud.Step(game, {}, std::nullopt, &log));   // solo: the HUD is ours, nothing made
    CHECK(game.made == 0);
    const std::vector<party::Block> two = {{"A", Quarters(10, 12)}, {"B", Quarters(16, 16), true}};
    CHECK(hud.Step(game, two, std::nullopt, &log));
    CHECK(game.made == 2);   // (no pack: no text part to clone, hearts only)
    CHECK(game.attached == 2);
    CHECK(game.unlinked == 2);   // (out of the animation list: it would show Link's life)
    CHECK(game.texts.empty());
    // A: 3 hearts, 10 quarters: two full, the third empty under the part heart at 2 quarters; the rest none.
    const std::uint64_t ga = kLive + 0x10000;   // (the first gauge: after its clone)
    CHECK(game.heart_set[ga + 0x2000] == 2);
    CHECK(game.heart_set[ga + 0x2100] == 2);
    CHECK(game.heart_set[ga + 0x2200] == 1);
    CHECK(game.heart_set[ga + 0x2300] == 0);
    CHECK(game.heart_set[ga + 0x2000 + 0x100 * party::kHearts] == 2);
    CHECK(game.gauge_frame[ga + 0x2000 + 0x100 * party::kHearts] == 50);
    bool said = false;
    bool no_names = false;
    for (const auto& l : log) {
        said = said || l.find("run-time gauge 1: made") != std::string::npos;
        no_names = no_names || l.find("no run-time name lines (") != std::string::npos;
    }
    CHECK(said);
    CHECK(no_names);
    // Back to solo: hidden, nothing new made.
    CHECK(hud.Step(game, {}, std::nullopt, &log));
    CHECK(game.shown[kLive + 0x1000] == 0);
    CHECK(game.made == 2);
    // The HUD made again (scene, title): the old ones are forgotten, new ones made on the new layout.
    game.Hud(kLayout + 0x100000);
    CHECK(hud.Step(game, two, std::nullopt, &log));
    CHECK(game.made == 4);
}

TEST_CASE("party hud: the pack's slots first, run-time gauges past them, no +N while they fit (R3)") {
    FakeGame game;
    game.Hud();
    game.game_gauge = true;
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<party::Block> five;
    for (const char* n : {"A", "B", "C", "D", "E"}) five.push_back({n, Quarters(12, 12)});
    std::vector<std::string> log;
    CHECK(hud.Step(game, five, std::nullopt, &log));
    CHECK(game.made == 4);   // (3 in the pack; past them a gauge and a name line each)
    CHECK(game.Shown(HeartName(2, 0)));
    CHECK_FALSE(game.Shown("Pa_CoopMore_0"));
    // Their names on the clones of the pack's text part, as the pack's own lines (0.11.12 had every shown name).
    CHECK(game.texts["Pa_CoopLiveName_3/T_Text_00"] == "D");
    CHECK(game.texts["Pa_CoopLiveName_3/T_Text_01"] == "D");
    CHECK(game.texts["Pa_CoopLiveName_4/T_Text_00"] == "E");
    std::uint64_t root = 0;
    for (const auto& [r, n] : game.clone_names) root = n == "Pa_CoopLiveName_3" ? r : root;
    REQUIRE(root != 0);
    CHECK(game.clone_sources[root] == kLabelSource);
    CHECK(game.shown[root] == 1);
    bool said = false;
    for (const auto& l : log) said = said || l.find("run-time name line 3: made") != std::string::npos;
    CHECK(said);
    // Fewer players: the run-time lines hide with their hearts.
    CHECK(hud.Step(game, {five[0], five[1], five[2]}, std::nullopt));
    CHECK(game.shown[root] == 0);
    CHECK(game.made == 4);
}

TEST_CASE("party hud: a test's no pack (COOP_TEST_PARTY_HUD_NO_PACK): with the pack, as without it") {
    FakeGame game;
    game.Hud();
    game.game_gauge = true;
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    hud.IgnorePack(true);
    std::vector<std::string> log;
    const std::vector<party::Block> two = {{"A", Quarters(10, 12)}, {"B", Quarters(16, 16)}};
    CHECK(hud.Step(game, two, std::nullopt, &log));
    CHECK(game.made == 2);   // (two gauges, no name lines: no text part of ours found)
    CHECK(game.texts.empty());
    CHECK_FALSE(game.Shown(HeartName(0, 0)));
    bool said = false;
    for (const auto& l : log) said = said || l.find("test: the pack's panes are not looked for") != std::string::npos;
    CHECK(said);
}

TEST_CASE("party hud: no pack: the name lines from the game's MenuOverlay_00 Pa_Category_00, text only") {
    FakeGame game;
    game.pack = false;
    game.game_gauge = true;
    game.menu_overlay = true;
    game.Hud();
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<std::string> log;
    const std::vector<party::Block> two = {{"A", Quarters(10, 12)}, {"B", Quarters(16, 16)}};
    CHECK(hud.Step(game, two, std::nullopt, &log));
    CHECK(game.made == 4);   // (a gauge and a name line each)
    CHECK(game.texts["Pa_CoopLiveName_0/T_Text_00"] == "A");
    CHECK(game.texts["Pa_CoopLiveName_1/T_Text_01"] == "B");
    std::uint64_t root = 0;
    for (const auto& [r, n] : game.clone_names) root = n == "Pa_CoopLiveName_0" ? r : root;
    REQUIRE(root != 0);
    CHECK(game.clone_sources[root] == kBareSource);
    CHECK(game.shown[root] == 1);
    // Its 4 backing panes (the menu tab's pictures) hidden in the clone: the line is text only.
    int hidden = 0;
    for (const auto& [p, n] : game.clone_panes) {
        for (const char* h : party::addr::kBareHide) {
            if (n == h && p / 0x10000 == root / 0x10000 && game.shown.count(p) && game.shown[p] == 0) ++hidden;
        }
    }
    CHECK(hidden == 4);
    bool route = false, bare = false;
    for (const auto& l : log) {
        route = route || l.find("run-time name lines can be made (the game's MenuOverlay_00 Pa_Category_00)") != std::string::npos;
        bare = bare || l.find("run-time name line 0: game's Pa_Category_00: 4 of 4 backing panes hidden") != std::string::npos;
    }
    CHECK(route);
    CHECK(bare);
}

TEST_CASE("party hud: the status screen gone a frame, back with the same layout: run-time parts kept, not made again") {
    FakeGame game;
    game.pack = false;
    game.game_gauge = true;
    game.menu_overlay = true;
    game.Hud();
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<std::string> log;
    const std::vector<party::Block> two = {{"A", Quarters(10, 12)}, {"B", Quarters(16, 16)}};
    CHECK(hud.Step(game, two, std::nullopt, &log));
    REQUIRE(game.made == 4);
    REQUIRE(game.attached == 4);
    game.Put(kStatus + ui::addr::kScreenLayout, 0);   // (gone one frame)
    CHECK_FALSE(hud.Step(game, two, std::nullopt, &log));
    game.Hud();                                         // (back: the same layout)
    game.texts.clear();
    CHECK(hud.Step(game, {{"C", Quarters(10, 12)}, two[1]}, std::nullopt, &log));
    CHECK(game.made == 4);       // (no second clone)
    CHECK(game.attached == 4);   // (nothing attached again)
    CHECK(game.texts["Pa_CoopLiveName_0/T_Text_00"] == "C");   // (the kept line is still its slot's)
    bool kept = false;
    for (const auto& l : log) kept = kept || l.find("the same layout again: 4 run-time part(s) kept") != std::string::npos;
    CHECK(kept);
    // A new layout (scene, title): made again on it.
    game.Hud(kLayout + 0x100000);
    CHECK(hud.Step(game, two, std::nullopt, &log));
    CHECK(game.made == 8);
}

// party-hud-crash-c126 (run party-hud-crash-c126-05b2d8c7, probe 967cac67d): the status screen is listed from the
// title on and Sequence_IsLoadingBanc goes off as the save's load starts, but the screen first runs at the first play
// frame (patch 0018: its update and the HUD screen's). Between the two (A 534 frames, B 268) nothing is called into
// it; once the screen runs, or the game's HUD screen has run, the HUD is found.
TEST_CASE("party hud: a listed status screen that has not run yet is not called into") {
    FakeGame game;
    game.Hud();
    ui::SetSceneLoading(true);    // the title (Sequence_IsLoadingBanc on through it) ...
    ui::SetSceneLoading(false);   // ... and the save's load starts: the flag off,
    ui::SetRunningScreens(std::vector<std::uint64_t>{}, false);   // neither the status nor the HUD screen run yet
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<std::string> log;
    const std::vector<party::Block> blocks = {{"Saria", Quarters(12, 12)}, {"Ruto", Quarters(8, 12)},
                                              {"Nabooru", Quarters(4, 12)}, {"Impa", Quarters(16, 16)}};
    const int calls = game.calls;
    CHECK_FALSE(hud.Step(game, blocks, 0.15, &log));
    CHECK(game.calls == calls);   // no Find, no MakeLive: no game function was handed its layout
    CHECK(ui::ScreenByVtable(game, party::addr::kStatusVtable) == 0);
    CHECK(ui::ScreenLayout(game, kStatus) == 0);
    ui::SetRunningScreens(std::vector<std::uint64_t>{0x1234}, false);   // another screen runs: still not this one
    CHECK_FALSE(hud.Step(game, blocks, 0.15, &log));
    CHECK(game.calls == calls);
    ui::SetRunningScreens(std::vector<std::uint64_t>{kStatus}, false);  // its own update ran: live
    CHECK(ui::ScreenLayout(game, kStatus) == kLayout);
    CHECK(hud.Step(game, blocks, 0.15, &log));
    ui::SetRunningScreens(std::vector<std::uint64_t>{}, std::nullopt);  // an exe whose patch 0018 does not say: not live
    CHECK(ui::ScreenLayout(game, kStatus) == 0);
    ui::SetRunningScreens(std::vector<std::uint64_t>{}, true);          // the game's HUD screen ran: the play UI is up
    CHECK(ui::GameUiUp());
    CHECK(ui::ScreenLayout(game, kStatus) == kLayout);
    ui::SetSceneLoading(true);
    CHECK_FALSE(ui::GameUiUp());
    CHECK(ui::ScreenLayout(game, kStatus) == 0);
    ui::SetSceneLoading(false);
    CHECK_FALSE(ui::GameUiUp());   // (up again only once the HUD screen ran after the load)
    ui::SetRunningScreens(std::vector<std::uint64_t>{}, true);
}

// party-hud-crash-c126-6 (run party-hud-bridge-ece368dd-2, both games): the pause menu's Title Screen goes from play
// to the title with no scene load (Sequence_IsLoadingBanc stays off), so the play UI stayed 'up' at the title and every
// listed screen counted as live there. The title's menu up is no play scene: nothing is live until the HUD runs after it.
TEST_CASE("party hud: the title reached from play (no scene load) is no play scene") {
    FakeGame game;
    game.Hud();
    ui::SetSceneLoading(false);
    ui::SetRunningScreens(std::vector<std::uint64_t>{}, true);   // in play: the game's HUD screen ran
    REQUIRE(ui::GameUiUp());
    REQUIRE(ui::ScreenLayout(game, kStatus) == kLayout);
    ui::SetTitleUp(true);   // the pause menu's Title Screen: the title's menu is up, no scene load
    CHECK(ui::PlayUiDown());
    CHECK_FALSE(ui::SceneLoading());
    CHECK_FALSE(ui::GameUiUp());
    CHECK_FALSE(ui::ScreenLive(kStatus));
    CHECK(ui::ScreenLayout(game, kStatus) == 0);
    CHECK(ui::ScreenByVtable(game, party::addr::kStatusVtable) == 0);
    ui::SetRunningScreens(std::vector<std::uint64_t>{kStatus}, true);   // (even a stale 'ran' word: not live at the title)
    CHECK_FALSE(ui::GameUiUp());
    CHECK(ui::ScreenLayout(game, kStatus) == 0);
    party::Hud hud;
    hud.AllowRunTimeClones(true);
    std::vector<std::string> log;
    const int calls = game.calls;
    CHECK_FALSE(hud.Step(game, {{"Saria", Quarters(12, 12)}}, 0.15, &log));
    CHECK(game.calls == calls);   // no game function was handed a layout at the title
    ui::SetTitleUp(false);   // the save's load from the title: the title's menu gone ...
    CHECK_FALSE(ui::GameUiUp());   // ... the play UI up again only once the HUD screen ran after it
    ui::SetRunningScreens(std::vector<std::uint64_t>{}, true);
    CHECK(ui::GameUiUp());
    CHECK(ui::ScreenLayout(game, kStatus) == kLayout);
}

// party-hud-crash-c126: at 'menu -> loading' the status screen was still listed while the scene loader reused its
// layout's memory, and the next layout lookup walked into a scene object (both games crashed). While the game's scene
// load (Sequence_IsLoadingBanc) is on, nothing of a screen's layout is called into; the HUD is found again after it.
TEST_CASE("party hud: a scene load calls nothing into the HUD's layout") {
    FakeGame game;
    game.Hud();
    party::Hud hud;
    hud.AllowRunTimeClones(true);   // (the clone path's tests: off in the module)
    std::vector<std::string> log;
    const std::vector<party::Block> blocks = {{"Saria", Quarters(12, 12)}};
    REQUIRE(hud.Step(game, blocks, 0.15, &log));
    ui::SetSceneLoading(true);
    // The layout's memory reused by the scene loader (as the crash's 'Land_kokiri_Branch_A_03.foliage' object).
    game.Hud(kLayout + 0x200000);
    const int calls = game.calls;
    CHECK_FALSE(hud.Step(game, blocks, 0.15, &log));
    CHECK_FALSE(hud.native());
    CHECK(game.calls == calls);   // no game function was handed the layout
    CHECK(ui::ScreenByVtable(game, party::addr::kStatusVtable) == 0);
    CHECK(ui::ScreenLayout(game, kStatus) == 0);
    CHECK(ui::PaneTextBox(game, kStatus, 0x1234) == 0);
    CHECK(game.calls == calls);
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("a scene load began") != std::string::npos; }));
    ui::SetSceneLoading(false);
    CHECK(ui::ScreenLayout(game, kStatus) == 0);   // (the load forgot the HUD screen's run: not live yet)
    CHECK_FALSE(hud.Step(game, blocks, 0.15, &log));
    CHECK(game.calls == calls);
    ui::SetRunningScreens(std::vector<std::uint64_t>{}, true);   // the game's HUD screen ran after the load
    CHECK(ui::ScreenLayout(game, kStatus) == kLayout + 0x200000);
    CHECK(hud.Step(game, blocks, 0.15, &log));   // found again on the new layout
    CHECK(hud.native());
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("the scene load ended") != std::string::npos; }));
}

// hud-fit-0120 (gate-hud shot of 2f734963a, A: the longest line ran 81 px off the screen's left edge, every line was
// centred over left-aligned hearts). Every line starts at the hearts' left edge and ends inside kLabelRoom, at the
// longest a line can be (16 wide letters, voice words, the longest place) and in any language.
namespace {
// The line's left and right edges in the parent's units (the pane's middle -/+ half the fitted text).
std::array<float, 2> LineEdges(const std::string& line) {
    const std::string shown = party::FitLabel(line);
    const float half = static_cast<float>(CoopNativeUi::Width(shown)) * party::kLabelEm / 200.0f;
    return {party::LabelX(shown) - half, party::LabelX(shown) + half};
}
}  // namespace

TEST_CASE("party hud fit: every line starts at the hearts' left edge and stays on the picture") {
    CoopNativeUi::ResetFontWidths();   // (the guesses: never narrower than the game's font)
    const std::string longest = std::string(16, 'W') + " (talking)  Inside Ganondorf's Castle";
    const std::vector<std::string> lines = {"You (talking)", "Twenty", "Three", "+1", longest,
                                            std::string(16, 'W') + " (muted)", longest + " 13/40",
                                            std::string(200, 'W'), "\xe4\xbd\xa0\xe5\xa5\xbd (talking)  " + longest};
    for (const auto& b : party::TestBlocks(0)) {
        CHECK(party::FitLabel(b.text).size() > 0);
    }
    for (const auto& line : lines) {
        CAPTURE(line);
        const auto [left, right] = LineEdges(line);
        CHECK(left == doctest::Approx(party::kLabelLeft));             // left-aligned on the hearts
        CHECK(left >= 0.0f);                                            // never off the left edge
        CHECK(right <= party::kLabelLeft + party::kLabelRoom + 0.5f);   // never past the room (30% of the picture)
        CHECK(right <= 3840.0f);
    }
    // A short line shows as it is; the longest is cut at its end with "...", its name and voice words kept.
    CHECK(party::FitLabel("Twenty") == "Twenty");
    const std::string cut = party::FitLabel(longest);
    CHECK(cut.size() >= 3);
    CHECK(cut.substr(cut.size() - 3) == "...");
    CHECK(cut.rfind(std::string(16, 'W') + " (talking)", 0) == 0);
    CHECK(party::FitLabel(std::string(16, 'W') + " (talking)") == std::string(16, 'W') + " (talking)");
}

TEST_CASE("party hud fit: lines are left-aligned on the picture in the fake game, blocks apart") {
    CoopNativeUi::ResetFontWidths();
    FakeGame game;
    game.Hud();
    party::Hud hud;
    std::vector<std::string> log;
    REQUIRE(hud.Step(game, party::TestBlocks(0), std::nullopt, &log, 0, party::TestOwn()));
    for (const char* pane : {"Pa_CoopSelf_0", "Pa_CoopName_0", "Pa_CoopName_1", "Pa_CoopName_2", "Pa_CoopMore_0"}) {
        CAPTURE(pane);
        const std::string shown = game.texts[std::string(pane) + "/T_Text_00"];
        REQUIRE_FALSE(shown.empty());
        const float half = static_cast<float>(CoopNativeUi::Width(shown)) * party::kLabelEm / 200.0f;
        CHECK(game.Where(pane)[0] - half == doctest::Approx(party::kLabelLeft));
        CHECK(game.Where(pane)[0] + half <= party::kLabelLeft + party::kLabelRoom + 0.5f);
    }
    // A name sits nearer its own hearts than the hearts of the block above it.
    const float name1 = game.Where("Pa_CoopName_1")[1];
    const float own_row = game.Where(HeartName(1, 0))[1];
    const float above_row = game.Where(HeartName(0, 10))[1];   // (Twenty: 20 hearts, two rows)
    CHECK(name1 - own_row < above_row - name1);
}
