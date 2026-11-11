// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue 129, test7 (docs/dungeon.md "Live events"): a room gimmick's event
// that another player's game played is started in ours while we are in that
// room, so its result (the Deku Tree ladder falling) happens live here too.
// Behind LICHII_COOP_LIVE_EVENTS=1 (off by default: not run in a game yet).
//
// The game's own way, read from main (build e0f3ccfaf50065ae), as the AI
// node SeqOneShotRequestEvent (execute at 0x32db1a0) does it:
//   request = a 0x1b20-byte EventRequestArg; 0xd79f10(request) builds it;
//   its event name is a 0x80-byte buffer: pointer at +0x1a90, capacity
//   (u32) at +0x1a98; the node copies its input string there;
//   0xd7a820(manager, request), manager = [[main + 0x4c7e090]];
//   then the two string members at +0x1968 and +0x1950 get the base vtable
//   (main + 0x4974b20) and 0x20b6f0(member) frees each.
// The cave calls them in that order on the game's calc thread; the block
// comes from the game's malloc once and is kept for the run.
// No emulator code here: tested in tests/lichii/live_events_test.cpp.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "coop_puppet.h"

namespace CoopLiveEvents {

namespace addr {
inline constexpr std::uint64_t kMalloc = 0x14c6c90;        // as coop_puppet.h
inline constexpr std::uint64_t kArgBuild = 0xd79f10;       // EventRequestArg(this)
inline constexpr std::uint64_t kRequest = 0xd7a820;        // (manager, arg)
inline constexpr std::uint64_t kManagerPtr = 0x4c7e090;    // [[main + this]] = the manager
inline constexpr std::uint64_t kStringFree = 0x20b6f0;     // (member)
inline constexpr std::uint64_t kStringVtable = 0x4974b20;  // the base vtable put back first
inline constexpr std::uint64_t kArgSize = 0x1b20;
inline constexpr std::uint64_t kArgName = 0x1a90;          // char* (inline buffer)
inline constexpr std::uint64_t kArgNameCap = 0x1a98;       // u32
inline constexpr std::uint64_t kArgMember1 = 0x1968;       // freed first, as the node does
inline constexpr std::uint64_t kArgMember2 = 0x1950;
inline constexpr std::uint64_t kEventStatePtr = 0x4e94840;  // as coop_boss_game.cpp
inline constexpr std::uint64_t kEventSession = 0x68;        // set while an event runs
} // namespace addr

// Which held gimmick events to start here, and when. Pure.
class Decider {
public:
    // Events allowed: empty means every gimmick event; by default only the
    // Deku Tree slingshot room's ladder (the first one to prove).
    explicit Decider(std::set<std::string> allowed = {"InsideTheDekuTree_Gimmick_FallingLadder"})
        : allowed_(std::move(allowed)) {}

    // Once a frame: the events (names without "HavePlayedEvent.") another
    // player played that our game holds back now, our scene, and whether an
    // event runs here. Returns the one to start now, if any: never while an
    // event runs, each at most once per visit of the scene.
    std::optional<std::string> Next(const std::vector<std::string>& held, const std::string& scene,
                                    bool event_running);

private:
    std::set<std::string> allowed_;
    std::string scene_;
    std::set<std::string> asked_;  // this visit of scene_
};

// LICHII_COOP_LIVE_EVENT_NAMES: "a,b,c" (spaces trimmed, empty parts
// dropped), or "all" for every held gimmick event (an empty set).
std::set<std::string> ParseNames(std::string_view text);

// The cave side: starts the asked events, one at a time. Pure over Guest.
class Requester {
public:
    void Want(std::string name);
    // CoopSvc::Caller's contract: asked once a frame with after_call false,
    // then again after each call it returned, with the call's x0.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main);
    // Log lines since the last call.
    std::vector<std::string> TakeNotes();
    std::uint64_t Block() const { return block_; }

private:
    enum class Next { none, malloc, build, request, free1, free2 };
    // The two string members' frees, as the node does them; every path after
    // a build goes through them, so the reused block is never built over
    // live members.
    std::optional<CoopPuppet::Call> FreeFirst(CoopPuppet::Guest& guest, std::uint64_t main);
    static bool EventRunning(CoopPuppet::Guest& guest, std::uint64_t main);
    std::deque<std::string> wanted_;
    std::string current_;
    std::uint64_t block_ = 0;
    Next next_ = Next::none;  // the call made last, whose return comes next
    bool malloc_failed_ = false;
    std::vector<std::string> notes_;
};

} // namespace CoopLiveEvents
