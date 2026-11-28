// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's customization in Character Select's Customize screen (hookshot/src/coop_snail_custom.h).
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_colors.h"
#include "coop_i18n.h"
#include "coop_snail_custom.h"

using CoopLook::Look;
using CoopLook::Part;
using namespace CoopSnailCustom;

TEST_CASE("snail customize: his focus rule, two columns of three, no wrap") {
    CHECK(Neighbour(0, Dir::kDown) == 1);
    CHECK(Neighbour(2, Dir::kDown) == -1);
    CHECK(Neighbour(0, Dir::kUp) == -1);
    CHECK(Neighbour(1, Dir::kRight) == 4);
    CHECK(Neighbour(4, Dir::kLeft) == 1);
    CHECK(Neighbour(4, Dir::kRight) == -1);
    CHECK(Neighbour(0, Dir::kLeft) == -1);
    CHECK(Neighbour(-1, Dir::kDown) == -1);
}

TEST_CASE("snail customize: his six options show co-op's parts, ZL / ZR step the focused one, Y turns the page") {
    Driver d;
    CHECK(d.Pad(kZR) == kZR);   // (closed: nothing of co-op's)
    Look start;
    start[Part::zora_tunic] = *CoopColors::Parse("red");
    d.Open(start, 0);
    REQUIRE(d.Active());
    auto t = d.Shown();
    CHECK(t.title == "Link (page 1 of 5)  Y: page  A: Fine-tune");
    CHECK(t.labels[0] == "Eye Colour");
    CHECK(t.labels[1] == "Kokiri Tunic");
    CHECK(t.labels[2] == "Cap Trim");
    CHECK(t.labels[3] == "Cap Trim on Hoods");   // (his right column; C4-4: off, as 0.11.12)
    CHECK(t.values[3] == "Off");
    CHECK(t.labels[4] == "Goron Tunic");
    CHECK(t.labels[5] == "Zora Tunic");
    CHECK(t.values[5] == "Red");   // (capitalised as his own values: the UI gate, review-ui-tabs-0119-4)
    CHECK(t.values[1] == "Default");
    // The d-pad is his (his marker moves); co-op follows: right to Cap Trim on Hoods, down twice to Zora Tunic.
    CHECK(d.Pad(kRight) == kRight);
    CHECK(d.Pad(kDown) == kDown);
    CHECK(d.Pad(kDown) == kDown);
    CHECK(d.Focus() == 5);
    // ZR: Zora Tunic to the next preset; never his (his option Int stays 0).
    CHECK(d.Pad(kZR) == 0);
    const auto change = d.TakeChange();
    REQUIRE(change);
    CHECK((*change)[Part::zora_tunic] != CoopColors::Parse("red"));
    // Y: Link's page of Character Select's options (look-link-cs), then the Shields page, the same six places.
    CHECK(d.Pad(kY) == 0);
    t = d.Shown();
    CHECK(t.title == "Hair and clothes (page 2 of 5)  Y: page  A: Fine-tune");
    CHECK(t.labels[0] == "Hair Colour");
    CHECK(t.labels[1] == "Vest Colour");
    CHECK(t.labels[2] == "Leather and Boots Colour");
    CHECK(t.labels[3] == "Equipment");
    CHECK(d.Pad(kY) == 0);
    t = d.Shown();
    CHECK(t.title == "Shields (page 3 of 5)  Y: page  A: Fine-tune");
    CHECK(t.labels[0] == "Hylian Shield Frame");
    CHECK(t.labels[5] == "Deku Shield");
    // B and -: his (back to his picker).
    CHECK(d.Pad(kB) == kB);
    CHECK(d.Pad(kMinus) == kMinus);
}

TEST_CASE("snail customize: A opens co-op's any-colour mixer in his six places, B and Done leave it") {
    Driver d;
    d.Open(Look{}, 0);
    CHECK(d.Pad(kA) == 0);   // (Kokiri Tunic, the first option)
    REQUIRE(d.Editing());
    auto t = d.Shown();
    CHECK(t.labels[0] == "Hue");
    CHECK(t.labels[1] == "Saturation");
    CHECK(t.labels[2] == "Brightness");
    CHECK(t.labels[3] == "Preset");
    CHECK(t.labels[4] == "Default colour");
    CHECK(t.values[4] == "A");
    CHECK(t.labels[5] == "Done");
    CHECK(t.title.find("Y:") == std::string::npos);   // (no page turn in the mixer)
    d.Pad(kZR);   // (Hue round the wheel)
    REQUIRE(d.TakeChange());
    CHECK(d.Pad(kB) == 0);   // (B: back to the parts, his screen stays)
    CHECK_FALSE(d.Editing());
    d.Pad(kA);
    REQUIRE(d.Editing());
    d.Pad(kRight), d.Pad(kDown), d.Pad(kDown);   // (Done: the right column's last)
    CHECK(d.Focus() == 5);
    d.Pad(kA);
    CHECK_FALSE(d.Editing());
}

