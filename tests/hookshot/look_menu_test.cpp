// SPDX-License-Identifier: GPL-3.0-or-later
// The Link customizer's page (hookshot/src/coop_look_menu.h).
#include <doctest/doctest.h>

#include <algorithm>
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
    CHECK(parts == static_cast<int>(CoopLook::kParts));
    CHECK(headings == 9);    // Tunic, 3 shields, 3 swords, Boots, Gauntlets
    CHECK(v.rows[static_cast<std::size_t>(v.selected)].kind == 1);   // a part, not a heading
    CHECK(v.rows[static_cast<std::size_t>(RowOf(page, "Hylian Shield", "Frame"))].value == "Game's own");
    CHECK(v.back == "Back");
    CHECK_FALSE(v.picture.empty());
}

TEST_CASE("look menu: Left and Right try the presets; the change is handed on once") {
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

TEST_CASE("look menu: Confirm mixes a colour of one's own, Back goes back to the list") {
    Look start;
    start[Part::master_hilt] = 0x102030u;
    Page page;
    page.Open(start);
    const int hilt = RowOf(page, "Master Sword", "Hilt");
    page.Select(static_cast<std::size_t>(hilt));
    page.Press(Input::kDecide);
    CHECK(page.editing());
    auto v = page.view();
    CHECK(v.rows[0].kind == 3);
    CHECK(v.rows[0].name == "Master Sword: Hilt");
    CHECK(v.rows[static_cast<std::size_t>(v.selected)].name == "Red");
    CHECK(v.rows[1].value == "16");
    page.Press(Input::kRight);                     // red + 8
    CHECK(page.look()[Part::master_hilt] == 0x182030u);
    page.Press(Input::kDown);
    for (int i = 0; i < 40; ++i) page.Press(Input::kRight);   // green up to the top
    CHECK(page.look()[Part::master_hilt] == 0x18FF30u);
    CHECK_FALSE(page.view().rows[2].right);        // no more to give
    page.Press(Input::kDown);
    for (int i = 0; i < 40; ++i) page.Press(Input::kLeft);    // blue down to nothing
    CHECK(page.look()[Part::master_hilt] == 0x18FF00u);
    CHECK(page.TakeChange());
    // the picture shows the colour mixed
    CHECK(PictureHas(page.view(), 0x18FF00u));
    CHECK(page.Press(Input::kBack));               // to the list, on the same row
    CHECK_FALSE(page.editing());
    CHECK(page.view().selected == hilt);
    CHECK_FALSE(page.Press(Input::kBack));         // the list: the tab's own Back
}

TEST_CASE("look menu: a part with no colour mixes from the game's own; Game's own colour and Done") {
    Page page;
    page.Open(Look{});
    page.Select(static_cast<std::size_t>(RowOf(page, "Boots", "Leather")));
    page.Press(Input::kDecide);
    page.Press(Input::kLeft);   // red - 8 from the boots' own brown
    REQUIRE(page.look()[Part::boots]);
    CHECK(*page.look()[Part::boots] == ((CoopLookMenu::Shown(Look{}, Part::boots) - 0x080000u)));
    auto v = page.view();
    int own = -1, done = -1;
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        if (v.rows[i].name == "Game's own colour") own = static_cast<int>(i);
        if (v.rows[i].name == "Done") done = static_cast<int>(i);
    }
    REQUIRE(own > 0);
    page.Click(static_cast<std::size_t>(own), 0);
    CHECK_FALSE(page.look()[Part::boots]);
    page.Click(static_cast<std::size_t>(done), 0);
    CHECK_FALSE(page.editing());
}

TEST_CASE("look menu: everything back to the game's own; the picture follows the item looked at") {
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
    CHECK(v.rows[all].name == "All parts back to the game's own");
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
