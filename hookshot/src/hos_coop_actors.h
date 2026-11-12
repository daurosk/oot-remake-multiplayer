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
/* main+0x49d8c0, the Life component's step, every frame for every Life component: a = the component
   ([[actor + 0x270] + 0x2b8]), b = the address (in hookshot.exe, not the guest) of an int32 holding this frame's damage
   sum, before the step stores it and takes it off life. The module may change it: what it holds afterwards is the
   damage the game's own step takes this frame. */
#define HOS_COOP_WORLD_LIFE_STEP 4u
/* The same create, first: a = the caller (the return address, a guest address), b = the create argument (x2:
   position at +0, rotation 3x3 at +0xc, on the caller's stack: read it at once or not at all). */
#define HOS_COOP_WORLD_CREATE_FROM 5u

#endif
