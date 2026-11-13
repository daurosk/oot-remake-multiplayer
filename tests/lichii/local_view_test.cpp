// Tests for split screen's world-layer viewport (lichii/hook/coop_local_view.h).
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "coop_local_view.h"

using CoopLocalView::Rect;
using CoopLocalView::Views;
using CoopLocalView::Want;
using CoopLocalLayout::Split;

namespace {

constexpr std::uint64_t kBase = 0x80020000;
constexpr std::uint64_t kRendererAt = 0x9000000;
constexpr std::uint64_t kArray = 0x9100000;
constexpr std::uint64_t kLayer = 0x9200000;
constexpr std::uint64_t kProjection = 0x9300000;

// Guest memory, byte by byte; anything never set is unmapped.
struct Game {
    std::map<std::uint64_t, std::uint8_t> bytes;
    std::vector<std::string> log;
    int writes = 0;

    void Put(std::uint64_t a, const void* in, std::size_t n) {
        for (std::size_t i = 0; i < n; i++) bytes[a + i] = static_cast<const std::uint8_t*>(in)[i];
    }
    template <class T> void Set(std::uint64_t a, T v) { Put(a, &v, sizeof v); }
    template <class T> T Get(std::uint64_t a) const {
        T v{};
        for (std::size_t i = 0; i < sizeof v; i++) reinterpret_cast<std::uint8_t*>(&v)[i] = bytes.at(a + i);
        return v;
    }
    bool Read(std::uint64_t a, void* out, std::size_t n) const {
        for (std::size_t i = 0; i < n; i++) {
            const auto it = bytes.find(a + i);
            if (it == bytes.end()) return false;
            static_cast<std::uint8_t*>(out)[i] = it->second;
        }
        return true;
    }
    Game() {
        Set<std::uint64_t>(kBase + CoopLocalView::kRenderer, kRendererAt);
        Set<std::uint32_t>(kRendererAt + CoopLocalView::kLayerCount, 32);
        Set<std::uint64_t>(kRendererAt + CoopLocalView::kLayerArray, kArray);
        Set<std::uint64_t>(kArray, kLayer);
        Set(kLayer + CoopLocalView::kLayerRect, Rect{0, 0, 1280, 720});
        Set<std::uint64_t>(kLayer + CoopLocalView::kLayerProjection, kProjection);
        Set<std::uint8_t>(kProjection + CoopLocalView::kProjectionDirty, 0);
        Set<float>(kProjection + CoopLocalView::kProjectionRatio, 1280.0f / 720.0f);
    }
    CoopLocalView::Memory Memory() {
        CoopLocalView::Memory m;
        m.main_base = [] { return kBase; };
        m.read = [this](std::uint64_t a, void* out, std::size_t n) { return Read(a, out, n); };
        m.write = [this](std::uint64_t a, const void* in, std::size_t n) {
            if (!Read(a, std::vector<std::uint8_t>(n).data(), n)) return false;
            Put(a, in, n);
            writes++;
            return true;
        };
        m.log = [this](const std::string& line) { log.push_back(line); };
        return m;
    }
    Rect LayerRect() const { return Get<Rect>(kLayer + CoopLocalView::kLayerRect); }
    float Ratio() const { return Get<float>(kProjection + CoopLocalView::kProjectionRatio); }
};

}  // namespace

TEST_CASE("local views: one player writes nothing, two side by side put player 1 in the left half, as the game's node does") {
    Game g;
    Views views(g.Memory());
    views.Step(Want{1, Split::Side});
    views.Step(Want{0, Split::Side});
    CHECK(g.writes == 0);
    views.Step(Want{2, Split::Side});
    CHECK(g.LayerRect() == Rect{0, 0, 640, 720});
    CHECK(g.Ratio() == doctest::Approx(640.0 / 720.0));
    CHECK(g.Get<std::uint8_t>(kProjection + CoopLocalView::kProjectionDirty) == 1);
    const int writes = g.writes;
    views.Step(Want{2, Split::Side});   // (already right: nothing written)
    CHECK(g.writes == writes);
}

TEST_CASE("local views: stacked, quarters, the game setting it back, and the whole picture given back once") {
    Game g;
    Views views(g.Memory());
    views.Step(Want{2, Split::Stacked});
    CHECK(g.LayerRect() == Rect{0, 0, 1280, 360});
    CHECK(g.Ratio() == doctest::Approx(1280.0 / 360.0));
    views.Step(Want{4, Split::Side});
    CHECK(g.LayerRect() == Rect{0, 0, 640, 360});
    // A scene sets its own viewport back: put back the next frame.
    g.Set(kLayer + CoopLocalView::kLayerRect, Rect{0, 0, 1280, 720});
    views.Step(Want{4, Split::Side});
    CHECK(g.LayerRect() == Rect{0, 0, 640, 360});
    // Local co-op off: the whole picture again, then nothing more is written.
    views.Step(Want{0, Split::Side});
    CHECK(g.LayerRect() == Rect{0, 0, 1280, 720});
    CHECK(g.Ratio() == doctest::Approx(1280.0 / 720.0));
    const int writes = g.writes;
    views.Step(Want{0, Split::Side});
    views.Step(Want{1, Split::Side});
    CHECK(g.writes == writes);
}

TEST_CASE("local views: the whole picture follows the game while nobody splits, a new layer, no layer, a broken rect") {
    Game g;
    Views views(g.Memory());
    g.Set(kLayer + CoopLocalView::kLayerRect, Rect{0, 0, 1920, 1080});   // (the game's own picture size)
    views.Step(Want{1, Split::Side});
    views.Step(Want{2, Split::Side});
    CHECK(g.LayerRect() == Rect{0, 0, 960, 1080});
    REQUIRE(views.Whole().has_value());
    CHECK(*views.Whole() == Rect{0, 0, 1920, 1080});
    // A new world layer (a reload): its own rect is its whole picture.
    constexpr std::uint64_t kOther = 0x9400000;
    g.Set(kOther + CoopLocalView::kLayerRect, Rect{0, 0, 1280, 720});
    g.Set<std::uint64_t>(kOther + CoopLocalView::kLayerProjection, 0);   // (no projection yet: the rect alone)
    g.Set<std::uint64_t>(kArray, kOther);
    views.Step(Want{2, Split::Side});
    CHECK(g.Get<Rect>(kOther + CoopLocalView::kLayerRect) == Rect{0, 0, 640, 720});
    // No layer, or a rect that is not a rect: nothing written.
    g.Set<std::uint64_t>(kArray, 0);
    const int writes = g.writes;
    views.Step(Want{2, Split::Side});
    g.Set<std::uint64_t>(kArray, kOther);
    g.Set(kOther + CoopLocalView::kLayerRect, Rect{0, 0, 0, 0});
    views.Step(Want{2, Split::Side});
    CHECK(g.writes == writes);
}
