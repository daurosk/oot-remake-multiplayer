/* SPDX-License-Identifier: GPL-3.0-or-later
   The texts the game puts in its text panes, for the play-test bridge (oot-remake-multiplayer, docs/playtest.md "Text
   boxes").

   One wrapped game function more in the one wrap table (patch 0005's nx2/coop_world_wraps.py WRAPS; the ids are in
   the one wrap space of hos_coop_world.h). Patch 0041 adds its row and this file (Hookshot's host/hos_coop_pane_text.h
   is the same file). The module's hos_coop_wrap gets it; Hookshot announces it in HOS_COOP_WORLD_HELLO's bits. The row:
     (25, 0x439520, (0xd503237f, 0xa9bd7bfd, 0xf9000bf5, 0xa9024ff4)),  # pacibsp; stp x29, x30; str x21; stp x20, x19
   Phase 0 only reads (never skips): x0 the pane (0xb0 bytes in its layout's array), x1 a reference {+0 the text,
   +8 its length in bytes, +0x10 the attributes}; the text box keeps the pointer, not a copy. Plain C. */
#ifndef HOS_COOP_PANE_TEXT_H
#define HOS_COOP_PANE_TEXT_H

/* main+0x439520, a text pane's set-message call: every text the game shows in a layout's text pane (the message
   window's line and speaker, menus, the HUD) goes through it. */
#define HOS_COOP_WRAP_PANE_TEXT 25u

#endif
