# Play-test scenarios (agent tester, two or more games)

The scenarios an agent tester runs through the play-test bridge, in order. How to start a run and the tools are in
[docs/playtest.md](../docs/playtest.md), "Playing through the bridge"; the tester's rules are in
[AGENTS.md](../AGENTS.md), "Play-testing the game". Tools are written as `p1.walk_to(...)`: the tool on that game's
server (`oot-p1`, `oot-p2`, ...).

Every scenario:

- starts with `get_state` on every game (that holds them) and sets up with `warp`, `flag` and `give`, not by walking;
- uses a fresh `--log-dir` per scenario (for example `playtest-logs/<date>-<scenario>`), so `desync_check` compares
  only this scenario;
- takes each checkpoint on every game with the same tag, after `step 30` on the game that did NOT act (a held game
  receives nothing);
- ends with `desync_check` (all) and a `screenshot` of every game where the result is visible;
- writes its result into the run's report: PASS or FAIL, the steps, expected, actual, the first tag that differs
  with each game's frame, and the shot paths.

Coordinates are metres from earlier runs (`tools/autotest/scenarios/`). If a warp leaves Link elsewhere, read
`get_state` and walk in short legs with a clear line between them.

## 1. Both players open the same chest on the same frame

Setup: `p1.warp InsideTheDekuTree Entrance`, `p2.warp InsideTheDekuTree Entrance`. Map chest `Obj_TBox_A_02` at
6.51 108.40 7.25 (entrance room). `p1.walk_to 5.45 6.15`, `p2.walk_to 7.57 8.35` (both facing the chest; turn with a
short `press` stick toward it). Checkpoint `start`.

Act: `p1.press A 1` and `p2.press A 1` without a `step` between them: both games are held, so both presses land on
their own frame 1 after the hold. Then `step 120` on each, checkpoint `both_opened`, `step 300` on each, checkpoint
`settled`.

Expected: the chest is open in both games, opened once (one map, no second get scene that sticks), the map flag
`DungeonInfo.InsideTheDekuTree.IsGetMap` is 1 in both, `desync_check` agrees at `settled`. Shots: the lid is open in
both.

## 2. Player 1 picks up an item while player 2 changes scene

Setup: both `warp KokiriForest_Child FrontDekuTree`; `p1.give bombs`, `step 1`. Find a rupee or a pot near p1
(`get_state`, a shot). Checkpoint `start`.

Act: `p2.warp HyruleField_Child HyruleField_FrontOfKakarikoVillage` (the scene change runs), then p1 walks onto the
rupee (`walk_to`) or breaks the pot and walks over its drop. `step 60` on both, checkpoint `picked`; `p2.warp
KokiriForest_Child FrontDekuTree`, `step 120` on both, checkpoint `back`.

Expected: p1's count went up once; the picked thing stays gone for p2 after it comes back; no `desync_check`
difference at `back`.

## 3. Both players hit the same switch, or kill the last enemy of a room

