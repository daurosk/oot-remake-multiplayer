// SPDX-License-Identifier: GPL-3.0-or-later
// A test and research tool, off unless OOT_COOP_DEBUG_ACTORS is set: once a
// second, every actor within 60 m of our Link whose name starts with one of
// the comma-separated words (OOT_COOP_DEBUG_ACTORS=Obj_TBox,Obj_Pot; "*" = all)
// is logged with its place, the way it faces, its state, its placement hash,
// its life and its AS slots. How the two-game tests find a chest's front, a
// block's place or a switch, and how a run's log shows that an object is in
// the same state in both games. OOT_COOP_DEBUG_ACTORS_RANGE=<metres>.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "emu_port.hpp"

namespace CoopNx2navi::Objects::WorldProbe {
bool On();
void Step(GuestPort& port, std::uint64_t main, std::uint64_t now_ms,
          const std::function<void(const std::string&)>& log);
}  // namespace CoopNx2navi::Objects::WorldProbe
