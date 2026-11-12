/* SPDX-License-Identifier: GPL-3.0-or-later
   Co-op's world hooks on Hookshot (oot-remake-multiplayer, docs/hookshot.md "World hooks"; our patch 0005).

   On the emulator co-op learns what the game does in its world through code caves patched into the game (the pause
   counters' cave, lichii/hook/coop_counter.s, fills a ring in the game's heap). On Hookshot the game is C: a lift-time
   hook at the game function calls hos_coop_world_event (Hookshot's host/coop_world.c), which hands the event to the
   module's optional export hos_coop_world. No guest memory, no cave, no game call.

   The same file is in Hookshot's host/ through our patch 0005 and in our repository (hookshot/src): keep them equal
   (tests/hookshot/module_test.cpp checks it). Plain C. */
#ifndef HOS_COOP_WORLD_H
#define HOS_COOP_WORLD_H
#include <stdint.h>

/* Events. a and b as listed; a new event takes the next number, an old module answers 0 to one it does not know. */
#define HOS_COOP_WORLD_HELLO 0u           /* asked by Hookshot before it keeps the export: 1 once the module is on */
#define HOS_COOP_WORLD_COUNTER_START 1u   /* main+0x5e6760 started an AI ExecuteGenericCounter: a = the node */

/* The module's export (extern "C"). Called on the game thread that ran the hooked function (not always the calc
   thread): it must be quick, take no lock the game waits on, and not call the game. Returns 1 when taken. */
typedef int (*hos_coop_world_fn)(uint32_t event, uint64_t a, uint64_t b);

#endif
