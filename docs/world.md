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
| Grass | `Obj_Plant_Grass_*` | |
| Signs | `Obj_SignBoard_*`, `Obj_*_Signboard_*`, `Obj_GuidePost_*` | 100 of 100 |
| Walls and floors | `Obj_*_CrackWall_*`, `Obj_*_CrackFloor_*` (bombable) | |
| Others | `Obj_BrokenIcePillar_*`, `Obj_Kokiri_Beehive_*`, `Obj_*_SpiderWeb_*` | Beehive 1 of 1 |

Left out on purpose: the chest `Obj_TBox_A_04` (opening is a flag, [flags.md](flags.md)), shop targets and the moving obstacle (minigames), icicles and hammer pillars (they fall or tilt), `Obj_Well_CandleStick_A_03`. `Land_*_Plants_*` are scenery with no life. The list is `lichii/hook/coop_world.cpp`.

### How

The breakables ride on the enemy sharing (`coop_objects_sync.cpp`, [objects.md](objects.md) "The hook side"): `IsShared` takes them, so each one gets the placed id (its placement hash; every breakable seen live has one), an owner (the first game that reports it present), samples of its life from the owner, and `DAMAGE` from a game that hits its copy.

- **A breaks it on the owner's game.** Life goes to 0, the owner's sample carries 0, every copy has 0 written, and each game runs its own break (pieces, sound, drop).
- **A breaks a copy.** The drop of life on the copy goes to the owner as `DAMAGE`; the owner writes life minus that, its game breaks it, and its sample breaks every other copy.
- **Gone before it was sent.** A pot is removed from the actor list soon after it breaks. A break read on the last walk before that is still sent (`Sync::Flush`): the owner's life 0 as a last sample, a copy's hit as `DAMAGE` at once, not after the 100 ms sum. An object that leaves with life left (unloaded by distance or a scene change) sends nothing.

No new message: `OBJECT_PRESENT`, `OBJECT_OWNER`, `OBJECT_SAMPLE` and `DAMAGE` as for enemies.

### Not done yet

- Drops: each game runs its own death, so each drops its own rupee or heart. Caleb wants one drop for one player (the one who broke it). Next: hold the copy's drop (the drop component, or remove the dropped `Obj_Rupee_*` that appears where a copy died).
- A breakable that is removed in the same walk it breaks in (life never read at 0) is missed. Seen live: not yet.
- Lifted and carried objects (a pot or rock in Link's hands) are not moved on the other screen; the throw breaks it there only through life. Movables are the step after switches.
- Grass and signs cut by the other player: the owner may be a game where that grass is far away. Works the same way, not checked.

## Switches and doors (next)

Most switch states are 64-bit-key flags of the switch's placement ([flags.md](flags.md)): `LICHII_COOP_DEBUG_FLAGS=1` logs which key changes when a switch is pressed. Plan: share live every `Bool64bitKey` change whose key is the placement hash of an actor loaded in the scene, so the other game's door reads the flag and opens. Where a door reacts only to its own trigger, an event message as well. Not started.

## Testing

`tools/autotest/run.ps1 -Scenario sign`: both games walk from the Graveyard save into Kakariko; A walks to the signpost by the Graveyard path with `goto` ([testing.md](testing.md), "Input scripts") and cuts it; B faces the post. Screenshots of both and the logs (`coop: objects: share` and `life` lines) show whether B's post was cut too.
