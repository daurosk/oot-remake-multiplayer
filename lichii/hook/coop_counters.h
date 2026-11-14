// SPDX-License-Identifier: GPL-3.0-or-later
//
// Looping platforms put in phase through their pause counters (docs/objects.md,
// "Moving platforms: the pause counters"; issues 196 and 194). Method and the
// game's addresses from Hot Onion (shared by Alex, 2026-11-12), same build.
//
// A looping platform (Dodongo's Cavern's up/down lifts and pillars) is moved
// by its AI graph: a move, a pause, a move, a pause. Each pause is an
// ExecuteGenericCounter node: +0x20 the frames left, +0x2c its length, and
// main+0x5e6760 starts it each time the graph enters it. The moves take the
// same time in every game, so when one pause in one game is made shorter or
// longer by the gap, the platform is in phase for good.
//
// - The cave (coop_counter.s) keeps every node the game starts in a ring;
//   the hook reads it and keeps the nodes whose actor is an `Obj_`.
// - Where a node's pause began is (its length - frames left) / 60 s ago; the
//   time between two starts of one node is its period. Two periods in a row
//   within kPeriodSlack: the node loops (a platform moved once by a switch
//   never does, and is left to the switch's flag). One node may time both
//   waits of a lift whose way up and way down take different times: the
//   periods then go A, B, A, B, and the loop is A + B (the 2026-11-12 run:
//   the Dodongo lifts never looped by the first rule); the games then
//   compare the start that ends the longer period. Once looping, it
//   stays so until kLoopMisses starts in a row fit neither.
// - Every kSendEveryUs each game sends its looping nodes (PLATFORM_COUNTER):
//   key, period, time since the last start, starts seen, with the server time.
// - A game whose node started more than kOffS from another game's start of
//   the same key (the short way round the period) and has seen fewer
//   starts (a tie: the higher player number) moves its frames left by the
//   gap once, during that pause. Once in phase nothing is written. A pause
//   menu or a hitch puts one out of phase; the next pause puts it back.
//
// No emulator code. Tested in tests/lichii/counters_test.cpp.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopCounters {

// The game's (build e0f3ccfaf50065ae; Hot Onion's notes, checked against
// the dump: the function starts `pacibsp; str d8, [sp, #-0x30]!; ...`).
inline constexpr std::uint64_t kStartOffset = 0x5e6760;       // ExecuteGenericCounter start
inline constexpr std::uint32_t kStartOriginal = 0xd503237f;   // pacibsp
inline constexpr std::uint64_t kNodeVtable = 0x499f200;
inline constexpr std::uint64_t kNodeRemaining = 0x20;         // f32 frames left
inline constexpr std::uint64_t kNodeDuration = 0x2c;          // f32 frames
inline constexpr std::uint64_t kNodeGraph = 0x8;              // [node + 8]: the graph instance
inline constexpr std::uint64_t kGraphOwner = 0x8;             // [[node + 8] + 8]
inline constexpr std::uint64_t kOwnerActor = 0x1f0;           // [[[node + 8] + 8] + 0x1f0]: the actor
inline constexpr float kFramesPerSecond = 60.0f;

// The cave (coop_counter.s) and the u64 that holds the ring's address.
inline constexpr std::uint64_t kCaveOffset = 0x3c46700;
inline constexpr std::uint64_t kRingPointer = 0x3c46780;
inline constexpr std::size_t kRingNodes = 256;  // every AI counter of the scene goes through it
inline constexpr std::size_t kRingBytes = 8 + 8 * kRingNodes;
inline constexpr std::array<std::uint8_t, 40> kCave{
    0x7f, 0x23, 0x03, 0xd5, 0xf0, 0x03, 0x00, 0x58, 0xf0, 0x00, 0x00, 0xb4, 0x11, 0x02,
    0x40, 0xb9, 0x31, 0x06, 0x00, 0x11, 0x11, 0x02, 0x00, 0xb9, 0x31, 0x1e, 0x00, 0x12,
    0x10, 0x22, 0x00, 0x91, 0x00, 0x5a, 0x31, 0xf8, 0x10, 0x80, 0x26, 0x17,
};

