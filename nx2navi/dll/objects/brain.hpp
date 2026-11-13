// SPDX-License-Identifier: GPL-3.0-or-later
//
// A copy's own AI left out (`objects_ai_hold = 3`): the third way to hold an
// enemy copy, after the two brain-skip-bit modes of hold.hpp, which change the
// bit and leave the copy's AI running (README, "Verified, and not").
//
// What the game does (build e0f3ccfaf50065ae; found with ai_probe.hpp's call
// chains of the AS play and read from main):
// - main+0x2c1d740 (actor, times) is an actor's calc of a frame: it runs the
//   actor's components one after the other, each through its own update, and
//   in the middle calls
// - main+0xcb9c0 (actor, times), its only caller: the update of the actor's
//   component at table +0x188 and of what hangs on it. From there the calls
//   go into the node trees at main+0x74xxxx (the AI and action sequences), and
//   from those to the AS play main+0x2022a0: every animation an enemy chooses
//   by itself (`Wait`, `Walk`, `NoWeaponAttack`, `Appear`, ...) is asked for
//   under this function. The actor's other components (the animation player,
//   physics, the model) are updated by the calls around it, not under it.
//
// The hook: main+0xcb9c0 is left out (skip) when its actor is a held copy,
// which is an actor whose BSA component ([[actor + 0x270] + 0xb0]) is in the
// Mover's hold table (hold.hpp's Table, written each game frame from
// Sync::Holds). Everything else of the actor's frame runs, so the owner's
// animation, which the Mover plays on the copy, is still animated and shown.
//
// One thing under that function a copy still needs: taking a hit. A hit comes
// into the actor's Life component as a record (lichii/hook/coop_objects.cpp,
// kHitTemplate: the attack's header at Life +0x1f8 with the hit's id at
// +0x200, the record at +0x240), put there by the game when our own Link
// strikes the copy, or by the Mover when the owner's game says the enemy was
// struck there. The Life step (main+0x49d8c0, called from the actor's calc
// before 0xcb9c0) takes the record in, but the life only goes down, and the
// enemy only flinches, in the update that is left out: with it always left out
// no hit was taken (runs e2 and e5: `hurts taken 0 missed 8`, against `taken 8
// missed 0` with the AI running; the Mover then wrote the life itself, and our
// own Link's strikes on a copy would have done nothing). So for kHitFrames
// updates after a hit came in, the copy's update runs. A hit is known to have
// come in when the Mover wrote a record into that Life component (NoteHit,
// from the port's write) or when the id in the attack's header changed (the
// game's own).
#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

struct Nx2PluginApi;
struct Nx2HookCall;

