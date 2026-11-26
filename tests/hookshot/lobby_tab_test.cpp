// SPDX-License-Identifier: GPL-3.0-or-later
// The Lobby tab (hookshot/src/coop_lobby_tab.*): the list, a player's page,
// what every player may do and what only the host may, and Kick's confirm.
#include <doctest/doctest.h>

#include <string>

#include "coop_lobby_tab.h"
#include "coop/protocol/messages.hpp"
#include "coop_room_list.h"

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

TEST_CASE("lobby tab: out of a game it says where to host or join, and lists Public games") {
    Tab t;
    t.Show();
    const View v = t.view();
    REQUIRE(v.rows.size() == 6);
    CHECK(v.rows[1].kind == RowKind::kNote);
    CHECK(v.rows[1].name == "You are not in a game.");
    CHECK(v.rows[3].kind == RowKind::kHeading);
    CHECK(v.rows[3].name == "Public games");
    CHECK(v.rows[4].name == "Looking for public games...");   // (nothing heard yet)
    CHECK(v.rows[5].name == "Refresh list");
    CHECK(v.selected == 5);
    CHECK_FALSE(t.Press(Input::kBack));  // the menu closes
}

TEST_CASE("lobby tab: Public games: a public game joins with one press, a private one goes to its password, "
          "the empty and unreachable states say so") {
    Tab t;
    t.Show();
    Lobby l;
    l.games_state = "No public games right now. Host one and choose Public game.";
    t.Set(l);
    CHECK(t.view().rows[4].name == l.games_state);
    l.games_state.clear();
    l.games = {PublicGame{"forest-4821", "forest-4821", "Public game, Players 1/8", true, true},
               PublicGame{"kokiri", "kokiri (Saria)", "Needs the host's password, Players 1/4", true, false},
               PublicGame{"lake", "lake (Ruto)", "Public game, Full", false, true}};
    t.Set(l);
    View v = t.view();
    REQUIRE(v.rows.size() == 8);
    CHECK(v.rows[4].name == "forest-4821");
    CHECK(v.rows[4].value == "Join");
    CHECK(v.rows[6].value == "Public game, Full");   // (not joinable: why, instead of Join)
    t.Select(4);
    CHECK(t.Press(Input::kDecide));
    auto r = t.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoinRoom);
    CHECK(r->name == "forest-4821");
    CHECK(r->on);
    t.Select(5);
    t.Press(Input::kDecide);
    r = t.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoinRoom);
    CHECK(r->name == "kokiri");
    CHECK_FALSE(r->on);   // (the Join page asks for the host's password; nothing is tried)
    t.Select(6);
    t.Press(Input::kDecide);
    CHECK_FALSE(t.TakeRequest().has_value());   // (full: nothing to do)
    t.Select(7);
    t.Press(Input::kDecide);
    r = t.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kRefreshGames);
}

TEST_CASE("lobby tab: Public games page by kGamesPerPage with a Page row that Left, Right, A and a click turn, round "
          "(row 264)") {
    Tab t;
    t.Show();
    Lobby l;
    const auto with = [&](int n) {
        l.games.clear();
        for (int i = 0; i < n; ++i) {
            const std::string room = "room-" + std::to_string(i);
            l.games.push_back(PublicGame{room, room, "Public game, Players 1/4", true, true});
        }
        t.Set(l);
    };
    with(kGamesPerPage);   // (one page: no Page row, every game listed)
    View v = t.view();
    CHECK_FALSE(Has(v, "Page"));
    CHECK(Has(v, "room-0"));
    CHECK(Has(v, "room-" + std::to_string(kGamesPerPage - 1)));
    with(2 * kGamesPerPage + 3);   // (three pages)
    v = t.view();
    const int page = RowOf(v, "Page");
    REQUIRE(page == 4);   // (right under the heading)
    CHECK(v.rows[4].kind == RowKind::kChoice);
    CHECK(v.rows[4].value == "1/3");
    CHECK(v.rows[4].left);
    CHECK(v.rows[4].right);
    CHECK(Has(v, "room-0"));
    CHECK_FALSE(Has(v, "room-" + std::to_string(kGamesPerPage)));
    CHECK(Has(v, "Refresh list"));
    t.Select(4);
    CHECK(t.view().change == "Turn the page");
    t.Press(Input::kRight);
    v = t.view();
    CHECK(v.rows[4].value == "2/3");
    CHECK(v.selected == 4);   // (the cursor stays on the Page row)
    CHECK(Has(v, "room-" + std::to_string(kGamesPerPage)));
    CHECK_FALSE(Has(v, "room-0"));
    t.Press(Input::kDecide);   // (A: the page after)
    CHECK(t.view().rows[4].value == "3/3");
    CHECK(Has(t.view(), "room-" + std::to_string(2 * kGamesPerPage + 2)));
    t.Press(Input::kRight);   // (round: past the last, the first)
    CHECK(t.view().rows[4].value == "1/3");
    t.Press(Input::kLeft);    // (round: before the first, the last)
    CHECK(t.view().rows[4].value == "3/3");
    t.Click(4, -1);           // (the value's left half: the page before)
    CHECK(t.view().rows[4].value == "2/3");
    // A game on a later page joins by its own room.
    v = t.view();
    const int game = RowOf(v, "room-" + std::to_string(kGamesPerPage + 1));
    REQUIRE(game >= 0);
    t.Select(static_cast<std::size_t>(game));
    t.Press(Input::kDecide);
    auto r = t.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoinRoom);
    CHECK(r->name == "room-" + std::to_string(kGamesPerPage + 1));
    // The list shrinks under the last page: the last page there is shown.
    t.Select(4);
    t.Press(Input::kRight);
    CHECK(t.view().rows[4].value == "3/3");
    with(kGamesPerPage + 1);
    CHECK(t.view().rows[4].value == "2/2");
    with(1);   // (one page again: no Page row)
    CHECK_FALSE(Has(t.view(), "Page"));
    t.Show();   // (the tab shown again: its first page)
    with(3 * kGamesPerPage);
    CHECK(t.view().rows[4].value == "1/3");
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
    CHECK(Has(v, "Kokiri Forest  2.5 / 3 hearts"));
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

