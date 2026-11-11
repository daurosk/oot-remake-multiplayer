# Boss rooms together

Issue 109. Caleb: "if two players are in the same dungeon and one walks into boss room, both players should receive the same cutscene and fight it together."

Game: main of build e0f3ccfaf50065ae (1.1.1), romfs of the same version. **Checked** means read from the game's files. **Live** means seen in the running game. Nothing on this page has been seen live yet.

Code: `lichii/hook/coop_boss.{h,cpp}` (the rooms and the rules, no emulator code; tests in `tests/lichii/boss_test.cpp`), `lichii/hook/coop_boss_game.cpp` (reads the game, moves Link, skips the intro). Message: `BOSS_ROOM`, type 50 ([protocol.md](protocol.md)).

## What the game files say

- **A boss room is a room of its dungeon's scene, not a scene of its own** (checked). `Banc/<Dungeon>/` holds one Static and one Dynamic file per dungeon; the boss, its heart container and its blue warp (`Obj_WarpPoint_A_01`) are placements in it with `Dynamic.DefaultDungeonRoomIndex` set to the boss room. So "walking into the boss room" changes no scene, and the scene name alone cannot tell it. The hook tests where Link stands instead: the dungeon's scene and a circle and height band around the room's placements.
- **The boss rush has a start point for every fight** (checked). `Banc/DreamWorld` (the boss rush) has a `StartPos` named `<Boss>Stage` for each boss, at the same coordinates as that boss's room in its dungeon (for example `GohmaStage` at 6.7 47.0 67.6, the Deku Tree's `StartPos_Get_StoneOfForest`). A moved player is put there.
- `GameBancParam` also has `DodongoStage`, `PhantomGanonStage`, `GanonStage`, `GanondorfStage` and `BossRushIntervalRoom`; those are the boss rush's, not the dungeons'.

## The eight rooms

| # | Dungeon scene | Age | Boss | Room index | Room: centre x, z, radius; height | Arrival (`<Boss>Stage`) | StartPos in the dungeon's room | Intro / outro event |
|---|---|---|---|---|---|---|---|---|
| 0 | `InsideTheDekuTree` | child | `Enemy_DungeonBoss_Gohma` | 10 | 7.5, 67.2, 37; 43 to 63 | 6.7 47.0 67.6 | `StartPos_Get_StoneOfForest` | `Dm_Deku_BossIn` / `Dm_Deku_BossOut` |
| 1 | `DodongoCavern` | child | `Enemy_DungeonBoss_Dodongo` | 22 | 24, -63, 22; 69 to 90 | 24.0 73.4 -77.5 | `StartPos_Get_StoneOfFire` | `Dm_DodongoCave_BossIn` / `_BossOut` |
| 2 | `InsideJabuJabuBelly` | child | `Enemy_DungeonBoss_Barinade` | 15 | 42.3, -110.3, 21; 97 to 118 | 42.4 102.0 -98.1 | `StartPos_StoneOfWater` | `Dm_InsideJabu_Jabu_BossIn` / `_BossOut` |
| 3 | `ForestTemple` | adult | `Enemy_DungeonBoss_PhantomGanon` | 29 | 0, -62.6, 20; 71 to 95 | -5.6 74.7 -65.8 | `StartPos_SageOfForest` | `Dm_ForestTemple_BossIn` / `_BossOut` |
| 4 | `FireTemple` | adult | `Enemy_DungeonBoss_Volvagia` | 33 | -82, 0, 38; 88 to 125 | -64.5 102.6 0.0 | `StartPos_SageOfFire` | `Dm_FireTemple_BossIn` / `_BossOut` |
| 5 | `WaterTemple` | adult | `Enemy_DungeonBoss_Morpha` | 23 | -4.5, -91.8, 20; 120 to 145 | 0.2 131.4 -87.1 | `StartPos_SageOfWater` | `Dm_WaterTemple_BossIn` / `_BossOut` |
| 6 | `ShadowTemple` | adult | `Enemy_DungeonBoss_BongoBongo` | 24 | -138.3, 37, 22; -16 to 6 | -138.3 -11.5 22.0 | `StartPos_SageOfShadow` | `Dm_ShadowTemple_BossIn` / `_BossOut` |
| 7 | `SpiritTemple` | adult | `Enemy_DungeonBoss_Twinrova` | 33 | 0, -157.8, 38; 139 to 165 | -0.7 142.2 -158.6 | none | `Dm_SpiritTemple_BossIn` / `_BossOut` |

