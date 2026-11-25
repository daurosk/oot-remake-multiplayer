// SPDX-License-Identifier: GPL-3.0-or-later
//
// Emotes and pings, the hook's side of the chat command (coop_emote.h has the
// rules; the puppet side is in coop_puppet.cpp).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace coop::host {
class Session;
}

namespace CoopEmote {

// Any thread (the chat box): true when `text` is an emote command, which is then done here and not sent as chat.
// Takes the session's network lock. Alone in the session, or too soon after the last one: a notice, nothing sent.
bool OnChat(std::string_view text, coop::host::Session& session);

// The chat notices since the last call (for the chat box).
std::vector<std::string> TakeLines();

} // namespace CoopEmote
