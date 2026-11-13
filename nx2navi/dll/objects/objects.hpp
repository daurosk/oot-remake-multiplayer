// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared enemies and boss rooms in the NX2navi port (docs/objects.md,
// docs/dungeons-boss.md). The emulator hook's own files do the work, compiled
// unchanged: lichii/hook/coop_objects.cpp (the actor walk, the Mover, lives,
// hits, kills), coop_objects_sync.cpp (the rules), coop_world.cpp,
// coop_spawn.cpp (its director, for rolled enemies), coop_boss.cpp and
// coop_boss_game.cpp. What they took from the emulator comes from the plugin
// interface (emu_port.hpp, shim/):
//
// | Emulator                                   | Here                                         |
// |--------------------------------------------|----------------------------------------------|
// | guest memory                               | the interface's read / write (fast in v2)    |
// | the cave in moduleCalc_, its svc loop      | a hook on the frame step main+0x3b4e300, the |
// |                                            | callers asked before the step runs, each     |
// |                                            | call made with guest_call_regs (stack args)  |
// | the AI hold's patch at main+0x369df8       | hooks on the brain's calc-bits functions     |
// |                                            | (hold.hpp), both call sites                  |
// | LICHII_COOP_OBJECTS etc. (environment)     | Options, written to this process's           |
// |                                            | environment by Init                          |
//
// Needs plugin interface version 2 (function hooks, fast read): on a version 1
// game Init logs why and everything stays off.
//
// Threads, as in the emulator:
// - Frame: the DLL's frame thread, right after Session::frame (the emulator's
//   present thread): walks the actor list, decides, sends;
// - OnEvent: the network thread (the session's on_event);
// - the frame step hook: the game's calc thread, once a game frame: the Mover
//   places copies, plays their animations, writes hits, deletes, and the boss
//   mover moves Link or skips the intro. Only there are game functions called.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <string_view>

struct Nx2PluginApi;

namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopNx2navi::Objects {

struct Options {
    bool on = false;          // settings `objects = 1`: everything below is off without it
    bool boss = true;         // settings `objects_boss`: boss rooms together (LICHII_COOP_BOSS)
    // settings `objects_ai_hold`: 0 off, 1 set the brain skip bit, 2 clear it
    // (hold.hpp), 3 leave the copies' AI update out (brain.hpp)
    int ai_hold = 0;
    bool world = true;        // settings `objects_world`: the field grass caller (order 40) runs
    // settings `objects_friendly`: Npc_ and Animal_ actors are shared too (the
    // emulator build's rule). Off: only enemies and the breakable world.
    bool friendly = false;
    // settings `objects_rolls_apart`: no longer used (the shared code's own rule since the upstream
    // update: Sync::RollerFor, the lowest player within 40 m of a roll keeps it).
    int rolls_apart_m = 30;
    bool time = true;         // settings `time`: the host's time of day in every game (CoopTime)
    bool drops = true;        // settings `drops`: one drop for all, taken by who walks on it (CoopDrops)
    bool debug = false;       // OOT_COOP_DEBUG_OBJECTS=1: the actor walk's research log
    // OOT_COOP_DEBUG_NEAR=1: once a second, each enemy this game owns within
    // 150 units of our Link, with its distance to our Link and to each other
    // player (FrameInput::peers): who an enemy is with, and who is nearer.
    bool near_log = false;
    unsigned calls_per_frame = 256;  // game calls a frame for all callers (CoopSvc::kCallsPerFrame)
};

// Once, before the session starts. `api` may be null when the interface is
// not known yet: the first Frame with FrameInput::api then decides. False
// when off, or when the interface given is older than version 2 (logged).
// The hook's files read their switches from the environment on first use,
// which comes only after this (no Frame or OnEvent does anything before).
bool Init(const Nx2PluginApi* api, const Options& options, void (*log)(const char* line));
// Sharing runs (on, and the interface is version 2).
bool Active();

struct FrameInput {
    const Nx2PluginApi* api = nullptr;        // the interface once known (Overlay::Api())
    std::uint64_t main_base = 0;              // 0 while the game's main is not trusted yet
    coop::host::Session* session = nullptr;   // null while not connected
    std::optional<std::string> scene;         // our Link's scene (PlayerReader), nothing while loading
    std::optional<std::array<float, 3>> position;  // our Link
    bool event_running = false;               // a cutscene or talk runs (PlayerReading::event_running)
    std::uint64_t server_now_us = 0;          // the session's server clock (FrameReport)
    // The other players in our scene, where presence shows them now (only
    // filled for Options::near_log).
    struct Peer {
        std::uint8_t number = 0;
        std::array<float, 3> position{};
    };
    std::vector<Peer> peers;
};

// The text of one line of the near log (Options::near_log): pure, for the tests.
std::string NearLine(std::string_view name, std::uint64_t actor, const std::array<float, 3>& at,
                     const std::array<float, 3>& self, const std::vector<FrameInput::Peer>& peers);

// The DLL's frame thread, once a frame after Session::frame.
void Frame(const FrameInput& input);

// The DLL's frame thread, when FrameReport::host_reset: the host loaded a
// save (CoopObjects::ForgetKeptBreaks, locked doors' kept breaks).
void OnHostReset();

// The network thread, from the session's on_event.
void OnEvent(const coop::net::Event& event);

// One line for the status log ("" when off).
std::string StatusText();

// Takes the hooks out. Any thread.
void Shutdown();

}  // namespace CoopNx2navi::Objects
