// SPDX-License-Identifier: GPL-3.0-or-later
//
// Player report #65 (jabu-lifts): another player on a lift moves our copy of it too.
//
// The Jabu-Jabu lift (Obj_InsideJabu_Jabu_ElevatorLift_A_01) and moving floor (MoveFloor_A_01) start on their logic
// plug RideOnPlayer. Their AI sets that plug every frame from a physics contact with a player (the probe run
// hs-logic-probe: 0 -> 1 when Link steps on, 1 -> 0 when he steps off; RideOnCharacter stays 0). Another player's
// puppet has no physics, so in our game the lift never sees them and stays down while it rises in theirs.
//
// The fix uses the game's own set-bool: the AI node ExecuteLogicSetBool's execute (main+0x2727540, vtable
// main+0x49a9550; reports/dungeon-sync-b.md). Each frame that a fresh player of our scene stands on one of our
// lifts, we call it on a node block of our own that names the lift and RideOnPlayer, value 1, as the lift's own
// AI node does for our Link. It only ever sets 1: the lift's AI puts its own 0 back when nobody of ours is on it,
// so nothing of ours stays set when the other player steps off. Our own Link on it changes nothing (already 1).
//
// The node block (one guest malloc, kept): the execute reads
//   node+0x00 vtable (slot 0x138 must be main+0x181440, the base: it is), node+0x18 byte bit 1 (enabled),
//   node+0x20 u32 the plug's name key (Murmur3 of the name; the plug's own key is this xor 7),
//   node+0x38 -> the value byte, and the actor at [[[node+8]+8]+0x1f0], its logic at [[actor+0x270]+0x118].
// [actor+0x270] is read without a null check, so before each call the lift must be present and its logic must
// have a live bool plug RideOnPlayer (CoopLogicProbe::Read).
//
// LICHII_COOP_LIFT_RIDE=0 turns it off. No emulator code (tests/lichii/lift_ride_test.cpp).
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_logic_set.h"
#include "coop_puppet.h"

namespace CoopLiftRide {

namespace addr = ::CoopLogicSet::addr;  // the set-bool node (coop_logic_set.h)

inline constexpr std::uint32_t kRideOnPlayer = 0xa4ab760a;  // Murmur3("RideOnPlayer"); the plug's key is ^ 7

// A footprint around the lift's place (its actor position) where a player counts as standing on it.
inline constexpr float kRideRadiusM = 1.8f;
inline constexpr float kRideBelowM = 0.5f;
inline constexpr float kRideAboveM = 2.5f;
// A rider stays wanted this long after its last sample over the lift (a lost sample does not drop the ride).
inline constexpr std::uint64_t kHoldMs = 400;

// The lifts that start on RideOnPlayer (#65): Jabu-Jabu's lift and moving floor; the other dungeons' elevator lifts
// run the same base AI (Obj_Base_ElevatorLift_A_00), and every name the romfs gives a RideOnPlayer logic
// (CoopRoomObjects::RiderName: the Forest Temple's elevators). Not the Deku Tree's basement floor (GroundControl-driven) or
// its compass-room lifts (on/off animation actors with no RideOnPlayer plug).
bool IsRideLift(std::string_view name);

struct Lift {
    std::uint64_t address = 0;
    std::string name;
    coop::protocol::Vec3 position{};
};

// The lifts (of `lifts`) with one of `others` standing on them.
std::vector<std::uint64_t> Ridden(const std::vector<Lift>& lifts, const std::vector<coop::protocol::Vec3>& others);

// ai-switches (held step switches, "anyone stands on it"): the step switch's logic has no contact input like
// RideOnPlayer (the probe on Dodongo's Obj_StepSwitch_A_01: pulses BootUp, Logic_Create, Logic_Delete,
// RespawnTrigger; bools OnState, OffState, FirstOn and two unnamed ones). Its AI sets its contact bool (plug key
// 0x9cdaf491, unnamed: 1 while our Link stands on it) from physics, so a puppet never holds it. The same rider sets
// that bool through the game's set-bool each frame a fresh player of our scene stands on the switch; the AI puts
// its 0 back once nobody is on it (holder left, timed out or stepped off: OthersHere drops them, kHoldMs later).
// Blocks need nothing: a pushed block is in every game (WORLD_MOVE) and presses the switch by its own physics.
// LICHII_COOP_SWITCH_STAND=0 turns it off.
// The lane (NEXT42): the contact bool already read 1 in the watching game with nobody on the switch, so a set-bool
// on it changes nothing; it is not what presses the switch. The stander sets OnState (the switch's "on" output its
// shutter follows) instead; LICHII_COOP_SWITCH_STAND=contact puts the contact bool back, for a probe.
inline constexpr std::uint32_t kStepContact = 0x9cdaf491u ^ 7u;  // name key of the unnamed contact bool
inline constexpr std::uint32_t kStepOnState = 0xf1792ec3u;        // Murmur3("OnState")
inline constexpr float kStandRadiusM = 1.2f;
inline constexpr float kStandBelowM = 0.5f;
inline constexpr float kStandAboveM = 1.5f;

// A step switch a player holds down by standing on it (Obj_*StepSwitch_*).
bool IsStandSwitch(std::string_view name);

// The switches (of `switches`) with one of `others` standing on them.
std::vector<std::uint64_t> Stood(const std::vector<Lift>& switches, const std::vector<coop::protocol::Vec3>& others);

// The svc caller: the set-bool of plug `key` (a name key) on each wanted actor, once a frame.
class Rider {
public:
    explicit Rider(std::uint32_t key = kRideOnPlayer, std::string_view what = "lift ride") : key_(key), what_(what) {}

    // The objects walk (any thread): the lifts ridden now. `main` is main's base.
    void Want(const std::vector<std::uint64_t>& lifts, std::uint64_t main, std::uint64_t now_ms);
    // The calc thread (CoopCallers::Caller contract).
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t now_ms);
    std::vector<std::string> TakeNotes();
    // Scene change or load: nothing ridden.
    void Clear();

private:
    std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, std::uint64_t main);
    // A note, once per step, of why a wanted actor got no set-bool (the in-game check reads it).
    void Why(std::string_view step);
    std::vector<std::string> whys_;

    std::mutex mutex_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> wanted_;  // (lift, until ms)
    std::uint64_t main_ = 0;
    std::vector<std::string> notes_;
    std::uint32_t key_;
    std::string what_;
    // The calc thread's own.
    std::deque<std::uint64_t> frame_;  // this frame's lifts still to set
    std::uint64_t block_ = 0;
    bool asked_malloc_ = false;
    bool failed_ = false;
    std::vector<std::uint64_t> told_;  // lifts logged once
};

// The one rider (coop_objects.cpp registers it).
Rider& TheRider();
// The one switch stander (plug from LICHII_COOP_SWITCH_STAND).
// The stander's plug key for LICHII_COOP_SWITCH_STAND's value (null: unset): OnState unless "contact".
std::uint32_t StandKey(const char* env);
Rider& TheStander();

}  // namespace CoopLiftRide