The room circles are drawn around the room's placements (heart container, blue warp, the room's own props, doors). They are a guess at the walls: a run must show that a player standing in the room is inside, and one in the room before it is not (check 2 below). The heights stop the rooms above and below from counting (the Shadow Temple's `StartPos_SageOfShadow_BeforeBossRoom` is right above the boss room, at height 42).

Ganon's Castle (`InsideGanonCastle` room 63, `Enemy_DungeonBoss_Ganondorf`; Ganon in `GanonCastle`) is left out: it is the end of the game, not one of the eight.

## How it works

1. **Entering.** A game whose Link is in a boss room sends `BOSS_ROOM` "in the room" at once and every 2 s while there.
2. **Moving the others in.** A game whose Link is in the same dungeon scene, but not in that room, moves Link to the room's arrival point, 1.5 units from it by player number. It tries again every 3 s while the other player is still in the room, at most 5 times per fight. This is also rule 4: a player who joins or dies during the fight comes back in the dungeon's scene (the game restarts a dead Link at the dungeon's entrance) and is moved in again. The pull stops when nobody has said "in the room" for 6 s, when they left, or when the boss is beaten.
   - **How Link is moved.** While an event runs in our own game, the game's own scene change takes a request (as the autotest `warp`, [testing.md](testing.md)): `Sequence_ReservedNextStartPosName` = the room's StartPos, then `Sequence_ReservedNextBanc` = the dungeon; the dungeon loads again with Link in the boss room. In free play the scene change is not taken (seen in autotest runs), so the hook does what the AI node `ExecuteActorForceSetMtx` does: writes Link's place (actor +0x31c) and calls the actor's vtable slot 0x218 = 0x4c6580(actor, 0), the engine's warp of the whole physics body ([world.md](world.md)). The scene stays loaded. `LICHII_COOP_BOSS_MOVE=matrix` always uses this one. The Spirit Temple's room has no StartPos, so it is always moved this way.
3. **The intro.** The first event that starts while Link is in the room is the intro (`intro started`); its end is `intro over`. A game cannot start another game's intro: a moved player's own game plays it when Link arrives (if the room starts it on entry; check 4). To leave together, a game whose intro still runs once any other player in that room has finished theirs skips the rest of it with the game's own force skip: `SeqOneShotEventRequestForceSkip` (0x32d1890) takes the event manager, and while an event runs calls 0x3b3af30 with the current event (`[[S+0xeb8] + 8 * u32 [S+0xf18]]`, `S` = manager +0x68). The hook does the same from the svc cave, every 0.5 s for up to 6 s. If no event starts within 10 s of entering (the intro was seen before), the fight has begun.
4. **The boss's death.** The next event in the room after the fight began is the outro: `the boss is beaten`. Nobody is pulled into that room again until they leave the dungeon's scene.

Off with `LICHII_COOP_BOSS=0`. `LICHII_COOP_DEBUG_BOSS=1` logs the phase each second. Log lines start `coop: boss:`. The cave (moves and skips) is patched in only with puppets or spawns on (`LICHII_COOP_PUPPET`, as every kit has).

## One boss (rule 3)

The boss and its parts must be one object group with one owner (OBJECT_GROUP, [objects.md](objects.md) "Groups", issue 105). This branch does not touch the object code; the part lists are in `coop_boss.cpp` (`Room::parts`) for the object-groups work to use:

| Boss | Parts (actor names) |
|---|---|
| Gohma | `Enemy_DungeonBoss_Gohma`, `_Gohma_Egg` |
| King Dodongo | `Enemy_DungeonBoss_Dodongo`, `_Dodongo_Attention`, `_Dodongo_Attention_RollingCenter`, `_Dodongo_SpawnBabyDodongo`, `DungeonBossDodongo_Bullet_Fire`, `_Bullet_StrongFire` |
| Barinade | `Enemy_DungeonBoss_Barinade`, `_Barinade_Arm`, `_Barinade_Attention`, `_Barinade_Beam`, `_Barinade_Jellyfish` |
| Phantom Ganon | `Enemy_DungeonBoss_PhantomGanon`, `DungeonBossPhantomGanon_Bullet_LightBall` |
| Volvagia | `Enemy_DungeonBoss_Volvagia`, `Dm_Enemy_DungeonBoss_Volvagia`, `DungeonBossVolvagia_Appear_Sign`, `_FireBreath`, `_FireBreathField` |
| Morpha | `Enemy_DungeonBoss_Morpha`, `_Morpha_Attention`, `_Morpha_Attention_Spin`, `_Morpha_Tentacle` |
| Bongo Bongo | `Enemy_DungeonBoss_BongoBongo`, `_BongoBongo_LeftHand`, `_BongoBongo_RightHand` |
| Twinrova | `Enemy_DungeonBoss_Twinrova`, `_Koume`, `_Kotake` |

The death, heart container and blue warp then follow from the shared boss (its death replayed in every game) and from the dungeon's shared flags ([dungeon.md](dungeon.md)).

## The boss goes to the player who hurts it (issue 150)

Testers on test6 (Gohma): the boss only ever followed and attacked player 1, its owner; and after player 2 stunned it, player 2's copy was turned the wrong way and could not be hit until player 1 hit it.

**Why.** Only the owner's game runs the boss; every other game's copy is held to the owner's pose and animation. A boss is in its fight for 60 s after every hit (`boss_fight_lock_us`), and an object in a fight never changes owner, so the boss stayed with player 1 for the whole fight. Its AI in player 1's game only knows player 1's Link (the game has one player; the puppets are not targets), so it went for player 1 alone. Player 2's seed in the eye stunned player 2's copy in player 2's game (the weak point hit and chance time are in Gohma's own AI: `DungeonBossGohma.action.interruptweakpointhit`, `.interruptchancetime`), but not player 1's boss; the copy then kept being turned to player 1's boss while its own AI lay stunned, until player 1's own hit changed the owner's state.

**What changed (server, `Session::on_damage`).** A `DAMAGE` on a boss or any part of its group from a player who does not own it hands the boss root (and so the group) to that player, at most once every 8 s (`boss_hitter_cooldown_us`; 0 turns it off). The hit's effect, a stun included, is then in the owner's game, the other copies follow it, and the boss turns on the player who hurt it last: in a long fight it goes back and forth with the hits, which gives both players their turns. It is not handed over while its owner hits it too: a life drop in the owner's samples with no `DAMAGE` passed on in the second before is the owner's own hit, and for 6 s after one (`boss_owner_hit_hold_us`) the boss stays, so two players hitting in turn do not bounce it (each hand-over can jump a phase: the state carries only place, turn, animation and life). The distance rule for plain enemies is unchanged. Tested in `tests/server/session_test.cpp` (issue 150 cases).

**Not covered.** A stun that takes no life sends no `DAMAGE` (a hit is only seen as a life drop): if the seed in Gohma's eye costs no life, player 2's stun still does not move the boss, and player 2's sword hits on it afterwards do. Making the owner's AI target the other player's puppet was looked at and left alone: the game's target choice reads its one player actor, and offering the puppet as a target would need code in the game's AI that we cannot test here.

## Not known yet (for the in-game check)

1. Whether the force-set-matrix move into a room of the same scene loads that room's walls and floor, or Link falls through (the dungeon may stream rooms by `DungeonRoomIndex`). If he falls, the fallback is the scene change, which needs an event running.
2. Whether the room circles match the walls.
3. Whether the force skip of a boss intro leaves the boss in its fight state (the graphs use it for going back to the title).
4. Whether a moved player's game starts the intro itself, and whether the outro is the first event after the fight (a hint from Navi in the fight would be taken for the boss's death and stop the pull; harmless otherwise).
5. Whether a boss door's own event (the boss key, the door opening) counts as the intro: it starts before Link is inside the circle, so it should not.
