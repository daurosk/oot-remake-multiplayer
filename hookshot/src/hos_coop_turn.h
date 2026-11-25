/* SPDX-License-Identifier: GPL-3.0-or-later
   The co-op module may turn the player camera (oot-remake-multiplayer, docs/hookshot.md "The camera's angles"; our patch
   0040): the Multiplayer tab's Customize page turns the game's own camera around the player's Link, so the game's own
   collision places the eye (a wall or a low ceiling in the way brings the eye nearer, as in play).

   One optional export, hos_coop_turn, asked by hos_mouse_camera (host/mouse_native.c) on the game's thread at the start
   of each update of the player camera (f_80644c80), before the camera reads its input, whatever the Mouse camera row is.
   It gets the camera's yaw and pitch (degrees: yaw falls as the view turns right, pitch rises as it tilts down) and its
   state (+0x204: 3 while it follows Link by itself, 4 while the player turns it, others: another camera owns the view).
   1 = the camera takes the module's yaw and pitch this frame (only in states 3 and 4; the yaw is wrapped into -180..180
   and the pitch kept in the camera's own limits), 0 = the camera as it is. A module without it: the game's camera always.
   Asked with yaw and pitch NULL and state -1 (the view hook, each frame the camera's update has not run for 50 ms; our
   patch 0043): 1 = the module wants to turn the camera, so the host wakes its update (mouse_native.c
   hos_coop_camera_wake), 0 = leave it. Nothing may change on that call.
   Generic: any module may turn the camera, with no further Hookshot build.
   The same file is in Hookshot's host/ through our patch and in our repository (hookshot/src): keep them equal
   (tests/hookshot/module_test.cpp checks it). Plain C. */
#ifndef HOS_COOP_TURN_H
#define HOS_COOP_TURN_H

typedef int (*hos_coop_turn_fn)(float *yaw, float *pitch, int state);   /* 1: yaw and pitch replaced */

#endif
