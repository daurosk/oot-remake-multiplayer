# Sync coverage: what is shared between games, and what is not

The goal (Caleb, 2026-11-11): everything one player does to the world is the same in every game, **live** (not only after a room reload), and nothing one player does can leave another unable to continue. This page is the map for that work. The fix branches (`dungeon-live`, `breakables`, `softlock-audit`) update their own rows when they change something.

State on `main` at 99332d9 (chunk 4 merged, protocol 6). Later branches changed some rows themselves; what the integration branch `integrate-p11` (protocol 10) added beyond the rows is listed in [Added for protocol 10](#added-for-protocol-10). Sources are the docs named in each row and the reports of the chunk 3 and 4 test turns (integrate-p6, integrate-p7). The actor classes are read from the romfs (`Pack/Actor`, 8,373 packs, grouped by name).

**Columns**
- **Path** is the code that shares it:
  - *flags*: named Bool/Int flags, by the flag watcher ([flags.md](flags.md)).
  - *keys*: 64-bit placement keys ([dungeon.md](dungeon.md), "The placement keys").
  - *take*: chest-sync's remove step for a taken key, `CoopObjects::RemovedWhenTaken`.
  - *objects*: the enemy sharing: owner, life samples, `DAMAGE` ([objects.md](objects.md)).
  - *world*: `WorldSync` with `WORLD_BREAK`, `WORLD_MOVE`, `WORLD_PLACE`, `WORLD_HIT` and `SIGN_CUT` ([world.md](world.md)).
  - *grass*: `GRASS_CUT`.
  - *drops*: `CoopDrops`, one roll per drop and `DROP_TAKE`.
  - *items*: the item rules in [items.md](items.md).
  - *spawns*: things a player makes ([spawns.md](spawns.md)).
  - *boss*: `BOSS_ROOM` ([dungeons-boss.md](dungeons-boss.md)).
  - *none*: not shared.
- **Live** says when the other game shows it:
  - *live*: the loaded copy changes at once.
  - *reload*: the value is shared, but the other game shows it only when the room loads again.
  - *?*: the value is shared, but whether a loaded copy reacts was never seen.
  - *-*: not shared.
- **Proven**:
  - *game*: seen in an autotest run, with the run named.
  - *logs*: the right log lines, but nothing seen on screen.
  - *unit*: unit tests only.
  - *-*: none.
- **P** (priority): P1 can softlock or block progress; P2 is a visible desync; P3 is cosmetic.

## Breakables

| Object | Actors (romfs) | Path | Live | Proven | Risk if not shared live | P |
|---|---|---|---|---|---|---|
| Pots | `Obj_Pot_*` (15), `Obj_ShadowTemple_SkullPot_*`, `Obj_Gerudo_HorsebackArcheryPot_*` | objects (life) + world `WORLD_BREAK` | ? | unit | Two drops, or a pot whole on one screen. A Shadow Temple skull pot holding a key is a *keys* row, kept, not removed | P2 |
| Crates | `Obj_BrokenBox_*` (5) | objects + world | live (4.6 s late) | game (`pots`, 2026-11-09, 3 runs) | - | P3 |
| Liftable rocks | `Obj_LiftRock_*` (7) | objects (gone near Link) + world | live | game (`rock`, thrown; chunk 3 lift) | Carried rock not seen moving | P2 |
| Bomb and hammer rocks | `Obj_BrokenRock_*`, `Obj_*_BrokenRock_*`, `Obj_Common_Fragment_Broken*Rock` | objects + world; dungeon ones also *keys* + *take* | ? | unit | A rock that blocks a path (Dodongo, Death Mountain): open in A, closed in B. B can bomb its own | **P1** where it blocks a path B cannot open itself (no bombs yet) |
| Cracked walls and floors | `Obj_*_CrackWall_*` (Dodongo, Shadow, Spirit, Water), `Obj_*_CrackFloor_*`, `WallCrack` | dungeon *keys* + *take* (removed); overworld: world | live via *take* | logs (chest take path, 2026-11-10) | B cannot pass a wall A opened if B has no bombs | **P1** until a take of a wall is seen live |
| Spider webs | `Obj_InsideTheDekuTree_SpiderWeb_*`, `*InteractiveSpiderWebParts*`, `Obj_InsideGanonCastle_SpiderWeb` | *keys* + *take* | live via *take* | unit | Deku Tree floor web: B cannot drop down | **P1** (Deku Tree basement route) |
| Red ice, ice over switches | `Obj_RedIce_*` (8), `Obj_Ice_A_01` | *keys* + *take* (all dungeon placements keyed); world gone-near-Link overworld | live via *take* / world | unit | Zora's Domain / Ice Cavern path blocked for B without blue fire. `Obj_LakeHrl_Ice_*` is scenery (model and physics only, no life or destruction; breakables audit) | **P1** |
| Ice pillars | `Obj_BrokenIcePillar_*` (4) | objects + world | ? | unit | - | P2 |
| Grass tufts and bushes | `Obj_Plant_Grass_*`, `Land_kokiri_Plant_*`, `Obj_Plant_DestructibleGrass_Base` | objects + world (deleted 0.4 s after) | ? | unit (life 0 alone failed in a game) | - | P3 |
| Field grass | foliage, no actor | grass `GRASS_CUT` | ? | unit | - | P3 |
| Signs and signposts | `Obj_SignBoard_*`, `Obj_*_Signboard_*`, `Obj_GuidePost_*`, `Obj_*_GuidePost_*` | world `WORLD_HIT` + `SIGN_CUT` | ? | logs (sign 20261110-122750: 11 of 11 words written in B; pieces not in view) | - | P3 |
| Beehives | `Obj_Kokiri_Beehive_*` | objects + world | ? | unit | - | P3 |
| Bomb flowers | `Obj_BombFlower_*` (4), its fruit | none (the fruit has no placement) | - | - | Each player picks their own; the rock it breaks is a *Bomb rock* row | P2 |
| Falling floors (Deku Tree) | `Obj_InsideTheDekuTree_FallingFloor_*` (`FallingFloor_A_03` has no saved key in the game, so it cannot be keyed) | *keys* + *take* | live via *take* | unit | B cannot fall to the next floor | **P1** to check |
| Falling floor (Shadow Temple) | `Obj_ShadowTemple_FallingFloor_*` (`FallingFloor_A_03` has no saved key in the game, so it cannot be keyed) | *keys*, kept (stays as ground); the unkeyed `_A_03` a world *shifter* (breakables-2) | reload; `_A_03` live | unit | Shown fallen only after a reload | P2 |
| Path pieces that move when hit | Fire Temple `HammerDropFloor`, `HammerDropPillar`, `HammerStair` (7, no key), Deku Tree `FallingLadder_A_01`, `FallingFloor_A_03` (no key) | world *shifter*: `WORLD_MOVE`/`WORLD_PLACE`, held in B (breakables-2) | live; late joiner by the server's place | unit | Before: not shared at all | **P1** |

## Pickups

| Pickup | Actors | Path | Live | Proven | Risk | P |
|---|---|---|---|---|---|---|
| Drops (rupees, hearts, ammo, magic from enemies, pots, grass, rocks) | `Obj_Rupee_*`, `Obj_Heart`, `Obj_DekuSeed`, `Obj_DekuNut`, `Obj_DekuStick`, `Obj_ArrowBundle_*`, `Obj_Bomb`, `Obj_MagicJar_*`, `Obj_Fairy` (breakables) | drops `DROP_SPAWN`/`DROP_TAKE` | ? | unit; issue 102 open (a bush drop taken twice in a player test) | Taken twice, or different drops | P2 |
| Placed rupees and recovery hearts | `Obj_Rupee_*`, `Obj_Heart`, `Obj_Heart_InWater` placed (170 keys, reset type 88) | key64 shared (breakables: IsPickup) + *take* | live via *take*; back on a load as in one-player | unit | - | P3 |
| Gold Skulltulas and tokens | `Obj_RupeeToken` (85), `Item_Token_Gold` | *keys* (dungeons; overworld through the flags file) + *take*; count delta | live via *take* | logs (chunk 2); issue 89 open | Counted twice, or left in B | P2 |
| Heart pieces | `Obj_Heart_Piece*` | key + count delta | ? | unit | Taken twice | P2 |
| Heart containers | `Obj_Heart_Container` | key + `MaxLife` max | live | game (hearts 094002: B 16 -> 20) | - | P3 |
| Small keys | `Obj_Dungeon_SmallKey*`, chest contents | `DungeonInfo.<d>.SmallKeyNum`, one shared count (delta) | live | unit; double key with the near rule (deku-softlock check 5) | A spends the last key: the count drops in B too, and B's door must open (*Locked doors* row), or B is stuck at a locked door with 0 keys | **P1** |
| Gold Skulltula token | `Obj_Token_Gold` (no placement; made at the Skulltula's death) | world `WORLD_BREAK` under its Skulltula's id + the Skulltula's key64 set in B (breakables-2); both at once: one token back | live; reload: the server's break and the key | unit | Before (#89): taken in A, still in B, also after B reloaded the room: a second token for the group | **P1** |
| Small keys lying | `Obj_Dungeon_SmallKey*` (10 placements: Well, Shadow, Gerudo Fortress; not `Obj_MiniGame_SmallKey`) | world `WORLD_BREAK` (breakables-2; they have no 64-bit key, so the key path never worked for them); both at once: the second taker gives one back | live, no 3 m rule; late joiner by the server's break | unit | Before: taken in A, still lying in B: a second +1 in the shared count | P2 |
| Map, compass, boss key | `Item_Dungeon_*` | items OR | live | unit | - | P3 |
| Items from chests and events (Slingshot, Bow, bags...) | `Item_*` | items (any/max, capacity and ammo grants, load mend) | live | game (sling-give, sling-stick, 2026-11-10) | Fixed (issue 138) | P3 |
| Silver rupees | `Obj_Rupee_Silver`, `Obj_RupeeToken_Silver`, `Obj_silverRupeeCounter_*` (6 names, 17 placements) | rupee: *keys* + *take* + drops; counter: key, kept; `…_SilverToken_Appear/_Unlock`, `…_SilverRupee*` flags | ? | unit | Shared besides the keys: `…_SilverToken_Appear/_Unlock` (events and flags), `InsideGanonCastle_Dungeon_Room03/10/14/16_SilverRupee`, `GerudoTrainingGround_Dungeon_Room02/06/09_SilverRupee`, `IceCavern_Room06_SilverRupeeGet`, `BottomOfTheWell_…_SilverRupee_GetAll` (Bool or Int, and whether the counter reads them, not known). If the counter counts only its own pickups: A takes 3, B takes 2: each game's counter may count only its own, and the rupees B took are removed from A's game uncounted. Then neither counter reaches 5 and the door never opens for anyone | **P1** |
| Magic beans, bean spots | `Obj_Plant_MagicBean*` | none | - | - | Bean planted by A not there in B | P2 |
| Fairies | `Obj_Fairy*`, `Npc_Fairy` | none | - | - | - | P3 |

## Dungeon mechanics

Dungeon state is in named flags that the room logic reads ([dungeon.md](dungeon.md)). All 971 named flags and 500 keys are shared. The open question for almost every row is **live**: a door, shutter or water plane already loaded in B must react to the flag, not just store it. Only two things are seen working: the Deku Tree switch flag, which is kept through B's load (deku-late, 2026-11-10), and the slingshot room's ladder and pillar flags, which reach B. Nothing has been seen moving on B's screen.

| Mechanic | Actors | Path | Live | Proven | Risk if not live | P |
|---|---|---|---|---|---|---|
| Floor switches that stay down | `Obj_StepSwitch`, `Obj_Stepswitch`, `Obj_*_StepSwitch`, `Obj_InsideJabu_Jabu_Stepswitch` | flags (the room's `…_Switch_…`, `…_StepSwitch`) | ? | logs (deku-late: the flag crosses) | Door open in A, shut in B | **P1** |
| Floor switches held down (a player or block on them) | same actors; the held state is in the actor | none | - | - | Caleb's case: "stands on a button that opens something". A holds, B's door stays shut. B cannot hold its own while A stands on A's | **P1** |
| Timed switches, timed torches | `Obj_CandleStick_TimeExtinguish`, reset type 280 | none (room-local) | - | - | Each player solves their own; two players cannot share one timer | P2 |
| Eye switches | `Obj_EyeSwitch_*` (5) | flags | ? | logs (Deku Tree slingshot room, chunk 4) | B's door or ladder stays: the Deku Tree ladder softlock (#129) | **P1** |
| Crystal switches (hit) | `Obj_SwitchHit_*`, `Obj_InsideJabu_Jabu_SwitchHit_*`, `Obj_LakeHrl_SwitchHit_*`, `Obj_LightSwitchHitBase` | flags | ? | unit | As above | **P1** |
| Rusted and hammer switches | `Obj_FireTemple_HammerSlipPillar_*` (*keys*, kept), hammer floor switches `Obj_HammerSwitch_*` (no key; a faller too since breakables-2) | world `WORLD_BREAK` as a *faller* (breakables: life 0 written in B, again each second while it stands, at most 10 times; never moved, never deleted; the same pillars in the Gerudo Training Ground are covered) + *keys* for a reload; flags | live (breakables) | unit | B's pillar stands: a missing bridge in the Fire Temple | **P1** |
| Sun switches, mirrors, light beams | `Obj_SolarSwitch_*`, `Obj_SpiritTemple_Mirror`, `Obj_SpiritTemple_CapstanMirror`, `Obj_SpiritTemple_RayShaft_*` | outcome flags; mirror angle none | ? / - | - | Each player turns their own mirror; the door's flag is shared | P2 |
| Torches lit for good | `Obj_CandleStick_*`, `Obj_*_CandleStick_*`, `Obj_ForestTemple_SwitchCandleStick_*`, `Obj_Well_CandleStick_*` (key) | flags; Well torch key, kept | ? | unit | The door the torches open | **P1** |
| Shutters and barred doors | `Obj_*_SwitchShutter_*` (574 placements), `Obj_DoorChainLock*`, `Obj_Base_RestrictedDoor` | flags (`…_ShutterOpen`, `…_OpenDoor…`) + played events | ? | unit | A walks through, B's stays shut | **P1** |
| Locked doors (small key) | `Obj_DoorChainLock*`, `Obj_*_DoorChainLock_*` | flags (`…_UnLock`) + the shared key count | ? | unit | The key is spent for everyone; if B's door does not open live, B is stuck (see Small keys) | **P1** |
| Boss doors | boss key doors | items OR + flags | ? | - | B needs the boss key: shared, so OK if the flag is live | P2 |
| Enemy-clear rooms | `Obj_EnemyCounter_*` (5 names, 51 keys, kept), `…_Clear`/`_Cleared` flags | flags + keys; enemies by objects (kills: issue 133 open) | ? | logs (Deku Babas placed in B, chunk 3) | B enters, the shutters close and B's own copies still live. Kills not shared means B must kill them again. A's `_Cleared` flag may open B's shutters while B's enemies are alive, or not at all. Either way B can be locked in | **P1** |
| Water level (Water Temple) | `Obj_WaterTemple_WaterPlane*` (8+2), `WaterTemple_Room00_WaterLevel` (Int), `HavePlayedEvent.WaterTemple_WaterLevelChange01` | flags (Int) + played event | ? | - | A changes the level, B's water does not move: B swims in water that is not there in A, or the reverse, and doors that need the level stay shut. The level change also plays an event, which B never plays | **P1** |
| Other water planes (Deku Tree, Jabu, Forest, Well, Lake Hylia, Zora) | `Obj_*_WaterPlane*`, `Obj_InsideTheDekuTree_WaterGate` | flags where the room has one | ? | - | Lake Hylia's refill after the Water Temple is story state | **P1** to check |
| Push and pull blocks | `Obj_MovableBox_*`, `Obj_ForestTemple_MovableBox_*` (5), `Obj_FireTemple_MovableBox_*`, `Obj_SpiritTemple_MovableBox_*`, `Obj_LonLonRanch_MovableBox`, `Obj_SliderBox_*`, `Obj_AmosStatue_*` | world `WORLD_MOVE`/`WORLD_PLACE` | ? | unit | Blocks out of place: a block pushed into a hole or onto a switch in A sits elsewhere in B, and B cannot push it back | **P1** |
| Elevators and lifts | `Obj_InsideTheDekuTree_ElevatorLift*`, `Obj_InsideJabu_Jabu_ElevatorLift_*`, `*ElevatorLift*` | none (each game's own); start events left out on purpose | - | - | Each rides their own; OK as long as each lift works alone | P2 |
| Moving and rising floors | `Obj_*_MoveFloor_*`, `Obj_FireTemple_RiseFloor_*`, `Obj_FireTemple_SinkFloor_*`, `Obj_ShadowTemple_GimmickFloor`, `Obj_WaterTemple_FloatFloor*` | flags where switch-driven; positions none | ? | - | Driven by a shared switch flag; if live, fine | P2 |
| Spinning rooms, Forest Temple corridor | `Obj_ForestTemple_TwistedCorridor_*`, `Obj_ShadowTemple_RotateFloor` | the start events are left out (one-way, review of 41d5b76), BUT the corridor's own state flags **are shared** (in the scan list): `ForestTemple_TwistCorridor_IsReverse`, `ForestTemple_TwistCorridor01_IsConstraint`, `…02_IsConstraint`, and `ForestTemple_SageOfForest_TwistedCorridor_A_01` to `_04` | ? | - | If B's loaded corridor follows `IsReverse` live, it turns under B while B is in it or on the far side. If it follows only on a reload, B's corridor is in the other state from what B's room logic expects. Decided (dungeon-live): shared, but held while the player is in the Forest Temple and written on their next load or scene entry (docs/dungeon.md, "Held until the room loads"), so the corridor never turns under anyone | done (unit tests) |
| Shadow Temple boat | `Obj_ShadowTemple_PaddleShip_*` | event left out | - | - | Each player rides their own boat | P2 |
| Chests | `Obj_TBox_*` | *keys* + opened flags + *take*: shown open in B (`OpenWait` played), removed only if that never takes (issue 195) | live | unit (chest-sync-2); game: chest-other to run | B may still open a shown-open chest (double small key) | P2 |
| Chests that appear (switch, enemies) | `Obj_TBox_*` with `…_TBoxAppear`, `…TreasureBoxAppear` events | flags + played events | ? | unit | Chest missing in B: if it holds a needed item, the item is shared anyway | P2 |
| Song of Time blocks | `Obj_SongOfTimeBlock` | none known | - | - | A plays, B's block stays (Spirit Temple, Temple of Time) | **P1** to check |
| Hookshot targets, crumbling platforms, spikes, blades, guillotines, fire walls | `Obj_*HookShotTarget*`, `Obj_CrumblingGanonCastle_*`, `Obj_MoveSpike_*`, `Obj_ShadowTemple_*Blade*`, `Obj_FireWall*` | none (each game runs its own) | - | - | - | P3 |
| Lens of Truth objects | `Obj_*_GlassTruth_*` | none (per player: each player's own lens) | - | - | - | P3 |
| Mini-boss and boss rooms | boss actors | boss `BOSS_ROOM` (all in, intro together) + named `…_Boss_Defeat` | live | game (boss 20261110-124007: B moved in, intro) | Walk-out after the fight (boss-walkout, chunk 5); the blue warp and medal per player | **P1** (warp out) |

## World and story

| State | Path | Live | Proven | Risk | P |
|---|---|---|---|---|---|
| Overworld doors, gates (Kakariko, Lon Lon, Gerudo, castle gate) | named flags where story, else none | ? | - | A gate opened by story in A, shut in B | **P1** to check |
| Story events, cutscenes, medals and stones | items (medals and stones are items) + the overworld's story scenes and story gates (`CoopStory::Flags()`, [flags.md](flags.md#story-and-cutscenes-issue-173)); dungeon scenes, time travel and bosses not shared | live; late joiner by the host's table | unit | Before (issue 173): each player watched Saria and the owl; a guest behind the host kept the forest-exit wall | **P1** to check in game |
| Threads of Time (pause menu quest log) | the unlock flag `UI_IsFinishChallengeMenuOpenEvent` in `CoopStory::Flags()`; 11 listed quests' saved Enums by step order, highest wins (`CoopQuests`, [flags.md](flags.md#threads-of-time-issue-193)) | live; late joiner by the host's table | unit | Before (issue 193): the second player never saw the owl, so the tab stayed shut; quests were each game's own | **P1** to check in game |
| Time of day | `TIME_OF_DAY` (host's clock) | live | game | - | P3 |
| Song events (Zelda's Lullaby doors, Sun's Song, Song of Storms well) | none, unless the result is a flag | ? | - | The door a song opened in A stays in B | P2 |
| Shops (stock sold out) | none; a bought Deku Shield reaches everyone (max, issue 130), a lost one is bought again by that player | - | - | - | P2 |

## Per-player state (not shared on purpose)

Position, life (except the max), magic, rupee count, what is in the hands and on the buttons, bottles' contents, equipped gear, the camera, Navi's hints, "in battle", the Lens of Truth, which room a player is in, one-way rides. Minigames played alone: the Scrub Brothers' order (issue 149) and the fishing pond (issue 151: the owner, the rod, the catch record `MiniGame_Fishing_RecordFishSize_Child`/`_Adult` and the tutorial flags); only their prizes are shared (the pond's fish and the cast lures are shared since fishing-2, items.md). These rows must stay per player; a fix must not share them.

## P1 list

The P1 rows above, grouped by the fix branch that should take them:

1. **dungeon-live** (types 110 to 119):
   - Make a loaded door, shutter, ladder, water plane or platform react to a shared flag at once. Covers the switches, eye and crystal switches, torches, shutters, locked doors, water level, and the room-twist check.
   - Held floor switches (Caleb's button case): share the held state, or make "pressed by anyone" hold the door in every game.
   - Enemy-clear rooms: the clear and the shutters must agree with B's live enemies.
   - Silver rupee counters: one count for the group.
   - The Water Temple level and its event.
   - Song of Time blocks.
2. **breakables** (types 120 to 129):
   - Path blockers: bomb and hammer rocks, cracked walls and floors, webs, red ice and ice, and the Deku Tree falling floors. Proven removed live in B, also for a B that loaded the room earlier.
   - Hammer pillars falling live in B.
   - Push blocks in the same place in every game, including blocks dropped in holes and on switches.
3. **softlock-audit** (no message):
   - The cases above where B ends up with 0 keys at a locked door, shut in a cleared room, behind a block, or before a story gate. Check the story flags left out (`Dm_`/`Demo_`) for any that open an area.
   - The boss walk-out and the blue warp per player.

## Softlock

Branch `softlock-audit`: every way co-op could leave a player unable to go on, and whether the code on main handles it. Read from the code, the docs and the romfs (`Product.111` dump: `Logic/*.ainb`, `AI/*.ainb`, `Event/EventFlow`, `Banc/*`, `GameData/GameDataList`); nothing here was run.

How the game keeps progress, in short: items are shared by rule ([items.md](items.md)); a dungeon's rooms are named saved flags (reset type 8) and placement keys (reset type 24), shared by [dungeon.md](dungeon.md)'s lists; chests and one-time pickups everywhere are keys ([items.md](items.md), "Chests"). **Overworld story flags are shared** (issue 173): the story scenes seen and the gates they open follow the host's save ([flags.md](flags.md#story-and-cutscenes-issue-173)); scenes inside dungeons, time travel and bosses stay each game's own.

Status: **Fixed** (on main, or in this branch), **OK** (the rules already cover it; the reason is given), **P1** (can block a player; to do), **P2** (can annoy or cost time, no hard block), **Check** (needs a run to know).

### Dungeons

| # | Case | Status | Why / what to do |
|---|---|---|---|
| D1 | Room state the dungeon list missed: 20 Jabu-Jabu flags spelt `Insidejabujabu_` (rooms cleared, the Room 7 switches and gates, the Room 12 chest gimmick), `Firetemple_`/`Foresttemple_SongOfTimeBlock_A_01_Disappearance`, the enemies-beaten and torch shutters `Gimmick_DefeatEnemy_Open_{DodongoCave,FireTemple,ForestTemple}_*`, `Gimmick_DeleteFire_FireTemple` | **Fixed (this branch)** | The scan took names that start with the dungeon's exact spelling. Enemies are shared and die once in every game, so a room whose shutter opens on "all enemies beaten" never opened for the second player: their enemies were already gone. 26 flags added to `coop_dungeon.cpp`. Left out after review: the Room 7 area sensors (`…_InsideArea`, `…_NearTheSwitch*`, set both ways from where a player or Ruto is), `…_OnlyLinkOrLuto` (Ruto), two-way state (D8), the Spirit Temple stone tablets (what Link reads). Second pass, 17 more: the Fire Temple's Goron cell shutters `Npc_Goron_0NN_ShutterisOpen` (9, in the scene's `PlgGdTable`) and 8 dungeon gimmick scenes (D14). 1,014 flags in all. **Check:** Jabu's Room 17 stone switch `InsidejaubjabuBelly_gimic_switchOn_Stone` (0x6fd2e588) is left out: its step-switch graph has set-true and set-false nodes, so it may go back to 0 when the stone leaves the switch (in-game check 8). |
| D2 | Small key spent twice for one door: both players open the same locked door before the other's break arrives | **Fixed (branch `locked-doors`)** | The lock is shared as a kept `WORLD_BREAK`. A lock another game opened is not deleted while our Link is within 2.5 m; if our player opens it anyway, our second key is given back. Two breaks at once: the server passes on both and the higher player number gives its key back. One key per door for the group ([dungeon.md](dungeon.md), "Locked doors"). Unit tests; in-game check 7. |
| D3 | A locked door opened in one game stays locked in the other while the key is spent for both | **Fixed (branch `locked-doors`), check** | The lock's "open" is a bool its AI makes from the placement, not found statically. So the lock actor itself is shared: opened in one game, it is deleted in the others (at once for a room loaded later, a late joiner too), and the server keeps that for the session. Not known until a run: that the door opens with its lock deleted (in-game check 7). If not, the run's `LICHII_COOP_DEBUG_FLAGS` lines while opening one show the bool to share (check 6). | **door-unlock (2026-11-12, after a live report of exactly this):** the lock is no longer deleted by default; it stays, and a player standing at it for 1 s is lent one key and opens it as the first player did (the lent key is that door's one spend; unused, it is taken back). Unit tests; runs `lock-open` show the break sent and received, not yet a second player through the door. |
| D4 | Key chest opened by both at once (the near rule leaves a chest loaded beside a player) | **Check** | Two +1 for one key: more keys, never fewer, so no block. deku-softlock check 5. |
| D5 | Loose Shadow Temple small key (no key of its own) taken in both games | **OK** | Two +1: a spare key, no block. |
| D6 | Enemy-counter rooms | **OK** | The 51 `Obj_EnemyCounter_*` keys and the `…_Clear`/`…_Cleared` flags are shared; D1 adds the misspelt and `Gimmick_` ones. |
| D7 | One-way rides (Shadow Temple boat, Forest Temple elevator and room spins) | **OK** | Left out of the list on purpose: each player rides their own. |
| D8 | Water Temple water level changed by one player while the other is elsewhere; also Jabu-Jabu's Room 14 water (`Gimmick_Insidejabujabu_*MoveWater*`) and the Water Temple torch shutters (`Gimmick_Ignition_Open_WaterTemple_01`/`02`), which step switches and torches set both ways and which are not shared | **P2** | Live, last write wins. A player can find the water at another height and must go to a level switch. No hard block known (the three switches are reachable at each level in the original), but it can cost time. Players should agree before playing Zelda's Lullaby at a switch. |
| D9 | Boss beaten in one game; the other enters the boss room later; the blue warp, the medallion and its flags | **OK, with a check** | A boss's end state is its own scene, `HavePlayedEvent.Dm_<dungeon>_BossOut` (and `Gohma_DefeatBoss`, `Morpha_DeleteWater`), per game; only the heart container key and a few room flags are shared. With boss-walkout every player in the dungeon is in the fight, sees the death and plays their own `BossOut`, warp and medallion scene, so each game sets its own story flags; the medallion item is shared anyway. A player who was not in the dungeon finds the boss alive and fights it (the heart container is gone): no block. Sharing a boss's defeat flag alone was left out on purpose: a game with the boss marked dead but no `BossOut` played may not make the warp, and the room has no other exit. Check 4. |
| D10 | Push blocks needed to go on (Forest Temple, Kakariko well, Spirit Temple) | **Check** | `WORLD_MOVE`/`WORLD_PLACE` move them live (unit tests only). [world.md](world.md) check 4: B can push the block afterwards. |
| D11 | Things removed from the other game when taken | **OK** | `CoopObjects::RemovedWhenTaken` takes only things that go away (chests, pickups, webs, red ice, cracked walls, broken rocks, the Deku Tree's falling floors); counters, hammer pillars and the Shadow Temple's falling floor stay. |
| D12 | Gerudo Fortress: the cell keys (`IsGet_SmallKey_GerudoFortress.SmallKey01`–`04`) and cell doors are per game; `DungeonInfo.GerudoFortress.SmallKeyNum` is shared | **P2** | Each game has 4 keys and 4 cells, and every pickup and spend counts once for both, so the sums match when both free all four. Halfway, one player can spend a key the other picked up. Not a block: the membership card (shared) is what the rest of the game needs. Sharing the key flags alone would make it worse (4 keys for 8 cells). |
| D14 | Dungeon rooms whose end state is a `Dm_` scene the room's logic reads (Dodongo's broken stair `Dm_DodongoCave_0010`, the Fire Temple hammer stair `Dm_FireTemple_0029`, Forest Room 16 `Dm_ForestTemple_0010`/`0014`, Water Room 17's pond `Dm_WaterTemple_0010`/`0015`, Ganon's Castle lift rock `Dm_InsideGanonCastle_0035` and tower barrier and floor `0090`) | **Fixed (this branch)** | The issue 129 ladder case again: the trigger was shared, the scene was not, so the second game's room never reached its end state (the last one would keep the tower shut). Their played events are now shared. Left out: `Dm_ForestTemple_0007` (the twisting corridor, one-way), `Dm_WaterTemple_0022` (read with the Longshot in the main room; not known what it is), `Dm_Deku_BossIn` (the boss rule). |
| D13 | Ammo for items shared without it (sticks, seeds, nuts, bombs, arrows) | **Fixed (main)** | #128 / #138: capacity with the item, the mend after a load. |

### Items, trades and bottles

| # | Case | Status | Why / what to do |
|---|---|---|---|
| I1 | Ruto's letter | **OK** | `IsGet.Item_Bottle_Letter` is not shared, so the letter is still in the other game's lake (`Obj_Bottle_Letter`, no key). Bottle contents are per player, so each player takes their own letter for King Zora (whose state is per game). Side effect: the second letter's bottle is shared as a new bottle (one extra empty bottle for everyone); harmless. |
| I2 | Fish, Blue Fire, bugs and other bottle contents needed to go on (Jabu-Jabu, Zora's Domain ice, the Ice Cavern) | **OK** | Per player, and every source can be used again. |
| I3 | Trade sequence items (mirror rule): one player hands an item over and it is gone for everyone | **P2** | Optional quests (Biggoron's Sword, masks). A player halfway through loses their step. Later: share the trade step too, or make the trade items per player. |
| I4 | Zelda's Letter, Ocarina of Time, songs, stones and medallions | **OK** | Shared items let the other player do the per-game event themselves (Kakariko gate, Temple of Time door) or skip it. |
| I5 | Deku Shield bought once for everyone (#130) | **P2** | Shared again (max, branch shop-shield-2; items-progress had made it per player); the shop opens for every player (quest catch-up); autotest `shop-buy`. |
| I6 | Keys and dungeon items with a join at another save point | **P2** | A late joiner takes the host's totals. A key the joiner's save had in hand but the host's did not is lost to the joiner; the chest it came from stays open in the joiner's game but closed in the host's, so the host player can take it again for both. Advice: join with saves from the same point. |

### Story, time travel, saves

| # | Case | Status | Why / what to do |
|---|---|---|---|
| S1 | Story scenes (Deku Tree, Zelda, Darunia, King Zora, Gerudo carpenters, sages) | **Shared (issue 173)** | The overworld's story scenes are shared from the host's save (`lichii/hook/coop_story.cpp`, [flags.md](flags.md#story-and-cutscenes-issue-173)): one player seeing a scene counts for everyone. Still each game's own: the opening of a new game, time travel, the Chamber of Sages (the first sword pull, the sages' awakenings), scenes inside dungeons, bosses (D9), talks. |
| S2 | One player adult, one child | **OK** | Shared flags belong to scenes; a child cannot be in an adult-only room. Small keys and dungeon flags are the same for both ages, as in one save. The Master Sword and time travel are per game. |
| S3 | Ganon's Castle escape and the last fights | **Check** | The escape flags (`EscapeFromCrumblingGanonCastle_*`) are per game; Ganondorf is one boss for all ([dungeons-boss.md](dungeons-boss.md)). Check what a second player inside the castle sees when the first beats Ganondorf. |
| S4 | Saves loaded at different points | **P2** | The joiner gets the host's table (`FLAG_SNAPSHOT`); its own 1s that the host has at 0 stay in its game and do not cross until they change. See I6. |
| S6 | Story gates: a scene or flag that the overworld's placements or field logic read (the forest-exit wall, Mido, Jabu-Jabu's mouth, Kakariko's well, Lake Hylia's ice, the Death Mountain gate) | **Shared (issue 173)** | Before, each was the scene of an event the other player could still play, but a guest behind the host kept the forest-exit wall (issue 173). The gates' flags (`Presence` in the Banc files) and the scenes are now shared, so the host's save opens them in every game. |
| S5 | Overworld entrances that need a per-game action (Kakariko well drained, the graveyard's Shadow Temple entrance `GraveYard_Field_ShadowTemple_ShutterOpen`, the Gerudo Valley bridge) | **OK** | Each player can do them: the songs, Din's Fire and the card are shared. |

### In-game checks (integrator)

1. Jabu-Jabu (D1): A beats a room's enemies (Room 1 or Room 5). B, in the same room or entering after: the shutter is open. B's log (`LICHII_COOP_DEBUG_FLAGS=1`): `bool 0x2ea0d05d 0 -> 1` (`Insidejabujabu_Room1_Cleared`) or `0x695a1137` (Room 5). Dodongo's Cavern `Gimmick_DefeatEnemy_Open_DodongoCave_01` (`0xdba3c3ef`): the same.
2. Locked door (D3): Water Temple or Fire Temple, both with one key. A opens a locked door. On B's screen the door opens (or is open when B comes), and B's key count drops by 1 only. If B's door stays locked: report the room; that dungeon's lock flag must be found (P1).
3. Same door at once (D2): both stand at a locked door with one key; both press open within a second. Read `DungeonInfo.<dungeon>.SmallKeyNum` in both logs: it must drop by 1. If by 2: D2 is real (P1).
4. Boss room late (D9): A beats Gohma while B is outside the Deku Tree. B goes to the boss room later: Gohma is there and B can beat it, the warp then appears and takes B out (or, with the walk-in, B was pulled in and saw the death). Either way B is never in a boss room without a way out.
5. Room gimmick scenes (D14): A drops Dodongo's Cavern's stair (or clears the Water Temple Room 17 pond). B's log: `bool 0x<hash of HavePlayedEvent.Dm_DodongoCave_0010> 0 -> 1`. B's stair is down, live or after re-entering the room (say which).
7. Locked doors (D2, D3), Water or Fire Temple, both with the same key count. (a) A opens a locked door with B more than 3 m away: B's log `world: <hash> Obj_DoorChainLock_A_01 broken by another player`, B's lock goes, **B can walk through the door**, B's key count dropped by 1 only. (b) Both stand at the next locked door; B waits by it while A opens it, then B presses open too (or both press at once): one log has `one key back` and `coop: doors: small key … given back`, and both games end with the count down by 1 only. (c) Both leave the dungeon and come back: the opened doors stay open in both. Since `door-unlock`: (a) is with `LICHII_COOP_LOCK_DELETE=1`; by default B's lock stays, B walks up to it, the log has `one key lent to open it`, B opens it with A and walks through, then `lock opened here with the lent key`, and both counts end down by 1 only (scenario `lock-open`).
8. Jabu-Jabu Room 17 stone switch (D1, Check), `LICHII_COOP_DEBUG_FLAGS=1` on A: A puts the stone on the switch, then takes it off or leaves the room. Copy A's `bool 0x6fd2e588` lines. Only `0 -> 1` and never back to 0: one-way, it can be shared. Back to 0: it stays per player.
6. Lock flag hunt (D3, only if 7a fails): `LICHII_COOP_DEBUG_FLAGS=1` in A only. A opens one locked door in the Water or Shadow Temple. Copy every `bool …` and `key64 …` line logged within 2 s of the door opening into the report, so the lock's flag can be added for all doors.

## Added for protocol 10

Merged into `integrate-p11`, with unit tests and review. None of it is proven in the game yet: the combined autotest run of that branch is the proof, and each fix branch's rows above get their **Proven** column from that run. Details are in each branch's commits and the topic docs.

| What | Issues | Branch |
|---|---|---|
| Enemy sharing fixes: owner, life, hits | #105 #132 #133 | enemy-sync-3 |
| Enemy shots: every game shows the owner's shot | #189 | enemy-shots |
| Enemy drops | #101 #102 | enemy-drops |
| Where shots land (spark, sound) | #166 | shot-impacts |
| Deku Scrubs; their talk and sales | #149, #144 | deku-scrubs, scrub-talk |
| Signs, rocks and more world objects | #84 #90, #146 #147 #171 | world-breakables, world-objects-2 |
| Guards and town soldiers | #191 | guard-sync |
| Boss targets, boss walk-in, Gohma and its larvae | #124 #150 #157, #148, #178 | boss-targets, boss-movein-2, gohma-sync |
| Dungeon events started in one game play live in the others (lifts, ladder) | #176 #196 | deku-dungeon-2 |
| Story cutscenes play in every game near the player who started one | | shared-cutscenes |
| Chests opened elsewhere show open | #195 | chest-sync-2 |
| Gold Skulltula tokens, the Deku Shield, the ocarina | #89 #130, #190 | items-sync, ocarina-use |
| Story scenes, quest steps, time of day, cutscene positions | #173 #192, #193, #194 | time-story, threads-of-time, cutscene-snap |
| Fishing: two players at once, each with their own catch | #151 | fishing-3 |
| Riding Epona | #199 | epona-ride |
| NPCs' moves copied from one game | #200 | npc-anim |
| Navi in the right room | #145 | navi-room-2 |
| A guest's save load no longer resets the host's world | #159 | world-reset |
| PvP, off by default ([play-online.md](play-online.md#pvp-players-hurting-each-other)) | | pvp |

## How to keep this page

A fix branch changes its rows: Path, Live, Proven (with the run, once integrate-p8 has run it) and P. It adds a row for any actor class it finds that is missing here. Keep the P1 list in step.
