// SPDX-License-Identifier: GPL-3.0-or-later
// The name tags on the game's balloons (hookshot/src/coop_native_tags.h): the plan, and the steps against a made-up
// game memory (the UI manager, the balloon screen and its parts) that records the game calls.
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop_native_tags.h"

namespace {

namespace T = CoopNativeTags;
constexpr std::uint64_t kBase = 0x80020000;

struct FakeGame final : T::Game {
    std::map<std::uint64_t, std::uint8_t> mem;
    struct Called {
        std::uint64_t f;
        std::array<std::uint64_t, 5> x;
    };
    std::vector<Called> calls;
    std::uint64_t next_malloc = 0x90000000;
    bool malloc_ok = true;
    std::uint64_t text_pane = 0;   // what kPaneByName returns

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
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x) override {
        calls.push_back({f, x});
        if (f == kBase + T::addr::kMalloc) return malloc_ok ? next_malloc : 0;
        if (f == kBase + T::addr::kPartRootPane) return x[0] - 0x40 + 0x1000;   // a part's root pane: part + 0x1000
        if (f == kBase + T::addr::kPaneByName) return text_pane;
        return 0;
    }
    template <typename V>
    void Put(std::uint64_t a, V v) { Write(a, &v, sizeof v); }
    template <typename V>
    V Get(std::uint64_t a) {
        V v{};
        Read(a, &v, sizeof v);
        return v;
    }
    std::size_t Count(std::uint64_t offset) const {
        std::size_t n = 0;
        for (const auto& c : calls) n += c.f == kBase + offset;
        return n;
    }
    std::string Str(std::uint64_t a) {
        std::string s;
        for (char ch; (ch = Get<char>(a)) != 0; ++a) s.push_back(ch);
        return s;
    }
};

constexpr std::uint64_t kManager = 0xa0000000, kScreens = 0xa0001000, kScreen = 0xa0002000;
std::uint64_t PartObj(std::size_t i) { return 0xb0000000 + i * 0x10000; }

// A game with the UI manager holding two screens, the second the balloon screen with its 16 parts.
void SetUp(FakeGame& g) {
    g.Put<std::uint64_t>(kBase + T::addr::kUiManagerGot, 0xa0000f00);
    g.Put<std::uint64_t>(0xa0000f00, kManager);
    g.Put<std::uint32_t>(kManager + T::addr::kUiCount, 2);
    g.Put<std::uint64_t>(kManager + T::addr::kUiScreens, kScreens);
    g.Put<std::uint64_t>(kScreens, 0xa0003000);
    g.Put<std::uint64_t>(0xa0003000, 0x1234);
    g.Put<std::uint64_t>(kScreens + 8, kScreen);
    g.Put<std::uint64_t>(kScreen, kBase + T::addr::kBalloonVtable);
    for (std::size_t i = 0; i < T::addr::kParts; ++i) {
        g.Put<std::uint64_t>(kScreen + T::addr::kPartTable + i * T::addr::kPartStride, PartObj(i));
    }
}

}  // namespace

TEST_CASE("native tags: names are cleaned of the game's tag byte and controls, cut at a whole character") {
    CHECK(T::Clean("Sa\x0E\x01ria", 48) == "Saria");
    CHECK(T::Clean("a\tb", 48) == "a b");
    CHECK(T::Clean("\xC3\xA9t\xC3\xA9", 4) == "\xC3\xA9t");  // "été" cut before the second é
    CHECK(T::Clean("\xFF" "x", 48) == "?x");
}

TEST_CASE("native tags: a scene pixel as the layout's point (3840 x 2160 canvas, centre 0, y up)") {
    auto p = T::LayoutPoint(640, 360, 1280, 720);
    CHECK(p[0] == doctest::Approx(0.0f));
    CHECK(p[1] == doctest::Approx(0.0f));
    p = T::LayoutPoint(0, 0, 1280, 720);
    CHECK(p[0] == doctest::Approx(-1920.0f));
    CHECK(p[1] == doctest::Approx(1080.0f));
    p = T::LayoutPoint(1280, 720, 1280, 720);
    CHECK(p[0] == doctest::Approx(1920.0f));
    CHECK(p[1] == doctest::Approx(-1080.0f));
    p = T::LayoutPoint(960, 540, 1280, 720);   // a quarter in from the bottom right
    CHECK(p[0] == doctest::Approx(960.0f));
    CHECK(p[1] == doctest::Approx(-540.0f));
}

