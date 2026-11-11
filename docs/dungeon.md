# Dungeons the same for every player

Status: the tables are read from the game's files; the sharing is tested on fake games (`tests/lichii/dungeon_test.cpp`); none of it is played through in a dungeon yet (issue 87).

Plan stage 5: "if someone triggers something in the Water Temple, all players see it happen". Switches, doors, torches, room clears and water levels. Breakables and movable blocks are [world sync](objects.md)'s; enemies are [objects.md](objects.md).

Game: main of build e0f3ccfaf50065ae (1.1.1), romfs of the same version. **Checked** means read from the game's files. **Live** means seen in the running game.

## Results

- **Switches, shutters, doors and torches have no flag of their own.** None of the 1,010 `CandleStick`, 574 `SwitchShutter`, 39 `StepSwitch`, 20 `EyeSwitch`, 18 `SwitchHit` or 98 `DoorChainLock` placements in the Banc files has a 64-bit key in `GameDataList` (checked). Only chests (247 of 546, reset type 24) and a few torches (22, reset type 280) do.
- **The dungeon's state is in named flags the room logic writes and reads.** A placement is linked to others only by `Reference` links and `EventRequestActor`s (checked, Water Temple). The rooms' logic graphs (`Logic/<Dungeon>_Level_Room_NN_*.ainb`) read and write named GameData flags through nodes like `HoldGameDataGetBoolSimple` and `GameDataNameBool` (checked). Examples: `WaterTemple_Room00_WaterLevel` (Int, default 2), `WaterTemple_Room00_Switch_NearTheShutter`, `WaterTemple_Room00_OpenDoor1F`, `WaterTemple_Room23_Cleared`, `WaterTemple_Room23_LightSwitch`, `WaterTemple_SkipWaterChange`.
- **The list.** Every string in `Logic/*.ainb`, `AI/*.ainb` and `Event/EventFlow/*.bfevfl` was hashed (Murmur3 x86 32, seed 0) and looked up in the Bool and Int tables of `GameDataList`. 859 hits start with a dungeon's name (`InsideTheDekuTree`, `DodongoCavern`, `InsideJabuJabu`, `ForestTemple`, `FireTemple`, `WaterTemple`, `ShadowTemple`, `SpiritTemple`, `BottomOfTheWell`, `IceCavern`, `GerudoTrainingGround`, `GanonCastle`, `InsideGanonCastle`, also after a `HavePlayedEvent.` or `DungeonInfo.` prefix) and have `ResetTypeValue` 8, the saved type. 76 of them are about one player alone and are left out (`NaviHelp`, `NaviHint`, `NaviChat`, `IsPlayerIn`, `InBattle`, `IsInWater`, `InTheWater`, `FocusOn`, `DoorSide`, `RightSide`, `LeftSide`, `OnThe`, `Playing`, `IsAfterSceneCreate`, `Camera`, `Talk`, `Message`). That leaves **783 flags, 16 of them Ints** (781 since two were taken off, below): `lichii/hook/coop_dungeon.cpp`.
- **Played gimmick events (issue 129).** A room gimmick that plays an event (`EventRequestActor` with `RequestEventName`, a shutter's `ShutterEvent`, `NomralEvent`, `OpenChainEvent`, `OpenChainShutterEvent`) leaves the saved Bool `HavePlayedEvent.<event>`. The string scan above found only 6 of them, because the logic graphs do not hold the full name: the event's name is in the placement (Banc), and the room's boot-up node (`ActorLogicBootUpActorIsFirstOnEventTrigger`) is wired to that placement. So 190 more are shared: every such event whose name starts with a dungeon's name (also the game's misspelt `SpritTemple_`, `ForesetTemple_`) or with `Gimmick_`, `FocusOnGimmick_`, `FocusGimmick_`, `TreasureBoxAppear_`, `TrapTreasureBoxOpen`, `StoneOfWater_ElevatedFloor`, and that has none of `Boss`, `Navi`, `Talk`, `Meet`, `Ruto`, `SageOf`, `Rainbow`, `Hint` in it. (Unlike the scan's one-player rule, `Camera` and `FocusOn` stay in here: in an event's name they are the gimmick's own camera shot, and its played flag says the gimmick ran.) Left out (84): story scenes (`Dm_`, `Demo_`, `Get_StoneOf…`, `SageOf…`, the Ganon's Castle escape), bosses (their intros are [dungeons-boss.md](dungeons-boss.md)'s), Navi and talks, and the `Common_Dungeon_Open…` door events every dungeon shares. Also left out after review, and taken off the scan's list too:
  - **One-way rides**: `ShadowTemple_Gimmick_Departure` (the boat), `ForestTemple_Room11_ElevatorStart`, `ForestTemple_Room11_RoomSpin`, `ForestTemple_Room22_RoomSpin`. Their end state is a boat, lift or room turned to the far side; shared, it could leave a player who was not on it stranded on the wrong side. Each player rides their own until a run shows otherwise.
  - **Camera shots that are no one room's state**: `FocusOnGimmick_Basic`, `FocusOnGimmick_NoSE`, `TrapTreasureBoxOpen`, `TreasureBoxAppear_Room04` (one name for many rooms), `InsideGanonCastle_LookBarrierFromDoor_*` (5) and `GanonCastle_Dungeon_SeeTowerBarrier`, `…_FromDoor`. Shared, they would only make the other player skip their own first look.

  The list now has **971 flags**.
- **Missed spellings (softlock audit, test7).** The scan above matched the dungeon's exact spelling at the start of the name. The game also writes `Insidejabujabu_` (20 Jabu-Jabu room flags: rooms cleared, the Room 7 switches and gates), `Firetemple_` and `Foresttemple_` (Song of Time blocks), and puts room state after `Gimmick_` (`Gimmick_DefeatEnemy_Open_<Dungeon>_NN`, the enemies-beaten shutters; `Gimmick_DeleteFire_FireTemple`). Enemies die once in every game, so a shutter waiting for its room's enemies stayed shut for the second player. 26 added. Left out: the Room 7 area sensors (`…_InsideArea`, `…_NearTheSwitch*`) and `…_OnlyLinkOrLuto`, Jabu's Room 14 water and the Water Temple torch shutters `Gimmick_Ignition_Open_*` (set both ways by step switches and torches), and the Spirit Temple stone tablets: **997 flags**. A second pass added 18: the Fire Temple's Goron cell shutters (`Npc_Goron_0NN_ShutterisOpen`), Jabu-Jabu's Room 17 stone switch (`InsidejaubjabuBelly_`), and 8 dungeon gimmick scenes (`HavePlayedEvent.Dm_…`) that a room's own logic reads for its end state, as the Deku Tree ladder (issue 129): **1,015 flags**. See [sync-coverage.md](sync-coverage.md), "Softlock".
- Flags with other reset types (1, 9, 32, 40) are things like `InsideTheDekuTree_OnThe3rdFloor`, `WaterTemple_Room04_IsAfterSceneCreate` or `WaterTemple_Room06_TektiteBlue00_Alive`: where one player is, or a room's own short-lived state. They are not shared.

## What is shared, kind by kind

Every kind of dungeon state, where the game keeps it and whether co-op shares it. "Named" = a named Bool or Int flag of the list above; "key" = a 64-bit placement key ([flags.md](flags.md)); "item" = [items.md](items.md).

| Dungeon state | Where the game keeps it | Shared | How |
|---|---|---|---|
| Switches pressed (floor, eye, crystal), shutters and barred doors opened | Named, written by the room logic (`…_Switch_…`, `…_OpenDoor…`, `…_ShutterOpen`) | Yes | Live, last write wins (host's answer) |
| Torches lit for good (puzzles that stay solved) | Named (`DodongoCavern_CandleStick`, …); the Well's `Obj_Well_CandleStick_A_03` is a key | Yes | Named live; key as one-time |
| Torches that burn out, timed switches | In the actor, or flags of reset type 280 | No | Room-local, back on a reload; each player solves their own |
| Room cleared (its enemies beaten, the shutters open) | Named `…_Clear`/`…_Cleared`, and the rooms' `Obj_EnemyCounter_*` keys (51) | Yes | Named live; keys one-time |
| Enemies, pots, grass, ice pillars | Keys of reset type 88 or 280: back on a scene load | No | [objects.md](objects.md) syncs the ones on screen |
| Mini-bosses and bosses beaten | Named (`…_Boss_Defeat`, `…_BossBattleEnd`…) and the heart container's key | Yes | Named live; key one-time |
| Chests opened (small key, map, compass, boss key, items) | Key of the chest's placement, reset type 24 (140 dungeon chests) | **Yes, new** | One-time key: opened in one game, opened in all; never closes again |
| Silver rupees and their counters | Keys (`Obj_RupeeToken_Silver` 85, `Obj_silverRupeeCounter_*` 17) | **Yes, new** | One-time keys |
| Walls and floors blown or hammered open, webs burnt, red ice melted, ghost paintings, skull pots, falling floors | Keys of reset type 24 (`WallCrack`, `*_CrackWall_*`, `*_SpiderWeb_*`, `Obj_RedIce_*`, `Obj_FireTemple_HammerSlipPillar_*` …) | **Yes, new** | One-time keys |
| Gold Skulltulas in dungeons, heart containers, the few hearts and rupees of type 24 | Keys | **Yes** (also as pickups, [items.md](items.md)) | One-time keys |
| Water level (Water Temple) and other dungeon Ints | Named Int (`WaterTemple_Room00_WaterLevel`) | Yes | Live, last write wins |
| Map, compass, boss key owned | Item: `DungeonInfo.<dungeon>.IsGetMap` / `IsGetCompass` / `IsGetBossKey` | Yes | OR: once anyone has it, everyone has it |
| Small keys in the pocket | Item: `DungeonInfo.<dungeon>.SmallKeyNum` (Int) | Yes | **One shared count per dungeon** (below) |
| Locked doors unlocked | Named (`…_UnLock`, `…_OpenDoor…`) | Yes | Live |
| Business scrubs sold out, fairy fountains (reset type 25) | Keys | No | Not known what type 25 means; left out |
| Where each player is, Navi's hints, the camera, "in battle" | Named, reset types 1, 9, 32, 40, or one-player names | No | Each player's own |

### Small keys: one shared count

`SmallKeyNum` is a count, not a flag, and uses the count (delta) rule of [items.md](items.md): every change a player makes (+1 for a key picked up, −1 for a door opened) is added to the one count all players have for that dungeon. So the players share one key ring. A key cannot be had twice: its chest is a one-time key (above), so once one player has opened it, it is open in every game and the +1 is counted once. Keys that drop from an event write named flags that are shared (`FireTemple_GetSmallKey_01` to `_08`, `BottomOfTheWell_Level_Room_03_Get_SmallKey`). The only two loose `Obj_Dungeon_SmallKey` placements (Bottom of the Well and Shadow Temple, `*_Static`) have no key of their own; the Well's has the named flag just given, the Shadow Temple's is not mapped and could be taken twice (check in a run). A door opened by one player costs one key for everyone, and the door's flag opens it in the other games, so nobody pays for it twice.

### Later players

A player who joins later gets the host's table of every shared flag and key (`FLAG_SNAPSHOT`, [protocol.md](protocol.md)): doors open, chests open, rooms cleared, keys and dungeon items as the group has them. One exception from the one-time rule: a key a late joiner's own save has at 1 while the host's is 0 stays 1 in the joiner's game but is not sent to the host (the watcher's first read is its baseline); it reaches the others only when it changes again.

## The Deku Tree slingshot room (issue 129)

A player test (2026-11-10, v0.1.0-test4.2): in the slingshot room the trap was not shared, and the ladder did not fall in the other game although the pillar moved; a player who had not opened the chest could not leave. Read from the romfs (no run):

- The room is `InsideTheDekuTree` room index 2 (`DefaultDungeonRoomIndex`, the slingshot chest `Obj_TBox_A_02`, key `0x0b66198d17d6584e`, at -54.4 112.0 12.2). Its logic graph is `Logic/InsideTheDekuTree_Level_Room_C_Prefab_efa7`: the eye switch `Obj_EyeSwitch_A_02` (hit with the slingshot), an `EventRequestActor` for `InsideTheDekuTree_Gimmick_FallingLadder`, the pillar `Obj_InsideTheDekuTree_Stopper_A_01`, the ladder `Obj_InsideTheDekuTree_FallingLadder_A_01`, both chests, a boot-up node, `TriggerGameDataSetBoolTrue` on `InsideTheDekuTree_RoomC_Cleared` and `TriggerDungeonRoomStep…`.
- **The trap** is the falling floor by the chest (`Obj_InsideTheDekuTree_FallingFloor_A_01`, `_A_02`, `_A_03`; `_A_02` links a `FallingFloorBreakPos` anchor 3 m below). `_A_01` and `_A_02` are saved placement keys (`0xe9f286aa47fef9b1`, `0x61f7b4da35494840`); test4.2 did not share them, the dungeon placement keys (above) do.
- **The pillar and the ladder** have no flag of their own. Both are on/off animation actors (`OnOffAnimController`, the ladder `OffToOnOnly`) driven by a bool of the room graph (`QueryLogicGetBool`). In test4.2 `InsideTheDekuTree_RoomC_Cleared` was already shared, `HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder` was not. That fits what was seen: the pillar follows the room's flag, the ladder follows the event, which only A's game played.
- **The fix here:** the played gimmick events above are shared, so B's game has the same record that the ladder event played. What is not known without a run: whether B's room, already loaded, puts the ladder down at once or only when it loads again (item 1 below). If only on a reload, the next step is to play the event in B (the game's event request by name), which needs a new message.

## The placement keys

`CoopDungeon::Keys()` (`lichii/hook/coop_dungeon.cpp`) lists the 500 placements in the 13 dungeons' Banc files (`Banc/<Dungeon>/*_Static.bcett.byml`, `*_Dynamic.bcett.byml`) whose `Hash` is in `GameDataList`'s `Bool64bitKey` table with `ResetTypeValue` 24 (checked; 522 had type 24 or 25, the 22 of type 25 are left out). Every chest in a dungeon has such a key (checked: no `Obj_TBox_*` in a dungeon lacks one). They join the shared list in `Merge`, so they are shared even with `LICHII_COOP_PICKUPS=0`; with pickups on (the default) the hook also hands each to `Session::share_pickup`, which makes them one-time: a 1 goes to everyone, and a 0 (the game's own table being reloaded, or a 0 from another game) is never sent and never written over a 1.

## How it is shared

`CoopDungeon::AddShared` adds the 1,015 flags (781 from the scan, 190 played gimmick events, 44 the softlock audit found) and the 500 placement keys to the session's shared list (`docs/flags.md`), next to the items and the user's flags file, unless `LICHII_COOP_DUNGEON=0`. The flag watcher then carries every change live: the player who pressed the switch writes the flag in their game, the host's answer sets it in every game, and the other games' room logic reads it like its own.

## Not done

1. **Not run in a dungeon.** The test saves are a child in the Graveyard; no scripted run reaches a dungeon. What is not known: whether a door, shutter or water plane already loaded in the other game reacts at once to a flag set from outside, or only when its room loads again. The graphs poll with `Hold…` nodes, which suggests at once; the Water Temple's level change also plays an event (`HavePlayedEvent.WaterTemple_WaterLevelChange01`), and whether the water moves without that event playing is not known.
2. Room-local, unsaved state (a timed switch, a torch lit for a puzzle, enemies of a room alive) is not shared: it lives in the actors and in flags of other reset types.
3. A late joiner gets the host's table like any shared flag; whether a room already loaded then catches up is item 1 again.
