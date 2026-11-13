// SPDX-License-Identifier: GPL-3.0-or-later
// Chat and name tags as the game's own UI (hookshot/src/coop_native_ui.h),
// against a pretend game: guest memory as a map of bytes and a list of calls.
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coop_native_ui.h"

namespace ui = CoopNativeUi;

namespace {

struct FakeGame final : ui::Game {
    std::map<std::uint64_t, std::uint8_t> mem;
    struct Made {
        std::uint64_t function;
        std::array<std::uint64_t, 5> x;
    };
    std::vector<Made> calls;
    std::map<std::uint64_t, std::uint64_t> returns;  // function -> x0
    std::set<std::uint64_t> missing;                 // functions that are not there

    bool Read(std::uint64_t a, void* out, std::size_t n) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = mem.find(a + i);
            if (it == mem.end()) {
                return false;
            }
            o[i] = it->second;
        }
        return true;
    }
    bool Write(std::uint64_t a, const void* in, std::size_t n) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < n; ++i) {
            mem[a + i] = p[i];
        }
        return true;
    }
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x, float) override {
        if (missing.count(f) != 0) {
            return std::nullopt;
        }
        calls.push_back({f, x});
        const auto it = returns.find(f);
        return it != returns.end() ? it->second : 0;
    }
    template <typename T>
    void Put(std::uint64_t a, T v) {
        Write(a, &v, sizeof v);
    }
    template <typename T>
    T Get(std::uint64_t a) {
        T v{};
        REQUIRE(Read(a, &v, sizeof v));
        return v;
    }
    std::string Text(std::uint64_t a) {
        std::string s;
        for (char c; Read(a + s.size(), &c, 1) && c != 0;) {
            s.push_back(c);
        }
        return s;
    }
    void Zero(std::uint64_t a, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            mem[a + i] = 0;
        }
    }
};

constexpr std::uint64_t kBlock = 0x90000000;
constexpr std::uint64_t kManager = 0x91000000;
constexpr std::uint64_t kScreen = 0x92000000;
constexpr std::uint64_t kLayout = 0x93000000;
constexpr std::uint64_t kOpenFn = 0x83409999;
constexpr std::uint64_t kInfo = 0x94000000;
constexpr std::uint64_t kUi = 0x95000000;
constexpr std::uint64_t kUiList = 0x96000000;

// The in-game window as the game has it at play: found through its getter, open (A = 1, B = 0).
void InGameWindow(FakeGame& g) {
    g.Put<std::uint64_t>(ui::addr::kInGameScreenGlobal, kManager);
    g.returns[ui::addr::kInGameScreenGet] = kScreen;
    g.Zero(kScreen, 0x500);
    g.Put<std::uint64_t>(kScreen, ui::addr::kInGameVtable);
    g.Put<std::uint64_t>(kScreen + ui::addr::kScreenLayout, kLayout);
    g.Put<std::int8_t>(kScreen + ui::addr::kScreenOpenA, 1);
    g.Put<std::uint64_t>(ui::addr::kInGameVtable + ui::addr::kSlotOpen * 8, kOpenFn);
}

// The UI manager's list with the banner second.
void BannerScreen(FakeGame& g) {
    g.Put<std::uint64_t>(ui::addr::kUiManagerGot, kUi + 0x100);
    g.Put<std::uint64_t>(kUi + 0x100, kUi);
    g.Put<std::uint32_t>(kUi + ui::addr::kUiCount, 2);
    g.Put<std::uint64_t>(kUi + ui::addr::kUiScreens, kUiList);
    g.Put<std::uint64_t>(kUiList, kScreen);
    g.Put<std::uint64_t>(kUiList + 8, kInfo);
    g.Put<std::uint64_t>(kScreen, 0x84000000);  // some other screen
    g.Put<std::uint64_t>(kInfo, ui::addr::kInfoVtable);
    g.Put<std::uint64_t>(kInfo + ui::addr::kScreenLayout, kLayout);
    g.Put<std::uint64_t>(ui::addr::kInfoVtable + ui::addr::kSlotOpen * 8, kOpenFn + 4);
}

}  // namespace

