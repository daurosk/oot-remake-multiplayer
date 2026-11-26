// SPDX-License-Identifier: GPL-3.0-or-later
// What co-op's menus show, as the play-test bridge's `menu` state member (docs/playtest.md "menu"): a tester reads
// the page from the state, not from a picture. A bridge shot leaves out Longshot's overlays, and so co-op's tab of
// the Insert menu (hos_coop_menu.h); Character Select's Customize screen with co-op's options is the game's own UI.
//
// Only co-op's own words go in: the rows' names (never their values on the Insert menu's tab: a join code, an
// address or a password can be a value there) and, on the Customize screen, the colour names co-op wrote.
// Logic only (no game, no lock): coop_pause.cpp and coop_pause_tab_module.cpp take the snapshots;
// tests/hookshot/menu_state_test.cpp drives it.
#pragma once

#include <string>

#include "coop_pause_menu.h"
#include "coop_snail_custom.h"

namespace CoopMenuState {

// Co-op's tab of Longshot's Insert menu (Multiplayer, its Customize page...).
struct Tab {
    bool open = false;
    std::string page;            // CoopPauseMenu::PageWord ("main", "customize", ...); "" when closed
    CoopPauseMenu::View view;    // its rows and the focused one
};

// Character Select's Customize screen while co-op drives it (coop_snail_custom.h).
struct Screen {
    bool open = false;       // the screen shows
    bool driven = false;     // co-op writes its texts (false: co-op waits, or the fallback keeps his rows)
    CoopSnailCustom::Texts texts;
    int focus = -1;          // the option focused (0..5: the left column top to bottom, then the right)
    bool mixer = false;      // the any-colour mixer is open
    int page_no = 0, pages = 0;
};
// The snapshot of a driver (open and driven as the module knows them).
Screen FromDriver(const CoopSnailCustom::Driver& d, bool open, bool driven);

// The `menu` member's value: {"coop":{...},"customize_screen":{...}} (docs/playtest.md).
std::string Json(const Tab& tab, const Screen& screen);

}  // namespace CoopMenuState
