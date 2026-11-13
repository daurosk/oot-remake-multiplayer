/* SPDX-License-Identifier: GPL-3.0-or-later
   Local co-op's pads (oot-remake-multiplayer, docs/local-coop.md; Hookshot patch 0010).

   Exports of hookshot.exe that the module finds by name (GetProcAddress on the exe / dlsym), as patch 0008's: an exe
   without patch 0010 has none, and local co-op is then not offered. host/nn_hid.c and host/pad_sdl.c have them. The
   pads are the controllers SDL has open, slot 0 and up; a pad that reconnects into a slot has a new serial.

   - hos_coop_pad_info(slot, &serial, name, cap, &npad): -1 past the last slot, 0 no pad there, 1 a pad: its serial,
     its name, and the Npad a local player took it as (0 none, 1 to 3: Npad No.2 to No.4). Any pointer may be NULL.
   - hos_coop_pad_read(slot, &buttons, stick): its buttons as the game would get them (Switch NpadButton bits, the
     menu's binds) and its sticks (left x, y, right x, y), taken or not; 1 when a pad is there. Any thread.
   - hos_coop_pad_take(slot, serial, npad): npad 1 to 3 makes the pad of that serial Npad No.2 to No.4: it leaves player
     1's merged pad, its rumble stops, and the game's reads of that Npad get it (another pad that was that Npad is given
     back); npad 0 gives it back. 1 when done.

   Plain C, so both sides' compilers agree. */
#ifndef HOS_COOP_PADS_H
#define HOS_COOP_PADS_H
#include <stddef.h>
#include <stdint.h>

#define HOS_COOP_PAD_INFO "hos_coop_pad_info"
#define HOS_COOP_PAD_READ "hos_coop_pad_read"
#define HOS_COOP_PAD_TAKE "hos_coop_pad_take"
typedef int (*hos_coop_pad_info_fn)(int slot, uint32_t *serial, char *name, size_t cap, int *npad);
typedef int (*hos_coop_pad_read_fn)(int slot, uint64_t *buttons, int32_t stick[4]);
typedef int (*hos_coop_pad_take_fn)(int slot, uint32_t serial, int npad);

#endif
