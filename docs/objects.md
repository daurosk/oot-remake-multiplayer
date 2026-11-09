# Shared enemies and objects: research and protocol proposal

Plan stage 5 ([plan.md](plan.md)): bombs, arrows and other projectiles seen on every screen, one owner per enemy with a real hand-over, and hits on a copy sent to the owner as damage events. This page says which actors the game uses, where their state is, how an actor is told apart on two games, and proposes the messages and the ownership rules. No product code yet.

Addresses are offsets into `main` of build `e0f3ccfaf50065ae` (1.1.1). **Checked** means read from the instructions or the data files. **Inferred** means taken from a name, a shape or the engine family. The game was not run for this page; see "Not done".

## Results

- Every player projectile is its own actor, created through the same function as everything else (0x372a70, [puppet.md](puppet.md)). Shot ones go through the shootable create at **0xc16cf0**, which passes the blackboard values `Shootable_KeyHash` and `Shootable_OverrideParamNum` with the request.
- The actors: `NormalArrow` (arrows, also the actor fire, ice and light arrows are shot as: inferred), `Shootable_DekuSeed` (slingshot), `Shootable_Hook` and `Shootable_HookLong` (hookshot and longshot tip), `Item_Bomb`, `Item_Bombchu`, `Item_DekuNut`, `Weapon_Boomerang`. The `Obj_*` and `Item_Arrow_*` packs of the same names are pick-ups.
- An actor's name is a `char*` at actor **+0x260**. Its life is an s32 at **`*(Life + 0x1c10) + 8`**, its maximum at `*(Life + 0x1c18) + 8`, where Life is component-table entry **+0x2b8** (table at actor +0x270).
- A placed actor's 64-bit placement hash, the key of its flag in [flags.md](flags.md), is a u64 at **`*(actor + 0x3d8) + 0x88`**. A game-made actor has 0 there.
- Proposal: projectiles stay with the game that made them for their whole life and are drawn elsewhere as harmless copies; enemies have one owner with a fight lock and a hand-over that carries the state; any life lost on a copy goes to the owner as `DAMAGE`. Eight new messages (below).

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
| A copy of life, max life | Life +0x1ca8, +0x1cac, written at 0x8d96a0 | Checked as stores; meaning inferred (the last values seen) |
| Placement hash | u64 at `*(actor + 0x3d8) + 0x88` | Checked: `OneShotInstanceGameDataSetBool` (0x28e64c4) passes it to the 64-bit-key setter 0x3761720, and uses an `InstanceGameParameter_%llu` name when it is 0 |
| The AI's actor | node +8, +8, +0x1f0 | Checked: 0xc70e70 and 0x833d60 |

The value at `*(Life + 0x1c10)` is an object of its own, not a plain field: its pointer is loaded at 103 places in 97 functions but written directly only by the reset. So damage is applied through some other function, not found yet (see "Not done").

Enemy damage is set in each pack's `Life/DamageParameters` (`IsMergeSameFrameHitInfo`, guard angles, burn, shock, freeze, impulse damage) and life in `Life/LifeParameters` (`MaxLife`; `Enemy_Bari` has 2). The game's damage pipeline has the AI queries `QueryGetLastExternalDamageData`, `QueryIsDamageTypeBomb`, `QueryDamageCalculatorIsOnDamageTypeAny` and others, all reading an external damage calculator. Names checked; the calculator was not read.

## Telling one actor from another on two games

| Kind | Id on the wire | Why |
|---|---|---|
| Placed (enemies, pots, chests, switches) | the u64 placement hash | Both games load the same Banc placement, and the hash is the key the flag store already uses ([flags.md](flags.md)) |
| Made by a player's game (projectiles, an enemy a player's action spawned) | the maker's player number and a u32 counter of its own | Nothing else names it on both sides; the maker says "I made 7" and everyone calls it (2, 7) |
| Made by an enemy (bullets, summons) | the same, made by the enemy's owner | The enemy's owner is the only game whose copy really fires |
| Made by the scene with no hash (spawners, waves) | not shared at first | Each game makes its own. If that shows (doubled waves, for example) it gets a rule of its own |

A placed id is only good inside one scene, since a scene can be loaded on one game and not the other. Messages that name a placed object say its scene once, in `OBJECT_PRESENT`.

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

1. **Live check (needs a run).** In the s1 hook, log every 0x372a70 call's name (`x1`) for a minute in Kokiri Forest while shooting the slingshot and in Dodongo's Cavern while throwing bombs: confirms the actor names. Read an enemy's +0x260, its life through the chain above and its placement hash; hit it and see life drop by the attack.
2. **Protocol.** Add the eight messages and the components to `coop/protocol/` with round-trip and refusal tests, and the rules to [protocol.md](protocol.md).
3. **Server.** The ownership table and rules, with tests driven by fake presence lists and positions: first owner, fight lock, out-of-fight hand-over with the hold, owner leaving, a hand-over whose old owner never answers, stale epochs dropped, damage only to the owner.
4. **The actor list (research).** The host needs every live actor each frame, or the creates as they happen. Not found: the Actor module (0x4e91920) and its step 0x3adf1f0 are where to look. Without it, the hook can record what 0x372a70 fills into each handle slot.
5. **Projectile copies.** The `CoopShot_*` packs, spawned through the puppet's create command; the maker's side sends spawn, samples and remove.
6. **Enemy copies and damage.** Pose and life written into non-owned enemies from samples; the life-drop forwarding; the owner applying `DAMAGE`.
7. **Bosses.** Group lists per boss, tried one fight at a time.

## Not done

1. Nothing here was run. The brief allows one portable instance to confirm actor names when a bomb is thrown; that needs step 1's create log, which the hook does not have yet.
2. The function that takes damage off life, and the stun paths of nuts and the boomerang. Writing life straight into `*(Life + 0x1c10) + 8` worked for TotK's owner side, but here life is behind an object of its own and has a second copy at Life +0x1ca8, so a direct write may be undone or skip the death.
3. Where `ExecuteBarrelBombAI` keeps the remaining fuse.
4. The rest of 0xc16cf0's argument (the shooter, the start position and velocity), and how the shooter gives a new projectile its velocity.
5. The explosion effect and sound names, for copies.
6. The live actor list.
7. World effects of projectiles that are not saved flags (timed switches, torches, webs burnt).
8. Whether a placement hash is unique across scenes, or only within one.
