# Native puppets: other players as real game bodies

Status: design and plan (native-puppets-a, 2026-11-19), plus the test switches `--puppet-body` (phase 1),
`LICHII_COOP_BOMB_SHARE` (phase 2) and `--puppet-liftable` + `LICHII_COOP_LIFT_PLAYER` (phase 3), each below, and phase 4, PvP on real bodies, `--puppet-pvp` + `LICHII_COOP_PVP_NATIVE` (section 5: the pack, the server and the glue are built; its path B, the victim's own game working out the damage, is not live yet, so today's PvP stays). Nothing here is in a release. Today's puppet ([puppet.md](puppet.md)) and the real-Player test
([native-player.md](native-player.md)) stay as they are until each phase below is proven in two games.

## STATUS (native-puppets-f, 2026-11-27)

Owner 2026-11-27: "native puppets with perfect PvP and interactions like lifting another child Link player and
throwing them like child Ruto; hit effects when hitting other players; blocking hits with the shield that shows on the
other person's screen perfectly synced; puppet physics sync and Navi physics sync". Standing rule: only a young Link
can be lifted (any Link may lift him), with Ruto's lift and carry animations.

"Proven in 2 games" means a two-game run judged PASS by state and a screenshot of each game. No item has that yet.
Every switch is off by default; solo and co-op idle have no copy, so none of this runs there.

| Owner item / phase | Built (where) | Proven in 2 games (run) | Missing |
|---|---|---|---|
| Bodies (phase 1, `--puppet-body`) | Pack: Link's own shapes, a Free character body, his cloth (build.py). Hook: one rule moves the body (rest, setter, warp; `coop_puppet_body.h`), it gives way to our Link (Yield); host test 11/11 (fable-puppet-body 1effc9493, merged on this branch) | Partly: proof-0116-5 on a487cd77 (COORD-LOG 17:59 UTC 2026-11-19): copies made with bodies, child and adult, 0 CRASH, shots of both games. Walk-into and stand-on NOT judged (the scenario's walk missed). The body rule itself never ran in a game | B1 (A stops at B's copy, A stands on it, no jitter); B2 (the speed field: a walking copy does not carry a rider yet); `Carry` not wired |
| Lift and throw a young Link like Ruto (phase 3) | Pack `--puppet-liftable`: Ruto's `LiftableRef` on the child copies only, her carried pose `Act_LiftRest_Wait` (asb_carry). Glue `LICHII_COOP_LIFT_PLAYER=1`: `LIFT_ASK` / `LIFT_ANSWER` / `LIFT_PLACE` (535-539), young-only gates on the lifter, the server (`not_young`) and the lifted game; the throw is `LIFT_PLACE flying` (native-puppets-c, -d; in integrate) | No, never ran: proof-0116-3 (COORD-LOG 14:55 UTC 2026-11-19) PACK FAIL `missing Pack/Actor/Npc_Zora_Ruto_Child_001.pack.zs` | That PACK FAIL's root cause, fixed here: a .nup build extracts only `dumpcheck.REQUIRED + OPTIONAL`, and OPTIONAL lacked Ruto's pack and `AS/Player_Child.root.asb` (both are in the .nup: read 2026-11-27). Then the first lift in a game (UNKNOWNs 1, 7, 8) and the throw's landing on both screens |
| Bomb hand-over (phase 2) | Protocol 530-532, server rules, glue `LICHII_COOP_BOMB_SHARE=1` (native-puppets-b, -b-2) | No: proof-0116 bomb-handover FAIL, no `BOMB_TAKE` (COORD-LOG 15:10 UTC 2026-11-19) | The director replays the owner's animation on the bomb copy before our walk sees `Lifted` (fable-puppet-body's reading). Not an owner item of 2026-11-27: after the others |
| Hit effects (the game's own spark, sound, flash on a hit player) | The victim's own hit replay with the strike's kind words (`HurtOurLinkNative`); every other game replays a result on its `_Pvp` copy (`CoopObjects::ReplayOnCopy`: flash, sparks, sound on its invincible Life) (native-puppets-e-3) | No: the PvP pack never built: proof-0116-5 `missing Pack/Actor/Enemy_Amos.pack.zs` (COORD-LOG 17:59 UTC 2026-11-19) | The same PACK FAIL cause, fixed here (`Enemy_Amos` added). Then path B live (next row), so a hit lands; the attacker's own spark comes from its own collision with the `_Pvp` copy (U6, unproven) |
| PvP damage and knockback | Protocol 540-544, server `on_pvp_strike`, `coop_pvp_native.h` (rings; the rewind check fair at 200 ms: fable-puppet-pvp 15c1f45e1, merged on this branch); path B's amount from the victim's own tables | No | The kinds' multipliers (U2/U7) are not filled, so `PathBLive` is false and every native strike is "kept off"; today's `PVP_HIT` (242) still does the damage. Needs the one probe run (U1, U2, U4, U6, U7) |
| Shield block, synced on both screens | Path B's guard check (today a guess: "guard" in our Link's AS name and 60 degrees); U5 read from main: the game's own guard bits (EquipmentUser +0x13a0 bit 5, player part +0x32 bit 4; fable-puppet-pvp-2) | No | `Guarding` from the game's two bits (measured once by the probe); the guard result replayed on the attacker's and every other game's copy (the guard reaction and the clang) |
| Puppet physics sync | The body rule (above); Yield wired; lift Carry wired for every copy (puppet-body-4, "Lift carry" below) | No | B2's speed field (a walking copy carries a rider); a thrown or knocked-back copy follows its owner's path |
| Navi physics sync | Each copy has a Navi (`CoopFairy`, `Dm_Npc_Navi`) placed from the copy's place plus a fixed shoulder offset and bob (docs/puppet.md "Fairy": "The remote player's real Navi position is not sent") | No | The owner's real Navi place and state (idle, flying to a target, Z-target) sent and shown, so another player's Navi moves as she does in her own game |

### Plan for the missing items (native-puppets-f: smallest first, each phase its own two-game bridge test)

Every proof is a pytest bridge test in `tools/autotest/ingame` (docs/ingame-tests.md), judged by state, log lines and a
screenshot of each game. The pack for it is a test pack (`--pack native`, `ingame/packs/native.py`: the tested sha's
build.py with the lane pack's own options plus the phase's switch); the glue switch goes by `--env`.

| # | Phase | Built in it | Two-game proof (bridge test) |
|---|---|---|---|
| N0 | Base | integrate-0121 + fable-puppet-body + fable-puppet-pvp; the .nup list fix (Ruto, the carry AS, Amos, InterestSpotActor) | Host: `test_nup_files_has_the_stand_in_sources`; one full CI |
| N1 | Bodies in a game | Test pack `native` (`--puppet-body`); built: `ingame/packs/native.py` (the lane's pack command + the switch, with lane A's Character Select, only the changed files go in), `test_native_body.py`, fake selftest `tests/test_ingame_native_body.py`; not run in a game yet | `test_native_body.py`: B stands; A walks into B's copy and stops short of it (state); A placed over the copy stays on its top (y); B walks away with no jump in the copy's `body:` line; shots of both games; 0 CRASH, 0 `unguarded null read` |
| N2 | Lift, carry and throw a young Link | `--puppet-liftable` in the test pack, `LICHII_COOP_LIFT_PLAYER=1` | `test_native_lift.py`: young A lifts young B's copy: A plays Ruto's lift and carry, B's game puts its Link in A's hands (log, B's place), B's screen shows A's copy in Ruto's carry, A's screen shows B in `Act_LiftRest_Wait` (shots both); A walks, B follows; A throws: B lands where A's screen showed (under 1 m, both states); adult B: no lift, no message |
| N3 | Hits: damage, knockback, hit effects | `--puppet-pvp`, `LICHII_COOP_PVP_NATIVE=1`; the one probe run fills U2/U7, then `PathBLive` | `test_native_pvp.py`: PvP on, A's sword hits B: B's life drops by the game's own amount and B is knocked back (state), `PVP_RESULT taken`; A's screen shows the spark and flash on B's copy (shot at the hit frame) and the sound line; PvP off: the sword passes through |
| N4 | Shield block on both screens | Guard from the game's own bits (U5); the guard result replayed on every copy | `test_native_guard.py`: B holds the shield facing A, A swings: B's life unchanged, B plays the game's guard reaction, both games' shots show the guard and the spark; from behind: the hit is taken |
| N5 | Puppet physics sync | B2's speed field, `Carry` wired; thrown and knocked-back copies follow the owner's path | `test_native_body.py`, more cases: A rides walking B's copy; a thrown B's copy on A's screen within 0.3 m of B's own place at each sample |
| N6 | Navi physics sync | The owner's Navi place and state sent (a new message, claimed in COORD-LOG) and shown on the copy's Navi | `test_native_navi.py`: B Z-targets a sign and B's Navi flies to it; on A's screen B's Navi is there within 0.5 m (state and shot); B idles: the same places |

## The goal

The other players stop being picture-only puppets and become things the game itself handles:

1. **Bodies.** You bump into another player, can stand on them, push them, and they block a doorway.
2. **Lift and throw a young player**, the way Link lifts and carries Ruto in Jabu-Jabu (owner rule: only a
   young Link can be lifted; any Link may lift him).
3. **Take another player's bomb.** Pick up the bomb they put down (or catch it), and the bomb changes
   hands the way enemies and bosses do today: one fuse and one explosion in every game. An unlit bomb
   you hold can go back into your own bomb count, through the game's own count.
4. **A base for good PvP.** Hits land on the real body (its hit and guard shapes), with the game's own
   knockback and shield.

## Words

- **Owner game**: the game of the player the Link belongs to. It is the only one that runs that Link's
  input, life and death.
- **Copy**: what the other games show for that Link (today the `CoopPuppet_*` pack actor).
- **Hand-over**: the server's object ownership move (`HANDOVER_ASK` 20 / `HANDOVER_STATE` 21,
  [protocol.md](protocol.md)), which carries the last state to the new owner.

Every game fact below gives where it was read. "romfs" means the game's own files read with our pack
reader (`tools/puppet_pack`: zs, sarc, byml; `flatten_actor_param` merges the `$parent` chain) from the
extracted romfs of the pinned game build. "main+0x..." are addresses in the game's `main` (build
e0f3ccfa, as in native-player.md and the reports named). UNKNOWN facts carry the probe that settles them.

## 1. What the game's Link has that a puppet lacks

The player's components (romfs, `Pack/Actor/Player_Child.pack.zs`, ActorParam merged: 54 components;
Player_Adult 55, hs-puppets-alt-b). The puppet keeps 10 of them (`KEEP_COMPONENTS` in
`tools/puppet_pack/build.py`: AS, animation, model, bone/model bind, equipment user, LOD, shadow), the
two sound ones (`SLinkRef`, `SoundFootStepRef`), the player's `AffiliationRef` and its own
`PerceptionEmitterRef` with a body-less `PhysicsRef` (InterestSpotActor's, an empty controller set:
"the puppet still collides with nothing"), and the shared default `WorldInfoRef` / `XLinkRef`.

What each feature needs, by component (names from the romfs; component slots from the engine's registry
main+0x11e9b00 / main+0x120c800, reports/hs-puppets-alt-b.md: slot 10 Physics, 87 Life, 107 Player):