TEST_CASE("native ui: Clean drops message tags and controls, keeps UTF-8 whole") {
    // A tag a player could type: 0x0E, group 0, type 3 (colour), size 2, index 0.
    const std::string tagged = std::string("red\x0e\x00\x00\x03\x00\x02\x00\x00\x00text", 16);
    CHECK(ui::Clean(tagged, 100) == "redtext");
    CHECK(ui::Clean("a\tb\nc\x7f", 100) == "a b c");
    CHECK(ui::Clean("caf\xc3\xa9", 100) == "caf\xc3\xa9");
    CHECK(ui::Clean("caf\xc3\xa9", 4) == "caf");  // a character is never cut in two
    CHECK(ui::Clean("\xff" "a\xc3", 100) == "?a?");
    CHECK(ui::Clean("\xed\xa0\x80", 100) == "???");  // a surrogate is not text
    CHECK(ui::Clean("\xe2\x9c\x93 ok", 100) == "\xe2\x9c\x93 ok");
}

TEST_CASE("native ui: a chat line's speaker") {
    CHECK(ui::SplitSpeaker("Saria: hello: there").speaker == "Saria");
    CHECK(ui::SplitSpeaker("Saria: hello: there").text == "hello: there");
    CHECK(ui::SplitSpeaker("no speaker").speaker.empty());
    CHECK(ui::SplitSpeaker(": odd").speaker.empty());
}

TEST_CASE("native ui: 16 different colours, 1-4 as the protocol's") {
    CHECK(ui::Colour(1) == 0x3070FF);
    CHECK(ui::Colour(2) == 0xFF3030);
    CHECK(ui::Colour(3) == 0x30D040);
    CHECK(ui::Colour(4) == 0xFFC020);
    std::set<std::uint32_t> all;
    for (int n = 1; n <= 16; ++n) {
        all.insert(ui::Colour(static_cast<std::uint8_t>(n)));
    }
    CHECK(all.size() == 16);
    CHECK(ui::Colour(0) == 0xFFFFFF);
    CHECK(ui::Colour(17) == 0xFFFFFF);
}

TEST_CASE("native ui: the chat feed shows each new line once, one at a time") {
    ui::ChatFeed feed;
    feed.Update({{"old: before", false, 3000}}, 10000);  // there before: not shown again
    CHECK_FALSE(feed.Next(10000).has_value());

    feed.Update({{"old: before", false, 3016}, {"Saria: hi", false, 0}, {"Saria joined", true, 0}}, 10016);
    const auto first = feed.Next(10016);
    REQUIRE(first.has_value());
    CHECK(first->speaker == "Saria");
    CHECK(first->text == "hi");
    CHECK(first->seconds == doctest::Approx(ui::ChatFeed::Seconds(2)));
    CHECK(feed.Busy(10016));
    CHECK_FALSE(feed.Next(11000).has_value());  // the window is still showing it

    // The same lines a frame later are not new.
    feed.Update({{"Saria: hi", false, 16}, {"Saria joined", true, 16}}, 10032);
    const auto second = feed.Next(20000);
    REQUIRE(second.has_value());
    CHECK(second->notice);
    CHECK(second->speaker.empty());
    CHECK(second->text == "Saria joined");
    CHECK_FALSE(feed.Next(40000).has_value());
}

TEST_CASE("native ui: a burst from many players keeps the newest lines, stale ones go") {
    ui::ChatFeed feed;
    feed.Update({}, 0);
    std::vector<ui::ChatIn> burst;
    for (int i = 0; i < 20; ++i) {
        burst.push_back({"P" + std::to_string(i) + ": line " + std::to_string(i), false, 0});
    }
    feed.Update(burst, 1000);
    CHECK(feed.waiting() == ui::ChatFeed::kWaiting);
    const auto first = feed.Next(1000);
    REQUIRE(first.has_value());
    CHECK(first->text == "line 12");  // 20 came, the 8 newest wait
    // Long after: what is left is stale and dropped.
    CHECK_FALSE(feed.Next(1000 + ui::ChatFeed::kStaleMs + 10000).has_value());
    CHECK(feed.waiting() == 0);
}

TEST_CASE("native ui: tags are the nearest players in range, at most 15, faded") {
    std::vector<ui::TagIn> players;
    for (int n = 1; n <= 16; ++n) {
        players.push_back({static_cast<std::uint8_t>(n), "P" + std::to_string(n), {float(n) * 3.0f, 0, 0}, false});
    }
    players[1].hidden = true;                      // P2: another scene
    players[2].head = {500.0f, 0, 0};              // P3: far away
    players[3].name = std::string("Bad\x0e\x00", 5);  // P4: a tag in the name
    auto tags = ui::PlanTags(players, {0, 0, 0});
    CHECK(tags.size() == 14);  // 16 - hidden - far
    CHECK(tags.front().number == 1);
    CHECK(tags[1].number == 4);
    CHECK(tags[1].name == "Bad");
    CHECK(tags[1].colour == 0xFFC020);
    CHECK(tags.front().alpha == 1.0f);
    // P16 at 48: past the fade start.
    CHECK(tags.back().number == 16);
    CHECK(tags.back().alpha < 1.0f);
    CHECK(tags.back().alpha > 0.0f);

    players[1].hidden = false;
    players[2].head = {1.0f, 0, 0};
    tags = ui::PlanTags(players, {0, 0, 0});
    CHECK(tags.size() == ui::kMaxTags);  // 16 in range: one balloon stays the game's
    CHECK(tags.front().number == 3);
}

