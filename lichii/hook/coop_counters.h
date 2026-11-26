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
// - Lifts whose moves do not take the same time in every game (report 138:
//   Dodongo's Cavern's lifts are physics sliders; a rider or a slower frame
//   makes a move longer, and the games' periods came out 21, 26, 29 and 54 s
//   for one lift) never agree on a period, so the rule above never followed
//   them. So:
//   - Pause by pause (AlignPause), when our wait is known: the leader is the
//     lowest player number of the games that send this counter's waits (the
//     same choice in every game, whatever their counts). A counter with no
//     wait known, or a game on an older module, keeps the period rule above.
//   - Each wait on its own key: a pause starts at the lift's home wait (within
//     kHomeM of its placement, CoopRoomObjects, the same in every game) or its
//     far one, and the pauses are sent per wait (WaitKey). So one counter that
//     times both waits is never lined up top on bottom. A lift with no
//     placement known is never lined up pause by pause: the period rule only.
//   - A node seen starting kPauseStarts times is sent (looping or not; a
//     switch's one move never is). A start that goes into the message is sent
//     at once, at most one message every kMinSendGapUs.
//   - AlignPause: our pause of a wait ends when the leader's pause of that
//     wait does, from the leader's actual start, never a guess from its last
//     period (a physics lift's period changes by seconds). When the leader's
//     start of this pause has not come yet (we began first, or it is more
//     than a pause ahead), our pause is held from running out until it comes
//     (at most kMaxHoldUs), then ends with the leader's.
//   - A mover its own player drives (a rider's lift, a player sensor:
//     Seen::driven) is never sent or written: each game's own Link moves it.
//     A mover with a switch's or an event's input (Seen::switched: Dodongo's
//     shortcut lift, IsShortCutMove) runs the same logic in every game, since
//     that state is shared: it is lined up once it loops on its own (row 164:
//     before, it was never lined up and drifted for good).
//   - Row 164, pause menus and late arrivals: a game whose player is away (a
//     pause or quick-switch menu, a load, a cutscene: PLAYER_STATUS) leads
//     nobody and writes nothing, since its lifts stand still while the server
//     clock runs (before, every other game held its lifts for it, then ran
//     free). A game that came back, or came into the scene, leads only after
//     kSettleUs of sending (until then it follows: lined up from its first
//     pause, not its third), so it never pulls the others to its own phase.
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
    // Which wait the platform is at (CoopRoomObjects's placement, the same in every game): 0 within kHomeM of
    // it, 1 away; -1 when its placement is not known. `home_m`: how far from it (logged at the first pauses).
    int wait = -1;
    float home_m = -1.0f;
    // Moved by its own player (a rider, a player sensor: CoopRoomObjects::PlayerDriven): never sent or written.
    bool driven = false;
    // Row 164: an input from a switch or an event every game shares (CoopRoomObjects's `why`, not the player's):
    // sent and lined up only once it loops on its own (a switch's moves never do).
    bool switched = false;
};

// Metres from its placement within which a platform is at its home wait.
inline constexpr float kHomeM = 0.5f;

