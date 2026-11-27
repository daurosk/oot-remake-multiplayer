// SPDX-License-Identifier: GPL-3.0-or-later
// Chat as the game's own UI (hookshot/src/coop_native_ui.h), against a pretend game: guest memory as a map of bytes
// and a list of the calls made.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "coop_native_ui.h"

namespace ui = CoopNativeUi;

namespace {

constexpr std::uint64_t kMain = 0x80020000;

struct FakeGame final : ui::Game {
    // A pretend game in play: the game's play UI is up (patch 0018's HUD screen ran since the last load), so its
    // listed screens are live (CoopNativeUi::ScreenLive, party-hud-crash-c126).
    FakeGame() {
        CoopNativeUi::SetSceneLoading(false);
        CoopNativeUi::SetRunningScreens(std::vector<std::uint64_t>{}, true);
    }
    std::map<std::uint64_t, std::uint8_t> mem;
    struct Made {
        std::uint64_t function;
        std::array<std::uint64_t, 5> x;
    };
    std::vector<Made> calls;
    std::map<std::uint64_t, std::uint64_t> returns;  // function -> x0
    std::set<std::uint64_t> missing;                 // functions that are not there
    // The text box the game's pane setter writes: it keeps the text's pointer at +0xf0 (f_80459680), or a copy's
    // (copy_to) when a game kept its own.
    std::uint64_t set_box = 0, copy_to = 0;

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
        if (missing.count(f) != 0) return std::nullopt;
        calls.push_back({f, x});
        if (f == kMain + ui::addr::kSetPaneText && set_box != 0) {
            std::uint64_t text = copy_to;
            if (text == 0) Read(x[2], &text, sizeof text);   // (the Ref's first word)
            Put<std::uint64_t>(set_box + ui::addr::kBoxText, text);
        }
        const auto it = returns.find(f);
        return it != returns.end() ? it->second : 0;
    }
    std::uint64_t Main() const override { return kMain; }
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
        for (char c; Read(a + s.size(), &c, 1) && c != 0;) s.push_back(c);
        return s;
    }
    std::vector<Made> CallsTo(std::uint64_t f) const {
        std::vector<Made> out;
        for (const auto& c : calls) {
            if (c.function == f) out.push_back(c);
        }
        return out;
    }
};

constexpr std::uint64_t kBlock = 0x90000000;
constexpr std::uint64_t kBanner = 0x93000000;
constexpr std::uint64_t kLayout = 0x94000000;
constexpr std::uint64_t kOther = 0x95000000;  // another screen in the manager's list
constexpr std::uint64_t kManager = 0x96000000;
constexpr std::uint64_t kScreens = 0x97000000;
constexpr std::uint64_t kPane = 0x98000000;
constexpr std::uint64_t kInner = 0x98100000;
constexpr std::uint64_t kBox = 0x98200000;
constexpr std::uint64_t kType = 0x98300000;  // the banner's Type animation
constexpr std::uint64_t kOpen = 0x804d6100, kClose = 0x802f6990;

std::uint64_t At(std::uint64_t main_offset) { return kMain + main_offset; }

// A game with the banner loaded and shut.
struct World {
    FakeGame g;
    ui::TextRing ring;
    World() {
        ring.SetBlock(kBlock);
        g.Put<std::uint64_t>(kBanner + ui::addr::kScreenLayout, kLayout);
        g.Put<std::int8_t>(kBanner + ui::addr::kScreenRequest, 0);
        g.Put<std::uint8_t>(kBanner + ui::addr::kScreenState, 0);
        g.Put<std::uint64_t>(kBanner + ui::addr::kInfoType, kType);
        // The banner, found in the UI manager's list by its vtable (after another screen).
        g.Put<std::uint64_t>(At(ui::addr::kUiManagerGot), kManager - 8);
        g.Put<std::uint64_t>(kManager - 8, kManager);
        g.Put<std::uint32_t>(kManager + ui::addr::kUiCount, 2);
        g.Put<std::uint64_t>(kManager + ui::addr::kUiScreens, kScreens);
        g.Put<std::uint64_t>(kScreens, kOther);
        g.Put<std::uint64_t>(kOther, 0x84c09678);
        g.Put<std::uint64_t>(kScreens + 8, kBanner);
        g.Put<std::uint64_t>(kBanner, At(ui::addr::kInfoVtable));
        g.Put<std::uint64_t>(At(ui::addr::kInfoVtable) + 5 * 8, kOpen);
        g.Put<std::uint64_t>(At(ui::addr::kInfoVtable) + 6 * 8, kClose);
        // T_Text_00: a text pane (kind 2) and its text box with the game's own colours.
        g.returns[At(ui::addr::kPaneByName)] = kPane;
        g.Put<std::uint32_t>(kPane + ui::addr::kPaneFlags, 2u << 22);
        g.Put<std::uint64_t>(kPane + 8, kInner);
        g.Put<std::uint64_t>(kInner + 8, kBox);
        const float game[8] = {1, 1, 1, 1, 0.9f, 0.9f, 0.9f, 1};
        g.Write(kBox + ui::addr::kBoxColour0, game, sizeof game);
        g.set_box = kBox;
        g.Put<std::uint64_t>(kBox + ui::addr::kBoxText, 0);
    }
    ui::Ref RefAt(std::uint64_t a) { return g.Get<ui::Ref>(a); }
    std::string LastText() {
        const auto sets = g.CallsTo(At(ui::addr::kSetPaneText));
        REQUIRE_FALSE(sets.empty());
        return g.Text(RefAt(sets.back().x[2]).text);
    }
    void Up() {  // open, the open taken
        g.Put<std::uint8_t>(kBanner + ui::addr::kScreenState, ui::addr::kStateOpen);
        g.Put<std::int8_t>(kBanner + ui::addr::kScreenRequest, 1);
    }
    void Shut() {
        g.Put<std::uint8_t>(kBanner + ui::addr::kScreenState, 0);
        g.Put<std::int8_t>(kBanner + ui::addr::kScreenRequest, 0);
    }
};