TEST_CASE("snail customize: B out of the mixer never reaches his screen, at its press or while held") {
    // (lane run chartab-fix-53d40ab3-2: B out of the mixer also closed his Customize screen)
    Driver d;
    d.Open(Look{}, 0);
    std::uint64_t kept = 0;
    std::uint64_t his = d.Pad(kA);
    REQUIRE(d.Editing());
    CHECK(GameKeys(kA, kA, his, d.Editing(), &kept) == 0);
    CHECK(GameKeys(0, 0, 0, d.Editing(), &kept) == 0);
    his = d.Pad(kB);   // (pressed: the mixer closes at this very read)
    CHECK(his == 0);
    CHECK_FALSE(d.Editing());
    CHECK(GameKeys(kB, kB, his, d.Editing(), &kept) == 0);
    CHECK(GameKeys(kB, 0, 0, d.Editing(), &kept) == 0);   // (still held: still co-op's)
    CHECK(GameKeys(kB | kDown, kDown, kDown, d.Editing(), &kept) == kDown);
    CHECK(GameKeys(0, 0, 0, d.Editing(), &kept) == 0);   // (let go)
    CHECK(kept == 0);
    his = d.Pad(kB);   // (a new B on the parts: his, back to his picker, and held as held)
    CHECK(his == kB);
    CHECK(GameKeys(kB, kB, his, d.Editing(), &kept) == kB);
    CHECK(GameKeys(kB, 0, 0, d.Editing(), &kept) == kB);
    CHECK(GameKeys(kPlus | kA | kY, 0, 0, d.Editing(), &kept) == kPlus);   // (A, Y: co-op's; +: his)
}

TEST_CASE("snail customize: - in the mixer is held back from his screen until let go") {
    Driver d;
    d.Open(Look{}, 0);
    std::uint64_t kept = 0;
    d.Pad(kA);
    REQUIRE(d.Editing());
    std::uint64_t his = d.Pad(kMinus);
    CHECK(GameKeys(kMinus, kMinus, his, d.Editing(), &kept) == 0);
    d.Pad(kB);
    REQUIRE_FALSE(d.Editing());
    CHECK(GameKeys(kMinus | kB, kB, 0, d.Editing(), &kept) == 0);   // (- still held from the mixer, B taken)
    CHECK(GameKeys(0, 0, 0, d.Editing(), &kept) == 0);
    his = d.Pad(kMinus);
    CHECK(GameKeys(kMinus, kMinus, his, d.Editing(), &kept) == kMinus);
}

TEST_CASE("snail customize (npc-customize): a character's screen shows its model's own rows, and they change its parts") {
    Driver d;
    d.Open(Look{}, 9);
    d.SetRows(32 | 0x80);   // (a model with the equipment only)
    auto t = d.Shown();
    CHECK(t.labels[0] == "Equipment");
    CHECK(t.values[0] == "Shown");
    CHECK(t.labels[1].empty());   // (the rest empty: the screen's write gives them a space, PaneText)
    CHECK(PaneText(t.labels[1]) == " ");
    CHECK(t.title.find("Y:") == std::string::npos);   // (one page)
    d.Pad(kZR);
    const auto change = d.TakeChange();
    REQUIRE(change);
    CHECK((*change)[Part::char_gear] == CoopLook::kGearHidden);
    CHECK(d.Shown().values[0] == "Hidden");
    d.SetRows(1 | 2 | 4 | 32 | 0x80);   // (hair, eyes, one clothing colour)
    t = d.Shown();
    CHECK(t.labels[0] == "Hair Colour");
    CHECK(t.labels[1] == "Eye Colour");
    CHECK(t.labels[2] == "Clothing Colour");
    CHECK(t.labels[3] == "Equipment");
    d.Pad(kA);   // (the hair: the mixer)
    CHECK(d.Editing());
}

