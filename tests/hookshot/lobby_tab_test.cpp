// SPDX-License-Identifier: GPL-3.0-or-later
// The Lobby tab (hookshot/src/coop_lobby_tab.*): the list, a player's page,
// what every player may do and what only the host may, and Kick's confirm.
#include <doctest/doctest.h>

#include <string>

#include "coop_lobby_tab.h"

using namespace CoopLobbyTab;
using coop::protocol::LobbyActionKind;
using CoopPauseMenu::RowKind;

namespace {

constexpr std::uint8_t kVoice = coop::protocol::lobby_mute::voice;
constexpr std::uint8_t kChat = coop::protocol::lobby_mute::chat;

Player P(std::uint8_t n, std::string name, bool self = false, bool host = false) {
    Player p;
    p.number = n;
    p.name = std::move(name);
    p.self = self;
    p.host = host;
    p.colour = "blue";
    return p;
}

Lobby Game(bool we_host) {
    Lobby l;
    l.connected = true;
    l.hosting = we_host;
    l.max_players = we_host ? 4 : 0;
    l.invite = we_host ? "INVITE-CODE-1234" : "";
    l.players.push_back(P(1, "Link", we_host, true));
    l.players.push_back(P(2, "Saria", !we_host));
    l.players.push_back(P(3, "Mido"));
    l.host_place = "Kokiri Forest";
    l.host_life = 10;
    l.host_max = 12;
    return l;
}

int RowOf(const View& v, const std::string& name) {
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        if (v.rows[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

bool Has(const View& v, const std::string& name) {
    return RowOf(v, name) >= 0;
}

// Selects the row by name (the mouse) and presses Choose.
void Choose(Tab& t, const std::string& name) {
    const int row = RowOf(t.view(), name);
    REQUIRE(row >= 0);
    t.Select(static_cast<std::size_t>(row));
    t.Press(Input::kDecide);
}

}  // namespace

TEST_CASE("lobby tab: out of a game it says where to host or join") {
    Tab t;
    t.Show();
    const View v = t.view();
    REQUIRE(v.rows.size() == 3);
    CHECK(v.rows[1].kind == RowKind::kNote);
    CHECK(v.rows[1].name == "You are not in a game.");
    CHECK(v.selected == -1);
    CHECK_FALSE(t.Press(Input::kBack));  // the menu closes
}

TEST_CASE("lobby tab: the list shows the players, the host, open places, the invite and the host's game") {
    Tab t;
    t.Show();
    Lobby l = Game(true);
    l.players[1].ms = 42;
    l.players[1].place = "Lost Woods";
    l.players[1].talking = true;
    l.players[2].life = Player::Life::downed;
    l.players[2].host_mutes = kChat;
    t.Set(l);
    const View v = t.view();
    CHECK(v.rows[0].kind == RowKind::kHeading);
    CHECK(v.rows[0].name == "Players  3 / 4");
    CHECK(Has(v, "Host: Link (you)"));
    REQUIRE(Has(v, "Saria"));
    CHECK(v.rows[RowOf(v, "Saria")].value == "42 ms  Lost Woods  talking");
    CHECK(v.rows[RowOf(v, "Mido")].value == "downed  muted");
    CHECK(Has(v, "Open place"));
    REQUIRE(Has(v, "Copy the invite code"));
    CHECK(v.rows[RowOf(v, "Copy the invite code")].value == "INVITE-CODE-1234");
    CHECK(Has(v, "Kokiri Forest  2 and a half of 3 hearts"));
    CHECK(v.selected == 1);  // the first player
    // Copy.
    Choose(t, "Copy the invite code");
    const auto r = t.TakeRequest();
    REQUIRE(r);
    CHECK(r->kind == Request::Kind::kCopyInvite);
    CHECK(t.view().gold == "The invite code is on the clipboard.");
}

TEST_CASE("lobby tab: a guest sees no invite code and no host controls") {
    Tab t;
    t.Show();
    t.Set(Game(false));
    View v = t.view();
    CHECK(v.rows[0].name == "Players  3");  // a guest does not know the most
    CHECK_FALSE(Has(v, "Copy the invite code"));
    CHECK(Has(v, "Only the host has the invite code: ask them for it."));
    Choose(t, "Mido");
    v = t.view();
    CHECK(t.shown_player() == 3);
    CHECK(Has(v, "Mute their voice (only you)"));
    CHECK(Has(v, "Mute their chat (only you)"));
    CHECK(Has(v, "Teleport to them"));
    CHECK_FALSE(Has(v, "Kick"));
    CHECK_FALSE(Has(v, "Ban for this session"));
    CHECK_FALSE(Has(v, "Mute their voice for everyone"));
}

TEST_CASE("lobby tab: every player mutes others for themselves and teleports to them") {
    Tab t;
    t.Show();
    t.Set(Game(false));
    Choose(t, "Mido");
    Choose(t, "Mute their voice (only you)");
    auto r = t.TakeRequest();
    REQUIRE(r);
    CHECK(r->kind == Request::Kind::kOwnMute);
    CHECK(r->player == 3);
    CHECK(r->what == kVoice);
    CHECK(r->on);
    // The game says it is muted now; Left turns it back off, Left again does nothing.
    Lobby l = Game(false);
    l.players[2].own_mutes = kVoice;
    t.Set(l);
    CHECK(t.view().rows[RowOf(t.view(), "Mute their voice (only you)")].on);
    t.Press(Input::kLeft);
    r = t.TakeRequest();
    REQUIRE(r);
    CHECK_FALSE(r->on);
    t.Press(Input::kRight);  // already on (the game has not changed yet): nothing
    CHECK_FALSE(t.TakeRequest());
    // A click on the chat toggle's On half.
    const int chat = RowOf(t.view(), "Mute their chat (only you)");
    t.Click(static_cast<std::size_t>(chat), 1);
    r = t.TakeRequest();
    REQUIRE(r);
    CHECK(r->what == kChat);
    CHECK(r->on);
    Choose(t, "Teleport to them");
    r = t.TakeRequest();
    REQUIRE(r);
    CHECK(r->kind == Request::Kind::kTeleport);
    CHECK(r->name == "Mido");
}

TEST_CASE("lobby tab: our own page has no actions") {
    Tab t;
    t.Show();
    t.Set(Game(false));
    Choose(t, "Saria (you)");
    const View v = t.view();
    CHECK(t.shown_player() == 2);
    CHECK_FALSE(Has(v, "Mute their voice (only you)"));
    CHECK_FALSE(Has(v, "Teleport to them"));
    CHECK(v.selected == -1);
    CHECK(t.Press(Input::kBack));
    CHECK(t.shown_player() == 0);
    CHECK(t.view().rows[t.view().selected].name == "Saria (you)");  // back on that row
}

TEST_CASE("lobby tab: the host mutes for everyone, and Kick and Ban need a second press") {
    Tab t;
    t.Show();
    t.Set(Game(true));
    Choose(t, "Saria");
    View v = t.view();
    CHECK(Has(v, "Host"));
    Choose(t, "Mute their chat for everyone");
    auto r = t.TakeRequest();
    REQUIRE(r);
    CHECK(r->kind == Request::Kind::kServer);
    CHECK(r->action == coop::protocol::LobbyAction{LobbyActionKind::mute_chat, 2});
    Choose(t, "Mute their voice for everyone");
    r = t.TakeRequest();
    REQUIRE(r);
    CHECK(r->action == coop::protocol::LobbyAction{LobbyActionKind::mute_voice, 2});
    // The server says muted: the toggle is on, and choosing it again unmutes.
    Lobby l = Game(true);
    l.players[1].host_mutes = kVoice;
    t.Set(l);
    Choose(t, "Mute their voice for everyone");
    r = t.TakeRequest();
    REQUIRE(r);
    CHECK(r->action == coop::protocol::LobbyAction{LobbyActionKind::unmute_voice, 2});
    // Kick: once asks, twice does.
    Choose(t, "Kick");
    CHECK_FALSE(t.TakeRequest());
    CHECK(t.view().rows[RowOf(t.view(), "Kick")].value == "Press again to kick");
    t.Press(Input::kDecide);
    r = t.TakeRequest();
    REQUIRE(r);
    CHECK(r->action == coop::protocol::LobbyAction{LobbyActionKind::kick, 2});
    CHECK(t.shown_player() == 0);  // back to the list
    CHECK(t.view().gold == "Kicking Saria.");
    // Ban: a move in between forgets the first press.
    Choose(t, "Mido");
    Choose(t, "Ban for this session");
    t.Press(Input::kUp);
    t.Press(Input::kDown);
    CHECK(t.view().rows[RowOf(t.view(), "Ban for this session")].value.empty());
    t.Press(Input::kDecide);
    CHECK_FALSE(t.TakeRequest());
    t.Press(Input::kDecide);
    r = t.TakeRequest();
    REQUIRE(r);
    CHECK(r->action == coop::protocol::LobbyAction{LobbyActionKind::ban, 3});
}

TEST_CASE("lobby tab: a player who leaves takes their page with them, many places are one line") {
    Tab t;
    t.Show();
    t.Set(Game(true));
    Choose(t, "Mido");
    Lobby l = Game(true);
    l.players.pop_back();
    t.Set(l);
    CHECK(t.shown_player() == 0);
    l.max_players = 16;
    t.Set(l);
    const View v = t.view();
    CHECK(Has(v, "14 more players can join"));
    CHECK_FALSE(Has(v, "Open place"));
    // A new host (us, after the host left) gets the host rows.
    Lobby g = Game(false);
    g.hosting = true;
    t.Set(g);
    Choose(t, "Mido");
    CHECK(Has(t.view(), "Kick"));
}
