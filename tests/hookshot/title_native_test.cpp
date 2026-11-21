// SPDX-License-Identifier: GPL-3.0-or-later
// The title's own buttons showing co-op's menu pages (hookshot/src/coop_title_native.h).
#include <doctest/doctest.h>

#include "coop_title_native.h"

using namespace CoopTitleNative;
using CoopPauseMenu::Row;
using CoopPauseMenu::RowKind;
using CoopPauseMenu::View;

namespace {
Row R(std::string name, std::string value = "", RowKind kind = RowKind::kAction) {
    Row r;
    r.name = std::move(name), r.value = std::move(value), r.kind = kind;
    return r;
}
}  // namespace

TEST_CASE("title native: a row's words on a title button") {
    CHECK(RowText(R("Your name", "Link")) == "Your name: Link");
    CHECK(RowText(R("Host now")) == "Host now");
    Row t = R("Proximity voice chat", "", RowKind::kToggle);
    t.on = true;
    CHECK(RowText(t) == "Proximity voice chat: On");
    Row c = R("Players when you host", "4", RowKind::kChoice);
    c.left = c.right = true;
    CHECK(RowText(c) == "Players when you host: < 4 >");
    c.right = false;
    CHECK(RowText(c) == "Players when you host: < 4");
    CHECK(RowText(R("", "Hosting on port 27510", RowKind::kNote)) == "Hosting on port 27510");
    CHECK(RowText(R(std::string(60, 'x'))).size() <= kTextBytes);
}

TEST_CASE("title native: a field typed into turns the four buttons into its keys") {
    View v;
    v.rows = {R("Settings", "", RowKind::kHeading), R("Your name", "Link_"), R("Port when you host", "27510")};
    v.selected = 1;
    v.typing = true;
    const Look l = TypingLook(v, 'q', static_cast<int>(Key::kLetter));
    CHECK(l.texts[0] == "Your name: Link_");
    CHECK(l.texts[1] == "Letter: < q >");
    CHECK(l.texts[2] == "Delete");
    CHECK(l.texts[3] == "OK");
    CHECK(l.focus == 1);
    for (const int row : l.rows) CHECK(row == -1);   // (no menu row: the keys are the title's)
    CHECK(TypingLook(v, ' ', static_cast<int>(Key::kOk)).texts[1] == "Letter: < Space >");
    CHECK(TypingLook(v, 'a', static_cast<int>(Key::kOk)).focus == 3);
    CHECK(TypingLook(v, 'a', static_cast<int>(Key::kField)).focus == 1);   // (the field is no key: Letter)
    // A long field shows its end, where the typing goes.
    v.rows[1].value = std::string(50, 'x') + "yz_";
    const std::string shown = TypingLook(v, 'a', 1).texts[0];
    CHECK(shown.size() <= kTextBytes);
    CHECK(shown.rfind("...", 0) == 0);
    CHECK(shown.substr(shown.size() - 3) == "yz_");
    // Up and down: round the three keys.
    CHECK(MoveKey(static_cast<int>(Key::kLetter), -1) == static_cast<int>(Key::kOk));
    CHECK(MoveKey(static_cast<int>(Key::kOk), 1) == static_cast<int>(Key::kLetter));
    CHECK(MoveKey(static_cast<int>(Key::kLetter), 1) == static_cast<int>(Key::kDelete));
    CHECK(MoveKey(static_cast<int>(Key::kField), 1) == static_cast<int>(Key::kDelete));
    CHECK(Letters().front() == 'a');
    CHECK(Letters().find('0') != std::string::npos);
    CHECK(Letters().find('.') != std::string::npos);   // (an address)
    CHECK(Letters().back() == ' ');
}