// `b kCaveOffset` at kStartOffset.
constexpr std::uint32_t HookWord() {
    return 0x14000000u |
           static_cast<std::uint32_t>(((kCaveOffset - kStartOffset) >> 2) & 0x3ffffff);
}

// The nodes in one read of the ring (0 entries left out, each once).
std::vector<std::uint64_t> RingNodes(const std::array<std::uint64_t, kRingNodes>& entries);

// Hookshot (patch 0005, hookshot/src/hos_coop_world.h): the game's C hands
// each started node to the module (HOS_COOP_WORLD_COUNTER_START), on
// whichever game thread ran the start; it goes into this ring in our own
// memory, the guest ring's layout: the count goes up, then the node goes
// at count % kRingNodes. No lock, as the cave: the reader checks every
// node itself, so a torn or lost entry costs nothing.
class HostRing {
public:
    void Push(std::uint64_t node) noexcept {
        if (node == 0) {
            return;
        }
        const std::uint32_t n = count_.fetch_add(1, std::memory_order_acq_rel) + 1;
        nodes_[n % kRingNodes].store(node, std::memory_order_release);
    }
    // The entries and the count (starts so far), as read from the guest ring.
    std::uint32_t Read(std::array<std::uint64_t, kRingNodes>& out) const noexcept {
        const std::uint32_t count = count_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < kRingNodes; ++i) {
            out[i] = nodes_[i].load(std::memory_order_acquire);
        }
        return count;
    }
    std::uint32_t Count() const noexcept { return count_.load(std::memory_order_acquire); }
    void Clear() noexcept {  // (a new game boot: the old nodes are another heap's)
        for (auto& n : nodes_) {
            n.store(0, std::memory_order_relaxed);
        }
    }

private:
    std::atomic<std::uint32_t> count_{0};
    std::array<std::atomic<std::uint64_t>, kRingNodes> nodes_{};
};
inline HostRing& TheHostRing() {
    static HostRing ring;
    return ring;
}

// The key a node has in every game: its actor's placement hash, its
// length (x10, rounded) and where it sits in its graph (node - graph,
// 0 when that is not a small offset), mixed.
std::uint64_t KeyOf(std::uint64_t placement_hash, float duration_frames, std::uint64_t node,
                    std::uint64_t graph);

// The pause counters' owners worth following.
bool IsPlatformOwner(std::string_view name);

// The nodes the ring named whose actor is not a platform (an enemy's, an
// NPC's, Link's), so they are not read every look. Each is kept with its
// actor: it is looked at again when the node now belongs to another actor
// (the heap gave its memory to a new node), when its actor left the walk,
// and after a scene change.
class OtherNodes {
public:
    static constexpr std::size_t kMax = 4096;  // then all are forgotten
    void Add(std::uint64_t node, std::uint64_t actor);
    // A node the ring named again, with the actor it belongs to now:
    // true to skip it. Another actor than the one kept: forgotten, false.
    bool Skip(std::uint64_t node, std::uint64_t actor_now);
    // Forgets the nodes whose actor is not alive (not in the walk).
    void Keep(const std::function<bool(std::uint64_t actor)>& alive);
    void Clear() { nodes_.clear(); }
    std::size_t Size() const { return nodes_.size(); }
    bool Contains(std::uint64_t node) const { return nodes_.contains(node); }

private:
    std::unordered_map<std::uint64_t, std::uint64_t> nodes_;  // node -> actor
};

// One look at a node, from the hook's read of the game.
struct Seen {
    std::uint64_t node = 0;
    std::uint64_t key = 0;
    std::uint64_t owner_hash = 0;
    std::string owner_name;
    float remaining = 0.0f;  // frames
    float duration = 0.0f;   // frames
};

