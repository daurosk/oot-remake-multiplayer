/* SPDX-License-Identifier: GPL-3.0-or-later
   Co-op's actor events on Hookshot (oot-remake-multiplayer, docs/hookshot.md "World hooks"; our patch 0007).

   Patch 0007's lift-time hooks (nx2/coop_actor_lift.py) hand them to the module through the same world hook as the
   pause counters (patch 0005, hos_coop_world.h: Hookshot's host/coop_world.c -> the module's hos_coop_world). Only
   the numbers live here; the hook text in the patch uses them as plain numbers (tests/hookshot/module_test.cpp checks
   they agree). Plain C. */
#ifndef HOS_COOP_ACTORS_H
#define HOS_COOP_ACTORS_H
#include "hos_coop_world.h"

/* main+0x372a70 requestCreateActorAsync was asked for an actor: a = the name (x1: its first 8 bytes a char* to the
   name), b = the handle slot (x3) the manager fills once the actor is made. Always just after CREATE_FROM, on the
   same thread. */
#define HOS_COOP_WORLD_CREATE_REQUEST 2u
/* main+0xa6490, the delete request, at the point it is accepted (main+0xa65d0: the function just set "delete asked",
   bit 16 of actor+0x30, and it was clear): a = the actor, b = the reason (x1). Once for an actor: not for one already
   being deleted (state 7 to 11), a refused request or a repeated one. */
#define HOS_COOP_WORLD_DELETE_REQUEST 3u
/* main+0x49d8c0, the Life component's step, each frame it reaches its damage sum (not when it leaves early: bits 3
   and 12 of Life+0x1d18 set, or no life value at Life+0x1c10): a = the component
   ([[actor + 0x270] + 0x2b8]), b = the address (in hookshot.exe, not the guest) of an int32 holding this frame's damage
   sum, before the step stores it and takes it off life. The module may change it: what it holds afterwards is the
   damage the game's own step takes this frame. */
#define HOS_COOP_WORLD_LIFE_STEP 4u
/* The same create, first: a = the caller (the return address, a guest address), b = the create argument (x2:
   position at +0, rotation 3x3 at +0xc, on the caller's stack: read it at once or not at all). */
#define HOS_COOP_WORLD_CREATE_FROM 5u
/* The same create, last (after CREATE_REQUEST, on the same thread): a = the address (in hookshot.exe, not the guest) of
   a uint32 holding 0. The module may set it to 1: the request then leaves through the game's own "the manager takes
   no creates now" exit (main+0x372c88: result 1 in the caller's status word, returns 0) and no actor is made. */
#define HOS_COOP_WORLD_CREATE_STOP 6u

/* main+0xc16cf0, the game's shoot code about to make a shot (it calls the create request at main+0xc172fc): a = x1,
   whose first 8 bytes are the shooter's actor (the callers store it there: main+0x992e44, main+0xd36a0's same lines),
   b = x0, the shooter's shot record. Before that create's CREATE_FROM, on the same thread. */
#define HOS_COOP_WORLD_SHOOT_FROM 7u
/* main+0x49d8c0, the Life component's step again, after LIFE_STEP on the same thread, where it is about to test its
   life stops (main+0x49dcbc: w10 = Life+0x70, how many stops the list at Life+0x60 holds; `cbnz w10` then clamps the
   sum so life stays at each stop and records the crossing at Life+0x1be8): a = the component, b = the address (in
   hookshot.exe) of a uint32 holding 0. The module may set it to 1: the step takes the sum past the stops this frame
   (w10 = 0), as for a component with none. Only for a copy taking its owner's death (review-boss-0114 risk 2). */
#define HOS_COOP_WORLD_LIFE_STOPS 8u
/* main+0x49d8c0, the Life component's step again, where life has fallen to 0 and the step is about to ask the revive
   check (main+0x49ed0c: main+0x49d6b0 on Life+0x18c0, the boss's BSA flags and then the actor's own callback; what it
   answers goes back onto life: Gohma answers 1 outside her chance time): a = the component, b = the address (in
   hookshot.exe) of a uint32 holding 0. The module may set it to 1: no revive (x0 = 0, on at main+0x49dd18, where the
   step goes when the check answers 0), so that death stays. Only for the step whose stops LIFE_STOPS passed (an
   owner's death on its copy, the test script's kill on our own). */
#define HOS_COOP_WORLD_LIFE_REVIVE 9u

/* The event numbers are one space with hos_coop_world.h's, which names the ranges: the actor events have
   HOS_COOP_WORLD_ACTOR_FIRST to HOS_COOP_WORLD_ACTOR_LAST (2 to 15; 2 to 9 used). */
#if HOS_COOP_WORLD_CREATE_REQUEST < HOS_COOP_WORLD_ACTOR_FIRST || HOS_COOP_WORLD_LIFE_REVIVE > HOS_COOP_WORLD_ACTOR_LAST
#error "hos_coop_actors.h: an actor event number outside its range (hos_coop_world.h)"
#endif

#endif
