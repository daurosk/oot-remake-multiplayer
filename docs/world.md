# Shared world objects

Caleb (2026-11-09): every world object the same for all players. A pot or crate broken by one player breaks for all; an object moved by one moves for all; a switch pressed or a dungeon event started by one happens for all. This page says how, in the order it is built: breakables first, then switches and doors, then movable blocks, then the Water Temple's water level. Enemies are [objects.md](objects.md); objects players make (bombs, arrows) are the spawn-sync work.

Game build e0f3ccfaf50065ae (1.1.1). **Checked** means read from the romfs or the code; **live** means seen in a run.

## Breakables (first cut, branch `world-sync`)

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

- Drops: each game runs its own death, so each drops its own rupee or heart. Caleb wants one drop for one player (the one who broke it). Next: hold the copy's drop (the drop component, or remove the dropped `Obj_Rupee_*` that appears where a copy died).
- Signposts (above), and pots and grass cut by the sword: not seen in a run yet. A pot that breaks with its life read at 0 goes through the life path; one removed at once goes through the gone rule, which needs it to have moved: a pot cut where it stands would be missed. Next live check: a pot.
- Lifted and carried objects (a pot or rock in Link's hands) are not moved on the other screen; only the break is shared. Movables are the step after switches.
- Grass, 2026-11-09: Caleb cut grass in Hyrule Field (build e2ed2aa) and it stayed on the other screen. That build shared no grass there: the field has no `Obj_Plant_Grass_*` at all (two logs with `LICHII_COOP_DEBUG_OBJECTS=1`), only 13 `Land_kokiri_Plant_A_02_DropFixed_03` near the Kokiri side, now on the list. Not checked in a game: that this grass leaves the list when cut. The rest of the field's grass is not an actor: no actor of it is on the list while it stands (the Foliage module probably draws it). Where its cut state lives is not known, so it is not shared. Next: one run with the debug switch where grass is cut, then read what changed.
- Grass and signs cut by the other player: the owner may be a game where that grass is far away. Works the same way, not checked.

## Switches and doors (next)

Most switch states are 64-bit-key flags of the switch's placement ([flags.md](flags.md)): `LICHII_COOP_DEBUG_FLAGS=1` logs which key changes when a switch is pressed. Plan: share live every `Bool64bitKey` change whose key is the placement hash of an actor loaded in the scene, so the other game's door reads the flag and opens. Where a door reacts only to its own trigger, an event message as well. Not started.

## Testing

`tools/autotest/run.ps1 -Scenario rock`: A walks to the Graveyard's liftable rock with `goto` ([testing.md](testing.md), "Input scripts"), lifts and throws it; B stands 6.5 m away facing it. The `coop: objects: share gone` and `share life` lines in the two logs and B's screenshots show the break on both. `sign` (Kakariko signpost, cut with the sword) and `crate` are there for the cases above that are not shared yet.

## Field grass (not an actor): research, not shared yet

Read from the plain `main` of build e0f3ccfaf50065ae with `nso_index.py` (integration's tool); the game was not run.

- The grass of the fields is drawn by `game::foliage::Module`. Its turf shader binds a buffer named `SsboGrassCut` (the code at main+0x2c02600), next to `SsboUnitPlacement`, `SsboGather` and `SsboVegeBehaviour`. So a cut is an entry in a buffer the shader reads, not an actor.
- Path, checked in the code: the module pointer is at main+0x4e22540; when its byte +0x30 has bit 0 clear, `[module+0xf0]+0x338` is the foliage scene (the game's own 0x2c013c0). `SsboGrassCut` is the 0x30 bytes at scene+0x58, `SsboGather` at scene+0x88.
- Other names in the binary: `OneshotTryCutFoliage` (an AI one-shot, name used at 0x296776c), `PlayerCutGrass`, `IsCutGrass`, `FoliageCutRadius` (0xa03484, 0x30fee8c), `EnableGrassCut`, `GrassCutBoost`, `GrassCutEffectEmitRate` (foliage parameters), `game__drop__FoliageDropParam`.
- Not known: the layout inside the buffer (one entry per cut, a grid, a ring), what writes it, and whether writing an entry from outside is enough for the shader to hide the grass (the buffer may be a copy of a list kept elsewhere).

`LICHII_COOP_DEBUG_OBJECTS=1` logs it (`CoopWorld::FoliageProbe`, `coop: foliage:` lines): the path once, then every 8-byte word that changes in scene+0x40..0xc0 and in what the six words of `SsboGrassCut` point to, two pointers deep (0x200 bytes each), with where our Link stood. At most 60 lines per block, 1,500 per session.

Next: one run where grass is cut (steps in the coordinator's notes), then read which words changed and how they relate to where Link stood. From that: the entry layout, then a GRASS_CUT message (protocol 4 or 5) that carries a cut's position and size and is written into the other games' buffers, or replayed through the game's own cut call if the buffer turns out to be a copy.

