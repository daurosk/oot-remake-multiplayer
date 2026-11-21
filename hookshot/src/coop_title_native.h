// SPDX-License-Identifier: GPL-3.0-or-later
// The title's Host, Join and Co-op Settings on the title's own buttons (docs/hookshot.md "The title menu"): co-op's
// menu (coop_pause_menu.h, the overlay's model) opened on that page without its overlay (CoopPause::OpenNative), its
// rows shown three at a time on the title's first three buttons and Back on the fourth, in the pack's live texts
// (tools/puppet_pack/title_menu.py, Coop_Live_T0..T3). The pad's up and down move the menu's selection (the rows
// scroll with it), left and right change a value, A does the row the title's cursor is on, B goes back. A field the
// menu types into (a name, a port, a code) turns the four buttons into its keys (TypingLook): the game has no keyboard
// of its own (docs/game-ui.md, "Frames for a text field"), so nothing is drawn over the title.
//
// Logic only (tests/hookshot/title_native_test.cpp): what each button says and which row it is.
#pragma once

#include <array>
#include <string>

#include "coop_pause_menu.h"

namespace CoopTitleNative {

inline constexpr int kButtons = 4, kRowButtons = 3;   // the game's own four (the fourth: Back)
// A pack with the list's buttons (tools/puppet_pack/title_menu.py: Pa_Button_04..13, stored hidden) has fourteen: a
// page shows as many rows as the module allows (CoopTitle), never scrolled: pages of them, "Page 2 of 3" in the hint.
inline constexpr int kMaxButtons = 14;
inline constexpr std::size_t kTextBytes = CoopPauseMenu::kTitleRowBytes;   // (a button's width; its placeholder holds 63)
inline constexpr std::size_t kHintBytes = 159;         // (the hint line's placeholder, Coop_Live_Hint, holds 159)

// A row's words on a title button: "Your name: Link", "Players when you host: < 4 >", "Voice: On", a note's line.
std::string RowText(const CoopPauseMenu::Row& r);

struct Look {
    std::array<std::string, kMaxButtons> texts{};
    std::array<int, kMaxButtons> rows = [] {           // the menu's row each button shows (-1: none, or Back)
        std::array<int, kMaxButtons> r{};
        r.fill(-1);
        return r;
    }();
    int focus = 0;                                     // the button the menu's selected row is on
    int top = 0;                                       // the first row shown, among the rows shown (a page's first)
    int back = kButtons - 1;                           // Back's button: right under the last row
    int buttons = kButtons;                            // the title's buttons shown (the rows and Back); the rest hidden
    int page = 0, pages = 1;                           // the page shown, of how many
    int per_page = kRowButtons;                        // rows a page (the buttons less Back)
    std::string hint;                                  // the line under the rows: the page, or what the row does
    std::array<bool, kMaxButtons> dim{};               // a row that cannot be used now (a full game): shown dimmed
};
// The buttons for the menu's view: `buttons` of the title's (kButtons, or up to kMaxButtons with the pack's list),
// the rows a page at a time (`buttons` - 1 of them) with Back right under the page's last, on the page that holds the
// selected row (`top`: the last look's, kept while nothing is selected). Headings are left out; notes show (the menu
// never selects them). The hint: "Page 2 of 3 (ZL/ZR)" with more than one page, else the selected row's help.
Look LookFor(const CoopPauseMenu::View& v, int top, int buttons = kButtons);
// The most buttons a page uses by default (the rows and Back): a rejoin row, eight games, Enter a code or address and
// Back. With the hint line under them the column still fits the title's screen once raised (ColumnRaise).
inline constexpr int kListButtons = 11;
// How far (in the alignment pane's own units) to raise the title's column of buttons so the lowest shown (Back, or the
// hint line under it) ends above the screen's foot: `lowest` its box's lower edge on the screen (the layout's units:
// centre 0, y up, the foot at -1080), `raised` the raise now, `scale` screen units per pane unit. Never below 0 (the
// game's own place) nor so high that `highest` (the first button's upper edge) leaves the screen's top.
float ColumnRaise(float lowest, float highest, float raised, float scale);
inline constexpr float kScreenFoot = -1030.0f, kScreenTop = 1030.0f;   // (a margin inside the 3840x2160 layout's +-1080)
// ZL (-1) or ZR (+1) on a look: the view's row to select, the first of the page before or after; -1 when there is
// none (one page, or at an end).
int PageJump(const CoopPauseMenu::View& v, const Look& l, int dir);

// While a field is typed into: the first button shows the field ("Your name: Link_"), the others are its keys: Letter
// (its letter between arrows; left and right pick it, A puts it in), Delete (the letter before the cursor) and OK. Up
// and down move between the keys, B is Cancel (the field as it was), + is OK; the PC keyboard types as before.
enum class Key { kField, kLetter, kDelete, kOk };
inline constexpr int kFirstKey = static_cast<int>(Key::kLetter), kLastKey = static_cast<int>(Key::kOk);
// The letters Letter picks from, in order (the menu's field takes only its own: a port its digits, a name no control
// character): small letters, capitals, digits, the marks addresses and passwords use, then a space.
const std::string& Letters();
// The key `key` moved by `dy` (-1 up, +1 down), round the three keys.
int MoveKey(int key, int dy);
// The buttons while the menu's selected row is typed into, `key` focused.
Look TypingLook(const CoopPauseMenu::View& v, char letter, int key);

}  // namespace CoopTitleNative
