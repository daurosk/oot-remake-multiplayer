# Agent handoff: what the play-test kit work learned (2026-11-22)

For the maintainer's agent, from the agent that worked on the play-test kit branch (pull request #315). Everything here is information
about the game (addresses, offsets, names, behaviour) and about our tools; nothing from the game's files. The code is
the source: each item names where it lives. The full list of every address and offset the code names, with its
comment, is generated into [addresses.md](addresses.md) by `tools/address_index.py` (run it
again after a game update).

What is done, open and waiting on (the coordination state, kept current): [agent-state.md](agent-state.md).

## 1. Actor and component memory (proven in game)

| What | Where | Proven by |
| --- | --- | --- |
| Actor state u32 `+0x2c` (4 alive, 6 asleep: what the game sets ~25 m from its Link) | `coop_objects.cpp` `kActorState` | every objects run |
| Actor name `char*` `+0x260`; component table `+0x270`; Life component at table `+0x2b8` | `kActorName`, `kActorComponents`, `kComponentLife` | every objects run |
| Place 3 x f32 `+0x31c`; rotation 3x3 f32 `+0x328`, rows of 3 (yaw = `atan2(m[2], m[8])`) | `kActorPosition`, `kActorRotation` | the turning floor's yaw read live (`coop: objects: ... yaw`) |
| Flags u32 `+0x4a8`, bit 23 set by the set-matrix call (a warp); cleared once when an enemy becomes ours | `kActorFlags`, `kWarpBit` | issue 150/157 runs |
| Actor list node `+0x538` (owner at node `+0x10`); the manager's lists, head `+0x670`, count `+0x10` | `kActorNode`, `kListHead` | every walk |
| Every actor seen shares vtable `main+0x4b25e18`; its slot `0x218` is `main+0x4c6580`: **set matrix** `(actor, 0)`. Write `+0x31c..+0x34b` (place, then the 3x3), then call it: the game hands the transform to the physics, so the collision moves too. A plain write of `+0x31c` is undone within a frame. | `kActorVtable`, `kSetMatrix` | enemy copies; **the Forest Temple basement floor turned 0 -> -45 -> -90 with its walls in the game that did not push it (run st-pivot3, 2026-11-21); its AI did not turn it back** |
| Request delete `main+0xa6490 (actor, 0)` | `kRequestDelete` | lock opens, copies |
| Create by name `main+0x372a70` (requestCreateActorAsync: name, place, rotation, scale; no Banc params or hash) | `coop_puppet.cpp` `kCreate` | puppets, enemy copies (not proven for `Land_*` terrain) |
| AS (animation) controller: `[[[actor+0x270]+0x40]+0x18]+0x18`; slot 0 `[controller+0x20]`; command name `char*` `+0x10`, rate f32 `+0xd4` | `kComponentsAs`, `kAsSlotName` | animation sync |

**Life component and the hit record** (`coop_hit_replay.h`): hit header `+0x1f8`, amount `+0x224`, damage this frame
s32 `+0x228`, the incoming hit `+0x240..+0x2d8` (attacker actor `+0x240`, hit id `+0x248`, present `+0x24c`, attack
kind and its parameters `+0x250` up to `+0x27c`, the hit's place `+0x27c`, the attacker's place `+0x2a0`, the id again
`+0x2c4`), last hit id `+0x1b18`. Our Link's own Life takes a replayed record with another kind
(`HurtOurLinkNative`: the 11 kind words from `+0x250`), so a player can be given a game hit of any kind; PvP uses it.

**Push blocks** (`coop_world.h` `BlockNode`): the push node `ExecuteMovableBoxControl` keeps its goal at `+0x4c` (its
`Gimmick_AnswerPosAnchor`) and its `IsOnAnswerPos` port at `+0x1f0`; its own start function runs again on a block
another game moved (`kStartFn`), so the next push starts where it rests. Logged per fix since 2026-11-21:
`coop: world: ... rests D m from its answer spot ...; its on-answer port reads 0x..`.

**Logic set** (`coop_logic_set.h`): `ExecuteLogicSetBool`'s execute `main+0x2727540`, its vtable `main+0x49a9550`; a
room graph's bool plug is set by name (Murmur3 key), guarded by a live plug of that key. Used for lifts and switches.

**Event request** (`hookshot/src/coop_hookshot.cpp`, `HOS_COOP_WRAP_EVENT_REQUEST`): phase 0 sees every event the
game asks, by name; `x[0] = 0; return 1` refuses it (the asker does nothing, no cutscene). The Forest Temple's
twisting hallway uses it (`ForestTemple_Room11_RoomSpin` / `_Room22_RoomSpin`, `Dm_ForestTemple_0005` / `_0007`);
the eye (`Obj_Base_OnOffAnim`) has no AI of its own, so a refused turn leaves it still.

