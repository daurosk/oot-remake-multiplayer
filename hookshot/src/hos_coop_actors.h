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
/* main+0xa6490, the delete request: a = the actor, b = the reason (x1). Not for an actor already being deleted. */
#define HOS_COOP_WORLD_DELETE_REQUEST 3u
/* main+0x49d8c0, the Life component's step: a = the Life component ([[actor + 0x270] + 0x2b8]), b = this frame's
   damage sum (an int32; 0 is never sent), before the step takes it off life. */
#define HOS_COOP_WORLD_LIFE_DAMAGE 4u
/* The same create, first: a = the caller (the return address, a guest address), b = the create argument (x2:
   position at +0, rotation 3x3 at +0xc, on the caller's stack: read it at once or not at all). */
#define HOS_COOP_WORLD_CREATE_FROM 5u

#endif
