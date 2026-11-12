# Things a player makes, in every game

Plan stage 5 ([plan.md](plan.md), [objects.md](objects.md)): a bomb, a slingshot seed, a Deku nut, an arrow, the boomerang or the hookshot tip that one player makes appears in the other games too, moves the same and goes (or explodes) at the same moment. Code: `lichii/hook/coop_spawn.{h,cpp}`, tests in `tests/lichii/spawn_test.cpp`.

Turn it on with `LICHII_COOP_SPAWN=1`. That also patches the svc cave; the spawn director is its caller 10 (`CoopSvc::AddCaller`), after the puppets.

## Shared actors

| Actor | When gone, the copy | Seen live |
|---|---|---|
| `Item_Bomb`, `Item_Bombchu` | explodes (life set to 0); a held bomb is the stand-in `CoopShot_Bomb` until thrown (issue 169) | yes (two-game tests, 2026-11-11) |
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

## Held bombs and the fuse (issue 169)

- **Seen (two-game test, 2026-11-11).** While a player held a bomb, the other games showed it fall to the floor, snap back to the hands, and fall again, over and over. The explosion was shared, but the fuse's blinking was not shown.
- **Cause (read from the code, not measured).** The copy was the game's own `Item_Bomb`, a physics body with gravity. The director puts it at the owner's place every frame through the setter (vtable slot 0x218, 0x4c6580). That path (0x4c6630) stores the new transform on the body as a teleport and does not clear its speed (read with a disassembler from the game's code; no speed write was found there). So the fall's speed grows frame after frame, the body is drawn lower each frame, until it hits the floor and stops, and the next place puts it back in the hands. The fuse is in the bomb's AI (`ExecuteBarrelBombAI`, AS `Bomb_Blink`), which starts it when Link lifts the bomb; a copy is never lifted, so its own fuse never runs.
- **The owner.** A bomb is held while it is over our Link (at most 0.9 m to the side, 0.3 to 2.8 m above his feet) and moves with him (at most 2 m/s apart), or while its own animation says `Lifted` (`CoopProjectiles::InHands`, `HeldByAnimation`). It is let go at once when its animation says `Thrown`, or after 2 walks out of that box. Our Link's place comes from `PlayerReader::read_position`. The spawn and every sample carry the `held` bit (`object_state`, protocol 4: no new message) and the bomb's AS slot 0 (command, frame, rate; the AS `LiftableExplodeObject_BallBomb` has `Lifted`, `Thrown`, `Wait`, `Bomb_Wait`, `Bomb_Blink`, `Explode`). A held or animated bomb is sampled 30 times a second even when it does not move. A change of hands is sent at once.
- **The other games.** A held copy is made as `CoopShot_Bomb`, a stand-in in the puppet pack with the bomb's model and its animations (`ASRef`, `ASInfoRef`, `AnimationRef`) and no physics, life, liftable or AI. It is placed as the puppet is, with no setter, so it cannot fall. It is shown 100 ms late, between the owner's two samples around that moment, as the puppet holding it is (`HeldPath`), so it stays in the puppet's hands.
- When the owner lets go, the copy gets a new key: the director deletes the stand-in and makes the game's own `Item_Bomb` where the owner's bomb is, which then follows the owner's flight and explodes as before. A bomb that goes off in the owner's hands is made as `Item_Bomb` in the hands first, then blown up once it is live.
- **The fuse on the copy.** The director plays the owner's AS slot 0 command on the copy (stand-in or bomb) with the game's play call (0x2022a0), as the objects' mover does for enemies, and sets its frame when it is more than 3 frames off (0x37b5d70). Names go in fixed cells after the director's slots (one per name, never rewritten).
- With a pack built before this (no `CoopShot_Bomb`), or after 3 failed tries, the held copy is the game's own bomb, as before.
- Log: `coop: shots: bombs held: N of ours, M copies in other players' hands` when either count changes; `spawn: CoopShot_Bomb: create copy ...`.
- **Not proven in a game.** The box and the `Lifted`/`Thrown` names on the owner, the stand-in drawing the bomb with its animation, and the blink frames.

## Seeds, arrows and hookshot tips: stand-ins (issue 85)

Players saw the other player aim the slingshot, but never the seed. The player logs (2026-11-10) show why it was not the network: every seed was sent (`spawn: sent Shootable_DekuSeed`), and the other game made each copy and deleted it at the remove (`create copy`, `copy N is actor`, `copy N deleted`). The copy was there and was not drawn.

- **Why** (read from the romfs and `main`, no run): the copy was the game's own `Shootable_DekuSeed`, made by the create request with no shooter.
  - A shootable made that way starts in the state its `ShootableParam` names as `NoShooterCreateStartState`. That is `UsedExternally` (hash 2207839275; the states are `Sleep`, `Prepare`, `Shoot`, `Stationary`, `Reflected`, `UsedExternally`).
  - The seed's `ShootableParam` also has `IsPrepareVisibleOff: true`, and nothing ever shoots the copy.
  - Arrows (`NormalArrow`) and hookshot tips (`Shootable_Hook`, `Shootable_HookLong`) are shootables made the same way, so they are given the same fix.
- **Fix:** the copy is a stand-in of ours: `CoopShot_Seed`, `CoopShot_Arrow`, `CoopShot_Hook`, `CoopShot_HookLong`, and (player-shots) `CoopShot_Boomerang`: the game's own `Weapon_Boomerang` made here has its AI, homing sensor and attack, so it would fly at this game's enemies and hit them.
  - The hookshot tips keep their animation set (`AS/Weapon_Range_Hookshot.root.asb`, as the bomb stand-in keeps its own): the tip's model is the whole chain (`Item_Hookshot_Chain`), and the animation set names `ChainExtend`, `HookshotChainExtend` and `HookLengthGuard`. The maker sends the tip's AS slot 0 in the spawn and every sample, and the director plays it on the copy (`PlayAs`, then the frame), as for a held bomb. Not yet seen in a game: whether this draws the chain from the puppet's hookshot to the tip.
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

## Where a shot lands (issue 166)

With the stand-ins in the pack and the spawn sync on, the spawn-seed run of 2026-11-11 00:44 shows the seed's place does move in flight: A's seeds went 11 to 15 m at 70 to 80 m/s and were removed as `hit` (reason 3). B made each `CoopShot_Seed` copy and deleted it at the remove. So the seed now flies on every screen. What was missing is its end.

- **Landing.** A copy removed as `hit` used to be deleted at once, wherever its carry-ahead had taken it (up to 250 ms past the last sample, so several metres beyond the wall). Now the remove stops its track at the owner's last place and the copy stays there for `CoopProjectiles::LandHoldUs`: 0.2 s for a seed, 1.5 s for an arrow (it sticks), then it goes. Nuts, bombs and the rest go as before. A made-up flight (above) ends as `gone`: its end is a guess, so its copies are not held there. Log on the receiver: `coop: shots: copy held where the owner's shot hit (N so far)`.
- **Bounces.** The maker checks each step of its seeds, arrows and nuts: a turn of over 45 degrees with both steps at 3 m/s or more is a bounce (`CoopProjectiles::Bounced`). That step's sample is sent at once, without waiting for the 33 ms gap, so the copies turn where the owner's did. A copy's track keeps its fall across a bounce (the turn is not counted as gravity). The maker's `gone` line now ends `bounced N times`.
- No new message: the landing and the bounce ride on `OBJECT_SAMPLE` and `OBJECT_REMOVE`.

**The spark and the sound are not done.** Read from the romfs (no run):
- The seed's hit is played by the game's shootable code, not by data a copy can carry: `ShootableParam/Shootable_DekuSeed` names `HitEffectType` (hash 2054188199), and its `ELinkParam` and `SLinkParam` only name the user `Shootable_DekuSeed`. The stand-ins leave effects and sounds out on purpose, and have no shootable component.
- A way that needs no code calls: a small actor of ours made at the landing place, with the seed's ELink and SLink user and an AI root that plays one xlink key on enter. The game has that node: `OneShotXLinkSearchAndEmit` (inputs `XLinkKeyName`, `ELinkCall`, `SLinkCall`) is in 454 actor AI files. The smallest such roots are `Shootable_ScrubBusinessSees_PostUpdateMatrix.root.ainb` (1,806 bytes: on a shootable state, emit `ScrubBusinessSeed_HitGround`) and `EftLoc_DodongoCave_21_Floor_A_01_Smoke_00.root.ainb` (1,880 bytes). The key names of the seed's hit, and an AINB writer for the pack tool, are still to find and build. The SLink strings include `SE_Shootable_DekuSeed_Shoot00` and `SE_Obj_DekuSeed_Land`.
- Until then a landing shows as the seed stopping where it struck, with no spark or sound in the other games.

## Not done

- Compiled into the emulator and run with two games (2026-11-09, `tools/autotest` scenarios `spawn-seed` and `spawn-bomb`, `LICHII_COOP_SPAWN=1`, Graveyard save of the auto kits). In the logs, two slingshot seeds and a Deku Nut from A reach B. B creates each copy 50 to 70 ms after it arrives and deletes it when A's remove comes (seeds about 0.5 s later, the nut 0.3 s). The copies were not seen in screenshots: they are small, live under half a second, and the save's cow stands in front of both Links. The server must be a build with object owners (`server: object owners`, 2026-11-08); the older kit servers drop every object message.
- No bomb thrown yet. With `LICHII_COOP_OBJECTS_CHEAT=1` (pass the switches with `-Command`: `powershell -File` hands `-Switches` over as one string) the bomb bag and 20 bombs reach the ZR slot through the Items page (scenario `spawn-bomb`). The first ZR opens a "Bomb" help box that stays up through A and two more ZR. After it closes, ZR still gives no bomb in hand and the count stays 20: each ZR makes an `Item_Bomb` that is gone in 0.1 s, as in the earlier "Link lifts empty hands" note in the objects cheat. The sync now removes such a bomb as gone (it exploded only if it lasted 1 s), so the other game deletes its copy instead of blowing it up; that was seen happening before the fix. R (quick select) opens a carousel (stick, slingshot, Deku Nut, bottle, map) and did not offer bombs. Cause and fix (issue 125): the cheat lacked flags a real bomb bag sets ([items.md](items.md#bombs-from-the-cheat-issue-125)); not run yet.
- A nut copy is deleted at the owner's remove, so it does not flash in the other game. A nut could be treated like a bomb (life to 0) if that makes the game flash it; not tried.
- Rotation, the path on the server clock and bomb fuses: see [objects.md](objects.md), "Projectiles" > "What is built" (branch `projectiles`).
- Copies are the game's real actors, so a copy bomb also hurts things in its own game, and its own fuse runs: it may go off slightly before the owner's remove arrives. The harmless `CoopShot_*` packs of [objects.md](objects.md) would avoid both.
- A copy seed or nut made by the create request may not behave like a shot one (the game may give it its nocked state); its place is still written every frame.
- The live test: two games, A throws a bomb and shoots a seed; screenshots of both windows show the copy and the explosion at the same time.