| Feature | Player's component (romfs) | What it holds | Puppet today |
|---|---|---|---|
| Body (bump, stand on, push) | `PhysicsRef` -> `Phive/ControllerSetParam/Player_Child` ($parent `Player`) | `InitializerName: Character`; character controller `Phive/CharacterControllerParam/Player` (gravity, slopes, `UserContactCollectionName`); shapes `Player_cct`, `Player_cct_crouch`, `Player_cct_climb`, `Player_Tgt_Body`, `Player_Atk_AtkPlayerBody`, `Player_Chemical_Body`, `Player_RideAstride`, `Player_ForArea`...; matter bodies `MainMatterRigidBody`, `ClimbMatterRigidBody`, `WeightBody`; contact collections `Contact_0_cct` (`HitToBlockable`), `RagdollBody`; ragdoll, foot/hand IK, cloth | body-less physics (perception only) |
| Being hit, guarding | `SensorHitRef` (`SensorHitParam/Player`: `AtHitInfoBufferSize 8`, `TgHitInfoBufferSize 8`, `IsThroughSameAffiliation false`) + the controller set's sensors `Tgt` (`Player_Tgt`, `PlayerReceipt`), `Atk` (`Player_Atk`, `PlayerAttackReceipt`), `Chemical`; `ReflectShield` rigid body | which attacks reach which shape; shield reflect | none |
| Life, knockback, death | `LifeRef` (`LifeParam/Player`), `ReactionRef` (`Reaction/Player`), `DeathRef`, `ConditionRef`, `ChemicalRef` | hearts, the hurt reaction, fire/ice/electric | none |
| Lifting things | `PlayerLifterRef` (`Game/PlayerLifterParam/Player_Child`, $parent `Player`): `LiftGrade` 3655598229 (child), `LiftThrowParams` Default (bind bone `Weapon_R`, throw 120 / 170 m/s impulse, `IsIgnoreBombOnPrepareLifted`), `HeavyRock`, `Fish` | what Link can lift and how he throws | none |
| Holding things | `ActorHolderRef` (`ActorHolderParam/Player_Child`) | what sits in his hands | none |
| Riding / being ridden | `RiderRef`, `RidableRef` (`RidableParam/Player`), the `RideAstride` rigid body (alias `Sub`) | Epona; something riding Link | none |
| Being the player | `PlayerRef` (slot 107, the player's own code), `ActorNameRef` `Player`, `PlayerCameraTargetRef`, `GameLifeConditionRef` `NoDelete` | input, camera, the game's "who is the player" | none (must stay out on a copy, alt-b) |
| Being seen | `AffiliationRef` `Player`, `PerceptionEmitterRef`, `AttentionReceiverRef`, `TargetRef` | enemies and Z-targeting find him | affiliation + emitter (perception-on-a) |

What is liftable in the game (romfs): an actor with a `LiftableRef`. `Item_Bomb`'s `LiftableParam`:
`LiftGrade` 3655598229 (the same hash as the child lifter's grade), `LiftableType` 3237216321,
`WeightForCalcThrowPower` 18, `ThrownEntityAt.IsDisable`. `Animal_Cucco` (a living, AI-run actor that
Link lifts: the model for lifting a player) has `LiftableRef` with `LiftGrade` 3069107721,
`LiftableType` 1577663790, `KeepSensorHitToParent`, `IsFinishThrownStateByDamage`,
`IsResetBehaviorModeOnThrown`, `WeightForCalcThrowPower` 28. **The player has no `LiftableRef`**, so
nothing in the game can lift Link as he is.

UNKNOWN: which lift grades the child and adult lifters accept (the hashes are names; the order between
them is in code). Probe: from the game's `LiftGrade` enum strings in main (search the rodata for the
murmur3 of the names the romfs uses: 3655598229, 3069107721, 375060617) and the lifter's check
(the `PlayerLifterParam` reader); or in game, one run lifting a Cucco as child and as adult.

## 2. The options

### A. A second real `Player_<Age>` driven by the network

What native-puppets-b2 started (`LICHII_COOP_NATIVE_PLAYER`, docs/native-player.md): the game's own Link,
made by the Director, then given the other player's pad through two wraps (buttons main+0x279ce80,
sticks main+0x279e2c0; Hookshot patch 0017 rows, wrap ids 16-17).

- For: everything at once and exactly as the game does it (body, lifter, sword, shield, items, cloth,
  hurt reaction), tens of bytes a frame instead of a pose.
- Against: the game is made for one player. Known (native-puppets-b, read from main): "the player" is the
  preload list record (main+0x9456b0 walker; main+0x37cca0, 15 callers), so a directly created second
  Player is not "the player" (good); but its `PlayerRef` code runs a full Link: it reads the pad
  (fixed by the wraps), it can open doors, start talks, pick up and spend items from **this** game's
  save (pouch and life live in the save, not on the actor: UNKNOWN which reads go to the save), trigger
  scene changes, voids, and cutscenes. Its result can drift from the owner game (two simulations of
  one Link) and must be corrected every frame, which the player code may fight (it moves by its own
  character controller).
- Status: step 1 code merged (test switch, idle by default); never run in a game.

### B. The puppet with bodies from the game's own data (component by component)

Keep `CoopPuppet_*` as the copy (placed by the hook through the game's physics-aware setter,
`ExecuteActorForceSetMtx` main+0x2622e50 / vtable slot 0x218 = main+0x4c6580, coop_puppet.h `kSetMatrix`),
and add, one at a time, the pieces each feature needs, each taken from the game's own files:

- a body: `PhysicsRef` with a character body and no gravity (phase 1);
- hit and guard shapes: the `Tgt`/`Atk` sensors + `SensorHitRef` (phase 4);
- liftable: a `LiftableRef` of its own, Ruto's, on the child copies only (phase 3).

- For: each piece small and testable alone; the owner game keeps sole authority (the copy never decides
  anything by itself); no player code runs on the copy, so nothing touches this game's save or camera.
- Against: knockback, guard and lift reactions are not played by the copy's own code; the owner game
  plays them and the copy shows them through pose sync (as today).

### C. Hybrid (the plan)

Option B for the copy's **body, sensors and liftable** (what the local game must collide with, hit and
lift), with the **decisions** taken where the game already has the code: the owner game's real Link.
When this game's sword touches the copy, the hit is sent to the owner, whose real Link takes it through
the game's own hit path (the hit replay, coop_hit_replay.h) and plays its own knockback; the copy shows
it. When this game lifts the copy, the owner's real Link is put in a "lifted" state by the owner game.
Option A stays a separate test track (driving with inputs); if it is ever proven it replaces the copy,
and every phase below still holds because the copy's components are the same ones the real Link has.

## 3. Lift a bomb, and stash it

What the game uses (romfs `Item_Bomb`): `LiftableRef` (above), `LifeRef` (max 1, `BallBomb`),
`ChemicalRef` (explode), `ShootableRef` (`ShootableParam/Item_Bomb`: `ShootUseAttackParam`,
`TimeKeepStationary` 30), `PouchContentRef` (`PouchContentParam/Item_Bomb`: `EnableStock true`,
`MaxNumGameData "Bomb"`, `SmallBagActor Item_BombBag_Small`), `DeathRef` `PlayerHoldedItem`, AI
`Item_Bomb.root.ainb`, AS `LiftableExplodeObject_BallBomb` (states `Lifted`, `Thrown`, `Bomb_Wait`,
`Bomb_Blink`, `Explode`: coop_projectiles.h `HeldByAnimation`). The fuse starts when Link lifts it
(reports/held-bomb.md: AI `ExecuteBarrelBombAI`, AS `Bomb_Blink`).

Today (docs/spawns.md "Held bombs and the fuse"): the thrower's game owns its bomb; the others see a
`CoopShot_Bomb` stand-in in the hands and a real `Item_Bomb` once thrown. A bomb lying on the floor in
the other game is a real `Item_Bomb` copy: its own AI would light and explode it, which is why only the
owner's explosion counts.

The plan (phase 2):

- **A bomb is a shared object with an owner** (coop_objects_sync, the same rules as enemies: one owner,
  epoch, hand-over with state). Its state: lit or not, fuse time left, held by (player or none).
