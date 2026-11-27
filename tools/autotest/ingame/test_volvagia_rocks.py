"""Row 234 by position (volvagia-rocks-234, B #131): in the Volvagia fight each game's arena drops its falling rocks
at ITS OWN Link, not at one Link for both games.

test_boss_respawn.test_volvagia_rocks_each_game (boss-respawn-289-4) proves each game logs rocks and keeps shots; this
test adds what the row asks: where the rocks fall. Both Links stand apart in the arena (B placed 8 m in from the start),
both games' world lists are dumped every few frames during the rock rain, and each falling rock's place is compared
with the two Links' places at that frame. A game whose rocks fall nearer the other player's Link FAILS (one target for
both games), as does a game with no falling rock at all (its arena never switched on).

| Test | Proves |
|---|---|
| test_volvagia_rocks_fall_at_each_link | each game's falling rocks land at its own Link (>= 3/4 of them nearer it than the other Link); until MEASURED is cited: NO-REF, and FAIL only for one target (most rocks nearer the other Link) |

Sources (worker-rules "no guessed values"):
- Set-up, arrival, hearts: test_boss_respawn.start_check / boss_arrival / give_hearts + hearts_in (boss-respawn-289-4's, imported, not
  copied): the ten MaxLife gives + Life 56 so both Links live through the rocks (lane-c3-234-rocks died at 4 hearts).
- The rocks: docs/agent-handoff.md "Volvagia's rocks" and docs/boss-multiplayer.md row 234: Obj_FallingRockManager_A_03
  wakes a pool of up to 30 Obj_FallingRock_A_06 (made at load, ASLEEP) every 30-90 frames, aimed at the local Link
  (QueryDungeonGetPlayerPos + lead TargetAdvFrame). So a rock counts only while it FALLS (its height drops between two
  dumps): the sleeping pool is never judged. Name prefix as coop_objects.cpp NoteFallingRocks ("Obj_FallingRock_").
- The world list: docs/playtest.md "world": a `dump` has every actor the objects walk sees (up to 200, `other` kinds
  such as the rocks included), each with its `pos` [x, y, z] in metres (y = height); the dump's `state.pos` is our Link.
- Start point: lichii/hook/coop_start_points.inc {"VolvagiaStage", "VolvagiaStage", -64.51, 102.58, -0.01}; lane2 run
  20261125-114351 'A asked to come here: VolvagiaStage at -64.5 102.4 -0.0' (both games arrive there).
- B's spot: 8 m from the start toward Volvagia's placement {-80.629, 93.330, 16.306} (coop_difficulty_spots.inc,
  VolvagiaStage), i.e. into the arena. UNPROVEN as floor (no ground marker of VolvagiaStage in coop_difficulty_spots.inc),
  so the set-up proves it: 'coop: test place ...: done' (docs/testing.md `place`), then after 60 frames B in play, still
  within FLOOR_DY of the start height and at least MIN_APART_M from A; any miss = SETUP-FAIL naming it.
- 'await 10800' for the first rocks line: hs-volvagia-rocks.{A,B}.txt (the old row's wait), the line
  "coop: objects: rocks: N new falling rocks" (coop_objects.cpp NoteFallingRocks), needs LICHII_COOP_DEBUG_OBJECTS=1.
- Sampling: every SAMPLE_EVERY frames for at most SAMPLE_MAX frames, until each game saw MIN_FALLING falling rocks
  (30-90 frames between rocks: about 10 in 900 frames).
"""
import json
import math
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pair as P                   # noqa: E402
import test_boss_respawn as BR     # noqa: E402  (shared steps of boss-respawn-289-4; never edited here)
from test_boss_respawn import back_after   # noqa: E402,F401  (fixture: the way back gets its own deadline)

STAGE = "VolvagiaStage"
ROCK = "Obj_FallingRock_"                    # coop_objects.cpp NoteFallingRocks
START = (-64.51, 102.58, -0.01)               # coop_start_points.inc
VOLVAGIA = (-80.629, 93.330, 16.306)          # coop_difficulty_spots.inc
INTO_M = 8.0                                  # B's spot: this far from the start toward VOLVAGIA
MIN_APART_M = 6.0                             # Links further apart than a rock's spread around its Link needs
FLOOR_DY = 1.5                                # B still on the floor (lava is below: Volvagia's placement y 93.3)
FALL_DY = 0.3                                 # a rock FALLS when its height drops more than this between two dumps
FALL_XZ = 2.0                                 # ... and it stays over the same ground (a rock put back into the pool
                                              # jumps far: not a fall)