TEST_CASE("native ui: the text ring writes a reference the game can read") {
    FakeGame g;
    ui::TextRing ring;
    CHECK(ring.Put(g, "x") == 0);  // no block yet
    ring.SetBlock(kBlock);
    const auto ref_at = ring.Put(g, "hello");
    REQUIRE(ref_at == kBlock);
    const auto ref = g.Get<ui::Ref>(ref_at);
    CHECK(ref.text == kBlock + sizeof(ui::Ref));
    CHECK(ref.bytes == 5);
    CHECK(ref.end == -1);
    CHECK(g.Text(ref.text) == "hello");
    CHECK(ring.Put(g, "next") == kBlock + ui::TextRing::kSlotBytes);
    for (std::size_t i = 2; i < ui::TextRing::kSlots; ++i) {
        ring.Put(g, "fill");
    }
    CHECK(ring.Put(g, "again") == kBlock);  // round: the first slot again
    const std::string long_text(400, 'a');
    const auto cut = g.Get<ui::Ref>(ring.Put(g, long_text));
    CHECK(cut.bytes == ui::TextRing::kSlotText);
}

TEST_CASE("native ui: a chat line goes into the game's in-game message window") {
    FakeGame g;
    InGameWindow(g);
    ui::TextRing ring;
    ring.SetBlock(kBlock);
    ui::Shown line{"Saria", "hello", false, 4.0f};
    REQUIRE(ui::ShowLine(g, ring, line));

    REQUIRE(g.calls.size() == 3);
    CHECK(g.calls[0].function == ui::addr::kInGameScreenGet);
    CHECK(g.calls[0].x[0] == kManager);
    // The speaker into T_Name_00 of the window's layout, with the game's own pane name.
    CHECK(g.calls[1].function == ui::addr::kSetPaneText);
    CHECK(g.calls[1].x[0] == kLayout);
    CHECK(g.calls[1].x[1] == ui::addr::kNameT_Name_00);
    const auto name = g.Get<ui::Ref>(g.calls[1].x[2]);
    CHECK(g.Text(name.text) == "Saria");
    CHECK(g.calls[1].x[3] == 1);
    // The line as the window's message, its time, the show flag, then open (mode 1).
    const auto text = g.Get<ui::Ref>(kScreen + ui::addr::kInGameText);
    CHECK(g.Text(text.text) == "hello");
    CHECK(text.bytes == 5);
    CHECK(text.end == -1);
    CHECK(g.Get<float>(kScreen + ui::addr::kInGameTime) == 4.0f * ui::addr::kFramesPerSecond);
    CHECK(g.Get<std::uint8_t>(kScreen + ui::addr::kInGameShow) == 1);
    CHECK(g.calls[2].function == kOpenFn);
    CHECK(g.calls[2].x[0] == kScreen);
    CHECK(g.calls[2].x[1] == 1);
}

TEST_CASE("native ui: no window, or not this build's window: nothing is touched") {
    FakeGame g;
    ui::TextRing ring;
    ring.SetBlock(kBlock);
    CHECK_FALSE(ui::ShowLine(g, ring, {"a", "b", false, 1.0f}));  // the title: no manager
    InGameWindow(g);
    g.Put<std::uint64_t>(kScreen, 0x84000000);  // another class
    g.calls.clear();
    CHECK_FALSE(ui::ShowLine(g, ring, {"a", "b", false, 1.0f}));
    CHECK(g.calls.size() == 1);  // the getter only
    CHECK(g.Get<std::uint8_t>(kScreen + ui::addr::kInGameShow) == 0);
}