- **Taking it:** when this game's Link lifts a bomb copy (the game's own lift, on the real `Item_Bomb`
  copy, which keeps its `LiftableRef`), this game asks for it (`HANDOVER_ASK`-like request with the lift
  time). The server gives it to the first asker by the hand-over rules; the new owner gets
  `HANDOVER_STATE` with the fuse left and continues that fuse on its own bomb; the old owner's bomb
  becomes the copy. A refused game drops its lift (the bomb returns to the owner's place). Fuse: one
  number (time left) carried in the state, so every game explodes it once, at the owner's word
  (`WORLD_BREAK`-like event, as bosses' deaths).
- **Stash:** an unlit bomb in Link's hands can go back into the pouch through the game's own count:
  `PouchContentParam.MaxNumGameData "Bomb"` is the game data the pouch counts in (coop_cheat_items.h
  writes the same "Bomb" count through the game's flag queue). The native route is the game's own
  "pick up a pouch item" path (`PouchContentRef`, `EnableStock`), not a count write. UNKNOWN: whether
  the game lets Link put a held unlit bomb back (a button) or only picks a lying bomb up. Probe (one
  game, solo, module log only): hold an unlit bomb, press the item button / put it down and walk over it;
  log the "Bomb" game data and the bomb actor's AS state each frame. If the game has no stash path,
  the stash is the pick-up path run on a bomb put down (no new rule).
- A bomb that is stashed is gone for everyone (the owner deletes; `WORLD_BREAK`-like gone with
  no explosion), and only the stashing game's count changes.

### Phase 2 status (native-puppets-b, branch `native-puppets-bomb`)

- **Built and host-tested:** the protocol ([protocol.md](protocol.md), "Bomb hand-over": `BOMB_TAKE` 530,
  `BOMB_ANSWER` 531, `BOMB_SHARE` 532, no protocol bump) and the server's rules (first lift wins, an
  unanswered ask keeps the bomb with its owner, the old owner's late explosion refused, a stash is a
  picked-up remove, a bomb whose owner leaves goes on with a sharing game and its fuse). The game-side
  rules as pure code, `lichii/hook/coop_bomb_share.h` (lift detection from the copy's own `Lifted`,
  one ask per lift, the fuse moment from the handed state, the owner's answer only when its Link does
  not hold the bomb, the stash from the pouch's "Bomb" count rising by one, the own-bomb registry).
- **Wired (native-puppets-b-2), host-tested, not yet run in the game:** `LICHII_COOP_BOMB_SHARE=1` turns it
  on in the spawn sync (`Sync::SetBombShare`; off by default, and off nothing is sent or taken in).
  Lifting a copy sends `BOMB_TAKE` and stops the director placing and animating it; the hand-over moves the
  copy's actor into our own bombs (the director lets go of it, `Act::release`), samples it at the new epoch
  with the fuse handed over, and at its end sets its life to 0 once (the game's own explosion, the way a
  copy is blown). A refused or unanswered lift gives the copy back and is not asked again until our Link
  lets go. Asked for one of ours: answered with its state unless our Link holds it. Handed on: our actor
  is deleted without an explosion (checked by vtable) and the new owner's copy shown. A taken unlit bomb
  gone as `PouchData.Item_Bomb.StockNum` rose by one is removed as picked up. The objects sync leaves a
  `HANDOVER_ASK` for one of these bombs to the spawn sync. A bomb handed to us because its owner left is
  adopted too (made as the game's own bomb first if it was shown held). Only `Item_Bomb` changes hands:
  bombchus, pots, rocks and other liftables are not shared this way (`CoopBombShare::IsBomb`).
- **The game facts this leans on (phase 0 probe):** that a lifted copy plays `Lifted` on its own AS slot 0
  (the copy is the game's real `Item_Bomb`, with its `LiftableRef`); that the director must stop placing it
  while our Link holds it; whether the game can stash a held unlit bomb at all (if not, the stash path is
  the pick-up of a bomb put down: the same remove, the same count rise).

## 4. Lifting another player

**Owner rule (2026-11-19, absolute): only a YOUNG Link can be lifted, carried the way Link carries Ruto.**
"You should only be able to grab a young link, like how you grab ruto. You can basically give link the same
animation." An adult Link is never liftable (no lift reaction in the game, no `LIFT_ASK`, no message); a young
Link may be lifted by a young or an adult Link (the lifter's age is never a reason to refuse; the game's own
lift rules decide). The carry is Ruto's: the copy's `LiftableRef` is Ruto's own `LiftableParam`, so the
lifter's game plays Link's Ruto set of lift, carry, put-down and throw.

Needs, on the child copy, what Jabu-Jabu's Ruto (`Npc_Zora_Ruto_Child_001`) has: a `LiftableRef` (phase 3;
her param, own file in the puppet pack), and on the owner game a "lifted" state for its real Link.

- **Who decides.** The lifted player's game (owner of that Link). The lifter's game sees the game's own
  lift start on the copy and sends `LIFT_ASK(lifted, lifter)`; the owner game accepts unless its Link is
  in an event, a cutscene, dead, already lifted, or (PvP off) the player turned it off. Until the answer
  (one round trip) the lifter's game plays the lift as the game does (the copy follows Link's hands);
  on refuse it drops the copy back to the owner's samples.
- **While lifted.** The lifter's game owns the **place** (the copy sits on the lifter's hand bone,
  `Weapon_R` / `Weapon_Root` per `LiftThrowParams`), and sends it; the lifted game puts its real Link
  there through the same physics-aware setter (main+0x4c6580), input to movement off (the game's own
  "input off" check main+0x3025850, native-player.md), the camera stays on its Link. The lifted
  player's buttons only ask to wriggle free (a timer, as the game's held Cucco escapes: UNKNOWN).
- **Throw.** The lifter's game throws the copy with the game's own throw (`ImpulseBasedThrowSpdForwardMps`
  120, `ImpulseBasedThrowSpeedYMps` 170 for Default), sends `LIFT_THROW(velocity)`, and the lifted
  game gives its real Link that velocity (knockback path, UNKNOWN: which setter takes a velocity on the
  player's character controller; probe: the hurt reaction's push in main, or the `ReactionParam/Player`
  knockback field), and owns its place again.
- Ownership of the lifted Link never moves; only "who drives the place" does, for the lift time. This
  is the same split as a boss carried by its root.

### Phase 3 status (native-puppets-c, branch `native-puppets-lift`)

**Row 317 (0.12.1): on by default.** The kit's pack command (kit-paths.ps1 `Get-KitPackArgs`, the Linux kit's
build.sh) passes `--puppet-body --puppet-liftable=auto` (the lift needs the copy's body, UNKNOWN 1 below; `auto`:
a game file without Ruto's pieces makes the pack without lifting, a note, never a failed pack), and the hook's
switch is on unless `LICHII_COOP_LIFT_PLAYER=0`. Not connected (solo, co-op idle) the glue only resets: no place
watch, nothing sent. The text below is the phase as first built (both switches off by default then); the in-game
check is the owner's rc play.

Built and host-tested, **not run in a game**. Two switches, both off by default; with either off nothing below
runs, nothing is sent, and the solo and idle game are as before (no session: no copy exists, and the glue
returns before anything when not connected).

- **Pack:** `tools/puppet_pack/build.py --puppet-liftable` gives every **child** puppet (`CoopPuppet_Child*`,
  `LIFTABLE_VARIANTS`) a `LiftableRef` of its own, Ruto's (native-puppets-d; before it the Cucco's):
  `Npc_Zora_Ruto_Child_001`'s `Component/LiftableParam/NpcZoraRutoChild.game__component__LiftableParam.bgyml`
  (`LiftGrade` 3069107721 as the Cucco's, `LiftableType` 3056776872, `LiftPosOffset`/`PutPosOffset`
  (0, -0.0022, 0.0501), `WeightForCalcThrowPower` 19, `SpecialDeal` [213340665], `IgnoreLifterOnThrownFrame` 8),
  its in-pack `$parent` chain merged (the game's file has none); own file
  `Component/LiftableParam/<puppet>.game__component__LiftableParam.bgyml`, sized in the size table as hers.
  Adult puppets get none: the game itself can never lift an adult copy. Off: the pack is byte for byte as
  before (test `test_pack_liftable`, which also checks the adult pack is the same on and off).
- **Ruto's carry (owner rule).** The lifter's game picks Link's animations by the liftable's type, and Link's AS
  (Player_Child and Player_Adult, both) has a Ruto set beside the Cucco's: `Lift_Ready_Ruto`, `Lift_Up_Ruto`
  (and `_High`), `Pose_Ruto` / `Pose_Ruto_Run` / `Pose_Ruto_Caution` (the carry), `Lift_Put_Ruto` /
  `Lift_Put_Stand_Ruto` (and `_High`), `Lift_Throw_Ruto`, `Damage_Though_Ruto*`. With Ruto's `LiftableType` on
  the copy the game plays those, with its own carry limits (walk, no roll or attack while carrying, put down or
  throw), as with Ruto. `OurLinkLifts` takes `Pose_Ruto` as a carry too.
- **The carried Link's pose: Ruto's own animation (native-puppets-d follow-up).** Ruto's carried pose is her
  AS command `Act_LiftRest` (`_St`, `_Wait`, `_Ed`, `_LookAround`, `_Detect`) in `AS/Npc_Zora_ZU_Ruto_Child.root.asb`,
  its loop the clip `Act_LiftRest_Wait` in `Npc_Zora_ZU_Ruto_Child_Animation.anim.bfres`. Link's AS has no such
  command, so the young copies get their own (`tools/puppet_pack/asb_carry.py`, with `--puppet-liftable`):
  - `AS/CoopPuppet_Carry_Child.root.asb`: a copy of `AS/Player_Child.root.asb` (from the user's romfs) in which
    the two clips of Link's `Eaten` command (`Eaten_F`, `Eaten_B`) name `Act_LiftRest_Wait` instead. The AS
    binary as far as this needs it (version 0x41b, read from the game's files): the string pool is the file's
    last section (offset header word 14, size word 23); a node's clip name is a u32 pool offset, and each of the
    two clip names is named by exactly one node field. The new name goes at the pool's end (no offset moves; 7
    bytes change and 18 are added on the game's file) and the two fields point at it. The child copies'
    `ASRef` names it (a skin keeps its own AS); the game's own Link, the adult copies and every other actor
    keep the game's file.
  - The copy's own AnimationParam (`Component/AnimationParam/<puppet>.engine__component__AnimationParam.bgyml`):
    the player's resource list plus `{"IsRetarget": true, "ModelProjectName": "Npc_Zora_ZU_Ruto_Child_Animation"}`,
    the way the game's own child Epona (`Animal_Horse_Child`) plays the adult Epona's clips. Link's and Ruto's
    skeletons name their main bones alike (`Arm_1/2_L/R`, `Leg_1/2_L/R`, `Spine_1/2`).
  - While lifted, the lifted game sends `Eaten` as its Link's animation (`CoopLiftPlayerGame::CarriedPose` in the
    hook's read, `CoopLiftPlayer::CarriedAnimation`), so every copy of it, on the lifter's screen and every
    other, plays `Eaten` from frame 0 at rate 1 (looped, not held on one frame), which on a young copy is
    Ruto's carry loop. Fallback: if the AS cannot be made (another
    AS version, a clip name missing or named twice) the build says so and the copies keep the game's AS, so
    `Eaten` plays Link's own Like Like hold (`Eaten_F`/`_B`), the closest pose Link himself has. One side effect:
    a young player really held by a Like Like also shows Ruto's pose on the copies (their copy's `Eaten`).
  - **The carried player's own screen: the exact limit.** Our own Link is the game's real `Player_Child`, on
    the game's own AS (changing that file would change solo play, which must stay untouched), and its pose is
    picked each frame by its own action AI (`Player.action.*`): no action of the player is "carried" (the
    nearest, `Player.action.reactionrestraintrideonenemy`, plays `Eaten` = the Like Like's hold). So on our own
    screen, while placed with input off, our Link plays what its own AI picks (likely its fall or float), not
    Ruto's pose. The probe (UNKNOWN 7): the lifted game logs `coop: lift: carried: our own Link plays '<X>'` once
    per change. If X is a fall or float, the native next step is asking our Link's action verb for
    `RestraintRideOnEnemy` through the game's own request-action-verb path (main+0xc08f0, as the downed crawl
    asks `Crawl`), which plays `Eaten`, and giving our own Link the carry AS only while that verb runs (a new
    probe: whether an actor's AS can be switched at run time); until that is proven, our own screen keeps the
    game's own pose.
- **Young only, on every side** (native-puppets-d, tests in lift_player_test.cpp and session_test.cpp):
  - Lifter: a copy counts only while its player's presence says child (`CopyView::young` from
    `CoopPuppet::Remote::young`, read every frame: the age can change, as at the Master Sword); an adult or an
    unknown age never asks and logs nothing. Asking or carrying when the copy stops being young: the lift ends
    (a `landed` place where it last was) and the director places the copy again.
  - Server: `on_lift` refuses a `LIFT_ASK` (`LIFT_ANSWER` 7 `not_young`, stat `lifts_not_young`) when the lifted
    player's last PLAYER_SAMPLE age is adult or not known (kept over scene changes, unset on a rejoin), and drops
    every `LIFT_PLACE` to such a player. Answers always go.
  - Lifted game: refuses `not_young` when its own Link is not child at answer time (the age read then, so an age
    change while the ask was on its way is caught); lifted, it frees itself (`LIFT_ANSWER not_young`) the frame
    its Link is not child: input back, no more places, its Link falls by its own character controller (the
    game's own drop).
  - Re-lift after landing, a throw (flying places), a scene change (an event or load frees it) and a rejoin (no
    age until a sample) all go through the same gates.
- **Protocol:** `LIFT_ASK` 535, `LIFT_ANSWER` 536, `LIFT_PLACE` 537, `LIFT_SHARE` 538 ([protocol.md](protocol.md),
  "Player lift"). The plan's `LIFT_THROW(velocity)` is not needed: the throw is the lifter's game's own (its
  lifter throws the copy), and the places go on as `flying` until the copy is at rest, then one `landed` place.
  So the lifted game never needs a velocity setter (the UNKNOWN above): its Link follows the thrown copy's path
  and is let go at the copy's resting place, where its own character controller takes over.
- **Rules** (`lichii/hook/coop_lift_player.h`, tests `tests/lichii/lift_player_test.cpp`):
  - Lifter: the copy is lifted when the game put it at least 0.3 m higher than the director drew it, within
    1.6 m of our Link, two frames running (the director reads the copy's place before each of its writes:
    `CoopPuppet::WatchGamePlaces`), and only while our own Link's main animation slot plays a lift or a carry
    (`OurLinkLifts`: a name with "lift" or "carry"; not read is no lift). So a copy raised by anything else (a
    rising platform under a `--puppet-body` copy, a step, our Link pushing under it) never asks; the log says
    "copy rose but our Link's animation '...' is no lift" once per such rise (the probe for UNKNOWN 6). It sends `LIFT_ASK` and the director stops placing, turning and setting that
    copy (`CoopPuppet::SetGameHolds`), so the game's own carry has it. No answer in 1.5 s, or a no: the director
    places it again and it is not asked again until the game lets it down. Taken: `LIFT_PLACE held` each frame
    with the copy's place as the game has it; when it is 0.6 m further out or lower than it was carried
    (thrown or set down) the places are `flying` until 8 frames with under 2 cm of movement (or 2 s), then
    `landed`, and the director places it from the samples again.
  - Lifted (decides): no while our Link is in an event (G1, our own), a load or a menu (`event`), dead or at
    life 0 (`down`), already lifted by another or lifting someone (`busy`), or the lifter's Link more than 3 m
    away, not known, or in another scene (`too far`; the server also answers `too far` itself when the lifter
    has no scene or no fresh sample position, or is not in the lifted player's scene, so a stray ask never takes
    a player's input away). Taken: each place is given to our real Link by the object walk the way copies are moved
    (place written, then the game's physics-aware setter main+0x4c6580 on it: `CoopObjects::PlaceOurLink`), and
    our Link's own input check (main+0x3025850, Hookshot wrap 18) answers "input off" for our Link only (the
    reading actor checked), as for an extra local player without a controller. The "off" is a time stamp each
    lift frame renews: older than 0.5 s it reads "on" again (`InputOffFresh`), so input can never stay off if
    the lift's frame stops being called. The lifted game frees itself
    (`LIFT_ANSWER freed`) on an event or death, when the places stop for 1 s (the lifter left), or after 15 s
    (it wriggles free; the game's own escape timer is UNKNOWN). The camera stays on our Link (nothing changes it).
    **Break free (row 312, puppet-body-4):** lifted, the player struggles as when an enemy holds Link: every new
    press of A, B, X or Y and every new push of the stick in a direction (`hos_coop_pad` -> `NotePad`, counted only
    while lifted) fills a meter that drains 2 a second; at 10 the lifted game frees itself (`LIFT_ANSWER freed`,
    log `our Link freed itself from P<n> (struggled free)`), its Link falls by its own character controller, and
    for 2 s every new ask is refused `busy` so the freed player is not caught again at once. About 1.3 s of
    mashing frees; a held button or a press every 0.6 s never does. Our numbers (the game's grab escape count is
    not read). The lifter's game gets `freed` as for every other free and lets the copy go (`Lifter::Drop`).
    Host test: `tests/lichii/lift_player_test.cpp` "row 312". Not proven in a game yet.
- **The game's functions used:** the lifter's own lift, carry and throw (`PlayerLifterParam` `LiftThrowParams` on
  the copy's `LiftableRef`); the physics-aware setter `ExecuteActorForceSetMtx` (vtable slot 0x218,
  main+0x4c6580) on our Link; the player's input-off check main+0x3025850 (wrap 18). No memory poke of the
  player's state; the only writes are the place before the setter, as for every copy.

Phase 3 UNKNOWNs (settled by the proof job in reports/native-puppets-c.md, screenshots on both games):

1. Whether the child and adult lifters lift a child copy with Ruto's `LiftableParam` at all (the copy has no
   Ruto AI; `SpecialDeal` 213340665, `IsResetBehaviorModeOnThrown` and `KeepSensorHitToParent` may need it), and
   that they play the Ruto set (`Lift_Up_Ruto`, `Pose_Ruto`) on it. Without
   `--puppet-body` the copy has no body to grab: the proof runs with both switches.
2. That the game's carry moves the copy's own place (+0x31c) so the read-back shows the rise (else the lift
   is not seen: then read the lifter's held actor instead, `ActorHolderRef`, UNKNOWN offsets).
3. That the game's throw moves a copy whose character controller is in `Free` (phase 1's body); if it does
   not fly, the copy rests at once and the lifted Link is let go at the lifter's hands (it then falls by its
   own controller): a set-down, not a throw.
4. That our Link placed through the setter each frame is not pulled back by its own controller between the
   walk's places (the held-bomb fall, reports/held-bomb.md); and that "input off" stops its own moves.
5. How the game lets a refused lift go (the lifter keeps carrying a copy the director now places): the next
   fix would be the lifter's own put-down action verb (the request-action-verb path main+0xc08f0, verb
   UNKNOWN).