class CounterSync {
public:
    static constexpr float kPeriodSlack = 0.08f;
    static constexpr float kOffS = 0.3f;
    static constexpr std::uint64_t kSendEveryUs = 3'000'000;
    static constexpr std::uint64_t kStaleUs = 10'000'000;      // another game's line this old is not used
    static constexpr std::uint64_t kMinPeriodUs = 300'000;
    static constexpr std::uint64_t kMaxPeriodUs = 120'000'000;
    static constexpr std::size_t kMaxLooping = 64;              // per scene
    static constexpr int kLoopMisses = 3;

    struct Write {
        std::uint64_t node = 0;
        std::uint64_t key = 0;
        float remaining = 0.0f;  // frames
        float was = 0.0f;
    };
    struct Out {
        std::vector<coop::protocol::Message> send;
        std::vector<Write> writes;
        std::vector<std::string> notes;
    };

    // Present thread (the glue queues the network's messages).
    void Receive(const coop::protocol::PlatformCounter& message);

    // Present thread, every look (a few frames): the nodes seen now.
    // `probe_only` (LICHII_COOP_PLATFORMS=probe): what would be written is
    // logged, not given as a write, and the node's state stays as read.
    void Tick(const std::string& scene, std::uint8_t self, std::uint64_t server_us,
              const std::vector<Seen>& seen, bool probe_only, Out& out);

    void Reset();

    // Placement hashes of the actors with a looping counter.
    std::set<std::uint64_t> LoopingOwners() const;
    // Placement hashes of every platform actor whose pause counter was seen
    // in this scene, looping yet or not: the clock-word path
    // (coop_platforms.h) leaves them alone, so the two never fight (the
    // 2026-11-12 run: it took a physics place of the Dodongo lifts, and
    // the game put it back).
    const std::set<std::uint64_t>& CounterOwners() const { return counter_owners_; }
    // The node's loop (microseconds; 0: not looping), for tests and logs.
    std::uint64_t CycleOf(std::uint64_t key) const;
    std::size_t Writes() const { return writes_; }
    bool Looping(std::uint64_t key) const;

private:
    struct Node {
        std::uint64_t key = 0;
        std::uint64_t owner_hash = 0;
        std::string owner_name;
        // The frames left at the last look. The game's counters go below
        // zero when a pause ends (-0.6 in the 2026-11-12 run), so a value
        // below zero is no "never looked" mark: `looked` is.
        float last_remaining = 0.0f;
        bool looked = false;
        std::uint64_t start_us = 0;           // server time of the last start
        std::deque<std::uint64_t> periods;    // the last four
        std::uint64_t cycle_us = 0;           // the loop: one period, or two that alternate
        // The start every game compares (server time): the last start, or
        // with two waits the last one that ended the longer period, so two
        // games never line up one's top wait with the other's bottom wait.
        std::uint64_t ref_us = 0;
        int misses = 0;                       // starts in a row that fit no loop
        std::uint32_t starts = 0;
        std::uint64_t written_start_us = 0;   // the start we moved already
        bool looping = false;
        bool logged = false;
    };
    struct Theirs {
        std::uint64_t start_us = 0;   // their last start (server time)
        std::uint32_t period_us = 0;
        std::uint32_t starts = 0;
        std::uint64_t got_us = 0;
    };

    std::string scene_;
    std::map<std::uint64_t, Node> nodes_;                               // by node address
    std::map<std::uint64_t, std::map<std::uint8_t, Theirs>> theirs_;    // by key, then player
    std::uint64_t last_send_us_ = 0;
    std::size_t writes_ = 0;
    std::set<std::uint64_t> counter_owners_;
};

// The loop of a node from its last periods between starts (oldest first;
// microseconds): the last period when the last two agree, the last two
// added when the last four alternate (A, B, A, B), else 0.
std::uint64_t LoopOf(const std::deque<std::uint64_t>& periods, float slack);

// The gap (seconds) from their start to ours, the short way round the
// period: positive when ours began later.
float PhaseGap(std::uint64_t ours_us, std::uint64_t theirs_us, std::uint64_t period_us);

}  // namespace CoopCounters
