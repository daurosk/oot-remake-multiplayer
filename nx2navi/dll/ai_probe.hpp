// SPDX-License-Identifier: GPL-3.0-or-later
// A research tool, off unless OOT_COOP_PROBE is set in the game's environment:
// who asks for an actor's animations, and what happens when a function of the
// game is left out. It is how the enemy copies' AI update was found
// (objects/brain.hpp; README, "Shared enemies"). Nothing of it runs in play.
//
//   OOT_COOP_PROBE=as            log, every 10 s, the callers of the game's AS
//                                play (main+0x2022a0) per actor name and
//                                animation: the return address and the return
//                                addresses found on the stack above it
//   OOT_COOP_PROBE_NAMES=Enemy_  only actors whose name starts so (default
//                                "Enemy_"; "*" every actor)
//   OOT_COOP_PROBE_COUNT=a,b,..  count the calls of these functions of main
//                                (hex offsets), with the first words they are
//                                given, every 10 s
#pragma once

#include <cstdint>

struct Nx2PluginApi;

namespace CoopNx2navi::Probe {

// Once a frame from the session's frame (any thread): registers the hooks when
// the environment asks for them and the interface has function hooks; dumps
// what was counted every 10 s.
void Frame(const Nx2PluginApi* api, std::uint64_t main_base, void (*log)(const char* line));

void Shutdown();

}  // namespace CoopNx2navi::Probe
