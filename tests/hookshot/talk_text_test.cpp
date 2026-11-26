// SPDX-License-Identifier: GPL-3.0-or-later
// The texts the game sets in its text panes, for the play-test bridge's `text` (coop_talk_text.h): a fake memory
// with a layout as Longshot's host/hos_wide_ui.c reads one.
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "coop_playtest_core.h"
#include "coop_talk_text.h"

using namespace CoopTalkText;

namespace {

// A byte-addressed fake guest memory: whatever was put is readable, everything else fails.
struct Fake {
    std::map<std::uint64_t, std::uint8_t> bytes;
    void Put(std::uint64_t at, const void* src, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) bytes[at + i] = static_cast<const std::uint8_t*>(src)[i];
    }
    void U64(std::uint64_t at, std::uint64_t v) { Put(at, &v, 8); }
    void U32(std::uint64_t at, std::uint32_t v) { Put(at, &v, 4); }
    void Str(std::uint64_t at, const std::string& s) { Put(at, s.c_str(), s.size() + 1); }
    ReadFn Reader() {
        return [this](std::uint64_t a, void* out, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                const auto it = bytes.find(a + i);
                if (it == bytes.end()) return false;
                static_cast<std::uint8_t*>(out)[i] = it->second;
            }
            return true;
        };
    }
};

// MessageInGame_00 with three panes: N_Root_00, T_Name_00, T_MessageWhiteOut_00.
constexpr std::uint64_t kLayout = 0x1000, kRes = 0x2000, kPanes = 0x3000, kNames = 0x4000;
constexpr std::uint64_t kName = 0x3000 + 1 * kPaneSize, kText = 0x3000 + 2 * kPaneSize;
constexpr std::uint64_t kRef = 0x8000, kBuf = 0x9000;

Fake Window() {
    Fake m;
    m.U64(kLayout + kLayoutRes, kRes);
    m.U64(kLayout + kLayoutPanes, kPanes);
    m.U64(kRes + kResName, 0x5000);
    m.Str(0x5000, "MessageInGame_00");
    m.U64(kRes + kResPaneNames, kNames);
    const char* names[] = {"N_Root_00", "T_Name_00", "T_MessageWhiteOut_00"};
    for (int i = 0; i < 3; ++i) {
        m.U64(kNames + 8 * i, 0x6000 + 0x40 * i);
        m.Str(0x6000 + 0x40 * i, names[i]);
        m.U64(kPanes + i * kPaneSize + kPaneLayout, kLayout);
    }
    return m;
}

// The setter's reference for `text`, `length` bytes counted (default: the text's own size).
void Set(Fake& m, const std::string& text, std::uint64_t buf = kBuf, std::int64_t length = -1) {
    m.Put(buf, text.data(), text.size());
    m.U64(kRef + kRefText, text.empty() ? 0 : buf);
    m.U32(kRef + kRefLength, length < 0 ? static_cast<std::uint32_t>(text.size()) : static_cast<std::uint32_t>(length));
}

}  // namespace

TEST_CASE("talk text: a text set in the message window is kept with its layout and pane") {
    Fake m = Window();
    Log log;
    Set(m, "Hey! You're finally awake!");
    log.Note(kText, kRef, 42, m.Reader());
    const auto all = log.Since(0, "");
    REQUIRE(all.size() == 1);
    CHECK(all[0].seq == 1);
    CHECK(all[0].frame == 42);
    CHECK(all[0].layout == "MessageInGame_00");
    CHECK(all[0].pane == "T_MessageWhiteOut_00");
    CHECK(all[0].text == "Hey! You're finally awake!");
    CHECK_FALSE(all[0].cut);
    CHECK(log.Last() == 1);
}

TEST_CASE("talk text: the window sets the same text every frame, it is kept once") {
    Fake m = Window();
    Log log;
    Set(m, "Hello");
    for (int f = 0; f < 30; ++f) log.Note(kText, kRef, f, m.Reader());
    CHECK(log.Since(0, "").size() == 1);
    CHECK(log.Seen() == 30);
    // The same text in another pane (the speaker) is that pane's own.
    log.Note(kName, kRef, 31, m.Reader());
    const auto all = log.Since(0, "");
    REQUIRE(all.size() == 2);
    CHECK(all[1].pane == "T_Name_00");
    // A new text, then the old one again: both kept (a second box may say the same thing).
    Set(m, "Bye");
    log.Note(kText, kRef, 40, m.Reader());
    Set(m, "Hello");
    log.Note(kText, kRef, 41, m.Reader());
    CHECK(log.Since(0, "").size() == 4);
}