6. Our Link's own animation names while it lifts and carries (AS slot 0). The gate takes any name with "lift"
   or "carry"; if the proof log shows "copy rose but our Link's animation 'X' is no lift" during a real lift,
   add X's family to `OurLinkLifts` (or read the lifter's `ActorHolderRef` instead, offsets UNKNOWN).
7. The carried pose: (a) that a young copy built with the carry AS plays Ruto's `Act_LiftRest_Wait` on Link's
   skeleton for `Eaten` (retargeted by bone name; the copy's heap holds the extra animation project), screenshot
   on the lifter's game; (b) a build log line `AS/CoopPuppet_Carry_Child.root.asb: ... Ruto's Act_LiftRest_Wait`
   (else the fallback note). The fallback is chosen at pack build time only: if the game fails to retarget
   Ruto's clip at run time (a bad pose, a T-pose, nothing played), no switch turns the carry AS off by itself;
   the cure is a pack built without it (the whole lift is off by default, `--puppet-liftable`). (c) what our own Link plays on its own screen while carried: the log line
   `carried: our own Link plays '<X>'`, and a screenshot of the lifted player's own screen.
8. That an adult copy really has no lift reaction (no `LiftableRef`: the lifter's button does nothing at it).

Proof job checks for the owner rule (add to the job in reports/native-puppets-c.md; screenshots of both games):
- A (adult, then young) lifts young B: both screens show B in A's hands with A in Ruto's carry pose
  (`Pose_Ruto`); A walks with B, puts B down, and throws B as Ruto is thrown. A's log has the Ruto lift names.
- Carried pose: see= A.png: B's copy in A's hands in Ruto's carried pose (sitting up, as Ruto sits on Link's
  hands), not Link's Like Like hold; see= B.png: B's own screen (what B's Link plays: B's log line
  `carried: our own Link plays '...'` names it). Both packs built with `--puppet-liftable`; the build log has the
  carry AS line.
- A tries to lift adult B: nothing happens on either screen (no lift, no message); A's log has no
  `coop: lift:` line for B; the server's `lifts_not_young` stays 0 (the ask is never sent).
- B ages while carried: in the game the age changes only in the Temple of Time's Master Sword event, which
  frees B first (the event gate). So A carries B into the Temple of Time and B takes or puts back the sword:
  B's log `freed itself from P1`, input back, B falls and lands by its own (screenshot B); A's log `landed`,
  A's carry ends; afterwards (B adult) A's lift at B does nothing. The `not_young` free itself is proven by
  the host tests (no game path changes the age outside that event).

The same "ask without our own state" and "unknown place" rules hold for the phase 2 bomb take: our game sends
`BOMB_TAKE` only when the copy's own animation says Lifted AND our Link's place is read and within 2.5 m of the
copy (`CoopBombShare::NearOurLink`); the server refuses a `BOMB_TAKE` (`not_a_bomb`) when the taker has no scene,
another scene than the bomb's, or no fresh sample position.

### Other liftable and shared things (named here, later phases)

- **Bomb flowers** (`Obj_BombFlower_*`, [sync-coverage.md](sync-coverage.md) P2): an unshared world object today
  (each player picks their own fruit; one picked and thrown in one game is not seen in the other). Not a made `Item_Bomb`,
  so phase 2's hand-over does not cover it. Later phase: share the plant's pick (a world object with an owner,
  as grass cuts and world breaks) and the picked bomb as phase 2's bomb.
- **Pots and rocks** (scene objects Link lifts and throws): their break is shared today ([world.md](world.md)); who
  carries one is not. Later phase, after phase 3 is proven: the lifted pot or rock as a shared object with an
  owner (phase 2's rules: one owner, the place sampled while carried, the break once at the owner's word).
- **Cuccos:** living AI actors; today each game runs its own. Later phase, with the pots: the Cucco as an
  enemy-like shared object (owner runs its AI, others copy), then lifting it is phase 2's take.
- Bombchus are not liftable (nothing to do).

## 5. PvP on real bodies

Today (reports/pvp.md, coop_pvp.*): the attacker's game finds a swing near the other Link (grass
blade points, 1.3 m) and sends `PVP_HIT` (242); the victim decides (kept off in events, 0.8 s mercy) and
replays a stand-in sword hit on its own Link (`HurtOurLink`, the hit replay), with a written life after
0.4 s if the game did not take it, then `PVP_HURT` (243) to all. Indoors (no grass) a sword hits nothing,
the damage is a fixed half heart, nobody can lock on, and a shield blocks nothing.

Phase 4 (design native-puppets-e, 2026-11-19; owner: "you should be able to target them, get your attacks
blocked, have real hit effects when hitting them, etc etc."): with PvP on, another player's copy is an
opponent the **game's own systems** handle. The attacker's game finds the hit with its own collision; the
**victim's game decides** with its real Link (guard, facing, invincible frames, life, death); every other
screen shows it. No number here is ours where the game has one.

### 5.0 Switches, and what PvP off means

- Session value: `PVP_MODE` (241, host only, off at every start), as today.
- Pack `--puppet-pvp` (implies `--puppet-body`): the copy gets the pieces in 5.1. Without it the pack is byte for
  byte as before (the pack test checks it).
- Glue `LICHII_COOP_PVP_NATIVE=1` (off by default) in the module; the game says so with `PVP_NATIVE` (542).
  Off: today's PvP (242/243) is untouched.
- **PvP off: the copy has no sensors at all** (review r1, B). The pack makes two actors per puppet: today's
  `CoopPuppet_*` (unchanged, no sensor, no Life, no attention sender) and `CoopPuppet_*_Pvp` (the pieces in 5.1).
  The director makes the plain one while PvP is off and the `_Pvp` one only while it is on and both games are
  `PVP_NATIVE`; a mode change re-makes the copies (as a look change does today). So with PvP off nothing can lock
  on to a copy or hit it because nothing is there to hit, not because a toggle worked. The runtime toggles in
  5.1 only cover the moments between a mode change and the re-make (and a downed player).
- **With lift (phase 3) in the same pack** (merge of integrate-0116, 2026-11-19): both switches together give
  each young puppet AND its `_Pvp` twin Ruto's `LiftableRef` and the carried pose (`asb_carry`), so a young
  player stays liftable with PvP on; the adult puppet and its `_Pvp` twin get the PvP pieces only and are never
  liftable (byte for byte the PvP-only adult copy). Tests: `test_pack_carry_and_pvp_together`,
  `test_build_liftable_and_pvp_together` (tools/puppet_pack/tests/test_puppet_pack.py).
- Glue on but the pack has no `_Pvp` actors (manifest without `"pvp": true`, an old pack): the game says
  `PVP_NATIVE 0` and keeps today's 242/243. A native victim that gets an old `PVP_HIT` (242) from a non-native
  attacker takes it by today's path, unchanged.
- Solo and co-op idle have no copy at all, so nothing changes.

### 5.1 The copy's pieces (`--puppet-pvp`, all cut from the player's own files)

| Piece | From (romfs) | Why |
|---|---|---|
| `SensorHitRef` | `SensorHitParam/Player` (`TgHitInfoBufferSize 8`, `IsThroughSameAffiliation false`: a Player-affiliation sword hits a Player-affiliation body) | the game records hits on the copy |
| Sensor `Tgt` (`PlayerReceipt`) with its shapes | `Phive/ControllerSetParam/Player` `CColSensorNamePathAry` (`Atk` receipt and `ForArea` left out) | the attacker's sword, shot or blast touches the copy. The `Atk` receipt (`PlayerAttackReceipt`, sword meets sword) is left out until a later phase syncs clashes: a clash would give the attacker the game's recoil while the victim knows nothing |
| `GameBalance/AttackParam/Player(_Child)` | the player's own rows | only so a hit info whose attacker is this copy resolves in the victim's game (5.3 A, option i, if U2 picks it); the copy has no active `Atk` shape, so it never hits anything by itself |
| `LifeRef` + `ReactionRef` | `LifeParam/Player` (DamageParameters/Player: `GuardableAngleMaxDeg 120`, every `DamageRates` 1.0), `Reaction/Player` | the attacker's game runs its own hit step on the copy (hit-stop, flash, sparks, the Reaction's `Hit` sound) and writes the incoming hit record (Life +0x240) that is read and sent |
| `AttentionSenderRef` | a new param whose `ClientList` is the copy's own `LockOn` and `AutoAim` clients, cut from `Attention/ClientParam/EnemyCommon_LockOn` / `_AutoAim` (`ClientName` LockOn / AutoAim; `AttentionType` 3016662870 = murmur3 "LockOn"; `CheckAffiliation Player`: who may lock on) | Z-targeting and item aim assist find the copy as they find an enemy |
| `TargetRef` | `TargetParam/Player` | the target slot the attention and the camera look at |

Never on the copy (alt-b's rule): `PlayerRef`, `ActorNameRef Player`, `PlayerCameraTargetRef`, `DeathRef`,
`ConditionRef`, `ChemicalRef`, any active `Atk` shape (the copy runs no attack of its own: its sword never hits
anything in our game; its owner's hits come from its owner's game as messages).

**Only our own attacks reach the copy** (review M3: an enemy's slash must not flash a copy whose owner took
nothing). The copy's `SensorHitParam` keeps the player's, with `DamageWhiteList` set to the Player affiliation if
U1 shows that list filters by the attacker's affiliation; else its `Tgt` sensor gets a contact layer that enemy
`Atk` shapes do not hit (U1 reads which layer the player's own `Atk` and the enemies' use). Until one of them is
shown, an enemy's hit on a `_Pvp` copy is a known false flash, written in the proof list, and never sent.

The copy's Life is **invincible from its creation**, through the game's own call: `OneShotLifeSetInvincible`'s
IsNoReaction **0** on-function, main+0xb34800 (Life, 1, &our key 0xC0DB0D02; the reacting list Life +0x1c28), the
sibling of the main+0x8d75e0 no-reaction call down-crawl uses (coop_dbno_body.h read both; r2 named 0x8d75e0 here,
which is the no-reaction list: corrected in glue part 2 r2). Built: coop_objects.cpp `PvpShieldCaller` (caller
order 67), once per live `_Pvp` actor and Life component (`CopyShield`, 3 tries, logged), so nothing falls on a copy
in any game; the third games' replay passes amount 0 too. UNKNOWN U1: the reaction still shows.

Runtime on/off per copy (at its creation and on every `PVP_MODE` change): attention sender valid = PvP on
(`OneShotAttentionSenderChangeValid`'s run), sensors hit = PvP on (`OneShotChangeForceNoHitModeRigidBody`'s run).
Both are AI nodes of the game's own; their runs are found as down-crawl found OneShotLifeSetInvincible's (the
node's vtable, its setup registering the argument names). UNKNOWN U3, which also checks whether the no-hit mode
reaches the copy's equipment actors (its shield and sword have their own `Tgt` sensors, 5.3): if not, the
director toggles them too. A downed or dead player's copy is made invalid and no-hit too (nobody locks on to a
downed player).

### 5.2 Point 1: targeting

The game's lock-on is our Link's `AttentionReceiverRef` (`AttentionReceiverParam/Player`: `HasLockOnState`,
`IsCheckScreen`) choosing among attention senders. With the copy's `LockOn` client valid, Z picks the copy as it
picks an enemy, and the game's own reticle (`Cursor_LockOn`), lock-on camera and strafing
(`ExecutePlayerLockOnMove` / `ExecutePlayerLockOnWait`) follow; the `AutoAim` client gives the bow, slingshot,
hookshot and boomerang their aim assist. Nothing of ours draws or moves. PvP off: the client is invalid, so a copy
is never a target (today). Left out: Navi's enemy hint (`NaviAttentionParam`, which names an enemy's hint event).
UNKNOWN U4.

### 5.3 Point 2: blocking (decided on the victim's game)

**Victim.** The hit goes into the victim's real Link the way the game delivers any hit, so the game's own guard
check runs with Link's real guard state, facing and shield: the angle (`GuardableAngleMaxDeg 120`), the shield
(`ShieldParam`: `BadConditionAttackGuard` per damage type, e.g. the Deku Shield and fire) and the weapon's
`GuardBreakPower` (WeaponParam: Kokiri Sword 2, Master Sword 40, Biggoron/Giant's 10, Deku Stick 10). Guarded:
Link plays the game's own guard push (`reactionguard`, `reactioncrouchguard`; `reactionguardbreak` /
`reactionguardfaile` when it fails), no life falls. Two ways in, A preferred:
- **A. The sensor path.** Put the hit into Link's `SensorHit` Tg hit info (the buffer the game fills from its
  collision, 8 entries) before the game's step that turns hit infos into guard, damage, reaction and invincible
  frames. Everything after that is the game's. UNKNOWN U2.
  **The attacker in the hit info (review H1).** A hit info points at its attacking side (actor, sensor, and
  through them the attack's AttackParam row, the weapon's `BaseAttack`, hit-stop and knockback). In the victim's
  game the attacker is its copy of the striker, which runs no attack. U2 reads (a) which hit-info words are
  pointers and to what, (b) what the consumer reads through each, and probes with a hit info whose attacker is
  **B's copy of A** carrying a Kokiri Sword slash's kind words. Then one of two, written down with U2's reason:
  (i) the `_Pvp` copy carries the player's AttackParam rows (5.1) and its real weapon actor from
  `EquipmentUserRef`, its `Atk` shapes no-hit, so every pointer is the game's own and resolves; or (ii) the hit
  info points at our own player's rows for the strike's kind (attack row by kind, the weapon by the copy's
  equipped weapon), the copy only as the place. (i) is preferred: nothing is built by us.
  **A copy that is gone** (scene change, despawn, re-made by a mode change) between the strike and the step: the
  strike is answered `kept off`. The hit info is put in on the game thread right before the consumer runs in the
  same step (as the Mover's hit replay is), and only while the copy's actor is the one the director holds, so it
  never points at a freed actor.
- **B. The life path (fallback, only if U2 finds no safe way in).** Today's hit replay into Life +0x240 (it
  already plays knockback and takes the life), behind a guard check made only of the game's own data: Link
  guarding = his main AS / verb is a guard one (U5), the attacker within 120 degrees of his facing, the shield not
  failing for that kind; guarded: the game's guard reaction verb (ExecuteBSASetActionVerb) and the shield's guard
  effect through the game's own XLink emit (docs/spawns.md, main+0x295b740's way). The reviewer checks that B is
  used only with U2's reason written down.

**Attacker.** Its sword must clang on the shield on its own screen, at once. The copy wears the real shield actor
(`EquipmentUserRef`), and `Weapon_Shield_*` has its own `Tgt` sensor (`WeaponShieldMetal`:
`WeaponShieldTgtMetal`). If the attacker's game judges a hit on that shield of a copy in the guard pose as
guarded, the game itself plays the clang, sparks and the attacker's recoil (`reactionattackcollide`,
`guardjustrepelled` on the attacker's real Link). UNKNOWN U6. If not, the attacker's Link plays them when the
victim's `PVP_RESULT guarded` comes (one round trip late), as in B.
Either way **the victim's result decides the damage**; a clang on one screen with a hit on the other is logged
(`pvp: strike N: attacker saw a guard, victim took it`) and counted in the proof. The same holds both ways: our
hit on a guarding copy, and the copy's owner's hit on our guarding Link.

### 5.4 Point 3: real hit effects, damage from the game

- **Attacker's screen.** The copy's own Life step (5.1): hit-stop, flash, sparks, sound, at the contact.
  Knockback and the hurt pose come from the victim by pose sync.
- **Victim's screen.** Link takes the hit by path A (or B): knockback, hurt reaction, hearts, the game's sound and
  effects, all native.
- **Every other screen.** `PVP_RESULT` (541) goes to every game in the scene with the hit's kind words. Each one
  replays that kind into its own copy of the victim (invincible Life: flash, sparks and sound, nothing taken), as
  protocol 14 does for enemy copies (objects.md, "The real hit"); pose sync shows the knockback.
- **Damage.** Never ours. The attacker's game computed the hit on the copy (the record's amount +0x224 and kind
  words +0x250..+0x27c, `ReadHitRecord`) from the weapon's `BaseAttack` (WeaponParam: Kokiri Sword 1, Master
  Sword 2, Biggoron/Giant's Knife 4, Megaton Hammer 3, Deku Stick 2), the attack's row in
  `GameBalance/AttackParam/Player(_Child)` (`DamageIntensity` Weak / Strong: spin and jump attacks are Strong,
  `DamageType`, `DamageMultiplier`, `HitStopType`, `KnockbackIntensity`) and the target's `DamageRates`. Path A:
  the victim's game counts it again from the hit info, and its own number is what Link loses. Path B (review M2):
  **the victim works the amount out itself** from the strike's kind words with its own copy of the same tables:
  the striker's equipped weapon as the victim's game knows it (`PLAYER_EQUIP`, the copy's `EquipmentUserRef`
  weapon) and its `BaseAttack`, the AttackParam row the kind names (intensity, multiplier) and Link's
  `DamageRates`. The attacker's amount is only compared: a difference is logged
  (`pvp: strike N: attacker said A, our game counts B`), never used. A kind the tables do not name is answered
  `kept off`. Path B is used only until U2 settles path A.
- **Items.** Arrows (their elements in `DamageElements`: fire, ice, light), seeds, bombs and bombchus (their
  blast's own Atk), nuts, the hookshot, the boomerang, Din's Fire: their own shapes touch the copy's `Tgt` in the
  attacker's game and are read the same way. A no-life hit (a nut's, boomerang's or hookshot's stun) has amount 0
  and is sent too: the victim's game does what the game does with that kind on Link. UNKNOWN U7. The hookshot never
  pulls a player (the game pulls only actors made for it; a copy is not one).
- **Which hits are sent.** Only a record whose attacker is our Link, or a shot or blast our game made (CoopSpawn's
  own list). An enemy's hit on a copy is never sent, and is kept off the copy's `Tgt` (5.1, U1): the owner
  game's enemies decide (section 6, "Enemies").

### 5.5 Point 4: the rest

- **Invincible frames: the game's own.** After a hit, Link's Life is invincible for the game's time
  (`OneShotLifeReserveInvincibility`); a hit landing then is refused by the game itself and answered
  `invincible`. Path A uses no time of ours. Path B (review L2): the Life +0x240 replay may not reserve them, so
  path B keeps today's 0.8 s `kMercyUs` until U7 shows the replay sets the game's invincibility (Life's
  invincible list or `OneShotLifeReserveInvincibility`'s timer moving after a replayed hit). The written-life
  fallback is never used on either path.
- **Death: no PvP path of its own.** The hit goes through Link's own Life, so a PvP kill is whatever an enemy's kill
  is in that session: the co-op downed path when it is on (coop_dbno: the game's Crawl verb 234, the downed
  invincibility main+0x8d75e0, revive by a teammate; it already counts an up rival as a threat with PvP on), else
  the game's own death and Game Over. Why: one rule for every death, both built on the game's own verbs, and the
  downed path is what the session already chose; a separate "PvP death" would be ours. A downed or dead player
  refuses every strike (`kept off`), and its copy is no-hit and invalid.
- **Shield break and burn: the game's** (path A: `GuardBreakPower` against the shield, `BadConditionAttackGuard`;
  path B keeps only what U2 and U5 read).
- **No friendly fire when PvP is off**: sensors no-hit and attention invalid in every game (5.1), and the server
  drops every `PVP_STRIKE` while off (as it drops 242 today).

### 5.6 Who decides, lag and cheating

- The victim decides. A strike names `seen_us`: the server-clock time of the victim's own sample the copy showed
  when it was hit (the director knows which sample it drew; samples carry server-clock times, protocol.md). No
  clock of the attacker's is trusted.
- **Time.** Taken only if `0 <= now - seen_us <= 550 ms` (`kStrikeLateUs`). Between the two lie one trip of the
  victim's sample to the attacker (both games' way to the server), the attacker's display delay of that copy
  (100 ms or more) and one trip of the strike back: with both games at a 200 ms ping that is 200 + 100 + 200,
  plus 50 ms of jitter. (The first value, 350 ms, answered `late` to every strike between two games at 150 ms
  pings: fable-puppet-pvp, 2026-11-25.) A `seen_us` after now is `late` too (never wrapped as u64). So the
  victim is hit where the attacker saw it up to 0.55 s ago, also when it has moved on since: the known price of
  judging what the attacker saw.
- **The victim's place.** The victim keeps a 1 s ring of its own places (each frame). The hit's place must be
  within 0.5 m (sample interpolation and jitter) plus the body (1.0 m level, -0.4 to 2.2 m high: today's body
  numbers) of the ring's place at `seen_us` (review L1: no `speed x elapsed` term, which would widen it for a
  rolling victim, just when lag hurts the victim). Else `out of reach`.
- **The attacker's place (review M1).** The victim also keeps a 1 s ring of its own copy of each player (the
  samples it received, also those its copy does not draw yet). The strike's `from` is the attacker's place when
  it HIT, which is later than `seen_us` by its display delay and a trip (a running attacker is 1.4 m on after
  200 ms), so it must be within 0.5 m of the **path** of that ring from `seen_us` - 100 ms on (its places and
  the lines between neighbours at most 250 ms apart and no farther apart than 30 m/s covers in that time plus
  0.5 m: a gap is no path and a warp is none, however close in time its two samples are;
  `PlaceRing::NearestSince`. The 30 m/s is a bound we chose, Link's top speed is not read: a faster honest move
  is checked at its samples only), and by source:
  - sword or body: the victim's ring place of the attacker within the weapon's reach of the hit place, the reach
    from the game's data (`WeaponParam.Range` of the weapon the attacker has equipped as `PLAYER_EQUIP` says: the
    Deku Stick's 1.8 m, ...; a weapon without one: today's 1.3 m) + 0.5 m; never less than 1.3 m;
  - shot or blast: the strike names the shot's or blast's object id; that object must be one the victim's game has
    (spawn sync: a made object of the attacker), and its last place within 1.5 m (shot) or 3 m (blast, today's bomb
    reach) of the hit place.
  Else `out of reach`.
- **Replays and floods.** A strike's `seq` must be newer than the last one taken from that attacker; an old or
  repeated one is dropped (no result). The server lets through at most 10 strikes a second per attacker (the
  game's quickest: a spin's several hits in one swing) and drops the rest (`stats.pvp_strikes_flood`).
- Kept off as today: an event, a menu, a load, pause, another scene, life not read, downed or dead, the attacker in
  one of those, or the victim's copy of the attacker not known. Each still gets its answer (`kept off`) and
  spends its seq. The rings are one scene's: a load clears them (a room change mid-swing), and a copy's ring
  takes only samples from the one that set its new scene on (`SampleOfScene`: the last scene's samples that came
  in the same tick are still queued). A player number that left starts again with no scene and no fall.
- **Both hit at once.** Each game judges the strike on its own Link, so two swings in one moment are a trade. An
  attacker that FELL (its status went from here to dead) at most 200 ms ago (`kTradeGraceUs`) is not kept off for
  it: its strike was swung before, and only the news overtook it. Only a fall has that grace: a pause, a load, a
  cutscene or a menu keeps a strike off at once, and so does dead from one of those. One grace per fall
  (`FallWatch`): a status flapping dead / not dead inside a second does not open it again.
- **A shield raised just before (path B).** Guarded when our Link guards toward the attacker now, or did at
  `seen_us` (`GuardRing`, a second of frames): never a hit through a shield either screen shows raised. Lag is
  paid by the attacker.
- So what the attacker saw is what it hit (within the ring), a laggy attacker cannot hit someone who got away, and
  a game that makes strikes up can neither hit from across the room nor choose the damage.

### 5.7 Protocol (types 540 to 544, claimed in COORD-LOG, 543 and 544 kept; no bump: off by default, older games ignore them)

| Type | Name | Sender | Channel | Body |
|---|---|---|---|---|
| 540 | `PVP_STRIKE` | both | ownership | `u8` player field (the attacker), `u8` victim (1 to 16, not the attacker), `u32` seq, `u64` seen_us, 11 x `u32` the hit's kind words (record +0x250 up to +0x27c), `s32` amount (0 to 80, the attacker game's: compared, never used), `u8` source (1 sword or body, 2 shot, 3 blast, 4 other; others refused), object id (10 bytes: a made object for a shot or blast, all zero for the others; others refused), 3 x `f32` hit place, 3 x `f32` the copy's place, 3 x `f32` the attacker's place (all finite). To the victim only |
| 541 | `PVP_RESULT` | both | ownership | `u8` player field (the victim), `u8` attacker (1 to 16, not the victim), `u32` seq, `u8` result (0 taken, 1 guarded, 2 guard broken, 3 invincible, 4 late, 5 out of reach, 6 kept off; others refused), `s32` amount taken (0 to 80, the victim game's own), `s32` life left (0 to 80), 11 x `u32` kind words, 3 x `f32` hit place (finite). To every game in the victim's scene |
| 542 | `PVP_NATIVE` | both | ownership | `u8` player field, `u8` on (0 or 1; others refused). Its game runs this path (switch on and the pack has `_Pvp` copies); sent after joining and every 5 s, relayed to all |

Server (`on_pvp_strike`): drops a strike while PvP is off, to or from a game not `PVP_NATIVE`, between two scenes,
from a player with no fresh place (5 s, `place_known_in`, as lifts), or past 10 a second from one attacker; relays it
to the victim only. A result is relayed only from the victim a strike in flight named (by seq), to its scene. A
native attacker sends the old `PVP_HIT` (242) to a victim that is not native, so mixed sessions keep today's PvP.
The victim still sends `PVP_HURT` (243) for the chat line.

### 5.8 UNKNOWNs and the probe that settles each (one module-only research run, offline job)

| # | Unknown | Probe |
|---|---|---|
| U1 | An invincible Life (IsNoReaction 0) on the copy still writes the incoming record (+0x240, last hit id +0x1b18) and plays flash, sparks, sound and hit-stop. And (review M3) what an **enemy's** hit on the copy does, and whether `SensorHitParam.DamageWhiteList` (or the `Tgt` contact layer) keeps enemy hits off it | A's sword on B's copy indoors, then a Deku Baba's bite on B's copy in A's game; log the copy's last hit id and record each frame; screenshot A at each contact; read the layers the player's and the enemies' `Atk` use |
| U2 | Path A's way in: the function that drains `SensorHit`'s Tg hit info into guard, damage and reaction, and whether a hit info can be added before it. (Review H1) which hit-info words are pointers and to what, and what the consumer reads through each | Read main from the `TgHitInfoBufferSize` reader's xrefs to the consumer and its reads; then one run adding a hit info whose attacker is **B's copy of A** (Kokiri Sword slash kind words) to B's Link's buffer, shield up and down; then option (i) or (ii) of 5.3 written down |
| U3 | The runs of `OneShotAttentionSenderChangeValid` and `OneShotChangeForceNoHitModeRigidBody`, and (review M4) whether no-hit reaches the copy's shield and sword actors; whether **today's** plain copy's shield already clangs (its equipment `Tgt`) | Read main (node vtables, setup names), then toggle them on a copy in the run; A slashes the plain copy's shield with PvP off, screenshot A |
| U4 | A copy with `AttentionSenderRef` + `TargetRef` and no AI is chosen by Z and by AutoAim; the lock lets go with no crash when the copy goes (scene change, PvP-off re-make, downed) | Run: Z at B's copy, screenshot the reticle and the strafe; aim the bow, slingshot, hookshot and boomerang at it; then turn PvP off, and down B, while A holds Z |
| U5 | Path B only: the guard verb values and the guard AS names | Every scheduler module's selector cases (as down-crawl's verb list) |
| U6 | A hit on a copy's real shield actor in the guard pose is judged guarded by the attacker's game (clang, recoil) | Run: B holds the shield up toward A; A slashes; screenshot A |
| U7 | What the game does with player-only kinds on Link (sword, nut, boomerang, hookshot, arrows, Din's Fire), and whether path B's replay sets the game's invincibility | One strike of each kind in the research run; screenshot B; log life, the reaction AS name and Link's invincible state per kind |

### 5.9 Proof scenarios (offline 2-game job, screenshots of both games, no PASS from logs)

Both kits: pack `--puppet-body --puppet-pvp`, `LICHII_COOP_PVP_NATIVE=1`, `/pvp on`, indoors (no grass). Every
[shot] is a screenshot of the game named; scenarios 2 to 5 run both ways (A on B, then B on A).
1. A Z-targets B's copy: [shot A] reticle on B, A strafes around B; bow, slingshot, hookshot, boomerang aim at B.
2. A's Kokiri Sword on B: [shot A] at contact (flash, sparks); five slashes, the copy still reacts to the fifth
   [shot A] (its Life invincible: A's log has `the game's invincibility on`); [shot B] B knocked back, hearts lost as B's game
   counts them; [shot A] B's knockback by pose sync. Then B on A the same.
3. B shields facing A, A slashes: [shot A] clang and recoil, [shot B] guard push, no hearts lost. B faces away: hit.
4. Two slashes inside B's invincible frames: the second answered `invincible`, one fall only [shot B].
5. Spin and jump attack, a Deku Stick, an arrow, a fire arrow, a bomb, a nut, the hookshot: each [shot A+B], life
   and result logged per kind.
6. Guard break and burn: the Master Sword on a guarding Deku Shield; a fire arrow on a guarding Deku Shield
   [shot A+B], as the game does it.
7. Lag: the lane's added delay (or a relay hop) on, 100 ms each way per game (200 ms pings): A's slash on its
   copy of B is `taken` though B rolled away right after the swing [shot A+B]; a strike A makes up from a place
   it never stood is `out of reach` [shot B]; with 150 ms each way per game: `late`, no hit [shot B].
8. PvP turned off mid-fight while A holds Z on B: the lock lets go, the copy is re-made plain, the next slash
   passes through [shot A+B]; nothing clangs on B's shield.
9. No double damage: B swings at A's real Link; A loses exactly what A's own result says, once [shot A].
10. B brought to 0: [shot B] B downed (or Game Over with downed off); A cannot lock on to downed B [shot A].
11. Mixed: an enemy and PvP together; the enemy hits B's copy in A's game: B's life unchanged [shot B], and the
    false-flash state of 5.1 noted [shot A].
12. Mixed session: a native game and a game without the switch: today's PvP (242/243) both ways [shot A+B].
13. Regression, PvP off, both kits with the new pack: co-op walk, nothing locks on, nothing clangs, phases 2 and 3
    (bomb take, lift) still work [shot A+B]; and a third game C watching A hit B with PvP on sees the hit effect
    on B's copy [shot C].
14. (Review LOW 2) A lifts a bomb while B Z-targets A's `_Pvp` copy; the host turns PvP off: [shot B] shows
    whether the reticle stays until A throws; after the throw the copy is plain and the lock is gone [shot B].

Host tests in step 2 (no run): forged strikes refused (attacker far from its ring place, hit place off the victim's
ring, a `seen_us` in the future or too old, a repeated seq, a shot naming no object); path B's amount is the
victim's own, never the attacker's 80; the server's 10 a second limit; PvP off or a non-native game: no strike
relayed.

### 5.10 Build order (step 2, behind the switches, host tests)

1. Protocol 540 to 542 (codec, tests), server `on_pvp_strike` (mode, native, scene, fresh place, 10 a second)
   + result relay by seq + `PVP_NATIVE` (tests).
2. Pure rules `coop_pvp_native.h`: the victim's two rings (its own places, its copies' places) and the checks of
   5.6 (time, the victim's place, the attacker's place and reach by source, the object of a shot or blast, seq),
   path B's amount from the victim's own tables (weapon `BaseAttack`, kind row, `DamageRates`) and its mercy,
   which records are ours to send, which copy actor (plain or `_Pvp`) the mode asks for (tests, forged strikes
   among them).
