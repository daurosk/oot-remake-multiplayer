# Shared enemies and objects: research and protocol proposal

Plan stage 5 ([plan.md](plan.md)): bombs, arrows and other projectiles seen on every screen, one owner per enemy with a real hand-over, and hits on a copy sent to the owner as damage events. This page says which actors the game uses, where their state is, how an actor is told apart on two games, and proposes the messages and the ownership rules. No product code yet.

Addresses are offsets into `main` of build `e0f3ccfaf50065ae` (1.1.1). **Checked** means read from the instructions or the data files. **Inferred** means taken from a name, a shape or the engine family. **Live** means seen in the running game; see "Live check", the only part that was run.

## Results

- Every player projectile is its own actor, created through the same function as everything else (0x372a70, [puppet.md](puppet.md)). Shot ones go through the shootable create at **0xc16cf0**, which passes the blackboard values `Shootable_KeyHash` and `Shootable_OverrideParamNum` with the request.
- The actors: `NormalArrow` (arrows, also the actor fire, ice and light arrows are shot as: inferred), `Shootable_DekuSeed` (slingshot), `Shootable_Hook` and `Shootable_HookLong` (hookshot and longshot tip), `Item_Bomb`, `Item_Bombchu`, `Item_DekuNut`, `Weapon_Boomerang`. The `Obj_*` and `Item_Arrow_*` packs of the same names are pick-ups.
- An actor's name is a `char*` at actor **+0x260**. Its life is an s32 at **`*(Life + 0x1c10) + 8`**, its maximum at `*(Life + 0x1c18) + 8`, where Life is component-table entry **+0x2b8** (table at actor +0x270).
- A placed actor's 64-bit placement hash, the key of its flag in [flags.md](flags.md), is a u64 at **`*(actor + 0x3d8) + 0x88`**. A game-made actor has `0xffffffffffffffff` there (live).
- Live (2026-11-08): the actor list is found (`[[main + 0x4e91888] + 0x118] + 0x670`, node at actor + 0x538); `Shootable_DekuSeed` is confirmed; the life chain reads right on every enemy; damage is applied by the Life component's step 0x49d8c0, and writing life directly holds and runs the game's own death.
- Proposal: projectiles stay with the game that made them for their whole life and are drawn elsewhere as harmless copies; enemies have one owner with a fight lock and a hand-over that carries the state; any life lost on a copy goes to the owner as `DAMAGE`. Eight new messages (below).

## Live check (2026-11-08)

One portable instance of the patched emulator, a child save in the Graveyard at night (slingshot, 48 seeds; no bombs, bow, boomerang or hookshot yet). Memory was read through the emulator's debug server (`EDEN_NX2_DEBUG_SERVER`, `read_memory` and `search_memory`) and written and watched through its GDB stub. The hook's own reader, `lichii/hook/coop_objects.cpp` (`LICHII_COOP_DEBUG_OBJECTS=1`), does the same walk and logs what is below; it compiles with the emulator's flags but has not yet run in a build.

### The live actor list (found)

| What | Where | Seen |
|---|---|---|
| List head | `[[main + 0x4e91888] + 0x118] + 0x670` | One circular doubly linked list of every actor: 242 to 740 entries in the Graveyard as areas loaded |
| Length | u32 at head + 0x10 | Equal to the walked length every time it was read still |
| Node | `{next +0, prev +8, owner +0x10}` | The actor's own node is at **actor + 0x538**. A handful of non-actor objects (3 of 312) share the list with the node at owner + 0x3a0 or + 0x3a8; skip any node whose owner + 0x538 is not the node |

It was found by searching the heap for the player actor's address: one hit is the player's own +0x548, a node whose owner field points back at the player. The list changes while it is read from another thread: a walk sees actors drop out and come back within 0.1 s. The hook's reader drops any walk whose length is not the head's count and calls an actor gone only after two walks without it.

Watching the list stands in for hooking 0x372a70: an actor made by a create shows up in the list a frame or two later, so names and positions are the same; only the create's arguments (the shooter, the override list) are not seen. That needs the SVC hook.

### Projectiles (one of eight confirmed)

| Action | Actors that appeared | Notes |
|---|---|---|
| Draw the slingshot (ZR held) | `Weapon_Range_SlingShot` at Link's hand, and `Shootable_DekuSeed` at the same place | The seed is made when it is nocked, before it flies |
| Each shot | the flying `Shootable_DekuSeed` goes 0.1 to 0.5 s later; a new one is made at once for the next shot | 48 to 45 seeds over three shots |

| Throw a Deku nut | `Item_DekuNut` at Link's hand | Gone 1 to 4 s later |

**Confirmed: `Shootable_DekuSeed` and `Item_DekuNut`.** `NormalArrow`, `Item_Bomb`, `Item_Bombchu`, `Weapon_Boomerang`, `Shootable_Hook` and `Shootable_HookLong` are still from the data files only: the save has none of them.

### Life chain (confirmed)

`*(*(*(actor + 0x270) + 0x2b8) + 0x1c10) + 8` reads life and the 0x1c18 twin reads max on every enemy and on Link:

