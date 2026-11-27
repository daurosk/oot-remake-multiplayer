// SPDX-License-Identifier: GPL-3.0-or-later
// Wait for the party (docs/party-wait.md), the hook's side: the session's PARTY_WAIT_MODE, /wait in the chat, and
// each frame the moment our Link is in and who is here, run through CoopPartyWait::Waiter (coop_party_wait.h).
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
}  // namespace coop::net

namespace CoopPartyWait {

// The session's value (any thread): false off a session, until the server says it is on.
bool On();

// /wait, /wait on, /wait off (any thread, from the chat box). False: not this command.
bool OnChat(std::string_view text, coop::host::Session& session);

// The network thread's messages (PARTY_WAIT_MODE) and connection changes.
void OnEvent(const coop::net::Event& event, coop::net::Client& client);

// The present thread, after CoopBoss::Frame: our scene and place (nothing: no Link). Returns at once, reading
// nothing, while the setting is off or there is no session (solo, co-op idle).
void Frame(const std::optional<std::string>& scene, const std::optional<coop::protocol::Vec3>& position,
           coop::host::Session* session, std::uint64_t server_now_us);

// Banner notices for our player, and the lines to say to the others (sent as our chat).
std::vector<std::string> TakeLines();
std::vector<std::string> TakeChatLines();

}  // namespace CoopPartyWait
