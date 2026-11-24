// SPDX-License-Identifier: GPL-3.0-or-later
// The HUD research probe (hookshot/src/coop_hud_probe.h) on a pretend game.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop_hud_probe.h"

namespace probe = CoopHudProbe;
namespace ui = CoopNativeUi;

namespace {

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kManager = 0x96000000, kScreens = 0x97000000, kStatus = 0x93000000, kLayout = 0x94000000;
constexpr std::uint64_t kPane = 0x98000000, kKin = 0x98100000, kArray = 0x98200000, kPaneVtable = 0x84d00000;
constexpr std::uint64_t kGauge = 0x99000000, kState = 0x99100000, kStrings = 0x9a000000;

std::uint64_t At(std::uint64_t main_offset) { return kMain + main_offset; }

struct FakeGame final : probe::Game {
    std::map<std::uint64_t, std::uint8_t> mem;
    std::map<std::uint64_t, std::string> strings;
    std::map<std::string, std::uint64_t> anims, panes;
    std::map<std::uint64_t, std::int32_t> last;   // player -> last frame
    std::vector<std::pair<std::uint64_t, float>> stops;
    std::uint64_t next = kStrings;

    bool Read(std::uint64_t a, void* out, std::size_t n) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = mem.find(a + i);
            if (it == mem.end()) return false;
            o[i] = it->second;
        }
        return true;
    }
    bool Write(std::uint64_t a, const void* in, std::size_t n) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < n; ++i) mem[a + i] = p[i];
        return true;
    }
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x) override {
        const auto name = [&] { return strings.count(x[1]) != 0 ? strings[x[1]] : std::string(); };
        if (f == At(probe::addr::kAnimFind)) {
            CHECK(x[0] == kLayout);
            const auto it = anims.find(name());
            return it == anims.end() ? 0 : it->second;
        }
        if (f == At(probe::addr::kPaneByName)) {
            const auto it = panes.find(name());
            return it == panes.end() ? 0 : it->second;
        }
        if (f == At(probe::addr::kAnimLastFrame)) {
            const auto it = last.find(x[0]);
            return it == last.end() ? 0 : static_cast<std::uint64_t>(static_cast<std::uint32_t>(it->second));
        }
        return std::nullopt;
    }
    std::uint64_t Main() const override { return kMain; }
    std::optional<std::uint64_t> CallFloat(std::uint64_t f, std::uint64_t x0, float s0) override {
        if (f != At(probe::addr::kAnimStopAt)) return std::nullopt;
        stops.emplace_back(x0, s0);
        return 0;
    }
    std::uint64_t String(std::string_view text) override {
        const std::uint64_t at = next;
        next += 0x100;
        strings[at] = std::string(text);
        return at;
    }
    void Put(std::uint64_t a, std::uint64_t v) { Write(a, &v, sizeof v); }
};

// The HUD in the UI manager's list, its heart pane in the screen, a pane of its kind, and an array of three.
struct World {
    FakeGame g;
    World() {
        g.Put(At(ui::addr::kUiManagerGot), kManager - 8);
        g.Put(kManager - 8, kManager);
        const std::uint32_t one = 1;
        g.Write(kManager + ui::addr::kUiCount, &one, sizeof one);
        g.Put(kManager + ui::addr::kUiScreens, kScreens);
        g.Put(kScreens, kStatus);
        g.Put(kStatus, At(probe::addr::kStatusVtable));
        g.Put(kStatus + ui::addr::kScreenLayout, kLayout);
        Open(false);
        g.Put(kPane, kPaneVtable);
        g.Put(kKin, kPaneVtable);
        for (std::uint64_t k = 0; k < 3; ++k) {
            g.Put(kArray + 8 * k, 0x98300000 + 0x100 * k);
            g.Put(0x98300000 + 0x100 * k, kPaneVtable);
        }
        g.Put(kStatus + 0x100, kPane);
        g.Put(kStatus + 0x200, kKin);
        g.Put(kStatus + 0x300, kArray);
        g.panes["Pa_HeartGauge_00"] = kPane;
        g.anims["Pa_HeartGauge_00/Pa_Gauge_00/HeartGauge"] = kGauge;
        g.anims["Pa_HeartGauge_00/Pa_NoGauge_00/HeartState"] = kState;
        g.Put(kGauge + probe::addr::kAnimPlayer, kGauge + 0x1000);
        g.Put(kState + probe::addr::kAnimPlayer, kState + 0x1000);
        g.last[kGauge + 0x1000] = 40;
        g.last[kState + 0x1000] = 2;
    }
    void Open(bool open) {
        const std::uint8_t state = open ? ui::addr::kStateOpen : 0;
        g.Write(kStatus + ui::addr::kScreenState, &state, 1);
    }
};

