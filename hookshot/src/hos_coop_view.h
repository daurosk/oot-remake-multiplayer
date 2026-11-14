/* SPDX-License-Identifier: GPL-3.0-or-later
   The co-op module may frame the game's camera (oot-remake-multiplayer, docs/hookshot.md "The Multiplayer tab"; our
   patch 0019): the Multiplayer tab's Customize page turns the gameplay camera slowly around the player's own Link, shown
   through a clear window in the menu (hos_coop_menu_picture.h, `live`).

   One optional export, hos_coop_view, asked by hos_oot_free_view (host/hos_main.c) on the game's thread each time the
   game builds its gameplay view, before the Debug tab's free camera. It gets the game's own eye and target (metres, the
   world's axes, y up) and may replace them: 1 = the camera looks from `eye` at `target` this frame (the game's own
   matrix is rebuilt from them, as the free camera's is), 0 = the game's camera as it is. A module without it: the game's
   camera always. Generic: any module may frame the camera, with no further Hookshot build.
   The same file is in Hookshot's host/ through our patch and in our repository (hookshot/src): keep them equal
   (tests/hookshot/module_test.cpp checks it). Plain C. */
#ifndef HOS_COOP_VIEW_H
#define HOS_COOP_VIEW_H

typedef int (*hos_coop_view_fn)(double eye[3], double target[3]);   /* 1: replaced */

#endif