TEST_CASE("native tags: the nearest get the last parts first, at most 15, the unnamed and faded left out") {
    std::vector<T::Spot> far_to_near;
    for (int i = 0; i < 20; ++i) far_to_near.push_back({"P" + std::to_string(i), 100.0f, 100.0f, 1.0f, 0x112233});
    far_to_near[19].alpha = 0;        // the nearest: faded out
    far_to_near[18].name = "\x0E";    // nothing left once cleaned
    far_to_near[17].x = 5000.0f;      // off the canvas
    const auto plan = T::PlanParts(far_to_near, 1280, 720);
    REQUIRE(plan.size() == T::kMaxTags);
    CHECK(plan[0].name == "P16");
    CHECK(plan[0].part == 15);
    CHECK(plan[14].part == 1);   // part 0 stays the game's
    CHECK(plan[14].name == "P2");
    CHECK(plan[0].rgb == 0x112233);
    CHECK(T::PlanParts(far_to_near, 0, 720).empty());
}

TEST_CASE("native tags: the balloon screen found by its vtable among the UI manager's screens") {
    FakeGame g;
    CHECK(T::ScreenByVtable(g, T::addr::kBalloonVtable) == 0);   // no manager yet
    SetUp(g);
    CHECK(T::ScreenByVtable(g, T::addr::kBalloonVtable) == kScreen);
    CHECK(T::ScreenByVtable(g, 0x42) == 0);
}

TEST_CASE("native tags: a pane moved as f_8022a3e0 does") {
    FakeGame g;
    const std::uint64_t pane = 0xc0000000;
    g.Put<std::uint32_t>(pane + T::addr::kPaneFlags, 1u << 8);
    CHECK_FALSE(T::MovePane(g, pane, {10, 20}));   // placed another way: left alone
    CHECK(g.Get<float>(pane + T::addr::kPaneXY) == 0.0f);
    g.Put<std::uint32_t>(pane + T::addr::kPaneFlags, 1u << 20);
    g.Put<std::uint32_t>(pane, 0);
    g.Put<std::uint32_t>(pane + 4, 0x40);
    CHECK(T::MovePane(g, pane, {10, 20}));
    CHECK(g.Get<float>(pane + T::addr::kPaneXY) == 10.0f);
    CHECK(g.Get<float>(pane + T::addr::kPaneXY + 4) == 20.0f);
    CHECK(g.Get<std::uint32_t>(pane) == 0x40);   // the dirty bit set as the game does
    CHECK(g.Count(T::addr::kPaneMarkA) == 1);
    CHECK(g.Count(T::addr::kPaneMarkB) == 1);
    g.calls.clear();
    CHECK(T::MovePane(g, pane, {10, 20}));   // same place: nothing told
    CHECK(g.calls.empty());
}