3. Pack `--puppet-pvp`: the `_Pvp` actor per puppet (pack test: components present; byte for byte without it).
4. Glue: the director makes plain or `_Pvp` copies by the mode; read the copy's record (the protocol 14 reader),
   send strikes; victim path B first (today's replay behind the guard check, which reads the guard from our Link's
   main AS name, any case containing "guard", as phase 3 reads "lift", and logs each name once for U5), path A
   when U2 is read; third-game replay on results.
Every game address U2/U3/U5 need is read before its glue lands; until then the glue logs and does nothing.

**Built (step 2, native-puppets-pvp, host tests only, not run in a game):**
- 1 done: codec 540 to 542; server `on_pvp_strike` / `on_pvp_result` (PvP on, both native, one scene, fresh place,
  own object, 10 a second, a result once for a strike in flight, an attacker that leaves takes its strikes with it).
- 2 done: `lichii/hook/coop_pvp_native.h` (tests/lichii/pvp_native_test.cpp): rings, the 5.6 checks, `SeqGate`
  (forgets an attacker that leaves or joins again: review LOW 1), path B's amount and mercy, `StrikeSource`,
  `WantedCopy`, and `RemakeNow`: a copy lifted by our Link or whose player lifts or carries something keeps its
  plain or `_Pvp` actor until that ends (review LOW 2; read from `GameHolds()` and the copy's animation, no lift
  file touched). A copy in the lift's carried pose (`CoopLiftPlayer::kCarriedPose`: its player is carried by
  someone) counts as busy too (`CopyBusy`, tests/lichii/puppet_test.cpp).