namespace CoopNx2navi::Objects::Brain {

inline constexpr std::uint64_t kActorAi = 0xcb9c0;       // (actor, times)
inline constexpr std::uint64_t kActorComponents = 0x270; // the actor's component table
inline constexpr std::uint64_t kComponentBsa = 0xb0;     // its BSA component
inline constexpr std::uint64_t kComponentLife = 0x2b8;   // its Life component
inline constexpr std::uint64_t kHitHeaderId = 0x200;     // Life: the id in the last attack's header
inline constexpr std::uint64_t kHitRecord = 0x240;       // Life: the hit record the Mover writes
inline constexpr std::size_t kHitRecordSize = 0x98;      // its size (coop_objects.cpp kHitTemplate)
inline constexpr int kHitFrames = 8;                     // updates a copy runs after a hit came in
// The first updates of a copy that is held for the first time run too: an
// enemy made as a copy and held from its first frame never left its first
// state (a Stalchild is still "under the ground" to itself, whatever the Mover
// shows), and the first hit on it was not taken (run e6: the first hit on each
// of four copies missed, the four later ones on one of them taken).
// 150 since coop-dll 0.8 (was 45): a Stalchild takes about 2.5 s to rise, and a copy whose AI was
// stopped before that stayed "rising" to itself, which is a state no sword can hit (a human
// player could not strike a copy at all; the scripted strikes happened to come early).
inline constexpr int kWarmFrames = 150;

// The far rule (Stalchildren). The Stalchild's battle AI sends it back under the
// ground ("Disappear", which ends in deleting the actor) when its target, the
// Link of its own game, is more than ReturnDistance (24 m) away, or more than
// OutsideUserPercepReturnDistance (18 m) away and not perceived (StalchildParam,
// read by the AI from the parameter file each time it asks). A copy's AI runs
// only for its first kWarmFrames updates and after a hit, and it measures from
// its own game's Link, who may be far from the enemy its owner's Link fights:
// so a copy decided to burrow (and deleted itself, and was made again, runs
// ds1, ds2), or was held in the middle of that decision and burrowed the moment
// it became ours. So the updates of a held Stalchild copy that run, and the first
// kGraceUpdates after it became ours, run with those two distances widened to
// kFarWide (written into the parameter file for that update, put back after it;
// one parameter file serves every Stalchild, so an owner's Stalchild updating
// at the same moment on another thread may keep chasing for that frame).
// Where the file is: [[[actor + 0x290] + 0x158] + 0x278 or + 0x280], a byml
// dictionary whose entries are (key | 0xd2 float, value) words; key 7 is
// OutsideUserPercepReturnDistance, key 11 ReturnDistance (build e0f3ccfaf50065ae,
// found by OOT_COOP_PROBE_PARAM, run pp2). OOT_COOP_STALCHILD_FAR=0 turns it off.
inline constexpr std::uint64_t kActorName = 0x260;
inline constexpr std::uint64_t kFarPath0 = 0x290;
inline constexpr std::uint64_t kFarPath1 = 0x158;
inline constexpr std::uint64_t kFarPath2 = 0x278;
inline constexpr std::uint64_t kFarPath2b = 0x280;
inline constexpr std::uint32_t kFarOutside = 0x41900000u;  // 18.0f
inline constexpr std::uint32_t kFarReturn = 0x41c00000u;   // 24.0f
inline constexpr std::uint32_t kFarWide = 0x447a0000u;     // 1000.0f
inline constexpr int kGraceUpdates = 300;
// The burrow itself. Read from the Stalchild's battle AI (AI/Enemy_Stalchild_LandBattle.actionseq):
// every way back under the ground is an ExecuteBSAInvokeActionVerbWithArgs node with ActionVerb 96
// ("Disappear"), taken when the time is out of the night, when the target information of its brain
// is not up to date (a copy held for seconds has none, a copy the AI-less director made never had
// one), or when the target is beyond the two distances above. That node's execute (main+0x262c160)
// fills a request (verb at +0x4c, the node at +0x58; the node's actor at [[[node + 8] + 8] + 0x1f0])
// and hands it to main+0xc08f0 (the actor's BSA component, request, 1). A hook there refuses verb 96
// for a Stalchild that is a held copy (its owner's game decides when it burrows, and the copy goes
// with the owner's OBJECT_REMOVE) or that became ours less than kGraceUpdates updates ago (its brain
// takes the new target first).
inline constexpr std::uint64_t kActionVerbInvoke = 0xc08f0;
inline constexpr std::uint64_t kVerbRequestVerb = 0x4c;  // (+0x48 is the ActionSlot)
inline constexpr std::uint64_t kVerbRequestNode = 0x58;
inline constexpr std::uint64_t kNodeActor = 0x1f0;
inline constexpr std::uint32_t kVerbDisappear = 96;
// What a Stalchild that just became ours does instead when our living Link is within
// kChaseRange of it: the verb its AI uses to walk (1, "Walk", as for its wander), with
// the request's TargetPos (+0x18, three floats) at our Link. Walking up to him, its
// brain takes him as its target (a solo Stalchild chases up to 18 to 24 m: run sb1).
inline constexpr std::uint32_t kVerbWalk = 1;
inline constexpr std::uint64_t kVerbRequestTargetPos = 0x18;
inline constexpr std::uint64_t kActorPos = 0x31c;
inline constexpr float kChaseRange = 24.0f;
// The target. An enemy's brain keeps its targets in the component at [[actor + 0x270] + 0x418],
// slot i at +0x28 + 0x130 * i: +0x10 the target's handle and +0x18 its id (the actor link),
// +0xc8 the frames since the last update (0 = up to date, what LandBattle asks), +0xdc flags (bit 2:
// refreshed every frame without perception), +0x12a state (2 = valid), +0x12c bit 0 tracked. The
// slots are refreshed in the AI update (main+0xd4680, called from main+0xcb9c0), so a held copy's
// target is frozen and a copy that becomes ours has a stale one, or none: it burrows. The game's
// "set target" behind OneShotTargetSetFromActor is main+0x88efe0 (slot, link, options): link is 0x18
// bytes (+8 the actor's [actor + 0x1b0], +0x10 its u32 [actor + 0x18]), options {keep 0, track 1,
// always 1, pad, u32 perception type 1}, as the game itself calls it at main+0x2b214b4. A Stalchild
// that became ours gets our Link as its target that way before its first update runs again.
// Read from main (static reading of main+0x2aee610, 0x88efe0, 0x37e980, 0xd4680).
inline constexpr std::uint64_t kComponentTarget = 0x418;
inline constexpr std::uint64_t kTargetSlot0 = 0x28;
inline constexpr std::uint64_t kSlotHandle = 0x10;
inline constexpr std::uint64_t kSlotId = 0x18;
inline constexpr std::uint64_t kSlotFlags = 0xdc;
inline constexpr std::uint64_t kSlotState = 0x12a;
inline constexpr std::uint64_t kSlotTracked = 0x12c;
inline constexpr std::uint8_t kSlotValid = 2;
inline constexpr std::uint64_t kPlayerHandle = 0x1b0;
inline constexpr std::uint64_t kPlayerId = 0x18;
inline constexpr std::uint64_t kTargetSet = 0x88efe0;
inline constexpr std::uint64_t kGameMalloc = 0x14c6c90;

// Hooks the function. False when the interface has no function hooks or the
// game refused. Any thread. OOT_COOP_BRAIN_SKIP=<hex offset> names another
// function of main to leave out instead (its x0 the actor), for research.
bool Install(const Nx2PluginApi* api, std::uint64_t main_base);
void Remove();

struct Counts {
    std::uint64_t calls = 0;    // calls of the function seen
    std::uint64_t skipped = 0;  // of them, left out: a held copy's
    std::uint64_t for_hits = 0; // a held copy's that ran, for a hit that waited
    std::uint64_t far_runs = 0; // updates run with the Stalchild's far rule widened
    std::uint64_t burrows_refused = 0;    // Disappear verbs refused (held copies)
    std::uint64_t burrows_redirected = 0; // ... turned into a walk to our Link (just taken over)
    std::uint64_t burrows_let = 0;        // ... let through: just taken over, our Link out of range
    std::uint64_t targets_set = 0;        // brains pointed at our Link (it became ours)
    std::uint64_t burrows_held = 0;       // ... refused: ours, our Link dead or far, another living player near
    bool far_found = false;
    int far_tries = 0;
    std::string verbs;          // OOT_COOP_DEBUG_VERBS=1: the action verbs Stalchildren asked for     // the parameter file's two words were found
};
// Since the last ask.
Counts Take();

// A write of the Mover's into guest memory: when it is a hit record (its size
// and place in a Life component), the copy whose Life that is runs its next
// kHitFrames updates. Called from the port's write, on the game's calc thread.
void NoteWrite(std::uint64_t address, std::size_t size);

// Our own Link this frame (nullptr: not readable) and whether he has life left.
// Any thread; the burrow rule above reads it.
void NoteOurLink(const float* position, bool alive);
// The other living players in our scene, where their puppets stand (each frame). An enemy of ours
// that would burrow while our Link is dead or far and one of them is within kChaseRange waits for
// the server's hand-over instead.
// Our Link's actor (0: not known) and main's base, for the target call. Any thread.
void NoteOurPlayer(std::uint64_t main_base, std::uint64_t actor);
// Our Link's actor from the game's actor list (Player_Adult / Player_Child), 0 when not found.
std::uint64_t FindPlayer(const Nx2PluginApi* api, std::uint64_t main_base);
void NoteOthers(const std::vector<std::array<float, 3>>& living);

// The hook callback, for the tests.
void OnActorAi(Nx2HookCall* call, void* user);
void OnActionVerb(Nx2HookCall* call, void* user);

}  // namespace CoopNx2navi::Objects::Brain
