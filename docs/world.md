# Shared world objects

dauro (2026-11-09): every world object the same for all players. A pot or crate broken by one player breaks for all; an object moved by one moves for all; a switch pressed or a dungeon event started by one happens for all. This page says how, in the order it is built: breakables first, then switches and doors, then movable blocks, then the Water Temple's water level. Enemies are [objects.md](objects.md); objects players make (bombs, arrows) are the spawn-sync work.

Game build e0f3ccfaf50065ae (1.1.1). **Checked** means read from the romfs or the code; **live** means seen in a run.

## Breakables (first cut, branch `world-sync`; more in "Broken for everyone" below)

### What breaks

Every `Obj_` actor pack with a `LifeRef` and a `DestructionRef` (checked: all 8,373 packs read from the romfs, 2026-11-09). These are the same Life and Death components enemies have, so a breakable dies the way an enemy does.

| Kind | Packs (name parts) | Life seen live (Kakariko) |
|---|---|---|
| Pots | `Obj_Pot_*` (15, the respawning and dream ones too), `Obj_ShadowTemple_SkullPot_*`, `Obj_Gerudo_HorsebackArcheryPot_*` | |
| Crates | `Obj_BrokenBox_*` | 5 of 5 |
| Rocks | `Obj_LiftRock_*` (lifted and thrown), `Obj_BrokenRock_*`, `Obj_*_BrokenRock_*` (bomb or hammer) | 1 of 1 |
| Grass | `Obj_Plant_Grass_*`; by full name `Land_kokiri_Plant_A_02_DropFixed_03`, `Land_Kokiri_Plant_G_01`, `Land_Kokiri_Zenmai_A_02`, `Obj_Plant_DestructibleGrass_Base` (the packs with the life of `Obj_Plant_DestructibleGrass_Base`: a pot's life, 1, and no `DestructionRef`, so the first scan missed them) | |
| Signs | `Obj_SignBoard_*`, `Obj_*_Signboard_*`, `Obj_GuidePost_*` | 100 of 100 |
| Walls and floors | `Obj_*_CrackWall_*`, `Obj_*_CrackFloor_*` (bombable) | |
| Others | `Obj_BrokenIcePillar_*`, `Obj_Kokiri_Beehive_*`, `Obj_*_SpiderWeb_*` | Beehive 1 of 1 |

Left out on purpose: the chest `Obj_TBox_A_04` (opening is a flag, [flags.md](flags.md)), shop targets and the moving obstacle (minigames), icicles and hammer pillars (they fall or tilt), `Obj_Well_CandleStick_A_03`. `Land_*_Plants_*` are scenery with no life. The list is `lichii/hook/coop_world.cpp`.

### How

The breakables ride on the enemy sharing (`coop_objects_sync.cpp`, [objects.md](objects.md) "The hook side"): `IsShared` takes them, so each one gets the placed id (its placement hash; every breakable seen live has one), an owner (the first game that reports it present), samples of its life from the owner, and `DAMAGE` from a game that hits its copy.

- **A breaks it on the owner's game.** Life goes to 0, the owner's sample carries 0, every copy has 0 written, and each game runs its own break (pieces, sound, drop).
- **A breaks a copy.** The drop of life on the copy goes to the owner as `DAMAGE`; the owner writes life minus that, its game breaks it, and its sample breaks every other copy.
- **Broken without a life read.** A thrown rock is removed from the actor list the moment it lands, and a crate 0.15 s after its life reaches 0, while the walks fail on the pieces it spawns; neither life 0 is seen by the sync (live, 2026-11-09). So a breakable that leaves the list near our own Link counts as broken here (`Sync::Flush`): within 8 m when it was moved first (lifted, carried, thrown: over 0.5 m from where it was first seen), within 3 m (sword reach) when not. The owner sends life 0, a copy sends its whole life as `DAMAGE` at once. One that leaves further away was unloaded and sends nothing. An object counts as gone only after two walks in a row miss it, and a breakable whose life can no longer be read (its life object freed after death) reads as 0.
- **Gone before it was sent.** A break whose life 0 was read on the last walk before the actor went is still sent: the owner's life 0 as a last sample, a copy's hit as `DAMAGE` at once, not after the 100 ms sum.
- The hook logs each shared object that leaves: `coop: objects: share gone <id> owner N life L moved M from Link D: broken here` (or `left`).

No new message: `OBJECT_PRESENT`, `OBJECT_OWNER`, `OBJECT_SAMPLE` and `DAMAGE` as for enemies.

### Live result (2026-11-09, two games on one PC, `tools/autotest`)

Scenario `rock` (Graveyard, child saves): A walks to the liftable rock with `goto`, lifts it (A) and throws it (ZR). A's log: `share gone 79b54c39a122b910 owner 1 life 1 moved 4.10 from Link 4.44: broken here`. B's log, at once: `share life ...: 1 -> 0`, and B's game removes its rock 0.1 s later. B's screenshots show the rock on the ground beside A's puppet, then gone with its pieces, while B stands 6.5 m away. **Pass** for a thrown rock.

Seen on the way:
- Signposts (`Obj_GuidePost_*`, `Obj_SignBoard_*`) cut by the sword keep their life (100) and stay in the list: the cut is a state of the same actor. Life sharing does not carry them; they need their own state read (not found yet).
- Scenario `pots` (Kakariko, crate south of the Graveyard gate, life 5): each of A's sword hits shows on B at once, `share life 5 -> 4 ... 1 -> 0` in B's log, and B's crate is removed (about 4.6 s after its life 0, against 0.15 s in A, where it breaks and drops a rupee: the death by a written life is slower than by a hit). **Pass** in three runs, through the life path and through the reach rule.
- Pots: the nearest (`Obj_Pot_A_08`, two at -16.6, 30.7 in KakarikoVillage_Child_Static) are inside a house whose door (`Obj_Kakariko_Door_A_09`, west side) the stepper could not reach: walls stop it at x -20.4. Not broken in a run yet; they take the same paths as the crate (life 1).
- The Kakariko side of the gate loads its own actors only after the scene change, so Graveyard objects are shared only while both players are in `Graveyard_Child`.

### Not done yet

- Drops: each game runs its own death, so each drops its own rupee or heart. dauro wants one drop for one player (the one who broke it). Next: hold the copy's drop (the drop component, or remove the dropped `Obj_Rupee_*` that appears where a copy died).
- Signposts (above), and pots and grass cut by the sword: not seen in a run yet. A pot that breaks with its life read at 0 goes through the life path; one removed at once goes through the gone rule, which needs it to have moved: a pot cut where it stands would be missed. Next live check: a pot.
- Lifted and carried objects (a pot or rock in Link's hands) are not moved on the other screen; only the break is shared. Movables are the step after switches.
- Grass, 2026-11-09: dauro cut grass in Hyrule Field (build e2ed2aa) and it stayed on the other screen. That build shared no grass there: the field has no `Obj_Plant_Grass_*` at all (two logs with `LICHII_COOP_DEBUG_OBJECTS=1`), only 13 `Land_kokiri_Plant_A_02_DropFixed_03` near the Kokiri side, now on the list. Not checked in a game: that this grass leaves the list when cut. The rest of the field's grass is not an actor: no actor of it is on the list while it stands (the Foliage module probably draws it). Where its cut state lives is not known, so it is not shared. Next: one run with the debug switch where grass is cut, then read what changed.
- Grass and signs cut by the other player: the owner may be a game where that grass is far away. Works the same way, not checked.

## Switches and doors (next)

Most switch states are 64-bit-key flags of the switch's placement ([flags.md](flags.md)): `LICHII_COOP_DEBUG_FLAGS=1` logs which key changes when a switch is pressed. Plan: share live every `Bool64bitKey` change whose key is the placement hash of an actor loaded in the scene, so the other game's door reads the flag and opens. Where a door reacts only to its own trigger, an event message as well. Not started.

## Testing

`tools/autotest/run.ps1 -Scenario rock`: A walks to the Graveyard's liftable rock with `goto` ([testing.md](testing.md), "Input scripts"), lifts and throws it; B stands 6.5 m away facing it. The `coop: objects: share gone` and `share life` lines in the two logs and B's screenshots show the break on both. `sign` (Kakariko signpost, cut with the sword) and `crate` are there for the cases above that are not shared yet.

## Field grass (not an actor): shared with GRASS_CUT

The grass of the fields is drawn by `game::foliage::Module`; a cut is no actor and no flag. Read from the plain `main` of build e0f3ccfaf50065ae and checked in one autotest run (2026-11-09, `coop-kit\shots\grass\20261109-145729`).

**Where a cut goes.** The module pointer is at main+0x4e22540; when its byte +0x30 has bit 0 clear, `[module+0xf0]+0x338` is the foliage scene (the game's own 0x2c013c0). Every cut is a 0x3c-byte slot the game takes from a free list (slots at scene+0x48, the list head at +0x50) and puts on a queue at scene+0x20: u32 capacity (16 in Hyrule Field), entries of 16 bytes at +0x28 (slot pointer, then sequence: i + 1 while queued, i + capacity once taken out), u64 read index +0x30, u64 write index +0x38 (0xb808f0 pushes). The renderer takes the slots out the same frame and copies each into the `SsboGrassCut` buffer its shader reads.

**The slot.** +0x00 position (3 x f32), +0x0c radius, +0x10 0.3 (the game's), +0x14 a flags byte, then three points of a sword swing at +0x18, +0x24 and +0x30 (3 x f32 each; NaN when the cut is a sphere only). Two places fill it: the sword (0x2ea4880, all of it) and `0xb80800(module, request)`, which copies a whole 0x3c-byte request into a free slot and queues it (used by the AI node `OneshotTryCutFoliage`, 0x29676a0, with position, radius, flags 4 and NaN points, and by two more callers at 0x303e464 and 0x438c9c). It does nothing with a radius under 0.01.

**Seen live.** Each sword swing queued 3 slots (write index 0 -> 3 -> 6), a spin attack 13, at Link's sword. Cutting the actor tufts (below) queued slots at each tuft too.

**The sync** (`CoopWorld::GrassSync`, coop_world.cpp). Each objects walk reads the queue's new entries and sends each slot as `GRASS_CUT` (protocol 4: scene, position, radius, flags, the three points; [protocol.md](protocol.md)). A game in the same scene makes the cut with the game's own call: at the svc cave it writes the request into a 0x40-byte block it mallocs once and calls 0xb80800(module, block), at most 8 a frame. That cut comes back through its own queue; a slot the same as one it made in the last 3 s (position and radius, bit for bit) is not sent again. A cut for another scene is dropped. Logs: `coop: grass: cut at ... sent`, `coop: grass: got cut from P...`, `coop: grass: N cuts of the others made here`.

**Late and returning players.** The server keeps each scene's cuts (at most 512) while anyone is in that scene and sends them to a player who comes in, a late joiner too ([protocol.md](protocol.md), `GRASS_CUT`). The game makes them only once its walk has seen the new scene's foliage queue stay the same for 2 s, so none lands in the scene it left. When the last player leaves a scene its cuts are forgotten: the game grows the grass back when a scene is loaded again, so a player who comes back to an empty scene finds it whole, as in the one-player game. Scenario `grass-late` checks both.

`LICHII_COOP_DEBUG_OBJECTS=1` still logs the research probe (`CoopWorld::FoliageProbe`, `coop: foliage:` lines): the path once (with `(off)` and no walk while the module's byte +0x30 has bit 0 set), then every 8-byte word that changes in the scene's first 0x400 bytes and in what the six words of `SsboGrassCut` point to, two pointers deep (0x200 bytes each), with where our Link stood. A word that changed six times is a counter: it is logged once more as `noisy` and then left out. At most 60 lines per block in each scene, 1,500 per session.



## Broken for everyone, moved for everyone (`WorldSync`, branch `world-sync-3`)

`CoopWorld::WorldSync` (coop_world.cpp) shares placed breakables and movables by their placement hash, beside the enemy sharing above (which still carries a breakable's life). Three messages, protocol 4 ([protocol.md](protocol.md)). **Unit-tested only: nothing in this section has been seen in a game yet.**

**`WORLD_BREAK`.** A game sends it once when a placed breakable breaks there: its life read at 0 (or no longer readable), or it left the actor list next to our Link (the rule above: 8 m when it was moved, 3 m when not). The second kind waits 0.7 s and is dropped when the area unloaded instead: four or more tracked objects went within that time and one of them was more than 8 m away, or our Link was not in the walk. The server keeps each scene's breaks (at most 1,024) while anyone is in the scene, passes each on once, and sends them to a player who enters. A game that gets one:

- has had the object for a while: writes life 0, so the game breaks it itself. If the object still stands, it is deleted with the game's own delete call: grass after 0.4 s whatever its life says (the Kokiri-side tufts kept standing at life 0), anything without a life after 0.4 s, anything else after 6 s and only if its life is back above 0 (a crate takes 4.6 s to go by itself).
- sees the object for the first time after the break (a late joiner, an area that loaded later): deletes it at once, no break effect.
- A game that saw its own copy go and sees it again 3 s or more later (a respawning pot) sends the break with `restored`; everyone forgets the break.

When the last player leaves a scene the server forgets its breaks, as the game restores everything on a reload.

**`WORLD_MOVE` and `WORLD_PLACE`.** The game whose own Link is within 15 m of a placed movable or breakable that left its rest place (5 cm for blocks, 15 cm for pots and rocks) is its mover: it sends `WORLD_MOVE` (place and rotation, at most 20 a second, unreliable) while it moves and `WORLD_PLACE` (reliable) when it has been still 0.4 s. The others put their copy where the mover's was 100 ms ago, every frame, through the same setter the enemy copies use, then hold it at the rest place for 0.5 s and let it be; for 1.5 s after that their own copy settling is not taken as a move. One mover at a time: a game does not start sending while another's moves come in, and when two start together the lower player number keeps it. The server keeps each object's last rest place per scene (at most 256) for a player who enters later. The enemy sharing no longer places these objects (`WorldSync::Places`).

### Table: what breaks or moves, and how it is shared

| Object | Actor names | How it breaks or moves | Shared by | Proven |
|---|---|---|---|---|
| Field grass | none (foliage) | sword, spin, AI cut | `GRASS_CUT`, server list per scene | unit tests; not in a game |
| Grass tufts | `Obj_Plant_Grass_*`, `Land_kokiri_Plant_A_02_DropFixed_03`, 3 more | sword | life + `WORLD_BREAK`, deleted 0.4 s later | unit tests; life 0 alone failed in a game |
| Pots | `Obj_Pot_*`, `*SkullPot_*`, `*ArcheryPot_*` | sword, thrown | life, `WORLD_BREAK`; carried: `WORLD_MOVE` | unit tests; not in a game |
| Crates | `Obj_BrokenBox_*` | sword (life 5) | life, `WORLD_BREAK` | life path seen in a game (3 runs) |
| Liftable rocks | `Obj_LiftRock_*` | lifted, thrown | gone-near-Link, `WORLD_BREAK`; carried: `WORLD_MOVE` | thrown break seen in a game; carry not |
| Bomb and hammer rocks, cracked walls and floors | `*BrokenRock_*`, `*CrackWall_*`, `*CrackFloor_*` | bomb, hammer | life, `WORLD_BREAK` | unit tests only |
| Ice pillars, beehive, spider webs | `Obj_BrokenIcePillar_*`, `*Beehive_*`, `*SpiderWeb_*` | hit, fire | life, `WORLD_BREAK` | unit tests only |
| Red ice | `Obj_RedIce_*` | blue fire (no life: it goes) | gone-near-Link, `WORLD_BREAK`, deleted | unit tests only |
| Signs | `*SignBoard_*`, `*Signboard_*`, `*GuidePost_*` | sword cut | `WORLD_HIT`: the swing is replayed on the others' sign (below) | unit tests only; the replayed hit may not cut (the sign probe, below, shows it in the next run) |
| Push and pull blocks | `*MovableBox_*`, `Obj_SliderBox_*`, `Obj_AmosStatue_*` | pushed, pulled | `WORLD_MOVE`, `WORLD_PLACE` | unit tests only |
| Hammer pillars (Fire Temple and Gerudo Training Ground) | `Obj_FireTemple_HammerSlipPillar_*` (the same actors in both) | hammer: falls and stays as a bridge | life, `WORLD_BREAK` as a faller (`IsFaller`): life 0 written in the others, again each second while it stands, at most 10 times (then one `did not fall on life 0` line); never moved, never deleted (the key makes it fallen on a reload) | unit tests only |
| Hammer floor switches (Fire Temple) | `Obj_HammerSwitch_*` (5, no key) | hammer: pressed once | a faller as above: life 0 (its SwitchHit life) written in the others presses it; the room's flag does the rest | unit tests only |
| Path pieces that move when hit | `Obj_FireTemple_HammerDropFloor_*`, `_HammerDropPillar_*`, `_HammerStair_*` (7, no key), `Obj_InsideTheDekuTree_FallingLadder_A_01` (the slingshot ladder), the Deku Tree's falling floors `Obj_InsideTheDekuTree_FallingFloor_A_01`/`_A_02`/`_A_03` (the slingshot room's bridge; `_A_01`/`_A_02` are keyed: the key's removal waits 4 s so the fall shows, [dungeon.md](dungeon.md)) and `Obj_ShadowTemple_FallingFloor_A_03` | hammer, slingshot or a step: they sink, fall or tip over and stay (no life) | a *shifter* (`IsShifter`): moved or turned more than 0.05 rad from its rest in our game, within 40 m of our Link (it moves only by its own AI hit in that game, so the shooter across the room sends it; other movers 15 m), it is sent as `WORLD_MOVE` then `WORLD_PLACE` (never as carried); the others put it there and, for the ones with an AI (hammer pieces, the ladder), hold it there for good (the AI would put it back up; the keyed Deku floors `_A_01`/`_A_02` are held too, until their key removes them); the two `FallingFloor_A_03` have no AI and settle like a block. A game that loads the room later gets the server's place | unit tests only |
| Small keys lying | `Obj_Dungeon_SmallKey*` (10: Bottom of the Well, Shadow Temple, the 4 Gerudo Fortress keys twice; not `Obj_MiniGame_SmallKey`) | taken | no 64-bit key exists, so: gone next to our Link is a `WORLD_BREAK`; the others delete theirs at once (no 3 m rule), a game that loads the room later deletes it from the server's break. Both taking it at once: the server keeps the first break only and relays it, so the game that gets the other's break for a key it took itself (sent, still waiting out the 0.7 s hold, or still on the walk but lifted 0.3 m from where it lay next to its Link: mid-pickup) gives one key back (`DungeonInfo.<dungeon>.SmallKeyNum` - 1, which the flag watcher sends as -1): the shared count rises once | unit tests only |
| Gold Skulltula tokens | `Obj_Token_Gold` (made where its Skulltula dies, no placement) | taken | as a lying small key, under `TokenId(Skulltula hash)`: the token that appears within 3 m of a Gold Skulltula we saw, or within 1.5 m of where a token of this scene was before (a room loaded again: the Skulltula is gone, the game makes the token again; the spots are kept for the session), or, failing both, that appears within 5 s of a Gold Skulltula leaving our walk dead (last seen with life 0) and within 12 m of where we saw it last, if that Skulltula's token is not known yet (the one that left last: a Skulltula knocked away as it died, or one whose token fell; items-sync). A Skulltula that left alive (its room unloaded) never claims a far token, and a second token near a known one (a Skulltula with no placement) gets no id and is not shared. Taken here: `WORLD_BREAK`; taken elsewhere: ours deleted at once, and its Skulltula's 64-bit key set to 1 here (its taken state, so a reload brings neither back); both at once: one token back (`PouchData.Item_Token_Gold.StockNum` - 1). Issue 89: before, the token stayed in B, also after B loaded the room again, and B could take it too | unit tests only |
| Push and pull blocks | `*MovableBox_*`, `Obj_SliderBox_*`, `Obj_AmosStatue_*` | pushed, pulled | `WORLD_MOVE`, `WORLD_PLACE`; the block's push node, rest place and base anchor rewritten where it is followed (issue 147, below) | moves seen by players; the rewrite unit tests only |
| Rolling boulders (issue 84) | `Obj_*RollingRock_*` with a placement (22: the Kokiri Sword crawlspace's `Obj_Kokiri_RollingRock_A_01`, Fire Temple 10, Gerudo Training Ground 4, Spirit Temple 3, Water Temple 2, Ganon's Castle 2; `IsRoller`) | roll by themselves | a movable: the game whose Link is within 15 m when it rolls sends its rolls as `WORLD_MOVE` (never carried; it never rests, so no `WORLD_PLACE` while it rolls), the lower player number if two; the others put their copy on those rolls. When that game stops sending (left the room), another near game takes over after 1.5 s. Before, each game rolled its own and they drifted apart (Lixyas). Not the spawned Death Mountain Trail boulders (no placement hash) | unit tests only |
| Bomb flowers | `Obj_BombFlower_*` | picked, thrown, explode | not shared here (the fruit is made by the game, no placement) | - |
| Small-key door locks | `Obj_DoorChainLock_*`, `Obj_SpiritTemple_DoorChainLock_*` (not the Market shops') | opened with a key (the game deletes the lock) | gone-near-Link, `WORLD_BREAK` with `kept` (kept by the server for the session), deleted; never while our Link is within 2.5 m; a lock opened twice gives one key back ([dungeon.md](dungeon.md), "Locked doors") | unit tests only |
| The Deku Tree basement's moving floors (issue 196) | `Obj_Deku_MoveFloor_A_01`, `_A_02` (`IsGimmickMover`) | moved by the room (water drop), bob on the water | a *gimmick mover*: moved more than 0.5 m from where it started in our game, within 40 m of our Link, sent as `WORLD_MOVE` then `WORLD_PLACE` (never carried); the others put it there and hold it while that place is away from its start; a place back at its start lets go. Bobbing near the start is nobody's move | unit tests only |
| Other moving floors, lifts, shutters, water level | `*MoveFloor_*`, `*ElevatorLift_*` (on/off animations: the actor place does not move), `*SwitchShutter_*`, `*WaterPlane_*` | driven by switches | the switch flags ([flags.md](flags.md)) and the live events ([dungeon.md](dungeon.md)); not positions | - |

### Blocks: the remembered rest place (issue 147)

A player test (Deku Tree basement): either player could push the block and it moved in both games, and both collided with it, but a player who then pushed it was snapped back to where that game's block had rested before. The copies are moved with the actor's own force-set-matrix (place, turn, physics body; above), which does not touch the block's own idea of where it rests. What the pack shows (`Obj_MovableBox_A_01`, read 2026-11-10): `MovableBoxParam` pushes by `PushPerBlock`, a `DgnObj_MovableBox` constraint controller holds the body, the blackboard has `MovableBox_MoveState`, and there is an anchor actor `Obj_MovableBox_BasePosAnchor` (no model, a dummy body) that no map places, so the block makes it at its base. Each basement block also links to an `Obj_Gimmick_AnswerPosAnchor` (the puzzle's goal). Block places are not in GameData (no flag holds either basement block's hash).

What is built (no game run): for the push blocks (`IsPushBlock`: the MovableBox family and the slider box, not the Armos statue), in a game that follows another's push, `WorldSync::Move` keeps where its copy rested before (`follow_from`). When the follow ends (the rest place held 0.5 s), it hands the glue a `BlockFix`:

- the base anchor found within 0.5 m of the old rest place is put at the new one through the same setter for 1 s (`Poses`; place only);
- `RestWords` looks through the block's memory (the actor +0..+0x600 but its own place, the first 0x200 bytes of each component its table points to, the physics root `[[table+0x50]+0x20]` +0..+0x400) for the old rest place as three floats in a row or a 3x4 matrix's translation, each axis within 2 cm, and the glue writes the new place there **only with `LICHII_COOP_BLOCK_REST=1`** (the integrator's test). The sizes searched are not known object sizes, so by default (players) it only logs where it would write; once a run shows which word is the rest place, writes go to that offset alone and the switch goes away. Nothing is found for a move under 5 cm, a place near the origin, a match with an axis exactly 0.0, or more than 12 places (a wrong match). The anchor move is always on.

A late joiner gets the same: its block is first seen where the map made it, the server's `WORLD_PLACE` puts it at the rest place, and the fix runs from the made place. The pusher's own game needs nothing. One mover at a time as before (the lower player number keeps it), so no tug-of-war.

Logs: `world: <hash> <name> came to rest where another player left it; its base anchor is put there too` (or `was not found near its old place`), then `world: <hash> <name> old rest x y z -> x y z: N places rewritten: ` (or `N places found, not written (LICHII_COOP_BLOCK_REST off)`) `actor+0x.. c+0x..+0x.. (matrix) physics root+0x..`, or `... found N times, not written (too many)`. The places listed are where the game keeps the rest place: the run tells which of the two (anchor or memory) mattered.

### Blocks: the push node (issue 147, world-objects-2)

Why the rest rewrite above may not be enough: the place a push starts from is kept in the block's AI node, not in the actor, its components or the physics root that `RestWords` looks through. Read from main (build e0f3ccfaf50065ae; disassembled, nothing run):

- The node is the AINB node `ExecuteMovableBoxControl` (name getter 0x275ac90). Its constructor 0xe5a870 and destructor 0x275ac10 store the vtables: `[node] = main + 0x49ae6f8`, `[node + 0x20] = main + 0x49ae8b8`.
- Its actor is `[[[node + 8] + 8] + 0x1f0]`: its start 0x2751790 reads the actor's place +0x31c, links +0x3d8, component table +0x270 through that chain.
- Fields (0x2751790): +0x40 the grid base, from the link named `MovableBox_BasePosAnchor` (else the actor's place); +0x4c the goal, from the link `Gimmick_AnswerPosAnchor`; both NaN when unset. +0xfc the place the push goes to and +0x108 the place it starts from, copies of the actor's place when the node starts. The ports are PushPerBlock +0x1d0, MoveSpeed +0x1d8, PushPullMovability +0x1e0, IfCliffNearThenPushRate +0x1e8, IsOnAnswerPos +0x1f0 (binder 0xf247f0). The next step's target comes from 0xd6ef80(node, base, place, PushPerBlock), on the grid of the block's size.
- So in a follower's game the setter moves the block, but +0xfc and +0x108 still hold the old rest place: its player's next push starts there, and the block (and the pusher holding it) snap back.

What is built: when a `BlockFix` comes (the same moment as above), `FindBlockNode` follows heap pointers from the block's components (breadth first, 3 steps, at most 2048 objects, the first 0x200 bytes of each) and takes an object only when both vtables match and its owner chain ends at this actor. `NodeRestWords` then writes the new rest place into +0xfc and +0x108 when both hold the old one (each axis within 2 cm). If only one does, the node is mid-push in this game (its target is the next cell): nothing is written (`push node ... busy (mid-push here): not written`), and the wide search is skipped for that block too. The base and the goal are left alone (the grid and the answer do not move).

Open question (no run yet): whether the node's start 0x2751790 runs once when the block's AI starts or again at every push. The block's AI tree (`Work/AI/Root/DgnObj_MovableBox.root.ain`) is not in the romfs folders read here. If it runs at every push, +0xfc and +0x108 are fresh at each push and this write changes nothing (harmless); `deku-block` tells: a snap back with the node line logged means the stale place is elsewhere. These are known fields of an object found by its type, so this is **on by default**; `LICHII_COOP_BLOCK_NODE=0` turns it off. `LICHII_COOP_BLOCK_REST` (the wide search) is unchanged.

Logs: `world: <hash> <name> push node <address>: N of its places moved to the new rest: node+0xfc node+0x108`, or `push node not found: its next push may start from the old place`. Check: autotest `deku-block` (the "b-push1" shot: no snap back).

### Signs (issue 90): what the pack says

Read from `Obj_GuidePost_A_02.pack` (romfs, 2026-11-09): a cuttable sign's life is `Signboard`, a child of `Obj_Invincible` with `IsDamagedWhileInvincibile`, so a hit never lowers its life; the cut is done by its `DestructionParam` (a `_fracture` shape whose pieces come off where the sword hit) and its AI is `CuttableSignboard`. It keeps no blackboard value for "cut". So a cut cannot be read from life or a flag.

**What is built (`WORLD_HIT`, type 35; not seen in a game).** The swing is shared, not the cut. Every sword swing is already read for the field grass: the game queues its foliage cut slots with three points of the blade. When a point of our own swing is within 1.5 m (level) and 2.5 m (height) of a sign, the hook sends `WORLD_HIT` with the sign's placement hash, the nearest point and where our Link stood (at most one per sign every 0.3 s). Every other game in the scene gives its own sign one hit at that point, from that place, through the hit replay the enemy sharing has (`Mover::AddHit`: the game's own damage call), so that game's destruction takes the pieces off itself. The server keeps the last 4 hits of each sign per scene for a player who enters later; they are given once the sign has been loaded 0.3 s.

**Read from the game (2026-11-09, no run).**

- The sign's AI (`AI/CuttableSignboard.root.ainb` in its pack) is one node, `ExecuteCuttableSignboardControl`, with the inputs `IsHitEffectiveAttack`, `IsCutExtension`, `KnockBackPower`, `RepairLinearVelocity` and `RepairAngularVelocity`. `IsHitEffectiveAttack` is the blackboard value `Gimmick_IsHitEffectiveAttack` (table `Gimmick_ReactionCoordination`), which the damage side sets: the sign's `DamageParameters/Signboard` has its own `DamageCalculatorExtensionType`, a rate of 1 for most damage types and 0 for three, and its reaction is `HitSetting.IsExternalEmitReaction`. So the cut starts in the Life component's hit handling, the same place the hit replay writes a whole hit record (Life +0x1f8..+0x2f0, the Kokiri Sword's header), and the node then breaks the pieces. That is the entry a local sword hit takes; the replay goes through it. The node's code was not found in `main` (its name is not referenced by address), so which fields of the hit it reads for the cut line is not known.
- The replay used to put the hit 1 m above the place it was given (right for an enemy's foot). A sign's hit now goes in at the blade's own point (`Mover::Hit::exact`).

**The pose setter and physics objects (read from `main`, no run).** The Mover's call, the actor's vtable slot 0x218 = main+0x4c6580 `(actor, 0)`, is the engine's warp for the whole physics component, not a plain field write: it sets bit 23 of actor+0x4a8 and calls main+0x4c6630 `(physics, &place, &matrix, warp = 1)` with the physics component `[[actor+0x270]+0x50]`. That stores the new transform at root+0x1d0 (root = `[physics+0x20]`) with the warp bits 3 at root+0x228 and marks the main rigid body (`[root+0x1c8]`, byte +0x1f2 bit 7), passes it to the controller set when there is one (`[root+0x10]`, its vtable slot 0xc8), to the character body when there is one (`[root+0x190]`: main+0x162250 place, main+0x6b8a40 rotation) and then to every body (main+0x4c6870). Pots, crates, rocks, grass and signs have the same actor vtable (main+0x4b25e18) as enemies in the logs of 2026-11-09, so they take this path. What a run must still show: that a block's own AI (`DgnObj_MovableBox`, blackboard `MovableBox_MoveState`) leaves it where it was put once the 0.5 s hold ends.

Open, to see in a run: (1) the swing is only read where the foliage module is on (outdoor scenes with grass; a sign in a scene without it is not shared); (2) whether the replayed hit makes the destruction cut, and the same pieces; if not, the other way is to find the Destruction component's state with a memory compare while a sign is cut and write it; (3) a swing near a sign that did not cut it in the sender's game is sent too, and the receiving game decides by its own geometry.

### Signs: the cut words (issue 90, `SIGN_CUT`)

The chunk 3 run (2026-11-10, `coop-kit/shots/sign/20261110-093807`) answered the probe's question: the replayed hit does not cut. B's sign changed 7 words, 6 of them A's too (the hit reaction: `c+0x70+0x138` 7>5 and the 0x100000>0x180000 flags at `c+0xc0`, `c+0xf0`, `c+0x130`, `c+0x160`, `c+0x198`), plus the replay's own hit record (`c+0x2b8+0x1f8`). The words of the cut itself changed only in A: the contact counters of two components (`c+0x130+0xb0` and `c+0x160+0x60` 0>1, their 0x400 bit at +0xc0 and +0x70 cleared), the cut point and a flag bit 0x80000 in `c+0x268` (+0x98..+0xa8 and +0x1e8; the same bit at `c+0x70+0x18`) and a float that was NaN (`c+0x58+0x120`, `c+0x60+0xc8`). So the cut comes from the blade's contact with the sign, which a Life hit record does not make.

So the cut is now written: 0.5 s after our own cut, the game that cut sends the words its probe saw change as `SIGN_CUT` (type 80): component words only, not the Life hit record, no counter the probe saw move before the hit, no heap pointer (0x10_0000_0000 to 0x100_0000_0000), no word that was and stayed two real floats (the pieces' transforms). Every other game writes them into its own sign (the same component table entry and offset) 0.2 s after it gave the hit, or 0.3 s after the sign was loaded for a player who enters later (the server keeps the latest per sign and scene). Logs: `world: <hash> <name> cut here: N words sent`, and in the others `cut by another player: N words written`; their probe lines then list those words too.

Not known until a run: whether the destruction reads that state and breaks the pieces, or needs the contact itself (then the next step is to find the contact call).

Run of 2026-11-10 (`coop-kit/shots/sign/20261110-175836`, read again 2026-11-11): B wrote 9 of the 11 words; `c+0x58+0x120` and `c+0x60+0xc8` (the NaN floats) were refused with no reason given, and B's camera faced the gate wall, so B's post was never on screen. The refusal line now names the failed check (`sign word <where> not written: component moved (...)`, `holds <value> here ...`, ...). The `sign` scenario now warps both into Kakariko at `Kakariko_GraveYardSide`, 6 m from the post, and puts B there with `place` (no walk), and A steps aside after the cut; `sign-late` has B in the Graveyard during the cut and walking into Kakariko after it (the late joiner's words).

### Signs: the probe (issue 90)

Point (2) above is answered by the next run, without a memory tool: the hook compares the sign's memory before and after a hit (`CoopWorld::SignProbe` in `lichii/hook/coop_world.{h,cpp}`, tested in `tests/lichii/world_test.cpp`; always on, it reads only signs within 12 m of our Link, the nearest 3, once a second).

- What it keeps: the actor (+0..+0x600), its component table (+0x270 -> +0..+0x300) and the first 0x200 bytes of every component the table points to, copied again every second while nothing hit the sign. A word that changes between two such copies is a counter or a timer and is left out of the lines.
- When it logs: our swing passed the sign (`our cut`: compared with a copy at least 0.25 s older than the moment the swing was read, since the grass queue is read a little after the swing), or another player's hit was given to it (`given hit`: the copy from before the hit was handed to the Mover). 0.5 s and 2 s after, one line each: `coop: sign probe: <hash> <name> our cut +0.5 s: N words changed: <where>:<before>><after> ...` (at most 24 shown). A sign that leaves the walk before its lines are done (the cut deleted or replaced the actor) logs `... our cut: gone after the hit (+T s)` instead. `<where>` is `actor+0x..`, `table+0x..` or `c+<table entry>+0x..` (a word of the component that table entry points to; `c+0x2b8+..` is the Life component).
- How to read it: the game that cut the sign shows what a cut changes. If the game that got the hit shows the same words changing the same way, the replay cut its sign (the same pieces, as far as these words go). If it shows nothing (or only a few hit-record words in `c+0x2b8`), the replay did not cut, and the words from the cutting game are the state to write instead, in a next step.
- Limits: a sign hit before it was within 12 m for a second has no copy from before (`no copy from before the hit`); a word the cut changes deeper than 0x200 bytes into a component, or behind a second pointer, is not seen.

### To check in a game (nothing here was run)

1. Pot: A breaks a pot with the sword, B sees it break (`coop: world: ... broken here (life 0): sent` in A, `got break` and `broken by another player` in B). B enters the scene later: the pot is not there (`was broken before we saw it: deleted`).
2. Tufts by the Kokiri side of Hyrule Field: A cuts, B's tuft goes (`still stands after the break: deleted`).
3. A carries a pot or rock: B sees it in the air at A's puppet (`moved here: sending` in A, `moved by another player` in B); does the setter move a physics object, and does it drop when let be?
4. A pushes a block (Forest Temple, Kakariko well): B's block follows and stays (`at rest: place sent`, `put where another player left it`). Does B's block keep its place after the 0.5 s hold, and can B push it afterwards?
5. A door or room change next to pots: no `broken here (gone next to Link)` line may appear (it should say `left with the area`).
6. Sign: A cuts the signpost by the Graveyard gate (scenario `sign`): A logs `our sword passed it: hit sent`, B `hit by another player's sword`; does B's sign lose the same pieces? And for B entering later? Read the `coop: sign probe:` lines of both games (above): the same words changed in B as in A means the replay cut it.