// lobby-state-b (player report 22): the Multiplayer tab said "Connected!" while this tab said "You are not in a game".
TEST_CASE("lobby tab: co-op started but not in the session yet says so, not 'not in a game'") {
    Tab t;
    t.Show();
    Lobby l;
    l.waiting = "Co-op is starting...";
    t.Set(l);
    View v = t.view();
    REQUIRE(v.rows.size() == 3);
    CHECK(v.rows[1].name == "Co-op is starting...");
    CHECK_FALSE(Has(v, "You are not in a game."));
    l.waiting = "Your friend's game turned this one away: the session is full.";
    t.Set(l);
    CHECK(t.view().rows[1].name == l.waiting);
    CHECK(t.view().rows[2].name == "The players show here once this game is in the session.");
    l.waiting_next = "Ask the host to choose Host (Multiplayer tab, or the title screen). Keep this page open: it joins "
                     "by itself.";
    t.Set(l);
    CHECK(t.view().rows[2].name == l.waiting_next);
}

TEST_CASE("lobby tab: a join with no host to answer says so, not 'Co-op is starting...' forever (row 208)") {
    // (hos_coop_host.h: 0 idle, 1 starting, 2 hosting, 3 joining, 4 waiting for the host, 5 connected, 6 failed)
    CHECK(WaitingText(0, "", false, 0) == Waiting{});
    CHECK(WaitingText(6, "", false, 99999) == Waiting{});
    // Our own hosting: as before.
    CHECK(WaitingText(1, "", false, 0).line == "Co-op is starting...");
    CHECK(WaitingText(2, "", false, kNoAnswerMs * 3).line == "Co-op is starting...");
    CHECK(WaitingText(2, "", false, 0).next.empty());
    // Joining: first looking, then (no session after kNoAnswerMs) the host has not answered: Host first.
    CHECK(WaitingText(3, "", false, 0).line == "Looking for the host's game...");
    CHECK(WaitingText(5, "", false, kNoAnswerMs - 1).line == "Joining the host's game...");
    for (const int state : {3, 5}) {
        const Waiting w = WaitingText(state, "", false, kNoAnswerMs);
        CHECK(w.line == "The host's game has not answered yet.");
        CHECK(w.next ==
              "Ask the host to choose Host (Multiplayer tab, or the title screen). Keep this page open: it joins by itself.");
    }
    // The host's PC did not answer directly: at once, with the ANSWER CODE.
    const Waiting answer = WaitingText(4, "ANSWER-1234", false, 0);
    CHECK(answer.line == "The host's game has not answered yet.");
    CHECK(answer.next == "Ask the host to choose Host (Multiplayer tab, or the title screen). If they host already, "
                         "send them this answer code: ANSWER-1234");
    // Held by the host's approval (approve_joins): the host has to let us in, whatever the state and time.
    for (const int state : {3, 4, 5}) {
        const Waiting held = WaitingText(state, "ANSWER-1234", true, kNoAnswerMs * 2);
        CHECK(held.line == "The host has to let you in.");
        CHECK(held.next == "They answer with /allow in their chat. This page joins by itself once they do.");
    }
}

TEST_CASE("lobby tab: in the session before a file is loaded, the players show with a line to load one") {
    for (const bool host : {false, true}) {
        Tab t;
        t.Show();
        Lobby l = Game(host);
        t.Set(l);
        View v = t.view();
        CHECK(Has(v, "Host: Link" + std::string(host ? " (you)" : "")));
        CHECK(Has(v, host ? "Saria" : "Saria (you)"));
        CHECK(Has(v, "Load your file to play: the others see you once you do."));
        for (auto& p : l.players) {
            if (p.self) p.place = "Kokiri Forest";
        }
        t.Set(l);
        CHECK_FALSE(Has(t.view(), "Load your file to play: the others see you once you do."));
    }
}