ui::Shown Line(std::string speaker, std::string text, std::uint32_t colour = 0) {
    ui::Shown s;
    s.speaker = std::move(speaker);
    s.text = std::move(text);
    s.seconds = 4.0f;
    s.colour = colour;
    return s;
}

ui::Shown Notice(std::string text) {
    ui::Shown s;
    s.notice = true;
    s.text = std::move(text);
    s.seconds = 4.0f;
    return s;
}

}  // namespace

TEST_CASE("native ui: the game's addresses as checked on the 1.1.1 main (main+ offsets)") {
    CHECK(kMain + ui::addr::kInfoVtable == 0x84c0d7c8);  // what the constructor stores, not slot 2 (0x84c0d7d8)
    CHECK(kMain + ui::addr::kSetPaneText == 0x8045b970);
    CHECK(kMain + ui::addr::kPaneByName == 0x8078e2b0);
    CHECK(kMain + ui::addr::kSetColour0 == 0x8152c740);
    CHECK(kMain + ui::addr::kSetColour1 == 0x8152c810);
    CHECK(kMain + ui::addr::kNameT_Text_00 == 0x83de2306);
    CHECK(kMain + ui::addr::kUiManagerGot == 0x84c9e098);
    CHECK(kMain + ui::addr::kAnimFirstFrame == 0x802f0fe0);
    CHECK(ui::addr::kInfoType == 0x2b0);  // f_80de2490 stores the Type animation there
    CHECK(ui::addr::kMalloc == 0x14c6c90);
}

TEST_CASE("native ui: other players' text never carries a game tag or a control byte") {
    // A colour tag (0x0E, u16 group 0, u16 type 3, u16 size 2, u16 index 1) as it could come: its bytes all go.
    const std::string tag("hi \x0E\x00\x00\x03\x00\x02\x00\x01\x00there", 3 + 9 + 5);
    CHECK(ui::Clean(tag, 100) == "hi there");
    CHECK(ui::Clean(std::string("a\x0E" "b", 3), 100) == "ab");
    CHECK(ui::Clean("tab\there", 100) == "tab here");
    CHECK(ui::Clean("line\nbreak", 100) == "line break");
    CHECK(ui::Clean("line\nbreak", 100, true) == "line\nbreak");
    CHECK(ui::Clean("del\x7f!", 100) == "del!");
    CHECK(ui::Clean("bad \xC3(", 100) == "bad ?(");
    CHECK(ui::Clean("\xE0\x80\x80", 100) == "???");     // overlong
    CHECK(ui::Clean("\xED\xA0\x80", 100) == "???");     // a surrogate
    CHECK(ui::Clean("caf\xC3\xA9 ok", 4) == "caf");     // cut at a whole character
    CHECK(ui::Clean("\xE4\xBD\xA0\xE5\xA5\xBD", 5) == "\xE4\xBD\xA0");
}

TEST_CASE("native ui: a chat line's speaker and text") {
    CHECK(ui::SplitSpeaker("Saria: hello: there").speaker == "Saria");
    CHECK(ui::SplitSpeaker("Saria: hello: there").text == "hello: there");
    CHECK(ui::SplitSpeaker("Mido joined").speaker.empty());
    CHECK(ui::SplitSpeaker(": odd").speaker.empty());
}

TEST_CASE("native ui: columns: Latin 1, Chinese, Japanese and Korean 2") {
    CHECK(ui::Columns("Link") == 4);
    CHECK(ui::Columns("\xE4\xBD\xA0\xE5\xA5\xBD") == 4);           // 你好
    CHECK(ui::Columns("\xEC\x95\x88\xEB\x85\x95") == 4);           // 안녕
    CHECK(ui::Columns("\xD0\x9F\xD1\x80\xD0\xB8") == 3);           // При (Cyrillic: narrow)
    CHECK(ui::Columns("\xEF\xBC\x81") == 2);                        // a full-width !
}

TEST_CASE("native ui: width: the game font's advances, else guesses never narrower than its Latin fonts") {
    ui::ResetFontWidths();
    CHECK(ui::Width("") == 0);
    CHECK(ui::Width("W") == ui::kGuessWidest);   // (1.10 em in Clarimo UD PE DemiBold: the widest Latin letter)
    CHECK(ui::kGuessWidest >= 110);
    CHECK(ui::Width("i") == ui::kGuessNarrow);
    CHECK(ui::Width(" ") == ui::kGuessSpace);
    CHECK(ui::Width("a") == ui::kGuessLower);
    CHECK(ui::Width("A") == ui::kGuessUpper);
    CHECK(ui::Width("m") == ui::kGuessBroad);
    CHECK(ui::Width("t") == ui::kGuessSlim);
    CHECK(ui::Width("\xE4\xBD\xA0\xE5\xA5\xBD") == 2 * ui::kWideWidth);   // 你好: 1 em each
    CHECK(ui::Width("\xC3\xA9") == ui::kGuessWidest);                     // é: no font, the widest guess
    CHECK(ui::Width("\xD0\x9F") == ui::kGuessWidest);                     // П
    // The bar: never wider than the game's own widest banner line (27.3 em), every guess at least the game's.
    CHECK(ui::kBannerWidth <= 2731);
    CHECK(ui::kNameWidth + 300 < ui::kBannerWidth);
}

