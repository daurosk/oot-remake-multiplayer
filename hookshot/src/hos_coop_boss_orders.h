/* SPDX-License-Identifier: GPL-3.0-or-later
   A battle manager's orders, on Hookshot (oot-remake-multiplayer, docs/dungeons-boss.md "Battle manager orders").

   One wrapped game function more in the one wrap table (patch 0005's nx2/coop_world_wraps.py WRAPS; the ids are in
   the one wrap space of hos_coop_world.h). Patch 0036 adds its row and this file (Hookshot's host/hos_coop_boss_orders.h
   is the same file). The module's hos_coop_wrap gets it; Hookshot announces it in HOS_COOP_WORLD_HELLO's bits. The row:
     (10, 0x2723f80, (0xd503237f, 0xd10103ff, 0xa9027bfd, 0xa9034ff4)),  # pacibsp; sub sp, #0x40; stp x29, x30; stp x20, x19
   Phase 0 may return 1 (skip); the module then sets x0 to 1, what the function always returns. Plain C. */
#ifndef HOS_COOP_BOSS_ORDERS_H
#define HOS_COOP_BOSS_ORDERS_H

/* main+0x2723f80, the message handler of the AI node ExecuteKotakeKoumeReceiveMessageProc (vtable main+0x49a8568,
   slot 39; called through the node's receiver at +0x20, main+0x27240e0): Twinrova's sisters Kotake and Koume take
   their battle manager's orders here (Enemy_KotakeKoumeBattleManager's ExecuteKotakeKoumeBattleManagerAI posts them
   through the game's message manager, main+0x379740). x0 the node, x1 the message: +0 the sender's actor id, +4 the
   type, +0x10 the payload. Types 0x70210000 (battle state, roll type, attack start place, main move) and 0x70210001
   (under the interval threshold) are written into the sister's blackboard; others are not read. Returns 1. */
#define HOS_COOP_WRAP_BOSS_ORDER 10u

#endif
