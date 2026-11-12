// SPDX-License-Identifier: GPL-3.0-or-later
// The item cheats panel on Hookshot (hookshot/src/coop_cheats_panel.h).
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_cheats_panel.h"

using CoopCheatsPanel::kKeyDown;
using CoopCheatsPanel::kKeyToggle;
using CoopCheatsPanel::kKeyUp;
using CoopCheatsPanel::Panel;

TEST_CASE("cheats panel: the quick gifts are the co-op window's") {
    const auto& q = CoopCheatsPanel::QuickRows();
    REQUIRE(q.size() == 6);
    CHECK(q[0].flag == "PouchData.Item_Token_Gold.StockNum");
    CHECK(q[2].flag == "PlayerStatus.MaxLife");
    CHECK(q[5].flag == "IsGet.Item_Melody_StormSong");
    CHECK(CoopCheatsPanel::Rows("", {"a"}).size() == 6);
}

TEST_CASE("cheats panel: typing finds item flags by any part of the name, any case") {
    const std::vector<std::string> items{"IsGet.Item_Boomerang", "PouchData.Item_Boomerang.StockNum",
                                         "PouchData.Item_Bow.StockNum"};
    const auto rows = CoopCheatsPanel::Rows("boom", items);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].flag == "IsGet.Item_Boomerang");
    CHECK(rows[0].label == rows[0].flag);
    CHECK(CoopCheatsPanel::Rows("zzz", items).empty());
    std::vector<std::string> many(40, "Item_X");
    CHECK(CoopCheatsPanel::Rows("x", many).size() == CoopCheatsPanel::kMaxRows);
}

TEST_CASE("cheats panel: F8 opens it, keys choose and give, Esc closes") {
    Panel p;
    CHECK_FALSE(p.Key('W', false, 6));  // closed: the game's keys pass
    CHECK_FALSE(p.Char('w', "w"));
    CHECK(p.Key(kKeyToggle, false, 6));
    CHECK(p.open());
    CHECK(p.KeysTaken(false));
    CHECK(p.Key(kKeyDown, false, 6));
    CHECK(p.Key(kKeyDown, true, 6));
    CHECK(p.selected() == 2);
    p.Key(kKeyUp, false, 6);
    CHECK(p.selected() == 1);
    for (int i = 0; i < 10; ++i) p.Key(kKeyDown, true, 6);
    CHECK(p.selected() == 5);  // not past the last row
    CHECK(p.Key(0x0D, false, 6));
    const auto give = p.TakeGive();
    REQUIRE(give);
    CHECK(*give == 5);
    CHECK_FALSE(p.TakeGive());
    CHECK(p.open());  // stays open for more gifts
    // Typing filters and goes back to the first row.
    CHECK(p.Char('b', "b"));
    CHECK(p.Char('o', "o"));
    CHECK(p.filter() == "bo");
    CHECK(p.selected() == 0);
    CHECK(p.Char('\r', "\r"));  // Enter's character: dropped
    CHECK(p.filter() == "bo");
    p.Key(0x08, false, 1);
    CHECK(p.filter() == "b");
    // Enter on no rows gives nothing.
    p.Key(0x0D, false, 0);
    CHECK_FALSE(p.TakeGive());
    // Esc closes; the keys stay taken until it is let go.
    CHECK(p.Key(0x1B, false, 1));
    CHECK_FALSE(p.open());
    CHECK(p.KeysTaken(true));
    CHECK_FALSE(p.KeysTaken(false));
    // F8 again opens it empty.
    p.Key(kKeyToggle, false, 6);
    CHECK(p.filter().empty());
    p.Key(kKeyToggle, false, 6);
    CHECK_FALSE(p.open());
}
