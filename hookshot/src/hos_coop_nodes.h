/* SPDX-License-Identifier: GPL-3.0-or-later
   Co-op's AI node starts on Hookshot (oot-remake-multiplayer, docs/hookshot.md "World hooks"; our patch 0029).

   Patch 0029's lift-time hooks (nx2/coop_node_lift.py, one row per node start) hand a started AI node and its actor to
   the module through the same world hook as the pause counters (patch 0005, hos_coop_world.h: Hookshot's
   host/coop_world.c -> the module's hos_coop_world). One event for every start: a new start is a new row there, not a
   new event. Only the number lives here; the hook text in the patch uses it as a plain number
   (tests/hookshot/module_test.cpp checks they agree). Hookshot's host/hos_coop_nodes.h is the same file. Plain C. */
#ifndef HOS_COOP_NODES_H
#define HOS_COOP_NODES_H
#include "hos_coop_world.h"

/* An AI node of an actor was started (entered): a = the node, b = its actor ([[[node + 8] + 8] + 0x1f0]), on the game
   thread that runs the actor's AI. The starts hooked (nx2/coop_node_lift.py STARTS):
   - main+0x2751790, the push node's start of every pushed block (ExecuteMovableBoxControl's Enter, vtable
     main+0x49ae6f8, and the two node classes whose Enter calls it first, vtables main+0x499dfd0 and main+0x49dd0f0):
     after main+0x27517c0, where it has read the actor and has not yet left early. */
#define HOS_COOP_WORLD_NODE_START 16u

/* The event numbers are one space with hos_coop_world.h's, which names the ranges: this one is the first of
   HOS_COOP_WORLD_NEXT_FIRST to HOS_COOP_WORLD_NEXT_LAST. */
#if HOS_COOP_WORLD_NODE_START < HOS_COOP_WORLD_NEXT_FIRST || HOS_COOP_WORLD_NODE_START > HOS_COOP_WORLD_NEXT_LAST
#error "hos_coop_nodes.h: the node start's event number is outside its range (hos_coop_world.h)"
#endif

#endif
