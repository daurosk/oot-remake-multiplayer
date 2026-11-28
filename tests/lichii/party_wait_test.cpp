// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>

#include "coop_party_wait.h"

using namespace CoopPartyWait;

namespace {

constexpr std::uint64_t kS = 1'000'000;

Input At(std::uint64_t s, unsigned others, unsigned here, bool in_moment = true, std::string id = "boss:ForestTemple:3") {
    Input in;
    in.on = true;
    in.scene = "ForestTemple";
    if (in_moment) in.moment = Moment{Kind::boss_door, id, WhereText(Kind::boss_door, "Forest Temple")};
    in.others = others;
    in.here = here;
    in.now_us = s * kS;
    return in;
}

}  // namespace

TEST_CASE("party wait: off, alone or not in a game says nothing") {
    Waiter w;
    Input off = At(1, 2, 0);
    off.on = false;
    CHECK(w.Frame(off).notices.empty());
    CHECK(w.Frame(At(2, 0, 0)).notices.empty());   // solo in a session
    Input title = At(3, 2, 0);
    title.scene.clear();
    CHECK(w.Frame(title).notices.empty());
    CHECK_FALSE(w.waiting());
}

TEST_CASE("party wait: a moment with someone missing says the count and tells the others once") {
    Waiter w;
    const Output first = w.Frame(At(1, 2, 1));
    REQUIRE(first.notices.size() == 1);
    CHECK(first.notices[0] == "Waiting for the other players to sync... (2/3 here)");
    REQUIRE(first.chat.has_value());
    CHECK(*first.chat == "Waiting for the party at the boss door (Forest Temple)");
    CHECK(w.waiting());
    const Output again = w.Frame(At(2, 2, 1));
    CHECK(again.notices.empty());
    CHECK_FALSE(again.chat.has_value());
}

TEST_CASE("party wait: everyone arriving ends it") {
    Waiter w;
    w.Frame(At(1, 1, 0));
    const Output o = w.Frame(At(5, 1, 1));
    REQUIRE(o.notices.size() == 1);
    CHECK(o.notices[0] == "Everyone is here.");
    CHECK_FALSE(w.waiting());
    CHECK(w.Frame(At(6, 1, 0)).notices.empty());   // the same moment does not wait again this visit
}

TEST_CASE("party wait: never forever") {
    Waiter w;
    w.Frame(At(1, 1, 0));
    CHECK(w.Frame(At(60, 1, 0)).notices.empty());
    const Output o = w.Frame(At(61, 1, 0));
    REQUIRE(o.notices.size() == 1);
    CHECK(o.notices[0] == "Not everyone is here: going on.");
    CHECK_FALSE(w.waiting());
    CHECK(w.Frame(At(200, 1, 0)).notices.empty());
}

TEST_CASE("party wait: a new count at most every 3 s") {
    Waiter w;
    w.Frame(At(10, 3, 0));
    CHECK(w.Frame(At(11, 3, 1)).notices.empty());   // 1 s after the first line
    const Output o = w.Frame(At(13, 3, 1));
    REQUIRE(o.notices.size() == 1);
    CHECK(o.notices[0] == "Waiting for the other players to sync... (2/4 here)");
    CHECK(w.Frame(At(14, 3, 1)).notices.empty());   // no change, no line
}

TEST_CASE("party wait: leaving the moment ends it quietly, a new visit may wait again") {
    Waiter w;
    w.Frame(At(1, 1, 0));
    CHECK(w.Frame(At(2, 1, 0, false)).notices.empty());
    CHECK_FALSE(w.waiting());
    CHECK(w.Frame(At(3, 1, 0)).notices.empty());   // back in: once a visit
    Input elsewhere = At(4, 1, 0, false);
    elsewhere.scene = "LostWoods";
    w.Frame(elsewhere);
    CHECK(w.Frame(At(5, 1, 0)).notices.size() == 1);   // a new visit of the dungeon
}

TEST_CASE("party wait: everyone already here needs no wait") {
    Waiter w;
    const Output o = w.Frame(At(1, 2, 2));
    CHECK(o.notices.empty());
    CHECK_FALSE(o.chat.has_value());
    CHECK_FALSE(w.waiting());
}

TEST_CASE("party wait: a player leaving the session can complete the party") {
    Waiter w;
    w.Frame(At(1, 2, 1));
    const Output o = w.Frame(At(2, 1, 1));
    REQUIRE(o.notices.size() == 1);
    CHECK(o.notices[0] == "Everyone is here.");
}

TEST_CASE("party wait: /wait in the chat") {
    CHECK_FALSE(Parse("hello").has_value());
    CHECK_FALSE(Parse("/waiting").has_value());
    CHECK(Parse("/wait") == Command::show);
    CHECK(Parse(" /WAIT On ") == Command::on);
    CHECK(Parse("/wait 0") == Command::off);
    CHECK(Parse("/wait maybe") == Command::bad);
}