- 3 done: `--puppet-pvp` (tools/puppet_pack/build.py `add_pvp`, `pvp_sensors`, `attention_clients`; checked on the
  game's romfs: `Tgt` / `Body_Tgt` only; the plain puppets byte for byte as before).
- 4 done (glue part 1 + 2; nothing of it runs without `LICHII_COOP_PVP_NATIVE=1`):
  - `PVP_NATIVE` (glue on and a pvp pack, every 5 s), the other games' `PVP_NATIVE`, the director's `_Pvp` mask
    (`CoopPuppet::SetPvpCopies`) by `WantedCopy`; a lifted or carrying copy keeps its actor until that ends.
  - **Attacker.** The director publishes each live `_Pvp` copy's actor (`Director::LivePvpActor`). The object
    walk (coop_objects.cpp `ReadPvpCopies`, beside `GivePvp`) reads its Life component's last hit id (+0x1b18);
    one that moved, on the same actor, not to our own third-game replay (`CopyHitWatch`), hands the record (kind
    words +0x250..+0x27c, hit place, attacker place, +0x228 amount) to the glue. `MakeStrike` sends it only when
    its attacker is our Link (`StrikeSource`), with `seen_us` / `copy_at` = presence's shown pose of the victim
    this frame (its sample's server time). An enemy's hit is not sent (counted). **Our shots and blasts are not
    sent yet**: the walk cannot name the shot's made object (CoopSpawn keeps it by address, not shared), and a
    strike without one is refused by the victim anyway; today's PvP gives shots meanwhile.
  - **Victim, path B.** Our own ring (each frame, server time) and a ring per player (each new shown sample);
    `DecidePathB` with the attacker's weapon from `PLAYER_EQUIP` (`WeaponOf`), guarding = our main AS has "guard"
    (each name logged once, U5) and the attacker within 60 degrees of our facing (`GuardableAngleMaxDeg 120` taken
    as the whole cone until U5); `FirstTables`: the BaseAttack values of 5.4. A taken strike goes to
    `CoopObjects::HurtOurLinkNative`: the game's own hit replay on our Link with the strike's kind words, **never
    the life write** (a hit the game does not take in 0.4 s stays untaken; with `LICHII_COOP_PVP_REPLAY=0` none
    is given). A no-life kind (amount 0) is the game's hit only. `PVP_RESULT` is sent, and `PVP_HURT` for the chat
    line when life was taken.
  - **The kinds' multipliers are NOT filled** (their keys are the U2/U7 read), so every strike is answered `kept
    off` and takes nothing until then (`PathBLive` false). So that PvP between two native games still works,
    today's `PVP_HIT` keeps hurting until path B is live; from then on the victim drops 242 from a player whose
    strikes it judges (`TakesStrikesFrom`, proof 9: never twice).
  - **Every other game.** A `PVP_RESULT` taken or guard broken that is not about us is replayed on our `_Pvp`
    copy of the victim (`CoopObjects::ReplayOnCopy`, the protocol 14 replay: flash, sparks, sound on its invincible
    Life). The attacker logs a `guarded` result (U6: the clang is its own game's on the copy's shield, if any).
  - Review M1 (r2): every live `_Pvp` copy's Life is made invincible once through the game's own call (5.1);
    paths that reach a copy's Life checked: our own hits and enemies' (invincible), the third-game replay (amount 0,
    invincible), the life writes in coop_objects.cpp (only shared objects and our own Link, never a puppet).
  - Review LOW 1: the seq gate forgets a player on `PLAYER_LEFT` and `PLAYER_JOINED`; a new connection resets the
    gate, the rings, the mercy and our own strike count.
  - Review LOW 2: a copy held while carrying when PvP is turned off keeps its `_Pvp` actor (and so its lock-on)
    until the carry ends. No strike comes of it (the walk reads copies only while the mask has PvP on, and the
    server drops strikes with PvP off). Turning its attention off at once waits for U3's toggle; proof 14 checks
    the gap.
  - Left: U3's runtime toggles; path A (U2); shots and blasts (the made object's id from CoopSpawn); the kinds'
    table (U2/U7); WeaponParam.Range (the 1.3 m floor until then).

## 6. Risks

- **The game's physics fights the sync.** A character body under gravity falls between the hook's places
  (the held-bomb bug: the setter keeps the body's fall speed, reports/held-bomb.md). Answer: the copy's
  character controller uses the "Free" move state, the one the game gives NPCs placed on seats and
  walls (romfs `Phive/CharacterControllerParam/Npc_Kokiri_MC_ThroughGround`: `DefaultMoveState: Free`;
  `Npc_Kokiri_MC`: `OnGround.IsEnable: false`). UNKNOWN: that a Free body still blocks Link and carries
  him when he stands on it. Probe: phase 1's offline proof.
- **The copy's body pushes this game's Link**, or Link pushes the copy off its place: the copy is put back
  every frame, so a push shows as a jitter. The proof measures the copy's place read back against the
  sample (CoopSnap::Meter, issue 194 "snaps logged").
- **Two Links confuse the game** (option A only): camera, HUD, save, scene changes, events. Option C keeps
  the copy free of `PlayerRef`, `ActorNameRef Player` and the camera target, as today.
- **Null reads.** A copy with more components but not `PlayerRef` can meet code that reads its wearer's
  Player component (crash 2, main+0x2dceef8 reads +0x7d8, hs-puppets-alt-b). The module's lazy null page
  (hookshot/src/coop_null_page.h) stays armed; a new site is a log line (`unguarded null read`).
- **Enemies.** A copy with a body and sensors can be hit by this game's enemies. Enemy hits on a copy
  must be dropped (the owner game's enemies decide), or forwarded only as perception. Phase 4 filters
  by attacker affiliation.
- **Heap.** New files in the pack need size-table entries (issue 111: a short entry drops every create).
  The pack builder sizes them as the player files they are cut from (hs-puppets-alt-b's rule).

## 7. Phases

Sizes: S = under a day of one worker, M = 1 to 3 days, L = more. Every proof is an offline 2-game job
(lanes, docs/testing.md) with a screenshot on each game (no pass from log lines alone).

| # | Phase | Size | What | Offline proof (2 games) |
|---|---|---|---|---|
| 0 | Facts | S | One module-only research run: lift grades, the Free body test, stash path, throw velocity setter (the UNKNOWNs above) | Logs + screenshots of each probe; no product change |
| 1 | Body | S-M | Pack switch `--puppet-body` (built, below): the copy gets Link's own shapes and character body in Free move state, plus his cloth; no sensors, no ragdoll | A walks into B's copy: A stops (screenshot both); A jumps on the copy: A stands on it (A's y = copy's top, screenshot); B walks away: the copy's place read back = samples (no jitter lines). Same with an adult copy. Solo and idle unchanged (pack built without the switch is byte for byte today's) |
| 2 | Bomb hand-over | M | Bomb as a shared object (owner, epoch, fuse left, held by); lift of a copy asks for it; stash through the pouch path | A puts down a lit bomb, B lifts it: one explosion in both games at B's hands (screenshots both, the same frame window); A puts down an unlit bomb, B lifts and stashes: B's count +1, A's unchanged, the bomb gone in both |
| 3 | Lift a young player (owner rule: young only, Ruto's carry) | M-L | Ruto's `LiftableRef` on the child copies only; `LIFT_ASK` / `LIFT_ANSWER` / `LIFT_PLACE` messages (the throw is `LIFT_PLACE flying`); young-only gates on the lifter, the server (`not_young`) and the lifted game; the lifted game places its real Link, its copies play the carried pose (built, not run: "Phase 3 status") | A lifts young B: both screens show B in A's hands with A in Ruto's carry (screenshot both), A walks, B follows; A throws: B lands where A's screen shows (within 1 m), both screenshots; A tries adult B: nothing happens, no message; B ages mid-carry: safe drop; B in a talk: the lift is refused |
| 4 | PvP on real bodies | M-L | Section 5 (design native-puppets-e): copy gets `Tgt`/`Atk` receipt sensors, `SensorHitRef`, an invincible `LifeRef` + `ReactionRef`, `AttentionSenderRef` (LockOn, AutoAim); the copy's hit record is sent as `PVP_STRIKE`; the victim's game decides (sensor path A or life path B, rewind check), `PVP_RESULT` to the scene; enemy hits on copies dropped | Section 5.9 (eight scenarios: lock-on, sword hit, guard, invincible frames, every weapon and item, downed, enemy and PvP off, a third game) |
| 5 | Option A track | L | native-player steps 2-5 (input wraps, correction) as a separate switch | As native-player.md |

Each phase is behind a switch, default off, and does not change the solo or idle game (no session: no copy
exists, so the pack switches change nothing without a co-op player).

## Who moves the copy's body (fable-puppet-body, 2026-11-25)

Each fact: **read** (from the code or the game's files, place given), **measured** (a run), or **UNKNOWN** (with its probe).

### Where phases 0 to 3 stand

| Phase | In the code | Switch (all off by default) | What a run proved | Never ran |
|---|---|---|---|---|
| 0 Facts | no probe job exists | none | nothing | lift grades, that a Free body blocks and carries, stash path, a speed setter |
| 1 Body | pack: body files, size entries (crash-0116-a), default preset (crash-0116-b); hook: this section | pack `--puppet-body` (each actor's `"body"` files in the manifest, which the hook now reads) | 2026-11-19, proof-0116-5: child x2 and adult x1, no crash, both games show the other's copy (shots). The two crashes before it were ours and are fixed | walk into, stand on, push, jitter: "not judged" (the scenario's walk missed). Nothing with this section's hook rule |
| 2 Bomb hand-over | protocol 530-532, server rules, `coop_bomb_share.h`, wired in the spawn sync | `LICHII_COOP_BOMB_SHARE=1` | one run, FAIL: B never lifted A's lit bomb, no `BOMB_TAKE` (proof-0116, 2026-11-19); cause not found | everything after the lift: hand-over, one explosion, stash |
| 3 Lift a young player | pack `LiftableRef` (Ruto's) and carry AS; protocol 535-538; `coop_lift_player*`; young-only gates | pack `--puppet-liftable` + `LICHII_COOP_LIFT_PLAYER=1` (needs `--puppet-body`) | none: the three tries were PACK FAIL (`Npc_Zora_Ruto_Child_001.pack.zs` not in the .nup the kit reads) | all of it, UNKNOWNs 1-8 of section 4 |

### The facts the rule stands on

- **When our place is set against the physics step (read, docs/puppet.md "The frame hook", docs/hookshot.md).** The
  Director runs on the calc thread at the frame hook in `moduleCalc_` (main+0x6e6dd8 / 0x6e70f8), after the frame's
  job graph has been waited on: the actors of frame N have run, the draw of N has not read them. So a place set in
  hook N is drawn in N and reaches the physics in frame N+1's step. UNKNOWN: that the physics step itself is inside
  that job graph (probe B1).
- **What the game's setter does (read, main+0x4c6580 and main+0x4c6630, disassembled).** `0x4c6580(actor, w1)`:
  nothing for actor state 1; sets bit 23 of actor+0x4a8; takes the physics component `[[actor+0x270]+0x50]` (none:
  returns) and calls `0x4c6630(physics, &place(+0x31c), &rows(+0x328), 1 & ~w1)`. `0x4c6630`: the controller set is
  `[physics+0x20]`; with the last argument 1 (our w1 = 0) and no request already waiting (`[set+0x228] & 3 == 0`) it
  stores the 3x4 at set+0x1d0, sets `[set+0x228] |= 3` and, when the set has a character controller
  (`[set+0x1c8]`), sets bit 7 of its byte +0x1f2; then `0x4c6870(set, the 3x4)`. So the setter is a **queued
  teleport** of the whole controller set and its character controller, taken at the next step. With w1 = 1 the
  queue and the bit are skipped and only `0x4c6870` runs (a move that is no teleport: UNKNOWN what it does to a
  character controller, probe B2).
- **A teleport keeps the body's speed (measured, reports/held-bomb.md).** The held bomb fell between places.
- **The Director never called the setter for a player's copy (read, coop_puppet.cpp).** `matrix_call_` is set only
  by the fairies' director (coop_fairy.cpp); a copy's place was two bare writes (+0x31c, +0x358). For an actor with
  a body a bare write is undone within a frame (measured on enemies, docs/objects.md; on a copy: likely, not measured until B1). So in every `--puppet-body`
  run so far the picture was written each frame and the body was never told: it stood where the copy was made, or
  dragged the picture back. That alone explains a copy you walk through, and a wall where nobody stands.
- **The copy's character controller (read, build.py `body_character_controller`).** The player's, on the default
  preset, components FrameSetup, MoveStateUpdate, Rotate, JumpExtension, FloatWater, AccumulateImpulse,
  FollowSurfaceExtension, FollowRagdoll, ApplyVelocity; states InAir, OnGround, Free; default Free.
- **UNKNOWN: which field takes a speed** for a Free character controller (ApplyVelocity's input). Probe B2.

### The rule (lichii/hook/coop_puppet_body.h, tests/lichii/puppet_body_test.cpp)

The owner game's samples are the only word on where a Link is. This game's body of it never decides anything: it
has no gravity, no ground logic and no input (Free), and is moved once a frame, at the frame hook, by one of three
commands (`Drive::Step`), worked out from where the game left the body (read back), where the samples put it this
frame (the follower's drawn place) and how fast that place moves:

1. **rest**: the body is within 2 mm and 0.004 rad of its place and the place is not moving. Nothing is written
   and no game function is called. A standing copy is a still body: our Link leans on it and stands on it with no
   tremble, and the body is not re-teleported under his feet.
2. **drive**: speed for the next step = the samples' own speed + 0.6 x gap / frame time, capped at 40 m/s. The
   solver moves the body, so its contacts have a real speed (it pushes a pot, carries our Link on its head). The
   gain is under 1 so a body stopped by a contact is not pressed harder each frame. Used only once B2 names the
   speed field (`speed_channel`); until then every move is command 3.
3. **warp**: the place is written and the game's setter called (a teleport), for: a place that jumped (scene
   change, a warp, a new copy, back from parked), a gap over 1.5 m, a stalled frame, a body not read, and a driven
   body more than 0.5 m behind for 6 frames (held by something only this game has: their real Link is where the
   samples say, so the way is free in their game).

The same rule in every state of their Link, because the body never runs a state of its own:

| Their Link | What the samples carry | What the body does here |
|---|---|---|
| ground, slope, stairs | place + speed along the slope | follows; no ground snapping of ours, so no sinking and no step pops |
| moving platform | place and speed in the world (the platform's included) | `Carry`: the offset from the platform when the sample was made (our own platform's pose then, `PoseHistory`; platforms run in step, coop_platforms.h) is put on our platform now; the look-ahead uses `RiderSpeed` (their walk on it), so the platform is not counted twice. Pure code and tests; the glue (which platform, from coop_lift_ride's "stands on" test) is open |
| ladder, climb, crawl | place | follows. The shape stays the standing one (`Player_cct`): UNKNOWN whether it should swap to `_climb`/`_crouch` (probe B4) |
| water | place (swimming or sunk in iron boots) | follows; `FloatWater` does nothing in Free (UNKNOWN, probe B3: no bobbing against the samples) |
| fall, jump, knock-back, thrown | place + their real speed | follows at their speed; no gravity of ours is added, so it neither falls ahead nor lags |
| warp, room or scene change | a jump | warp, speed zero; hidden copies park their body through the setter too (a bare write left the body behind as a wall) |
| samples lost | nothing new | the follower's look-ahead ends (coop_follow.h `kAheadMaxMs`), the place stands, and the rule rests: the body stays (it still blocks). No flag of its own: the rule never sees "lost", only a place that stopped; the Director's hold and hide rules go on as before |

### What two games do, and why no real Link is stuck or thrown

Every case rests on one more rule, `Yield`: a body is never put inside the one Link this game simulates. If the
samples (late by the lag) put the copy's capsule into ours, the copy is moved the shortest way out: sideways, or
down under his feet, or up over his head. The other game does the same for its own Link.

- **Two players push each other.** In A's game B's copy is a wall for A (probe B1 proves a Free body blocks); in
  B's game A's copy is a wall for B. Neither real Link is ever moved by a copy: a copy is teleported or driven
  only to where its owner really is, and gives way if that is inside us. So nobody can be pushed off a ledge or
  through a wall by another player; each sees the other stop. The overlap left by lag (both walk in at once)
  shows as each copy standing aside by the overlap, a few centimetres.
- **A copy on a moving platform.** Carried by our own platform (table above); without the glue it follows the
  world place, late by the lag x the platform's speed.
- **Standing on a copy.** At rest it is a still body. When it moves by warp, our Link is not carried (a teleport
  has no speed): he slides off as off a thing pulled away. Carried rides need command 2 (B2). When it rises into
  our feet it is kept under them (`Yield`), so our Link is not thrown.
- **A copy crushed by a door or a block.** The copy is never held by this game's movers: a warp goes through, and
  a driven body held back 6 frames is warped. Our door is not stopped by it either (UNKNOWN whether a Free
  character controller blocks a moving rigid body, probe B1). The crush itself is the owner game's: its real Link
  and its own door decide.
- **Copies against enemies.** The body blocks an enemy's walk as the real Link does in his own game. It has no
  sensors (phase 1), so nothing hits it or is hit; phase 4 filters hits by attacker (section 5).

### In the hook now (only for an actor whose manifest entry has `"body"` files; no such actor: nothing changes)

Decided **per actor, never per pack**: `CoopPuppetBody::PackBodyActors` (each `"actors"` entry with files in its
`"body"`) -> `PlannerSettings::body_actors` -> `Director::SetBodyActors`; `Create` sets `slot.body` for a player's
copy made from one of them and starts the rule's state again (`ResetBody`: a new actor is warped, never "at rest"
by the old one's rotation or place). A `--puppet-body` pack gives every copy a body; a `--puppet-pvp` pack alone
(its own `"body": false`) gives one only to its `_Pvp` copies, which the same slot places: same cause, same rule.

For such a copy:
- `DrawAt` first gives way to our own Link (`Yield`): his place is his actor's (`CoopPose::FindPlayer`, +0x31c),
  his age his actor's name; the sizes are the game's own (read from the romfs, `GameParameter/
  CctComponentPlayerClimbParam`: `CharacterRadius` / `CharacterHeight` = 0.35 / 1.68 m adult (`Player`), 0.23 /
  1.17 m child (`Player_Child`)). UNKNOWN: that the standing shape `Player_cct` is exactly that size, and that
  the actor's place is the capsule's foot (probe B1: the height our Link stands at on a copy). Bodies that only
  touch (overlap up to 0.03 m: our Link standing or leaning on a copy) are left to the solver, or the copy would
  sink from under him a little each frame.
- Then `Drive::Step` (no speed channel yet): rest writes nothing; a move writes the place and owes one setter call.
- The owed call (`Director::Move`) is the slot's FIRST call of the frame, before sounds, pieces and animations.
  The frame's budget is 8 calls for all copies (`kMaxCallsPerFrame`; one is our own state): with more than 7
  copies moving at once some wait a frame, in turn (the round starts one slot further each frame). A copy that
  waits has its picture right and its body one frame behind; the log counts them.
- `Park` (hidden) owes one call per place parked at, not one a frame. While our Link holds the copy (phase 3)
  nothing is owed and the rule starts again when he lets go.
- A copy whose class is not the one the setter is known for (`kActorVtable`) is said once in the log (`body: ...
  is NOT moved`), and no call is counted for it.

Log, once per copy: `P<n> body: <actor> has a body; it is moved through the game's setter`. Every 300 frames:
`P<n> body: <r> frames at rest, <w> moved (<s> setter calls made, <q> waited a frame for one), <y> gave way to
our Link; the game had it up to <x> m from our place`. `<x>` compares our place with the actor's +0x31c, a field we
write ourselves: UNKNOWN whether the game writes it back from a Free character body (if it does not, `<x>` is
always 0 and "rest" only means "we did not move it"). With `LICHII_COOP_DEBUG_BODY=1` each such line is followed by
`body probe` lines (read only): the actor's place, the controller set's request flags (+0x228) and 3x4 (+0x1d0),
and the first 0x400 bytes of the character controller (`[set+0x1c8]`), for the copy and for our own Link.
Open in the glue: `Carry` (which platform: coop_lift_ride's stands-on test).

### Probes (one module-only two-game run, pack `--puppet-body`, shots on both games)

- **B1** (run with `LICHII_COOP_DEBUG_BODY=1`) first the copy's `body: ... is moved through the game's setter` line
  (its class). The `body probe` lines while B walks and A pushes the standing copy: does the actor's +0x31c follow
  the body (is `<x>` real), and where in the set or the controller the body's own place is. A walks into standing B:
  A stops (shot both). A jumps onto B's copy: A's height = the copy's top (shot).
  B walks: A's log `body:` line, x under 0.02 m. B hidden (cutscene) then back: no wall left behind (A walks through
  the old spot, shot). A door closes on B's copy in A's game: the door closes (shot).
- **B2** the speed field, from the same `body probe` dumps (the copy's character controller at rest and while B
  walks, our own Link's while he stands and while he walks): the three floats that are his speed are the field. Then `speed_channel` on, and B1 again plus: A stands on walking B and is
  carried (shot).
- **B3** B swims and dives: the copy's `body:` x stays under 0.02 m. **B4** B climbs a ladder beside A: A is not
  pushed (shot); if he is, the body swaps to `Player_cct_climb`.

## Lift carry (N5, puppet-body-4; `lichii/hook/coop_platform_carry.h`)

A copy is drawn where its owner was `CoopShowDelay::Us(owner)` ago (at least 100 ms), but our lift is where every
game's lift is now (coop_platforms.h keeps them in step). On a lift moving v m/s the copy was v x delay off the floor
(the Jabu-Jabu lift, ~2 m/s: 0.2 m inside it going up; a copy with a body was then pushed out or warped each frame).
Every copy, body or not, standing on one of our lifts is now carried by what that lift moved over the delay
(`Carry(lift then, lift now, place)`). "Stands on" is CoopLiftRide's rider footprint (1.8 m around, 0.5 m under,
2.5 m over the lift's actor place) asked of the lift's place THEN, so only the lift it stood on carries it. The
lifts are #65's (`IsRideLift`: ElevatorLift_ and MoveFloor_), recorded by the objects walk (every two frames); they
do not turn, so no yaw is kept. A carry over 2 m (a lift record that jumped) and a lift unseen for 500 ms carry
nothing; race mode carries nothing (its lifts are not in step). The log says `P<n> lift carry: on our lift, carried
<m> m (show delay <ms> ms)` and `... off our lift` on each change. `LICHII_COOP_PUPPET_CARRY=0` turns it off.
Host test: `tests/lichii/puppet_body_test.cpp` ("platform carry: ..."). Not proven in a game yet.

## The phase 1 switch (built)

`tools/puppet_pack/build.py --puppet-body` (off by default). Every puppet (colours, skins and quiet copies
included) gets its own `PhysicsParam` and `ControllerSetParam` made from the player's own controller set
(its `$parent` chain merged), with only:

- `InitializerName`, `ShapeNamePathAry`, `MatterRigidBodyNamePathAry`, `CColEntityNamePathAry`,
  `PathCharacterController`: Link's own body shapes and character controller;
- `Cloth`, `ClothAdvandecOption`, `ClothList`, `ClothReaction`: his cloth (hair, hood, belt), as
  hs-puppets-alt-b's `cloth` mode, which ran without the slot-10 crash.

Left out: sensors (`Tgt`, `Atk`, `Chemical`: nothing hits or is hit by the copy yet), the ragdoll, the
rigid body entities (ride, elastic hands, reflect shield, liftable sweep), the controller entities, IK and
looking. Its character controller is the player's (merged) with `DefaultMoveState: Free` (the NPC
`ThroughGround` value) so it does not fall between the hook's places. Each new file's size-table entry is
the sum of the entries of every player file merged into it (`build.chain_entry`: the game parses a param in a
heap of its entry; with Player_Child's entry alone the child's character controller, merged with Player's,
ran out at its `InteractWithRagdoll` object and crashed `GeneralFormatAsync_` as the copy loaded,
crash-0116-a). The manifest (`coop_puppet.json`) records `"body": true`. The hook's side is
"Who moves the copy's body" above (the first version of this switch said the Director already placed the
puppet through the physics-aware setter: it did not, only the fairies' director called it).

Build for a proof (lane pair, offline job): `python -I -B tools\puppet_pack\build.py --romfs <the .nup>
--mod-dir <kit>\mods\contents\0400486024DEC000\coop-puppet --puppet-body` on both games, then phase 1's
checks. Back to normal: rebuild without the switch.
