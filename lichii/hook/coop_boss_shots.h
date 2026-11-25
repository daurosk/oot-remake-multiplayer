// SPDX-License-Identifier: GPL-3.0-or-later
//
// The owner's Twinrova shots known in every game (Twinrova hand-over; docs/dungeons-boss.md "Shots on the owner's
// word"). Reads only: nothing is written to a bullet and no bullet is woken or put back here.
// No emulator code: tested in tests/lichii/boss_native_test.cpp.
//
// Twinrova's bullets are split shots (coop_enemy_shots.h IsSplitShot): every game keeps its own copy boss's bullet,
// and the owner's is not sent as an object. A copy fired only when its own AI ran the owner's move in time. After
// hand-overs its AI is out of phase with the owner's (NEXT49: 4 of 11 copy attacks fired no bullet).
//
// Her bullets are made once at room load, each through a create request whose handle the game keeps, and stay kept
// (actor state 3) until her AI wakes one (state 4): no create and no call of the shoot code in the fight (NEXT65,
// NEXT66). What wakes it is her Shooter component (prepare, shoot, request sleep), which also moves the actor's
// flags, the actor manager's queue and the Shooter's own record. A write of the state alone leaves those three in the
// old state, so there is none here. The copy's wake through the game's own calls is not in yet (what it needs is
// read in game first). Until then the copy's own AI fires its own bullets, as before. So:
// - Every game keeps the handles of its Twinrova bullets as they are made (CREATE_REQUEST, KeepBullet) and reads
//   each one's actor at the next frames, before the game's take clears the handle's slot (NEXT68: read later, none
//   gave an actor). Looked for from the first session on (the caller is added then): a rejoin in her room has them,
//   a first join there has none, and so sends and counts no shot until her room is made again.
// - A kept actor is used only while it still is that bullet: the actor class's vtable and the bullet's name are read
//   before every use, and a gone state (7 to 11) or another actor in its place drops the bullet.
// - The owner's game counts each wake of one of them as a shot and sends the bullets awake now, with their places,
//   in the boss's samples (OBJECT_SAMPLE component 29, coop::protocol::BossShots).
// - A copy game counts the owner's shots once each and pairs them with its own AI's wakes (Counts): which of the
//   owner's shots had no bullet here, and which of ours the owner never fired.
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
inline constexpr std::uint64_t kActorState = 0x2c;     // u32: 3 made and kept, 4 present (awake), 6 asleep
inline constexpr std::uint64_t kActorPosition = 0x31c; // 3 x f32
inline constexpr std::uint32_t kStateKept = 3;
inline constexpr std::uint32_t kStateAwake = 4;
inline constexpr std::uint32_t kStateAsleep = 6;
inline constexpr std::uint32_t kStateGoneFirst = 7;  // 7 to 11: being deleted (main+0x37cca0)
inline constexpr std::uint32_t kStateGoneLast = 11;
}  // namespace addr

// The bosses whose shots go this way (Twinrova, as coop_boss_native.h HoldsOwnStarts).
bool Covers(std::string_view boss_name);
// A boss of her room: she and, before she is listed, Koume and Kotake. A room left in their phase is left all the same.
bool RoomBoss(std::string_view boss_name);
// A bullet of theirs (its create is kept: KeepBullet).
bool IsBullet(std::string_view name);

enum class Role : std::uint8_t { owner, copy };

// A boss the walk saw this round (coop_objects.cpp, with the native boss list).
struct Member {
    std::uint64_t actor = 0;
    Role role = Role::owner;
    std::uint8_t owner = 0;  // the owning player's number (a new one: the copy starts again)
    std::string name;
    std::uint16_t epoch = 0;  // the ownership's: it rises with every change, also one back between two walks
};

struct Counts {
    std::uint64_t owner_kept = 0;  // our boss's wakes (sent)
    std::uint64_t seen = 0;        // a copy: the owner's shots that came (each seq once)
    std::uint64_t unseen = 0;      // a copy: the owner's shots in no list that came (began and ended between two)
    std::uint64_t own = 0;         // a copy: its own AI's wakes
    std::uint64_t paired = 0;      // a copy: the owner's shots with an own wake of the same name within kPairUs
    std::uint64_t missing = 0;     // a copy: the owner's shots with none
    std::uint64_t extra = 0;       // a copy: own wakes with no owner's shot
    std::uint64_t restarts = 0;    // started again (a new owner)
    std::uint64_t lost = 0;        // bullets dropped: their actor is gone or another, their room left, or never read
    bool operator==(const Counts&) const = default;
};