TEST_CASE("Lobby tab: the Discord status line is Solo with co-op idle and the count in a session") {
    CHECK(DiscordState(false, 0) == "Solo");
    CHECK(DiscordState(false, 3) == "Solo");   // still starting: not in the session yet
    CHECK(DiscordState(true, 1) == "Co-op: 1 player");
    CHECK(DiscordState(true, 3) == "Co-op: 3 players");
    // The patch's checks (host/discord_rpc.c valid_text): 2 to 127 bytes, no control character.
    for (const std::string& s : {DiscordState(false, 0), DiscordState(true, 1), DiscordState(true, 255)}) {
        CHECK(s.size() >= 2);
        CHECK(s.size() <= 127);
    }
}

// Player report 120: a guest asks for the host's save again from the Lobby tab (CoopSave::RequestResync).
TEST_CASE("lobby tab: Resync from host is a guest's row only, and asks once per press") {
    Tab t;
    t.Show();
    Lobby guest = Game(false);
    guest.can_resync = true;
    t.Set(guest);
    REQUIRE(Has(t.view(), "Resync from host"));
    Choose(t, "Resync from host");
    const auto r = t.TakeRequest();
    REQUIRE(r);
    CHECK(r->kind == Request::Kind::kResync);
    CHECK_FALSE(t.TakeRequest());
    CHECK(t.view().gold == "Asked the host for its save: the chat says how it goes.");

    // Never for the host, never where the game cannot take it (the emulator), never out of a session (solo).
    Lobby host = Game(true);
    host.can_resync = true;
    t.Set(host);
    CHECK_FALSE(Has(t.view(), "Resync from host"));
    guest.can_resync = false;
    t.Set(guest);
    CHECK_FALSE(Has(t.view(), "Resync from host"));
    Lobby solo;
    solo.can_resync = true;
    t.Set(solo);
    CHECK_FALSE(Has(t.view(), "Resync from host"));
}

TEST_CASE("public games: only a room with a proof that checks is the official game, first and marked") {
    using CoopRoomList::Proof;
    using CoopRoomList::Room;
    namespace proto = coop::protocol;
    // A test key (RFC 8032's), given to Order in place of the kits' built-in one.
    const proto::OfficialSeed seed =
        *proto::official_seed_from_text("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    const proto::OfficialPublicKey key = proto::official_public_key(seed);
    const std::uint64_t now = 1'795'000'000;
    const auto room = [](const char* name, bool pub) {
        Room r;
        r.room = name;
        r.is_public = pub;
        r.protocol = proto::kProtocolVersion;
        return r;
    };
    const std::string kits = proto::official_relay_name("159.54.182.85", 27530);
    const std::string other_relay = proto::official_relay_name("203.0.113.9", 27530);
    const proto::OfficialProof good =
        proto::official_sign(seed, kits, "squadala", proto::public_listing_version(), now - 5);
    std::vector<Room> rooms{room("kokiri", false), room("forest-4821", true), room("squadala", true), room("zora", true)};
    CoopRoomList::Order(rooms, {Proof{"squadala", good}}, now, kits, kits, key);
    REQUIRE(rooms.size() == 4);
    CHECK(rooms[0].room == "squadala");
    CHECK(rooms[0].official);
    CHECK(rooms[1].room == "forest-4821");   // (then public games, in the relay's order)
    CHECK(rooms[2].room == "zora");
    CHECK(rooms[3].room == "kokiri");
    CHECK_FALSE(rooms[1].official);
    // No proof (an old relay, or someone who took the name): an ordinary public game, in the relay's order.
    std::vector<Room> plain{room("forest-4821", true), room("squadala", true)};
    CoopRoomList::Order(plain, {}, now, kits, kits, key);
    CHECK(plain[0].room == "forest-4821");
    CHECK_FALSE(plain[1].official);
    // The proof given for another room, a stale (replayed) one, one of another key, or a private room: not official.
    CHECK_FALSE(CoopRoomList::Proven(room("forest-4821", true), {Proof{"forest-4821", good}}, now, kits, kits, key));
    CHECK_FALSE(CoopRoomList::Proven(room("squadala", true), {Proof{"squadala", good}},
                                     now + proto::kOfficialProofMaxAgeS + 60, kits, kits, key));
    CHECK_FALSE(CoopRoomList::Proven(room("squadala", true), {Proof{"squadala", good}}, now, kits, kits));   // (built-in key)
    // The same fresh proof passed on through another relay (listed there with our proofs): never official, whether
    // that relay is the player's own setting or not.
    CHECK_FALSE(CoopRoomList::Proven(room("squadala", true), {Proof{"squadala", good}}, now, other_relay, kits, key));
    CHECK_FALSE(CoopRoomList::Proven(room("squadala", true), {Proof{"squadala", good}}, now, other_relay,
                                     other_relay, key));
    CHECK_FALSE(CoopRoomList::Proven(room("squadala", true), {Proof{"squadala", good}}, now, "", "", key));
    CHECK_FALSE(CoopRoomList::Proven(room("squadala", false), {Proof{"squadala", good}}, now, kits, kits, key));
    // A room of another version: its listing's version is in the proof.
    Room older = room("squadala", true);
    older.protocol = proto::kProtocolVersion - 1;
    CHECK_FALSE(CoopRoomList::Proven(older, {Proof{"squadala", good}}, now, kits, kits, key));
}