TEST_CASE("title native: three rows a page and Back, the page that holds the selection, headings left out") {
    View v;
    v.rows = {R("Settings", "", RowKind::kHeading), R("Your name", "Link"), R("Link's look", "Green", RowKind::kChoice),
              R("Players when you host", "4", RowKind::kChoice), R("Port when you host", "27510"),
              R("Chat key", "T", RowKind::kChoice)};
    v.selected = 1;
    Look l = LookFor(v, 0);
    CHECK(l.texts[0] == "Your name: Link");
    CHECK(l.rows[0] == 1);
    CHECK(l.rows[2] == 3);
    CHECK(l.texts[3] == "Back");
    CHECK(l.rows[3] == -1);
    CHECK(l.focus == 0);
    CHECK(l.top == 0);
    CHECK(l.pages == 2);
    CHECK(l.hint == "Page 1 of 2 (ZL/ZR)");
    v.selected = 4;   // (Port: the fourth shown row, the next page; never a scroll)
    l = LookFor(v, l.top);
    CHECK(l.top == 3);
    CHECK(l.page == 1);
    CHECK(l.rows[0] == 4);
    CHECK(l.rows[1] == 5);
    CHECK(l.focus == 0);
    CHECK(l.hint == "Page 2 of 2 (ZL/ZR)");
    CHECK(l.texts[2] == "Back");   // (two rows on the last page: Back right under them)
    CHECK(l.back == 2);
    v.selected = 5;
    l = LookFor(v, l.top);
    CHECK(l.top == 3);
    CHECK(l.focus == 1);
    CHECK(PageJump(v, l, -1) == 1);    // (ZL: the first row of the page before)
    CHECK(PageJump(v, l, +1) == -1);   // (no page after)
    v.selected = 1;   // (back up: the page before)
    l = LookFor(v, l.top);
    CHECK(l.top == 0);
    CHECK(l.focus == 0);
    CHECK(PageJump(v, l, +1) == 4);
    CHECK(l.back == kButtons - 1);   // (three rows or more: Back on the fourth)
    CHECK(l.buttons == kButtons);
    v.selected = -1;   // (nothing selected: the page shown before stays)
    CHECK(LookFor(v, 3).page == 1);
    // Fewer rows than buttons: Back right under the last row, the buttons after it not shown.
    View s;
    s.rows = {R("Host now")};
    s.selected = 0;
    l = LookFor(s, 5);
    CHECK(l.top == 0);
    CHECK(l.texts[1] == " ");   // (one row: three buttons still, the game's own word came on a second of two)
    CHECK(l.texts[2] == "Back");
    CHECK(l.back == 2);
    CHECK(l.buttons == 3);
    s.rows = {R("Copy your answer code"), R("Leave the game")};
    l = LookFor(s, 0);
    CHECK(l.texts[2] == "Back");
    CHECK(l.buttons == 3);
    // Nothing selectable: the cursor on Back.
    View none;
    none.rows = {R("", "Looking...", RowKind::kNote)};
    none.selected = -1;
    CHECK(LookFor(none, 0).focus == LookFor(none, 0).back);
    CHECK(LookFor(none, 0).back == 2);
}

TEST_CASE("title native: with the pack's list, every row on one page, Back right under the last") {
    View v;
    for (int i = 0; i < 9; ++i) v.rows.push_back(R("Game " + std::to_string(i), "2/4"));
    v.rows.push_back(R("Enter a code or address"));
    v.selected = 8;
    v.help = "Joins this game.";
    Look l = LookFor(v, 0, kMaxButtons);
    CHECK(l.pages == 1);
    CHECK(l.texts[9] == "Enter a code or address");
    CHECK(l.texts[10] == "Back");
    CHECK(l.back == 10);
    CHECK(l.buttons == 11);   // (buttons 11..13 hidden)
    CHECK(l.focus == 8);
    CHECK(l.hint == "Joins this game.");
    CHECK(PageJump(v, l, 1) == -1);
    for (int i = 0; i < 20; ++i) v.rows.push_back(R("More " + std::to_string(i)));
    v.selected = 20;   // (13 rows a page: the second page)
    l = LookFor(v, 0, kMaxButtons);
    CHECK(l.pages == 3);
    CHECK(l.page == 1);
    CHECK(l.rows[0] == 13);
    CHECK(l.focus == 7);
    CHECK(l.texts[13] == "Back");
    CHECK(l.hint == "Page 2 of 3 (ZL/ZR)   Joins this game.");   // (the page first, then what the row does)
    CHECK(PageJump(v, l, 1) == 26);
    CHECK(PageJump(v, l, -1) == 0);
    CHECK(LookFor(v, 0, 99).buttons == kMaxButtons);   // (never more than the pack has)
}

TEST_CASE("title native: the column is raised until its lowest box ends on the screen, never below its own place") {
    // (the game's four: their foot above the screen's: no raise)
    CHECK(ColumnRaise(-920.0f, -470.0f, 0.0f, 1.0f) == 0.0f);
    // (eleven buttons and the hint: 600 units under the foot, 1.1 screen units a pane unit: raised 600 / 1.1)
    const float r = ColumnRaise(kScreenFoot - 600.0f, -470.0f, 0.0f, 1.1f);
    CHECK(r == doctest::Approx(600.0f / 1.1f));
    // (drawn with that raise, the foot is on the screen's: it stays)
    CHECK(ColumnRaise(kScreenFoot, -470.0f + 600.0f, r, 1.1f) == doctest::Approx(r));
    // (back to fewer rows: the foot well above, the raise goes down to the game's place, never under it)
    CHECK(ColumnRaise(-300.0f, 900.0f, r, 1.1f) == 0.0f);
    // (a column taller than the screen: never raised past its first row's top)
    const float tall = ColumnRaise(kScreenFoot - 900.0f, 500.0f, 0.0f, 1.0f);
    CHECK(500.0f + tall <= kScreenTop + 0.01f);
}