SAMPLE_EVERY, SAMPLE_MAX = 5, 900
MIN_FALLING = 3
ROCKS_FRAMES = 10800                          # hs-volvagia-rocks.{A,B}.txt 'await 10800 objects: rocks:' (the old row's wait)
# The test's budget, from its parts (boss-respawn-289-5 a637fe404: a fixed 150 s arrival ran out on lane 2):
#   the arrival BR.BOSS_ARRIVAL_S (its own cited ceiling) + the set-up and stand_apart (each within P.SETUP_LIMIT_S)
#   + ONE wait for both rocks lines (ROCKS_FRAMES of game, both games stepped together) + the sampling's own cap.
# SAMPLE_S: the sampling's WALL cap. Its cost per dump is NOT measured yet (no lane run reached the rocks); a run that
# hits it stops sampling and keeps what it saw (the notes give dumps and s/dump: the probe measures it, MEASURED cites
# it). It never decides PASS/FAIL by itself: fewer rocks seen = fewer judged (none = NO-REF).
SAMPLE_S = 2 * SAMPLE_MAX / P.FPS + 60.0      # 900 frames of game, twice (margin), + 60 s for the 2x180 dumps
BUDGET_S = int(BR.BOSS_ARRIVAL_S + 2 * P.SETUP_LIMIT_S + ROCKS_FRAMES / P.FPS + SAMPLE_S) + 1
OWN_SHARE = 0.75
# review-rows-c130 10:32 (1), the kokiri-band rule chain: MIN_APART_M, FALL_DY, FALL_XZ, MIN_FALLING and OWN_SHARE are
# NOT measured yet. Until MEASURED cites a probe run (run name + per-rock distances own/other, the spread), a run that
# meets them ends pair.unproven (NO-REF, never PASS); only a clear one-target game (most of its falling rocks nearer the
# OTHER Link) FAILS. A run with no falling rock seen is NO-REF too (a fall the 5-frame dumps missed is not the game's).
MEASURED = None


def b_spot():
    dx, dz = VOLVAGIA[0] - START[0], VOLVAGIA[2] - START[2]
    n = math.hypot(dx, dz)
    return (round(START[0] + INTO_M * dx / n, 2), START[1], round(START[2] + INTO_M * dz / n, 2))


