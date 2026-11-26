// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge's `menu` state member (hookshot/src/coop_menu_state.h).
#include <doctest/doctest.h>

#include <string>

#include "coop_look_menu.h"
#include "coop_menu_state.h"

using namespace CoopMenuState;
using CoopSnailCustom::Driver;

namespace {

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// As the module opens his screen (a character's rows: its model's own parts, CoopCharColours::ForModel).
Screen OpenFor(Driver& d, std::uint16_t character) {
    d.Open(CoopLook::Look{}, character);
    return FromDriver(d, true, true);
}

}  // namespace

TEST_CASE("menu state: both closed") {
    CHECK(Json(Tab{}, Screen{}) == "{\"coop\":{\"open\":false},\"customize_screen\":{\"open\":false}}");
}

TEST_CASE("menu state: co-op's tab gives its page, the rows' names and kinds, the focused row, never a value") {
    Tab tab;
    tab.open = true;
    tab.page = "join";
    tab.view.rows = {{"Join", "", CoopPauseMenu::RowKind::kHeading},
                     {"Code", "SECRET-CODE", CoopPauseMenu::RowKind::kChoice},
                     {"Voice", "", CoopPauseMenu::RowKind::kToggle, true},
                     {"Join now", "", CoopPauseMenu::RowKind::kAction, false, false, false, true}};
    tab.view.selected = 1;
    const auto j = Json(tab, Screen{});
    CHECK(Has(j, "\"coop\":{\"open\":true,\"tab\":\"multiplayer\",\"page\":\"join\",\"rows\":["));
    CHECK(Has(j, "{\"name\":\"Join\",\"kind\":\"heading\"}"));
    CHECK(Has(j, "{\"name\":\"Voice\",\"kind\":\"toggle\",\"on\":true}"));
    CHECK(Has(j, "{\"name\":\"Join now\",\"kind\":\"action\",\"fixed\":true}"));
    CHECK(Has(j, "\"focused\":1,\"focused_name\":\"Code\",\"typing\":false}"));
    CHECK_FALSE(Has(j, "SECRET"));   // (a value can be a code, an address or a password)
}

TEST_CASE("menu state: no focused row is null") {
    Tab tab;
    tab.open = true;
    tab.page = "main";
    tab.view.rows = {{"Multiplayer", "", CoopPauseMenu::RowKind::kHeading}};
    const auto j = Json(tab, Screen{});
    CHECK(Has(j, "\"focused\":null,\"focused_name\":null"));
}

TEST_CASE("menu state: a character's Customize screen gives its model's own rows (npc-customize ForModel)") {
    Driver d;
    OpenFor(d, 9);
    d.SetRows(1 | 2 | 4 | 32 | 0x80);   // (hair, eyes, one clothing colour, the equipment)
    const auto s = FromDriver(d, true, true);
    const auto j = Json(Tab{}, s);
    CHECK(Has(j, "\"customize_screen\":{\"open\":true,\"driven\":true,"));
    CHECK(Has(j, "\"page_no\":1,\"pages\":1"));
    CHECK(Has(j, "{\"option\":0,\"label\":\"Hair Colour\","));
    CHECK(Has(j, "{\"option\":1,\"label\":\"Eye Colour\","));
    CHECK(Has(j, "\"focused\":0,\"focused_label\":\"Hair Colour\""));
    CHECK(s.page_no == 1);
}

TEST_CASE("menu state: a character whose model has only the equipment shows that row alone") {
    Driver d;
    OpenFor(d, 27);
    d.SetRows(32 | 0x80);
    const auto j = Json(Tab{}, FromDriver(d, true, true));
    CHECK(Has(j, "{\"option\":0,\"label\":\"Equipment\","));
    CHECK_FALSE(Has(j, "Eye Colour"));
    CHECK_FALSE(Has(j, "\"option\":1,"));   // (his other places are empty: left out)
}

TEST_CASE("menu state: Link's pages count as the title does, and Y turns them") {
    Driver d;
    auto s = OpenFor(d, 0);
    CHECK(s.page_no == 1);
    CHECK(s.pages == 4);
    CHECK(Has(s.texts.title, "(page 1 of 4)"));
    CHECK(Has(Json(Tab{}, s), "{\"option\":0,\"label\":\"Eye Colour\","));
    for (int page = 2; page <= 4; ++page) {
        d.Pad(CoopSnailCustom::kY);
        s = FromDriver(d, true, true);
        CHECK(s.page_no == page);
        CHECK(Has(s.texts.title, "(page " + std::to_string(page) + " of 4)"));
    }
    d.Pad(CoopSnailCustom::kY);   // (past the last: back to the first)
    s = FromDriver(d, true, true);
    CHECK(s.page_no == 1);
    CHECK(Has(Json(Tab{}, s), "\"page_no\":1,\"pages\":4"));
}

TEST_CASE("menu state: the mixer is said, and a screen co-op does not drive gives no rows") {
    Driver d;
    OpenFor(d, 0);
    d.Pad(CoopSnailCustom::kA);
    CHECK(Has(Json(Tab{}, FromDriver(d, true, true)), "\"mixer\":true"));
    CHECK(Json(Tab{}, FromDriver(d, true, false)) ==
          "{\"coop\":{\"open\":false},\"customize_screen\":{\"open\":true,\"driven\":false}}");
    d.Close();
    CHECK(Json(Tab{}, FromDriver(d, false, true)) ==
          "{\"coop\":{\"open\":false},\"customize_screen\":{\"open\":false}}");
}
