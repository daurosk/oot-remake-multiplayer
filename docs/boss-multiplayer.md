# Bosses built for more than one player (design notes, 2026-11-21)

The question asked of every boss stage: **is it fun with two to four players, and if not, how should it change so
the fight feels built around a group** (the way Four Swords builds its fights around several Links)? This page is
the plan; [dungeons-boss.md](dungeons-boss.md) says how the sync works today.

## What every fight has today

- One game runs the boss (its owner); the others show a copy that follows it (native boss sync). Hits from every
  player count, the death plays once in each game, the heart container is one per player.
- **Boss turns** (#308 and `boss-turns-0119`): the boss goes to the next player by number at the end of **every**
  attack in its owner's game (4 s at least with one player, `boss_turn_us`, so a quick combo is not split), so its
  AI fights each player in turn. A player who is down, dead or paused is skipped, and is in the round again at the
  next attack's end once up (their puppet is seen by enemies again then too).
- **Never stuck on a player who cannot fight** (2026-11-21, every enemy): a boss, or any enemy in its fight, leaves
  an owner who goes down or dies at once and one paused after 2 s (`fight_owner_dead_us`, `fight_owner_paused_us`;
  out of a fight the old 10 s and 6 s), for a player up and playing. With nobody up it stays where it is.
- **Nor on a player held in a cutscene of their own** (row 164 follow-up): an item get, Navi's talk or a late intro of their own game holds the owner's Link, and the boss's AI (which fights that Link alone) stood with it on every screen. A boss whose owner has been `in_cutscene` for 3 s (`fight_owner_cutscene_us`; 0 off) goes to a player up, playing and in no cutscene. Never while the boss itself plays an event or its death or is at life 0 (its own intro, phase change or death: every game is in that one, the owner's AI is in it on purpose), and never to a game in a cutscene too. Only a boss whose AI stands in that event: never in its attack (`attack_hold_max_us`), never while a piece of it holds its owner's Link (row 232), and only once its owner's samples show one move (animation, action verb, life) unchanged for those 3 s, so a boss that grabs or throws its owner's Link (Morpha's tentacle: an event of about 6 s in its owner's game) keeps it. Stat `boss_from_cutscene`. Tests: `session_test.cpp` (row 164 follow-up).
- **Plain enemies can take turns too, OFF by default** (`enemy_turns`): the same rule as a boss's, between its
  attacks, among the players within 12 m of it (`enemy_turn_m`). It stays off: a plain enemy's AI chases only its
  owner's Link, so every turn walks it over to the other player and back (the shuttle of report 138, docs/objects.md
  "Sticky target"), where solo a mob keeps the Link it fights; the chase, near and sticky rules already give it to
  the player fighting it. On, the server's `enemies:` line counts them as `enemy turns`. (Also: only a boss or part
  is listed with `object_presence::takes_turns` by the hook, so plain enemies would need that flag first.)
- **A stun hands the boss to the stunner, also right after a turn:** a stun in another player's game (Gohma's eye,
  a ball sent back into a copy Phantom Ganon or Ganondorf) asks the boss over at most every 8 s
  (`boss_hitter_cooldown_us`), counted from the last change that was no turn (`Object::hitter_changed_at`): with a
  turn at every attack, a count from every change refused nearly every stun.
- The difficulty scales by the number of players (2 players: boss life 200%, damage 120%).

What that does not give yet: **an attack aimed at more than one player at once**. The game's AI picks one target,
and a shot or a grab is real only in the owner's game (a copy's own shots are refused, the owner's come as copies),
except the split shots below.

## Tools we have or can build

