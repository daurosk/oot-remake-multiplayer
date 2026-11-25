// SPDX-License-Identifier: GPL-3.0-or-later
//
// A copy boss shows the owner's shots (Twinrova hand-over; docs/dungeons-boss.md "Shots on the owner's word").
// No emulator code: tested in tests/lichii/boss_native_test.cpp.
//
// Twinrova's bullets are split shots (coop_enemy_shots.h IsSplitShot): every game keeps its own copy boss's bullet,
// and the owner's is not sent as an object. A copy fired only when its own AI ran the owner's move in time. After
// hand-overs its AI is out of phase with the owner's (NEXT49: 4 of 11 copy attacks fired no bullet).
//
// Her bullets are made once at room load, each through a create request whose handle the game keeps, and stay kept
// (actor state 3). Her AI fires one by waking it (state 4) and it goes back to 3 when it ends: no create and no call
// of the shoot code in the fight (NEXT65, NEXT66). So:
// - Every game keeps the handles of its Twinrova bullets as they are made (CREATE_REQUEST, KeepBullet) and reads
//   each one's actor at the next frames, before the game's take clears the handle's slot (NEXT68: read later, none
//   gave an actor).
// - The owner's game counts each wake of one of them as a shot and sends the bullets awake now, with their places,
//   in the boss's samples (OBJECT_SAMPLE component 29, coop::protocol::BossShots).
// - A copy game wakes a kept bullet of the same name for each owner shot, as the game's own wake does (state 3 to 4,
//   a write), and puts it on the owner's place every frame, so it moves where the owner's does and still hits our own
//   Link. When the owner's ends, ours is put back to 3. Once the owner's first sample has come, a wake by our own AI
//   is put back at once, so each owner shot is one bullet in every game.
// Until the owner's first sample comes (a new copy, an older owner) the copy's own wakes stand, as before.
// Hand-over: the new owner's own AI fires from then on; a game made a copy starts again from the new owner's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_caller_orders.h"
#include "coop_puppet.h"

namespace CoopBossShots {

using coop::protocol::BossShot;
using coop::protocol::BossShots;

namespace addr {
// Build e0f3ccfa.
inline constexpr std::uint64_t kRequestActor = 0x08;   // a create request: its actor (coop_actor_events.h)
inline constexpr std::uint64_t kActorState = 0x2c;     // u32: 3 made and kept, 4 present (awake)
inline constexpr std::uint64_t kActorPosition = 0x31c; // 3 x f32
inline constexpr std::uint32_t kStateKept = 3;
inline constexpr std::uint32_t kStateAwake = 4;
}  // namespace addr

// The bosses whose shots go this way (Twinrova, as coop_boss_native.h HoldsOwnStarts).
bool Covers(std::string_view boss_name);
// A bullet of theirs (its create is kept: KeepBullet).
bool IsBullet(std::string_view name);

enum class Role : std::uint8_t { owner, copy };

// A boss the walk saw this round (coop_objects.cpp, with the native boss list).
struct Member {
    std::uint64_t actor = 0;
    Role role = Role::owner;
    std::uint8_t owner = 0;  // the owning player's number (a new one: the copy starts again)
    std::string name;
};

struct Counts {
    std::uint64_t owner_kept = 0;    // our boss's wakes (sent)
    std::uint64_t copy_stopped = 0;  // a copy's own AI's wakes put back (the owner's come)
    std::uint64_t fired = 0;         // owner's shots a copy woke
    std::uint64_t dropped = 0;       // owner's shots not woken (no kept bullet of the name free, or missed)
    std::uint64_t restarts = 0;      // a copy started again (a new owner)
    std::uint64_t missed = 0;        // bullets whose slot was taken or used again before their actor was read
};

class Table {
public:
    static constexpr int kCallerOrder = CoopCallerOrders::kBossShots;
    static constexpr std::uint64_t kKeepUs = 500'000;  // an empty list is sent this long after the last bullet ends
    static constexpr std::size_t kMaxBullets = 32;
    static constexpr std::uint32_t kBindSteps = 30;  // steps a kept slot may stay empty before it is given up

    void SetOn(bool on);
    bool On() const;

    // CREATE_REQUEST of a bullet (IsBullet), on the game's thread: `handle` is where the game keeps its request.
    void KeepBullet(std::uint64_t handle, std::string_view name);

    // The walk's bosses this round. One no longer listed is forgotten; a role or owner change starts it again.
    void Publish(std::span<const Member> members);

    // Ours: the bullets awake now, for its samples (nothing: not ours, or none for kKeepUs).
    std::optional<BossShots> OwnerShots(std::uint64_t actor, std::uint64_t now_us);
    // A copy's: the owner's bullets from its samples (coop_objects_sync.cpp BossShotsIn).
    void Receive(std::uint64_t actor, const BossShots& shots);

    // The caller (coop_callers.h contract), every frame on the game's thread: reads our bullets' wakes and, for a
    // copy, wakes, moves and puts back its bullets. It never asks for a call.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main_base, std::uint64_t now_us);

    // A new game process: every boss and bullet is dropped.
    void Reset();

    Counts GetCounts() const;
    std::vector<std::string> TakeNotes();

private:
    struct Bullet {
        std::uint64_t handle = 0;
        std::string name;
        bool awake = false;          // ours: awake when last read
        std::uint32_t seq = 0;       // ours: its wake's seq; a copy: the owner shot it shows (0: none)
        coop::protocol::Vec3 position{};
        std::uint64_t actor = 0;     // its actor, read while the slot named its request (0: not yet)
        std::uint64_t request = 0;   // the first request its slot held (0: none yet)
        std::uint32_t waits = 0;     // steps with no request in its slot yet
        bool missed = false;         // its slot changed before its actor was read: never read again
    };
    struct Entry {
        Member member;
        std::uint32_t seq = 0;         // ours: the newest wake's seq (never goes back in this game)
        std::uint64_t ended_us = 0;    // ours: when the last bullet ended (an empty list is sent kKeepUs after)
        bool owner_came = false;       // a copy: the owner's samples have come (its own wakes are put back)
        std::uint32_t through = 0;     // a copy: the newest owner seq woken or dropped
        BossShots owner;               // a copy: the owner's newest list
    };
    void Note(std::string text);
    void DropGame();  // (mutex_ held)
    void StepOwner(CoopPuppet::Guest& guest, Entry& e, std::uint64_t now_us);
    void StepCopy(CoopPuppet::Guest& guest, Entry& e);
    std::uint64_t Live(CoopPuppet::Guest& guest, Bullet& b, std::uint32_t state);
    void ReadActors(CoopPuppet::Guest& guest);

    mutable std::mutex mutex_;
    bool on_ = false;
    std::map<std::uint64_t, Entry> entries_;  // by actor
    std::vector<Bullet> bullets_;
    Counts counts_{};
    std::vector<std::string> notes_;
    std::uint64_t main_base_ = 0;
};

Table& TheTable();

}  // namespace CoopBossShots