Setup: both `warp InsideTheDekuTree Entrance`. Use a room with one floor switch or a last enemy that opens a door
(read the dungeon's flags from a first checkpoint). Checkpoint `start`.

Act: both players' Links on the switch (or both attacking the last enemy) in the same held frame, as in scenario 1.
`step 120` on both, checkpoint `hit`, `step 300`, checkpoint `settled`.

Expected: the door, chest or platform the switch drives is the same in both games; the room's clear or switch flag is
the same in both; no duplicated reward.

## 4. Player 2 disconnects in a dungeon and comes back

Setup: both `warp InsideTheDekuTree Entrance`; p1 sets a dungeon flag (`p1.flag DungeonInfo.InsideTheDekuTree.IsGetMap 1`).
`step 60` on both, checkpoint `before`.

Act: close p2's game (its server's `resume`, then the game's own quit or the window closed), keep p1 playing: p1
sets another flag or opens a chest, `step 120`, checkpoint (p1 only) `alone`. Start p2 again with the bridge on,
join, `p2.warp InsideTheDekuTree Entrance`, `step 300` on both, checkpoint `rejoined`.

Expected: at `rejoined` p2 has every shared flag p1 set while it was away; nothing p1 did is undone. (`alone` is
p1 only: desync_check calls it not compared, which is right.)

## 5. One player saves and reloads while the other keeps playing

Setup: both in `KokiriForest_Child FrontDekuTree`. Checkpoint `start`.

Act: p1 opens the pause menu and saves (`press PLUS`, then the menu's save with `press` and stick moves; a shot to
read the menu), then reloads its file. Meanwhile p2 `step`s and sets a shared flag. `step 300` on both after p1 is
back in play, checkpoint `reloaded`.

Expected: p1 comes back into co-op (state `coop` connected), has p2's flag, and nothing it saved is older than the
shared state.

## 6. The same flag set by both players under 300 ms of lag

Setup: start the run with the server's delay (`-Lag 300 -Jitter 30`, docs/playtest.md "Lag on purpose"). Both
`warp InsideTheDekuTree Entrance`, checkpoint `start`.

Act: in one held frame each, `p1.flag DungeonInfo.InsideTheDekuTree.IsGetMap 1` and
`p2.flag DungeonInfo.InsideTheDekuTree.IsGetMap 1`. `step 60` on both, checkpoint `early`; `step 300` on both,
checkpoint `settled`. Repeat with a value set to 1 by p1 and 0 by p2 in the same frame.

Expected: `settled` agrees in both repeats; the flag never flips back and forth (`early` may differ: lag).

## 7. Child and adult Link in different eras at once

Setup: `p1.warp KokiriForest_Child FrontDekuTree`, `p2.warp KakarikoVillage_Adult Kakariko_WellSide`
(`get_state` age: child and adult). Checkpoint `start`.

Act: each sets a flag of its own era (a chest or an event of that scene), `step 120` on both, checkpoint `set`; then
p2 `warp KakarikoVillage_Child Kakariko_WellSide`, `step 300`, checkpoint `same_era`.

Expected: shared story and dungeon flags agree; each game keeps its own Link's age; the puppet of the other player
is drawn as their age (shots).

## 8. Three or four players in the same room

Setup: four games with the bridge on (ports 5001 to 5004, or 5011 to 5014 on lane 1), servers `oot-p1` to `oot-p4`
on one `--log-dir`. All `warp InsideTheDekuTree Entrance`. Checkpoint `start` on all four.

Act: scenario 1's chest with p3 and p4 next to it, p1 opening; then p2 and p4 set two different flags in the same
held frame. `step 120` on all, checkpoint `four`; `step 300`, checkpoint `settled`.

Expected: `desync_check` compares four games and agrees at `settled`; every game sees three puppets in the right
places (one shot each).

## 9. Volvagia's falling rocks in each game (row 234)

Why: co-op shares none of the arena's rocks (no sync rule matches `Obj_FallingRock_*`), so each game drops its own,
at its own Link, wherever its arena is switched on. The open question is whether the game that does not own
Volvagia switches the arena on at all. Each game logs `coop: objects: rocks: N new falling rocks in our game since
the last line (M live)` at most every 5 s (real time) while new rocks come (needs commit 2920301c: in every build of
this line).

Setup: games from `live.py start --playtest` (servers with `--session`, for the `log` tool). Both
`warp VolvagiaStage VolvagiaStage` (the boss rush's arena: it places its own boss). Skip the boss title with
`press A` and `step 180`, three times on each game. `get_state` on both, then put the two Links apart: p1
`walk_to` about 8 m to one side of its start, p2 about 8 m to the other side (a clear line; read the positions
first). `log` on both (it starts the count from here), checkpoint `arena`, `screenshot` both (`arena`).

Act: let both games run the fight in turns, `step 600` on p1, then `step 600` on p2, and repeat. Read
`log match "rocks:"` and `log match "bosses:"` on both after each round. When either game logs rocks, take
`screenshot` on BOTH (`rocks-1`, `rocks-2`, ...), with no step between them, a few times. If no game shows rocks
after 6000 frames each, the rock phase needs the boss hit first: give the hammer (`give`, an item name the cheats
panel knows) and hit Volvagia when she surfaces, then go on. Note it in the report either way.

Expected (PASS): both games log `rocks:` lines during the rock phase, with counts of the same order. Each game's
shots show rocks landing around its own Link (p1's rocks near p1's Link, p2's near p2's), not only around the
owner. Report each game's lines with their frames, which game owns Volvagia (`bosses:` lines), and the shot paths.

Finding (report as row 234, not a fix): the game that does not own Volvagia never logs `rocks:` while the owner
does (its arena is not switched on), or rocks show on one screen only.
