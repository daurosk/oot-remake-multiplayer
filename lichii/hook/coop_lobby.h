// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Lobby's rules (docs/play-online.md "Lobby", docs/protocol.md "Lobby"):
// the host's chat commands /kick, /ban, /mute and /unmute, each player's own
// mutes, and the lines the chat shows. No game, no network: the glue
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
    enum class Kind : std::uint8_t { kick, ban, mute, unmute, bad };
    Kind kind = Kind::bad;
    std::string name;            // the player as typed (a name, its start, or a number)
    std::uint8_t what = kBoth;   // mute and unmute: kVoice, kChat or both
    std::string word;            // the command's own word ("kick"), for the lines
};

// Nothing for text that is no lobby command. "/kick <player>", "/ban
// <player>", "/mute <player> [voice|chat]", "/unmute <player> [voice|chat]",
// any case. A command without a player is `bad` (its line says how).
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

}  // namespace CoopLobby