def xz(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def stand_apart(pair):
    """B to b_spot() by the test `place`, proven: the place line, in play after 60 frames, on the floor, apart from A.
    Any miss = SETUP-FAIL (the row's own check never ran)."""
    try:
        pair.log["B"].mark()
        spot = b_spot()
        pair.act("B", "place %.2f %.2f %.2f" % spot)
        pair.step_both(2)
        pair.expect_log("B", r"coop: test place .*: done", max_frames=30)
        pair.step_both(60)

        def both_in_stage():                  # stepped, never one read (B #139 one-read class; review-rows-c130-2 16:31):
            sts = {x: pair.state(x) for x in ("A", "B")}   # a sample in an attack/landing/event is not ready YET
            ok = all(pair.in_play(s) and (s.get("scene") or "").lower() == STAGE.lower() for s in sts.values())
            return sts if ok else None
        sts = pair.until("both games in play in %s after the place" % STAGE, both_in_stage, max_frames=P.READY_FRAMES)
        a, b = sts["A"], sts["B"]
        for x, s in (("A", a), ("B", b)):
            assert abs(s["pos"][1] - START[1]) <= FLOOR_DY, "%s not on the floor: y %.2f (start %.2f)" % (
                x, s["pos"][1], START[1])
        d = xz(a["pos"], b["pos"])
        assert d >= MIN_APART_M, "Links only %.1f m apart (want >= %.0f m): A %s B %s" % (d, MIN_APART_M, a["pos"], b["pos"])
    except P.SetupFail as e:
        raise P.SetupFail("stand apart: %s" % e) from e
    except Exception as e:                    # noqa: BLE001
        raise P.SetupFail("stand apart (B to %s): %s: %s" % (b_spot(), type(e).__name__, e)) from e
    pair.notes.append("Links %.1f m apart: A %s B %s" % (d, a["pos"], b["pos"]))


def world_dump(pair, x, i):
    r = pair.call(x, "dump", tag="vr234-%s-%03d" % (x, i))
    with open(r["path"], encoding="utf-8") as f:
        d = json.load(f)
    st = d.get("state") or {}
    actors = (st.get("world") or {}).get("actors") or []
    rocks = {a["id"]: a["pos"] for a in actors if str(a.get("name", "")).startswith(ROCK) and a.get("pos")}
    return {"frame": d.get("frame"), "link": st.get("pos"), "rocks": rocks}


def sample(pair):
    """Both games' dumps every SAMPLE_EVERY frames until each saw MIN_FALLING falling rocks (or SAMPLE_MAX frames).
    Returns {x: [falling rock sightings: (rock id, rock pos, own Link pos, other Link pos)]} and the sample count."""
    seen = {"A": [], "B": []}
    last = {"A": {}, "B": {}}
    falling_ids = {"A": set(), "B": set()}
    shot = {"A": False, "B": False}
    i = ran = 0
    cap = P.Deadline(max(0.0, min(SAMPLE_S, pair.deadline.left())), clock=pair.deadline.clock)
    while ran <= SAMPLE_MAX:
        if cap.left() <= 0:
            pair.notes.append("sampling stopped at its %.0f s cap after %d dumps per game (%.2f s per dump pair)" % (
                cap.seconds, i, cap.seconds / max(1, i)))
            break
        now = {x: world_dump(pair, x, i) for x in ("A", "B")}
        for x, y in (("A", "B"), ("B", "A")):
            own, other = now[x]["link"], now[y]["link"]
            new_fall = False
            for rid, pos in now[x]["rocks"].items():
                before = last[x].get(rid)
                if before is not None and before[1] - pos[1] > FALL_DY and xz(before, pos) <= FALL_XZ and own and other:
                    seen[x].append((rid, pos, own, other))
                    new_fall = new_fall or rid not in falling_ids[x]
                    falling_ids[x].add(rid)
            last[x] = now[x]["rocks"]
            if new_fall and not shot[x]:      # the rock in the air over this game's own Link, on this game's screen
                pair.check_shot(x, "volvagia-rocks-234-fall", tolerance=0.80)   # 0.80: fire and moving rocks
                shot[x] = True
        i += 1
        if all(len(falling_ids[x]) >= MIN_FALLING for x in ("A", "B")):
            break
        pair.step_both(SAMPLE_EVERY)
        ran += SAMPLE_EVERY
    return seen, falling_ids, i


def judge(pair, x, seen, ids):
    """Game x: each falling rock's LAST sighting (nearest its landing) against x's own Link and the other Link.
    Returns (share nearer its own Link, how many rocks, the rows); None when it saw no falling rock. A clear
    one-target game (most rocks nearer the OTHER Link) FAILS here."""
    if not ids:
        pair.notes.append("%s: no falling rock in its world list" % x)
        return None
    lastpos = {}
    for rid, pos, own, other in seen:
        lastpos[rid] = (pos, own, other)
    near_own = 0
    rows = []
    for rid, (pos, own, other) in sorted(lastpos.items()):
        do, dt = xz(pos, own), xz(pos, other)
        near_own += 1 if do < dt else 0
        rows.append("%s: rock %s at %.1f m from its Link, %.1f m from the other" % (x, rid, do, dt))
    pair.notes.extend(rows)
    n = len(lastpos)
    assert 2 * near_own > n, ("%s: one target: only %d of %d falling rocks nearer its own Link, the rest nearer the "
                              "other Link: %s" % (x, near_own, n, "; ".join(rows)))
    return near_own / float(n), n, rows


ROCKS_LINE = r"coop: objects: rocks: \d+ new falling rocks"   # coop_objects.cpp NoteFallingRocks


@pytest.mark.budget(BUDGET_S)                 # from its parts (BUDGET_S above): arrival + set-ups + rocks wait + sampling
@pytest.mark.look
@pytest.mark.needs_env(BR.OBJECTS)
@pytest.mark.row("#234 Volvagia's falling rocks at each player's own Link (positions from the world list)")
def test_volvagia_rocks_fall_at_each_link(pair, back_after):
    clock = pair.deadline.clock
    with pair.setup():
        BR.start_check(pair)
        BR.give_hearts(pair)                  # the one heart set-up of the row 234 tests (review-rows-c130 10:32 (2))
    BR.boss_arrival(pair, STAGE, then=lambda: BR.hearts_in(pair, "after the arrival"))
    stand_apart(pair)
    for x in ("A", "B"):
        pair.log[x].mark()
    t = clock()                               # ONE wait for both games' lines (both are stepped together), not two
    pair.until("both games' rocks lines /%s/" % ROCKS_LINE,
               lambda: all(pair.log[x].find(ROCKS_LINE) for x in ("A", "B")), max_frames=ROCKS_FRAMES)
    pair.notes.append("rocks lines in both games after %.1f s" % (clock() - t))
    t = clock()
    seen, ids, n = sample(pair)
    pair.notes.append("%d dumps in %.1f s; falling rocks A %d, B %d" % (n, clock() - t, len(ids["A"]), len(ids["B"])))
    got = {x: judge(pair, x, seen[x], ids[x]) for x in ("A", "B")}
    none = [x for x in ("A", "B") if got[x] is None]
    if none:
        pair.unproven("no falling rock seen in %s's world list (rocks lines came; a fall the %d-frame dumps missed is "
                      "not the game's)" % (" and ".join(none), SAMPLE_EVERY))
    if MEASURED is None:
        pair.unproven("judge values not measured yet (MIN_APART_M, FALL_DY, FALL_XZ, MIN_FALLING, OWN_SHARE): nearer own "
                      "Link A %.0f%% of %d, B %.0f%% of %d" % (100 * got["A"][0], got["A"][1], 100 * got["B"][0], got["B"][1]))
    for x in ("A", "B"):
        assert got[x][0] >= OWN_SHARE, "%s: only %.0f%% of %d falling rocks nearer its own Link (want >= %d%%, %s): %s" % (
            x, 100 * got[x][0], got[x][1], int(OWN_SHARE * 100), MEASURED, "; ".join(got[x][2]))
