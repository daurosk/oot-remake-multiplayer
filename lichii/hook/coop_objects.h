// SPDX-License-Identifier: GPL-3.0-or-later
//
// Debug reader for stage 5 (docs/objects.md). With LICHII_COOP_DEBUG_OBJECTS=1
// it walks the game's live actor list every other frame and logs each actor
// that appears or goes (name, position, placement hash), and every change of
// an enemy's life. It reads only; it works with co-op on or off.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Core {
class System;
}
namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopObjects {

// Called once per presented frame, before the co-op session's frame.
void OnFrame(Core::System& system);

// LICHII_COOP_OBJECTS=1: shares the enemies. Present thread, once per frame
// after the session's frame; `session` is null when not connected.
void Share(Core::System& system, coop::host::Session* session,
           const std::optional<std::string>& scene, std::uint64_t server_now_us);

// Issue 88 (docs/items.md, "Heart containers"): the game's PlayerStatus.MaxLife
// flag as the session left it this frame (quarter hearts; nothing when not
// readable). The walk logs it beside our Link's Life component and raises the
// component's max to it (on by default; LICHII_COOP_HEARTS_TO_LINK=0 turns
// that off).
void SetMaxLife(std::optional<std::int32_t> quarters);

// Network thread, from the session's on_event: takes the object messages.
void OnEvent(const coop::net::Event& event);

// Present thread: the actors of the shared things this game owns now (for
// coop_drops: a drop beside one of them is its owner's to decide).
std::vector<std::uint64_t> OwnedActors();

} // namespace CoopObjects
