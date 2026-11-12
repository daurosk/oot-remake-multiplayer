// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue 149 (docs/dungeon.md "Order puzzles"): the Scrub Brothers (hit 2, 3,
// 1) counted per game, so players could not split the order. Now a brother
// hit in one game is told to the others, and each other game gives its own
// manager the same hit, the game's own way.
//
// Read from main (build e0f3ccfaf50065ae):
// - A hit brother's AI node OneShotSendScrubDekuBrothersDefeatedOrderMessage
//   (vtable main+0x4a2f928, execute 0x292e810, send 0x292e9a0) finds the
//   manager through the ScrubDekuLink and calls
//     0x2838c50(sender, -, header, payload)
//   sender  = the brother actor + 0x3e0 (its transceiver; +8 = its id, u32)
//   header  = {u32 the manager's transceiver id (manager actor + 0x3e8),
//              u32 0x77440000 (the "defeated order" message), u8 0}
//   payload = 0x18 bytes, copied: u32 the brother's DefeatedOrder, u8 a bool
//             input of the node, the rest 0.
//   The send only reads the sender's id ([sender + 8]) and queues the message.
// - The manager's node (ExecuteScrubDekuManagerAI, vtable main+0x49d2a68,
//   0x2837520) takes message 0x77440000 into the record of the brother whose
//   transceiver id sent it (node +0xa0/+0xb4/+0xc8, the id at +0x10 of each),
//   and the manager's own logic decides from there (stun, all escape, the
//   solve). So a hit sent as our own brother counts as if it was hit here.
//
// Here: the hit is seen by its animation (AS slot 0 turning to a hit
// command), sent as WORLD_HIT with the brother's placement hash (no new
// message; the server relays it to the scene), and a WORLD_HIT for a brother
// from another game is sent to our manager through the cave.
// Pure over CoopPuppet::Guest: tested in tests/lichii/scrubs_test.cpp.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"

namespace CoopScrubs {

namespace addr {
inline constexpr std::uint64_t kMalloc = 0x14c6c90;            // as coop_live_events.h
inline constexpr std::uint64_t kSend = 0x2838c50;              // (sender, -, header, payload)
inline constexpr std::uint64_t kTransceiver = 0x3e0;           // actor + : the actor's transceiver
inline constexpr std::uint64_t kTransceiverId = 0x3e8;         // actor + : its id (u32)
inline constexpr std::uint32_t kDefeatedOrderMessage = 0x77440000;
inline constexpr std::uint64_t kBlockSize = 0x40;
inline constexpr std::uint64_t kHeader = 0x00;   // in the block: id, type, u8 0
inline constexpr std::uint64_t kPayload = 0x10;  // in the block: 0x18 bytes
inline constexpr std::uint64_t kPayloadSize = 0x18;
} // namespace addr

// One brother, from the Banc files (InsideTheDekuTree_Static,
// KokiriForest_Child_Static, KokiriForest_Adult_Static): its placement
// hash, its manager's, and its DefeatedOrder (the blackboard's 1 when the
// placement gives none: the brother called Two).
struct Brother {
    std::uint64_t hash;
    std::uint64_t manager;
    std::uint32_t order;
    std::string_view name;
};
std::span<const Brother> Brothers();
const Brother* FindBrother(std::uint64_t hash);
bool IsManager(std::uint64_t hash);

// The AS commands a brother plays when it is hit (its interruptsmallhit
// module): "Paralysis_Start" (stunned, waits for the others), "SmallHit" (the
// last right hit: then Escape, Damage, Stop, and the talk when Link comes).
// LICHII_COOP_SCRUB_HIT_ANIMS=a,b replaces them. A command matches when it
// holds one of the names.
bool IsHitAnimation(std::string_view anim, const std::set<std::string>& names);
std::set<std::string> DefaultHitAnimations();
bool IsIdleAnimation(std::string_view anim);  // the brothers' own cycle (not logged)
std::set<std::string> ParseAnimations(std::string_view text);  // "a,b": empty parts dropped

// A brother or manager of the last objects walk.
struct Seen {
    std::uint64_t address = 0;
    std::uint64_t hash = 0;
    std::uint32_t id = 0;     // its transceiver id, read in the walk
    std::string animation;    // AS slot 0, empty if not read
    coop::protocol::Vec3 position{};
};

class Puzzle {
public:
    // A hit from another game waits at most this long for the cave.
    static constexpr std::uint64_t kWaitUs = 2'000'000;
    // Hits from another game are taken only once our brothers have been
    // here this long: the server's kept hits of an earlier try, sent when we
    // enter, never count.
    static constexpr std::uint64_t kSettleUs = 3'000'000;
    // After a hit from another game is given to our brother, its own hit
    // animation here (the manager's stun) is not sent back.
    static constexpr std::uint64_t kEchoUs = 4'000'000;
    // The walk's addresses are trusted for the cave this long.
    static constexpr std::uint64_t kFreshUs = 1'500'000;
    static constexpr int kMaxAnimLines = 200;