namespace {
// A small OpenType font: 'head' (units per em 1000), 'hhea' (3 advances), 'hmtx' (500, 700, 1234), 'cmap' format 4
// mapping A -> glyph 1 and B -> glyph 2.
std::vector<std::uint8_t> TinyFont() {
    std::vector<std::uint8_t> b(222, 0);
    const auto be = [&](std::size_t at, std::uint32_t v, int n) {
        for (int k = n - 1; k >= 0; --k, v >>= 8) b[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v);
    };
    be(0, 0x00010000, 4);
    be(4, 4, 2);
    const std::uint32_t tags[4] = {0x68656164u, 0x68686561u, 0x686d7478u, 0x636d6170u};   // head hhea hmtx cmap
    const std::uint32_t offs[4] = {76, 130, 166, 178}, lens[4] = {54, 36, 12, 44};
    for (std::size_t i = 0; i < 4; ++i) {
        be(12 + 16 * i, tags[i], 4);
        be(12 + 16 * i + 8, offs[i], 4);
        be(12 + 16 * i + 12, lens[i], 4);
    }
    be(76 + 18, 1000, 2);    // unitsPerEm
    be(130 + 34, 3, 2);      // numberOfHMetrics
    be(166, 500, 2);
    be(170, 700, 2);
    be(174, 1234, 2);
    be(178 + 2, 1, 2);       // one subtable: platform 3, encoding 1, at +12
    be(178 + 4, 3, 2);
    be(178 + 6, 1, 2);
    be(178 + 8, 12, 4);
    const std::size_t f = 190;
    be(f, 4, 2);
    be(f + 2, 32, 2);
    be(f + 6, 4, 2);         // two segments
    be(f + 14, 0x42, 2);     // ends
    be(f + 16, 0xFFFF, 2);
    be(f + 20, 0x41, 2);     // starts (after the pad)
    be(f + 22, 0xFFFF, 2);
    be(f + 24, 0x10000 - 0x40, 2);   // deltas: A (0x41) -> 1
    be(f + 26, 1, 2);
    return b;
}
}  // namespace

TEST_CASE("native ui: width: the font's own advances (cmap + hmtx), rounded up, bad bytes leave the guesses") {
    const auto w = ui::FontWidths(TinyFont());
    REQUIRE(w.size() == 2);
    CHECK(w[0] == std::pair<std::uint32_t, std::uint16_t>{0x41, 70});
    CHECK(w[1] == std::pair<std::uint32_t, std::uint16_t>{0x42, 124});   // 1.234 em, rounded up
    REQUIRE(ui::SetFontWidths(TinyFont()));
    CHECK(ui::Width("AB") == 194);
    CHECK(ui::Width("AC") == 70 + ui::kGuessUpper);   // C: not in the font, guessed
    CHECK(ui::Width("\xE4\xBD\xA0") == ui::kWideWidth);
    auto cut = TinyFont();
    cut.resize(100);
    CHECK(ui::FontWidths(cut).empty());
    CHECK_FALSE(ui::SetFontWidths(cut));
    CHECK(ui::Width("AB") == 194);   // (a bad file keeps what was read)
    CHECK_FALSE(ui::SetFontWidths({}));
    CHECK(ui::FontWidths(std::vector<std::uint8_t>(64, 0xFF)).empty());
    ui::ResetFontWidths();
    CHECK(ui::Width("A") == ui::kGuessUpper);
}

TEST_CASE("native ui: wrapping fits the game's pane: whole words, pages, nothing lost") {
    ui::ResetFontWidths();
    const auto one = ui::Wrap("hello there", 4400, 2);
    REQUIRE(one.size() == 1);
    CHECK(one[0] == "hello there");
    const auto two = ui::Wrap("aaaa bbbb cccc", 600, 2);   // (aaaa bbbb: 558)
    REQUIRE(two.size() == 1);
    CHECK(two[0] == "aaaa bbbb\ncccc");
    const auto pages = ui::Wrap("aaaa bbbb cccc dddd eeee", 600, 2);
    REQUIRE(pages.size() == 2);
    CHECK(pages[0] == "aaaa bbbb\ncccc dddd");
    CHECK(pages[1] == "eeee");
    // A word longer than a line breaks inside, at a character; every row fits.
    const std::string longword(100, 'x');
    const auto cut = ui::Wrap(longword, 2904, 2);
    std::string joined;
    for (const auto& p : cut) {
        std::size_t from = 0;
        while (from <= p.size()) {
            const auto nl = p.find('\n', from);
            const std::string row = p.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
            CHECK(ui::Width(row) <= 2904);
            joined += row;
            if (nl == std::string::npos) break;
            from = nl + 1;
        }
    }
    CHECK(joined == longword);
    // Chinese: 1 em a character, broken anywhere.
    std::string zh;
    for (int i = 0; i < 30; ++i) zh += "\xE4\xBD\xA0";
    const auto zp = ui::Wrap(zh, 2200, 2);
    REQUIRE(zp.size() == 1);
    const auto nl = zp[0].find('\n');
    REQUIRE(nl != std::string::npos);
    CHECK(ui::Width(zp[0].substr(0, nl)) == 2200);
    CHECK(ui::Width(zp[0].substr(nl + 1)) == 800);
    CHECK(ui::Wrap("", 4400, 2).empty());
    CHECK(ui::Wrap("   ", 4400, 2).empty());
    // The worst line: 200 of the widest letter, each page inside the banner's line (48 columns of W were 48.7 em, past
    // the game's pane), and nothing lost.
    const std::string ws(200, 'W');
    std::string back;
    for (const auto& p : ui::Wrap(ws, ui::kBannerWidth, 1)) {
        CHECK(ui::Width(p) <= ui::kBannerWidth);
        back += p;
    }
    CHECK(back == ws);
}

