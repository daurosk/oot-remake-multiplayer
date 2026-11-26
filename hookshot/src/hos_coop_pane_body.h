/* SPDX-License-Identifier: GPL-3.0-or-later
   Two more text pane setters for the play-test bridge (oot-remake-multiplayer, docs/playtest.md "Text boxes"): the
   ones that skip the set-message call of patch 0041 (main+0x439520) and call the text box's set-string themselves.

   Two wrapped game functions more in the one wrap table (patch 0005's nx2/coop_world_wraps.py WRAPS; the ids are in
   the one wrap space of hos_coop_world.h). Patch 0042 adds their rows and this file (Hookshot's
   host/hos_coop_pane_body.h is the same file). The module's hos_coop_wrap gets them; Hookshot announces them in
   HOS_COOP_WORLD_HELLO's bits. The rows:
     (26, 0x76a460, (0xd503237f, 0xa9bc7bfd, 0xa9015ff8, 0xa90257f6)),  # pacibsp; stp x29, x30; stp x24, x23; stp x22, x21
     (27, 0x377a990, (0xd503237f, 0xa9bc7bfd, 0xf9000bf7, 0xa90257f6)),  # pacibsp; stp x29, x30; str x23; stp x22, x21
   Phase 0 only reads (never skips). 26: x0 the pane (as 0041's), x1 the text, w2 its length (the low 16 bits); its
   only caller is a letter-animated pane's set-text (a LetterAnimControl's vtable slot 7), so a dialogue box's
   text. 27: x0 the pane, x1 a reference {+0 the text, +8 its length, +0x10 the attributes} as 0041's; called for
   each text pane of a layout whose alias matches. Read from main only, not yet seen in a run. Plain C. */
#ifndef HOS_COOP_PANE_BODY_H
#define HOS_COOP_PANE_BODY_H

/* main+0x76a460, a letter-animated text pane's set-text (pane, text, length). */
#define HOS_COOP_WRAP_PANE_BODY 26u
/* main+0x377a990, the set-text of each pane with a matching alias (pane, &{text, length}). */
#define HOS_COOP_WRAP_PANE_ALIAS 27u

#endif
