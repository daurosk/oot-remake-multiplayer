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

## Seeds, arrows and hookshot tips: stand-ins (issue 85)

Players saw the other player aim the slingshot, but never the seed. The player logs (2026-11-10) show why it was not the network: every seed was sent (`spawn: sent Shootable_DekuSeed`), and the other game made each copy and deleted it at the remove (`create copy`, `copy N is actor`, `copy N deleted`). The copy was there and was not drawn.

- **Why** (read from the romfs and `main`, no run): the copy was the game's own `Shootable_DekuSeed`, made by the create request with no shooter.
  - A shootable made that way starts in the state its `ShootableParam` names as `NoShooterCreateStartState`. That is `UsedExternally` (hash 2207839275; the states are `Sleep`, `Prepare`, `Shoot`, `Stationary`, `Reflected`, `UsedExternally`).
  - The seed's `ShootableParam` also has `IsPrepareVisibleOff: true`, and nothing ever shoots the copy.
  - Arrows (`NormalArrow`) and hookshot tips (`Shootable_Hook`, `Shootable_HookLong`) are shootables made the same way, so they are given the same fix.
- **Fix:** the copy is a stand-in of ours: `CoopShot_Seed`, `CoopShot_Arrow`, `CoopShot_Hook`, `CoopShot_HookLong`.
  - The puppet pack tool builds them with the puppets (`tools/puppet_pack/build.py`, `STAND_INS`): the shootable's model and LOD and the shared default components, with no physics, sensor, attack, AI, effect or sound. So the copy is drawn and can hit nothing.
  - The director (`CoopSpawn::StandIn`) creates the stand-in and places it as the puppet is placed: the place and turn at +0x31c, and the pending transform at +0x358/+0x364. There is no setter call, since the stand-in has no physics.
- **Fallback:**
  - The hook asks only for stand-ins the installed pack lists. It reads every `coop_puppet.json` under `load/0400486024DEC000/` once, where build.py lists them under `shots`, and logs `coop: spawn: stand-ins in the puppet pack: ...`. A create for a name ActorInfo does not know has never been run, so it is never made.
  - A pack built before this change lists none, so its copies are the game's own actors, as before. The kit's pack hint already asks for a rebuild when the tool changes.
  - A stand-in that never becomes an actor (create failed, timed out or dropped, or the take gave none) is made again. After 3 failures in a row the director logs `<actor>: CoopShot_... could not be made 3 times in a row; the game's own actor is used` and uses the game's own actor for the rest of the session.
  - The stand-ins' ActorInfo rows take the puppet's overrides (CalcRadius 100000, CoreUsedLevel 0), so a far shot is still calculated.
