/* SPDX-License-Identifier: GPL-3.0-or-later
   The other Links' sounds on Hookshot (oot-remake-multiplayer, docs/puppet.md "Link sounds").

   One wrapped game function more in the one wrap table (patch 0005's nx2/coop_world_wraps.py WRAPS; the ids are in
   the one wrap space of hos_coop_world.h). Patch 0014 adds its row and this file (Hookshot's host/hos_coop_sound.h is
   the same file). The module's hos_coop_wrap gets it; Hookshot announces it in HOS_COOP_WORLD_HELLO's bits. The row:
     (24, 0x440630, (0xd503237f, 0xa9bb7bfd, 0xa90167fa, 0xa9025ff8)),  # pacibsp; stp x29, x30; stp x26, x25; stp x24, x23
   Phase 0 only reads (never skips): x0 the xlink2 user (an actor's SLink or ELink user), x1 the asset cell (8 bytes:
   the asset, whose first word is its name's char*), x2 the handle. Plain C. */
#ifndef HOS_COOP_SOUND_H
#define HOS_COOP_SOUND_H

/* main+0x440630, xlink2's searchAndEmit (48 callers: the AI node OneShotXLinkSearchAndEmit, the player's and the
   actors' own code): every named sound or effect the game starts on a user. */
#define HOS_COOP_WRAP_SOUND_EMIT 24u

#endif