| Actor | Life / max |
|---|---|
| `Enemy_ReDead` (5 of them) | 12 / 12 |
| `Enemy_Keese` (4) | 1 / 1 |
| `Enemy_Skulltula_Gold` | 1 / 1 |
| `Enemy_Poe` (made by the game near the graves) | 3 / 3 |
| `Player_Child`, 4 hearts | 16 / 16 (4 a heart) |

### Damage (how life changes)

- **The function.** The Life component's step **0x49d8c0** applies damage. Each frame it sums what was dealt to the actor into **Life + 0x1cb0** (from **Life + 0x228**, **Life + 0x1350** and one more term), then sets life with a compare-and-swap at **0x49dcf8**: life minus that sum, at most max, never below 0. A GDB write watchpoint on an enemy's life stops there every frame, with sum 0 when nothing hit.
- **Life + 0x228** is a per-frame damage accumulator: 0xaa920 (called from 0x2c23e28) swaps it to 0 each frame. What adds a hit into it was not found: there is no atomic add to that offset in the code, so it is written through a base other than the Life component. A watchpoint cannot follow it live: the field is written several hundred times a second and each stop starves the frame loop (52,000 stops, no frame shown, in 90 s).
- **Life + 0x1ca8 and + 0x1cac** are a mirror of life and max, rewritten 1 to 2.5 s after life changes (0x8d96a0). They read -1 for an instant now and then. They do not guard life.
- **A real hit.** A player killed a Poe with the Kokiri Sword while a logger read it: life 3 to 2 to 1 to 0, one per hit, and the sum at Life + 0x1cb0 read 1 after each. 2 s after life reached 0 the Poe left the list and `Obj_Spirit_Poe` (its soul) appeared; its lantern `Weapon_PoeCantera` went with it. That Poe was made by the game near a dug-up grave (hash `0xffffffffffffffff`), so it is a game-made enemy: the "Made by the scene with no hash" row of the id table applies to it.
- **A direct write holds and kills.** Through the GDB stub: a ReDead's life 12 to 5 stayed 5, and its mirror followed. 5 to 0: 2.4 s later an `Obj_Rupee_Green` appeared at the ReDead's place, the game's own death drop. The ReDead stayed in the list in state 4, life 0, for the 20 s it was watched. So the owner can apply `DAMAGE` by writing life (the proposal's plan), and the game's own death follows; a copy's life must not be written to 0 by anyone but its owner.

### Placement hash (confirmed)

`*(actor + 0x3d8) + 0x88` is a u64 on every placed enemy, and the same after a reload: ReDead `0xf9b880ab096a9cd6`, Gold Skulltula `0x32f42f523be6425`, Keese `0x62a0218c021047f1`, `0x9b68efc6a861c030`, `0x30843dad7976f0fa`, `0xb8a209d8a2f24c4d`. Actors the game made (Link, the camera, the Poe) have **`0xffffffffffffffff`** there, not 0.

### States seen

4 for every awake actor. The Gold Skulltula went to **6** while Link was about 25 m away and left the list later, with life still 1 before that; whether 6 is asleep or out of range, and why it was then removed, is not known.

## The projectile actors

Read from the actor packs in the romfs (`Actor/<name>.engine__actor__ActorParam.bgyml` and the component files they point to).

| Item | Actor shot or thrown | How it is chosen | What it has | Status |
|---|---|---|---|---|
| Bow | `NormalArrow` | `ShootableActor` in `Weapon_Range_Bow`'s `BowParam`; initial speed 1.875, range 20, reload 0.2 s, base attack 2 | Physics `Arrow`, `SensorHitRef` `Arrow`, `ChemicalRef` (element), `ShootableParam/Arrow` | Checked |
| Fire, ice, light arrows | `NormalArrow` with an element | `Item_Arrow_Fire` and the others are pouch items with no shooter of their own; `NormalArrow` has a `ChemicalParam` | | Inferred |
| Slingshot | `Shootable_DekuSeed` | Its own pack; `Weapon_Range_SlingShot`'s `BowParam` is only the default | `SensorHitRef` `Arrow`, no chemical | Name checked, link to the slingshot inferred |
| Hookshot, longshot | `Shootable_Hook`, `Shootable_HookLong` | `ShootableActor` in `HookshotParam`; speed 0.25, range 10 for the hookshot | AI, `LifterRef` (it pulls things), `SweepOutRef` | Checked |
| Bomb | `Item_Bomb` | Held, then thrown or dropped | Life (max 1, `BallBomb`), `ChemicalRef` (`BombFruit` material, explode blast), `LiftableRef`, `ShootableRef` (thrown), AI `Item_Bomb.root.ainb` | Checked |
| Bombchu | `Item_Bombchu` | As the bomb | Life, chemical, its own AI (`Item_Bombchu_Remake`) | Checked |
| Deku nut | `Item_DekuNut` | Thrown | `ShootableRef`, `SensorHitRef`, AI; no life | Checked |
| Boomerang | `Weapon_Boomerang` | Thrown, homes and comes back | AI (two graphs), `AttackParam`, homing and scene-change sensors | Checked |

The bomb's fuse is in its AI: `Item_Bomb.root.ainb` uses the node `ExecuteBarrelBombAI` (registered at 0x2639a90) with the values `BombTimer`, `RemainingFrameTime`, `ElapsedFrameTime` and the animation `Bomb_Blink`. Where the node keeps the remaining time was not read. Checked as names; that `RemainingFrameTime` is the fuse is inferred.