    explicit Puzzle(std::set<std::string> hit_animations = DefaultHitAnimations())
        : hit_animations_(std::move(hit_animations)) {}

    struct Out {
        std::vector<coop::protocol::WorldHit> send;
        std::vector<std::string> notes;
    };
    // Once per objects walk with the brothers and managers seen.
    void Look(const std::string& scene, const std::vector<Seen>& seen,
              const std::optional<coop::protocol::Vec3>& player, std::uint64_t now_us, Out& out);
    // A WORLD_HIT from another game. False when it is not a brother's (for
    // the world sync); true when taken here (queued or dropped, see notes).
    bool Receive(const coop::protocol::WorldHit& h, std::uint64_t now_us);
    // Issue 144: our Link's event (a brother's talk), once a frame. While it
    // runs, and kEventTailUs after (the talk's run away; as coop_scrub_talk.h),
    // hits from other games wait (the 2 s wait starts again when it ends), so
    // no message reaches our manager in the middle of our talk.
    void SetEvent(bool in_event, std::uint64_t now_us);
    static constexpr std::uint64_t kEventTailUs = 2'000'000;
    // A manager whose last brother in order (One, order 3) played SmallHit
    // here this visit (the last right hit, proven in deku-scrubs-solo):
    // solved; later hits from other games for it are dropped (issue 144).
    bool Solved(std::uint64_t manager) const { return solved_.contains(manager); }

    // CoopSvc::Caller's contract (as CoopLiveEvents::Requester).
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main, std::uint64_t now_us);
    std::vector<std::string> TakeNotes();
    void Reset();

    std::uint64_t Block() const { return block_; }
    std::size_t Pending() const { return pending_.size(); }

private:
    struct Here {
        std::uint64_t address = 0;
        std::uint32_t id = 0;
    };
    struct Want {
        std::uint64_t hash = 0;
        std::uint8_t player = 0;
        std::uint64_t got_us = 0;
    };
    enum class Next { none, malloc, send };

    std::set<std::string> hit_animations_;
    std::string scene_;
    std::uint64_t first_seen_us_ = 0;  // our brothers first seen in this scene visit
    std::uint64_t look_us_ = 0;
    std::map<std::uint64_t, Here> here_;                 // by hash: brothers and managers of the last walk
    std::map<std::uint64_t, std::string> animation_;     // by address: the last AS command
    std::map<std::uint64_t, std::uint64_t> echo_until_;  // by brother hash
    std::set<std::uint64_t> solved_;                     // managers solved here (issue 144)
    bool in_event_ = false;                              // our Link's event + tail (issue 144)
    std::uint64_t last_event_us_ = 0;
    std::deque<Want> pending_;
    std::uint64_t block_ = 0;
    Next next_ = Next::none;
    Want current_;
    bool malloc_failed_ = false;
    int anim_lines_ = 0;
    std::vector<std::string> notes_;
};

} // namespace CoopScrubs
