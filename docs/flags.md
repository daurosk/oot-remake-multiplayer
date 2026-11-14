# The game's flag store

Where Ocarina of Time keeps its world flags, and how `coop_host` reads and writes them (`coop/host/include/coop/host/game_flags.hpp`). Game: main module of build e0f3ccfaf50065ae (1.1.1). Guest address = main base (0x7100000000 in the runs so far) + offset.

Sources: the integration project's `actor-map.md` (section 4), `player-state-map.md` and `save-format.md`, plus a static read of `main` for this page. **Checked** means read from the instructions. **Not run** means no live game confirmed it. Nothing on this page is confirmed in a run yet; see "Not done".

## Summary

- One object, `gmd::GameDataMgr`, holds every flag. Its pointer is at **main + 0x4e842a8**. Checked: the 272 code references load it through GOT slot 0x4c7e180.
- Named flags are keyed by the **Murmur3 x86 32-bit hash, seed 0** of their name (`player-state-map.md`). Each type has its own table.
- Chests, pots, enemies and other placed actors are **64-bit-key flags** (`Bool64bitKey`), keyed by the placement's 64-bit `Hash` in the Banc files, not by a name.
- Setters do not write the value. They queue the write, and the GameData module applies the queue on its next update.

## Layout

`M` is the manager. Type `t` (the save type ids of `save-format.md`) has its table at `M + 0x10 + 0x60 × t`:

| Offset in the table | Field |
|---|---|
| +0x00 | u32 entry count |
| +0x08 | Pointer to the entries |
| +0x10 | Pointer to the hash slots: 8 bytes each, {u32 hash, u32 entry index}, hash 0 = empty |
| +0x1c | u32 slot count |
| +0x20 | u32 queue size (slots) |
| +0x28 | Pointer to the queue |
| +0x30 | u32 queue word |

A lookup starts at `hash % slot count` and steps forward by one, wrapping, until it finds the hash or an empty slot. An entry index of the entry count or more is taken as 0 by the game. Checked at 0x833920.

| Type | Table | Entry size | Live value | Queue slot |
|---|---|---|---|---|
| Bool (0) | M + 0x10 | 0x18 | u8 at +0x16; saved copy at +0x17 | 8 bytes: u8 value at +0, u32 hash at +4 |
| BoolArray (1) | M + 0x70 | 0x40, has a vtable | bits at `*(+0x28)` in u32 words, lowest bit first; element count u32 at +0x20 | not read |
| Int (2) | M + 0xd0 | 0x28 | u32 at +0x1c | 12 bytes: u32 value at +0, u32 hash at +4 |
| UInt (20) | M + 0x790 | 0x28 | u32 at +0x1c | 12 bytes: u32 value at +0, u32 hash at +4, u32 1 at +8 |

All checked in the getters and setters below. The BoolArray element count at +0x20 is from a bounds test in the applier, so its meaning is inferred.

64-bit keys are separate, at **M + 0xc58**:

| Offset | Field |
|---|---|
| M + 0xc58 | u32 record count |
| M + 0xc60 | Pointer to the records, **sorted by key**, 16 bytes each: u64 key +0, u32 save-file mask +8, u8 live value +0xc, s8 file index +0xd, u8 flags +0xe (bit 1 is the saved copy) |
| M + 0xc68 | u32 queue size |
| M + 0xc70 | Pointer to the queue, 8 bytes a slot: u8 value +0, u32 **record index** +4, empty = 0xffffffff |
| M + 0xc78 | u32 queue word |

Checked at 0x838960 (binary search, value at +0xc) and 0xba24b0 (applier). The save-file mask and file index are as `save-format.md` reads them.

### The queue word

Bits 0–19 count slots taken, bits 20–27 count slots finished (they go back to 0 after 255), bits 28–31 are a round number that the applier moves on (wrapping after 14) each time it empties the queue. A setter takes a slot with compare-and-swap on the word, writes the slot, then counts it finished with a second compare-and-swap. When the round changed between the two, or the queue was full, it calls 0x34fc280, which ends the game (result 2162-0001). Checked at 0x682b20 and 0x608a10 (a second Bool setter, table at x0). Our writes therefore take a slot only while fewer than half the queue's slots are taken, and never write a value the game already holds (issue 175: a join snapshot written at once filled the Bool queue, and an actor's own set in Graveyard_Child ended the game).

## Functions

