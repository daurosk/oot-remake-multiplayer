// SPDX-License-Identifier: GPL-3.0-or-later
// The other players as a second Link (docs/puppet.md), in a game with
// NX2navi's plugin interface. The emulator build asks the puppet director
// (lichii/hook/coop_puppet.cpp, no emulator code) once a game frame from a
// code cave in the game's frame step, and the cave makes each game call the
// director returns. Here the frame callback of the plugin interface asks the
// same director and makes the calls with guest_call_regs (README, "Puppets").
//
// Threads:
// - the DLL's frame thread plans what should be shown (Publish) from presence;
// - the network thread keeps each player's animation and gear (OnEvent);
// - the game's presenting thread runs the frame callback: it is the only one
//   that touches the puppets' actors and calls game functions.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "coop_puppet.h"

namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopNx2navi::Puppet {

struct Options {
    bool puppets = true;       // false: only the game-thread callback (for teleport.hpp)
    // Interface version 2: run on the game's frame step (a function hook)
    // instead of the frame callback (OOT_COOP_FRAME_HOOK=0 turns it off).
    bool frame_hook = true;
    bool test_puppet = false;  // one more Link beside you (OOT_COOP_TEST_PUPPET=1)
    bool verbose = false;      // a status line a second (OOT_COOP_DEBUG_PUPPET=1)
    bool debug_anim = false;   // animation and gear lines (OOT_COOP_DEBUG_ANIM=1)
    bool pose_copy = false;    // the bone-by-bone copy (OOT_COOP_PUPPET_POSE=1; README)
    bool debug_pose = false;   // a pose line a second (OOT_COOP_DEBUG_POSE=1)
};

// Once, before the session starts (the director's log goes to `log`).
void Configure(const Options& options, void (*log)(const char* line));
bool Configured();

// Frame thread, once the game's main is trusted: registers the frame callback
// when the interface is there. True while the callback is registered.
bool Register();
bool Active();

// The session's own work of a frame on the game's thread, done first in the
// game-thread callback (before the director's step): reading the local player
// and everything else that touches the game's memory (coop_dll.cpp). `guest`
// is the game's memory as that thread reaches it, valid during the call.
// nullptr takes it out and waits for a call that is running.
using FrameWork = void (*)(CoopPuppet::Guest& guest, std::uint64_t main_base, void* user);
void SetFrameWork(FrameWork work, void* user);
// Game-thread callbacks so far (the frame step hook or the frame callback).
std::uint64_t Frames();

// Frame thread, every frame. `in_game`: the player was read this frame.
// `session` may be null (not connected).
void Publish(std::uint64_t main_base, bool in_game, const std::optional<std::string>& scene,
             const std::optional<std::array<float, 3>>& position, bool local_event,
             coop::host::Session* session, std::uint64_t server_now_us);

// Network thread, from the session's on_event.
void OnEvent(const coop::net::Event& event);

// One line for the status log: frames of the callback and what each puppet does.
std::string StatusText();

// Takes the frame callback out. Any thread.
void Shutdown();

}  // namespace CoopNx2navi::Puppet
