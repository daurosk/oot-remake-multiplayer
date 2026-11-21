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

TEST_CASE("title native: three rows a time and Back, scrolled to the selection, headings left out") {
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
    v.selected = 4;   // (Port: the fourth shown row, so the rows scroll one)
    l = LookFor(v, l.top);
    CHECK(l.top == 1);
    CHECK(l.rows[2] == 4);
    CHECK(l.focus == 2);
    v.selected = 5;
    l = LookFor(v, l.top);
    CHECK(l.top == 2);
    CHECK(l.focus == 2);
    v.selected = 1;   // (back up: the window follows)
    l = LookFor(v, l.top);
    CHECK(l.top == 0);
    CHECK(l.focus == 0);
    CHECK(l.back == kButtons - 1);   // (three rows or more: Back on the fourth)
    CHECK(l.buttons == kButtons);
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
