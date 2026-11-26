// SPDX-License-Identifier: GPL-3.0-or-later
// CoopMapRefresh (dungeon-sync-b, dungeon-maps-a; rows 63/76, #146): when to ask the game's own HUD refresh after a
// co-op write of a dungeon's map, compass, boss key or floors. The refresh itself runs at the HUD screen's own update
// in Longshot (patch 0018, hos_coop_hud_refresh).
#include <doctest/doctest.h>

#include "coop_map_refresh.h"

using CoopMapRefresh::kDelayMs;

TEST_CASE("map refresh: asked once, a moment after the last write") {
    CoopMapRefresh::Refresher r;
    r.SetInPlay(true);
    CHECK_FALSE(r.Due(100));  // nothing wanted
    r.Want(100);
    CHECK_FALSE(r.Due(100 + kDelayMs - 1));  // the game's setter first
    r.Want(200);                             // a later write moves it
    CHECK_FALSE(r.Due(100 + kDelayMs));
    CHECK(r.Due(200 + kDelayMs));
    CHECK_FALSE(r.Due(5000));  // once
}

TEST_CASE("map refresh: not in play (the title, a load, a reload): not asked, kept until Link is back") {
    CoopMapRefresh::Refresher r;
    r.Want(100);
    CHECK_FALSE(r.Due(1000));  // not in play yet (the start)
    r.SetInPlay(true);
    CHECK(r.Due(1000));
    r.Want(2000);
    r.SetInPlay(false);
    CHECK_FALSE(r.Due(3000));
    r.SetInPlay(true);
    CHECK(r.Due(3000));
}

TEST_CASE("map refresh: our own put-back is asked on the next frame, then once more a moment later (row 140)") {
    CoopMapRefresh::Refresher r;
    r.SetInPlay(false);
    r.WantNow(100);
    CHECK_FALSE(r.Due(100));  // not in play: waits
    r.SetInPlay(true);
    CHECK(r.Due(101));                       // the same frame's setters have run: now
    CHECK_FALSE(r.Due(102));                 // once
    CHECK_FALSE(r.Due(100 + kDelayMs - 1));
    CHECK(r.Due(100 + kDelayMs));            // and after the setter's usual delay
    CHECK_FALSE(r.Due(5000));
}
