// SPDX-License-Identifier: GPL-3.0-or-later
//
// Emotes and pings, the hook's side of the chat command (coop_emote.h has the
// rules; the puppet side is in coop_puppet.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}

namespace CoopEmote {

// Any thread (the chat box): true when `text` is an emote command, which is then done here and not sent as chat.
// Takes the session's network lock. Alone in the session, or too soon after the last one: a notice, nothing sent.
bool OnChat(std::string_view text, coop::host::Session& session);

// The network thread, each event from the server with its client (CoopHook's OnSessionEvent): an EMOTE with
// something to read (CoopEmote::HeardLine: a call) from another player becomes a chat notice with the sender's
// name, in every game, whether or not the sender's puppet is in sight.
void OnEvent(const coop::net::Event& event, coop::net::Client& client);

// The chat notices since the last call (for the chat box).
std::vector<std::string> TakeLines();

// The player numbers whose call (an EMOTE with something to read, as OnEvent's notice) came since the last call,
// oldest first: CoopFairy's Publish flies each caller's Navi up over their head (the ping's marker). At most 16
// wait; more are dropped.
std::vector<std::uint8_t> TakeCalls();

} // namespace CoopEmote
