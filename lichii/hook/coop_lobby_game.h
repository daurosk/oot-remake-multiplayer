// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Lobby, the hook's side (coop_lobby.h has the rules; docs/play-online.md
// "Lobby"): the host's chat commands and the Lobby tab's buttons go to the
// server as LOBBY_ACTION, the server's LOBBY_MUTES and LOBBY_RESULT come
// back as chat lines and as what the tab shows, and each player's own mutes
// hide that player's chat lines and voice in this game only.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}

namespace CoopLobby {

// Any thread (the chat box): true when `text` is /kick, /ban, /mute or
// /unmute, which is then done here and not sent as chat. The host's goes to
// the server; anyone else's /mute and /unmute are their own mutes, and their
// /kick and /ban are refused with a line. Takes the session's network lock.
bool OnChat(std::string_view text, coop::host::Session& session);

// Network thread, from the session's on_event (the network lock is held).
void OnEvent(const coop::net::Event& event, coop::net::Client& client);

// Present thread, once per frame (`session` null when not connected): sends
// what the Lobby tab asked for since the last frame.
void Frame(coop::host::Session* session);

// The chat notices since the last call (for the chat box).
std::vector<std::string> TakeLines();

// Any thread, from the Lobby tab: a host action, sent at the next Frame (the
// server refuses it unless we host).
void Ask(const coop::protocol::LobbyAction& action);
// Any thread: our own mute of a player (by number: kept by name).
void SetOwnMute(std::uint8_t player, std::uint8_t what, bool on);

// Any thread: whether our own mutes hide this player's chat lines, voice.
bool ChatHidden(std::uint8_t player);
bool VoiceHidden(std::uint8_t player);
// Any thread: the host muted our own chat (the server drops it), so the chat
// box says so instead of showing the line as sent.
bool OwnChatMuted();
// Any thread: the host muted our own voice (the microphone then stays closed).
bool OwnVoiceMuted();

// What the Lobby tab shows of the session. Any thread.
struct PlayerSeen {
    std::uint8_t number = 0;
    std::string name;
    bool self = false;
    bool host = false;
    std::uint32_t color = 0;             // RGB: theirs, or their number's
    std::uint8_t status = 0;             // the last PLAYER_STATUS bits (dead, paused, loading)
    coop::protocol::DbnoState dbno = coop::protocol::DbnoState::up;
    std::uint8_t host_mutes = 0;         // what the host muted (kVoice, kChat)
    std::uint8_t own_mutes = 0;          // what we muted
};
struct Seen {
    bool connected = false;
    std::uint8_t self = 0;
    std::uint8_t host = 0;
    std::vector<PlayerSeen> players;     // by number
};
Seen Now();

}  // namespace CoopLobby
