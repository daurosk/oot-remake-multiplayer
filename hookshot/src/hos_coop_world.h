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

/* The level of the hooks in this file: one more with every change of the events or the wraps. Hookshot tells the module
   at HOS_COOP_WORLD_HELLO, so a module knows what the Hookshot it runs in has (1, the first, sent 0 there). */
#define HOS_COOP_WORLD_VERSION 2u

/* Events. a and b as listed; a new event takes the next number, an old module answers 0 to one it does not know. */
#define HOS_COOP_WORLD_HELLO 0u           /* asked by Hookshot before it keeps the exports: 1 once the module is on.
                                             a = HOS_COOP_WORLD_VERSION, b = this build's wraps (bit id for each) */
#define HOS_COOP_WORLD_COUNTER_START 1u   /* main+0x5e6760 started an AI ExecuteGenericCounter: a = the node */
/* The event numbers are one space over two headers: 0-1 above; 2-15 are the actor events' (hos_coop_actors.h, patch
   0007); the world's next ones here are 16-63. */
#define HOS_COOP_WORLD_ACTOR_FIRST 2u
#define HOS_COOP_WORLD_ACTOR_LAST 15u
#define HOS_COOP_WORLD_NEXT_FIRST 16u
#define HOS_COOP_WORLD_NEXT_LAST 63u

/* Wrapped game functions (nx2/coop_world_wraps.py WRAPS: id, main offset, first words). A new wrap takes the next id. */
#define HOS_COOP_WRAP_FLAG_APPLY 1u   /* main+0xb99980: the game applies its queued flag writes (the save load's too) */

/* The module's exports (extern "C"), both optional. Called on the game thread that ran the hooked function (not always
   the calc thread): quick, no lock the game waits on, no game call. hos_coop_world returns 1 when taken.
   hos_coop_wrap: phase 0 before the game's function, phase 1 after it; x = the guest registers x0-x31 (x[31] is SP),
   read and written in place (arguments at phase 0, x0 the result at phase 1); ctx = the game's context (for later
   use with HosCoopApi::call, not yet allowed here). Phase 0 returning 1 means the module did the call's work and the
   game's function is not run (phase 1 still comes); return 0 to let the game run it, as nearly always.
   The first call can be a phase 1 without its phase 0: the exports are looked up while the game runs, and a function
   that was running then ends with them found. */
typedef int (*hos_coop_world_fn)(uint32_t event, uint64_t a, uint64_t b);
typedef int (*hos_coop_wrap_fn)(uint32_t id, int phase, uint64_t *x, void *ctx);

#endif
