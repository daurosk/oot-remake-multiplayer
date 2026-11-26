// SPDX-License-Identifier: GPL-3.0-or-later
//
// room-objects (issues 196, 249, 194; reports/room-objects-a.md): which moving room objects are driven by their own
// logic, so another game's copy is never written from outside.
//
// The rule: in a game that follows another, a room object moves only through its own logic, the input its own
// switch, sensor or rider would set: CoopEyeSwitch sets the switch's plug (the Deku Tree compass-room lifts' IsOn),
// CoopLiftRide sets RideOnPlayer (Jabu-Jabu's and the Forest Temple's lifts), the push block's own node starts again
// (CoopWorld::FindBlockNode). A word written into such an object from outside fights that logic: the clock path
// (coop_platforms.h) took a frame counter on the Deku lifts (the game put it back, "do not hold") and a guessed float
// on Jabu-Jabu's moving floor. So the clock path leaves these objects alone and is for free-running movers only
// (no wired control input, no rider plug): the Deku Tree's water-room wave, the loops.
//
// The table is generated from the romfs: tools/puppet_pack/room_objects.py. Each placement of a platform name
// (CoopPlatforms::IsDriver) with why it is driven (the wired inputs, RideOnPlayer, or "with <name>": a part placed on
// a driven one), or "" when free. A live actor is the placement of its name nearest to it in its scene; a name or
// scene not in the table is free.
//
// No emulator code (tests/lichii/room_objects_test.cpp).
#pragma once

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace CoopRoomObjects {

struct Placement {
    std::string_view scene;
    std::string_view name;
    std::array<float, 3> place;
    std::string_view why;  // "" free
};

std::span<const Placement> Placements();

// The placement a live actor `name` at `at` in `scene` is (its name's nearest there), or nullptr.
const Placement* Nearest(std::string_view scene, std::string_view name, const std::array<float, 3>& at);

// Driven by its own logic (its placement's `why` is not empty): never written from outside.
bool Driven(std::string_view scene, std::string_view name, const std::array<float, 3>& at);

// Row 164: a `why` with an input one game's own player sets (RideOnPlayer, IsInPlayer, IsTargetFloor: a rider's lift,
// a player sensor): that game's mover runs on its own player, so no other game's timing applies to it. The other
// inputs (IsShortCutMove, IsOn, MoveToPosX, ...) come from a switch or an event whose state every game shares, so the
// mover runs the same logic in every game (Dodongo's Cavern's shortcut lift loops like its two neighbours).
bool PlayerDriven(std::string_view why);

// Its logic type has the bool output RideOnPlayer (the generated list): CoopLiftRide drives it. Taken by the logic,
// whatever the name: the Fire Temple's RiseFloor_A_02 and Ganon's Castle's ElevatedFloor_A_01 run the Forest
// Temple elevator's logic and are no CoopPlatforms::IsDriver name.
bool RiderName(std::string_view name);

// A placement group with a saved flag (fable-g3-objects, issue 249): actors that share a Phive.Placement.GroupGUID
// with a GroundControlActor_ResidentMoveTo. The controller has no input in the room graph; it moves the whole
// group (a water plane, a cage, a hanging floor, enemy air walls) when the flag the scene's PlgGdTable gives the
// group changes. So the flag is the group's only input: co-op shares the flag (coop_dungeon.cpp) and never writes
// a member's place. A test holds every dungeon group's flag to the shared list or to the list of flags left out
// on purpose (docs/dungeon.md, "Movers with a group flag").
struct GroupFlag {
    std::string_view scene;
    std::string_view flag;
    bool is_int;
    std::array<float, 3> place;  // the group's controller
    std::string_view moves;      // the other members' names
};
std::span<const GroupFlag> GroupFlags();

// The switch output plugs a room object reads (through gates, timers, delays and module inputs) that no CoopEyeSwitch
// line covers: the timed switches. SWITCH_STATE carries their presses (coop_switch_share.h). Generated with
// room_objects.py --eye-switches coop_eye_switch.cpp (its third table).
//
// g3-objects (A #44): some of these plugs also raise an event asker (a CoopEyeSwitch line whose plug the switch's AI
// rewrites, so CoopEyeSwitch does not drive it). Holding such a plug in a follower would start the follower's own
// copy of the gimmick event (owner 07:40 UTC 2026-11-17: result only, no replay). So such a line names its `guard`:
// the shared flags ('+'-joined) that stop every asker it raises (a flag that deletes the asker, or one its trigger
// reads negated). It is held only while all of them are on in our game: our asker can no longer fire, and the hold
// runs the room objects only (Jabu-Jabu room 1's shutters, the Deku Tree room D lifts, GTG room 5's chest, Spirit
// room 19's mirror). A plug whose asker no shared flag stops is not in the table (its result reaches the follower's
// switch through a shared flag on its own ON input: coop_room_objects.cpp lists them).
// A guard written "load:<flag>" is a flag the room reads only when it loads (a gate with UpdateOnce 1): on at the
// load, it stopped our asker for this visit; landing live (the other player's press) stops nothing, our asker still
// exists. Such a guard counts only when it was on at our room's load (CoopSwitchShare::Sharer latches it at the
// switch's first sight): the Deku Tree room D lifts, whose compass asker RoomD_StepSwitch_On_01 deletes only at a load
// (Room_D_Prefab_b6c3). Mid-visit the follower's lifts depend on our asker's EventBool: row 98(2) sets it
// (coop_event_result.h), but the asker's own AI writes it 0 every tick until its trigger rises, so whether the room
// sees it is not proven (deku-platforms-b21: in big-run-0115 only the camera played in the follower's game).
struct SwitchPlug {
    std::string_view scene;
    std::string_view name;
    std::array<float, 3> place;
    std::string_view plug;
    std::string_view guard = {};  // "" no asker; else the flags that must all be on in our game before a hold
};
std::span<const SwitchPlug> SwitchPlugs();
inline constexpr std::string_view kLoadGuard = "load:";
struct GuardFlag {
    std::string_view flag;
    bool at_load = false;  // written "load:<flag>": read by the room only when it loads
    bool operator==(const GuardFlag&) const = default;
};
// A line's guard flags, split ("" none), the "load:" mark taken off into at_load.
std::vector<GuardFlag> GuardFlags(const SwitchPlug& line);
// The lines of a live switch `name` at `at` in `scene` (its placement within kSwitchMatchM).
inline constexpr float kSwitchMatchM = 1.5f;
std::vector<const SwitchPlug*> SwitchPlugsOf(std::string_view scene, std::string_view name,
                                             const std::array<float, 3>& at);
// A switch name some line has (any scene): the objects walk keeps these actors.
bool SharedSwitchName(std::string_view name);

}  // namespace CoopRoomObjects