The same node compares actor +0x260 with `"Item_Bomb"` and `"Item_Bombchu"` (0x26394e4 to 0x2639564) and then calls 0x32293a0 with 3 or 4. Checked; what 0x32293a0 does is not read.

Enemy projectiles are actors too (`Octorok_Bullet`, `Dodongo_Bullet_Fire`, `Enemy_Scrub_Business_Seed`, the bosses' `*_Bullet_*`). They belong with the enemy that fires them (below).

## The create path

| Address | What | Status |
|---|---|---|
| 0x372a70 | `requestCreateActorAsync`, as in [puppet.md](puppet.md) | Checked there |
| **0xc16cf0** | Creates a shootable: fills a list of blackboard overrides (`Shootable_KeyHash` from its argument +8, `Shootable_OverrideParamNum` from +0x10), then calls 0x372a70 at 0xc172fc with `w4` from its argument +0xc and the override list in `x7` | Checked as calls and stores; the argument's other fields were not read |
| 0xc17410, 0xc17a10, 0xc17ff0, 0xc181a0, 0xc18350, 0xc18510 | Six more creators beside it, each calling 0x372a70 | Checked as calls |
| 0x2eb07d0 | Calls 0xc16cf0. A virtual method: its only reference is the table entry at 0x4b4ffe0; the code near it reads `Shootable_ShootableActorDataIndex` (0x2eb5020) | Checked; that it is the shooter component is inferred |
| 0x992df0, 0xd36a0 | The other two callers of 0xc16cf0 | Checked as calls |

So a remote player's arrow can be made on this screen the same way the puppet is: the hook's create command with the actor name, a position and a rotation. A velocity is not part of the create argument that [puppet.md](puppet.md) read; it would be written after the create (see "Copies").

## Fields of a live actor

| Field | Where | Status |
|---|---|---|
| Name | actor +0x260, `char*` | Checked: compared with `"Item_Bomb"` at 0x26394f8 |
| Position, rotation | +0x31c, +0x328 | [player-state-map.md](../../integration/docs/player-state-map.md) |
| State (4 alive, 7 to 11 gone) | +0x2c | Same |
| Component table | +0x270 | Checked |
| Life component | table +0x2b8 | Checked: 0x833e00, used by `QueryActorLife` |
| Life, max life | s32 at `*(Life + 0x1c10) + 8` and `*(Life + 0x1c18) + 8` | Checked: `QueryActorLife` 0x29afbc0 reads both and divides them; 0x8d8c50 copies max into life (a reset) |
| A copy of life, max life | Life +0x1ca8, +0x1cac, written at 0x8d96a0 | Live: a mirror that follows life 1 to 2.5 s later |
| Placement hash | u64 at `*(actor + 0x3d8) + 0x88` | Checked: `OneShotInstanceGameDataSetBool` (0x28e64c4) passes it to the 64-bit-key setter 0x3761720, and uses an `InstanceGameParameter_%llu` name when it is 0 |
| The AI's actor | node +8, +8, +0x1f0 | Checked: 0xc70e70 and 0x833d60 |

The value at `*(Life + 0x1c10)` is an object of its own, not a plain field: its pointer is loaded at 103 places in 97 functions but written directly only by the reset. So damage is applied through some other function. Live: it is the Life component's step 0x49d8c0, which writes life with a compare-and-swap (see "Live check").

Enemy damage is set in each pack's `Life/DamageParameters` (`IsMergeSameFrameHitInfo`, guard angles, burn, shock, freeze, impulse damage) and life in `Life/LifeParameters` (`MaxLife`; `Enemy_Bari` has 2). The game's damage pipeline has the AI queries `QueryGetLastExternalDamageData`, `QueryIsDamageTypeBomb`, `QueryDamageCalculatorIsOnDamageTypeAny` and others, all reading an external damage calculator. Names checked; the calculator was not read.

## Telling one actor from another on two games

| Kind | Id on the wire | Why |
|---|---|---|
| Placed (enemies, pots, chests, switches) | the u64 placement hash | Both games load the same Banc placement, and the hash is the key the flag store already uses ([flags.md](flags.md)) |
| Made by a player's game (projectiles, an enemy a player's action spawned) | the maker's player number and a u32 counter of its own | Nothing else names it on both sides; the maker says "I made 7" and everyone calls it (2, 7) |
| Made by an enemy (bullets, summons) | the same, made by the enemy's owner | The enemy's owner is the only game whose copy really fires |
| Made by the scene with no hash (spawners, waves; the night Stalchild now go by "Rolled spawns" below) | kind placed, value = `SceneSpawnId(scene, actor name, order)`: FNV-1a 64 of the three with the top bit set. `order` counts the enemies of that name seen in this scene visit, first seen first (one walk ordered by x, then z) | Stalchild were seen with hash ~0 (Caleb's test, 2026-11-09). The n-th Stalchild in game A and the n-th in game B get the same id, so a hit on one shows on the other, even when each game spawned its own near its own Link. Positions are not shared yet, so the two may stand in different places; a later rule can make the scene host's game the only spawner and hide the others' extras |

A placed id is only good inside one scene, since a scene can be loaded on one game and not the other. Messages that name a placed object say its scene once, in `OBJECT_PRESENT`.

### Rolled spawns (the night Stalchild)

Caleb (2026-11-09): every player must see the same Stalchild rise out of the ground at the same spot, at the same time. The scene-order ids above paired each game's own rolls, which rose in different places and chased their own Link.

**The spawner** (static reading of main, build e0f3ccfaf50065ae; not yet run):

| What | Where | Status |
|---|---|---|
| The spawner is the scene component `AutoPlacementMgr`, not an AI module. Its parameters are `Work/Scene/Component/AutoPlacementMgrParam/Enemy_Stalchild.game__scene__AutoPlacementMgrParam.gyml` (child Hyrule Field; the adult field also has `Enemy_Stalchild_Big_Adult`), named by `GameBancParam/HyruleField_Child`. The field's logic modules `HyruleField_Level_StalchildArea_Walking` and `_Riding` turn it on by area | romfs | Read |
| Scene component 0x23: the AI nodes get it with 0xb2af20 (scene) and 0xb2b7b0(scene, 0x23). Created by 0xf45d30 (0x96c8 bytes), vtable 0x4ba0628 | 0x28b7e20 (`OneShotAutoPlacementMgrRequestDisappearAll`'s run, which writes 0x11940 to mgr +0x74), 0x32b5424 | Read |
| Its spot rule is in C++, read from the parameter file: `StalchildFrontSpawnDirRand`, `StalchildBackSpawnAddBaseDir`, `StalchildBackSpawnRandDirOnBase`, `StalchildLineSpawnNearDistance`, `...MinGap`, `...MaxGap`, `...MaxAngleCamZ`, `StalchildBigCreateDistance`, `MaxCreateStalchildByFixedSpawn`: a Stalchild rises in front of or behind Link, or several in a line, at a random distance and angle | the reader 0x325fb2c | Names read; the code that rolls was not traced |
| One spawn: 0x326e140(request, element) builds the create argument (name at request +0x1818, place at +0x14ac) and calls `requestCreateActorAsync` (0x372a70) at 0x326e874 | | Read |
| The Stalchild asks the manager which rise to play (`OneshotAutoPlacementMgrReturnStalchildAnimeIndex`) | node name only | Not traced |
| The game's own ways to stop it: `ExecuteAutoPlacementMgrSetManualForbidAutoPlacementArea` (run 0x262b2c0) works on components of the player actor, not on the manager; `ForbidAutoPlacementArea` actors and the `StalchildAutoPlacementStopMask` tag stop it by area. No plain on/off byte like TotK's (`mgr+0x1a`) was found | | Read; not used |

**The rule** (branch `objects-6`, `coop_objects_sync.cpp`, tested in `objects_test.cpp`):

- Each game knows the others' scenes from their `PLAYER_SAMPLE`s (a scene older than 5 s does not count). The **decider** of a scene is the lowest player number in it. Only the decider's rolls are kept.
- On the decider, a new hashless `Enemy_Stalchild` or `Enemy_Stalchild_Big` gets a made id (maker = the decider, its own counter) and is sent once as `OBJECT_SPAWN` (name, place, rotation, animation, life); then samples as for any enemy it owns, and `OBJECT_REMOVE` (dead or gone) when it leaves the list.
- On every other game in the scene, its own roll is deleted on the next walk with the game's delete request (0xa6490), the same way as a taken pickup, and counted. The spawner is not stopped, so it may roll again and be deleted again: the log line `rolls decided by player N, C copies to make, R own rolls deleted` shows how often.
- The decider's `OBJECT_SPAWN` makes the spawn director (`CoopSpawn::Director::SetEnemies`, keys with the top bit set, `place` off) create the same actor by name at the decider's spot. When it is on the list it takes the decided id; from then on the Mover places it where the decider's one is and plays the decider's animation at its frame (so the rise from the ground is the decider's rise). A fresh Stalchild within 4 m of a copy still being made is not deleted for 3 s, since it is likely that copy before the director lists it. At the decider's `OBJECT_REMOVE` the director deletes the copy.
- The director needs the svc cave (`LICHII_COOP_PUPPET` or `LICHII_COOP_SPAWN`), as the Mover does.
- Not done: a decider who leaves the scene (its made ones are then gone for the server too, and the next player starts rolling); the copy's rise starts when `OBJECT_SPAWN` arrives, so it is about one round trip later than the decider's (the Mover then sets it to the decider's frame).

## Design

### Projectiles

- **The maker owns it, for its whole life.** No hand-over: an arrow lives under a second, a bomb a few. This is the TotK lesson in reverse: there, arrows and bombs were never synced and only their damage was, which showed nothing on the other screen. Here both are done.
- **Copies are harmless.** On other screens a projectile is drawn with a small pack of our own (`CoopShot_Arrow`, `CoopShot_Seed`, `CoopShot_Bomb`, `CoopShot_Bombchu`, `CoopShot_Nut`, `CoopShot_Boomerang`, `CoopShot_Hook`), built like the puppet's: the game's own model, animation and effect files by path, no physics, sensor, chemical, attack or AI. A copy can never hit, explode, light a torch or pull Link. So nothing it does needs to be undone, and no hit is counted twice.
- **What the maker sends.** `OBJECT_SPAWN` once, reliable, with the actor kind, position, velocity, rotation and its server time; then `OBJECT_SAMPLE` while it flies, for the ones that do not fly straight (bomb rolling or held, bombchu, boomerang, hook tip), at up to 30 a second; then `OBJECT_REMOVE` with the reason and the place (hit, exploded, gone). An arrow and a seed need no samples: the copy flies them from the spawn's position and velocity, with gravity (to measure: the arrow's physics param), until the remove.
- **Explosions and hits shown.** On `OBJECT_REMOVE` with reason exploded, the copy plays the bomb's explosion effect and sound at that place and disappears; with reason hit, an arrow copy sticks there for a moment. The effect's name comes from `Item_Bomb`'s `ELink` and `SLink` files (not read).
- **Held bombs.** A bomb in a player's hands is sent as samples like any other; the receiver may instead attach the copy to that player's puppet hand. Measured later.
- **What projectiles do to the world.** A bombed wall, a shot eye switch, a lit torch: what is saved goes through shared flags ([protocol.md](protocol.md), stage 4). What is not saved (a timed switch, a torch) is not shared at first and is listed under "Not done".

### Enemies

The rules answer each way the TotK sync broke ([protocol.md](protocol.md) "Rules it is built on"; the TotK port's `docs/online-sync.md`, section 3).

- **Who may own.** Only a game that has the enemy loaded and awake. Each game says what it has in `OBJECT_PRESENT`.
- **First owner.** The first game to report the enemy present. Two at once: the one nearer it, then the lower number.
- **Fight lock.** An enemy is "in a fight" from the first change of its life, or from its owner's report that it is targeting a player, until 12 s after the last of those (60 s for a boss). In a fight it keeps its owner. It moves only when the owner leaves the session or the scene, unloads it, is loading, has been paused over 6 s, or has been dead over 10 s. Hits from other players never move it.
- **Out of a fight.** It moves to another player only if that player is within 30 m of it, at least 10 m nearer than the owner, and the owner is over 40 m away, for 3 s in a row, and not within 5 s of the last change.
- **Groups.** A boss and its parts, and an enemy and what it fired or summoned, are one group with one owner. A group's root is the boss (or the summoner); parts follow it. Part lists come from the actor names (`DungeonBossDodongo_Bullet_*` follows `DungeonBossDodongo`, and so on), checked for each boss when its fight is tried.
- **Hand-over carries the state.** The server asks the owner to let go; the owner answers with its last state (position, velocity, rotation, life, animation command and frame); the server passes it to the new owner and only then announces the change. If the owner is gone and cannot answer, the server uses the last sample it relayed. The new owner places the enemy at that state advanced by the velocity over its age, and sets its life to the lower of its own and the received life, so a boss never heals across a hand-over.
- **An owner epoch.** Each change of owner adds 1 to the object's epoch. Every sample and damage event carries it, and a receiver drops anything from an older epoch. A late sample from the old owner can then never pull the enemy back.
- **Copies.** A non-owner's enemy is the game's own actor (it is placed, so it exists anyway). Each frame the hook writes the owner's interpolated pose into it, as for the puppet, and its life from the latest sample (never above it). Its AI still runs; holding it still is a later step (TotK's `TOTK_SYNC_AI_HOLD` idea), measured first.
- **Only the owner removes.** A copy whose life reaches 0 here is held at 1 until the owner's sample says 0, so the enemy dies once, on every screen, when its owner says so. The owner's `OBJECT_REMOVE` (reason dead) removes copies. A non-owner unloading its copy only sends `OBJECT_PRESENT` without it.

### Damage

One path for every kind of hit, as TotK's `TOTK_SYNC_DAMAGE` found worked best (`docs/online-sync-ws3.md` there): read the life of every enemy copy right after the hook writes it, and again at the next frame hook, after the game's own frame. A drop is a hit taken on this screen.

- Drops on one copy are summed for 100 ms from the first, then sent once as `DAMAGE` to the owner, with the amount, the cause if known and where it hit.
- Not sent: when the owner's own life for it came down by the same amount since 50 ms before (both games ran the same hit), when our player is over 80 m away, or over 20 a second.
- The owner takes the amount off its enemy (life at least 0) and sends a sample at once. Its life change starts the fight lock, but does not move the enemy (above).
- Hits that change no life (a Deku nut's stun, a boomerang's stun, a hookshot pull) are not seen by a life read. They need the cause sent instead: `DAMAGE` with amount 0 and the cause, applied by whatever the game does for that cause. Not found yet.
- Projectiles hit only on their maker's screen, so a remote player's arrow reaches a copy on the maker's game, and that game forwards the drop. Copies of projectiles never hit.
- Players are not hurt by each other, and puppets have no life. Enemy attacks on a puppet do nothing; an enemy attacks the player of its owner's game. Whether enemies should target puppets is a later question.

## The hook side (first cut)

`LICHII_COOP_OBJECTS=1` turns it on (off by default). `lichii/hook/coop_objects_sync.cpp` holds the rules, tested in `tests/lichii/objects_test.cpp`; `coop_objects.cpp` walks the actor list every second frame and writes lives. Only actors whose name starts with `Enemy_` are shared.

- `OBJECT_PRESENT` lists the enemies loaded in our scene (the scene of `PLAYER_SAMPLE`), on every change and every 2 s. A new scene forgets every id.
- The owner sends `OBJECT_SAMPLE` (time, position, life) at most 20 times a second while life or place changes, once a second while still.
- On a game that does not own an enemy, a drop of its life is our hit: the drops over 100 ms go to the owner as one `DAMAGE` (cause unknown). The owner writes life minus the amount (not below 0) into its enemy, and its next sample carries it.
- A copy takes the owner's life from samples: a lower life at once, a higher one only when none of our hits is in flight (500 ms after our `DAMAGE`), so it does not heal back between our hit and the owner's answer. `HANDOVER_STATE` gives a new owner the last life.
- Animals are shared the same way (`Animal_` names, so a cucco's hits and place are shared).
- **Copies follow the owner** (branch `objects-4`). The owner's samples also carry rotation (the 3x3 matrix at +0x328 as a quaternion), velocity (from the change of place between walks) and the animation (AS slot 0: command, frame, rate; a new command alone is worth a sample). Every other game works out, each presented frame, the owner's pose 100 ms in the past, blended between the two samples around that time or carried on by the velocity for at most 150 ms past the last. A copy with no sample for 2.5 s is left to itself; an enemy we own is never moved. `LICHII_COOP_OBJECTS_POSE=0` turns this off.
- **Moving an actor needs the game's own setter.** A plain write of +0x31c from the emulator is undone within a frame (two-game test, 2026-11-09: copies were 4.8 m off on average, up to 71 m). The AI node `ExecuteActorForceSetMtx` (run 0x2622e50) writes +0x31c..+0x34b and then calls the actor's vtable slot 0x218 with `(actor, 0)`. Every actor seen has the vtable 0x4b25e18, whose slot 0x218 is **0x4c6580**: it sets bit 23 of actor +0x4a8 and hands the new transform to the physics component (table +0x50, 0x4c6630 and 0x4c6870). So the hook's Mover, a caller of the svc cave (`CoopSvc::AddCaller`, order 30, after the puppets, spawn copies and fairies), does the same on the game's calc thread each game frame for every copy, after checking that the actor is still on the list (its node points back at it), awake (state 4) and has that vtable. Live: copies held exactly where they were put (0.000 m off over 1,488 moves in 5 s). The cave is patched only when `LICHII_COOP_PUPPET` or `LICHII_COOP_SPAWN` is on.
- **Animations and hit reactions.** On copies the Mover plays the owner's AS slot 0 command through the game's play request (0x2022a0, as for the puppet) and checks the copy every game frame on the calc thread: when its slot 0 shows another name (the copy's own AI took over), the owner's one is played again at once and started at the owner's frame (0x37b5d70 `(controller, 0, s0 = frame)`, as the puppet), the sample's frame plus rate x 30 a second, shown 100 ms late like the pose; on each new sample a copy more than 3 frames off is set to it. Before (Caleb's hand test, 2026-11-09) the replay came every 250 ms from frame 0, so a cucco near the other player and a carried cucco looked frozen in a loop (`Lifted` replayed 42 times in 11 s). Not yet seen live with this change. The names live in one block from the game's malloc (0x14c6c90), one fixed cell per name, since a slot may keep the pointer. Live: a ReDead hit by A's sword showed the same place and pose in B, and B's copy played `Standing`, `Turn`, `Run`, `Wait`, `Attack_Start`, `Attack_Fail` right after A's. The red hit flash is not an animation and is not copied.
- **Life on copies is written directly.** The Life step 0x49d8c0 reads an s32 at Life + 0x228 (and Life + 0x1350), stores their sum at Life + 0x1cb0 and takes it off life; but Life + 0x228 belongs to a whole hit record (+0x1f0..+0x2f0: a present byte at +0x24c, a hit id at +0x248 compared with the last at +0x1b18, the attacker at +0x240..+0x26c, copied to +0x1b20.. as the last hit). An amount written there alone was never taken (live). `LICHII_COOP_OBJECTS_HURT=1` still tries it, with a direct write after 400 ms; off by default. 0xaa920, which clears Life + 0x228, is the Life reset, not a per-frame step. Hit records seen live (A's ReDead, Kokiri Sword): +0x1f8 `00060001 40040000`, +0x200 the hit id (0xbc, 0xbe, 0xc0), +0x218 the hit actor itself.
- **One-time pickups taken elsewhere.** A key64 flag arriving set to 1 (`FLAG_SET`, `FLAG_STATE` or the snapshot) marks that placement key taken; a loaded actor with that placement hash (a Gold Skulltula, its `Obj_RupeeToken`) is no longer shared and is deleted once with the game's delete request (0xa6490 `(actor, 0)`) by the Mover. Live with a test key (the ReDead's hash): B deleted its ReDead 0.4 s after, A kept its own. Not yet run with a real Skulltula.
- Test switches (two-game tests only): `LICHII_COOP_OBJECTS_TEST_DRIVE=1` holds one of player 1's owned enemies 3 m ahead of Link, swaying, hurt by 1 every 4 s; `=2` holds it still 2 m ahead for real sword hits; `LICHII_COOP_OBJECTS_TEST_TAKE=<hex key>` acts on player 2's game as if that key arrived taken, 3 s after sharing starts.
- **Townspeople** (branch `objects-5`): `Npc_` actors are shared like enemies and animals (place, rotation, animation, life), except each player's own: `Npc_Navi` (the fairy sync has her), `Npc_Equipment_*` (held equipment) and `Npc_Invisible_*` helpers, which the first Kakariko run showed being shared. Live: both games in `KakarikoVillage_Child`, A sharing 7 townspeople and animals, B holding 5 copies where A's are (0.000 m off). No townsperson was in either camera.
- **Hit replay** (on by default since Caleb's Stalchild test; `LICHII_COOP_OBJECTS_HIT_REPLAY=0` writes life directly instead): a life an enemy must lose, on a copy (the owner's sample has less) or on the owner's own enemy (a `DAMAGE` came), is written into its Life component on the calc thread as a whole hit, shaped like a live Kokiri Sword hit (attack header +0x1f8, amount +0x224/+0x228, incoming hit +0x240..+0x2d8 with our player's actor as the attacker, a fresh hit id, the enemy's place; the list links from +0x2d8 on untouched). The game's own damage step then takes the life and plays the reaction: the flinch, the knockback, a Stalchild's head coming off (`Damage_Head`). The attacker's place in the record is where the player who hit stands (from their `PLAYER_SAMPLE`, not older than 2 s; the owner's place for a life that came down in a sample), so the knockback goes away from them. Only `Enemy_` actors: an animal, a pot or a crate gets its life written. A hit the game has not taken after 400 ms is written as life, and one that could not be given within 200 ms is dropped. If the game still takes that hit in the second after (it was paused, a text box was up), the extra fall is put back and is not sent on as a hit of ours. Why: in Caleb's run (2026-11-09, `caleb-logs/run2`) a hit in the game that did not own a Stalchild reached the owner as a bare life write (hit record all zero, no `Damage_Head`), and 0.5 s later the owner's `NoWeaponAttack` was played over the hitter's own flinch. So also: for 700 ms after our own hit on a copy, the owner's animation from before the hit is not played over it; a new one (the owner's reaction) is. Earlier live result with the switch on: B's game took 4 of 5 replayed hits through its own damage step. Not yet seen live as the default: the reaction on the owner's screen, and the head.
- **Every copy gets its call** (branch `enemy-copies`). The cave made at most 16 game calls a frame for all callers, and the Mover asked one per copy first: in Caleb's run (2026-11-09, `caleb-logs/run3`, 19 to 21 copies in Hyrule Field) exactly 16 of them were moved each frame, always the same ones, the rest never (a Peahat larva up to 32 m off), and no call was ever left for an animation ("0 plays"). Now the cave allows 256 (`CoopSvc::kCallsPerFrame`), the Mover asks for deletes and animations before poses, and the pose list starts one further on each frame. Read from the code, not yet seen live: the setter 0x4c6580 called with `(actor, 0)` already takes the teleport path (it passes `1 & ~w1` to 0x4c6630, which then stores the whole transform at body +0x1d0 and sets bits 0 and 1 of body +0x228).
- **A change of owner** (same branch). An `OBJECT_OWNER` with an older epoch than the one known is dropped, and so is a sample from an older epoch. When the owner changes, the samples kept from the old one are dropped, so the copy goes to the new owner's place with its first sample. An enemy that becomes ours stays where it stands here, takes the lower of its life and the handed-over one (never a higher), sends its sample at once, forgets a hit of ours not yet sent (it is already in its life) and ignores our own `DAMAGE` if the server hands it back to us.
- Not yet: holding a copy's own AI (it still chooses its own animation and target each frame; the Mover puts the owner's place and animation back); projectiles, the fight lock and bosses; the hit flash proven; B hitting A's enemy was not run in this round (the `DAMAGE` path of the first cut). Copies are moved to the owner's enemy wherever it is.

**Two-game test (Caleb, 2026-11-09, Hyrule Field at night, child saves).** Two Stalchild spawned in each game (hash ~0, so ids by the scene rule). Game A owned both; each sword hit in A lowered life in B, 2 to 1 to 0 (`coop: objects: share life` lines in B), and both died on both screens. Seen as not done yet: B shows no flinch and no head knocked off on the first hit (only the life value is written, not the hit reaction), and each game's Stalchild chase their own Link (positions are not written into copies).


## Protocol proposal

All on the channels [protocol.md](protocol.md) reserves: channel 2 (reliable) for ownership, spawns, removes and damage, channel 4 (unreliable, sequenced) for object samples. Type numbers are given when the code is written, after whatever other stages have taken by then (the next free is 17 today).

An **object id** is 10 bytes: `u8` kind (0 placed, 1 made), `u8` maker player (0 for placed), `u64` value (the placement hash, or the maker's counter). Every message that names an object starts with it, after the player field where there is one.

| Type | Sent by | Body |
|---|---|---|
| `OBJECT_PRESENT` | client | `u8` player field, scene (string), `u16` count, then per object: object id, `u8` flags (bit 0 awake, bit 1 in a fight, bit 2 targeting our player). Sent on every change and every 2 s; it replaces the sender's previous list for that scene |
| `OBJECT_OWNER` | server | object id, `u8` owner (0 none), `u16` epoch, `u32` group (0 none: the group root's index on the server) |
| `HANDOVER_ASK` | server | object id, `u16` epoch, `u8` to player |
| `HANDOVER_STATE` | client, then server | `u8` player field, object id, `u16` epoch, then sample components (below). The old owner sends it; the server relays it to the new owner and then sends `OBJECT_OWNER` to all |
| `OBJECT_SPAWN` | both | `u8` player field, object id (kind 1, maker = sender), actor kind (string, the game's actor name, up to 64), `u8` flags (bit 0 projectile), then sample components; `OBJECT_OWNER` is implied: the maker, epoch 0 |
| `OBJECT_SAMPLE` | both | `u8` player field, object id, `u16` epoch, then the components below |
| `OBJECT_REMOVE` | both | `u8` player field, object id, `u16` epoch, `u8` reason (0 gone, 1 dead, 2 exploded, 3 hit, 4 picked up), position (3 x f32) |
| `DAMAGE` | both | `u8` player field, object id, `u16` epoch the hitter knew, `s32` amount (0 or more), `u8` cause (0 unknown, 1 sword, 2 arrow, 3 fire arrow, 4 ice arrow, 5 light arrow, 6 seed, 7 bomb, 8 nut, 9 boomerang, 10 hookshot, 11 hammer, 12 magic, others kept), position (3 x f32) |

Object sample components, the same scheme and lengths as `PLAYER_SAMPLE`'s where they share an id:

| Id | Name | Data | Length |
|---|---|---|---|
| 1 | `time` | `u64` server time, `u32` sequence | 12 |
| 2 | `position` | 3 x f32 | 12 |
| 3 | `velocity` | 3 x f32, metres a second | 12 |
| 4 | `rotation` | 4 x f32 quaternion | 16 |
| 7 | `animation` | as in `PLAYER_SAMPLE` | 2 + string bytes + 8 |
| 16 | `life` | `s32` life, `s32` max life | 8 |
| 17 | `fuse` | f32 seconds left (bombs and bombchus) | 4 |
| 18 | `state` | `u8` flags: bit 0 in a fight, bit 1 held by a player, bit 2 asleep | 1 |

`time` is required. The owner sends `OBJECT_SAMPLE` at 20 to 30 a second while an object moves (60 for a boss), and once a second while it is still, so silence always means "late". Receivers place them like player samples: 100 ms behind, up to 150 ms of extrapolation.

What the server does:

- Keeps, per object: which games have it present, owner, epoch, group, the fight timer, the last relayed sample.
- Runs the rules above on every `OBJECT_PRESENT`, `PLAYER_STATUS`, `PLAYER_LEFT` and once a second, from the player samples it already relays (for distances) and the owners' samples.
- Relays `OBJECT_SAMPLE`, `OBJECT_SPAWN` and `OBJECT_REMOVE` only from the owner of that epoch (or the maker, for a spawn), to the players that have the object's scene; anything else is dropped.
- Relays `DAMAGE` only to the current owner; drops it when there is none. A `DAMAGE` with an old epoch still goes to the current owner: the hit was real.
- Forgets an object when no game has it present and its owner removed it or left.

The server's per-player limits (60 a second on channels 0 to 2, 240 on 3 and 4) are enough for a room of enemies at 30 a second plus projectiles; a busy fight is measured before they change.

## Next steps

In order, each its own branch:

1. **Live check, the rest.** Done for the slingshot, the nut, life, a sword kill, hashes and the list (above). Left: the other six projectiles, which need a save with them (run with `LICHII_COOP_DEBUG_OBJECTS=1` and read the `coop: objects:` lines), and the hook's reader run once in a build.
2. **Protocol.** Add the eight messages and the components to `coop/protocol/` with round-trip and refusal tests, and the rules to [protocol.md](protocol.md).
3. **Server.** The ownership table and rules, with tests driven by fake presence lists and positions: first owner, fight lock, out-of-fight hand-over with the hold, owner leaving, a hand-over whose old owner never answers, stale epochs dropped, damage only to the owner.
4. **The actor list.** Found (see "Live check"). The host library gets a reader for it, with the same guards as `coop_objects.cpp`.
5. **Projectile copies.** The `CoopShot_*` packs, spawned through the puppet's create command; the maker's side sends spawn, samples and remove.
6. **Enemy copies and damage.** Pose and life written into non-owned enemies from samples; the life-drop forwarding; the owner applying `DAMAGE`.
7. **Bosses.** Group lists per boss, tried one fight at a time.

## Not done

1. Only the slingshot's seed and the Deku nut were seen live; the other projectile names are from the data files. The hook's reader has not run in a build yet.
2. What adds a hit into Life + 0x228 (the step that applies it, 0x49d8c0, is found), and the stun paths of nuts and the boomerang. A direct life write is enough for the owner (live check), so this is only needed to learn the cause of a hit.
3. Where `ExecuteBarrelBombAI` keeps the remaining fuse.
4. The rest of 0xc16cf0's argument (the shooter, the start position and velocity), and how the shooter gives a new projectile its velocity.
5. The explosion effect and sound names, for copies.
6. When the game removes a dead enemy from the list (a ReDead at life 0 stayed for 20 s), and what actor state 6 means.
7. World effects of projectiles that are not saved flags (timed switches, torches, webs burnt).
8. Whether a placement hash is unique across scenes, or only within one.
