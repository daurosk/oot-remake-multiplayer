# Dungeons the same for every player

Plan stage 5: "if someone triggers something in the Water Temple, all players see it happen". Switches, doors, torches, room clears and water levels. Breakables and movable blocks are [world sync](objects.md)'s; enemies are [objects.md](objects.md).

Game: main of build e0f3ccfaf50065ae (1.1.1), romfs of the same version. **Checked** means read from the game's files. **Live** means seen in the running game.

## Results

- **Switches, shutters, doors and torches have no flag of their own.** None of the 1,010 `CandleStick`, 574 `SwitchShutter`, 39 `StepSwitch`, 20 `EyeSwitch`, 18 `SwitchHit` or 98 `DoorChainLock` placements in the Banc files has a 64-bit key in `GameDataList` (checked). Only chests (247 of 546, reset type 24) and a few torches (22, reset type 280) do.
- **The dungeon's state is in named flags the room logic writes and reads.** A placement is linked to others only by `Reference` links and `EventRequestActor`s (checked, Water Temple). The rooms' logic graphs (`Logic/<Dungeon>_Level_Room_NN_*.ainb`) read and write named GameData flags through nodes like `HoldGameDataGetBoolSimple` and `GameDataNameBool` (checked). Examples: `WaterTemple_Room00_WaterLevel` (Int, default 2), `WaterTemple_Room00_Switch_NearTheShutter`, `WaterTemple_Room00_OpenDoor1F`, `WaterTemple_Room23_Cleared`, `WaterTemple_Room23_LightSwitch`, `WaterTemple_SkipWaterChange`.
- **The list.** Every string in `Logic/*.ainb`, `AI/*.ainb` and `Event/EventFlow/*.bfevfl` was hashed (Murmur3 x86 32, seed 0) and looked up in the Bool and Int tables of `GameDataList`. 859 hits start with a dungeon's name (`InsideTheDekuTree`, `DodongoCavern`, `InsideJabuJabu`, `ForestTemple`, `FireTemple`, `WaterTemple`, `ShadowTemple`, `SpiritTemple`, `BottomOfTheWell`, `IceCavern`, `GerudoTrainingGround`, `GanonCastle`, `InsideGanonCastle`, also after a `HavePlayedEvent.` or `DungeonInfo.` prefix) and have `ResetTypeValue` 8, the saved type. 76 of them are about one player alone and are left out (`NaviHelp`, `NaviHint`, `NaviChat`, `IsPlayerIn`, `InBattle`, `IsInWater`, `InTheWater`, `FocusOn`, `DoorSide`, `RightSide`, `LeftSide`, `OnThe`, `Playing`, `IsAfterSceneCreate`, `Camera`, `Talk`, `Message`). That leaves **783 flags, 16 of them Ints**: `lichii/hook/coop_dungeon.cpp`.
- Flags with other reset types (1, 9, 32, 40) are things like `InsideTheDekuTree_OnThe3rdFloor`, `WaterTemple_Room04_IsAfterSceneCreate` or `WaterTemple_Room06_TektiteBlue00_Alive`: where one player is, or a room's own short-lived state. They are not shared.

## How it is shared

`CoopDungeon::AddShared` adds the 783 flags to the session's shared list (`docs/flags.md`), next to the items and the user's flags file, unless `LICHII_COOP_DUNGEON=0`. The flag watcher then carries every change live: the player who pressed the switch writes the flag in their game, the host's answer sets it in every game, and the other games' room logic reads it like its own.

## Not done

1. **Not run in a dungeon.** The test saves are a child in the Graveyard; no scripted run reaches a dungeon. What is not known: whether a door, shutter or water plane already loaded in the other game reacts at once to a flag set from outside, or only when its room loads again. The graphs poll with `Hold…` nodes, which suggests at once; the Water Temple's level change also plays an event (`HavePlayedEvent.WaterTemple_WaterLevelChange01`), and whether the water moves without that event playing is not known.
2. Room-local, unsaved state (a timed switch, a torch lit for a puzzle, enemies of a room alive) is not shared: it lives in the actors and in flags of other reset types.
3. A late joiner gets the host's table like any shared flag; whether a room already loaded then catches up is item 1 again.
