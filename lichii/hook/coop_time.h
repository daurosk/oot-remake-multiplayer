// SPDX-License-Identifier: GPL-3.0-or-later
//
// Time of day, the same in every game (docs/time.md): reads the game's clock
// on the present thread and sets it with the game's own setter on its calc
// thread (a caller of the svc cave). The rules are in coop_time_sync.h.
// LICHII_COOP_TIME=0 turns it off; LICHII_COOP_DEBUG_TIME=1 logs the clock.
#pragma once

#include <cstdint>

namespace Core {
class System;
}
namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopTime {

// Present thread, once per presented frame. `main_base` 0 when the game is
// not up; `session` null when not connected.
// `in_event`: our Link is in an event (cutscene, talk): no clock nudge then.
void Frame(Core::System& system, std::uint64_t main_base, coop::host::Session* session,
           std::uint64_t server_now_us, bool in_event = false);

// Network thread, from the session's on_event.
void OnEvent(const coop::net::Event& event);

// A save was just loaded in this game (issue 159: a guest's load is not sent
// as a jump; it takes the host's clock again).
void SaveLoaded(std::uint64_t server_now_us);
// Our Link cannot be read this frame (any load or reload).
void Loading(std::uint64_t server_now_us);

} // namespace CoopTime
