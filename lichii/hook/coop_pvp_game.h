// SPDX-License-Identifier: GPL-3.0-or-later
//
// Player against player, the hook's side (coop_pvp.h has the rules;
// docs/play-online.md "PvP").
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/player_reader.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}

namespace CoopPvp {

// Any thread (the chat box): true when `text` is /pvp, which is then done
// here and not sent as chat. Takes the session's network lock.
bool OnChat(std::string_view text, coop::host::Session& session);

// Network thread, from the session's on_event (the network lock is held):
// PVP_MODE, PVP_HIT and PVP_HURT; a new connection starts with PvP off.
void OnEvent(const coop::net::Event& event, coop::net::Client& client);

// Present thread, from the grass sync: one sword swing of our own Link (the
// foliage cut it queued; coop_objects.cpp). Kept for the next Frame.
void OwnSwing(const coop::protocol::GrassCut& cut);

// Present thread, from the spawn sync: our own shot ended at `at` (an arrow
// or seed that hit something, a bomb that went off).
void OwnShot(coop::protocol::DamageCause cause, const coop::protocol::Vec3& at);

// Present thread, once per frame, after the objects and spawn syncs.
// `session` null when not connected.
void Frame(const std::optional<std::string>& scene, const std::optional<coop::host::PlayerReading>& reading,
           coop::host::Session* session, std::uint64_t server_now_us);

// Any thread: whether PvP is on in this session (the server's value).
bool On();

// The chat notices since the last call (for the chat box).
std::vector<std::string> TakeLines();

} // namespace CoopPvp
