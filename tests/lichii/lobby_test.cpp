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

// Host approval (docs/protocol.md "Host approval"): the host's /allow and /deny.
using coop::protocol::JoinAllow;
using coop::protocol::JoinKnock;

TEST_CASE("lobby: /allow and /deny are parsed, with or without a name, and always") {
    auto c = Parse("/allow");
    REQUIRE(c);
    CHECK(c->kind == Command::Kind::allow);
    CHECK(c->name.empty());  // the oldest who asks
    CHECK_FALSE(c->always);
    c = Parse("  /ALLOW  Big Goron ");
    REQUIRE(c);
    CHECK(c->kind == Command::Kind::allow);
    CHECK(c->name == "Big Goron");
    c = Parse("/allow Riley Always");
    REQUIRE(c);
    CHECK(c->name == "Riley");
    CHECK(c->always);
    c = Parse("/deny always");
    REQUIRE(c);
    CHECK(c->kind == Command::Kind::deny);
    CHECK(c->name.empty());
    CHECK(c->always);
    c = Parse("/deny Riley");
    REQUIRE(c);
    CHECK(c->word == "deny");
    CHECK(c->name == "Riley");
    CHECK_FALSE(c->always);
    CHECK_FALSE(Parse("/allowed Riley"));
    // Nothing for the server's Lobby actions.
    CHECK(ActionsFor(Command::Kind::allow, kBoth, 2).empty());
}

TEST_CASE("lobby: the players who ask to join are kept until answered or over, with a line each") {
    Knocks k;
    CHECK_FALSE(k.Find(""));
    CHECK(k.Take(JoinKnock{7, 60, 0, "Riley"}) == "Riley wants to join: /allow or /deny (60 s)");
    // A second one: the commands name the player.
    CHECK(k.Take(JoinKnock{8, 60, coop::protocol::join_knock::known, "Big Goron"}) ==
          "Big Goron wants to join (you let this player in before): /allow Big Goron or /deny Big Goron (60 s)");
    REQUIRE(k.open().size() == 2);
    CHECK(k.Find("")->knock == 7);           // the oldest
    CHECK(k.Find("big goron")->knock == 8);  // any case
    CHECK(k.Find("Bi")->knock == 8);         // the start of a name
    CHECK_FALSE(k.Find("Saria"));
    // The server says a knock is over (the time ran out, or they left).
    CHECK(k.Take(JoinKnock{7, 0, 0, "Riley"}) == "Riley is no longer waiting to join");
    CHECK(k.open().size() == 1);
    CHECK_FALSE(k.Take(JoinKnock{7, 0, 0, "Riley"}));  // over already: no second line
    // Answered: forgotten.
    k.Forget(8);
    CHECK(k.open().empty());
    CHECK_FALSE(k.Take(JoinKnock{8, 0, 0, "Big Goron"}));
    k.Take(JoinKnock{9, 60, 0, "Mido"});
    k.Clear();
    CHECK(k.open().empty());
}

TEST_CASE("lobby: the host's answer and its lines") {
    const JoinKnock knock{7, 60, 0, "Riley"};
    CHECK(AnswerFor(knock, Command::Kind::allow, false) == JoinAllow{7, true, false});
    CHECK(AnswerFor(knock, Command::Kind::deny, true) == JoinAllow{7, false, true});
    CHECK(AnswerLine("Riley", true, false) == "you let Riley in");
    CHECK(AnswerLine("Riley", true, true) == "you let Riley in; they join without asking until you stop hosting");
    CHECK(AnswerLine("Riley", false, false) == "you turned Riley away");
    CHECK(AnswerLine("Riley", false, true) == "you turned Riley away; they are kept out until you stop hosting");
    CHECK(NoKnockLine("allow", "") == "allow: nobody is asking to join");
    CHECK(NoKnockLine("deny", "Saria") == "deny: nobody called Saria is asking to join");
    CHECK(WaitingLine(60) == "the host is asked to let you in: waiting up to 60 s");
}

TEST_CASE("lobby: who hosts, in the chat and on the title") {
    CHECK(HostChangedLine(1, 3, "Emily", false) == "Emily is now the host: their save is the one played");
    CHECK(HostChangedLine(1, 2, "Zach", true).empty());   // (this game: the host_changed event's own line, translated)
    CHECK(HostChangedLine(0, 1, "Zach", false).empty());   // (the first host: joining, not a change)
    CHECK(HostChangedLine(2, 2, "Zach", false).empty());
    CHECK(TitleHostLine(false, true, "Zach").empty());
    CHECK(TitleHostLine(true, true, "Zach") == "Co-op: you host. Continue loads your save.");
    CHECK(TitleHostLine(true, false, "Emily") == "Co-op: Emily hosts. Continue loads their save.");
}
