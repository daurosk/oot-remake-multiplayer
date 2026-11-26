// SPDX-License-Identifier: GPL-3.0-or-later
//
// The order of every caller of game functions at the svc cave (CoopSvc::AddCaller, CoopCallers::Add): the puppet
// director first, then these, lowest first, each its own number. Callers with the same number used to run in the
// order they were registered, so a clash (drops, grass cuts and the clock all at 40; room-objects' switch holds and
// down-but-not-out's body both at 48) made the order depend on start-up. Every AddCaller takes its order from here;
// the static_assert below and tests/lichii/caller_orders_test.cpp (which reads this file and every AddCaller in the
// hook) fail the build on a number used twice or a caller with an order of its own.
#pragma once

#include <array>
#include <cstddef>

namespace CoopCallerOrders {

inline constexpr int kSpawn = 10;         // spawn copies (coop_spawn.cpp)
inline constexpr int kShotReflect = 11;   // row 177: shots another player sent back (coop_spawn.cpp, coop_shot_reflect.h)
inline constexpr int kFairy = 20;         // fairies (coop_fairy.h)
inline constexpr int kMover = 30;         // the objects' mover (coop_objects.cpp)
inline constexpr int kDrops = 40;         // drops, after the mover (coop_drops.cpp)
inline constexpr int kGrass = 41;         // the others' field grass cuts (coop_objects.cpp, coop_world.h)
inline constexpr int kTime = 42;          // the clock, after the objects (coop_time.cpp)
inline constexpr int kSwitchStand = 44;  // ai-switches: another player on our step switch (coop_lift_ride.h)
inline constexpr int kBossTable = 45;     // after the drops, before the boss rooms (coop_boss_native.h)
inline constexpr int kBossOrders = 46;    // row 222: the owner's battle manager orders into our copies (coop_boss_orders.h)
inline constexpr int kLiftRide = 47;      // jabu-lifts #65 (coop_lift_ride.h)
inline constexpr int kDbnoBody = 48;      // down but not out: our downed Link (coop_dbno_body.h)
inline constexpr int kSwitchShare = 49;   // room-objects: another player's timed switch held on ours
inline constexpr int kBossRooms = 50;     // boss rooms (coop_boss_game.cpp)
inline constexpr int kEventResult = 58;   // row 98(2): our asker's result of another player's gimmick event (coop_hook.cpp)
inline constexpr int kEyeSwitch = 59;     // deku-switches: before the live events' requester (coop_hook.cpp)
inline constexpr int kLiveEvents = 60;    // live events' requester (coop_hook.cpp)
inline constexpr int kTeleportEvent = 61; // after the live events (coop_teleport_game.cpp)
inline constexpr int kScrubs = 62;        // issue 149 (coop_objects.cpp)
inline constexpr int kCounters = 63;      // the pause counters' ring (coop_objects.cpp)
inline constexpr int kStory = 64;         // issue 171: shared quest steps (coop_hook.cpp)
inline constexpr int kOcarinaEquip = 66;  // rows 93/103: the shared ocarina's equip (coop_hook.cpp)
inline constexpr int kPvpShield = 67;     // phase 4: the _Pvp copies' Life invincible (coop_objects.cpp)
inline constexpr int kTestDress = 68;     // test only: `dress` asks the game's own equip for our Link (coop_hook.cpp)
inline constexpr int kKeptShield = 69;    // row 160 (b): kept kills' copies held unhittable (coop_objects.cpp)
inline constexpr int kLifeGauge = 70;     // row 238: HUD gauges refreshed after co-op life writes (coop_life_gauge.h)

inline constexpr std::array kAll{kSpawn,     kShotReflect, kFairy,       kMover,     kDrops,   kGrass,        kTime,
                                 kSwitchStand, kBossTable, kBossOrders, kLiftRide,    kDbnoBody,  kSwitchShare, kBossRooms, kEventResult, kEyeSwitch,
                                 kLiveEvents, kTeleportEvent, kScrubs, kCounters, kStory,      kOcarinaEquip, kPvpShield,
                                 kTestDress, kKeptShield, kLifeGauge};

constexpr bool Unique() {
    for (std::size_t i = 0; i < kAll.size(); ++i) {
        if (kAll[i] <= 0) return false;  // 0 is the puppet director's
        for (std::size_t j = 0; j < i; ++j) {
            if (kAll[i] == kAll[j]) return false;
        }
    }
    return true;
}
static_assert(Unique(), "two callers share an order (coop_caller_orders.h)");

}  // namespace CoopCallerOrders
