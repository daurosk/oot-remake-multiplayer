# Things a player makes, in every game

Plan stage 5 ([plan.md](plan.md), [objects.md](objects.md)): a bomb, a slingshot seed, a Deku nut, an arrow, the boomerang or the hookshot tip that one player makes appears in the other games too, moves the same and goes (or explodes) at the same moment. Code: `lichii/hook/coop_spawn.{h,cpp}`, tests in `tests/lichii/spawn_test.cpp`.

Turn it on with `LICHII_COOP_SPAWN=1`. That also patches the svc cave; the spawn director is its caller 10 (`CoopSvc::AddCaller`), after the puppets.

## Shared actors

| Actor | When gone, the copy | Seen live |
|---|---|---|
| `Item_Bomb`, `Item_Bombchu` | explodes (life set to 0) | not yet |
| `Shootable_DekuSeed`, `Item_DekuNut` | is deleted | names seen ([objects.md](objects.md)) |
| `NormalArrow`, `Weapon_Boomerang`, `Shootable_Hook`, `Shootable_HookLong` | is deleted | names from the data files |

## How

**The maker** (present thread, every frame): walks the game's actor list (as `coop_objects.cpp` does) and keeps the shared names.

- Actors already on the list at the first walk in a scene are nobody's and are never sent.
- A new one that is not a copy is ours: `OBJECT_SPAWN` once (id kind made, maker = our player number, value = our counter, which starts at the server time so it stays unique after a reconnect), with time, position and velocity 0, and the projectile flag.
- While it lives: `OBJECT_SAMPLE` (time, position, velocity from the last two walks) at most 30 a second while it moves, every 0.5 s while it is still.
- Missing from two walks in a row: `OBJECT_REMOVE` at its last place, reason exploded for bombs and bombchus, gone for the rest.

**The others** (no new message or field): the server relays these to the maker's scene ([protocol.md](protocol.md)).

- A spawn becomes a copy at the sent place. Samples move it; between samples it is carried ahead along the velocity for at most 150 ms.
- The copy is made through the puppet cave: the director (`CoopSpawn::Director`) runs at the same `svc #0xf4`, after the puppets, with one malloc'd block of 16 create slots, and uses the puppet's create request (`requestCreateActorAsync`, then take at state 3). Each copy is created once only.
- Every game frame the copy's position (+0x31c) is written from the latest sample.
- A remove with reason exploded sets the copy bomb's life to 0 (`[[[actor + 0x270] + 0x2b8] + 0x1c10] + 8`), which runs the game's own explosion where the owner's bomb went off. Any other remove deletes the copy (`0xa6490(actor, 0)`).
- A copy shows up on our own actor list too. It is never sent back: the director tells the sync each copy's actor, and a new actor of a copy's name within 1.5 units of a copy not yet placed is taken as that copy.
- A new scene, a lost connection or a new game forgets everything. A copy with no sample or remove for 20 s is dropped.

## Not done

- Compiled into the emulator and run with two games (2026-11-09, `tools/autotest` scenarios `spawn-seed` and `spawn-bomb`, `LICHII_COOP_SPAWN=1`, Graveyard save of the auto kits). In the logs, two slingshot seeds and a Deku Nut from A reach B. B creates each copy 50 to 70 ms after it arrives and deletes it when A's remove comes (seeds about 0.5 s later, the nut 0.3 s). The copies were not seen in screenshots: they are small, live under half a second, and the save's cow stands in front of both Links. The server must be a build with object owners (`server: object owners`, 2026-11-08); the older kit servers drop every object message.
- No bomb yet: the auto kits' save has no bombs, and `LICHII_COOP_OBJECTS_CHEAT=1` did not give any (no `objects: cheat` line in the log). R (quick select) opens a carousel in this version (stick, slingshot, Deku Nut, bottle, map); the left stick picks, letting go of R selects.
- A nut copy is deleted at the owner's remove, so it does not flash in the other game. A nut could be treated like a bomb (life to 0) if that makes the game flash it; not tried.
- Rotation is not sent (copies are made facing the identity rotation): arrows point the wrong way.
- Copies are the game's real actors, so a copy bomb also hurts things in its own game, and its own fuse runs: it may go off slightly before the owner's remove arrives. The harmless `CoopShot_*` packs of [objects.md](objects.md) would avoid both.
- A copy seed or nut made by the create request may not behave like a shot one (the game may give it its nocked state); its place is still written every frame.
- The live test: two games, A throws a bomb and shoots a seed; screenshots of both windows show the copy and the explosion at the same time.
