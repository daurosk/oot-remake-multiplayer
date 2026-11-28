// SPDX-License-Identifier: GPL-3.0-or-later
//
// /teleport <player>, the hook's side (coop_teleport.h has the rules).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/player_reader.hpp"
#include "coop_teleport.h"

namespace Core {
class System;
}
namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}

namespace CoopTeleport {

// Any thread (the chat box): true when `text` is a command, which is then
// done here and not sent as chat. Takes the session's network lock.
bool OnChat(std::string_view text, coop::host::Session& session);

// Network thread, from the session's on_event (the network lock is held):
// answers the others' requests, and takes the answers to ours.
void OnEvent(const coop::net::Event& event, coop::net::Client& client);

// Present thread, once per frame. `main_base` 0 when the game is not up;
// `session` null when not connected.
void Frame(Core::System& system, std::uint64_t main_base, const std::optional<coop::host::PlayerReading>& reading,
           coop::host::Session* session);

// Down but not out (coop_dbno_game.cpp): a spectating player comes back
// beside player `number` (`name` for the chat), as /teleport takes them
// there. Any thread. False when teleport is off in this game.
bool Beside(std::uint8_t number, std::string name);
// The respawn's fallback: our scene again at its entrance nearest our Link
// (the game's own scene change to a start point: its own respawn place).
bool ToEntrance();
// The play-test bridge's `warp` in free play (docs/playtest.md): our scene change to `scene` at its start point `start`
// by the game's own map change event (Dm_SY_BancChangeGo), as an entrance does it, with no player to follow and no
// talk or sign needed: any scene of the start list, a boss stage included. The caller has checked the names
// (CoopTestInput::WarpProblem); `start` empty means the scene's first start point (DefaultStart). False when
// teleport is off or the map change event is off (LICHII_COOP_TELEPORT=0, LICHII_COOP_TELEPORT_EVENT=0), or there is
// no start point. The event is asked at the next frames for 5 s (the respawn's window); the caller waits for the
// scene. Any thread.
bool MapChange(std::string scene, std::string start);
// map-icons-255-4 (the --solo probe's warp SETUP-FAIL 2026-11-28 00:08): with no co-op session, Frame is not called by
// the session's frame; the solo frame (coop_hook.cpp LocalFrameOff) calls it, with `session` null, while this is true:
// a MapChange waits, its event window or a trip is open, or the arrival's starter is still kept. Present thread.
bool WantsSoloFrame();
// How the last Beside / ToEntrance went, once known: true moved (or the
// scene change asked), false not (refused, no answer, not moved, no start
// point). Nothing while not known. Taken once.
std::optional<bool> TakeRespawnMove();

// cutscene-bleed (#197): a trip of ours is past its answer (travelling to its scene, settling there, or asking
// again): no other player's event may be asked of our game now (coop_hook.cpp drops them). Any thread.
bool Underway();

// The chat notices since the last call (for the chat box).
std::vector<std::string> TakeLines();

} // namespace CoopTeleport