## 2. Game behaviour found in game runs

- **The basement turning floor** (`Obj_ForestTemple_PivotedFloor_A_01`, 0 59.5 -3.5, room 27): a capstan; the bars
  are the model's bones `GrabPoint_01/_02` (local -9.55 0.65 3.96 and its mirror). Its direction int flag
  `ForestTemple_SageOfForest_PivotedFloor_A_01` (`InitialDirIndexGameDataPath`) is read **only at load**: set under a
  loaded floor it does nothing. A push turns the actor's 3x3 by 45 degrees in about 1.4 s in even steps; the flag is
  written as the turn ends; yaw = 45 x direction (a push to -45 read flag 7). `coop_pivot.h` drives other games'
  floors to the shared direction through set matrix.
- **Pushing needs A held** (a grab); walking into a block only shows "Grab".
- **Redead**: two players 4 m from the Graveyard's three Redeads (113 97.3 9.5) for 50 s saw no scream (they stayed
  in `Wait`/`Sit`). What starts the scream is not known yet (`coop: scream:` probe lines in the debug walk).
- **King Dodongo in the boss rush** idles even solo (its stage's own logic). Rush bosses are the same actors as the
  dungeons'; the rush has no difficulty tables of its own.
- **Meg** spawns from `Dm_ForestTemple_0015`, asked by an area trigger at the main hall platform (0 100 -3.5); it did
  not trigger from a save placed by flags.

## 3. Game data facts (read from the romfs, not yet all checked in game)

- **Packs** fight together by messages between their AIs' blackboards: Lizalfos/Dinolfos
  (`SetBattlingAllyFromReferenceLink`, `IsRequestingCoop`, `IsReceivedCoopAttackMessage`,
  `IsReceivedCoopStandbyMessage`, `ExecuteEnemyCoopWriteBlackboardReceiveMessage`), Wolfos
  (`AllAlliesDisableMelee`, tag `WolfosAlly`, `IsDisableMeleeBattle`). A copy's held brain sends none, so packs are
  now one group under one owner (`PackFamily`).
- **Meg's decoys** `Enemy_MiddleBoss_Poe_Meg_Dummy`: no BSA component (their own AI), MaxLife 1, holder `MegDummy`
  (3), made anew each cycle; seat `MegDummy_SeatId` and `MegDummy_PhaseNum` are blackboard s32s; Meg's Appear rolls
  the layout angle with `QueryMathRandomF32Once`. In co-op they get ids by order of sighting (`SceneSpawnId`), so the
  four Poes can match differently between games.
- **Dead Hand**: holder `DeadHandHand` (6 hands); its utility AI wakes them and sends `'Appear'`/`'Disappear'` every
  frame; a hand's grab rides Link (`OneShotRiderRide`, restraint 720 frames, mash window 660) and sends
  `MessageAppear` to `DeadHand`'s transceiver; his brain rises on it (`QueryDeadHandAppearMessageReceiver`).
- **Volvagia's rocks** are the arena's: `Obj_FallingRockManager_A_03` wakes a pool of up to 30 `Obj_FallingRock_A_06`
  (made at load, asleep) every 30-90 frames while `IsActive`, aimed at the local Link
  (`QueryDungeonGetPlayerPos` plus lead `TargetAdvFrame`); switched by Volvagia's `FallRockActivate` /
  `FallRockEffectActivate` plugs. Co-op does not touch them.
- **Redead freeze**: `Enemy_ReDead_Shout`; `ParalysisEnableDistMin/Max`, `ParalysisEnableAngleDiff` and their
  `InTown` values; `AtkRestraint` for the grab.
- **Capstans** (turned by a pushed bar): `Obj_SpiritTemple_CapstanMirror_A_01` (3, 8 a lap, `CapstanMirrorDirIndex`,
  per-placement game data) and `Obj_ShadowTemple_Statue_A_01` (`CorrectDirIndex`). Only these three actors and the
  basement floor read an `Initial*` direction in the 13 dungeons.
- **Goron City statue** `Obj_Goron_Statue_A_01` runs the movable-box AI (`ExecuteMovableBoxControl`).
- **Presence-gated placements whose shared flag changes in play** (probably read only at a scene load): the Water
  Temple's level planes and waterfall (`WaterTemple_WaterLevelChange01/03`, `HavePlayedEvent.WaterTemple_WaterLevelChange01`),
  the Shadow tower (`ShadowTemple_RoomB3FB4FJ_TowerBreak_Group01/02`), Bottom of the Well water
  (`BottomOfTheWell_Room_01_Blow_Ocarina`), the Forest Temple's room 7 water (`ForestTemple_Room07_ChangeWaterLevelActor`)
  and elevator (`ForestTemple_Elevator_BeforeDm0010`), Ice Cavern pots (the only gates with `IsCheckLazy`), a
  Jabu-Jabu air wall.
- **Per-player flags that must not be shared**: the Water Temple Dark Link rooms' passage flags (the door event
  toggles them while one player passes; room graph 22 hides rooms and the dome from them), the Shadow lifts' crush
  flag `ShadowTemple_Level_PaddelShip_Elevator_AbyssTriggered` (it sinks the boat).
