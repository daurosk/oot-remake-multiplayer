// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 177: a shot sent back by a player whose game does not own it (docs/objects.md, "Shots sent back").
//
// An enemy's shot is the owner's game's (coop_enemy_shots.h); the others show a copy flown on the owner's path.
// In the game a shot is sent back by its own Shootable: Link's sword (a light ball) or his shield (an Octorok
// rock, a Deku Scrub's nut) hits it, and the shot's state goes to Reflected. A copy never does that (it is
// placed on the owner's path, Stationary), and on the owner's game the other player is a puppet with no
// sword or shield of the game's own, so nobody's hit but the owner's own Link's ever sent a shot back.
//
// The fix, with the game's own code at both ends:
//   - the game that shows the copy reads its copy's own hit lists (the Sensor component's, the same lists the
//     game's own reflect queries read) and finds our Link's hit there of the kind that shot is sent back by;
//     it says so to the shot's owner (DAMAGE, cause reflected, amount 0);
//   - the owner's game asks the game's own Shooter::RequestChangeStateReflected for its real shot (the
//     shooter's record of it, found by the shot's handle), on the calc thread; the shot flies back as it does
//     for the owner's own Link, and its copies follow its path as always.
// Pure over CoopPuppet::Guest; tested in tests/lichii/shot_reflect_test.cpp.
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_puppet.h"

