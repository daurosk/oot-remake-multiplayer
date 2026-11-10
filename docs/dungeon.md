# Dungeons the same for every player

Status: the tables are read from the game's files; the sharing is tested on fake games (`tests/lichii/dungeon_test.cpp`); none of it is played through in a dungeon yet (issue 87).

Plan stage 5: "if someone triggers something in the Water Temple, all players see it happen". Switches, doors, torches, room clears and water levels. Breakables and movable blocks are [world sync](objects.md)'s; enemies are [objects.md](objects.md).

Game: main of build e0f3ccfaf50065ae (1.1.1), romfs of the same version. **Checked** means read from the game's files. **Live** means seen in the running game.

## Results

- **Switches, shutters, doors and torches have no flag of their own.** None of the 1,010 `CandleStick`, 574 `SwitchShutter`, 39 `StepSwitch`, 20 `EyeSwitch`, 18 `SwitchHit` or 98 `DoorChainLock` placements in the Banc files has a 64-bit key in `GameDataList` (checked). Only chests (247 of 546, reset type 24) and a few torches (22, reset type 280) do.
- **The dungeon's state is in named flags the room logic writes and reads.** A placement is linked to others only by `Reference` links and `EventRequestActor`s (checked, Water Temple). The rooms' logic graphs (`Logic/<Dungeon>_Level_Room_NN_*.ainb`) read and write named GameData flags through nodes like `HoldGameDataGetBoolSimple` and `GameDataNameBool` (checked). Examples: `WaterTemple_Room00_WaterLevel` (Int, default 2), `WaterTemple_Room00_Switch_NearTheShutter`, `WaterTemple_Room00_OpenDoor1F`, `WaterTemple_Room23_Cleared`, `WaterTemple_Room23_LightSwitch`, `WaterTemple_SkipWaterChange`.
- **The list.** Every string in `Logic/*.ainb`, `AI/*.ainb` and `Event/EventFlow/*.bfevfl` was hashed (Murmur3 x86 32, seed 0) and looked up in the Bool and Int tables of `GameDataList`. 859 hits start with a dungeon's name (`InsideTheDekuTree`, `DodongoCavern`, `InsideJabuJabu`, `ForestTemple`, `FireTemple`, `WaterTemple`, `ShadowTemple`, `SpiritTemple`, `BottomOfTheWell`, `IceCavern`, `GerudoTrainingGround`, `GanonCastle`, `InsideGanonCastle`, also after a `HavePlayedEvent.` or `DungeonInfo.` prefix) and have `ResetTypeValue` 8, the saved type. 76 of them are about one player alone and are left out (`NaviHelp`, `NaviHint`, `NaviChat`, `IsPlayerIn`, `InBattle`, `IsInWater`, `InTheWater`, `FocusOn`, `DoorSide`, `RightSide`, `LeftSide`, `OnThe`, `Playing`, `IsAfterSceneCreate`, `Camera`, `Talk`, `Message`). That leaves **783 flags, 16 of them Ints**: `lichii/hook/coop_dungeon.cpp`.
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

## The placement keys

`CoopDungeon::Keys()` (`lichii/hook/coop_dungeon.cpp`) lists the 500 placements in the 13 dungeons' Banc files (`Banc/<Dungeon>/*_Static.bcett.byml`, `*_Dynamic.bcett.byml`) whose `Hash` is in `GameDataList`'s `Bool64bitKey` table with `ResetTypeValue` 24 (checked; 522 had type 24 or 25, the 22 of type 25 are left out). Every chest in a dungeon has such a key (checked: no `Obj_TBox_*` in a dungeon lacks one). They join the shared list in `Merge`, so they are shared even with `LICHII_COOP_PICKUPS=0`; with pickups on (the default) the hook also hands each to `Session::share_pickup`, which makes them one-time: a 1 goes to everyone, and a 0 (the game's own table being reloaded, or a 0 from another game) is never sent and never written over a 1.

## How it is shared

`CoopDungeon::AddShared` adds the 783 flags and the 500 placement keys to the session's shared list (`docs/flags.md`), next to the items and the user's flags file, unless `LICHII_COOP_DUNGEON=0`. The flag watcher then carries every change live: the player who pressed the switch writes the flag in their game, the host's answer sets it in every game, and the other games' room logic reads it like its own.

## Not done

1. **Not run in a dungeon.** The test saves are a child in the Graveyard; no scripted run reaches a dungeon. What is not known: whether a door, shutter or water plane already loaded in the other game reacts at once to a flag set from outside, or only when its room loads again. The graphs poll with `Hold…` nodes, which suggests at once; the Water Temple's level change also plays an event (`HavePlayedEvent.WaterTemple_WaterLevelChange01`), and whether the water moves without that event playing is not known.
2. Room-local, unsaved state (a timed switch, a torch lit for a puzzle, enemies of a room alive) is not shared: it lives in the actors and in flags of other reset types.
3. A late joiner gets the host's table like any shared flag; whether a room already loaded then catches up is item 1 again.
