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
#include <vector>

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

// Co-op's tabs of the game's - menu (Multiplayer, Lobby, Mods: coop_pause_tab.h MinusPage), as the module drew them
// last (CoopPauseTab::MinusWords). Row 223: the details list on the right of the Multiplayer page (the main and
// Session views) has two pages, the session's lines and the key items; only the key items' lines go in (the
// session's page can hold a join code or an address).
struct Minus {
    bool open = false;
    std::string tab;            // "multiplayer", "lobby", "mods"
    std::string view;           // the rows' view ("main", "session", "players", ...: CoopPauseTab::ViewWord)
    int details_page = 0;       // 0 the session's lines, 1 the key items
    int details_pages = 0;      // 2 while the game's key items can be read, 1 when not, 0 on a view without the list
    struct Line {
        std::string name, value;   // "Songs", "3 of 12"
    };
    std::vector<Line> key_items;   // the key items page's lines under its heading (only while details_page is 1)
};

// Row 295: the game's Map page (the - menu's first tab) as its screen's last update left it: its content panes found
// and how many of them show (the backdrop BG_00 not counted). `up`: that screen updated within the last 300 ms.
struct MapPage {
    bool up = false;
    int panes = 0;
    int shown = 0;
};

// The `menu` member's value: {"coop":{...},"customize_screen":{...}} (docs/playtest.md); with the - menu's tabs,
// {"coop":...,"customize_screen":...,"minus":{...}}; with the Map page too, ...,"map_page":{...}.
std::string Json(const Tab& tab, const Screen& screen);
std::string Json(const Tab& tab, const Screen& screen, const Minus& minus);
std::string Json(const Tab& tab, const Screen& screen, const Minus& minus, const MapPage& map);
std::string MinusJson(const Minus& minus);
std::string MapPageJson(const MapPage& map);

}  // namespace CoopMenuState
