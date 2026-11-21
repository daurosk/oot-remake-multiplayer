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

// Any thread (the chat box): true when `text` is /kick, /ban, /mute,
// /unmute, /allow or /deny, which is then done here and not sent as chat. The
// host's goes to the server; anyone else's /mute and /unmute are their own
// mutes, and their other commands are refused with a line. /allow and /deny
// answer a player who asks to join (JOIN_ALLOW; docs/protocol.md "Host
// approval"). Takes the session's network lock.
bool OnChat(std::string_view text, coop::host::Session& session);

// Network thread, from the session's on_event (the network lock is held).
// Host approval: a JOIN_KNOCK to the host and our own join waiting for the
// host (waiting_for_host) become chat lines.
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
    bool connected = false;              // the session took this game in (WELCOME came)
    std::string refused;                 // why the session turned this game away, in full ("" it did not)
    bool banned = false;                 // that reason is the host's ban (joining again cannot work)
    // How many times the session turned this game away for a reason joining again by itself cannot undo
    // (StopsRejoin: the host's Kick or ban or no, a wrong password, another version) since co-op started. It only
    // grows (Forget keeps it), so a caller that looks less often than events come still sees each one: `refused` is
    // cleared again by the disconnect that follows a kick.
    std::uint32_t refusals = 0;
    // How many times the session refused this game's name as in use there, after the client's own retries (the
    // player's own crashed game holding it) ran out. Only grows, as `refusals`: the module joins again as "<name> (2)".
    std::uint32_t names_taken = 0;
    std::uint8_t self = 0;
    std::uint8_t host = 0;
    std::vector<PlayerSeen> players;     // by number
};
Seen Now();
// A refusal auto-rejoin must not try again after (Seen::refusals): the host's own choice (kicked, banned, not let in)
// or one a retry cannot change (a wrong password, another protocol version or game build). A full server or a name
// taken may be gone a few seconds later: those still retry.
inline bool StopsRejoin(coop::protocol::RefuseReason r) {
    using R = coop::protocol::RefuseReason;
    return r == R::kicked || r == R::banned || r == R::not_let_in || r == R::bad_password ||
           r == R::wrong_protocol_version || r == R::wrong_game_build || r == R::wrong_coop_version;
}
// Who asks to join this host now (the JOIN_KNOCKs not answered yet), oldest first, by nickname. Any thread.
std::vector<std::string> Asking();
// Co-op was left (CoopHook::End sends no last event): nothing is seen any more.
void Forget();

}  // namespace CoopLobby
