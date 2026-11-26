// SPDX-License-Identifier: GPL-3.0-or-later
// The game's menu screens as code mods see them (hookshot/src/coop_mod_screens.*): which screen a vtable is, the one
// focused button of a screen's list, and a button's pane name, on a made-up memory.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop_mod_api.h"
#include "coop_mod_screens.h"

using namespace CoopModScreens;

namespace {

// Guest memory as separate bytes; anything not written is not mapped.
struct Memory {
    std::map<std::uint64_t, std::uint8_t> bytes;
    void Put(std::uint64_t a, const void* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) bytes[a + i] = static_cast<const std::uint8_t*>(p)[i];
    }
    void U64(std::uint64_t a, std::uint64_t v) { Put(a, &v, 8); }
    void U32(std::uint64_t a, std::uint32_t v) { Put(a, &v, 4); }
    void U8(std::uint64_t a, std::uint8_t v) { Put(a, &v, 1); }
    void Text(std::uint64_t a, const std::string& t) { Put(a, t.data(), t.size()); }
    Reader reader() {
        return [this](std::uint64_t a, void* out, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                auto it = bytes.find(a + i);
                if (it == bytes.end()) return false;
                static_cast<std::uint8_t*>(out)[i] = it->second;
            }
            return true;
        };
    }
};

constexpr std::uint64_t kScreen = 0x200000, kFocusObj = 0x210000, kManager = 0x220000;
constexpr std::int32_t kEntryOff = 0x40;   // a node is this far into its entry

// A screen with the given buttons' states, in list order (buttons at 0x300000 + 0x1000 * i).
Memory ScreenWith(std::initializer_list<std::uint8_t> states, bool taking_input = true) {
    Memory m;
    m.U64(kScreen + kScreenFocus, kFocusObj);
    m.U64(kFocusObj + kFocusManager, taking_input ? kManager : 0);
    m.U32(kScreen + kListEntryOffset, static_cast<std::uint32_t>(kEntryOff));
    const std::uint64_t head = kScreen + kListHead;
    std::uint64_t prev_next = kScreen + kListFirst;
    int i = 0;
    for (const std::uint8_t state : states) {
        const std::uint64_t entry = 0x400000 + 0x100ull * static_cast<std::uint64_t>(i);
        const std::uint64_t node = entry + static_cast<std::uint64_t>(kEntryOff);
        const std::uint64_t button = 0x300000 + 0x1000ull * static_cast<std::uint64_t>(i);
        m.U64(prev_next, node);
        m.U64(entry + kEntryButton, button);
        m.U8(button + kButtonState, state);
        prev_next = node + 8;
        ++i;
    }
    m.U64(prev_next, head);
    return m;
}

}  // namespace

TEST_CASE("mod screens: the title, the load list and its question are named, the rest is other") {
    CHECK(KindOf(kTitleVtable) == COOP_MOD_SCREEN_TITLE);
    CHECK(KindOf(kLoadListVtable) == COOP_MOD_SCREEN_LOAD_LIST);
    CHECK(KindOf(kLoadQuestionVtable) == COOP_MOD_SCREEN_LOAD_QUESTION);
    CHECK(KindOf(0x4bebaa8) == COOP_MOD_SCREEN_OTHER);   // (the pause menu's pages)
    CHECK(KindOf(0) == COOP_MOD_SCREEN_OTHER);
}

TEST_CASE("mod screens: the focused button is the one whose state is set and not decided") {
    Memory m = ScreenWith({0, 1, 0});
    CHECK(FocusedButton(m.reader(), kScreen) == 0x301000);
    m = ScreenWith({4, 0, 2, 5});   // (4 and 5: decided, not focus)
    CHECK(FocusedButton(m.reader(), kScreen) == 0x302000);
}

TEST_CASE("mod screens: no focused button with none, with two, or while the screen takes no input") {
    Memory none = ScreenWith({0, 0});
    CHECK(FocusedButton(none.reader(), kScreen) == 0);
    Memory two = ScreenWith({1, 1});
    CHECK(FocusedButton(two.reader(), kScreen) == 0);
    Memory idle = ScreenWith({0, 1}, false);
    CHECK(FocusedButton(idle.reader(), kScreen) == 0);
    Memory empty;   // (not mapped at all: the HUD's kind of object)
    CHECK(FocusedButton(empty.reader(), kScreen) == 0);
}

TEST_CASE("mod screens: a long list's focused button is found far down it (the pause menu's pages hold 100 and more)") {
    std::vector<std::uint8_t> states(120, 0);
    states[100] = 3;
    Memory m;
    {
        // (ScreenWith for 120 buttons)
        m.U64(kScreen + kScreenFocus, kFocusObj);
        m.U64(kFocusObj + kFocusManager, kManager);
        m.U32(kScreen + kListEntryOffset, static_cast<std::uint32_t>(kEntryOff));
        std::uint64_t prev_next = kScreen + kListFirst;
        for (std::size_t i = 0; i < states.size(); ++i) {
            const std::uint64_t entry = 0x400000 + 0x100ull * i, node = entry + static_cast<std::uint64_t>(kEntryOff);
            const std::uint64_t button = 0x300000 + 0x400ull * i;
            m.U64(prev_next, node);
            m.U64(entry + kEntryButton, button);
            m.U8(button + kButtonState, states[i]);
            prev_next = node + 8;
        }
        m.U64(prev_next, kScreen + kListHead);
    }
    CHECK(FocusedButton(m.reader(), kScreen) == 0x300000 + 0x400ull * 100);
}

