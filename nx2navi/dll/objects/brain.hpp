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

// Hooks the function. False when the interface has no function hooks or the
// game refused. Any thread. OOT_COOP_BRAIN_SKIP=<hex offset> names another
// function of main to leave out instead (its x0 the actor), for research.
bool Install(const Nx2PluginApi* api, std::uint64_t main_base);
void Remove();

struct Counts {
    std::uint64_t calls = 0;    // calls of the function seen
    std::uint64_t skipped = 0;  // of them, left out: a held copy's
    std::uint64_t for_hits = 0; // a held copy's that ran, for a hit that waited
};
// Since the last ask.
Counts Take();

// A write of the Mover's into guest memory: when it is a hit record (its size
// and place in a Life component), the copy whose Life that is runs its next
// kHitFrames updates. Called from the port's write, on the game's calc thread.
void NoteWrite(std::uint64_t address, std::size_t size);

// The hook callback, for the tests.
void OnActorAi(Nx2HookCall* call, void* user);

}  // namespace CoopNx2navi::Objects::Brain