TEST_CASE("native ui: a closed window is opened without the show flag, as the game does") {
    FakeGame g;
    InGameWindow(g);
    g.Put<std::int8_t>(kScreen + ui::addr::kScreenOpenA, 0);
    ui::TextRing ring;
    ring.SetBlock(kBlock);
    REQUIRE(ui::ShowLine(g, ring, {"a", "b", false, 1.0f}));
    CHECK(g.Get<std::uint8_t>(kScreen + ui::addr::kInGameShow) == 0);
    CHECK(g.calls.back().function == kOpenFn);
}

TEST_CASE("native ui: a notice goes into the game's banner, found by its vtable") {
    FakeGame g;
    BannerScreen(g);
    ui::TextRing ring;
    ring.SetBlock(kBlock);
    REQUIRE(ui::ShowNotice(g, ring, {"", "Saria joined", true, 3.0f}));
    REQUIRE(g.calls.size() == 2);
    CHECK(g.calls[0].function == ui::addr::kSetPaneText);
    CHECK(g.calls[0].x[0] == kLayout);
    CHECK(g.calls[0].x[1] == ui::addr::kNameT_Text_00);
    CHECK(g.Text(g.Get<ui::Ref>(g.calls[0].x[2]).text) == "Saria joined");
    CHECK(g.calls[1].function == kOpenFn + 4);
    CHECK(g.calls[1].x[0] == kInfo);
    CHECK(g.calls[1].x[1] == 2);
}

TEST_CASE("native ui: the game's projection, behind the camera is nothing") {
    FakeGame g;
    const std::uint64_t scratch = kBlock + ui::TextRing::kBlockBytes;
    g.returns[ui::addr::kProject] = 1;
    g.Put<float>(scratch + 8, 0.0f);
    // The pretend game writes nothing: xy stays the zeros Project wrote.
    auto xy = ui::Project(g, scratch, {1.0f, 2.0f, 3.0f});
    REQUIRE(xy.has_value());
    CHECK(g.calls.back().x[0] == scratch);
    CHECK(g.calls.back().x[1] == scratch + 16);
    CHECK(g.calls.back().x[2] == scratch + 8);
    CHECK(g.Get<float>(scratch + 20) == 2.0f);
    g.returns[ui::addr::kProject] = 0;  // off screen
    CHECK_FALSE(ui::Project(g, scratch, {1.0f, 2.0f, 3.0f}).has_value());
    CHECK_FALSE(ui::Project(g, 0, {1.0f, 2.0f, 3.0f}).has_value());
}

TEST_CASE("native ui: the game's own message on the window is never written over") {
    FakeGame g;
    InGameWindow(g);
    ui::TextRing ring;
    ring.SetBlock(kBlock);
    // An NPC's line of the game's: its text outside our block, time left.
    g.Put<std::uint64_t>(kScreen + ui::addr::kInGameText, 0x83000000);
    g.Put<float>(kScreen + ui::addr::kInGameTime, 90.0f);
    CHECK_FALSE(ui::InGameFree(g, kScreen, ring));
    CHECK_FALSE(ui::ShowLine(g, ring, {"a", "b", false, 1.0f}));
    CHECK(g.Get<std::uint64_t>(kScreen + ui::addr::kInGameText) == 0x83000000);
    // Its time out: free.
    g.Put<float>(kScreen + ui::addr::kInGameTime, 0.0f);
    CHECK(ui::InGameFree(g, kScreen, ring));
    // One of ours still showing: free (the next line follows it).
    REQUIRE(ui::ShowLine(g, ring, {"a", "b", false, 1.0f}));
    CHECK(ui::InGameFree(g, kScreen, ring));
}

TEST_CASE("native ui: a line put back is first again, and goes stale from when it came") {
    ui::ChatFeed feed;
    feed.Update({}, 0);
    feed.Update({{"A: one", false, 0}, {"B: two", false, 0}}, 1000);
    auto line = feed.Next(1000);
    REQUIRE(line.has_value());
    CHECK(line->came_ms == 1000);
    feed.PutBack(*line);
    CHECK_FALSE(feed.Busy(1000));
    line = feed.Next(2000);
    REQUIRE(line.has_value());
    CHECK(line->text == "one");
    feed.PutBack(*line);
    CHECK_FALSE(feed.Next(1000 + ui::ChatFeed::kStaleMs + 1).has_value());  // both stale now
}