TEST_CASE("mod screens: a list that never comes back to its head stops after kMaxButtons") {
    Memory m;
    m.U64(kScreen + kScreenFocus, kFocusObj);
    m.U64(kFocusObj + kFocusManager, kManager);
    m.U32(kScreen + kListEntryOffset, 0);
    m.U64(kScreen + kListFirst, 0x500000);
    m.U64(0x500000 + 8, 0x500000);   // (a node that points at itself)
    m.U64(0x500000 + kEntryButton, 0x600000);
    m.U8(0x600000 + kButtonState, 1);
    CHECK(FocusedButton(m.reader(), kScreen) == 0);   // (the same button found twice: not one focus)
}

TEST_CASE("mod screens: a sound's name is letters, digits and _, at most 63, and the call is searchAndEmit's") {
    CHECK(SoundName("button_decide"));
    CHECK(SoundName("Pa_Button_00_decide"));
    CHECK(SoundName("mc_TabToRight"));
    CHECK_FALSE(SoundName(""));
    CHECK_FALSE(SoundName(nullptr));
    CHECK_FALSE(SoundName("button decide"));
    CHECK_FALSE(SoundName("../x"));
    CHECK(SoundName(std::string(63, 'a').c_str()));
    CHECK_FALSE(SoundName(std::string(64, 'a').c_str()));
    CHECK(0x80020000 + kScreenSound == 0x8045e400);
}

TEST_CASE("mod screens: a button takes a choice only with both bits of its flags") {
    Memory m;
    const std::uint64_t b = 0x700000;
    m.U32(b + kButtonFlags, 0x3f);   // (the title's Continue once it takes input)
    CHECK(Choosable(m.reader(), b));
    m.U32(b + kButtonFlags, 0x3b);   // (while the title comes in)
    CHECK_FALSE(Choosable(m.reader(), b));
    m.U32(b + kButtonFlags, 0x10);
    CHECK_FALSE(Choosable(m.reader(), b));
    CHECK_FALSE(Choosable(m.reader(), 0));
    CHECK_FALSE(Choosable(m.reader(), 0x710000));   // (not mapped)
}

TEST_CASE("mod screens: a button's name is the text its +0x18 points at") {
    Memory m;
    const std::uint64_t b = 0x700000;
    m.U64(b + kNameText, 0x710000);
    m.Text(0x710000, std::string("Pa_Button_00\0Pa_Button_01\0", 26));   // (the names lie end to end)
    CHECK(ButtonName(m.reader(), b) == "Pa_Button_00");
}

TEST_CASE("mod screens: no name for no button, an unmapped, unended or unprintable text, and long names are cut") {
    Memory m;
    CHECK(ButtonName(m.reader(), 0).empty());
    const std::uint64_t b = 0x700000;
    CHECK(ButtonName(m.reader(), b).empty());   // (no pointer)
    m.U64(b + kNameText, 0x710000);
    CHECK(ButtonName(m.reader(), b).empty());   // (the text is not mapped)
    m.Text(0x710000, "Pa_Bu");                  // (mapped, then unmapped before its end)
    CHECK(ButtonName(m.reader(), b).empty());
    m.Text(0x710000, std::string("Pa\x01x\0", 5));
    CHECK(ButtonName(m.reader(), b).empty());
    m.Text(0x710000, std::string(200, 'a'));
    CHECK(ButtonName(m.reader(), b) == std::string(kNameMax, 'a'));
}

TEST_CASE("mod screens: every button of a screen's list, in its order, whether or not the screen takes input") {
    Memory m = ScreenWith({0, 3, 0}, false);
    CHECK(Buttons(m.reader(), kScreen) == std::vector<std::uint64_t>{0x300000, 0x301000, 0x302000});
    CHECK(Buttons(m.reader(), 0).empty());
}

TEST_CASE("mod screens: a button's hit box is its rectangle through its box's global matrix, none while not shown") {
    Memory m;
    const std::uint64_t button = 0x300000, box = 0x500000;
    m.U64(button + kHitBox, box);
    float matrix[12] = {2, 0, 0, 0, 3, 0, 0, 0, 1, -1000, 500, 0};   // (by column: scales 2 and 3, moved to -1000, 500)
    const float rect[4] = {-100, -20, 200, 40};
    m.Put(box + kBoxMatrix, matrix, sizeof matrix);
    m.Put(box + kBoxRect, rect, sizeof rect);
    const auto b = HitBox(m.reader(), button);
    REQUIRE(b.has_value());
    CHECK(b->x0 == doctest::Approx(-1200));
    CHECK(b->x1 == doctest::Approx(-800));
    CHECK(b->y0 == doctest::Approx(440));
    CHECK(b->y1 == doctest::Approx(560));
    for (float& f : matrix) f = 0;   // (its page not shown: the game's matrix is all 0)
    m.Put(box + kBoxMatrix, matrix, sizeof matrix);
    CHECK_FALSE(HitBox(m.reader(), button).has_value());
    CHECK_FALSE(HitBox(m.reader(), 0).has_value());
    CHECK_FALSE(HitBox(m.reader(), 0x310000).has_value());   // (nothing there)
}