class Table {
public:
    static constexpr int kCallerOrder = CoopCallerOrders::kBossShots;
    static constexpr std::uint64_t kKeepUs = 500'000;  // an empty list is sent this long after the last bullet ends
    static constexpr std::uint64_t kPairUs = 2'000'000;   // an own wake and the owner's shot are one within this
    static constexpr std::uint64_t kReadUs = 60'000'000;  // a create whose actor is not read by then failed
    static constexpr std::size_t kMaxBullets = 32;
    static constexpr std::size_t kMaxPending = 32;

    void SetOn(bool on);
    bool On() const;

    // CREATE_REQUEST of a bullet (IsBullet), on the game's thread: `handle` is where the game keeps its request.
    void KeepBullet(std::uint64_t handle, std::string_view name);

    // The walk's bosses this round, in `scene`. One no longer listed is forgotten. A role, owner or epoch change
    // starts it again. Another scene than her room's drops her room's bullets.
    void Publish(std::span<const Member> members, std::string_view scene);
    // Co-op ends here: no boss is owned or copied any more. The bullets stay known (a rejoin in her room has no new
    // creates), their shots do not; a room left meanwhile drops them as ever (their actors are gone).
    void Leave();

    // Ours: the bullets awake now, for its samples (nothing: not ours, or none for kKeepUs).
    std::optional<BossShots> OwnerShots(std::uint64_t actor, std::uint64_t now_us);
    // A copy's: the owner's bullets from its samples (coop_objects_sync.cpp BossShotsIn). A list older than one that
    // came changes nothing.
    void Receive(std::uint64_t actor, const BossShots& shots, std::uint64_t now_us);

    // The caller (coop_callers.h contract), every frame on the game's thread: reads our bullets' wakes. It never asks
    // for a call and writes nothing.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main_base, std::uint64_t now_us);

    // A new game process: every boss and bullet is dropped.
    void Reset();

    Counts GetCounts() const;
    std::size_t Bullets() const;  // known now
    std::vector<std::string> TakeNotes();

private:
    struct Bullet {
        std::uint64_t handle = 0;  // where the game keeps its request (0: its actor is read, the slot is others')
        std::string name;
        std::uint64_t actor = 0;     // its actor, read while the slot named it (0: not yet)
        std::uint64_t since_us = 0;  // the first frame it was looked for
        bool awake = false;          // awake when last read
        bool room = false;           // known while a boss of her room was listed (that scene left: dropped)
        std::uint32_t seq = 0;       // ours: its wake's seq
        coop::protocol::Vec3 position{};
    };
    struct Pending {
        std::string name;
        std::uint64_t at_us = 0;
    };
    struct Entry {
        Member member;
        std::uint32_t seq = 0;        // ours: the newest wake's seq in this ownership (0: none yet)
        std::uint64_t ended_us = 0;   // ours: when the last bullet ended (an empty list is sent kKeepUs after)
        bool owner_came = false;      // a copy: the owner's samples have come
        std::uint32_t through = 0;    // a copy: the newest owner seq counted
        std::vector<Pending> owners;  // a copy: the owner's shots with no own wake yet
        std::vector<Pending> owns;    // a copy: own wakes with no owner's shot yet
    };
    enum class Look : std::uint8_t { bullet, making, other };
    Look LookAt(CoopPuppet::Guest& guest, std::uint64_t actor, const std::string& name, std::uint32_t& state);
    void Note(std::string text);
    void DropGame();     // (mutex_ held, as all below)
    void ForgetShots();  // the bullets in flight are no one's shot; the next wake is
    void ReadActors(CoopPuppet::Guest& guest, std::uint64_t now_us);
    void ReadWakes(CoopPuppet::Guest& guest, Entry* e, std::uint64_t now_us);
    void Pair(std::vector<Pending>& with, std::vector<Pending>& wait, std::string_view name, std::uint64_t now_us);
    void Expire(Entry& e, std::uint64_t now_us);

    mutable std::mutex mutex_;
    bool on_ = false;
    std::map<std::uint64_t, Entry> entries_;  // by actor
    std::vector<Bullet> bullets_;
    std::string scene_;             // her room's, once a boss of it was listed there
    std::uint32_t newest_seq_ = 0;  // ours: never goes back in this process, over hand-overs and rejoins
    bool class_noted_ = false;
    Counts counts_{};
    std::vector<std::string> notes_;
    std::uint64_t main_base_ = 0;
};

Table& TheTable();

}  // namespace CoopBossShots