TEST_CASE("snail customize: a place with nothing is given a space, never an empty text (Saria's screen, lane 035925)") {
    Driver d;
    d.Open(Look{}, 9);
    d.SetRows(32 | 0x80);   // (a character whose model has the equipment only: one row, the rest of six places empty)
    const auto t = d.Shown();
    bool empty = false;
    for (int f = 1; f < kOptions; ++f) empty = empty || t.labels[static_cast<std::size_t>(f)].empty();
    REQUIRE(empty);   // (what the screen's write met: places with no text)
    for (int f = 0; f < kOptions; ++f) {
        CHECK_FALSE(PaneText(t.labels[static_cast<std::size_t>(f)]).empty());
        CHECK_FALSE(PaneText(t.values[static_cast<std::size_t>(f)]).empty());
    }
    CHECK(PaneText("") == " ");
    CHECK(PaneText("Eye Colour") == "Eye Colour");
}

TEST_CASE("snail customize: his option past a page's parts does nothing") {
    Driver d;
    d.Open(Look{}, 0);
    d.Pad(kY), d.Pad(kY), d.Pad(kY), d.Pad(kY);   // (the Boots and gauntlets page, 5th since look-link-cs: two parts)
    REQUIRE(d.Shown().labels[1] == "Gauntlets");
    d.Pad(kDown), d.Pad(kDown);                  // (his third option: past the page's two)
    REQUIRE(d.Focus() == 2);
    d.Pad(kZR);
    d.Pad(kA);
    CHECK_FALSE(d.TakeChange());
    CHECK_FALSE(d.Editing());
}

TEST_CASE("snail customize: his screen's texts in the game's language, as co-op's own pages (zh: no English left)") {
    const CoopI18n::Lang* hans = nullptr;
    for (int i = 0; i < CoopI18n::kLangCount; ++i) {
        if (std::string(CoopI18n::kLangs[i].code) == "zh-Hans") hans = &CoopI18n::kLangs[i];
    }
    REQUIRE(hans != nullptr);
    Driver d;
    Look start;
    start[Part::zora_tunic] = *CoopColors::Parse("red");
    start[Part::goron_tunic] = 0x123456u;   // (a colour of their own: shown as it is)
    d.Open(start, 0);
    const auto t = d.Shown(hans);
    CHECK(t.title.find("Y: page") == std::string::npos);
    CHECK(t.title.find("Y：翻页") != std::string::npos);
    CHECK(t.title.find("A：调色") != std::string::npos);
    CHECK(t.labels[0] == CoopI18n::Tr(hans, "Eye Colour"));
    CHECK(t.labels[0] != "Eye Colour");
    CHECK(t.labels[1] == CoopI18n::Tr(hans, "Kokiri Tunic"));
    CHECK(t.values[5] == CoopI18n::Tr(hans, "red"));
    CHECK(t.values[5] != "red");
    CHECK(t.values[5] != CoopI18n::Tr(hans, "Red"));   // (capitalised after the table: the colour, not a colour's red part)
    CHECK(t.values[1] == CoopI18n::Tr(hans, "Default"));
    CHECK(t.values[3] == CoopI18n::Tr(hans, "Off"));   // (C4-4's switch: its value in the table)
    CHECK(t.values[4] == d.Shown(nullptr).values[4]);   // (#123456 stays)
    // English (no table): the same texts as before.
    CHECK(d.Shown(nullptr).title == "Link (page 1 of 5)  Y: page  A: Fine-tune");
    CHECK(d.Shown(nullptr).values[5] == "Red");
}

TEST_CASE("snail customize: values capitalised as his own (Red next to Default), the rest unchanged") {
    CHECK(Capitalised("red") == "Red");
    CHECK(Capitalised("Default") == "Default");
    CHECK(Capitalised("#123456") == "#123456");
    CHECK(Capitalised("") == "");
    CHECK(Capitalised("çº¢è²") == "çº¢è²");   // (Chinese: unchanged)
}