TEST_CASE("native ui: the typed line refreshes without opening the window again, and ends when the box closes") {
    FakeGame g;
    InGameWindow(g);
    ui::TextRing ring;
    ring.SetBlock(kBlock);
    auto typing = ui::TypingLine("you", "hel", false);
    CHECK(typing.text == "hel_");
    CHECK(typing.seconds == ui::kTypingSeconds);
    CHECK(ui::TypingLine("you", "x\x0e", true).text == "x_  (not sent)");
    REQUIRE(ui::ShowLine(g, ring, typing, true));
    // Up already (A = 1, B = 0): text and flag, no open call.
    CHECK(g.calls.back().function == ui::addr::kSetPaneText);
    CHECK(g.Text(g.Get<ui::Ref>(kScreen + ui::addr::kInGameText).text) == "hel_");
    CHECK(g.Get<std::uint8_t>(kScreen + ui::addr::kInGameShow) == 1);
    // Closed: it is opened.
    g.Put<std::int8_t>(kScreen + ui::addr::kScreenOpenA, 0);
    REQUIRE(ui::ShowLine(g, ring, ui::TypingLine("you", "hello", false), true));
    CHECK(g.calls.back().function == kOpenFn);
    // The box closes: our line's time goes to 0.
    CHECK(ui::EndLine(g, ring));
    CHECK(g.Get<float>(kScreen + ui::addr::kInGameTime) == 0.0f);
    // The game's own message is left alone.
    g.Put<std::uint64_t>(kScreen + ui::addr::kInGameText, 0x83000000);
    g.Put<float>(kScreen + ui::addr::kInGameTime, 50.0f);
    CHECK_FALSE(ui::EndLine(g, ring));
    CHECK(g.Get<float>(kScreen + ui::addr::kInGameTime) == 50.0f);
}

TEST_CASE("native ui: scene pixels to the game's layout points") {
    const auto c = ui::LayoutPoint(960.0f, 540.0f, 1920.0f, 1080.0f);
    CHECK(c[0] == 0.0f);
    CHECK(c[1] == 0.0f);
    const auto tl = ui::LayoutPoint(0.0f, 0.0f, 1536.0f, 864.0f);  // Hookshot's scene size in a run
    CHECK(tl[0] == -960.0f);
    CHECK(tl[1] == 540.0f);  // y up
    const auto br = ui::LayoutPoint(1536.0f, 864.0f, 1536.0f, 864.0f);
    CHECK(br[0] == 960.0f);
    CHECK(br[1] == -540.0f);
    CHECK(ui::LayoutPoint(1.0f, 1.0f, 0.0f, 0.0f)[0] == 0.0f);
}

TEST_CASE("native ui: tags take balloon parts from the last one down, nearest first, at most 15") {
    std::vector<ui::TagSpot> spots;
    for (int i = 0; i < 17; ++i) {  // far to near: P0 the farthest
        spots.push_back({"P" + std::to_string(i), 100.0f + i, 200.0f, 1.0f});
    }
    spots[16].alpha = 0.0f;  // faded out: no part
    auto parts = ui::PlanParts(spots, 1920.0f, 1080.0f);
    REQUIRE(parts.size() == ui::kMaxTags);
    CHECK(parts[0].part == 15);
    CHECK(parts[0].name == "P15");  // the nearest one shown
    CHECK(parts[14].part == 1);     // part 0 stays the game's
    CHECK(parts[14].name == "P1");
    CHECK(parts[0].at[0] == doctest::Approx(115.0f - 960.0f));
    CHECK(parts[0].at[1] == doctest::Approx(340.0f));
}

TEST_CASE("native ui: a fixed text slot is written only when its text changes") {
    FakeGame g;
    ui::TextRing ring;
    ring.SetBlock(kBlock);
    const auto a = ring.PutFixed(g, 3, "Saria");
    REQUIRE(a == kBlock + ui::TextRing::kRingBytes + 3 * ui::TextRing::kSlotBytes);
    CHECK(ring.Owns(a));
    CHECK(g.Text(g.Get<ui::Ref>(a).text) == "Saria");
    g.mem.erase(a + sizeof(ui::Ref));  // were it written again, this byte would come back
    CHECK(ring.PutFixed(g, 3, "Saria") == a);
    CHECK(g.mem.count(a + sizeof(ui::Ref)) == 0);
    ring.PutFixed(g, 3, "Mido");
    CHECK(g.Text(g.Get<ui::Ref>(a).text) == "Mido");
    CHECK(ring.PutFixed(g, 16, "x") == 0);
}