TEST_CASE("talk text: the same text in the same pane after a frame gap is kept again") {
    Fake m = Window();
    Log log;
    Set(m, "Hello");
    for (int f = 0; f < 10; ++f) log.Note(kText, kRef, f, m.Reader());   // (one talk: set every frame)
    CHECK(log.Since(0, "").size() == 1);
    // Within kSameAgain frames of the last set: still the same showing, not kept.
    log.Note(kText, kRef, 9 + kSameAgain, m.Reader());
    CHECK(log.Since(0, "").size() == 1);
    // A later talk: the pane was not set for more than kSameAgain frames, then the same text again.
    const std::uint64_t later = 9 + 2 * kSameAgain + 1;
    log.Note(kText, kRef, later, m.Reader());
    auto all = log.Since(0, "");
    REQUIRE(all.size() == 2);
    CHECK(all[1].text == "Hello");
    CHECK(all[1].frame == later);
    // Then set every frame again: kept once.
    for (std::uint64_t f = later + 1; f < later + 20; ++f) log.Note(kText, kRef, f, m.Reader());
    CHECK(log.Since(0, "").size() == 2);
    // An empty text after a gap is not kept again either (a window closed, closed again).
    Set(m, "");
    log.Note(kText, kRef, later + 20, m.Reader());
    CHECK(log.Since(0, "").size() == 3);
    log.Note(kText, kRef, later + 100, m.Reader());
    CHECK(log.Since(0, "").size() == 3);
}

TEST_CASE("talk text: a text that only grows replaces its shorter self") {
    Fake m = Window();
    Log log;
    for (const std::string s : {"H", "He", "Hel", "Hello"}) {
        Set(m, s);
        log.Note(kText, kRef, 1, m.Reader());
    }
    auto all = log.Since(0, "");
    REQUIRE(all.size() == 1);
    CHECK(all[0].text == "Hello");
    CHECK(all[0].seq == 4);   // (a reader that saw "He" at seq 2 asks since 2 and gets the grown text)
    // Not when another text came between: the shorter one stays.
    Set(m, "Name");
    log.Note(kName, kRef, 2, m.Reader());
    Set(m, "Hello there");
    log.Note(kText, kRef, 3, m.Reader());
    all = log.Since(0, "");
    REQUIRE(all.size() == 3);
    CHECK(all[0].text == "Hello");
    CHECK(all[1].text == "Name");
    CHECK(all[2].text == "Hello there");
}

TEST_CASE("talk text: since and the layout filter") {
    Fake m = Window();
    Log log;
    for (int i = 0; i < 5; ++i) {
        Set(m, "box " + std::to_string(i));
        log.Note(kText, kRef, i, m.Reader());
    }
    const auto after = log.Since(3, "");
    REQUIRE(after.size() == 2);
    CHECK(after[0].text == "box 3");
    CHECK(log.Since(5, "").empty());
    CHECK(log.Since(0, "MessageInGame").size() == 5);
    CHECK(log.Since(0, "PickUpWin").empty());
}

TEST_CASE("talk text: trailing 0s go, a long text is cut, an empty first set is not kept") {
    Fake m = Window();
    Log log;
    Set(m, std::string("Hi\0\0", 4));
    log.Note(kText, kRef, 1, m.Reader());
    auto all = log.Since(0, "");
    REQUIRE(all.size() == 1);
    CHECK(all[0].text == "Hi");
    CHECK(all[0].length == 4);
    // A clear (no text) after it is kept: the box closed.
    Set(m, "");
    log.Note(kText, kRef, 2, m.Reader());
    all = log.Since(0, "");
    REQUIRE(all.size() == 2);
    CHECK(all[1].text.empty());
    // A first set to nothing in a pane never seen: not kept.
    Log fresh;
    fresh.Note(kName, kRef, 3, m.Reader());
    CHECK(fresh.Since(0, "").empty());
    // Longer than kMaxBytes.
    Set(m, std::string(kMaxBytes + 100, 'a'));
    log.Note(kText, kRef, 4, m.Reader());
    all = log.Since(2, "");
    REQUIRE(all.size() == 1);
    CHECK(all[0].cut);
    CHECK(all[0].text.size() == kMaxBytes);
    CHECK(all[0].length == kMaxBytes + 100);
}