TEST_CASE("snail customize: the title fits its pane (61 bytes) on every page in every language, the page's name first, both hints unless too long") {
    int lost = 0;
    for (int i = 0; i < CoopI18n::kLangCount; ++i) {
        Driver d;
        d.Open(Look{}, 0);
        for (int page = 0; page < 5; ++page) {   // (look-link-cs: Link's five pages)
            const auto t = d.Shown(&CoopI18n::kLangs[i]);
            INFO(CoopI18n::kLangs[i].code << " page " << page + 1 << ": " << t.title);
            CHECK(t.title.size() <= 61);
            const auto name = CoopI18n::Tr(&CoopI18n::kLangs[i], std::vector<std::string>{"Link", "Hair and clothes", "Shields", "Swords", "Boots and gauntlets"}[static_cast<std::size_t>(page)]);
            CHECK(t.title.compare(0, name.size(), name) == 0);   // (the group's name first)
            const auto y = CoopI18n::Tr(&CoopI18n::kLangs[i], "Y: page"), a = CoopI18n::Tr(&CoopI18n::kLangs[i], "A: Fine-tune");
            if (t.title.find(y) == std::string::npos || t.title.find(a) == std::string::npos) ++lost;
            d.Pad(kY);
        }
    }
    CHECK(lost == 0);   // (no title loses a hint: the hints are short enough for all of them)
}

TEST_CASE("snail customize: Y and A as the game's guide keys (menu-customize-01113), the title then the page's name only") {
    Driver d;
    CHECK(d.KeysNow() == Driver::Keys{});   // (closed: no key)
    d.Open(Look{}, 0);
    // Link: five pages, each with parts: both keys.
    CHECK(d.KeysNow() == Driver::Keys{true, true});
    // Without the key parts in his layout (a kit not merged again) the title keeps the hints in words.
    CHECK(d.Shown().title.find("Y:") != std::string::npos);
    d.SetKeysShown(true);
    auto t = d.Shown();
    CHECK(t.title.find("Y:") == std::string::npos);
    CHECK(t.title.find("A:") == std::string::npos);
    CHECK(t.title.rfind("Link", 0) == 0);
    // The mixer: neither key (A is on its own rows' values, B the footer's Back).
    CHECK(d.Pad(kA) == 0);
    REQUIRE(d.Editing());
    CHECK(d.KeysNow() == Driver::Keys{});
    CHECK(d.Pad(kB) == 0);
    CHECK(d.KeysNow() == Driver::Keys{true, true});
    // A character with one page (the equipment only): no page key, the mix key stays.
    Driver c;
    c.Open(Look{}, 9);
    c.SetRows(32 | 0x80);
    CHECK(c.KeysNow() == Driver::Keys{false, true});
    c.Close();
    CHECK(c.KeysNow() == Driver::Keys{});
}

TEST_CASE("snail customize (chartab-0121): the keys count as shown only with both parts AND their texts set, else the title keeps its hints") {
    // Owner, v0.12.0: "no more indicator to pick the exact hue or change page". The kit's key parts were in his layout,
    // so the title dropped its hints, but the game had not given their panes any text: nothing showed at all.
    CHECK(KeysShownFrom(true, true, true));
    CHECK_FALSE(KeysShownFrom(true, true, false));   // (the parts are there, their texts are not: the 0.12.0 case)
    CHECK_FALSE(KeysShownFrom(true, false, true));
    CHECK_FALSE(KeysShownFrom(false, true, true));
    CHECK_FALSE(KeysShownFrom(false, false, false));
    // The module sets these panes: the kit's parts (Y, then A) and PaGuideKey_00's two text panes.
    CHECK(std::string(kKeyParts[0]) == "Pa_CoopKeyY_00");
    CHECK(std::string(kKeyParts[1]) == "Pa_CoopKeyA_00");
    CHECK(std::string(kKeyPanes[0]) == "T_Icon_00");
    CHECK(std::string(kKeyPanes[1]) == "T_Guide_00");
    // Not shown (texts not set): every page with more than one page and parts says both in words, as 0.11.12 did.
    Driver d;
    d.Open(Look{}, 0);
    d.SetKeysShown(KeysShownFrom(true, true, false));
    CHECK_FALSE(d.KeysShown());
    const auto t = d.Shown();
    CHECK(t.title.find("Y:") != std::string::npos);
    CHECK(t.title.find("A:") != std::string::npos);
    d.SetKeysShown(KeysShownFrom(true, true, true));
    CHECK(d.KeysShown());
    CHECK(d.Shown().title.find("Y:") == std::string::npos);
}