namespace CoopShotReflect {

// What the game sends a shot back with: its ShootableParam (romfs, build e0f3ccfaf50065ae) says
// IsMeleeWeaponAttackReflect (and IsBottleWeaponAttackReflect: the bottle is a melee swing too) or IsGuardReflect.
enum class Way : std::uint8_t { none, melee, guard };
Way ReflectedBy(std::string_view shot);
const char* NameOf(Way way);

// Main offsets (build e0f3ccfaf50065ae; VA = 0x80020000 + offset).
namespace addr {
inline constexpr std::uint64_t kActorComponents = 0x270;
inline constexpr std::uint64_t kComponentSensor = 0x3b8;   // main+0x8d6320 reads it
inline constexpr std::uint64_t kComponentShooter = 0x3d8;  // main+0x2c24ba0 reads it
// A shot's own Shootable component (QueryShootableGetState reads it): valid byte +0x20 (bit 0), state s32 +0x24:
// 0 Sleep, 1 Prepare, 2 Shoot, 3 Stationary, 4 Reflected, 5 UsedExternally.
inline constexpr std::uint64_t kComponentShootable = 0x3d0;
inline constexpr std::uint64_t kShootableValid = 0x20;
inline constexpr std::uint64_t kShootableState = 0x24;
inline constexpr std::int32_t kStateReflected = 4;
// The Sensor's hit lists (QuerySensorHitIsHitTg / GetAtHitInfo / GanondorfChargeLightBallIsReflect):
// list +0x8 capacity, +0x10 entries, +0x18 count (the game reads min(count, capacity)).
inline constexpr std::uint64_t kSensorTgList = 0x68;  // hits it took, 0x1d0 bytes each
inline constexpr std::uint64_t kSensorAtList = 0x70;  // hits it dealt, 0xa0 bytes each
inline constexpr std::uint64_t kTgEntrySize = 0x1d0;
inline constexpr std::uint64_t kTgKind = 0x90;          // the attack's kind (byte)
inline constexpr std::uint64_t kTgHandleFlags = 0x95;   // bit 0: the attacker is the handle at +0x78, else +0x60
inline constexpr std::uint64_t kTgAttackerA = 0x60;
inline constexpr std::uint64_t kTgAttackerB = 0x78;
// The kinds the game's own reflect query (QuerySensorHitGanondorfChargeLightBallIsReflect, main+0x2adaca0)
// takes as a hit that sends a light ball back: kind <= 0x12 and bit set in this mask.
inline constexpr std::uint32_t kReflectKinds = 0x6000c;
inline constexpr std::uint64_t kAtEntrySize = 0xa0;
inline constexpr std::uint64_t kAtFlags = 0x0b;     // bit 0: guarded (GetAtHitInfo's IsGuard)
inline constexpr std::uint64_t kAtDefender = 0x78;  // the guarding actor's handle (IsGuardedByShield*)
// An actor handle (main+0x379ca0): +0x8 its holder, holder+0x40 the actor.
inline constexpr std::uint64_t kHandleHolder = 0x8;
inline constexpr std::uint64_t kHolderActor = 0x40;
// The Shooter's records (Shooter::RequestChangeStateReflected, main+0x2eb3bd0): map at +0x20 with +0x2c slots of
// 0x38 bytes, keyed by a u32 at +0 (0 empty); +0x8 count, +0x10 records of 0xf8 bytes; a record's shot handle +0x20.
inline constexpr std::uint64_t kShooterMap = 0x20;
inline constexpr std::uint64_t kShooterSlots = 0x2c;
inline constexpr std::uint64_t kMapEntrySize = 0x38;
inline constexpr std::uint64_t kEntryCount = 0x8;
inline constexpr std::uint64_t kEntryRecords = 0x10;
inline constexpr std::uint64_t kRecordSize = 0xf8;
inline constexpr std::uint64_t kRecordShot = 0x20;
inline constexpr std::uint64_t kRequestChangeStateReflected = 0x2eb3bd0;  // (shooter, u32 key, s32 index) -> bool
}  // namespace addr

// Bounds for lists read from the game (a list being rewritten while read).
inline constexpr std::uint32_t kMaxHits = 64;
inline constexpr std::uint32_t kMaxSlots = 1024;
inline constexpr std::uint32_t kMaxRecords = 64;

// The actor a handle at `handle` points to; 0 when none.
std::uint64_t HandleActor(CoopPuppet::Guest& guest, std::uint64_t handle);

// True when `shot`'s own hit lists hold a hit of our Link (`link`) of the kind the game sends this shot back
// with: melee, a reflect-kind hit taken from our Link; guard, a hit it dealt that our Link guarded.
bool OurHit(CoopPuppet::Guest& guest, std::uint64_t shot, std::uint64_t link, Way way);

// What `shot`'s hit lists hold now, one line (empty: both empty or unreadable), for the log: each taken hit's kind
// and attacker, each dealt hit's flags and defender.
std::string Describe(CoopPuppet::Guest& guest, std::uint64_t shot);

// The shooter record of `shot` in `maker`'s Shooter component, for RequestChangeStateReflected.
struct Record {
    std::uint64_t shooter = 0;
    std::uint32_t key = 0;
    std::int32_t index = 0;
};
std::optional<Record> FindRecord(CoopPuppet::Guest& guest, std::uint64_t maker, std::uint64_t shot);
CoopPuppet::Call ReflectCall(std::uint64_t main_base, const Record& record);

// The owner's side: shots another player sent back, each asked once on the calc thread.
class Queue {
public:
    // Any thread. `makers`: the actors that may have fired it (our shared enemies).
    void Ask(std::uint64_t shot, std::string name, std::vector<std::uint64_t> makers, std::uint64_t main_base);
    // Calc thread (a CoopCallers caller).
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result);
    std::vector<std::string> TakeNotes();
    void Reset();
    std::size_t Asked() const;

private:
    struct Ask_ {
        std::uint64_t shot = 0;
        std::string name;
        std::vector<std::uint64_t> makers;
        std::uint64_t main_base = 0;
    };
    mutable std::mutex mutex_;
    std::vector<Ask_> asks_;  // guarded
    std::vector<std::string> notes_;  // guarded
    std::size_t asked_ = 0;  // guarded
    std::optional<Ask_> calling_;  // calc thread only
};
Queue& TheQueue();

}  // namespace CoopShotReflect
