// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue 129 (docs/dungeon.md "Live events"): a room gimmick's event
// that another player's game played is started in ours while we are in that
// room, so its result (the Deku Tree ladder falling) happens live here too.
// On unless LICHII_COOP_LIVE_EVENTS=0 (dungeon-switches, issue 176, a player
// test): started only while our Link is near the place that asks for the
// event (Places()), so the room it moves is loaded.
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

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <span>
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
inline constexpr std::uint64_t kSessionCount = 0xeb0;       // u32, as coop_boss_game.cpp
inline constexpr std::uint64_t kSessionEvents = 0xeb8;      // -> event pointers
inline constexpr std::uint64_t kSessionCurrent = 0xf18;     // u32
inline constexpr std::uint64_t kScanSession = 0x1000;       // bytes of the session searched
inline constexpr std::uint64_t kScanEvent = 0x400;          // bytes of the current event searched
inline constexpr std::uint64_t kScanInner = 0x100;          // bytes of what the event's words point to
} // namespace addr

// Where an event is asked for in its dungeon (coop_live_places.cpp, read
// from the Banc files): x, height, z of the placement.
struct Place {
    std::string_view event;
    std::string_view scene;
    float x, y, z;
};
std::span<const Place> Places();

// How near our Link must be to one of the event's places for it to start
// here. The Deku Tree's slingshot room: its door and the chest ledge are
// 11 and 17 m from the ladder's asker; the compass room's asker is 20 m
// from that door, so that room's event waits until we are in it.
inline constexpr float kNearM = 18.0f;

// Which held gimmick events to start here, and when. Pure.
class Decider {
public:
    // Events allowed: empty means every gimmick event that has a place (the
    // default); a named event with no place starts anywhere in its scene.
    explicit Decider(std::set<std::string> allowed = {}) : allowed_(std::move(allowed)) {}

    // Once a frame: the events (names without "HavePlayedEvent.") another
    // player played that our game holds back now, our scene, whether an
    // event runs here, and our Link's position (x, height, z) if read.
    // Returns the one to start now, if any: never while an event runs, only
    // near one of its places in our scene, each at most once per visit of
    // the scene.
    std::optional<std::string> Next(const std::vector<std::string>& held, const std::string& scene,
                                    bool event_running,
                                    std::optional<std::array<float, 3>> link = std::nullopt);

    // deku-dungeon-2 (DUNGEON_EVENT): another player's game started `name` in
    // `scene` just now. If it is our scene and not asked yet this visit, Next
    // offers it as if its played flag were held here (the same near-place and
    // once-a-visit rules), before that flag comes at the end of their event.
    void Started(const std::string& name, const std::string& scene);
    // Our own game started `name` in `scene` (StartWatch): never asked here
    // again this visit, also when its flag or another player's start comes later.
    void Ours(const std::string& name, const std::string& scene);

private:
    void Visit(const std::string& scene);
    std::set<std::string> allowed_;
    std::string scene_;
    std::set<std::string> asked_;  // this visit of scene_
    std::set<std::string> early_;  // Started, not asked yet (this visit)
};

// deku-dungeon-2: which gimmick event our own game just started, read from the
// game's event session ([[main + kEventStatePtr] + kEventSession]; its event
// list at +kSessionEvents, count +kSessionCount, current +kSessionCurrent, as
// coop_boss_game.cpp reads them). Where the running event keeps its name is
// not known, so the current event (kScanEvent bytes) and the session
// (kScanSession) are searched for one of `names`: as a whole C string in them,
// or as the string one of their words points to; then the same one step
// further from the event's words (kScanInner bytes each). Pure over `read`.
using Reader = bool (*)(void* context, std::uint64_t address, void* out, std::size_t size);
std::optional<std::string> FindRunningEvent(Reader read, void* context, std::uint64_t main,
                                            const std::vector<std::string_view>& names);
// The gimmick events with a place in `scene` (Places()), each once.
std::vector<std::string_view> EventsIn(std::string_view scene);

// Once a frame: when an event starts, look for its name (FindRunningEvent) on
// its 1st, 2nd, 4th, 8th and 16th frame (kTries searches: every event in a
// dungeon starts one, doors too, so not every frame); at most one name per event.
class StartWatch {
public:
    static constexpr int kTries = 5;
    // `find` is called only on those frames of a running event, until a name is found.
    template <typename Find>
    std::optional<std::string> Frame(bool running, Find&& find) {
        if (!running) {
            frame_ = 0;
            done_ = false;
            return std::nullopt;
        }
        const int f = frame_++;
        if (done_ || f >= (1 << (kTries - 1)) || (f != 0 && (f & (f + 1)) != 0)) {
            return std::nullopt;  // tries on frames 0, 1, 3, 7, 15 (counted from 0)
        }
        std::optional<std::string> name = find();
        done_ = name.has_value();
        return name;
    }

private:
    int frame_ = 0;
    bool done_ = false;
};

// LICHII_COOP_LIVE_EVENT_NAMES: "a,b,c" (spaces trimmed, empty parts
// dropped), or exactly "all" for every held gimmick event (an empty set,
// as Decider takes it). Nothing when the text names no event (" , "): then
// no live events at all.
std::optional<std::set<std::string>> ParseNames(std::string_view text);

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