- **Lures (issue 151, fishing-2):** a cast lure (`Weapon_Lure`, `Weapon_SinkingLure`) is shared the same way, as `CoopShot_Lure` and `CoopShot_SinkingLure` (their model `Item_Lure` / `Weapon_SinkingLure` and LOD). A lure copy is only ever the stand-in (`CoopSpawn::StandInOnly`): the game's own lure in another game would be a lure that game's fish go for. With no stand-in in the pack, or after 3 failed tries, no lure copy is made. Our own lures' places also go to the objects sync for the fish rules ([objects.md](objects.md), "Fish and lures").
- Bombs, bombchus, nuts and the boomerang are unchanged: they have their own AI and are drawn as copies (the nut's flash was seen by players).

## Does the seed's place move in flight? (issue 158)

Players never saw a seed fly. The test logs (spawn-seed 2026-11-10 18:01, slingshot 2026-11-09) show the seed's life on A: made when the slingshot is drawn, sitting in the sling while A aims (its copy in B's game is the seed B sees in the puppet's sling); shot when ZR is let go; gone 0.1 to 0.35 s later; and a new one made in the sling at once for the next shot. Every shot seed was removed as `gone`, not `hit`: at its last two walks it moved under 3 m/s. So either the game keeps a flying shootable's +0x31c at the sling (and the copy never moves), or the seed stops before it goes. Which one is not known.

Each of our projectiles that leaves the list now logs how it moved: `coop: shots: our Shootable_DekuSeed #... gone (reason N) after T ms: went D m from where it was made, top speed S m/s` (`Sync::Flights`). A shot seed with D under a metre means its place does not move in flight, and the copy needs its path from elsewhere (the shooter's aim and the seed's speed).

**The shot's moment from the ammo, and a made-up flight.** So the copy flies whichever way it is, the maker reads its seed and arrow counts every walk (`PouchData.Item_DekuSeed.StockNum` 0x5fadf7fc, `Item_Arrow_Normal` 0x518ec74b; `CoopProjectiles::AmmoWatch`). When one goes down, the newest nocked `Shootable_DekuSeed` or `NormalArrow` of ours was let go then (`Sync::Shot`; log `coop: shots: our Shootable_DekuSeed let go (N left)`).
- If its place moved faster than 8 m/s in the next 100 ms, nothing changes: its samples carry the flight.
- If not, its flight is made up and sent in place of its place, as ordinary `OBJECT_SAMPLE`s. It goes straight from where it sat, along the +z axis of its turn (+0x328), at 40 m/s for a seed and 56 m/s for an arrow, for at most 0.4 s, then holds. Its remove carries the end of that flight. Log: `... did not move in its place after the shot: its flight is sent from the shot, from X Y Z at VX VY VZ m/s`. No new message, and the receiving games need no change.
- Switches, for the in-game check: `LICHII_COOP_SHOT_AXIS` (0 to 5: +x +y +z -x -y -z of the turn, default 2) if the copies fly the wrong way, and `LICHII_COOP_SHOT_SPEED` (m/s).
- Not covered: the hookshot tip (no ammo count) and a shot of the last seed or arrow if the count is not read in time. Bombs, bombchus, nuts and the boomerang are thrown actors with their own physics and keep their samples.

## Not done

- Compiled into the emulator and run with two games (2026-11-09, `tools/autotest` scenarios `spawn-seed` and `spawn-bomb`, `LICHII_COOP_SPAWN=1`, Graveyard save of the auto kits). In the logs, two slingshot seeds and a Deku Nut from A reach B. B creates each copy 50 to 70 ms after it arrives and deletes it when A's remove comes (seeds about 0.5 s later, the nut 0.3 s). The copies were not seen in screenshots: they are small, live under half a second, and the save's cow stands in front of both Links. The server must be a build with object owners (`server: object owners`, 2026-11-08); the older kit servers drop every object message.
- No bomb thrown yet. With `LICHII_COOP_OBJECTS_CHEAT=1` (pass the switches with `-Command`: `powershell -File` hands `-Switches` over as one string) the bomb bag and 20 bombs reach the ZR slot through the Items page (scenario `spawn-bomb`). The first ZR opens a "Bomb" help box that stays up through A and two more ZR. After it closes, ZR still gives no bomb in hand and the count stays 20: each ZR makes an `Item_Bomb` that is gone in 0.1 s, as in the earlier "Link lifts empty hands" note in the objects cheat. The sync now removes such a bomb as gone (it exploded only if it lasted 1 s), so the other game deletes its copy instead of blowing it up; that was seen happening before the fix. R (quick select) opens a carousel (stick, slingshot, Deku Nut, bottle, map) and did not offer bombs. Cause and fix (issue 125): the cheat lacked flags a real bomb bag sets ([items.md](items.md#bombs-from-the-cheat-issue-125)); not run yet.
- A nut copy is deleted at the owner's remove, so it does not flash in the other game. A nut could be treated like a bomb (life to 0) if that makes the game flash it; not tried.
- Rotation, the path on the server clock and bomb fuses: see [objects.md](objects.md), "Projectiles" > "What is built" (branch `projectiles`).
- Copies are the game's real actors, so a copy bomb also hurts things in its own game, and its own fuse runs: it may go off slightly before the owner's remove arrives. The harmless `CoopShot_*` packs of [objects.md](objects.md) would avoid both.
- A copy seed or nut made by the create request may not behave like a shot one (the game may give it its nocked state); its place is still written every frame.
- The live test: two games, A throws a bomb and shoots a seed; screenshots of both windows show the copy and the explosion at the same time.
