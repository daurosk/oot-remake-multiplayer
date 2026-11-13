/* SPDX-License-Identifier: GPL-3.0-or-later
   Native boss sync on Hookshot (oot-remake-multiplayer, docs/dungeons-boss.md "Native boss sync").

   Two more wrapped game functions for the one hook table (hs-build-speed; the same rows as patch 0005's WRAPS, ids in
   the one wrap space of hos_coop_world.h). The module's hos_coop_wrap gets them; Hookshot announces each wrap its build
   has in HOS_COOP_WORLD_HELLO's bits, and the module turns boss sync on only when both are there. Rows (nx2 form):
     (4, 0x6733b0, (0xd503237f, 0xa9bd7bfd, 0xf9000bf5, 0xa9024ff4)),  # pacibsp; stp x29, x30; str x21; stp x20, x19
     (5, 0xc08f0,  (0xd503237f, 0xd10743ff, 0xa9187bfd, 0xf900cbfc)),  # pacibsp; sub sp, #0x1d0; stp x29, x30; str x28
   Phase 0 may return 1 (skip); the module then sets x0 to the result: 0x6733b0 returns nothing its callers read (0 is
   set anyway), 0xc08f0 returns 0 for a request it took (main+0xc10f4; 2 and 3 on its refusal paths), and 5 of its 9
   callers pass that up to their AI node, so a skipped request gives 0, as a taken one. Plain C. */
#ifndef HOS_COOP_BOSSES_H
#define HOS_COOP_BOSSES_H

/* main+0x6733b0, the BSA component's "set brain verb" (13 callers: the AI node OneShotBSABrainVerbSetter, the plug
   setter, death, events): x0 the BSA ([[actor+0x270]+0x188]), w1 the verb, w2 force (bit 0), x3 the asking node. */
#define HOS_COOP_WRAP_BRAIN_VERB 4u
/* main+0xc08f0, the BSA component's "request action verb" (9 callers: ExecuteBSASetActionVerb and the other
   ExecuteBSA* nodes): x0 the BSA, x1 the request (0x60 bytes, coop_boss_native.h addr), w2 1 when the verb changed. */
#define HOS_COOP_WRAP_ACTION_VERB 5u

#endif
