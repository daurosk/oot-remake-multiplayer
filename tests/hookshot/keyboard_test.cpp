// SPDX-License-Identifier: GPL-3.0-or-later
// The on-screen keyboard's grid and cursor (hookshot/src/coop_keyboard.h), and the page typing with it.
#include <doctest/doctest.h>

#include <cstring>

#include "coop_keyboard.h"

using namespace CoopKeyboard;

TEST_CASE("keyboard: five rows of characters, then Space, Delete, Cancel and OK") {
    CHECK(At(0, 0).key == Key::kChar);
    CHECK(At(0, 0).ch == 'A');
    CHECK(At(1, 12).ch == 'Z');
    CHECK(At(2, 0).ch == 'a');
    CHECK(At(4, 0).ch == '0');
    CHECK(At(4, 10).ch == '-');
    CHECK(At(4, 11).ch == '.');
    CHECK(At(4, 12).ch == ':');   // (an address's port)
    for (int r = 0; r < kCharRows; ++r) CHECK(std::strlen(CharRow(r)) == static_cast<std::size_t>(kCols));
    CHECK(At(kCharRows, 0).key == Key::kSpace);
    CHECK(At(kCharRows, 1).key == Key::kDelete);
    CHECK(At(kCharRows, 2).key == Key::kCancel);
    CHECK(At(kCharRows, 3).key == Key::kOk);
    CHECK(At(kCharRows, 4).key == Key::kNone);
    CHECK(At(-1, 0).key == Key::kNone);
    CHECK(At(0, kCols).key == Key::kNone);
    CHECK(std::string(BottomWord(3)) == "OK");
}

TEST_CASE("keyboard: the cursor wraps in its row and column, and goes onto and off the bottom keys by place") {
    Cursor c;
    c.Move(0, -1);   // (left from the first: the row's last)
    CHECK(c.col() == kCols - 1);
    c.Move(0, 1);
    CHECK(c.col() == 0);
    c.Move(-1, 0);   // (up from the top: the bottom keys, the one under the first column: Space)
    CHECK(c.row() == kCharRows);
    CHECK(c.col() == 0);
    c.Move(0, -1);   // (four keys wrap)
    CHECK(c.col() == kBottomKeys - 1);
    c.Move(1, 0);    // (down from the bottom: the top row, the middle of OK's place)
    CHECK(c.row() == 0);
    CHECK(c.col() >= 10);
    c.Set(4, 12);    // (the last column, down: OK)
    c.Move(1, 0);
    CHECK(c.row() == kCharRows);
    CHECK(c.col() == kBottomKeys - 1);
    c.Set(99, 99);
    CHECK(c.row() == kCharRows);
    CHECK(c.col() == kBottomKeys - 1);
}

TEST_CASE("keyboard: nothing to draw and no key under the mouse while closed") {
    Publish(Shown{});
    CHECK_FALSE(Wanted());
    int r = -1, c = -1;
    CHECK_FALSE(KeyAtPoint(0.5f, 0.5f, &r, &c));
    Shown s;
    s.open = true;
    s.title = "Room name";
    Publish(s);
    CHECK(Wanted());
    CHECK_FALSE(KeyAtPoint(0.5f, 0.5f, &r, &c));   // (none drawn yet: no layout)
    Publish(Shown{});
}