TEST_CASE("native tags: balloons named, coloured, moved and shown, hidden again when no longer needed") {
    FakeGame g;
    T::Balloons b;
    // Solo or before a load: no screen, nothing touched, no memory asked.
    CHECK_FALSE(b.Step(g, {{"Saria", 640, 300, 1.0f, 0xFF0000}}, 1280, 720));
    CHECK(g.Count(T::addr::kMalloc) == 0);
    SetUp(g);
    // No tags: no memory asked either.
    CHECK(b.Step(g, {}, 1280, 720));
    CHECK(g.Count(T::addr::kMalloc) == 0);

    const std::uint64_t text = 0xd0000000;
    g.text_pane = text;
    g.Put<std::uint32_t>(text + T::addr::kPaneFlags, 2u << 22);   // a text pane
    g.Put<std::uint64_t>(text + 8, text + 0x100);
    g.Put<std::uint64_t>(text + 0x108, 0xd0001000);               // its text box
    REQUIRE(b.Step(g, {{"Mido", 320, 300, 1.0f, 0x00FF00}, {"Saria", 640, 300, 1.0f, 0xFF0000}}, 1280, 720));
    CHECK(g.Count(T::addr::kMalloc) == 1);
    CHECK(b.shown() == 2);
    // Saria (the nearest) on part 15: her name from the block's slot 15, as the game's text setter takes it.
    const std::uint64_t block = g.next_malloc;
    const std::uint64_t slot = block + 15 * T::Balloons::kSlotBytes;
    CHECK(g.Get<std::uint64_t>(slot) == slot + 24);
    CHECK(g.Get<std::uint32_t>(slot + 8) == 5);
    CHECK(g.Get<std::int64_t>(slot + 16) == -1);
    CHECK(g.Str(slot + 24) == "Saria");
    bool named = false;
    for (const auto& c : g.calls) {
        if (c.f == kBase + T::addr::kSetPaneText && c.x[0] == PartObj(15)) {
            named = c.x[2] == slot && g.Str(c.x[1]) == "T_Text_00" && c.x[3] == 1;
        }
    }
    CHECK(named);
    // Her colour on both of the text box's colours.
    CHECK(g.Count(T::addr::kTextColour0) == 2);
    CHECK(g.Count(T::addr::kTextColour1) == 2);
    const std::uint64_t rgba = block + T::Balloons::kSlotBytes * 16 + 15 * 16;
    CHECK(g.Get<float>(rgba) == 1.0f);
    CHECK(g.Get<float>(rgba + 4) == 0.0f);
    // Her part's root pane at her layout point, shown.
    const std::uint64_t root = PartObj(15) + 0x1000;
    CHECK(g.Get<float>(root + T::addr::kPaneXY) == doctest::Approx(0.0f));
    CHECK(g.Get<float>(root + T::addr::kPaneXY + 4) == doctest::Approx(1080.0f - 300.0f / 720.0f * 2160.0f));
    std::size_t shows = 0;
    for (const auto& c : g.calls) shows += c.f == kBase + T::addr::kPaneShow && c.x[1] == 1;
    CHECK(shows == 2);

    // Next frame, only Mido: he takes part 15 (renamed), part 14 is hidden; nothing new asked of the game's memory.
    g.calls.clear();
    REQUIRE(b.Step(g, {{"Mido", 320, 300, 1.0f, 0x00FF00}}, 1280, 720));
    CHECK(g.Count(T::addr::kMalloc) == 0);
    CHECK(b.shown() == 1);
    CHECK(g.Str(slot + 24) == "Mido");
    bool hid14 = false;
    for (const auto& c : g.calls) hid14 |= c.f == kBase + T::addr::kPaneShow && c.x[0] == PartObj(14) + 0x1000 && c.x[1] == 0;
    CHECK(hid14);
    // The same again: no new name and no new colour.
    g.calls.clear();
    b.Step(g, {{"Mido", 320, 300, 1.0f, 0x00FF00}}, 1280, 720);
    CHECK(g.Count(T::addr::kSetPaneText) == 0);
    CHECK(g.Count(T::addr::kTextColour0) == 0);
    b.HideAll(g);
    CHECK(b.shown() == 0);
}

TEST_CASE("native tags: no colour on a pane that is not a text pane, no memory: said, nothing shown") {
    FakeGame g;
    SetUp(g);
    g.text_pane = 0xd0000000;   // flags 0: a null pane
    T::Balloons b;
    REQUIRE(b.Step(g, {{"Saria", 640, 300, 1.0f, 0xFF0000}}, 1280, 720));
    CHECK(g.Count(T::addr::kTextColour0) == 0);
    CHECK(b.shown() == 1);

    FakeGame h;
    SetUp(h);
    h.malloc_ok = false;
    T::Balloons c;
    CHECK_FALSE(c.Step(h, {{"Saria", 640, 300, 1.0f, 0xFF0000}}, 1280, 720));
    CHECK(c.no_memory());
    CHECK(c.shown() == 0);
    CHECK(h.Count(T::addr::kSetPaneText) == 0);
}

TEST_CASE("native tags: the map probe lists the MapIcon layouts a screen points at, with their offsets") {
    FakeGame g;
    const std::uint64_t screen = 0xe0000000;
    const auto layout = [&](std::uint64_t at, const char* name) {
        g.Put<std::uint64_t>(at + 0xf8, at + 0x800);           // its resource
        g.Put<std::uint64_t>(at + 0x800 + 0x18, at + 0x900);   // the resource's name
        g.Write(at + 0x900, name, std::strlen(name) + 1);
    };
    layout(0xe1000000, "PaMapIconPlayer_00");
    layout(0xe2000000, "PaMapFloorTabBtn_00");
    g.Put<std::uint64_t>(screen + 0x10, 0xe1000000);
    g.Put<std::uint64_t>(screen + 0x18, 0xe2000000);
    g.Put<std::uint64_t>(screen + 0x20, 0x1234);       // not a guest pointer
    g.Put<std::uint64_t>(screen + 0x28, 0xe3000000);   // a pointer to something with no name
    const auto held = T::LayoutsHeld(g, screen, 0x100, "MapIcon");
    REQUIRE(held.size() == 1);
    CHECK(held[0].offset == 0x10);
    CHECK(held[0].layout == 0xe1000000);
    CHECK(held[0].name == "PaMapIconPlayer_00");
    CHECK(T::LayoutsHeld(g, screen, 0x100, "").size() == 2);
    CHECK(T::LayoutsHeld(g, 0, 0x100, "").empty());
}