TEST_CASE("talk text: memory that cannot be read keeps nothing or an empty name") {
    Fake m = Window();
    Log log;
    log.Note(kText, 0xdead0000, 1, m.Reader());   // (the reference cannot be read)
    log.Note(0, kRef, 1, m.Reader());
    CHECK(log.Since(0, "").empty());
    // The text pointer cannot be read.
    m.U64(kRef + kRefText, 0xbad00000);
    m.U32(kRef + kRefLength, 5);
    log.Note(kText, kRef, 1, m.Reader());
    CHECK(log.Since(0, "").empty());
    // A pane outside any layout: kept, with no names.
    Set(m, "orphan");
    log.Note(0x7000, kRef, 2, m.Reader());
    const auto all = log.Since(0, "");
    REQUIRE(all.size() == 1);
    CHECK(all[0].layout.empty());
    CHECK(all[0].pane.empty());
    // A pane between two array slots: the layout, no pane name.
    m.U64(kPanes + 8 + kPaneLayout, kLayout);
    std::string layout, name;
    PaneNames(kPanes + 8, m.Reader(), layout, name);
    CHECK(layout == "MessageInGame_00");
    CHECK(name.empty());
}

TEST_CASE("talk text: a name is read to its 0, never past kMaxName") {
    Fake m;
    m.Str(0x100, "Short");
    CHECK(ReadName(0x100, m.Reader()) == "Short");
    // At the end of what can be read: still read byte by byte.
    m.Str(0x200, "AB");
    CHECK(ReadName(0x200, m.Reader()) == "AB");
    m.Str(0x300, std::string(kMaxName + 10, 'x'));
    CHECK(ReadName(0x300, m.Reader()).empty());
    CHECK(ReadName(0, m.Reader()).empty());
}

TEST_CASE("talk text: the oldest go first and the reader is told it lost some") {
    Fake m = Window();
    Log log;
    for (std::size_t i = 0; i < kKeep + 10; ++i) {
        Set(m, "line " + std::to_string(i) + " .");   // (never a grown copy of the one before)
        log.Note(kText, kRef, i, m.Reader());
    }
    bool lost = false;
    const auto all = log.Since(0, "", &lost);
    CHECK(all.size() == kKeep);
    CHECK(lost);
    CHECK(all.front().seq == 11);
    log.Since(10, "", &lost);
    CHECK_FALSE(lost);
    log.Since(log.Last(), "", &lost);
    CHECK_FALSE(lost);
}

TEST_CASE("talk text: JSON text keeps UTF-8 and shows the game's other bytes") {
    CHECK(JsonText("plain") == "\"plain\"");
    CHECK(JsonText("say \"hi\"\\") == "\"say \\\"hi\\\"\\\\\"");
    CHECK(JsonText("a\nb") == "\"a\\u000ab\"");
    CHECK(JsonText("Kok\xc3\xadri \xe2\x80\x94 ok") == "\"Kok\xc3\xadri \xe2\x80\x94 ok\"");
    CHECK(JsonText(std::string("\x0e\x01x", 3)) == "\"\\u000e\\u0001x\"");
    CHECK(JsonText("\xff\xc3") == "\"\\u00ff\\u00c3\"");   // (not UTF-8: each byte shown)
    CHECK_FALSE(HasMarkup("Kok\xc3\xadri"));
    CHECK(HasMarkup(std::string("a\x0e" "b", 3)));
    CHECK(HasMarkup("\xff"));
    // The whole reply parses as JSON.
    Entry e;
    e.seq = 3;
    e.frame = 9;
    e.layout = "MessageInGame_00";
    e.pane = "T_MessageWhiteOut_00";
    e.text = std::string("Buy?\n\x0e\x05 Yes", 10);
    e.length = 10;
    const std::string members = Members(true, false, 3, false, {e});
    const auto v = CoopModJson::Parse("{" + members + "}");
    REQUIRE(v);
    CHECK(v->Bool("hooked").value_or(false));
    CHECK(v->Number("last").value_or(0) == 3);
    CHECK_FALSE(v->Bool("body").value_or(true));
    const auto* texts = v->Get("texts");
    REQUIRE(texts);
    REQUIRE(texts->array.size() == 1);
    CHECK(texts->array[0].Text("text").value_or("") == e.text);
    CHECK(texts->array[0].Bool("markup").value_or(false));
    CHECK(texts->array[0].Text("pane").value_or("") == "T_MessageWhiteOut_00");
    CHECK(texts->array[0].Number("wrap").value_or(0) == 25);
}