TEST_CASE("native ui: a pane moves as the game moves it, and the layout is told") {
    FakeGame g;
    constexpr std::uint64_t kPane = 0x97000000;
    g.Zero(kPane, 0x100);
    g.Put<std::uint32_t>(kPane + ui::addr::kPaneFlags, 1u << 20);
    g.Put<std::uint32_t>(kPane + 4, 0x40);
    REQUIRE(ui::MovePane(g, kPane, {12.5f, -40.0f}));
    CHECK(g.Get<float>(kPane + ui::addr::kPaneXY) == 12.5f);
    CHECK(g.Get<float>(kPane + ui::addr::kPaneXY + 4) == -40.0f);
    CHECK(g.Get<std::uint32_t>(kPane) == 0x40);
    REQUIRE(g.calls.size() == 2);
    CHECK(g.calls[0].function == ui::addr::kPaneMarkA);
    CHECK(g.calls[0].x[1] == 0x40);
    CHECK(g.calls[1].function == ui::addr::kPaneMarkB);
    // The same place again: nothing more.
    REQUIRE(ui::MovePane(g, kPane, {12.5f, -40.0f}));
    CHECK(g.calls.size() == 2);
    // Another kind of pane (bit 8): left alone.
    g.Put<std::uint32_t>(kPane + ui::addr::kPaneFlags, 1u << 8);
    CHECK_FALSE(ui::MovePane(g, kPane, {1.0f, 1.0f}));
}

TEST_CASE("native ui: the balloons step names, places and shows parts, and hides the ones no longer used") {
    FakeGame g;
    constexpr std::uint64_t kBalloon = 0x98000000;
    constexpr std::uint64_t kPart = 0x99000000;  // part i at kPart + i * 0x1000
    constexpr std::uint64_t kRoot = 0x9a000000;  // its root pane at kRoot + i * 0x1000
    g.Put<std::uint64_t>(ui::addr::kUiManagerGot, kUi + 0x100);
    g.Put<std::uint64_t>(kUi + 0x100, kUi);
    g.Put<std::uint32_t>(kUi + ui::addr::kUiCount, 1);
    g.Put<std::uint64_t>(kUi + ui::addr::kUiScreens, kUiList);
    g.Put<std::uint64_t>(kUiList, kBalloon);
    g.Put<std::uint64_t>(kBalloon, ui::addr::kBalloonVtable);
    for (std::uint64_t i = 0; i < 16; ++i) {
        g.Put<std::uint64_t>(kBalloon + ui::addr::kBalloonPartTable + i * ui::addr::kBalloonPartStride, kPart + i * 0x1000);
        g.Zero(kRoot + i * 0x1000, 0x100);
    }
    // The pretend game's root-pane call: part 14 or 15 by the argument.
    ui::TextRing ring;
    ring.SetBlock(kBlock);
    ui::Balloons balloons;
    // FakeGame returns one value per function: give part 15's root.
    g.returns[ui::addr::kPartRootPane] = kRoot + 15 * 0x1000;
    std::vector<ui::TagSpot> spots = {{"Saria", 960.0f, 540.0f, 1.0f}};
    REQUIRE(balloons.Step(g, ring, spots, 1920.0f, 1080.0f));
    CHECK(balloons.shown() == 1);
    bool named = false, rooted = false, shown = false;
    for (const auto& c : g.calls) {
        named |= c.function == ui::addr::kSetPaneText && c.x[0] == kPart + 15 * 0x1000 &&
                 c.x[1] == ui::addr::kNameT_Text_00;
        rooted |= c.function == ui::addr::kPartRootPane && c.x[0] == kPart + 15 * 0x1000 + 0x40;
        shown |= c.function == ui::addr::kPaneShow && c.x[0] == kRoot + 15 * 0x1000 && c.x[1] == 1;
    }
    CHECK(named);
    CHECK(rooted);
    CHECK(shown);
    // Next frame, same name: no new text call.
    g.calls.clear();
    REQUIRE(balloons.Step(g, ring, spots, 1920.0f, 1080.0f));
    for (const auto& c : g.calls) {
        CHECK(c.function != ui::addr::kSetPaneText);
    }
    // Nobody to tag: the part is hidden again.
    g.calls.clear();
    REQUIRE(balloons.Step(g, ring, {}, 1920.0f, 1080.0f));
    CHECK(balloons.shown() == 0);
    REQUIRE(g.calls.size() == 1);
    CHECK(g.calls[0].function == ui::addr::kPaneShow);
    CHECK(g.calls[0].x[1] == 0);
    // No balloon screen (a load): false.
    g.Put<std::uint64_t>(kBalloon, 0x84000000);
    CHECK_FALSE(balloons.Step(g, ring, spots, 1920.0f, 1080.0f));
}
