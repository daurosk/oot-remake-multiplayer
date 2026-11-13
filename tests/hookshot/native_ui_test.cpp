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