// The key a node's pauses of one wait are sent under (`wait` 0 or 1).
std::uint64_t WaitKey(std::uint64_t key, int wait);

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
    static constexpr std::uint32_t kPauseStarts = 3;            // starts before a node's pauses are shared
    static constexpr std::uint64_t kMinSendGapUs = 250'000;     // a start sent at once: at most this often
    static constexpr std::uint64_t kMaxHoldUs = 60'000'000;     // a pause held for the leader's start: at most
    static constexpr float kKeepFrames = 10.0f;                 // a held pause is kept above this many frames
    // Row 164: a game leads only once it has sent its counters this long without being away (a pause menu, a
    // load, a cutscene) in this scene; until then it follows.
    static constexpr std::uint64_t kSettleUs = 45'000'000;
    // Row 164: a pause lined up on the leader's is left alone when it ends this close to the leader's (seconds;
    // was kOffS: 0.3 s of a moving lift is a visible step).
    static constexpr float kAlignOffS = 0.15f;

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
    // `away`: the players whose game stands still now (PLAYER_STATUS paused, in_menu, loading, in_cutscene),
    // ourselves included (row 164).
    void Tick(const std::string& scene, std::uint8_t self, std::uint64_t server_us,
              const std::vector<Seen>& seen, bool probe_only, Out& out,
              const std::set<std::uint8_t>& away = {});

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
        bool started_now = false;             // a start this tick: sent at once
        int wait = 0;                         // the wait of the last start (0 when not known)
        bool wait_known = false;
        std::uint64_t aligned_start_us = 0;   // the pause AlignPause is done with
        std::uint64_t held_start_us = 0;      // the pause held for the leader's start (said once)
        int home_said = 0;                    // pauses whose distance from the placement was logged
        std::array<std::uint64_t, 2> wait_start{};   // the last start of each wait
        std::array<std::uint64_t, 2> wait_period{};  // between two starts of the same wait
        bool switched = false;                // Seen::switched: shared only while it loops
        bool looping = false;
        bool logged = false;
    };
    struct Theirs {
        std::uint64_t start_us = 0;   // their last start (server time)
        std::uint32_t period_us = 0;
        std::uint32_t starts = 0;
        std::uint64_t got_us = 0;
    };

    // A node whose pauses are shared: looping, or seen starting kPauseStarts times.
    static bool Shared(const Node& n);
    // The game every game follows pause by pause: the lowest number of the games that send this node's
    // waits, ourselves included (the same in every game). PR 279 review M1: a game whose newest start of this
    // node's waits is older than kLeaderStalePeriods of its period (its lift stopped starting: a pause menu, it
    // left) is no leader, although it keeps resending that old start.
    std::uint8_t Leader(const Node& n, std::uint8_t self, std::uint64_t server_us) const;
    static constexpr std::uint64_t kLeaderStalePeriods = 2;
    // Row 164: settled (sent without being away for kSettleUs in this scene), the same answer in every game: the
    // times are the server times of that player's messages.
    bool Settled(std::uint8_t player, std::uint64_t server_us) const;
    // PR 279 review M1: a pause held for the leader's start waits at most until that start is due: the leader's
    // last start of this wait + kHoldDuePeriods of its period + the pause (and never past kMaxHoldUs).
    static constexpr double kHoldDuePeriods = 1.25;
    // This pause alone, lined up on the leader's same wait: true when written.
    bool AlignPause(Node& n, const Seen& s, std::uint8_t leader, std::uint64_t server_us, bool probe_only,
                    Out& out);
    // A write of `target` frames left for this pause, with its note.
    void WritePause(Node& n, const Seen& s, float target, float gap, std::uint8_t leader, const char* how,
                    bool probe_only, Out& out);

    std::string scene_;
    std::map<std::uint64_t, Node> nodes_;                               // by node address
    std::map<std::uint64_t, std::map<std::uint8_t, Theirs>> theirs_;    // by key, then player
    std::uint64_t last_send_us_ = 0;
    std::size_t writes_ = 0;
    std::set<std::uint64_t> counter_owners_;
    // Row 164: the players away now (Tick's `away`), and since when each has sent counters in this scene without
    // being away (its first message's server time after that; ours: our first send).
    std::set<std::uint8_t> away_;
    std::map<std::uint8_t, std::uint64_t> present_since_;
};

// The loop of a node from its last periods between starts (oldest first;
// microseconds): the last period when the last two agree, the last two
// added when the last four alternate (A, B, A, B), else 0.
std::uint64_t LoopOf(const std::deque<std::uint64_t>& periods, float slack);

// The gap (seconds) from their start to ours, the short way round the
// period: positive when ours began later.
float PhaseGap(std::uint64_t ours_us, std::uint64_t theirs_us, std::uint64_t period_us);

}  // namespace CoopCounters