TEST_CASE("native ui: the feed shows each line once, in banner pages, game-paced, and drops stale ones") {
    ui::ChatFeed f;
    f.Update({{"Saria: old", false, 100, 0}}, 1000);  // there before: not shown again
    CHECK(f.waiting() == 0);
    f.Update({{"Saria: old", false, 150, 0}, {"Saria: hi", false, 0, 0x3070FF}}, 1050);
    f.Update({{"Saria: hi", false, 30, 0x3070FF}}, 1080);  // the same line a frame later
    REQUIRE(f.waiting() == 1);
    auto a = f.Next(1100);
    REQUIRE(a);
    CHECK(a->speaker == "Saria");
    CHECK(a->text == "hi");
    CHECK(ui::BannerText(*a) == "Saria: hi");
    CHECK(a->colour == 0x3070FF);
    CHECK(a->first);
    CHECK(f.Busy(1100));
    CHECK_FALSE(f.Next(1200));
    // A 200-character line (the protocol's most): pages of one banner line, each "Mido: " and what fits after it.
    std::string words;
    for (int i = 0; i < 40; ++i) words += "word ";
    f.Update({{"Mido: " + words, false, 0, 0}}, 20000);
    const std::size_t pages = f.waiting();
    CHECK(pages == 7);  // 40 words after "Mido: ", 6 a page with the "... " marks (hud-fit-0120: 6 pages of 7 before)
    std::uint64_t t = 20000;
    for (std::size_t k = 0; k < pages; ++k) {
        const auto p = f.Next(t);
        REQUIRE(p);
        CHECK(p->speaker == "Mido");
        CHECK(p->first == (k == 0));
        CHECK(p->text.find('\n') == std::string::npos);
        CHECK(ui::Width(ui::BannerText(*p)) <= ui::kBannerWidth);
        CHECK(p->seconds <= 6.0f);
        t += static_cast<std::uint64_t>(p->seconds * 1000.0f) + 1;
    }
    CHECK(t > 20000 + ui::ChatFeed::kStaleMs);  // the line outlasts the stale time: begun, so shown to its end
    // A notice stale before it began: dropped.
    f.Update({{"Ruto joined the game", true, 0, 0x123456}}, 60000);
    CHECK_FALSE(f.Next(2'000'000));
    f.Update({{"Ruto left", true, 0, 0}}, 3'000'000);
    const auto m = f.Next(3'000'000);
    REQUIRE(m);
    CHECK(m->notice);
    CHECK(m->speaker.empty());
    CHECK(m->colour == 0);
    CHECK(ui::BannerText(*m) == "Ruto left");
    // A long line stale before it began: all of its pages go.
    f.Update({{"Mido: " + words, false, 0, 0}, {"Saria: after", false, 0, 0}}, 4'000'000);
    CHECK_FALSE(f.Next(4'000'000 + ui::ChatFeed::kStaleMs + 1));
    CHECK(f.waiting() == 0);
    // Never more than kWaiting wait, and never half a line.
    std::vector<ui::ChatIn> burst;
    for (int i = 0; i < 40; ++i) burst.push_back({"P" + std::to_string(i) + ": x", false, 0, 0});
    burst.push_back({"Mido: " + words, false, 0, 0});
    f.Update(burst, 5'000'000);
    CHECK(f.waiting() <= ui::ChatFeed::kWaiting);
    auto first = f.Next(5'000'000);
    REQUIRE(first);
    CHECK(first->first);
    // A page put back is first again.
    f.PutBack(*first);
    CHECK_FALSE(f.Busy(5'000'000));
    CHECK(f.Next(5'000'000)->speaker == first->speaker);
}

TEST_CASE("native ui: the typed line: the end that fits the banner line, a caret, (not sent)") {
    const auto t = ui::TypingLine("you", "hello", false);
    CHECK(t.speaker == "you");
    CHECK(t.text == "hello_");
    CHECK(ui::BannerText(t) == "you: hello_");
    CHECK(t.seconds == ui::kTypingSeconds);
    CHECK(t.colour == 0);   // (the caller gives it the speaker's colour)
    const auto failed = ui::TypingLine("you", "hi", true, "(pas envoy\xC3\xA9)");
    CHECK(failed.text == "hi_ (pas envoy\xC3\xA9)");
    CHECK(failed.colour == ui::kFailedColour);   // a send that failed: plain at a glance
    const auto l = ui::TypingLine("you", std::string(200, 'y'), false);
    CHECK(l.text.substr(0, 3) == "...");
    CHECK(l.text.substr(l.text.size() - 2) == "y_");
    CHECK(l.text.find('\n') == std::string::npos);
    // The line full (one more y would not fit), never past it.
    CHECK(ui::Width(ui::BannerText(l)) <= ui::kBannerWidth);
    CHECK(ui::Width(ui::BannerText(l)) + ui::Width("y") > ui::kBannerWidth);
    std::string zh;
    for (int i = 0; i < 20; ++i) zh += "\xE4\xBD\xA0";  // 20 em
    CHECK(ui::TypingLine("you", zh, false).text == zh + "_");
    CHECK(ui::Width(ui::BannerText(ui::TypingLine("you", zh, false))) <= ui::kBannerWidth);
    CHECK(ui::TypingLine("you", std::string("a\x0E" "b", 3), false).text == "ab_");
    // A long name: cut, and the line still fits.
    const auto n = ui::TypingLine(std::string(60, 'N'), std::string(100, 'y'), true);
    CHECK(n.speaker == std::string(10, 'N'));   // (10 x 0.82 em of the 9 a name has)
    CHECK(ui::Width(n.speaker) <= ui::kNameWidth);
    CHECK(ui::Width(ui::BannerText(n)) <= ui::kBannerWidth);
}

