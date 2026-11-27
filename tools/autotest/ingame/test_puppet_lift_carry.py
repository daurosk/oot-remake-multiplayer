"""Native puppets N5 lift carry (docs/native-puppets.md "Lift carry"; lichii/hook/coop_platform_carry.h, fed by
coop_objects.cpp, applied in coop_puppet.cpp DrawAt): the other player's copy stands ON a moving lift of ours, not
sunk into it (going down: floating over it; going up: inside it) by the lift's speed x the copy's show delay.

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_puppet_lift_carry | (new: plan N5, puppet-body-4) | default (carry is on for every copy) | world list (A's lift + copy) + module log + LOOK shots of both games |

Place: the Jabu-Jabu moving floor `Obj_InsideJabu_Jabu_MoveFloor_A_01`, placed at FLOOR (romfs Banc/
InsideJabuJabuBelly_Static, read in reports/puppet-body-4.md "In-game check list"); solid floor beside it at the
`SwapRestartPosAnchor` ANCHOR (3.56 m off: outside CoopLiftRide's 1.8 m rider footprint, coop_lift_ride.h kRideRadiusM).
It starts on RideOnPlayer (coop_lift_ride.h header: 0 -> 1 when Link steps on), so B's Link on it moves it in B's game,
and #65's lift ride moves A's (coop_lift_ride.h: another player's copy on our lift sets the same plug).
Steps (B is the copy, P<n> in A's game):
- set-up: both warp to Jabu-Jabu (coop_start_points.inc:78), A to ANCHOR, B onto FLOOR; A's world list has the lift
  within FIND_M of FLOOR and the copy; the lift moves in A's game within MOVE_FRAMES (else SETUP-FAIL "did not move").
- ride: every SAMPLE frames, from ONE world dump of A (the lift and the copy from the same walk): offset = copy y - lift
  y. Still samples (lift moved <= STILL_DY over SAMPLE) give the copy's rest offset (median); moving samples (lift
  speed >= V_MIN) are compared to it. Without the carry a moving sample is off by speed x delay (>= 100 ms: docs
  "Lift carry"), so a deviation >= half of that at the measured speed is a clear FAIL (the copy sank/floated), as is no
  `P<n> lift carry: on our lift` line in A's log (coop_puppet.cpp DrawAt). Shots of both games mid-ride.
The ESTIMATED values (MOVE_FRAMES, RIDE_FRAMES, V_MIN, CARRY_TOL) are starting values (worker-rules: no guessed values;
as test_native_body.py): until MEASURED cites a lane run's notes, a run whose clear checks hold ends NO-REF
(pair.unproven) with the measured values, never PASS; a CARRY_TOL miss is listed there, not failed.
"""
import re
import statistics

import pytest

import pair as P
from pair import FPS, SetupFail     # (30: the game's calc rate on Hookshot, pair.py)

ON_LIFT = r"coop: puppet: P%d lift carry: on our lift, carried [0-9.]+ m \(show delay (\d+) ms\)"  # coop_puppet.cpp DrawAt
SCENE = "InsideJabuJabuBelly"                       # coop_start_points.inc:78
ENTRANCE = "InsideJabuJabuBelly_FromZoraFountain"   # coop_start_points.inc:78
LIFT = "MoveFloor_A_01"                             # Obj_InsideJabu_Jabu_MoveFloor_A_01 (romfs, report above)
FLOOR = (0.32, 91.0, -42.63)                        # its placement (romfs Banc/InsideJabuJabuBelly_Static)
ANCHOR = (-3.24, 91.0, -42.68)                      # SwapRestartPosAnchor beside it (same file)
ON_M = 0.1                                          # B placed this far over the floor's place (falls onto it)
FIND_M = 1.0                                        # the lift in A's world list this close to FLOOR (flat)
MIN_DELAY_S = 0.1                                   # docs/native-puppets.md "Lift carry": show delay >= 100 ms
BACK_TO = "KakarikoVillage_Child"                   # test_smoke.WARP_TO: the lane's boot map (when the boot names none)
SAMPLE = 5                                          # frames between world dumps
STILL_DY = 0.005                                    # m per SAMPLE: the lift stands
MOVE_FRAMES = 90                                    # ESTIMATED: the lift starts within 3 s of B stepping on
RIDE_FRAMES = 450                                   # ESTIMATED: 16 m down (FLOOR y 91 -> Lift anchor y 75) at ~2 m/s = 8 s
STOP_SAMPLES = 6                                    # still this many samples after it moved: the ride is over
V_MIN = 0.5                                         # ESTIMATED m/s: a moving sample (the carry error >= 0.05 m)
CARRY_TOL = 0.05                                    # ESTIMATED m: the copy's offset to the lift while moving vs at rest
MEASURED = None   # e.g. "run lift-carry-<sha8>: speed 2.0 m/s, deviation 0.01 m (rest offset 0.0 m)"


