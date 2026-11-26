/* SPDX-License-Identifier: GPL-3.0-or-later
   A save loaded into the game, on Hookshot (oot-remake-multiplayer, docs/flags.md "A load window").

   One wrapped game function more in the one wrap table (patch 0005's nx2/coop_world_wraps.py WRAPS; the ids are in
   the one wrap space of hos_coop_world.h). Patch 0024 adds its row and this file (Hookshot's host/hos_coop_save_load.h
   is the same file). The module's hos_coop_wrap gets them; Hookshot announces them in HOS_COOP_WORLD_HELLO's bits. The
   rows:
     (6, 0x32d8690, (0xd503237f, 0xa9be7bfd, 0xf9000bf3, 0x910003fd)),  # pacibsp; stp x29, x30; str x19; mov x29, sp
     (7, 0x32c1db0, (0xd503237f, 0xa9bd7bfd, 0xf9000bf5, 0xa9024ff4)),  # pacibsp; stp x29, x30; str x21; stp x20, x19
     (8, 0x32c2080, (0xd503237f, 0xa9bf7bfd, 0x910003fd, 0xb000dea8)),  # pacibsp; stp x29, x30; mov x29, sp; adrp x8
     (9, 0x32bbce0, (0xd503237f, 0xa9be7bfd, 0xf9000bf3, 0x910003fd)),  # pacibsp; stp x29, x30; str x19; mov x29, sp
   Phase 0 only reads (never skips). x0 the node. Plain C. */
#ifndef HOS_COOP_SAVE_LOAD_H
#define HOS_COOP_SAVE_LOAD_H

/* main+0x32d8690, the one-shot body of the sequence node SeqOneShotOnloadGameSaveDataWorldMgr (its vtable
   main+0x4bb4558, slot 39, which the one-shot base main+0x32c9ae0 calls at vtable+0x138; no direct caller). The node
   is in Sequence/AfterLoadSaveData.module, which Sequence/OnLoadGameSaveData.module runs after its
   SeqExecuteLoadSaveData: every save the game loads, from the title's Continue and load list
   (TitleToMainWithLoadSave), the System menu's Load (_LocalModule_bde133631675: PlusMenu_00, SystemSaveLoad_00) and
   Game Over's Try Again (Retry: "Continue from last save."). It calls the WorldMgr's own on-load (main+0x3452050). */
#define HOS_COOP_WRAP_SAVE_LOADED 6u

/* main+0x32c1db0 and main+0x32c2080: the Enter of SeqExecuteLoadSaveData (vtable main+0x4ba6cf8 slot 29) and of
   SeqExecuteLoadTemporarySave (vtable main+0x4ba6e40 slot 29), the two nodes of OnLoadGameSaveData.module that load
   the save (the second, the temporary one); no direct caller. A save load starts: the game's flags are the new
   save's from here until HOS_COOP_WRAP_SAVE_LOADED, whether Link is still read or not. */
#define HOS_COOP_WRAP_SAVE_LOAD_START 7u
#define HOS_COOP_WRAP_TEMP_LOAD_START 8u

/* main+0x32bbce0: the Enter of SeqExecuteAutoSaveMgrLastGameOverSave (vtable main+0x4ba4a28 slot 29; no direct caller),
   which only Game Over's graphs run: Sequence/Retry.module (Try Again) and EventToTitle.module (Quit Game, then the
   title). A save load that comes after it with no title menu between is Game Over's Try Again. (SeqExecuteLoadSaveData
   also runs at the game's start, BeforeInitializeCommon.module: a start with no save in then.) */
#define HOS_COOP_WRAP_GAME_OVER_SAVE 9u

#endif
