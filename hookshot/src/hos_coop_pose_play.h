/* SPDX-License-Identifier: GPL-3.0-or-later
   The game's AS play request, on Hookshot (oot-remake-multiplayer, docs/play-online.md "Down but not out", the 0.12.1
   downed-move root fix).

   One wrapped game function more in the one wrap table (patch 0005's nx2/coop_world_wraps.py WRAPS; the id is in the
   one wrap space of hos_coop_world.h). Patch 0049 adds its row and this file (Hookshot's host/hos_coop_pose_play.h is
   the same file). The module's hos_coop_wrap gets it; Hookshot announces it in HOS_COOP_WORLD_HELLO's bits. The row:
     (32, 0x2022a0, (0xd503237f, 0xd10203ff, 0xa9047bfd, 0xf9002bf7, 0xa90657f6, 0xa9074ff4)),
   Plain C. */
#ifndef HOS_COOP_POSE_PLAY_H
#define HOS_COOP_POSE_PLAY_H

/* main+0x2022a0: play an AS command on an actor (x0 the request: +0 actor, +8 char* command, +0x10 s32 slot, +0x14
   rate, +0x18 / +0x1c, +0x20 u8 skip-if-same, +0x28 node; docs/addresses.md kComponentsAs, the puppets' kPlayAs).
   Returns a bool: 1 when played, and 1 without playing when skip-if-same is set and the slot already plays that
   command (main+0x2023a0 .. +0x2023d0 compare the names, both go to main+0x2025c8 = mov w20, #1). 87 call sites in
   the game (tools/game_scan/callers.py), the player's state steps among them. The module answers 1 (as played) for its
   own downed Link held in a lying pose, slot 0, any command but the pose (and a horse's): the game's ground verbs
   (Walk / Wait) keep moving him from the stick while the pose stays on screen. Every other call runs the game's
   play. */
#define HOS_COOP_WRAP_POSE_PLAY 32u

#endif
