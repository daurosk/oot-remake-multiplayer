// SPDX-License-Identifier: GPL-3.0-or-later
// Flags through the game's own code on Hookshot (docs/hookshot.md, "World hooks"; docs/flags.md, "Through the game's
// own functions"). Two halves, both from coop/host/game_flag_events.hpp:
// - what the game writes: the wrap of the game's flag applier (main+0xb99980, patch 0005) reads the game's write
//   queues just before the game takes them (OnApply, on the game's thread) into Events(); the hook's frame hands them
//   to the flag watcher (Feed), which then reads only those flags and not the whole list;
// - what co-op writes: GameFlags' setters put each write into Calls(), and the game's calc thread hands them to the
//   game's own setter functions (RunSetters), a few a frame, while the game's queue has room.
// Nothing here runs on the emulator: there the watcher diffs the tables and the writes go into the queues.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "coop/host/game_flag_events.hpp"
#include "coop/host/game_flags.hpp"

namespace CoopFlagNative {

// Setter calls made a frame at most. A join snapshot is some thousand writes: at 30 frames a second it is in within
// a few seconds, and each queue's own room (half of it, GameFlags::setter_has_room) holds a frame's writes further.
inline constexpr std::size_t kSettersPerFrame = 256;

inline coop::host::GameFlagEvents& Events() {
    static coop::host::GameFlagEvents events;
    return events;
}

inline coop::host::GameFlagCalls& Calls() {
    static coop::host::GameFlagCalls calls;
    return calls;
}

struct Counters {
    std::atomic<std::uint64_t> applies{0};     // runs of the game's applier seen
    std::atomic<std::uint64_t> writes{0};      // writes read from its queues
    std::atomic<std::uint64_t> setters{0};     // game setter calls made for co-op
    std::atomic<std::uint64_t> no_room{0};     // frames a setter call waited for room in the game's queue
    std::atomic<bool> events_on{true};         // LICHII_COOP_FLAG_EVENTS=0 turns the events off
};
inline Counters& Counts() {
    static Counters counters;
    return counters;
}

// The applier's wrap, phase 0, on the game's thread: `manager` is the applier's x0. Reads only.
inline void OnApply(coop::host::GameMemory& memory, std::uint64_t manager) {
    Counters& c = Counts();
    c.applies.fetch_add(1, std::memory_order_relaxed);
    if (!c.events_on.load(std::memory_order_relaxed)) {
        return;
    }
    coop::host::GameFlagEvents& events = Events();
    const std::size_t n = coop::host::queued_flag_writes(
        memory, manager, [&events](const coop::host::GameFlagWrite& write) { events.push(write); });
    c.writes.fetch_add(n, std::memory_order_relaxed);
}

// True once the game's applier ran through its wrap with the events on: from then on the watcher can go by events.
inline bool EventsLive() {
    const Counters& c = Counts();
    return c.events_on.load(std::memory_order_relaxed) && c.applies.load(std::memory_order_relaxed) != 0;
}

// The hook's frame, before the session's: the game's writes since the last frame to the flag watcher. `Session` is
// coop::host::Session (flag_events, flag_wrote, flag_wrote_all).
template <typename Session>
void Feed(Session& session) {
    const bool live = EventsLive();
    session.flag_events(live);
    coop::host::GameFlagEvents& events = Events();
    if (!live) {
        events.clear();
        return;
    }
    if (events.take_overflow()) {
        session.flag_wrote_all();  // some were dropped: the whole list once
    }
    coop::host::GameFlagWrite write;
    while (events.pop(write)) {
        session.flag_wrote(coop::host::key_of(write));
    }
}

// The game's calc thread, at the frame hook: the waiting writes to the game's setters. `call(function, x0, x1, x2)`
// calls a game function (the bridge's call) and says whether there was one. False when a setter could not be called:
// the caller then goes back to the queues (GameFlags::use_calls(nullptr)).
inline bool RunSetters(coop::host::GameMemory& memory,
                       const std::function<bool(std::uint64_t function, std::uint64_t x0, std::uint64_t x1,
                                                std::uint64_t x2)>& call) {
    coop::host::GameFlagCalls& calls = Calls();
    if (calls.size() == 0) {
        return true;
    }
    const auto manager = coop::host::GameFlags::manager_address(memory);
    if (!manager) {
        return true;  // the game's flags are not up yet: they wait
    }
    Counters& c = Counts();
    bool ok = true, waited = false;
    const std::size_t made = calls.run(kSettersPerFrame, [&](const coop::host::GameFlagWrite& write) {
        if (!coop::host::GameFlags::setter_has_room(memory, write.type)) {
            waited = true;
            return false;
        }
        // (manager, value, hash or key): docs/flags.md, "Functions".
        if (!call(memory.main_base() + coop::host::setter_offset(write.type), *manager, write.value, write.id)) {
            ok = false;
            return false;
        }
        return true;
    });
    c.setters.fetch_add(made, std::memory_order_relaxed);
    if (waited) {
        c.no_room.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

}  // namespace CoopFlagNative
