// SPDX-License-Identifier: GPL-3.0-or-later
// The on-screen keyboard (the pause menu's Multiplayer page): the game has no text entry of its own (docs/game-ui.md),
// so a field typed into on the page (a room name, a join password, the host's new join password) brings up a keyboard
// drawn over the menu in the look of the Save Profiles file select's name keyboard: five rows of characters, then
// Space, Delete, Cancel and OK. The pad's arrows move over it and A presses the key under the cursor; the mouse clicks
// a key; the PC keyboard types into the field as before.
//
// The grid and its cursor are logic only (tests/hookshot/keyboard_test.cpp). What it shows is published by the page
// (Publish, under the page's lock) and drawn on Hookshot's render thread (Draw); the last drawn layout answers where
// a click falls (KeyAtPoint).
#pragma once

#include <cstdint>
#include <string>

#include "hos_coop_draw.h"

namespace CoopKeyboard {

inline constexpr int kCharRows = 5, kCols = 13, kBottomKeys = 4;
inline constexpr int kRows = kCharRows + 1;   // the last: Space, Delete, Cancel, OK

enum class Key { kNone, kChar, kSpace, kDelete, kCancel, kOk };
struct KeyAt {
    Key key = Key::kNone;
    char ch = 0;   // kChar: the character
};
// The key at a place of the grid (row kCharRows: column 0..3 Space, Delete, Cancel, OK).
KeyAt At(int row, int col);
// A character row's characters.
const char* CharRow(int row);
// A bottom key's word.
const char* BottomWord(int col);

// The grid's cursor. Left and right wrap within a row, up and down within the column; onto the bottom row a column
// goes to the key under it (the four keys share the row's width), and back up to the middle of that key.
class Cursor {
public:
    void Reset() { row_ = 0, col_ = 0; }
    void Move(int drow, int dcol);
    void Set(int row, int col);
    int row() const { return row_; }
    int col() const { return col_; }

private:
    int row_ = 0, col_ = 0;
};

// What the keyboard shows now.
struct Shown {
    bool open = false;
    std::string title;   // what is typed: "Join password", "Room name", "New join password"
    std::string text;    // the field as it shows it (a password: stars but the letter at the cursor)
    int row = 0, col = 0;
};
void Publish(const Shown& s);
bool Wanted();
// Over the picture (hos_coop_draw2's table; font: the game look's font id, 0 Hookshot's own).
void Draw(const HosCoopDraw& d, int font);
// The key under a point of the window as the last drawn keyboard has it (fx, fy: 0 to 1 across the window's client
// area). False when none (or none drawn yet).
bool KeyAtPoint(float fx, float fy, int* row, int* col);

}  // namespace CoopKeyboard
