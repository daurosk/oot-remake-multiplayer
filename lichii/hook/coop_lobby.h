// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Lobby's rules (docs/play-online.md "Lobby", docs/protocol.md "Lobby"):
// the host's chat commands /kick, /ban, /mute and /unmute, its answers to
// players who ask to join (/allow and /deny, docs/protocol.md "Host
// approval"), each player's own mutes, and the lines the chat shows. No game, no network: the glue
// (coop_lobby_game.h) sends what these say and the server carries the host's
// actions out (server/session.cpp on_lobby), so a game cannot moderate by
// itself. Tested in tests/lichii/lobby_test.cpp.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopLobby {

namespace proto = coop::protocol;

// What a mute is of: the bits are the protocol's (proto::lobby_mute).
inline constexpr std::uint8_t kVoice = proto::lobby_mute::voice;
inline constexpr std::uint8_t kChat = proto::lobby_mute::chat;
inline constexpr std::uint8_t kBoth = kVoice | kChat;

struct Command {
    enum class Kind : std::uint8_t { kick, ban, mute, unmute, allow, deny, bad };
    Kind kind = Kind::bad;
    std::string name;            // the player as typed (a name, its start, or a number); allow, deny: may be ""
    std::uint8_t what = kBoth;   // mute and unmute: kVoice, kChat or both
    bool always = false;         // allow and deny: "always" (the server remembers the answer)
    std::string word;            // the command's own word ("kick"), for the lines
};

// Nothing for text that is no lobby command. "/kick <player>", "/ban
// <player>", "/mute <player> [voice|chat]", "/unmute <player> [voice|chat]",
// "/allow [<name>] [always]", "/deny [<name>] [always]", any case. A kick,
// ban, mute or unmute without a player is `bad` (its line says how); /allow
// and /deny without one answer the oldest player who asks to join.
std::optional<Command> Parse(std::string_view text);

// The server actions the host's command asks for, in order (mute without
// voice or chat: both).
std::vector<proto::LobbyAction> ActionsFor(Command::Kind kind, std::uint8_t what, std::uint8_t target);

// The line for a `bad` command.
std::string UsageLine(std::string_view word);

// The chat line for the server's answer to one of our actions; nothing for
// one that needs no line (a done mute: LOBBY_MUTES's line says it).
std::optional<std::string> ResultLine(const proto::LobbyResult& result, std::string_view name);

// The chat line for a change of what the host muted of a player.
std::string MutesLine(std::uint8_t before, std::uint8_t after, std::string_view name, bool self);

// A player's own mutes of others, kept by name (any ASCII case) so they hold
// when that player leaves and comes back. Only in this game: nobody is told.
class Mutes {
public:
    std::uint8_t Of(std::string_view name) const;
    // Sets or clears `what` of `name`; true when it changed.
    bool Set(std::string_view name, std::uint8_t what, bool on);
    bool empty() const { return by_name_.empty(); }

private:
    std::map<std::string, std::uint8_t> by_name_;
};

// The chat line for our own mute (or unmute) of a player.
std::string OwnMuteLine(std::string_view name, std::uint8_t what, bool on, bool changed);

// "voice", "chat", "voice and chat".
std::string WhatWords(std::uint8_t what);

// The players who ask to join (the server's JOIN_KNOCKs to this host), oldest
// first. Only the host is sent them.
class Knocks {
public:
    // A JOIN_KNOCK to the host: kept, or (wait 0) forgotten, the question
    // being over. The chat line for it; nothing for an over knock not kept.
    std::optional<std::string> Take(const proto::JoinKnock& knock);
    // The knock `who` names: "" the oldest; else the oldest whose name is it
    // (any ASCII case), else the oldest whose name starts with it.
    std::optional<proto::JoinKnock> Find(std::string_view who) const;
    void Forget(std::uint32_t knock);
    void Clear() { open_.clear(); }
    const std::vector<proto::JoinKnock>& open() const { return open_; }

private:
    std::vector<proto::JoinKnock> open_;
};

// The host's answer to `knock`, as JOIN_ALLOW: allow or deny, always or not.
proto::JoinAllow AnswerFor(const proto::JoinKnock& knock, Command::Kind kind, bool always);
// The chat line for our answer.
std::string AnswerLine(std::string_view name, bool allow, bool always);
// The chat line when /allow or /deny finds nobody (`who` as typed).
std::string NoKnockLine(std::string_view word, std::string_view who);
// The chat line for a joiner whose own join waits for the host (its JOIN_KNOCK).
std::string WaitingLine(std::uint16_t wait_s);

// The chat line when the session's host changes to another game (the host left, another took over): "" when nothing
// changed, and when this game is the new host (the host_changed event's own line says that).
// `before`/`after`: the host's number (0 none), `name`: the new host's, `self`: the new host is this game.
std::string HostChangedLine(std::uint8_t before, std::uint8_t after, std::string_view name, bool self);
// The title's hint line in a session: who hosts, so a player knows whose save Continue loads. "" when not connected.
std::string TitleHostLine(bool connected, bool self_hosts, std::string_view host_name);

}  // namespace CoopLobby