TEST_CASE("native ui: texts live in the game's memory, the Ref as the game reads it") {
    FakeGame g;
    ui::TextRing r;
    CHECK(r.Put(g, "x") == 0);  // no block yet
    r.SetBlock(kBlock);
    const auto a = r.Put(g, "hello\nthere");
    REQUIRE(a == kBlock);
    const auto ref = g.Get<ui::Ref>(a);
    CHECK(ref.text == a + sizeof(ui::Ref));
    CHECK(ref.bytes == 11);
    CHECK(ref.end == -1);
    CHECK(g.Text(ref.text) == "hello\nthere");
    CHECK(r.Put(g, "b") == kBlock + ui::TextRing::kSlotBytes);
    CHECK(r.Owns(ref.text));
    CHECK_FALSE(r.Owns(kBlock + ui::TextRing::kBlockBytes));
    CHECK(r.Scratch() == kBlock + ui::TextRing::kSlotBytes * (ui::TextRing::kSlots + ui::TextRing::kFixed));
    for (std::size_t i = 2; i < ui::TextRing::kSlots; ++i) r.Put(g, "z");
    CHECK(r.Put(g, "again") == kBlock);  // round again, long after
}

TEST_CASE("native ui: a chat line on the game's banner, as f_83419b10 puts one up, in the speaker's colour") {
    World w;
    REQUIRE(ui::ScreenByVtable(w.g, ui::addr::kInfoVtable) == kBanner);
    ui::Banner b;
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kNothing);  // shut, no open asked
    REQUIRE(b.Show(w.g, w.ring, Line("Saria", "hi", 0x30D040), 1000));
    CHECK(b.showing());
    // The ordinary look first: the Type animation at its first frame, as the game does for its own kinds.
    const auto type = w.g.CallsTo(At(ui::addr::kAnimFirstFrame));
    REQUIRE(type.size() == 1);
    CHECK(type[0].x[0] == kType);
    const auto sets = w.g.CallsTo(At(ui::addr::kSetPaneText));
    REQUIRE(sets.size() == 1);
    CHECK(sets[0].x[0] == kLayout);
    CHECK(sets[0].x[1] == 0x83de2306);  // T_Text_00
    CHECK(w.g.Text(w.RefAt(sets[0].x[2]).text) == "Saria: hi");
    const auto opens = w.g.CallsTo(kOpen);
    REQUIRE(opens.size() == 1);
    CHECK(opens[0].x[0] == kBanner);
    CHECK(opens[0].x[1] == ui::addr::kInfoOpenMode);
    // The line in the speaker's colour: both of the text box's colours, the game's alpha kept.
    const auto c0 = w.g.CallsTo(At(ui::addr::kSetColour0));
    const auto c1 = w.g.CallsTo(At(ui::addr::kSetColour1));
    REQUIRE(c0.size() == 1);
    REQUIRE(c1.size() == 1);
    CHECK(c0[0].x[0] == kBox);
    CHECK(c0[0].x[1] == w.ring.Scratch());
    float top[4] = {}, bottom[4] = {};
    REQUIRE(w.g.Read(c0[0].x[1], top, sizeof top));
    REQUIRE(w.g.Read(c1[0].x[1], bottom, sizeof bottom));
    CHECK(top[0] == doctest::Approx(0x30 / 255.0f));
    CHECK(top[1] == doctest::Approx(0xD0 / 255.0f));
    CHECK(top[2] == doctest::Approx(0x40 / 255.0f));
    CHECK(top[3] == doctest::Approx(1.0f));
    CHECK(bottom[1] == doctest::Approx(0xD0 / 255.0f));
    // Up with ours: the next page only changes the text (no opening again); the same colour: no colour call.
    w.Up();
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kOurs);
    REQUIRE(b.Show(w.g, w.ring, Line("Saria", "again", 0x30D040), 2000));
    CHECK(w.LastText() == "Saria: again");
    CHECK(w.g.CallsTo(kOpen).size() == 1);
    CHECK(w.g.CallsTo(At(ui::addr::kSetColour0)).size() == 1);
    // A notice: the game's own colour back.
    REQUIRE(b.Show(w.g, w.ring, Notice("Ruto joined"), 3000));
    CHECK(w.LastText() == "Ruto joined");
    const auto back = w.g.CallsTo(At(ui::addr::kSetColour1));
    REQUIRE(back.size() == 2);
    REQUIRE(w.g.Read(back[1].x[1], bottom, sizeof bottom));
    CHECK(bottom[0] == doctest::Approx(0.9f));  // the game's own second colour
}