def others(st):
    return [p["number"] for p in (st.get("coop") or {}).get("players", []) if not p.get("self")]


def flat(a, b):
    return ((a[0] - b[0]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5


def lift_and_copy(pair, x):
    """Game x's lift (the MoveFloor nearest FLOOR) and the copy (kind player, CoopPuppet_) from ONE world dump
    (playtest_client.py Client.world: the full list; `state` lists the nearest known kinds only)."""
    actors = (pair.c[x].world().get("world") or {}).get("actors") or []
    lifts = [a for a in actors if LIFT in (a.get("name") or "") and a.get("pos")]
    lift = min(lifts, key=lambda a: flat(a["pos"], FLOOR)) if lifts else None
    copies = [a for a in actors if a.get("kind") == "player" and (a.get("name") or "").startswith("CoopPuppet_")
              and a.get("pos")]
    return (lift["pos"] if lift else None), (copies[0]["pos"] if copies else None)


@pytest.fixture
def back_after(pair):
    """Both games back to the boot's scene after the test (as test_boss_respawn.py back_after), so the next test sharing
    the boot starts there, not in Jabu-Jabu."""
    yield pair
    pair.deadline = P.Deadline(120)           # (the test's own deadline may be spent: the way back has its own)
    try:
        pair.warp_both(pair.boot_scene or BACK_TO)
    except Exception as e:                    # noqa: BLE001  (a failed way back is said, never a result of this test)
        pair.notes.append("way back to %s failed: %s" % (pair.boot_scene or BACK_TO, e))


def shots(pair, name):
    """A bridge shot of each game (the copy and the lift are 3D actors: the framebuffer has them). Tolerance 0.80 as
    test_native_body.py (people in a lit room). Not proven to show the copy (no in-view check): for the owner's eye
    and the NO-REF reference only; the verdict is the world list's."""
    for x in ("A", "B"):
        pair.check_shot(x, name, tolerance=0.80)


@pytest.mark.budget(480)   # ESTIMATED: two warps + ~1'100 frames in SAMPLE steps with a world dump each + 2 shots
@pytest.mark.look
@pytest.mark.row("Native puppets N5: the other player's copy rides our moving lift on it (not sunk in or floating over it)")
def test_puppet_lift_carry(pair, back_after):
    with pair.setup():
        pair.need_boot_place(age="child")
        b_nums = others(pair.state("A"))
        assert len(b_nums) == 1, "A sees players %s, not one other" % b_nums
        n = b_nums[0]
        pair.warp_both(SCENE, ENTRANCE)
        pair.act("A", "place %.2f %.2f %.2f" % ANCHOR)
        pair.step_both(20)
        mark = pair.log["A"].mark()
        pair.act("B", "place %.2f %.2f %.2f" % (FLOOR[0], FLOOR[1] + ON_M, FLOOR[2]))
        pair.step_both(10)
        lift0, copy0 = lift_and_copy(pair, "A")
        assert lift0 is not None and flat(lift0, FLOOR) <= FIND_M, (
            "A's world list has no %s within %.1f m of %s (nearest %s): not the cited place" % (LIFT, FIND_M, FLOOR, lift0))
        assert copy0 is not None and flat(copy0, FLOOR) <= 1.8, (
            "A's copy of B is not on the floor (copy %s, floor %s)" % (copy0, FLOOR))
        try:
            pair.until("the lift moves in A's game with B on it",
                       lambda: abs((lift_and_copy(pair, "A")[0] or lift0)[1] - lift0[1]) > 0.05, max_frames=MOVE_FRAMES)
        except AssertionError as e:
            raise SetupFail("the lift did not move with B on it (%s): B is not on it or #65's ride did not take" % e)

    # ride: offsets from one dump each SAMPLE frames
    rows, last, went, still_after = [], None, False, 0
    shot_taken = False
    for _ in range(RIDE_FRAMES // SAMPLE):
        pair.step_both(SAMPLE)
        lift, copy = lift_and_copy(pair, "A")
        if lift is None or copy is None:
            rows.append(None)
            continue
        dy = None if last is None else lift[1] - last[1]
        last = lift
        rows.append((lift[1], copy[1] - lift[1], dy))
        if dy is not None and abs(dy) > STILL_DY:
            went, still_after = True, 0
            if not shot_taken and abs(dy) * FPS / SAMPLE >= V_MIN:
                shots(pair, "lift-carry-ride")
                shot_taken = True
        elif went:
            still_after += 1
            if still_after >= STOP_SAMPLES:
                break
    got = [r for r in rows if r is not None]
    lost = len(rows) - len(got)
    still = [off for (_, off, dy) in got if dy is not None and abs(dy) <= STILL_DY]   # (the 1st has no dy: moving already)
    moving = [(off, abs(dy) * FPS / SAMPLE) for (_, off, dy) in got if dy is not None and abs(dy) * FPS / SAMPLE >= V_MIN]
    assert moving, "no moving sample (speed >= %.1f m/s) in %d samples: lift ys %s" % (
        V_MIN, len(got), ["%.2f" % r[0] for r in got[:20]])
    rest = statistics.median(still) if still else got[0][1]
    speed = statistics.median(v for _, v in moving)
    dev = max(abs(off - rest) for off, _ in moving)
    ride = ("ride: %d samples (%d lost), %d still, %d moving, speed %.2f m/s, rest offset %.3f m, "
                      "max deviation %.3f m, lift %.2f -> %.2f m" % (len(rows), lost, len(still), len(moving), speed,
                                                                     rest, dev, got[0][0], got[-1][0]))
    pair.notes.append(ride)
    if lost:
        pair.notes.append("%d samples had no lift or copy in A's world list" % lost)
    line = pair.log["A"].find(ON_LIFT % n, pos=mark)
    assert line, "A's log has no 'P%d lift carry: on our lift' line (/%s/): the carry never took the copy" % (n, ON_LIFT % n)
    delay_s = max(MIN_DELAY_S, int(re.search(ON_LIFT % n, line).group(1)) / 1000.0)   # (the delay the line logged)
    bug_m = 0.5 * speed * delay_s
    assert dev < bug_m, (
        "the copy was %.3f m off the lift while it moved (rest offset %.3f m, speed %.2f m/s; uncarried it would be "
        ">= %.3f m off): it sank into or floated over the lift" % (dev, rest, speed, 2 * bug_m))
    if not shot_taken:
        shots(pair, "lift-carry-ride")
    misses = []
    if not still:
        misses.append("no still sample: the rest offset is the first sample's")
    if still_after < STOP_SAMPLES:
        misses.append("the lift did not stop within %d frames" % RIDE_FRAMES)
    if dev > CARRY_TOL:
        misses.append("max deviation %.3f m > %.2f" % (dev, CARRY_TOL))
    if not MEASURED:
        pair.unproven("the N5 lift carry thresholds are not measured yet%s (MEASURED: this run's notes: %s)" % (
            "; misses: " + "; ".join(misses) if misses else "", ride))
    assert not misses, "; ".join(misses)
