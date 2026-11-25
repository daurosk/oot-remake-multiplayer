// SPDX-License-Identifier: GPL-3.0-or-later
//
// A copy boss throws what its owner's boss prepared (row 222 part 3, Twinrova's bullets; docs/dungeons-boss.md
// "Shooter requests"). No emulator code: tested in tests/lichii/boss_shots_test.cpp.
//
// The game's ranged attacks are two things. The action (the verb the native boss sync carries, coop_boss_native.h)
// plays the throw and lets go of a shootable actor; which shootable is ready in the boss's shooter was asked before
// it, by the shooter's create request main+0x374d20 (x0 the shooter, w1 the shootable's name hash, w2 how many): it
// finds the shootable's entry in the shooter's table and raises its asked count. Its three callers (build
// e0f3ccfa): the attack sequences' start step game__bsa__ActionSeqProcShootableActorCreate (main+0x746b48), the AI
// node OneShotShooterRequestCreateShootableActors (main+0x293ff98) and the shooter itself (main+0x378754). All of
// them run from the game's own AI, so on a copy the request came from the copy's own choice of move: the owner's
// move was put in and played, with the bullet of the variation the copy's own AI happened to choose (another one:
// nothing thrown for the hand that plays; none: nothing prepared at all). Run 20261124-161320: 3 of 4 shots lost.
//
// The function is wrapped (Hookshot's hook table, HOS_COOP_WRAP_SHOOTER_REQUEST, patch 0044):
// - the owner's game lets each request of a shared boss run and keeps the ones that found their shootable; they go
//   out in the boss's own samples (OBJECT_SAMPLE component 25, coop::protocol::BossShots), from the start (an empty
//   list tells the copies their owner shares them);
// - a copy game skips its own boss's requests once the owner's list has come, and makes the owner's requests through
//   the same function, called by our caller on the game's calc thread before the boss table's put-ins, in the
//   owner's order, each once.
// Until the owner's list comes (an older owner) the copy's own requests run, as before. A shooter that is no shared
// boss's (Link's, any other enemy's, solo play) is never touched.
// The owner's list holds only fresh requests (kFreshRounds walks; its count goes on): a copy that starts (a join, a
// hand-over) makes the ones in the list, which are the fight's current move, and nothing older.
// Hand-over: the new owner's own AI asks from then on (its list starts empty); a game that became a copy starts
// with the new owner's list. What was queued from the old owner is dropped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_caller_orders.h"
#include "coop_puppet.h"

namespace CoopBossShots {

using coop::protocol::BossShot;
using coop::protocol::BossShots;

namespace addr {
// The shooter's create request, build e0f3ccfa: a leaf (no frame), returns 1 when the shootable was found.
inline constexpr std::uint64_t kRequestCreate = 0x374d20;  // main+
// An actor's shooter: [[actor + 0x270] + 0x3d8] (the component table, as main+0x746b30 reads it for the request).
inline constexpr std::uint64_t kActorComponents = 0x270;
inline constexpr std::uint64_t kComponentShooter = 0x3d8;
}  // namespace addr

// Guest memory for the wrap's side (the game's thread); false: not read.
using Reader = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;

// The actor's shooter (0: none, or not read).
std::uint64_t ShooterOf(const Reader& read, std::uint64_t actor);

// What the game's function would do for these arguments: nothing for no hash or a low byte of 0 (never kept).
constexpr bool Asks(std::uint32_t hash, std::uint32_t num) { return hash != 0 && (num & 0xffu) != 0; }

enum class Role : std::uint8_t { owner, copy };

// A boss the walk saw this round (coop_objects.cpp, with the native boss list).
struct Member {
    std::uint64_t actor = 0;
    Role role = Role::owner;
    std::uint8_t owner = 0;  // the owning player's number (a new one: the copy starts again)
    std::string name;
};

struct Counts {
    std::uint64_t owner_kept = 0;    // requests our own boss made that found their shootable (sent)
    std::uint64_t copy_skipped = 0;  // a copy's own requests not run (the owner's come)
    std::uint64_t copy_own_ran = 0;  // a copy's own requests run before the owner's list came
    std::uint64_t delivered = 0;     // owner's requests made on a copy
    std::uint64_t missed = 0;        // owner's requests the copy's shooter did not know (the function answered 0)
    std::uint64_t dropped = 0;       // owner's requests dropped (too many queued, no shooter, or older than the list)
    std::uint64_t restarts = 0;      // a copy started again with the owner's list (its count went back: a new owner)
};

// The planner, without game memory: which of the owner's requests a copy makes next (tested). `made`: the newest
// seq already queued (nothing: a start, the whole list). Every request newer than it, in order; `lost` gets how
// many the list no longer holds.
std::vector<BossShot> Plan(const BossShots& owner, std::optional<std::uint32_t> made, std::uint32_t* lost = nullptr);

class Table {
public:
    static constexpr int kCallerOrder = CoopCallerOrders::kBossShots;
    static constexpr std::size_t kMaxQueued = 16;
    // How many walks (Publish) one of our requests stays in the list we send: long enough for every copy to get it
    // (a new one is sent at once), short enough that a copy starting later does not make a finished move's.
    static constexpr std::uint64_t kFreshRounds = 90;

    void SetOn(bool on);
    bool On() const;

    // The walk's bosses this round. One no longer listed is forgotten; a role or owner change starts it again.
    void Publish(std::span<const Member> members);

    // The wrap, phase 0, on the game's thread: x0 the shooter, w1 the hash, w2 how many. True: skip it (the game's
    // function does not run). `own_call`: our caller's own request (always runs).
    bool OnRequest(std::uint64_t shooter, std::uint32_t hash, std::uint32_t num, bool own_call, const Reader& read);
    // The wrap, phase 1: a request that ran, and whether it found its shootable. Ours: kept and sent.
    void OnRan(std::uint64_t shooter, std::uint32_t hash, std::uint32_t num, bool found, bool own_call,
               const Reader& read);

    // Ours: the requests our boss made, for its samples (nothing: not ours, or off).
    std::optional<BossShots> OwnerShots(std::uint64_t actor) const;
    // A copy's: the owner's requests from its samples (coop_objects_sync.cpp BossShotsIn).
    void Receive(std::uint64_t actor, const BossShots& shots);

    // The caller (coop_callers.h contract): the owner's requests made on our copies, through the game's function.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main_base);

    // A new game process: every boss and any call in flight are dropped. Step does the same when main_base changes.
    void Reset();

    Counts GetCounts() const;
    std::vector<std::string> TakeNotes();

private:
    struct Entry {
        Member member;
        BossShots mine;                  // ours: what our boss asked (seq never goes back in this game)
        std::deque<std::uint64_t> mine_rounds;  // ours: the walk each of mine.recent was kept in
        bool owner_came = false;         // a copy: the owner's list has come (its own requests are skipped)
        std::uint32_t queued_up_to = 0;  // a copy: the newest owner seq queued
        std::deque<BossShot> queue;      // a copy: the owner's requests to make, oldest first
    };
    void Note(std::string text);
    Entry* OfShooter(std::uint64_t shooter, const Reader& read);  // (mutex_ held)

    mutable std::mutex mutex_;
    bool on_ = false;
    std::map<std::uint64_t, Entry> entries_;  // by actor
    Counts counts_{};
    std::vector<std::string> notes_;
    std::uint64_t main_base_ = 0;
    std::uint64_t round_ = 0;  // walks (Publish) so far
    bool pending_ = false;  // a request of ours is in flight
    std::uint64_t pending_actor_ = 0;
};

Table& TheTable();

}  // namespace CoopBossShots