| Tool | What it does | State |
|---|---|---|
| Turns | the boss fights each player in turn, between attacks | in (proof runs) |
| Turn length | per boss: every attack, every second attack, every N s | to build (a server table by boss) |
| Split shot | an attack's shot made in **every** game, aimed at that game's own Link: each player gets the ball / the rock / the beam at the same moment | in for Phantom Ganon's bolt, shockwaves and balls and Ganondorf's `DungeonBossGanondrof_Bullet_*` (#308, `CoopEnemyShots::IsSplitShot`); the rest to build |
| Half split | a split shot in half the games, the other half the next time (every other attack) | to build on split shot |
| Part targets | a boss of several bodies (Bongo Bongo's hands, Twinrova's sisters, Morpha's tentacles) gives each body its own target player | to build (owner per group member) |
| Stun for all | a stun in one game stuns the boss for everyone (the stun hand-over already moves him to the stunner) | in |

## Boss by boss

### Queen Gohma (Deku Tree, child)
- **Ceiling / wander:** she crawls toward her target and drops eggs. With turns she goes after each player; the
  eggs hatch in every game. Fun: yes, one player stuns the eye with the slingshot, the others slash.
- **Change:** none needed. Later: more eggs with more players (the enemies scale says 125%).

### King Dodongo (Dodongo's Cavern, child)
- **Inhale / fire breath:** aimed at one player; any player can throw the bomb in. **Roll:** chases one player.
- Fun with turns: yes (whoever he faces is the bait, another bombs him). **Change:** turn after **every** roll and
  every breath (turn length = every attack), so nobody waits a whole minute.

### Barinade (Jabu-Jabu, child)
- **Tentacles (3), jellyfish ring, body:** the beam is aimed at one player.
- **Change:** part targets for the tentacles' zaps (each player cuts one); the jellyfish ring spins toward its
  target, so turns every attack. Not tested yet (no save with Barinade's room before his fight).

### Phantom Ganon (Forest Temple, adult)
- **Stage 1, paintings:** he rides out of a painting toward his target; the others can shoot him. With turns each
  player gets a ride at them. Fun: yes.
- **Stage 1, the leap:** he leaps out of a painting and throws a bolt at the room's middle; four shockwaves run
  out from it in a cross. As copies flown on the owner's path the shockwaves hurt nobody but the owner. **Done
  (split shot):** every game makes its own bolt and its own four shockwaves, seen and felt in that game only, so
  each player dodges one cross, the same one, at the same moment.
- **Stage 2, the ball rally:** today one player rallies with him, the others watch. **Done (split shot):** each
  player gets their own ball at the same moment, seen and played in their own game only. The owner's Ganon sends
  it back as alone; a copy's cannot (his AI runs in the owner's game), so a ball sent back into him stuns him in
  that game and the stun hand-over gives him to that player for the sword hits.
- **Stage 2, the goal (asked for, the hardest):** he does not go down until **half of the players** have sent
  their ball back into him. Each game reports its return to the server (a new message, or a DAMAGE 0 with the
  rally's round), the server counts the players of that round, and the knockdown is held in every game (the
  owner's too: its own stun reaction waits) until the count reaches half (2 of 2 or 3, 2 of 4); then everyone sees
  him fall at once and the one who made the last return owns him. Needs: a way to hold the game's own knockdown on
  the owner's Ganon (patch 0007's Life step hold is for life, not for the reaction), and a round count per rally.

### Volvagia (Fire Temple, adult)
- **Holes / head out:** the hammer player hits the head; **flight + fire breath:** at one player; **falling rocks:**
  around one player.
- **Change:** split shot for the falling rocks (rocks around every player); turns every flight.

### Morpha (Water Temple, adult)
- **Tentacle grabs:** one target. **Nucleus:** one player hookshots it out, another slashes it: the best
  two-player moment of the game already.
- **Change:** part targets: with more players, one tentacle per player (the game makes two late in the fight);
  turns every grab.

### Bongo Bongo (Shadow Temple, adult)
- **Two hands, the eye:** both hands must be stunned, then the eye shot. Built for two players already: one takes
  the left hand, one the right.
- **Change:** part targets: the left hand goes for one player, the right for another (today both go for the
  owner). Turns between slams.

### Twinrova (Spirit Temple, adult)
- **Stage 1, Koume and Kotake:** fire and ice shots, three of one kind charge the Mirror Shield.
- **Change:** part targets: each sister has her own target player (two players each duel one sister). With more
  players the sisters swap targets every attack.
- **Stage 2, Twinrova:** one body, one target: turns every attack. Later: a charged Mirror Shield blast from any
  player stuns her for all (stun for all).
- **Row 222 (sisters held in an attack, the fight never ends; reports/twinrova-222-2.md):** each game's
  `Enemy_KotakeKoumeBattleManager` chooses the sisters' battle state from its own Link, and only their brain and
  action verbs are synced. Option C (enemies-01110, the safety net): the manager is flagged
  `object_presence::battle_manager` and the server moves its group only from an unfit owner (paused, dead,
  loading, gone) or an unready one, never by distance, a nearer player, a chase or a stun hit, so a new owner's
  manager no longer takes over mid-fight on a state it never knew. Option A (one manager decides for every game,
  its orders carried through the game's own message path) is its own branch, twinrova-a-01110.

### Ganondorf (Ganon's Tower)
- **Energy ball rally, multi-ball, light arrows:** the same as Phantom Ganon's stage 2 (asked for): his ball at
  every player at once (split shot; `DungeonBossGanondrof_Bullet_` is on the list, his own `Ganondorf_Bullet_*`
  names to be read in a run), and he goes down only when half of the players have sent theirs back into him; a
  light arrow from any player stuns him for all.
- **Row 211 (a ball sent back by a player who does not own him):** the ball is a split shot, so the hitter's own
  ball goes back natively in the hitter's game, into that game's copy Ganondorf. Its hit there is his own
  weak-point interrupt (interruptweakpointhit, `DownTimeAtLightBallHit` / `DownTimeChargeLightBallHitArray`), and
  it plays `LightBallDamage_Start/Loop/End`. The objects walk did not read that name as a stun
  (`CoopObjects::IsStunAnimation`), so the copy's own stun was never told to the owner. Nobody got him handed over,
  the owner's poses ran over the stun, and the return did nothing. Now the name is a stun, like the others with
  another first word (Ganon's light-arrow downs, `StunA`.. of the Lizalfos and Gerudo Thief, Big Octo's
  `ShortStun`). Phantom Ganon's ball knockdown is `Down_Fall*` / `DownTimeReaction*`, which was read already.
  Open: his volley (`Parry`, an action of his own AI) runs only in the owner's game, so in a copy game the first
  ball sent back stuns him.
- Not tested yet: the boss rush stage did not place him after a `warp` (no boot event; see "Boss rush stages reached by
  a warp" below). The route now asks `DreamBossStage_Rematch_Ganondorf_Start` after the warp.

### Ganon (final battle)
- **Stage 1:** he faces one player; the tail is the weak spot. With turns the player he faces is the bait and the
  others go for the tail: the most Four-Swords fight in the game as it is.
- **Stage 2 (sword lost):** same; turns every swing.

## Findings of the every-stage runs (2026-11-21, two games, the boss rush stages)

- **Synced and dying once for both, with turns at attack ends:** Phantom Ganon (stage 1; stage 2 needs real arrow hits
  in the paintings, a test hit only takes life), Volvagia, Morpha, Bongo Bongo (hands and body change owner
  together), Twinrova (both stages, once her merge was shared), Ganon (both halves).
- **Fixed:** Twinrova's merge (`SageOfSpirit_BossChange`) played in one game only, so the other had no Twinrova;
  every boss's single shots are split shots now; every enemy takes turns and leaves a dead or paused owner at once.
- **The boss rush is not harder by itself:** the bosses are the same actors with the same placement parameters as in
  their dungeons (Phantom Ganon's differ only in `DefaultDungeonRoomIndex`), and no parameter table names the rush;
  only the rush rooms' own logic graphs do. Co-op's scaling applies the same to both.
- **King Dodongo in his rush stage stays in `Wait`** with one player alone too (a single-game run): not co-op. His
  stage's logic (`DodongoStage_497d`) makes him appear through `DreamWorld_BossAppear` from a player's area trigger.
- **Volvagia at 2 players is very hard standing still:** both test Links reached 0 life several times in her fire
  (her breath and floor fire are each game's own, so every player is attacked). Worth a look in the difficulty
  review.
- **Not placed from the test saves:** Barinade and Ganondorf. Not the saves: the warp (boss-route-01112, below).

### Boss rush stages reached by a warp (boss-route-01112, 2026-11-23)

The game's own rematch (`DreamBossStage`'s `Dream_Navi_03` / `NaviHint_Rematch_Sub`, through
`SubEvent_AllBossBattle:BossStageEnterBase`) changes to a stage with three arguments: `NextBanc`, `NextStartPos`
(`<Boss>Stage`, the start point the autotest's `warp` uses too) and `BootEventName`, the event played on arrival. The
autotest's `warp` (and the module's own warp, `coop_testinput.cpp` Warp) writes only the scene and the start point, so
no boot event plays. Writing `Sequence_ReservedBootEventName` into the request flags does not help: the game never
played it on that path (docs/scenes.md, #197). Each boss's `DreamBossStage_Rematch_<Boss>_Start` turns
`DreamWorld_BossAppear` on and calls `EventTriggerSetWasAppearInEvent` on the boss. The stage's root logic makes an
event-appear boss (`IsIntendedForEventAppear`) from a player in its area AND `DreamWorld_BossAppear`.
Most stages also have an asker of their own for the boss's appear event, so their boss comes without the boot event.
Two have none, so a warp leaves them empty:

| Stage (the autotest warps there) | The rematch's boot event | Appear asker in the stage | After a warp |
| --- | --- | --- | --- |
| GohmaStage | `InsideTheDekuTree_BossAppear` | (Gohma is not event-appear) | placed |
| DodongoStage | `Get_StoneOfFire_AppearBoss` | `Get_StoneOfFire_AppearBoss` | placed, stays in `Wait` (above) |
| BarinadeStage | `DreamBossStage_Rematch_Barinade_Start` | none | **no boss** |
| PhantomGanonStage | `ForestTemple_SageOfForest_PhantomGanonAppearance` | the same | placed |
| VolvagiaStage | `SageOfFire_BossAppear` | the same | placed |
| MorphaStage | `Demo_SageOfWater_BossAppears` | the same | placed |
| BongoBongoStage | `ShadowTemple_BossAppear` | the same | placed |
| TwinrovaStage | `SageOfSpirit_BossStart` | the same (Twinrova herself is not event-appear) | placed |
| GanondorfStage | `DreamBossStage_Rematch_Ganondorf_Start` | none | **no boss** |
| GanonStage | `InsideGanonCastle_Dungeon_BossAppear_2` | the same | placed |

The full boss rush (`BossRush_01..03`, `BossRushBossStageExitSuccess*`) boots every stage with
`DreamBossStage_Rematch_TimeAppear` instead. So the routes `hs-barinade-native` and `hs-ganondorf-native` ask the
rematch's boot event after the warp with the test step `event NAME` (docs/testing.md). Not a co-op bug: a player
reaches these stages only through the game's own rematch, which plays the boot event. Open, not changed:
`/teleport` into a boss rush stage arrives with the plain `Dm_SY_BancChangeArrive` (`CoopTeleport::ArrivalEvent`).
The target refuses while it fights (`cannot_warp`) or plays an event (`busy`), and after the boss is beaten the
plain arrival is right; the rematch's boot event there would make a beaten boss appear again. Only a player who
teleports in after the target's arrival but before its fight starts would get a stage without Barinade or Ganondorf.
That needs a game check before any change.

## Whole-game survey (2026-11-21): every enemy, room event and push puzzle

Three surveys of the romfs (every map, actor pack and event flow) for places tonight's fixes apply. Applied without
a game run (unit tests only), for players to check:

| What | Where it went | Check in a game |
| --- | --- | --- |
| Never mid-attack: no enemy changes owner by a chase, the near rule or a nearer player while its owner's attack runs (a grab or hold stays on its victim), at most 8 s | server `attack_hold_max_us`, `mid_attack` | B hits a Like Like holding A: A freed, A moves after |
| Not applied (geno-fix-0119): boss-style turns for one-on-one fighters (Iron Knuckles, Poe sisters Amy/Beth/Joelle, Stalfos, Lizalfos, Dinolfos, white Wolfos, the club Moblin). Their AI is a plain enemy's (it chases only its owner's Link), so boss rules would pass them between players: the mob shuttle the owner settled off (enemy_turns off) | none: they are plain enemies | no "boss turns:" lines for them |
| Packs under one owner: Lizalfos/Dinolfos, Stalfos and Wolfos of one family within 25 m are one group (root: the lowest id), so their teamwork (messages between their AIs: `IsRequestingCoop`, `IsDisableMeleeBattle`...) runs whole in one game (never the boss bit: plain enemies, no turns; geno-fix-0119) | `PackFamily` in `GroupsOf` | one owner for both of a pair; one attacks at a time |
| Anubis keeps its target (it mirrors the player moving it) | `KeepsTarget` in `Sync::StateOf` | the mirror follows one player |
| Ganon bound for the last blow (`InsideGanonCastle_Dungeon_BossBind0`) shared like the other stage changes | `coop_boss_scenes` entry 12 | both games' Ganon bound |
| The Flare Dancer's fireballs split | `kSplit` | each player gets their own fire |
| The Goron City statue's moves shared | `IsMovable` | both see it pushed |
| Capstans' turns shared (Spirit Temple mirrors, Shadow Temple room 1 statue) | `IsCapstan` shifters | the turn shows; the beam and the "correct" check in the game that follows |
| Per-player room flags no longer shared: the Dark Link rooms' passage flags, the Shadow lifts' crush flag | `coop_dungeon.cpp` | B in room 22/23 while A passes a Dark Link door; B on the boat while A is crushed |

Found and left as they are: Octorok and Deku Scrub shots stay shared (the player shot at sends them back); the
Wallmaster carry-back and the Gerudo Fortress loss are per player; every block puzzle's answer counter is shared;
no other actor reads its state only at load (the turning floor's fix fits nowhere else); no other event turns a
room under a player (the hallway block fits nowhere else); the water level stays a change for everyone.

Still to design (need a game run first):
- Redead and Gibdo (row 230, enemies-01110): the scream froze only the owner's Link. Read from the romfs: the
  scream is the Redead's own sensor attack (`Redead.action.sleepparalysis`: `ExecuteSensorHitSetupAttack`, AS node
  `Shout`/`CryStanding`) on its AI's target, which is only its own game's Link; the player takes it as a hit and
  runs its own `SleepParalysis` reaction (Player ConditionParam: 210 frames, 120 invincible). Its reach is the
  Redead's RedeadParam: `ParalysisEnableDistMax` 5.5 m, `ParalysisEnableAngleDiff` 45 degrees (the same in town;
  the Gibdo's param has the Redead's as its parent). Now: the owner's game reads each new hit on its own Link (its
  Life's last hit id) with its attacker; a hit from its own Redead/Gibdo while that one plays the scream (or one
  that took no life outside the bite's `Attack_*`) goes in that enemy's samples as object component 22 `strike`
  (its kind words). Each other game whose Link stands in that reach of its copy gives its own Link that hit with
  no life (`HurtOurLinkNative`, as PvP's no-life kinds). Log lines `coop: objects: scream: ...` on both sides.
  The bite is not shared (the other Links are not held). Unit tests only; to check in a game: B within 5 m in
  front of a Redead A owns, A walks up until it screams: both Links freeze. (Geno's probe saw no scream in 50 s at
  4 m: the scream needs the Redead to have noticed its target and face it within 45 degrees.)
- Meg (Forest Temple; row 231, enemies-01110): her decoys (`Enemy_MiddleBoss_Poe_Meg_Dummy`, no BSA, MaxLife 1,
  their own AI that goes to the seat Meg's message names: `ExecutePoeMegMessageReceiver`, `MegDummy_SeatId`) are
  made anew each cycle by each game's Meg, which rolls its own layout, and got ids by order (`SceneSpawnId`), so
  three made in one frame matched differently between games. Now (Geno's MinionMaker idea) `MinionMaker` maps the
  decoy to Meg: only her owner's game keeps its decoys and shares them (made ids, as Gohma's eggs); a copy game
  deletes its own and makes the owner's, so every game shows the owner's layout. His worry (a copy's Meg remaking
  them in a loop) is logged for every boss minion: more than `kMinionLoopMax` (12) of one minion deleted within
  3 s in one game gives one `minions: ... (a loop): still deleted, the owner's kept` line a scene; ours stay
  deleted (matching them by order again would bring row 231 back and show the owner's decoys beside ours). A copy's
  decoy made on the seat of an owner's decoy being made waits up to `kCreateWaitUs` (it may be the director's copy)
  and goes once the copy is bound. A turn mid-cycle: the decoys made so far stay their maker's (made ids); those
  made from then on are the new owner's to share. Unit tests only; to check in a game: B watches A's Meg split: the same four
  places on both screens; no `a loop` line.
- Dead Hand (row 232, enemies-01110): his hands were boss parts of him (`PartRules` kSummoned), so they followed
  his owner and grabbed only that player. They are placed enemies with their own AI, BSA and perception
  (`DeadHand_Hand.action.basicattack` grabs the target it perceives: its own game's Link), so they are plain
  enemies now: each goes to its nearest player, whose game grabs that player. Dead Hand stays a boss by name
  (`IsBossOrPart`). A grab tells its own game's Dead Hand (`OneShotDeadHandHandSendMessage` `MessageAppear`); he
  rises for it where he is owned. A game whose own hand holds its own Link lists Dead Hand with
  `object_presence::grabbed_here` (a `grab:` log line), and the server hands him to that game once his attack in
  his owner's game has ended (`Session::grab_owner`, `boss grab:` server line, `boss_to_grabbed`), unless his
  owner's Link is held too (then his turns pass him on). The reverse (he sinks or is hit, and his hands let go): his
  own game's AI tells its own hands; a hand owned by another game is a copy there and follows its owner's motion, so
  the hand that holds a Link is in the same game as Dead Hand once the grab rule has moved him, and his own message
  reaches it. Unit tests only;
  to check in a game: A and B each near a different hand: both grabbed; Dead Hand rises for each in turn.
- Water Temple and others (row 233, enemies-01110: probe): the dungeon pieces whose `Presence.FlagName` is a
  shared flag are now listed from the romfs (`tools/puppet_pack/presence_gates.py` ->
  `lichii/hook/coop_presence_gates.inc`: 24 rows, 12 flags: the Water Temple's planes, waterfall, Room22 shutter
  and air wall; the Shadow tower's pillar, rubble and altar steps; Bottom of the Well's water; the Forest Temple's
  room 7 water and elevator; two Ice Cavern pots and their red ice; a Jabu-Jabu air wall; all `IsCheckLazy`
  false). Whether the game takes such a piece away or puts it in when the flag changes in play, or only at the
  next load, is the live check, and no fix is made before it (deleting or making a placement live is proven for
  enemies only, not for terrain collision). Each game logs `coop: objects: presence: <flag> (<area>) went 1, now:
  <piece> wanted|unwanted (N of M here) ...` when one of those flags changes, and again 3 s later. Run: Water
  Temple, A raises or lowers the water at a symbol while B stands in the central room: the owner's lines show
  what a game of one does, B's whether the same happens from the shared flag alone.
- Shadow Temple's falling pillar (`Dm_ShadowTemple_0015`): in the probe's rows above (TowerBreak groups).
- Volvagia's falling rocks (row 234, enemies-01110, code read): made by the arena (`Obj_FallingRockManager_A_03`,
  a pool of `Obj_FallingRock_A_06`), aimed at the local Link (`QueryDungeonGetPlayerPos`), switched on by
  Volvagia's `FallRockActivate` / `FallRockEffectActivate` plugs. No co-op rule matches `Obj_FallingRock_*` (not a
  breakable, movable, faller or enemy), so each game drops its own rocks at its own Link wherever its arena is on.
  What is left is whether a copy game's Volvagia (native boss sync: the owner's verbs, its own action modules)
  switches its arena on: a `coop: objects: rocks: N new falling rocks` line, at most every 5 s, now shows it per
  game. To check in a game: both logs show rock lines during her rock attack, the owner's and the other's.

## Order of work

1. **Turns between attacks** (Dauro's `boss-turns-0119`, merged in our branch): prove in every boss's every stage
   with two games (proof runs per boss).
2. **Turn length per boss** (every attack for Dodongo's roll, Volvagia's flight, Morpha's grab; every second for
   the long ones), a server table.
3. **Split shot** for Phantom Ganon's and Ganondorf's ball, then Volvagia's rocks; half split as a setting.
4. **Part targets** for Bongo Bongo's hands, Twinrova's sisters, Morpha's tentacles, Barinade's tentacles.

## Testing every stage

Each boss gets a proof run that walks it through every stage with two games: the test `hit` command takes it to
each stage's life (as `hs-boss-turns-pg` takes Phantom Ganon 42 -> 12 into the rally), both players watch it act
there for a minute (turns, its attacks at each player), then the kill. Checked per stage: the same move on both
screens, an attack at each player, a turn at an attack's end, no stall, one death.
