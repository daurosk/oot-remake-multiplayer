// SPDX-License-Identifier: GPL-3.0-or-later
// The Lobby's rules (lichii/hook/coop_lobby.*): the host's chat commands,
// what they ask the server for, each player's own mutes and the chat lines.
#include <doctest/doctest.h>

#include "coop_lobby.h"

using namespace CoopLobby;
using coop::protocol::LobbyAction;
using coop::protocol::LobbyActionKind;
using coop::protocol::LobbyResult;
using coop::protocol::LobbyResultCode;

TEST_CASE("lobby: the commands are parsed, any case, with what to mute") {
    CHECK_FALSE(Parse("hello"));
    CHECK_FALSE(Parse("/teleport Saria"));
    CHECK_FALSE(Parse("/kickall Saria"));
    auto c = Parse("  /KICK  Saria ");
    REQUIRE(c);
    CHECK(c->kind == Command::Kind::kick);
    CHECK(c->name == "Saria");
    c = Parse("/ban Big Goron");
    REQUIRE(c);
    CHECK(c->kind == Command::Kind::ban);
    CHECK(c->name == "Big Goron");  // names may have spaces
    c = Parse("/mute Big Goron voice");
    REQUIRE(c);
    CHECK(c->kind == Command::Kind::mute);
    CHECK(c->name == "Big Goron");
    CHECK(c->what == kVoice);
    c = Parse("/unmute 2 CHAT");
    REQUIRE(c);
    CHECK(c->kind == Command::Kind::unmute);
    CHECK(c->name == "2");
    CHECK(c->what == kChat);
    c = Parse("/mute Saria");
    REQUIRE(c);
    CHECK(c->what == kBoth);
    // A name that is only "voice": a player called Voice.
    c = Parse("/mute voice");
    REQUIRE(c);
    CHECK(c->name == "voice");
    CHECK(c->what == kBoth);
    // Without a player: bad, with the word for its line.
    c = Parse("/kick");
    REQUIRE(c);
    CHECK(c->kind == Command::Kind::bad);
    CHECK(c->word == "kick");
    CHECK(UsageLine("kick") == "kick: /kick <player> (a name, the start of one, or the number)");
    CHECK(UsageLine("mute").find("/mute <player> voice") != std::string::npos);
}

TEST_CASE("lobby: each command asks the server for its actions") {
    using K = Command::Kind;
    CHECK(ActionsFor(K::kick, kBoth, 3) == std::vector<LobbyAction>{{LobbyActionKind::kick, 3}});
    CHECK(ActionsFor(K::ban, kBoth, 3) == std::vector<LobbyAction>{{LobbyActionKind::ban, 3}});
    CHECK(ActionsFor(K::mute, kBoth, 2) ==
          std::vector<LobbyAction>{{LobbyActionKind::mute_voice, 2}, {LobbyActionKind::mute_chat, 2}});
    CHECK(ActionsFor(K::mute, kChat, 2) == std::vector<LobbyAction>{{LobbyActionKind::mute_chat, 2}});
    CHECK(ActionsFor(K::unmute, kVoice, 2) == std::vector<LobbyAction>{{LobbyActionKind::unmute_voice, 2}});
    CHECK(ActionsFor(K::unmute, 0, 2).size() == 2);
    CHECK(ActionsFor(K::bad, kBoth, 2).empty());
}

TEST_CASE("lobby: the server's answers become plain lines") {
    CHECK(ResultLine({LobbyActionKind::kick, 2, LobbyResultCode::done}, "Saria") ==
          "you took Saria out of the game");
    CHECK(ResultLine({LobbyActionKind::ban, 2, LobbyResultCode::done}, "Saria") ==
          "you banned Saria until you stop hosting");
    CHECK_FALSE(ResultLine({LobbyActionKind::mute_chat, 2, LobbyResultCode::done}, "Saria"));
    CHECK_FALSE(ResultLine({LobbyActionKind::mute_chat, 2, LobbyResultCode::unchanged}, "Saria"));
    CHECK(ResultLine({LobbyActionKind::kick, 2, LobbyResultCode::not_host}, "Saria") ==
          "lobby: only the host can do that");
    CHECK(ResultLine({LobbyActionKind::kick, 2, LobbyResultCode::no_player}, "P2") ==
          "P2 is not in the game any more");
    CHECK(ResultLine({LobbyActionKind::kick, 2, static_cast<LobbyResultCode>(99)}, "P2") ==
          "lobby: the server did not do it");
    CHECK(MutesLine(0, kChat, "Saria", false) == "the host muted Saria's chat for everyone");
    CHECK(MutesLine(0, kBoth, "", true) == "the host muted your voice and chat for everyone");
    CHECK(MutesLine(kBoth, kChat, "Saria", false) == "the host turned Saria's voice back on");
    CHECK(MutesLine(kVoice, kChat, "Saria", false) ==
          "the host muted Saria's chat for everyone; turned Saria's voice back on");
    CHECK(MutesLine(kChat, kChat, "Saria", false).empty());
}

TEST_CASE("lobby: our own mutes are kept by name, any case") {
    Mutes m;
    CHECK(m.empty());
    CHECK(m.Set("Saria", kVoice, true));
    CHECK_FALSE(m.Set("SARIA", kVoice, true));  // already
    CHECK(m.Of("saria") == kVoice);
    CHECK(m.Set("saria", kChat, true));
    CHECK(m.Of("Saria") == kBoth);
    CHECK(m.Set("Saria", kBoth, false));
    CHECK(m.Of("Saria") == 0);
    CHECK(m.empty());
    CHECK(OwnMuteLine("Saria", kVoice, true, true) == "you muted Saria's voice (only you: nobody is told)");
    CHECK(OwnMuteLine("Saria", kChat, false, true) == "you hear Saria's chat again");
    CHECK(OwnMuteLine("Saria", kBoth, true, false) == "Saria's voice and chat were muted already");
}