- **Mid-fight events**: `SageOfSpirit_BossChange` (Twinrova merge), `InsideGanonCastle_Dungeon_Boss_GetMasterSword`,
  `InsideGanonCastle_Dungeon_BossBind0`; per player, never shared: `Enemy_Wallmaster_ReturnToEntrance*`,
  `GerudoFortress_MiddleBossLose*`.
- **Model bones**: a bone record in a model's skeleton (`FSKL`, 0x58 each) holds, from `+0x30`, scale (3 f32),
  rotation as euler radians (3 f32), 1.0, then the position (3 f32). Read with our `puppet_pack/bfres.py` plus the raw
  floats; that is how the basement floor's bars were found.

## 4. How our agent tests (tools, and what works)

- **Build**: llvm-mingw from the kit's `hookshot/tools` (our agent's toolchain for the module; the maintainer's builds
  and CI compile the module with MSVC on Windows and gcc/clang on Linux, and llvm-mingw builds only the game); unit
  tests `coop_tests -tce="lag*,traffic*"` and `coop_server_tests`. Note: `#ifdef _MSC_VER` for Windows-only code
  breaks an llvm-mingw build (a path is wide there too); `_WIN32` is the right test (`coop_save.cpp` uses it for
  every file it opens).
- **Two-game runs**: `tools/autotest/hookshot.py run` on two Hookshot test copies. Our scenarios warp at the title
  (`warp SCENE`, then A presses) with `#! noload`, an adult or child save copied into both copies, Save Profiles off for
  the run (its file select takes the title presses). Switches: `LICHII_COOP_CHEATS=1`, `LICHII_COOP_DEBUG_OBJECTS=1`,
  `LICHII_COOP_DEBUG_BOSS=1`. Kill-switches for tonight's features: `LICHII_COOP_PIVOT=0`, `LICHII_COOP_SPLIT_SHOTS=0`,
  `LICHII_COOP_BLOCK_NODE=0`.
- **Live play-testing** ([playtest.md](playtest.md)): `tools/autotest/live.py` keeps both games open and takes one
  step at a time; `dump TAG` checkpoints every shared flag; `desync_check.py`; `mcp_playtest.py` for Claude Code.
  The play-test bridge in our module holds the game and runs an exact number of frames (`step`), walks by world
  direction (`move`), and `--pair` drives both games from one MCP server. An agent starts here:
  [playtest-agents.md](playtest-agents.md); shared scripts and known stuck spots: `tools/playtest/`.
- **Reading the romfs** for facts: `tools/puppet_pack` (`zs`, `byml`, `sarc`, `bfres`): maps in
  `Banc/<Scene>/*.bcett.byml.zs` (actors, `Dynamic` params, `Links`, `Presence`), packs in `Pack/Actor/<Name>.pack.zs`;
  AI graphs read only as names (grep printable strings out of `.ainb`). Scanning the whole romfs at once takes over
  10 minutes; read per folder.

## 5. What we would like from your agent

Any of these, as a reply comment or a docs file in a PR:

1. **Memory scan results not in the repo**: tables, Cheat Engine finds, `tools/game_scan` notes, offsets you use
   that the code does not name yet.
2. **A blackboard's value layout**: where an s32 key's value sits in an actor's blackboard (for Meg's
   `MegDummy_SeatId`).
3. **The player's paralysis/freeze**: the function or flag that freezes Link (for the Redead), and what starts a
   Redead's scream.
4. **Presence**: whether a placement's `Presence.FlagName` is checked again when a dungeon room streams back in, or
   only at a scene load.
5. **A frame step in the control bridge** (done for our module: `step` in [playtest.md](playtest.md), exact frames
   then hold). Still wanted: the same in Longshot's own bridge, so no module is needed.
6. **An AI-graph reader with the wiring** (node inputs and links), if your agent has one: ours reads names only.
7. **Your agent's own handoff**: its scripts, memory notes and what it learned that is not written down.
