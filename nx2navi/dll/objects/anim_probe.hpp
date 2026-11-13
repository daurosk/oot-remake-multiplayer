// SPDX-License-Identifier: GPL-3.0-or-later
//
// A research tool, off unless OOT_COOP_PROBE_ANIM is set (1: read only; 2:
// with the freeze test). On the game's calc thread, in the frame step hook,
// for every Enemy_ actor within 30 m of our Link: twice a second one log line
// with how many of the last game frames changed its skeleton's local
// transforms (is it animated?), its AS slots' names and the words around the
// places the notes take for rate and frame. Mode 2 also writes each such
// enemy's skeleton back to one pose every frame, OOT_COOP_PROBE_FREEZE_AFTER
// seconds (25) after it was first seen, and counts whether the write was still
// there a frame later: whether a pose written at this point is what is drawn.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "emu_port.hpp"

struct Nx2PluginApi;

namespace CoopNx2navi::Objects::AnimProbe {
bool On();
// Also logs every AS play of a few enemy kinds (a hook on main+0x2022a0).
void InstallHooks(const Nx2PluginApi* api, std::uint64_t main, void (*log)(const std::string&));
void Step(GuestPort& port, std::uint64_t main, std::uint64_t now_ms,
          const std::function<void(const std::string&)>& log);

}  // namespace CoopNx2navi::Objects::AnimProbe
