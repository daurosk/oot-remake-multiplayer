// SPDX-License-Identifier: GPL-3.0-or-later
// Each player's look of several parts over the network (lichii/hook/coop_look_share.h).
#include <doctest/doctest.h>

#include "coop/net/client.hpp"
#include "coop_look_share.h"

using coop::net::Event;
using coop::net::EventKind;
using coop::protocol::LookPart;
using coop::protocol::PlayerLook;

namespace {
Event Kind(EventKind kind, std::uint8_t player = 0) {
    Event e;
    e.kind = kind;
    e.player = player;
    return e;
}
Event Look(std::uint8_t player, CoopLookShare::Parts parts) {
    Event e;
    e.kind = EventKind::message;
    e.message = PlayerLook{player, std::move(parts)};
    return e;
}
}  // namespace

TEST_CASE("look share: ours goes out once per change, and again for a newcomer") {
    CoopLookShare::Share s;
    CHECK_FALSE(s.TakeSend());                 // nothing chosen: nothing sent
    CHECK_FALSE(s.OnEvent(Kind(EventKind::player_joined, 2)));
    CHECK_FALSE(s.TakeSend());                 // a join with no look chosen sends nothing
    s.Set({{3, 0xE0C030}, {4, 0x20A040}});
    const auto sent = s.TakeSend();
    REQUIRE(sent);
    CHECK(sent->player_number == coop::protocol::kPlayerSelf);
    CHECK(sent->parts == CoopLookShare::Parts{{3, 0xE0C030}, {4, 0x20A040}});
    CHECK_FALSE(s.TakeSend());                 // once
    s.OnEvent(Kind(EventKind::player_joined, 3));
    CHECK(s.TakeSend());                       // the late joiner gets it
    s.OnEvent(Kind(EventKind::reconnected));
    CHECK(s.TakeSend());                       // and a server that lost us
    s.Set({});
    const auto cleared = s.TakeSend();         // everything back to the game's own: an empty look goes out
    REQUIRE(cleared);
    CHECK(cleared->parts.empty());
}

TEST_CASE("look share: the others' looks are kept by player and forgotten when they leave") {
    CoopLookShare::Share s;
    CHECK_FALSE(s.Of(2));
    CHECK(s.OnEvent(Look(2, {{11, 0xFF0000}})));
    CHECK_FALSE(s.OnEvent(Look(2, {{11, 0xFF0000}})));   // the same again (a resend for a newcomer)
    CHECK(s.Of(2) == CoopLookShare::Parts{{11, 0xFF0000}});
    CHECK(s.OnEvent(Look(2, {})));                        // back to the game's own
    CHECK(s.Of(2) == CoopLookShare::Parts{});
    CHECK_FALSE(s.OnEvent(Look(coop::protocol::kPlayerSelf, {{1, 1}})));   // not relayed: no player
    CHECK_FALSE(s.Of(coop::protocol::kPlayerSelf));
    CHECK(s.OnEvent(Kind(EventKind::player_left, 2)));
    CHECK_FALSE(s.Of(2));
}

TEST_CASE("look share: a new room forgets the last room's looks, a reconnect keeps them") {
    CoopLookShare::Share s;
    CHECK_FALSE(s.OnEvent(Kind(EventKind::welcomed)));   // nothing to forget
    CHECK(s.OnEvent(Look(2, {{11, 0xFF0000}})));
    CHECK_FALSE(s.OnEvent(Kind(EventKind::reconnected)));
    CHECK(s.Of(2));                                     // the same room: player 2 is still that player
    CHECK(s.OnEvent(Kind(EventKind::welcomed)));         // another room: its player 2 is someone else
    CHECK_FALSE(s.Of(2));
    s.Set({{3, 0xE0C030}});
    s.TakeSend();
    s.OnEvent(Kind(EventKind::welcomed));
    CHECK(s.TakeSend());                                // ours goes to the new room
}