TEST_CASE("native ui: our page closes when its time is over, only while the banner is ours") {
    World w;
    ui::Banner b;
    REQUIRE(b.Show(w.g, w.ring, Line("Saria", "hi", 0x30D040), 1000));  // 4 s
    w.Up();
    CHECK_FALSE(b.Step(w.g, w.ring, std::nullopt, 4999));
    CHECK(w.g.CallsTo(kClose).empty());
    CHECK(b.showing());
    CHECK_FALSE(b.Step(w.g, w.ring, std::nullopt, 5000));
    const auto closes = w.g.CallsTo(kClose);
    REQUIRE(closes.size() == 1);
    CHECK(closes[0].x[0] == kBanner);
    CHECK(closes[0].x[1] == static_cast<std::uint64_t>(-1));
    CHECK_FALSE(b.showing());
    CHECK(w.g.CallsTo(At(ui::addr::kSetColour1)).size() == 2);  // the game's colour back
    CHECK_FALSE(b.Step(w.g, w.ring, std::nullopt, 9000));
    CHECK(w.g.CallsTo(kClose).size() == 1);  // once
    // The next page right after a close (not taken yet): opened again.
    w.g.Put<std::int8_t>(kBanner + ui::addr::kScreenRequest, -1);
    REQUIRE(b.Show(w.g, w.ring, Line("Saria", "more"), 9000));
    CHECK(w.g.CallsTo(kOpen).size() == 2);
    // The game takes the banner over before our time is out: never closed by us, its colour its own.
    w.Up();
    w.g.Put<std::uint64_t>(kBox + ui::addr::kBoxText, 0x88000000);
    CHECK_FALSE(b.Step(w.g, w.ring, std::nullopt, 20000));
    CHECK(w.g.CallsTo(kClose).size() == 1);
    // Closed by itself: not ours any more.
    World v;
    ui::Banner c;
    REQUIRE(c.Show(v.g, v.ring, Line("Saria", "hi"), 0));
    v.Shut();
    CHECK_FALSE(c.Step(v.g, v.ring, std::nullopt, 100));
    CHECK_FALSE(c.showing());
    c.End(v.g, v.ring);
    CHECK(v.g.CallsTo(kClose).empty());
}

TEST_CASE("native ui: the typed line ends at once when the box closes") {
    World w;
    ui::Banner b;
    REQUIRE(b.Show(w.g, w.ring, ui::TypingLine("you", "hel", false), 0));
    w.Up();
    REQUIRE(b.Show(w.g, w.ring, ui::TypingLine("you", "hello", false), 100));
    CHECK(w.LastText() == "you: hello_");
    CHECK(w.g.CallsTo(kOpen).size() == 1);  // typing: no opening again
    b.End(w.g, w.ring);
    CHECK(w.g.CallsTo(kClose).size() == 1);
    CHECK_FALSE(b.showing());
}

TEST_CASE("native ui: the colour only on a text pane whose box holds colours") {
    World w;
    ui::TextTint t;
    w.g.Put<std::uint32_t>(kPane + ui::addr::kPaneFlags, 1u << 22);  // a picture pane
    CHECK_FALSE(t.Set(w.g, w.ring, kBanner, ui::addr::kNameT_Text_00, 0xFF0000));
    w.g.Put<std::uint32_t>(kPane + ui::addr::kPaneFlags, 2u << 22);
    w.g.Put<float>(kBox + ui::addr::kBoxColour0, 300.0f);  // not a colour: not the box we think
    CHECK_FALSE(t.Set(w.g, w.ring, kBanner, ui::addr::kNameT_Text_00, 0xFF0000));
    CHECK(w.g.CallsTo(At(ui::addr::kSetColour0)).empty());
    // And the line still shows, in the game's colour.
    ui::Banner b;
    CHECK(b.Show(w.g, w.ring, Line("Saria", "hi", 0xFF0000), 0));
    CHECK(w.g.CallsTo(At(ui::addr::kSetColour0)).empty());
}

TEST_CASE("native ui: no banner, or none of this build's: nothing changed") {
    World w;
    w.g.Put<std::uint64_t>(kBanner, 0x84c09678);  // another screen's vtable
    ui::Banner b;
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kGames);
    CHECK_FALSE(b.Show(w.g, w.ring, Notice("Ruto joined"), 0));
    CHECK(w.g.calls.size() == 0);
    w.g.Put<std::uint32_t>(kManager + ui::addr::kUiCount, 1);  // the banner not loaded
    CHECK_FALSE(b.Show(w.g, w.ring, Notice("Ruto joined"), 0));
    CHECK(b.Step(w.g, w.ring, std::string("You are down"), 0));  // held: waits for a banner
    CHECK(w.g.calls.empty());
}

TEST_CASE("native ui: the game's own banner is never covered: a page waits for it") {
    World w;
    ui::Banner b;
    // The game shows its own banner text (a pointer that is not ours).
    w.Up();
    w.g.Put<std::uint64_t>(kBox + ui::addr::kBoxText, 0x88000000);
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kGames);
    CHECK_FALSE(b.Show(w.g, w.ring, Notice("Ruto joined"), 0));
    CHECK(w.g.CallsTo(At(ui::addr::kSetPaneText)).empty());
    CHECK(w.g.CallsTo(kOpen).empty());
    // Opening (state 0, an open asked) with the game's text: still the game's.
    w.g.Put<std::uint8_t>(kBanner + ui::addr::kScreenState, 0);
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kGames);
    // Its text box unreadable: counted as the game's.
    w.g.Put<std::uint32_t>(kPane + ui::addr::kPaneFlags, 1u << 22);
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kGames);
    w.g.Put<std::uint32_t>(kPane + ui::addr::kPaneFlags, 2u << 22);
    // Gone (shut, no open asked): the page goes.
    w.g.Put<std::int8_t>(kBanner + ui::addr::kScreenRequest, 0);
    CHECK(b.Show(w.g, w.ring, Notice("Ruto joined"), 0));
    CHECK(w.g.CallsTo(kOpen).size() == 1);
}

TEST_CASE("native ui: a game that keeps its own copy of our text: ours until the game sets another") {
    World w;
    w.g.copy_to = 0x77000000;  // the text box points at the game's copy, not at our block
    ui::Banner b;
    REQUIRE(b.Show(w.g, w.ring, Notice("Ruto joined"), 0));
    w.Up();
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kOurs);
    w.g.Put<std::uint64_t>(kBox + ui::addr::kBoxText, 0x88000000);  // the game's own text now
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kGames);
    CHECK_FALSE(b.Show(w.g, w.ring, Notice("Ruto left"), 0));
}