| Address | What | Status |
|---|---|---|
| 0x833920, 0x20d9c0 | Bool get (M, out, hash). Bool table, then BoolArray element 0 | Checked |
| 0x20fd90 | Bool get that first looks through the queue, newest first | Checked |
| 0x682b20 | Bool set (M, value, hash): queues | Checked |
| 0x837070 | Int get | Checked |
| 0x163b70 | Int set: queues | Checked |
| 0x57a4e0 | UInt get: the Int getter with every table offset 0x6c0 further on; a hash not found falls back to a second table (slots M + 0x800, count M + 0x80c), not read | Checked (issue 125) |
| 0x57af30 | UInt set: queues at +0x7b0 / +0x7b8 / +0x7c0 like the Int setter, but also stores u32 1 at slot + 8 (0x57b018); the fallback table goes to 0x376be00 on M + 0x7e8 | Checked (issue 125) |
| 0x607f70 | Float get (`f_7100607f70` in the recomp's main): the Int layout 0xc0 further on (type 4, M + 0x190), entries of 0x28, value at +0x1c | Read from the recomp source (branch `rando-items`) |
| 0xba6f80 | Float set (`f_7100ba6f80`): (M, value in s0, hash in w1), queues {f32 value, u32 hash} in 12-byte slots at +0x1b0 / +0x1b8 / +0x1c0. Found by scanning the recomp source for a setter with the Int setter's shape at the Float offsets; the same scan finds the known Int (0x163b70) and Enum (0x3e0980) setters and nothing else | Read from the recomp source, not run in a game yet |
| 0x838960, 0x3761680 | 64-bit key get (M, out, key) | Checked |
| 0x3761720 | 64-bit key set (M, value, key): finds the record, then 0xf071c0 queues it | Checked |
| 0xb99980 | Applies the queues (M). Called from 0x3b500a0, in the GameData module | Checked for Bool and 64-bit keys |
| 0xba24b0 | Applies the 64-bit key queue, from 0xb99980 | Checked |
| 0x1739e0 | Flag name hash (string, seed) | `player-state-map.md` |

What the applier does for a Bool slot: when the value differs, it stores the live byte (+0x16), the saved copy (+0x17, unless entry +0x10 bit 31 is set or M + 0xd0c is not 0), marks the entry changed (+0xc = 1, +0x14 = 0), ORs the entry's file mask (+8) into M + 0x60, and adds the hash to a change ring at M + 0x48. It stops at the first empty slot, clears the slots it walked, and moves the round on. For a 64-bit key slot it stores +0xc, the saved bit in +0xe (when the file index is not negative), and ORs the mask into M + 0xca8. Checked.

So writing the live byte directly would skip the saved copy and the save-file mask: the change would be lost on save. `game_flags` queues instead, exactly as the setters do, and its reads look through the queue first, as 0x20fd90 does, so a value read straight after a write is the new one.

## Which flags are which

| Kind | Where | Evidence |
|---|---|---|
| Chest opened | 64-bit key = the chest's placement `Hash` | Of the Banc placements whose `Hash` is a Bool64bitKey, 235 are `Obj_TBox_*`. A chest's blackboard has `IsOpened`. Checked statically, **not run** |
| Pots, rupees, Gold Skulltula tokens, rocks, enemies | 64-bit keys, same way | The other placements in the key list (`Obj_Pot` 511, `Obj_Rupee` 170, `Obj_RupeeToken` 85, enemies). Checked statically |
| Events seen, songs, dungeon state | Named Bool and Int flags | Names hash into the Bool table (`player-state-map.md`). Which name is which was not mapped |

`ResetTypeValue` in the flag list tells persistent keys from ones reset on a scene change: chests are 24 (0x18), most pots and enemies 88 or 280. Which value means what is inferred, not read.

## What this means for shared progress

- Chests, small-key chests and heart pieces in the world are 64-bit keys. The protocol's flag key has a 64-bit kind for them (`protocol.md`, "Shared flags"), and `GameFlagStore` gives `FlagWatcher` the named Bool and Int flags and the 64-bit keys (table 0, through `get_key64` and `set_key64`).
- The emulator must implement `WritableGameMemory`: a write and a 32-bit compare-and-swap that is atomic against the game's threads (or run them while the guest is paused).
- The list of shared flags should take only persistent keys (reset type 24), or a pot broken by one player would break for everyone.

## Story and cutscenes (issue 173)

A player test (2026-11-11): both players had to watch Saria at the forest exit and the owl before Hyrule Field, and a guest whose save had not beaten Gohma still had the invisible wall at the forest exit although the host's save had. Story flags were not shared at all then: each game played its own story scenes. Now the story is shared like the rest of the progress, so the host's save decides it (the join snapshot, `FLAG_RESET` on the host's save load, "Saves" below). The list is `CoopStory::Flags()` (`lichii/hook/coop_story.cpp`, 207 saved Bools, every name hashed and found in `GameDataList` with `ResetTypeValue` 8); `LICHII_COOP_STORY=0` turns it off. No new message: they are ordinary shared flags.

- **Scenes seen.** A story scene leaves the saved Bool `HavePlayedEvent.<scene>`. Saria's goodbye on the bridge is `Dm_Kokiri_0150` (0xd711a296: Saria, the Fairy Ocarina; Saria's schedule reads it), the owl at the entrance of Hyrule Field `Dm_HrlField_0010` (0xd4ef7853: `Npc_Kaepora`), the Deku Tree's last scene `Dm_Kokiri_0140` (0xebaec07f). Shared: `Dm_<area>_*` of the overworld (Kokiri, `Deku_0000`, HrlField, HrlCastle, LostWood, SacredForest, LonLon, Kakariko, Graveyard, DeathMtTrail, DeathMtCrater, Goron, Zora, ZoraRiver, ZoraFountain, LakeHrl, Gerudo, GerudoValley, DesertColossus, the songs `MelodyGet`, the Great Fairies `FairyFountain`), and the story events of the spiritual stones' quests (`Get_StoneOf…` outside the dungeons: Dodongo's Cavern opening, Darunia, the bomb bag), the "go to" prompts (`Goto_…`), the Door of Time scenes in Hyrule Field and the carpenters freed.
- **The owl's talks**: `HaveTalked_Special.Npc_Kaepora_*`.
- **Story gates in the world.** An overworld placement can carry `Presence: {FlagName, IsNegation}`: it is there only while the flag is set (or not set). Read from every `Banc/<area>/*.bcett.byml`. The forest-exit wall is `FldObj_AirWall_Player_GenerateNavMesh` at (-82.0, 115.1, -38.0) in `KokiriForest_Child_Dynamic`, present while `KokiriForest_Npc_Kokiri_007_Child_Blocker_00` (0xe5dc5674, default 1) is set: the Kokiri who blocks the exit. Mido's wall on the Deku Tree path is `KokiriForest_Npc_Kokiri_Mido_Child_Blocker`. Shared, one by one: those blockers (also adult Mido's in the Lost Woods), Jabu-Jabu's mouth, Kakariko's ladder and well water, the windmill song, Lake Hylia's ice, the royal tomb, the Death Mountain gate guard, Talon and Malon, the four Gerudo carpenters, the light prelude, the owl at Lake Hylia. A blocker goes from 1 to 0, so this is a plain shared value, not a one-way rule.
- **Quests that follow the story (issue 130).** Some placements test a quest's step, not a flag: a `Presence` on a BoolExp such as `Meet_DekuTree_IsAfter_Step02_Exp` ("the quest's Enum is at Step02 or later"; GameDataList `BoolExp`, the quest's steps in `RSDB/Challenge`). The Kokiri Shop's keeper and goods are there only from Step02 of `Meet_DekuTree` (Enum 0x3372689b: NotReady, Ready, Step01, Step02, Step03, Complete); before that the keeper is `Npc_Kokiri_001_Shop_Child_ShortStock` with sold-out boards. Only Mido's talk moves the quest to Step02 and Step03 (`Npc_Kokiri_Mido_Child` event flow, which also clears his blocker), and `Dm_Kokiri_0075` to Complete. With his blocker shared, a player whose Mido stepped aside for another player could never talk him into Step02, so that player's shop never opened. The Enum is not shared (an Enum hash has no order, and each game's opening writes its own step); instead the hook raises its own quest from the shared story each frame (`CoopStory::QuestCatchUp`, `CoopHook` `CatchUpQuest`): Mido's blocker 0 gives Step03, `Dm_Kokiri_0075` played gives Complete; only ever up, and only from Ready, Step01 or Step02. Log: `coop: story: quest Meet_DekuTree ... -> ...`. Unit tests in `tests/lichii/story_test.cpp`; not yet run in a game.
- **Left out on purpose**:
  - the opening of a new game (`Dm_Kokiri_0000` to `_0030`): a new save must still wake up. Leaving the house is shared (below);
  - time travel (`Dm_TimeTemple_*`): a child who has not pulled the sword must still be able to. The Ocarina of Time and Song of Time scenes in Hyrule Field are shared: what they give (the ocarina, the song) is a shared item, and the Door of Time itself stays each game's own;
  - the Chamber of Sages (`Dm_SageRoom_*`, `SageRoom_*`, the sages' awakenings `Dm_Awake_*`): the first sword pull and the scenes after an adult temple's blue warp. The Chamber has no exit to walk out of, the scene moves Link out, so each game plays its own; the medallions are shared items;
  - system scenes (`Dm_SY_*`, `DmF_*`: item gets, bottles, Game Over), the title, credits, ending, Ganon's Castle and the final battle;
  - every scene and story event inside a dungeon (`Dm_<dungeon>_*`, `Demo_*`, the Spirit Temple's `SageOfSpirit_*`): a dungeon's state is `coop_dungeon.cpp`'s ([dungeon.md](dungeon.md));
  - bosses: `Gohma_DefeatBoss`, `Dm_*_BossIn`, `Dm_*_BossOut` stay each game's own ([sync-coverage.md](sync-coverage.md), D9). The story after a boss (the Deku Tree's death, the blocker stepping aside) is shared, so a guest whose save has not beaten Gohma can leave the forest; the Deku Tree's boss room stays as that guest's save has it;
  - one player's own: Navi, talks (other than the owl's and the Kokiri Shop keeper's, below), hints, tutorials, minigames, arrivals (`ArriveFrom…`), the scarecrow, Gossip Stone fairies, weather and age (`Sequence_IsChildhood`).
- Tests: `tests/lichii/story_test.cpp` (the tester's four flags are in the list; nothing of the dungeons' list, time travel, the opening or bosses; A's scenes reach B; a guest with an early save joins a host past Gohma and gets no wall and no Saria; the host's save load puts a wall back).
- Not seen in a game yet: that a scene marked played is not played again (Saria and the owl), and that the wall goes when the blocker flag goes to 0 in a forest that is already loaded (else on the next load of the forest).
- **Kokiri Forest, from Alex's play-test (2026-11-12, issue 173).** Not shared then: Link's house door, the Kokiri Shop keeper's talk, the Deku Tree's talk. Read from the romfs: leaving the house plays `Dm_Kokiri_0045` (the door, which sets `KokiriForest_LinkHouseDoor_Open`, 0x9b1b27d2) and then `Dm_Kokiri_0050` (Saria at the door); the wake-up before it (`OpeningEvent`: `Dm_Kokiri_0000`, `_0030`, and Meet_DekuTree Step01) stays each game's own. The Deku Tree's talk `Dm_Kokiri_0070`/`_0075` was already shared; `_0075` also sets `KokiriForest_Meet_DekuTree_OpenDekuTreeMouth` (0x9a60a460), now shared, and the quests it moves (Meet_DekuTree Complete, Get_StoneOfForest GoInside) are caught up by shop-shield-2's Meet_DekuTree rule and threads-of-time's quest sharing, not here. The shop keeper's talks `HaveTalked.Npc_Kokiri_001_Shop_Child` (0x22d6ff6f) and `HaveTalked_Npc_Kokiri_001_Shop_Child_PresentDekuShield` (0xbfc69399) are shared (kind `talk`); her Navi line and `KokiriForest_Meet_DekuTree_BoughtDekuShield` are not (the shield is shop-shield-2's). Each name hashed and found in `GameDataList` (saved Bools, ResetTypeValue 8). Not seen in a game: that a door already marked open lets a player who is still inside walk out (if the door stays shut until the next load, that player re-enters the house once).
- **Quests catch up with the story (issue 173, `lichii/hook/coop_quest.{h,cpp}`).** The game's quests (the `Challenge` table) are saved Enums each game keeps (values are step-name hashes, not ordered, so not shared). Scenes move them on: meeting Zelda (event flow `Goto_HyruleCastle`, entry `Goto_HyruleCastle_MeetZelda`: scenes `Dm_HrlCastle_0030`, `0035`, `0045`, `0050`, then Impa's `0060`) sets `Goto_HyruleCastle` (Enum 0xdb89c1a9) to Complete and `DoorOfTime` (0x378913ca) to Step01. Impa's courtyard placement (`Presence: NOT DoorOfTime_IsAfter_Step01_Exp`), her Step01 schedule and the `HyruleCastle_Zelda_CanbeStopped_Impa_01/02` events (she stops Link when he leaves, then takes him out for Zelda's Lullaby) all belong to Step01. In a game where another player had already met Zelda, those shared scenes count as played, the quest stayed behind and Impa never came (Alex's test, 2026-11-12). Each game now raises its own quests from the shared scenes every frame after a load: `Dm_HrlCastle_0060` played gives Goto_HyruleCastle Complete and then DoorOfTime Step01; the owl at the field entrance (`HyruleField_Goto_HyruleCastle_Kaepora` or `Dm_HrlField_0010`) Goto_HyruleCastle Step02; Talon woken (`Dm_HrlCastle_0020`, whose flow sets Step04) Step04. Only raised; Goto_HyruleCastle only from Step01 on (after our own Deku Tree); DoorOfTime only once our Goto_HyruleCastle is Complete; DoorOfTime is never raised past Step01, so each player meets Impa. Log `coop: story: quest DoorOfTime 0x57b849f0 -> 0xeb62add9 (shared story; issue 173)`. Off with `LICHII_COOP_STORY=0`. Tests: `tests/lichii/quest_test.cpp`. Read from the romfs (event flows, Banc, GameDataList), not seen in a game: whether Impa's schedule starts at once in a courtyard that is already loaded or only on the next load.

## Threads of Time (issue 193)

A player test (2026-11-12): one player could not open the pause menu's "Threads of Time" tab at all, and its progress was each game's own. The tab is the game's quest log (USen `LayoutMsg/MenuOverlay_00`; the screen is `ChallengeMenu_00`, `Sequence/ChallengeLog.module.ainb`). It lists the quests of `RSDB/Challenge.Product.111` (45 rows, the 21 without `IsListAdventureLog: false` are listed).

- **Why one player could not open it.** The tab is masked until the saved Bool `UI_IsFinishChallengeMenuOpenEvent` (0x1450c74f) is set, and only the owl's event at the entrance of Hyrule Field sets it (`Event/EventFlow/Goto_HyruleCastle.bfevfl`, entry `Goto_HyruleCastle_Kaepora_00`, scene `Dm_HrlField_0010`). That scene is a shared story flag (above), so the second player never plays it and the flag stayed 0 in that game. It is now a shared story flag too (`CoopStory::Flags()`). `UI_IsOpenChallengeMenuOnce` (0x6203007a, the first-open prompt) stays each game's own.
- **Its progress.** Each quest is one saved Enum (`GameDataList`, `ResetTypeValue` 8) whose values are, in order, `NotReady`, the quest's steps (the RSDB row), `Complete`; a value is `flag_hash` of the step's name. `lichii/hook/coop_quests.cpp` (`CoopQuests::Store`, a flag store layer) shows the watcher each shared quest as an Int, the step's place in the list, with the max rule (`FlagStore::is_max`): every game is at the furthest step any player reached (the union of the group's progress), a guest who joins gets the group's steps, the host's save load sets them exactly (`FLAG_RESET`). A step that comes in while our Link is in a dungeon waits until he is out. A higher step that waits is kept when our own game takes a lower step meanwhile (the watcher keeps a held max-rule value above the game's own), so the player is not left behind after the dungeon. No new message type. `LICHII_COOP_QUESTS=0` turns it off. Log: `coop: quests: <quest> <from> -> <to>`.
- **Shared quests (11, and `Get_Epona`'s end)**: `Get_StoneOfForest`, `Get_StoneOfWater`, `Goto_HyruleCastle`, `Meet_DekuTree`, `Meet_GreatFairy`, `SageOf` Fire, Forest, Light, `SubEvent_GetMelodySun`, `SubEvent_GetScarecrow`, `SubEvent_GetStormSong`. The Enum names are not in the romfs: each is found by its values (`Meet_DekuTree` is 0x3372689b; two minigames have the same step list). `Goto_HyruleCastle` moves a player on only once their own is at `Step01` (set as Link leaves the Deku Tree), so a player still in the opening is left alone (time-story's DoorOfTime rule reads it). A game whose quest value is unread or not in the list takes no step.
- **Shared by its end only**: `Get_Epona` (0x92431c55). Its steps stay each game's own (the song as a child, the race against Ingo, which goes back a step when lost), but a game at `Complete` makes every other game's quest `Complete` (`Quest::complete_only`: read as `NotReady` until then, only `Complete` is ever written, the host's save load never lowers it, and a game in the race or riding out after it finishes by itself). So once one player has won Epona, every adult can call and ride their own ([puppet.md](puppet.md#riding-issue-199)).
- **Left out**: minigames and the quests the tab does not list (their steps go back and forth), `SubEvent_GetSwordBigGoron`, `SubEvent_MaskSalesman` (one player's own items), `DoorOfTime` (time travel), `AllSage` (its steps are not in an order), `Defeat_Ganon`, `EscapeFromCrumblingGanonCastle`; for now also `Get_StoneOfFire`, `SageOfShadow`, `SageOfSpirit`, `SageOfWater`, whose values look out of order or branched (to check against their event flows).
- Tests: `tests/lichii/quests_test.cpp` (the table; read as a place; raise, never lower; the host's load sets; A's step reaches B and B's reaches A; a late guest gets the steps; the unlock flag is in the story list).
- Not seen in a game yet: that the game takes an Enum write of a quest and the tab shows it on open, and that the tab opens for the second player once the flag is 1.

## Live check

Run in the patched emulator (`lichii/hook/coop_flags.cpp`, settings in [lichii.md](lichii.md)) with a child save in the Kakariko graveyard, two instances and a local server, 2026-11-08.

- The tables read live: `main at 0x80d68000` (it moves per run), `bool table, 11292 entries`, `int table, 1259 entries`, `key64 table, 11657 records`.
- **The flower-grave chest is a named Bool, not a 64-bit key.** Opening it changed one flag that stays: `bool 0x39e4b7ec (entry 9361) 0 -> 1` (with the rupee Ints). No 64-bit key changed. So at least some chests are Bool flags; the "Chest opened" row above holds for the Banc chests, not for every chest.
- The ReDead under that grave is a 64-bit key, `0xf9b880ab096a9cd6` (record 11382): 0 -> 1 when killed, back to 0 on the next scene load, like the other enemy and pot keys (a scene load flips about 40 keys at once).
- `set_key64` and `set_bool` write through the queue and the game takes them: `set_key64(0xf9b880ab096a9cd6, 1) queued (before 0, now 1)`, then the table scan shows `0 -> 1`. A write made before the save has loaded is lost: loading the save puts the old value back. So a build must not write flags until the save is in (`LICHII_COOP_SET_AFTER` delays the debug writes).
- **A flag crosses from A to B.** Both instances shared `0x39e4b7ec`. Opening the chest in A logged `0 -> 1` in A, and B logged `0 -> 1` within the same second (after allowing for B starting 3.5 s later); in B the chest was already open when its player got there. The same worked earlier with a debug write (`LICHII_COOP_SET_BOOL`) on another Bool: moving A's write later moved B's change with it.

Not checked live: an Int or 64-bit key crossing, B to A, a late joiner getting the host's table.

## Saves: the host's save decides (branch `host-save`)

Alex (2026-11-11): "Let's make it follow the hosts save." Before, the group's state was the union of everyone's progress, and it survived any one player's load. So a newer state kept coming back into an older save: in #148 a later state's shut boss door locked a player out.

- **A load window** (`FlagWatcher`): from the first frame without our Link (the title screen, a save load, a door, a cutscene, a warp, a death) to the end of the 120 settle frames after it. Nothing is sent while there is no Link. **A save load is known by two signs in one window**: no scene (Banc) was loaded for at least 30 frames (the title screen and file select: the game has no title Banc, so `PlayerReader::read_scene` reads nothing there), and at least 2 named shared flags went back (a Bool from set to 0, an Int lower) while there was no Link. A door, a warp song, a blue warp, a cutscene, time travel or a Game Over continue keeps a scene loaded, so a room's "enter" flags going back on a door, or the story flags of a cutscene, never make a reset. The one-way room flags the game resets itself never count. Not seen in a run yet: that no scene is read on the title screen and file select, and is read through a door. The hook does not log the scene's absence yet, so check `host_resets` and the `coop: flags: the host loaded a save` line instead. Not done: a hook on the game's own save load (`gmd::SaveDataMgr`'s `read`, or the sequence node `SeqOneShotOnloadGameSaveDataWorldMgr` at 0x32d870c), which would be the surest sign. Without that sign the rules before host-save hold: a guest puts the group's value back at once by the old `backwards()` rule, and what is left goes out when Link is back. A save that differs from the group in only one flag that went back is therefore not seen as a save load; the next host load or progress merges it.
- **The host's save load**: nothing is put back. When the window ends, every shared flag whose value differs from before the load goes out as `FLAG_RESET` ([protocol.md](protocol.md), "Saves"). The server replaces its values with them; every player writes them exactly (`FlagStore::write_exact`, with no max, OR, mirror or one-way rule). A host's first save load after the title screen does the same: the group starts from the host's save.
- **A guest's save load**: nothing is sent. Each flag the load moves that the group has a value for is written back at once (so a room loading now has it). When the window ends, any other flag the load changed goes back to what this game had before it. The group never changes from a guest's load.
- **While playing**, changes merge as before (OR, max, counts). Only a host's save load resets.
- **Per player** stays per player: rupees, ammo counts, life, position (not shared at all).
- **A new host** (the old one left) sends its table as before (merged); no reset.
- **One-way rules** (map and compass, chests, pickups) hold for `FLAG_STATE`. A `FLAG_RESET` sets them back to 0 when the host's save has 0.
- **One-way room flags** (dungeon-live's live room flags, which the game resets itself) are never in a `FLAG_RESET`, never put back by a guest's revert, and never count as a sign of a save load.
- **Hearts**: a reset that lowers `PlayerStatus.MaxLife` holds the player's own `PlayerStatus.Life` to the new maximum.
- **Hooks** for session-wide state the host's save undoes: the server's `Session::on_host_reset()` and the game's `FrameReport::host_reset` (logged `coop: flags: the host loaded a save: the group follows it`).
- Limits:
  - A guest's own change in the 2 s after its save load is put back with the load's.
  - Not cleared by a reset yet (each owner adds its clear at the hooks): world breaks, places and hits kept by the server for late joiners, field grass cuts, sign cuts, boss room state, drop takes, the kills-late dead list, locked doors' kept breaks. Pots, grass and enemies come back on a room load anyway.
  - An equipped item the host's save lacks is taken out of the pouch while still equipped (the Equip strings are per player and not touched). Not seen in a run.
  - The dungeon-live hold keeps a reset's 1 for a gimmick event until the player leaves the dungeon (a 0 goes through).
- Tests: `tests/host/flag_watcher_test.cpp` ("the host's save load resets the group to its save", "a door is no save load", "a guest's save load takes the group's values", "a FLAG_RESET is written exactly ... #148", "lowers an item", "a new host does not reset"), `tests/server/session_test.cpp`, `tests/protocol/codec_test.cpp`.

## The host's save file: guests play from it (branch `host-save-load`, issue 167)

A tester (2026-11-11): every player loaded a save by hand, and a guest whose own save had not beaten Gohma kept the forest-exit wall although the host had beaten him. Now a guest plays from the host's save file itself, not only its shared flags.

- **The co-op save folder.** A guest's game reads and writes `<title>-coop` beside its own save folder (`nand\user\save\0000000000000000\<user>\0400486024DEC000-coop`; `lichii/patches/0006-coop-save-folder.patch`). Its own saves are never read for play and never written: the host's save goes only into the co-op folder (`write_coop_save` refuses any folder whose name does not end in `-coop`), and the player's own `option.sav` and `account.sav` are copied in (never moved) the first time.
- **Who uses it** (`save::folder_choice`, issue 167). It is never decided from `coop.ini`'s address: an invite join goes to `127.0.0.1` (the join helper), and an old address may be a friend's while the player now hosts.
  - **On the emulator**, `LICHII_COOP_HOST_SAVE` decides when set: play.ps1 sets it to 1 for a join (by address or invite code: `Join.bat`) and to 0 for a host, or `coop-options.ini` sets it. **On Hookshot** it is not used: the title menu may Host or Join after any start, so the session decides (below), and play.ps1 does not set it.
  - **On the emulator** the folder is fixed when the game opens its save (patch 0006), so without the variable it is the player's own.
  - **On Hookshot** (patch 0008) the session decides while the game runs. hookshot.exe exports `hos_coop_fs_save_root` and `hos_coop_fs_save_remount`, which the module finds by name. At the first connection that has a host, a guest (the title menu's Join, or an invite) asks for the host's save and stays on its own folder; the co-op folder's slots are taken out. When the host's save is in, it is written whole into `<user>\save\<uid>-coop` (each slot into `slot_NN.part`, then renamed) and only then is the account save mounted again from there: the title never offers an empty or half-written co-op folder (pressing Continue on the empty one crashed the game, 10.1 offline runs). The switch happens only while the title's menu is up (never while a file loads), and is dropped for the session once the player picks a file on the title (their own save, a new game), the title says the save did not come, or Link is in the game: `coop: save: <why>: this game stays on its own saves; the host's save is not used`. The host keeps its own. A game that already has a save loaded keeps its own: switching then would mix two saves.
  - The log says which: `coop: save: a guest: this game plays from the co-op save folder ...`, `... the host: this game uses its own saves`, or the `LICHII_COOP_HOST_SAVE` line.
  - **Tests only:** `LICHII_COOP_TEST_OWN_SAVE=1` keeps a guest on Hookshot on its own save folder and asks for no host's save (`coop: save: LICHII_COOP_TEST_OWN_SAVE: this guest keeps its own saves (a test)`), so each autotest game plays the save its scenario put in. `tools/autotest/hookshot.py` and `hs_steps.py` set it for both games unless a scenario's switches set it (`LICHII_COOP_TEST_OWN_SAVE=0` for a host-save test). Players never set it.
  - `CoopSave::HostSaveReady()` says when the host's save is in the folder, for a title menu that loads it by itself (not done yet: the chat says "The host's save is ready").
- **On joining**: the guest asks once (`SAVE_ASK`; the host answers each player once while it stays, and logs `coop: save: the host's save (transfer N) stopped at part P of C for 10 s` on a guest whose transfer stalls); the host sends its newest slot (`SAVE_PART`s, [protocol.md](protocol.md), "Save transfer"); the guest writes it into `slot_00` of the co-op folder, every other slot already there and every slot name of its own folder, so the load menu offers only the host's save. The host answers at once with the newest save on disk, also right after its own save load; a host with no save on disk answers "no save" at once and the title says "<host> has no save yet: New Game starts one, Continue plays your own." With nothing within a minute the title says the save did not arrive (Continue plays the player's own save, New Game a new one). A slot that comes before the game opened its save folder is kept and written when it does. Chat: "The host's save is ready: load it (Continue, or Load Game, newest save)."
- **The host loads another save**: the `FLAG_RESET` above already sets every guest's shared flags to it at once. A guest that asks now gets the newest save on disk at once. The host's game also waits for the game's next save (a door, a save point; an older manual save loaded and a newer autosave on disk must not be mixed up) and sends that slot to every guest, who writes it into the co-op folder. Chat: "The host loaded a save. Load it from your co-op save to stand where it starts (your progress already follows the host)."
- **Per player** (decided for issue 167): loading the host's save gives the guest everything in it, the host's rupees, hearts, ammo and place included: the guest starts where the host's save starts (its entrance or save point). From then on rupees, ammo counts, life and position are each player's own again (not shared, as above). The guest's own saves keep its own progress for single player.
- **Not done**: the guest still loads the co-op save itself (Continue on the title screen, or Load Game while playing); nothing presses it for the guest yet (the game's own load call is not found, `SeqOneShotOnloadGameSaveDataWorldMgr` at 0x32d870c is a candidate). A guest that is in game when the host loads keeps playing where it is until it loads.
- Log lines: `coop: save: asked the host (player N) for its save`, `coop: save: player N asked for our save`, `coop: save: we loaded a save: a guest that asks gets our newest save at once, ...`, `coop: save: the host has no save yet (a new game) ...`, `coop: save: now playing from the co-op save folder <folder>`, `coop: save: sending slot_NN (B bytes, P parts) to player N|every guest`, `coop: save: the host's save is in (B bytes, ...)`, `coop: save: the host's save written to <folder>`, `coop: save: the game's save folder is <folder>`.
- Tests: `tests/host/save_transfer_test.cpp` (packing, parts and CRC, refusals, the host's wait after its load, the newest slot, never over the player's own saves), `tests/server/session_test.cpp` ("the host's save: ..."), `tests/protocol/codec_test.cpp` ("save ask and save part ...").

## Through the game's own functions (Hookshot)

On the emulator the flag watcher reads every shared flag every frame and compares (a diff of the tables), and co-op's writes go into the game's write queues from outside, as the setters would. On Hookshot the game is C, so both halves use the game's own code (`coop/host/include/coop/host/game_flag_events.hpp`, `lichii/hook/coop_flag_native.h`, [hookshot.md](hookshot.md) "World hooks").

- **What the game writes, as events.** Every flag write of the game waits in a queue until the applier (0xb99980) takes it. Patch 0005 wraps the applier; just before the game's own function runs, the module reads the queues (Bool, Int, Enum, UInt, 64-bit keys: `queued_flag_writes`) on that game thread and puts each write into a ring (`GameFlagEvents`, no lock). The hook's frame hands them to the watcher (`FlagWatcher::game_wrote`), which then reads **only those flags**, not the whole list.
- **The whole list is still read** on the first frame (the baseline), in every load window (no Link, and the settle time after: a save load or a room load moves many flags and the window's rules look at each), when a flag joins the list, and when the ring overflowed.
- **The audit.** Each frame the watcher also reads a small slice of the list (the whole list about every 240 frames). A change found there that no event named is handled like any other and counted (`event_misses`). So a write the applier's wrap does not see (a queue slot taken between our read and the game's, a type whose queue is not read, a writer that does not use the queues) costs a few seconds, never the change, and the log shows it: `coop: flags: by the game's events: N flag reads for its writes, M changes only the audit found, K whole-list reads (loads)`. M staying 0 through a play session is the proof that the events are complete; then the audit can go.
- **What co-op writes, through the game's setters.** `GameFlags::set_*` puts each write into a list (`GameFlagCalls`) and writes nothing into the game's memory. On the game's calc thread, at the frame hook, the module calls the game's own setter for each (0x682b20 Bool, 0x163b70 Int, 0x3e0980 Enum, 0x57af30 UInt, 0x3761720 64-bit key: `(manager, value, hash or key)`, read from the instructions), at most 256 a frame and only while that queue is less than half full (the game's own setters stop the game on a full queue). A read sees a write that still waits, as it sees a queued one. A flag written twice before the setter ran is set once, to the newer value.
- **Switches** (both on unless set to 0): `LICHII_COOP_FLAG_EVENTS=0` reads the tables every frame again; `LICHII_COOP_FLAG_SETTERS=0` writes into the game's queues again. A setter that cannot be called turns the second off by itself, with a warning.
- Log lines: `coop: flags: the game's writes as events ..., our writes through the game's setters ...` at the start; every 10 s `coop: flags native: the game's applier ran N times, W writes read from its queues (D dropped), S setter calls made for co-op (P waiting, R frames waited for room)`.
- Tests: `tests/host/flag_events_test.cpp` (the ring, also from several threads; the call list; the watcher by events: only written flags are read, the audit finds and counts an unnamed change, load windows and lost events read everything, no echo of a received value) and `tests/host/game_flags_test.cpp` (the queues decoded; the setters route to the call list and reads see it; room).
- Not seen in a game yet: all of it. To check in a run: the two log lines above, M = 0 after opening chests, taking items and walking through doors, a flag crossing both ways as fast as before, a join snapshot going in through the setters (S grows, P back to 0), and a save load still seen (`coop: flags: save load seen`).

## Not done

1. The other types (UInt, Enum and Float are read, above): their tables follow the same pattern (M + 0x10 + 0x60 × t) but were not read.
2. The queue sizes (read at run time, never logged).
3. BoolArray writes, Struct (+0xc20) and BoolExp (+0xc48).
