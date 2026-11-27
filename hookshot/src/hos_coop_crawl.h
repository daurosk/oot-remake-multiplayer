/* SPDX-License-Identifier: GPL-3.0-or-later
   The game's crawl's "room to stand up" check, on Hookshot (oot-remake-multiplayer, docs/play-online.md "Down but not
   out", issue 337 and the 0.12.1 downed-move fix).

   One wrapped game function more in the one wrap table (patch 0005's nx2/coop_world_wraps.py WRAPS; the id is in the
   one wrap space of hos_coop_world.h). Patch 0046 adds its row and this file (Hookshot's host/hos_coop_crawl.h is the
   same file). The module's hos_coop_wrap gets it; Hookshot announces it in HOS_COOP_WORLD_HELLO's bits. The row:
     (31, 0x301ac50, (0xd503237f, 0xa9bd7bfd, 0xf9000bfc, 0xa9024ff4, 0x910003fd, 0xd13b03ff)),
   Plain C. */
#ifndef HOS_COOP_CRAWL_H
#define HOS_COOP_CRAWL_H

/* main+0x301ac50: is there room for the player to stand up (x0 the player actor; a shape cast of its standing body).
   Returns a bool. Its one caller is the crawl's state step (ExecutePlayerCrawl, main+0x27d2fb0; x19 its node): in
   state 4 (crawling) a true answer goes to state 5, the get-up. In open ground it is true at once, so the game's crawl
   of a downed Link ended as soon as it began. The module answers 0 (no room) for its own downed Link only: the game's
   crawl then lasts the down (its clips, its stick movement, its camera); every other call runs the game's check. */
#define HOS_COOP_WRAP_CRAWL_ROOM 31u

#endif