TEST_CASE("native ui: once our banner is shut or let go, the game's next one from the same buffer is the game's") {
    World w;
    w.g.copy_to = 0x77000000;  // the game shows every banner text from one buffer of its own
    ui::Banner b;
    REQUIRE(b.Show(w.g, w.ring, Notice("Ruto joined"), 0));
    w.Up();
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kOurs);
    // Our notice ends (shut, no open asked) ...
    w.Shut();
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kNothing);
    // ... and the game opens its own banner, its text in the same buffer: the game's.
    w.Up();
    CHECK(b.Holding(w.g, w.ring) == ui::Holds::kGames);
    CHECK_FALSE(b.Show(w.g, w.ring, Notice("Ruto left"), 0));

    // The same after a held line is let go: the game's banner from that buffer is never closed by us.
    World v;
    v.g.copy_to = 0x77000000;
    ui::Banner h;
    ui::HoldBanner("You are down  0:05");
    REQUIRE(h.Step(v.g, v.ring, ui::HeldBanner(), 0));
    REQUIRE(h.up());
    v.Up();
    ui::EndBanner();
    CHECK_FALSE(h.Step(v.g, v.ring, ui::HeldBanner(), 0));
    CHECK(v.g.CallsTo(kClose).size() == 1);  // ours: closed
    // The close not taken yet, the game puts its own text up in the same buffer: the game's, never closed again.
    CHECK(h.Holding(v.g, v.ring) == ui::Holds::kGames);
    CHECK_FALSE(h.Step(v.g, v.ring, ui::HeldBanner(), 0));
    CHECK(v.g.CallsTo(kClose).size() == 1);
}

TEST_CASE("native ui: a held banner line stays, changes its text in place, closes when let go") {
    World w;
    ui::Banner b;
    CHECK_FALSE(ui::HeldBanner());
    ui::HoldBanner("You are down  0:32");
    REQUIRE(ui::HeldBanner());
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner(), 0));
    CHECK(b.up());
    CHECK(w.g.CallsTo(kOpen).size() == 1);
    CHECK(w.g.CallsTo(At(ui::addr::kSetPaneText)).size() == 1);
    CHECK(w.g.CallsTo(At(ui::addr::kAnimFirstFrame)).size() == 1);  // the ordinary look, as the game's own
    // Pages wait while it is held.
    CHECK_FALSE(b.Show(w.g, w.ring, Line("Saria", "hi"), 0));
    w.g.Put<std::int8_t>(kBanner + ui::addr::kScreenRequest, 1);  // the open taken
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner(), 100));
    CHECK(w.g.CallsTo(At(ui::addr::kSetPaneText)).size() == 1);  // the same text: nothing set
    ui::HoldBanner("You are down  0:31");
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner(), 1000));
    const auto sets = w.g.CallsTo(At(ui::addr::kSetPaneText));
    REQUIRE(sets.size() == 2);
    CHECK(sets[1].x[2] == sets[0].x[2]);  // the fixed slot, written in place: the ring does not move
    CHECK(sets[0].x[2] == kBlock + ui::TextRing::kSlotBytes * ui::TextRing::kSlots);
    CHECK(w.g.Text(w.RefAt(sets[1].x[2]).text) == "You are down  0:31");
    CHECK(w.ring.Put(w.g, "chat") == kBlock);  // the ring's first slot still next
    CHECK(w.g.CallsTo(kOpen).size() == 1);  // up already: not opened again
    CHECK(w.g.CallsTo(At(ui::addr::kAnimFirstFrame)).size() == 1);  // nor its Type put back (revive-flicker-0121)
    // The banner closed by itself (state 0, nothing asked): opened again.
    w.g.Put<std::int8_t>(kBanner + ui::addr::kScreenRequest, 0);
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner(), 2000));
    CHECK(w.g.CallsTo(kOpen).size() == 2);
    CHECK(w.g.CallsTo(At(ui::addr::kAnimFirstFrame)).size() == 2);  // put up again: the ordinary look again
    w.g.Put<std::int8_t>(kBanner + ui::addr::kScreenRequest, 1);
    ui::EndBanner();
    CHECK_FALSE(ui::HeldBanner());
    CHECK_FALSE(b.Step(w.g, w.ring, ui::HeldBanner(), 3000));
    const auto closes = w.g.CallsTo(kClose);
    REQUIRE(closes.size() == 1);
    CHECK(closes[0].x[0] == kBanner);
    CHECK(closes[0].x[1] == static_cast<std::uint64_t>(-1));
    CHECK_FALSE(b.up());
    CHECK_FALSE(b.Step(w.g, w.ring, std::nullopt, 4000));
    CHECK(w.g.CallsTo(kClose).size() == 1);  // once
}

TEST_CASE("native ui: a held line whose text changes every frame never replays the banner (revive-flicker-0121)") {
    // 0.12.0: the downed player's line while revived ('Saria is reviving you  0:45  NN%') changes its share every frame;
    // each change put the banner's Type animation back at its first frame (Banner::Set), and the banner blinked.
    World w;
    ui::Banner b;
    ui::HoldBanner("Saria is reviving you  0:45  0%");
    REQUIRE(b.Step(w.g, w.ring, ui::HeldBanner(), 0));
    w.Up();
    for (int frame = 1; frame <= 100; ++frame) {
        ui::HoldBanner("Saria is reviving you  0:45  " + std::to_string(frame) + "%");
        CHECK(b.Step(w.g, w.ring, ui::HeldBanner(), static_cast<std::uint64_t>(frame) * 33));
    }
    CHECK(w.g.CallsTo(kOpen).size() == 1);
    CHECK(w.g.CallsTo(At(ui::addr::kAnimFirstFrame)).size() == 1);   // once, when it was put up
    CHECK(w.g.CallsTo(kClose).empty());
    CHECK(w.g.CallsTo(At(ui::addr::kSetPaneText)).size() == 101);    // the text kept current, every frame
    CHECK(w.LastText() == "Saria is reviving you  0:45  100%");
    // The reviver's line ('Reviving Saria'), the downed clock, spectating's next player: the same held path.
    ui::HoldBanner("Reviving Saria  0:45  50%");
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner(), 4000));
    CHECK(w.g.CallsTo(At(ui::addr::kAnimFirstFrame)).size() == 1);
    ui::EndBanner();
}

