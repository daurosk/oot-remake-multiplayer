// SPDX-License-Identifier: GPL-3.0-or-later
// The Link customizer's page (hookshot/src/coop_look_menu.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>

#include "coop_colors.h"
#include "coop_look_menu.h"

using CoopLook::Look;
using CoopLook::Part;
using CoopLookMenu::Input;
using CoopLookMenu::Page;

namespace {

// The row of a part on the list.
int RowOf(const Page& page, const std::string& item, const std::string& name) {
    const auto v = page.view();
    std::string at;
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        if (v.rows[i].kind == 3) {
            at = v.rows[i].name;
        } else if (at == item && v.rows[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Outlined(const CoopLookMenu::View& v) {
    return std::any_of(v.picture.begin(), v.picture.end(), [](const CoopLookMenu::Shape& s) { return s.outline; });
}

bool PictureHas(const CoopLookMenu::View& v, std::uint32_t rgb) {
    return std::any_of(v.picture.begin(), v.picture.end(),
                       [&](const CoopLookMenu::Shape& s) { return !s.outline && s.rgba == ((rgb << 8) | 0xFFu); });
}

}  // namespace

TEST_CASE("look menu: every part on the list, by item, under its heading") {
    Page page;
    page.Open(Look{});
    const auto v = page.view();
    int parts = 0, headings = 0;
    for (const auto& r : v.rows) {
        parts += r.kind == 1;
        headings += r.kind == 3;
    }
    // (Link's list: every part but a character's, which are on the Character page)
    const auto link_parts = std::count_if(CoopLook::Parts().begin(), CoopLook::Parts().end(),
                                          [](const CoopLook::PartInfo& i) { return !CoopLook::IsCharacterPart(i.part); });
    CHECK(parts == static_cast<int>(link_parts));
    CHECK(link_parts == static_cast<std::ptrdiff_t>(CoopLook::kParts) - 6);
    CHECK(headings == 12);   // Tunic, 3 shields, 3 swords, Boots, Gauntlets, Eyes, Hair (0.11.9), Link (look-link-cs)
    CHECK(v.rows[static_cast<std::size_t>(v.selected)].kind == 1);   // a part, not a heading
    CHECK(v.rows[static_cast<std::size_t>(RowOf(page, "Hylian Shield", "Frame"))].value == "Default");
    CHECK(v.back == "Back");
    CHECK_FALSE(v.picture.empty());
}

TEST_CASE("look menu: Left and Right try the presets, the change is handed on once") {
    Page page;
    page.Open(Look{});
    const int frame = RowOf(page, "Hylian Shield", "Frame");
    page.Select(static_cast<std::size_t>(frame));
    CHECK_FALSE(page.TakeChange());
    page.Press(Input::kRight);
    auto change = page.TakeChange();
    REQUIRE(change);
    CHECK((*change)[Part::hylian_frame] == CoopColors::Parse("red"));
    CHECK_FALSE(page.TakeChange());
    CHECK(page.view().rows[static_cast<std::size_t>(frame)].value == "red");
    page.Press(Input::kLeft);
    page.Press(Input::kLeft);   // past the game's own: the last preset
    CHECK(page.look()[Part::hylian_frame] == CoopColors::Parse("black"));
    page.Press(Input::kRight);
    CHECK_FALSE(page.look()[Part::hylian_frame]);
    // the picture shows the Hylian Shield with its frame outlined
    CHECK(Outlined(page.view()));
}

TEST_CASE("look menu (0.11.9): Confirm picks any colour by hue, saturation and brightness, Back to the list") {
    Look start;
    start[Part::master_hilt] = 0xFF0000u;   // red: hue 0, full saturation and brightness
    Page page;
    page.Open(start);
    const int hilt = RowOf(page, "Master Sword", "Hilt");
    page.Select(static_cast<std::size_t>(hilt));
    page.Press(Input::kDecide);
    CHECK(page.editing());
    auto v = page.view();
    CHECK(v.rows[0].kind == 3);
    CHECK(v.rows[0].name == "Master Sword: Hilt");
    CHECK(v.rows[static_cast<std::size_t>(v.selected)].name == "Hue");
    CHECK(v.rows[1].value == "0 degrees");
    CHECK(v.rows[2].value == "100%");
    CHECK(v.rows[3].value == "100%");
    page.Press(Input::kLeft);                      // round the wheel backwards: 350 degrees
    CHECK(page.view().rows[1].value == "350 degrees");
    for (int i = 0; i < 13; ++i) page.Press(Input::kRight);   // 350 + 130 = 120: green
    CHECK(page.look()[Part::master_hilt] == 0x00FF00u);
    for (int i = 0; i < 12; ++i) page.Press(Input::kRight);   // 240: blue
    CHECK(page.look()[Part::master_hilt] == 0x0000FFu);
    page.Press(Input::kDown);
    for (int i = 0; i < 30; ++i) page.Press(Input::kLeft);    // saturation to nothing: grey (white at full brightness)
    CHECK(page.look()[Part::master_hilt] == 0xFFFFFFu);
    CHECK_FALSE(page.view().rows[2].left);         // no less to give
    for (int i = 0; i < 10; ++i) page.Press(Input::kRight);   // back to 50%: the hue was kept (a light blue)
    CHECK(page.look()[Part::master_hilt] == 0x8080FFu);
    page.Press(Input::kDown);
    for (int i = 0; i < 10; ++i) page.Press(Input::kLeft);    // brightness 50%
    CHECK(page.look()[Part::master_hilt] == 0x404080u);
    CHECK(page.TakeChange());
    // the picture shows the colour picked
    CHECK(PictureHas(page.view(), 0x404080u));
    CHECK(page.Press(Input::kBack));               // to the list, on the same row
    CHECK_FALSE(page.editing());
    CHECK(page.view().selected == hilt);
    CHECK_FALSE(page.Press(Input::kBack));         // the list: the tab's own Back
}

TEST_CASE("look menu: a part with no colour mixes from the game's own, Default colour and Done") {
    Page page;
    page.Open(Look{});
    page.Select(static_cast<std::size_t>(RowOf(page, "Boots", "Leather")));
    page.Press(Input::kDecide);
    // (the sliders start at the boots' own brown: a hue press keeps its saturation and brightness)
    const CoopLookMenu::Hsv brown = CoopLookMenu::ToHsv(CoopLookMenu::Shown(Look{}, Part::boots));
    page.Press(Input::kLeft);
    REQUIRE(page.look()[Part::boots]);
    CHECK(*page.look()[Part::boots] == CoopLookMenu::ToRgb({(brown.h + 350) % 360, brown.s, brown.v}));
    auto v = page.view();
    int own = -1, done = -1;
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        if (v.rows[i].name == "Default colour") own = static_cast<int>(i);
        if (v.rows[i].name == "Done") done = static_cast<int>(i);
    }
    REQUIRE(own > 0);
    REQUIRE(done > own);
    page.Click(static_cast<std::size_t>(done), 0);   // Done: back to the parts, the colour kept
    CHECK_FALSE(page.editing());
    CHECK(page.look()[Part::boots]);
    page.Select(static_cast<std::size_t>(RowOf(page, "Boots", "Leather")));
    page.Press(Input::kDecide);
    REQUIRE(page.editing());
    page.Click(static_cast<std::size_t>(own), 0);   // Game's own colour: the game's colour, and back to the parts
    CHECK_FALSE(page.look()[Part::boots]);
    CHECK_FALSE(page.editing());
}

TEST_CASE("look menu: everything back to the game's own, the picture follows the item looked at") {
    Look start;
    start[Part::tunic] = 0xFF0000u;
    start[Part::deku_shield] = 0x00FF00u;
    Page page;
    page.Open(start);
    CHECK(PictureHas(page.view(), 0xFF0000u));    // the tunic
    CHECK_FALSE(PictureHas(page.view(), 0x00FF00u));   // the Hylian Shield shows, not the Deku
    page.Select(static_cast<std::size_t>(RowOf(page, "Deku Shield", "Wood")));
    CHECK(PictureHas(page.view(), 0x00FF00u));
    auto v = page.view();
    const auto all = static_cast<std::size_t>(v.rows.size() - 1);
    CHECK(v.rows[all].name == "All parts to the game's own");
    page.Select(all);
    page.Press(Input::kDecide);
    CHECK(page.look() == Look{});
    CHECK(page.TakeChange() == Look{});
    // Up from the first part wraps to the last row, past the heading
    page.Open(Look{});
    page.Select(1);
    page.Press(Input::kUp);
    CHECK(page.view().selected == static_cast<int>(all));
}

TEST_CASE("look menu: the Cap trim, under the Tunic heading after the tunics, follows the tunic until set") {
    Look start;
    start[Part::tunic] = 0x3070FFu;
    Page page;
    page.Open(start);
    const int zora = RowOf(page, "Tunic", "Zora Tunic");
    const int cap = RowOf(page, "Tunic", "Cap trim");
    REQUIRE(cap > 0);
    CHECK(cap == zora + 1);
    auto v = page.view();
    CHECK(v.rows[static_cast<std::size_t>(cap)].value == "Same as tunic");
    CHECK(CoopLookMenu::Shown(start, Part::cap_trim) == 0x3070FFu);   // the picture's band: the tunic's colour
    page.Select(static_cast<std::size_t>(cap));
    v = page.view();
    CHECK(v.help_title == "Tunic: Cap trim");
    CHECK(v.help.find("headband") != std::string::npos);
    CHECK(Outlined(v));   // the band on the Kokiri cap outlined
    // Right: the first preset, the tunic as it was.
    page.Press(Input::kRight);
    const auto change = page.TakeChange();
    REQUIRE(change);
    CHECK((*change)[Part::cap_trim] == CoopColors::Parse("red"));
    CHECK((*change)[Part::tunic] == 0x3070FFu);
    CHECK(page.view().rows[static_cast<std::size_t>(cap)].value == "red");
    CHECK(PictureHas(page.view(), *CoopColors::Parse("red")));
    // Mixing an unset trim starts from the tunic's colour; Default colour sets it back to follow the tunic.
    page.Press(Input::kLeft);   // back to unset
    CHECK_FALSE(page.look()[Part::cap_trim]);
    page.Press(Input::kDecide);
    REQUIRE(page.editing());
    v = page.view();
    CHECK(v.rows[static_cast<std::size_t>(v.selected)].name == "Hue");
    const CoopLookMenu::Hsv tunic = CoopLookMenu::ToHsv(0x3070FFu);   // the sliders start at the tunic's colour
    CHECK(v.rows[1].value == std::to_string(tunic.h) + " degrees");
    CHECK(v.rows[4].value == "Same as tunic");   // the Preset row
    page.Press(Input::kRight);
    CHECK(page.look()[Part::cap_trim] == CoopLookMenu::ToRgb({(tunic.h + 10) % 360, tunic.s, tunic.v}));
    CHECK(page.look()[Part::tunic] == 0x3070FFu);
}

TEST_CASE("look menu (0.11.9): any colour round-trips through hue, saturation and brightness") {
    using CoopLookMenu::Hsv;
    CHECK(CoopLookMenu::ToHsv(0xFF0000u) == Hsv{0, 100, 100});
    CHECK(CoopLookMenu::ToHsv(0x00FF00u) == Hsv{120, 100, 100});
    CHECK(CoopLookMenu::ToHsv(0x0000FFu) == Hsv{240, 100, 100});
    CHECK(CoopLookMenu::ToHsv(0x000000u) == Hsv{0, 0, 0});
    CHECK(CoopLookMenu::ToHsv(0xFFFFFFu) == Hsv{0, 0, 100});
    CHECK(CoopLookMenu::ToRgb({60, 100, 100}) == 0xFFFF00u);
    CHECK(CoopLookMenu::ToRgb({300, 100, 100}) == 0xFF00FFu);
    CHECK(CoopLookMenu::ToRgb({-60, 100, 100}) == 0xFF00FFu);   // (a hue below 0 wraps)
    // Every colour comes back within the steps' rounding (1% is 2.55 of 255).
    for (std::uint32_t c : {0x4C8C3Cu, 0x27357Au, 0x634D38u, 0xBEC3DCu, 0x123456u, 0xFEDCBAu}) {
        const std::uint32_t back = CoopLookMenu::ToRgb(CoopLookMenu::ToHsv(c));
        for (int shift : {16, 8, 0}) {
            const int a = static_cast<int>((c >> shift) & 0xFF), b = static_cast<int>((back >> shift) & 0xFF);
            CHECK(std::abs(a - b) <= 4);
        }
    }
}

TEST_CASE("look menu (0.11.9): the Character page: three pages of six, ZL and ZR turn, nothing below a scroll") {
    Page page;
    page.SetPaged(true);
    Look start;
    start[Part::master_hilt] = *CoopColors::Parse("red");   // (a preset: its name shows)
    page.Open(start);
    auto v = page.view();
    REQUIRE(v.rows.size() == 7);                    // the heading and six parts: one screen (as the Shields page)
    CHECK(v.rows[0].kind == 3);
    CHECK(v.rows[0].name == "Link (page 1 of 5)");
    CHECK(v.rows[1].name == "Eye Colour");
    CHECK(v.rows[2].name == "Kokiri Tunic");
    CHECK(v.rows[3].name == "Cap Trim");
    CHECK(v.rows[4].name == "Cap Trim on Hoods");   // (C4-4: off, as 0.11.12, until asked)
    CHECK(v.rows[4].value == "Off");
    CHECK(v.rows[6].name == "Zora Tunic");
    CHECK(v.selected == 1);
    page.Press(Input::kPageNext);
    v = page.view();
    CHECK(v.rows[0].name == "Hair and clothes (page 2 of 5)");   // (look-link-cs, every Link)
    page.Press(Input::kPageNext);
    v = page.view();
    CHECK(v.rows[0].name == "Shields (page 3 of 5)");
    CHECK(v.rows[1].name == "Hylian Shield Frame");
    CHECK(v.rows[6].name == "Deku Shield");
    page.Press(Input::kPageNext);
    v = page.view();
    CHECK(v.rows[0].name == "Swords (page 4 of 5)");
    CHECK(v.rows[4].name == "Master Sword Hilt");
    CHECK(v.rows[4].value == "red");
    CHECK(v.rows[6].name == "Biggoron's Sword Hilt");
    page.Press(Input::kPageNext);
    v = page.view();
    CHECK(v.rows[0].name == "Boots and gauntlets (page 5 of 5)");
    REQUIRE(v.rows.size() == 3);
    CHECK(v.rows[1].name == "Boots");
    CHECK(v.rows[2].name == "Gauntlets");
    page.Press(Input::kPageNext);                   // round to the first
    CHECK(page.view().rows[0].name == "Link (page 1 of 5)");
    page.Press(Input::kPagePrev);
    page.Press(Input::kPagePrev);
    CHECK(page.GroupShown() == 2u);   // (Swords)
    // Every part is on exactly one page (look-link-cs: the hair on the Link page of Character Select's options).
    std::vector<Part> seen;
    for (const auto& g : CoopLookMenu::Groups()) seen.insert(seen.end(), g.parts.begin(), g.parts.end());
    CHECK(seen.size() == CoopLook::kParts);
    for (const auto& info : CoopLook::Parts()) {
        CHECK(std::count(seen.begin(), seen.end(), info.part) == 1);
    }
    // A on a part: the colour mixer, which a page turn leaves alone; X resets every part.
    page.Press(Input::kDown);
    page.Press(Input::kDown);
    page.Press(Input::kDown);                       // Master Sword Hilt
    page.Press(Input::kDecide);
    REQUIRE(page.editing());
    page.Press(Input::kPageNext);
    CHECK(page.editing());
    page.Press(Input::kBack);
    page.Press(Input::kResetAll);
    CHECK(page.look() == Look{});
}

TEST_CASE("look menu (look-link-cs, report 293): Character Select's options on every Link, with or without the mod") {
    using CoopLookMenu::Input;
    using CoopLookMenu::Page;
    Page page;
    page.SetPaged(true);
    page.Open(Look{});
    page.SetCharacter(0, false);
    CHECK(page.GroupsOffered() == 5u);              // (no model rows asked: C #132, co-op's own paint and flag)
    page.Press(Input::kPageNext);
    auto v = page.view();
    CHECK(v.rows[0].name == "Hair and clothes (page 2 of 5)");
    REQUIRE(v.rows.size() == 5);
    CHECK(v.rows[1].name == "Hair Colour");
    CHECK(v.rows[2].name == "Vest Colour");
    CHECK(v.rows[3].name == "Leather and Boots Colour");
    CHECK(v.rows[4].name == "Equipment");
    CHECK(v.rows[4].value == "Shown");
    CHECK(v.rows[2].value == "Same as tunic");   // (vest-link: unset, the vest is painted in the tunic's colour)
    // The vest and the leather take any colour (the presets, and the mixer on A) as Link's other parts.
    page.Select(2);
    page.Press(Input::kRight);
    CHECK(page.look()[Part::link_vest].has_value());
    page.Press(Input::kDecide);
    CHECK(page.editing());
    page.Press(Input::kBack);
    page.Select(3);
    page.Press(Input::kRight);
    CHECK(page.look()[Part::link_leather].has_value());
    page.Select(4);
    page.Press(Input::kRight);
    CHECK(page.look()[Part::link_gear] == CoopLook::kGearHidden);
    CHECK(page.view().rows[4].value == "Hidden");
    // The hair takes any colour (the mixer).
    page.Select(1);
    page.Press(Input::kDecide);
    CHECK(page.editing());
    page.Press(Input::kBack);
    // Rows told by a model (a Link without the mod's hair palette) change nothing on Link's page.
    page.SetRows(2 | 32 | 0x80);
    CHECK(page.GroupsOffered() == 5u);
    CHECK(page.view().rows.size() == 5);
    // X on Link resets his parts, the mod's among them.
    page.Press(Input::kResetAll);
    CHECK_FALSE(page.look()[Part::link_vest]);
    CHECK_FALSE(page.look()[Part::link_gear]);
}

TEST_CASE("look menu (npc-customize): a character other than Link: its own parts, only the rows its model has") {
    Page page;
    page.SetPaged(true);
    Look link;
    link[Part::eyes] = 0x112233u;
    page.Open(link);
    page.SetCharacter(9, false);
    auto v = page.view();
    REQUIRE(v.rows.size() == 7);                    // (rows not known yet: every row)
    CHECK(v.rows[0].name == "Character");
    CHECK(v.rows[1].name == "Hair Colour");
    CHECK(v.rows[6].name == "Equipment");
    CHECK(v.rows[6].value == "Shown");
    CHECK(page.GroupsOffered() == 1u);
    // Saria's model: hair, eyes, Clothing Colour 1 and 2 (CoopCharColours::Rows: 1 | 2 | 4 | 8 | 32 | known)
    page.SetRows(1 | 2 | 4 | 8 | 32 | 0x80);
    v = page.view();
    REQUIRE(v.rows.size() == 6);
    CHECK(v.rows[3].name == "Clothing Colour");
    CHECK(v.rows[4].name == "Clothing Colour 2");
    CHECK(v.rows[5].name == "Equipment");
    // A cow: the equipment only.
    page.SetRows(32 | 0x80);
    REQUIRE(page.view().rows.size() == 2);
    CHECK(page.view().rows[1].name == "Equipment");
    page.Press(Input::kRight);                      // Hidden
    CHECK(page.look()[Part::char_gear] == CoopLook::kGearHidden);
    CHECK(page.view().rows[1].value == "Hidden");
    page.Press(Input::kDecide);                     // (no mixer: A steps on) Shown again
    CHECK_FALSE(page.editing());
    CHECK_FALSE(page.look()[Part::char_gear]);
    // The clothing rows step through the mod's seven colours, no mixer.
    page.SetRows(1 | 2 | 4 | 32 | 0x80);
    page.Select(3);
    REQUIRE(page.view().rows[3].name == "Clothing Colour");
    page.Press(Input::kRight);
    CHECK(page.look()[Part::char_cloth1] == CoopColors::Parse("green"));
    page.Press(Input::kLeft), page.Press(Input::kLeft);
    CHECK(page.look()[Part::char_cloth1] == CoopColors::Parse("white"));
    page.Press(Input::kDecide);
    CHECK_FALSE(page.editing());
    // The hair takes any colour (the mixer); X resets the character's parts only, Link's eyes stay.
    page.Select(1);
    page.Press(Input::kDecide);
    CHECK(page.editing());
    page.Press(Input::kBack);
    page.Press(Input::kResetAll);
    CHECK_FALSE(page.look()[Part::char_cloth1]);
    CHECK(page.look()[Part::eyes] == 0x112233u);
    page.SetCharacter(9, true);
    v = page.view();
    CHECK(v.rows[0].name == "Character (page 1 of 3)");
    page.Press(Input::kPageNext);
    CHECK(page.view().rows[0].name == "Shields (page 2 of 3)");   // (its held gear, once shown to work)
    page.Press(Input::kPageNext);
    CHECK(page.view().rows[0].name == "Swords (page 3 of 3)");
    page.SetCharacter(0, true);                     // Link again: every page
    CHECK(page.view().rows[0].name == "Link (page 1 of 5)");
}

TEST_CASE("look menu (C4-4): Cap Trim on Hoods is a switch, off as 0.11.12, no mixer") {
    Page page;
    page.SetPaged(true);
    page.Open(Look{});
    page.Select(4);
    auto v = page.view();
    REQUIRE(v.rows[4].name == "Cap Trim on Hoods");
    CHECK(v.rows[4].value == "Off");
    CHECK_FALSE(page.look()[Part::cap_trim_hoods]);
    page.Press(Input::kRight);                      // On
    CHECK(page.look()[Part::cap_trim_hoods] == CoopLook::kOn);
    CHECK(page.view().rows[4].value == "On");
    page.Press(Input::kDecide);                     // (no mixer: A steps on) Off again
    CHECK_FALSE(page.editing());
    CHECK_FALSE(page.look()[Part::cap_trim_hoods]);
    page.Press(Input::kLeft);
    CHECK(page.look()[Part::cap_trim_hoods] == CoopLook::kOn);
    // The list (not paged) has the row under the Tunic heading, its help a switch's.
    Page list;
    list.Open(Look{});
    const int row = RowOf(list, "Tunic", "Cap trim on hoods");
    REQUIRE(row >= 0);
    CHECK(list.view().rows[static_cast<std::size_t>(row)].value == "Off");
}
