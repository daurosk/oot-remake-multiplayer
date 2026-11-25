/* SPDX-License-Identifier: GPL-3.0-or-later
   A boss's shooter requests, on Hookshot (oot-remake-multiplayer, docs/dungeons-boss.md "Shooter requests").

   One wrapped game function more in the one wrap table (patch 0005's nx2/coop_world_wraps.py WRAPS; the ids are in
   the one wrap space of hos_coop_world.h). Patch 0044 adds its row and this file (Hookshot's host/hos_coop_boss_shots.h
   is the same file). The module's hos_coop_wrap gets it; Hookshot announces it in HOS_COOP_WORLD_HELLO's bits. The row:
     (11, 0x374d20, (0x72001c5f, 0x54000260, 0x34000241, 0xb9402c09)),  # tst w2, #0xff; b.eq; cbz w1; ldr w9, [x0, #0x2c]
   Phase 0 may return 1 (skip); the module then sets x0 to 1, the function's answer for a shootable it found. Phase 1
   has its answer in x0. Plain C. */
#ifndef HOS_COOP_BOSS_SHOTS_H
#define HOS_COOP_BOSS_SHOTS_H

/* main+0x374d20, the shooter's create request: x0 the shooter (an actor's component, [[actor+0x270]+0x3d8]), w1 the
   shootable's name hash, w2 how many (the low byte). It finds the shootable in the shooter's table (+0x20, 0x38-byte
   entries, count at +0x2c) and raises its asked count (entry+0x34, up to entry+8); returns 1 when found, else 0. A
   leaf with no frame. Its callers: the attack sequences' start step game__bsa__ActionSeqProcShootableActorCreate
   (main+0x746b48), the AI node OneShotShooterRequestCreateShootableActors (main+0x293ff98) and the shooter itself
   (main+0x378754). It prepares the bullet a ranged attack throws. */
#define HOS_COOP_WRAP_SHOOTER_REQUEST 11u

#endif