TEST_CASE("native ui: a page of ours up already: the next page changes only the text (revive-flicker-0121)") {
    World w;
    ui::Banner b;
    REQUIRE(b.Show(w.g, w.ring, Line("Saria", "one"), 0));
    w.Up();
    REQUIRE(b.Show(w.g, w.ring, Line("Saria", "two"), 100));
    CHECK(w.g.CallsTo(kOpen).size() == 1);
    CHECK(w.g.CallsTo(At(ui::addr::kAnimFirstFrame)).size() == 1);
    CHECK(w.g.CallsTo(At(ui::addr::kSetPaneText)).size() == 2);
}

TEST_CASE("native ui: the held line takes the banner from a page of ours, in the game's own colour") {
    World w;
    ui::Banner b;
    REQUIRE(b.Show(w.g, w.ring, Line("Saria", "hi", 0x30D040), 0));
    w.Up();
    ui::HoldBanner("You are down  0:45");
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner(), 100));
    CHECK(b.up());
    CHECK_FALSE(b.showing());
    CHECK(w.LastText() == "You are down  0:45");
    CHECK(w.g.CallsTo(At(ui::addr::kSetColour1)).size() == 2);  // the game's colour back
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner(), 60000));  // the page's time long over: the held line is not closed
    CHECK(w.g.CallsTo(kClose).empty());
    ui::EndBanner();
}

TEST_CASE("native ui: the held line waits for the game's own banner, and never closes it") {
    World w;
    ui::Banner b;
    w.Up();
    w.g.Put<std::uint64_t>(kBox + ui::addr::kBoxText, 0x88000000);  // the game's own banner text
    ui::HoldBanner("You are down  0:20");
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner(), 0));  // held (pages wait) ...
    CHECK_FALSE(b.up());                               // ... but not shown
    CHECK(w.g.CallsTo(At(ui::addr::kSetPaneText)).empty());
    CHECK(w.g.CallsTo(kOpen).empty());
    // The game's banner gone: the held line shows.
    w.Shut();
    CHECK(b.Step(w.g, w.ring, ui::HeldBanner(), 0));
    CHECK(b.up());
    CHECK(w.g.CallsTo(kOpen).size() == 1);
    // The game takes the banner over, then the line is let go: the game's banner stays.
    w.Up();
    w.g.Put<std::uint64_t>(kBox + ui::addr::kBoxText, 0x88000000);
    ui::EndBanner();
    CHECK_FALSE(b.Step(w.g, w.ring, ui::HeldBanner(), 0));
    CHECK(w.g.CallsTo(kClose).empty());
    CHECK_FALSE(b.up());  // (pages are free to wait for it again)
}

// hud-fit-0120 (gate-hud shot of 2f734963a, B: the banner showed a page of only "things changed)"): a long line's
// pages are about the same width, each but the last ends " ..." and each but the first starts "... ", none wider than
// the banner, and the words are all there in order.
TEST_CASE("native ui: banner pages are balanced and marked as going on") {
    ui::ResetFontWidths();
    const std::string notice = "A loaded a save - the group follows it (26 items, 2 songs, 1 maps and compasses, "
                               "1 heart containers, 1 tokens, 5 other things changed)";
    const auto pages = ui::BannerPages(notice, ui::kBannerWidth);
    REQUIRE(pages.size() >= 2);
    CHECK(pages.size() == ui::Wrap(notice, ui::kBannerWidth - 2 * (ui::Width("...") + ui::Width(" ")), 1).size());
    std::string back;
    std::size_t narrowest = ui::kBannerWidth, widest = 0;
    for (std::size_t k = 0; k < pages.size(); ++k) {
        CAPTURE(pages[k]);
        CHECK(ui::Width(pages[k]) <= ui::kBannerWidth);
        std::string body = pages[k];
        if (k > 0) {
            REQUIRE(body.rfind("... ", 0) == 0);
            body = body.substr(4);
        }
        if (k + 1 < pages.size()) {
            REQUIRE(body.size() > 4);
            CHECK(body.substr(body.size() - 4) == " ...");
            body.resize(body.size() - 4);
        }
        back += (back.empty() ? "" : " ") + body;
        narrowest = std::min(narrowest, ui::Width(body));
        widest = std::max(widest, ui::Width(body));
    }
    CHECK(back == notice);
    CHECK(narrowest * 2 >= widest);   // no page a short tail of the line ("things changed)" alone)
    CHECK(pages.back() != "... things changed)");
    // A line that fits: one page, unmarked.
    CHECK(ui::BannerPages("Ruto joined", ui::kBannerWidth) == std::vector<std::string>{"Ruto joined"});
    CHECK(ui::BannerPages("", ui::kBannerWidth).empty());
    // One word longer than any page: still every page fits.
    for (const auto& p : ui::BannerPages(std::string(300, 'W'), ui::kBannerWidth)) CHECK(ui::Width(p) <= ui::kBannerWidth);
}