bool Has(const std::vector<std::string>& lines, const std::string& part) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(part) != std::string::npos; });
}

}  // namespace

TEST_CASE("hud probe: the holds: a gauge at its quarters, a state at each frame") {
    const auto g = probe::HoldsFor("Pa_HeartGauge_00/Pa_Gauge_00/HeartGauge", 1, 40);
    REQUIRE(g.size() == 5);
    CHECK(g[0].frame == 0.0f);
    CHECK(g[1].frame == 10.0f);
    CHECK(g[4].frame == 40.0f);
    const auto s = probe::HoldsFor("x/HeartState", 1, 2);
    REQUIRE(s.size() == 3);
    CHECK(s[2].frame == 2.0f);
    CHECK(probe::HoldsFor("x/HeartState", 1, 100).size() == 8);
    CHECK(probe::HoldsFor("x/HeartState", 0, 2).empty());
    CHECK(probe::HoldsFor("x/HeartState", 1, -1).empty());
}

TEST_CASE("hud probe: waits for the HUD, asks every path, finds the heart's panes, then holds each frame in turn") {
    World w;
    probe::Probe p;
    auto lines = p.Step(w.g, 1000);
    CHECK(Has(lines, "coop: hud probe: waiting for PlayerStatus_00 open"));
    CHECK(p.Step(w.g, 1100).empty());   // (said once)
    w.Open(true);
    lines = p.Step(w.g, 2000);
    CHECK(Has(lines, "anim Pa_HeartGauge_00/Pa_Gauge_00/HeartGauge: found 0x99000000 last frame 40"));
    CHECK(Has(lines, "anim Pa_HeartGauge_00/Pa_NoGauge_00/HeartState: found 0x99100000 last frame 2"));
    CHECK(Has(lines, "anim Pa_HeartGauge_00/Pa_Gauge_00/Pinch: not found"));
    CHECK(Has(lines, "pane Pa_HeartGauge_00: 0x98000000 vtable 0x84d00000"));
    CHECK(Has(lines, "pane N_HeartPos_00: not found"));
    CHECK(Has(lines, "screen+0x100 = Pa_HeartGauge_00"));
    CHECK(Has(lines, "screen+0x200 -> a pane like Pa_HeartGauge_00"));
    CHECK(Has(lines, "screen+0x300 -> 0x98200000: 3 panes like Pa_HeartGauge_00"));
    CHECK(Has(lines, "pane Pa_HeartGauge_00: 1 panes of its kind straight in the screen"));
    CHECK(Has(lines, "8 holds"));
    CHECK(w.g.stops.empty());
    // Nothing held before kStartMs.
    CHECK(p.Step(w.g, 2000 + probe::kStartMs - 1).empty());
    CHECK(w.g.stops.empty());
    lines = p.Step(w.g, 2000 + probe::kStartMs);
    CHECK(Has(lines, "hold 1/8 Pa_HeartGauge_00/Pa_Gauge_00/HeartGauge frame 0.00"));
    CHECK(p.Step(w.g, 2000 + probe::kStartMs + 16).empty());   // set again each frame, said once
    REQUIRE(w.g.stops.size() == 2);
    CHECK(w.g.stops[1] == std::pair<std::uint64_t, float>{kGauge, 0.0f});
    lines = p.Step(w.g, 2000 + probe::kStartMs + probe::kHoldMs);
    CHECK(Has(lines, "hold 2/8 Pa_HeartGauge_00/Pa_Gauge_00/HeartGauge frame 10.00"));
    lines = p.Step(w.g, 2000 + probe::kStartMs + 5 * probe::kHoldMs);
    CHECK(Has(lines, "hold 6/8 Pa_HeartGauge_00/Pa_NoGauge_00/HeartState frame 0.00"));
    CHECK(w.g.stops.back().first == kState);
    lines = p.Step(w.g, 2000 + probe::kStartMs + 8 * probe::kHoldMs);
    CHECK(Has(lines, "coop: hud probe: done"));
    CHECK(p.done());
    const auto n = w.g.stops.size();
    CHECK(p.Step(w.g, 2000 + probe::kStartMs + 9 * probe::kHoldMs).empty());
    CHECK(w.g.stops.size() == n);   // done: nothing more set
}

TEST_CASE("hud probe: no animations found: nothing held, done at once") {
    World w;
    w.g.anims.clear();
    w.Open(true);
    probe::Probe p;
    const auto lines = p.Step(w.g, 0);
    CHECK(Has(lines, "done (nothing to hold)"));
    CHECK(p.done());
    CHECK(p.Step(w.g, probe::kStartMs).empty());
    CHECK(w.g.stops.empty());
}