TEST_CASE("talk text: threads may note at once") {
    Fake m = Window();
    Log log;
    Set(m, "same");
    const auto read = m.Reader();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) log.Note(kText, kRef, i, read);
        });
    }
    for (auto& t : threads) t.join();
    CHECK(log.Since(0, "").size() == 1);
    CHECK(log.Seen() == 800);
}

TEST_CASE("talk text: the bridge's text request") {
    using CoopPlaytest::ParseRequest;
    std::string error;
    auto r = ParseRequest(R"({"cmd":"text","since":12,"layout":"MessageInGame","id":1})", error);
    REQUIRE(r);
    CHECK(r->cmd == "text");
    CHECK(r->since == 12);
    CHECK(r->layout == "MessageInGame");
    r = ParseRequest(R"({"cmd":"text"})", error);
    REQUIRE(r);
    CHECK(r->since == 0);
    CHECK(r->layout.empty());
    CHECK_FALSE(ParseRequest(R"({"cmd":"text","since":-1})", error));
    CHECK(error.find("since") != std::string::npos);
    CHECK_FALSE(ParseRequest(R"({"cmd":"text","since":1.5})", error));
    CHECK_FALSE(ParseRequest(R"({"cmd":"text","layout":5})", error));
    CHECK(error.find("layout") != std::string::npos);
    CHECK_FALSE(CoopPlaytest::GatedCommand("text"));
    CHECK_FALSE(ParseRequest(R"({"cmd":"nope"})", error));
    CHECK(error.find("text") != std::string::npos);
}

TEST_CASE("talk text: panes only ever cleared do not grow the frame memory past kMaxPanes") {
    Fake m = Window();
    m.U64(kRef + kRefText, 0);
    m.U32(kRef + kRefLength, 0);
    Log log;
    for (std::uint64_t i = 0; i < kMaxPanes + 50; ++i) log.Note(0x100000 + i * kPaneSize, kRef, i, m.Reader());
    CHECK(log.PanesTimed() <= kMaxPanes);
    CHECK(log.PanesTimed() > 0);
    CHECK(log.Last() == 0);   // (nothing kept: every set was a clear)
}

TEST_CASE("talk text: a letter-animated pane's text (wrap 26) is taken as given, its length 16 bits") {
    Fake m = Window();
    Log log;
    m.Str(kBuf, "Line 1");
    // The game masks w2 to 16 bits: high bits in x2 are not the length.
    log.NoteText(kText, kBuf, 0xabcd0000ull | 6, 4, m.Reader(), 26);
    auto got = log.Since(0, "");
    REQUIRE(got.size() == 1);
    CHECK(got[0].text == "Line 1");
    CHECK(got[0].length == 6);
    CHECK(got[0].wrap == 26);
    CHECK(got[0].layout == "MessageInGame_00");
    CHECK(got[0].pane == "T_MessageWhiteOut_00");
    // The same text in the same pane while it shows: not kept again, whichever wrap gave it.
    log.NoteText(kText, kBuf, 6, 5, m.Reader(), 26);
    m.U64(kRef + kRefText, kBuf);
    m.U32(kRef + kRefLength, 6);
    log.Note(kText, kRef, 6, m.Reader(), 27);
    CHECK(log.Since(0, "").size() == 1);
    // A text pointer that cannot be read keeps nothing; a zero pane neither.
    log.NoteText(kText, 0x77770000, 3, 7, m.Reader(), 26);
    log.NoteText(0, kBuf, 6, 8, m.Reader(), 26);
    CHECK(log.Since(0, "").size() == 1);
}

TEST_CASE("talk text: the alias setter (wrap 27) reads the reference as 0041's and says its wrap") {
    Fake m = Window();
    Log log;
    m.Str(kBuf, "Speaker A");
    m.U64(kRef + kRefText, kBuf);
    m.U32(kRef + kRefLength, 9);
    log.Note(kName, kRef, 1, m.Reader(), 27);
    const auto got = log.Since(0, "MessageInGame");
    REQUIRE(got.size() == 1);
    CHECK(got[0].text == "Speaker A");
    CHECK(got[0].pane == "T_Name_00");
    CHECK(got[0].wrap == 27);
    const auto v = CoopModJson::Parse("{" + Members(true, true, log.Last(), false, got) + "}");
    REQUIRE(v);
    CHECK(v->Bool("body").value_or(false));
    CHECK(v->Get("texts")->array[0].Number("wrap").value_or(0) == 27);
}

TEST_CASE("talk text: the wrapped setters are the ones read from main") {
    CHECK(0x80020000 + kPaneSetBody == 0x8078a460);
    CHECK(0x80020000 + kPaneSetAlias == 0x8379a990);
}
