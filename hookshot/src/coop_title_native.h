// SPDX-License-Identifier: GPL-3.0-or-later
// The title's Host, Join and Co-op Settings on the title's own buttons (docs/hookshot.md "The title menu"): co-op's
// menu (coop_pause_menu.h, the overlay's model) opened on that page without its overlay (CoopPause::OpenNative), its
// rows shown three at a time on the title's first three buttons and Back on the fourth, in the pack's live texts
// (tools/puppet_pack/title_menu.py, Coop_Live_T0..T3). The pad's up and down move the menu's selection (the rows
// scroll with it), left and right change a value, A does the row the title's cursor is on, B goes back. A field the
// menu types into (a name, a port, a code) comes up on the on-screen keyboard (coop_keyboard.h).
//
// Logic only (tests/hookshot/title_native_test.cpp): what each button says and which row it is.
#pragma once

#include <array>
#include <string>

#include "coop_pause_menu.h"

namespace CoopTitleNative {

inline constexpr int kButtons = 4, kRowButtons = 3;   // (the fourth: Back)
inline constexpr std::size_t kTextBytes = 40;          // (a title button's width; the placeholder holds 63)

// A row's words on a title button: "Your name: Link", "Players when you host: < 4 >", "Voice: On", a note's line.
std::string RowText(const CoopPauseMenu::Row& r);

struct Look {
    std::array<std::string, kButtons> texts{};
    std::array<int, kButtons> rows{-1, -1, -1, -1};   // the menu's row each button shows (-1: none, or Back)
    int focus = 0;                                     // the button the menu's selected row is on
    int top = 0;                                       // the first row shown, among the rows shown (kept)
    int back = kButtons - 1;                           // Back's button: right under the last row
    int buttons = kButtons;                            // the title's buttons shown (the rows and Back)
};
// The buttons for the menu's view, the rows scrolled so its selected row shows (`top`: the last look's). Headings are
// left out; notes show (the menu never selects them).
Look LookFor(const CoopPauseMenu::View& v, int top);

}  // namespace CoopTitleNative
