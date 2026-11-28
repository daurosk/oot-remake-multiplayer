"""Native puppets N1 (docs/native-puppets.md STATUS + "Probes" B1): the other player's copy has a body of its own in
each game, pack `--puppet-body` (packs/native.py, NATIVE_PUPPETS=body, the default), hook side lichii/hook/
coop_puppet.cpp (slot.body) + coop_puppet_body.h.

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_native_body | (new: plan N1, probe B1) | default + --pack native | positions (state) + module log + LOOK shots of both games |

B is the copy (P2 in A's game: B joins second, the number the module's `P<n>` lines use). Steps, each judged by state:
1. B stands; A, placed WALK_FROM_M beside B on the same ground, walks at B for WALK_FRAMES: A moves (control: at least
   MOVED_MIN_M, so nothing else held him) and stops short of B (never closer than GAP_MIN_M, never past him). Shots.
2. A placed DROP_M over B: A comes to rest on the copy's top (his feet CHILD_HEIGHT_M +- TOP_TOL_M over B's, two
   samples still) at least once, never down at B's feet level first. Shots.
3. B walks away; A's next `body:` summary line counts moves and setter calls, and the game had the copy at most
   BODY_DRIFT_MAX_M from our place (coop_puppet.cpp: "the game had it up to <x> m from our place"; B1 asks < 0.02 m).
4. B walked off A's line (toward +z); A walks along x through B's old spot: nothing is left there (A passes it).
Set-up proof: A's log has `P<n> body: <actor> has a body; it is moved through the game's setter` (coop_puppet.cpp
~2199), never `its body is NOT moved`; both games in play at the boot's place, A sees one other player.
"""
import math
import re

import pytest

HAS_BODY = r"coop: puppet: P%d body: \S+ has a body; it is moved through the game's setter"   # coop_puppet.cpp:2199
NOT_MOVED = r"coop: puppet: P%d body: .*its body is NOT moved"                                   # coop_puppet.cpp:2201
# coop_puppet.cpp:2251 (every 300 frames of the copy: body_rest + body_warp >= 300)
SUMMARY = (r"coop: puppet: P%d body: (\d+) frames at rest, (\d+) moved \((\d+) setter calls made, (\d+) waited a frame "
           r"for one\), (\d+) gave way to our Link; the game had it up to ([0-9.]+) m from our place")
SUMMARY_FRAMES = 300 + 60         # coop_puppet.cpp: a summary every 300 copy frames; + 60 for the copy to start
CHILD_HEIGHT_M = 1.17             # docs/native-puppets.md: romfs GameParameter/CctComponentPlayerClimbParam
CHILD_RADIUS_M = 0.23             #   Player_Child CharacterHeight / CharacterRadius
GAP_MIN_M = 2 * CHILD_RADIUS_M - 0.15   # two child capsules touching (0.46 m) less ESTIMATED solver slack / sample lag
WALK_FROM_M = 3.0                 # ESTIMATED: open ground at the boot's spot (test_dbno_crawl.py places 1.3 m beside B)
WALK_FRAMES = 120                 # ESTIMATED: child Link runs > 3 m in 2 s; without a body A would pass B
MOVED_MIN_M = 1.5                 # control: A walked at least half the way (else a wall, not B, held him)
DROP_M = 2.5                      # A placed this far over B's feet (above the copy's 1.17 m top)
TOP_TOL_M = 0.35                  # ESTIMATED: capsule top (radius 0.23) + sample lag
STILL_M = 0.02                    # two samples this close in y: A stands (not falling)
SAMPLE = 5                        # frames between position samples
DROP_FRAMES = 150                 # samples of the drop (2.5 s)
BODY_DRIFT_MAX_M = 0.02           # docs/native-puppets.md probe B1: "x under 0.02 m"
PAST_M = 1.5                      # A walks at a point this far past B's old spot along +x
PASSED_M = 0.5                    # and must get at least this far past it (ESTIMATED: > a capsule radius)
AWAY_FRAMES = 60                  # B walks away this long (yaw 0: toward +z, off the line A walks along x)
# The ESTIMATED thresholds above (GAP slack, WALK_FROM_M, WALK_FRAMES, MOVED_MIN_M, TOP_TOL_M, PASSED_M) are starting
# values (worker-rules: no guessed values; as test_hair_link.py). Until MEASURED cites a lane run's notes (walk/drop/
# old spot values), a run whose clear checks all hold ends NO-REF (pair.unproven) with the measured values, never PASS;
# a threshold miss is listed there, not failed. Clear FAILs at once, whatever MEASURED: no body line (SETUP-FAIL), the
# body made no move/setter call or the game had it farther than B1's 0.02 m (docs/native-puppets.md), A passed through
# B's standing copy (his x crossed B's), A dropped over B went down to B's feet (no top), A stopped at B's old spot.
MEASURED = None   # e.g. "run native-body-<sha8>: closest 0.4x m, top 1.1x m, past 2.x m"


def flat(a, b):
    return math.dist((a[0], a[2]), (b[0], b[2]))


def yaw_to(a, b):
    """World yaw from a toward b (docs/playtest.md `move`: 0 toward +z, 90 toward +x)."""
    return math.degrees(math.atan2(b[0] - a[0], b[2] - a[2])) % 360.0


def others(st):
    return [p["number"] for p in (st.get("coop") or {}).get("players", []) if not p.get("self")]


def pos(pair, x):
    p = pair.state(x).get("pos")
    assert p is not None, "%s: no position (%s)" % (x, pair.state(x).get("not_ready"))
    return p


def shots(pair, name):
    """A bridge shot of each game (the copy is a 3D actor: the framebuffer has it). Tolerance 0.80: people in an open
    field (test_dbno_crawl.py shots)."""
    for x in ("A", "B"):
        pair.check_shot(x, name, tolerance=0.80)


def walk_at(pair, target, frames):
    """A walks toward `target` (a place) for `frames`; returns A's samples [pos] every SAMPLE frames."""
    seen = [pos(pair, "A")]
    yaw = yaw_to(seen[0], target)
    for _ in range(frames // SAMPLE):
        pair.call("A", "move", yaw=yaw, strength=1.0, frames=SAMPLE)
        pair.step_other("B", SAMPLE)
        seen.append(pos(pair, "A"))
    return seen


@pytest.mark.budget(420)   # ESTIMATED: ~1'200 frames in small steps + 4 shots + the pack's boot
@pytest.mark.look
@pytest.mark.needs_pack("native")
@pytest.mark.row("Native puppets N1: the other player is a real body (A stops at B, stands on B, nothing left behind)")
def test_native_body(pair):
    with pair.setup():
        pair.need_boot_place(age="child")
        b_nums = others(pair.state("A"))
        assert len(b_nums) == 1, "A sees players %s, not one other" % b_nums
        n = b_nums[0]
        assert pair.log["A"].find(HAS_BODY % n, pos=0), (
            "A's copy of P%d has no body line (/%s/): the pack is not --puppet-body or the hook did not take it" % (
                n, HAS_BODY % n))
        pair.never_log("A", NOT_MOVED % n)
        b0 = pos(pair, "B")
        pair.act("A", "place %.2f %.2f %.2f" % (b0[0] - WALK_FROM_M, b0[1], b0[2]))
        pair.act("A", "face %.2f %.2f" % (b0[0], b0[2]))
        pair.step_both(30)
        a0 = pos(pair, "A")
        assert abs(a0[1] - b0[1]) < 0.3 and abs(flat(a0, b0) - WALK_FROM_M) < 0.5, (
            "A not on B's ground %.1f m beside him (A %s, B %s): the boot's spot is not open ground" % (
                WALK_FROM_M, a0, b0))

    # 1. A walks at standing B and stops short of him
    seen = walk_at(pair, b0, WALK_FRAMES)
    b1 = pos(pair, "B")
    closest = min(flat(p, b1) for p in seen)
    moved = flat(seen[0], seen[-1])
    passed = (seen[-1][0] - b1[0]) * (seen[0][0] - b1[0]) < 0      # A's x crossed B's
    pair.notes.append("walk: moved %.2f m, closest %.2f m to B, end %.2f m" % (moved, closest, flat(seen[-1], b1)))
    assert not passed, "A walked through B's standing copy (closest %.2f m): the copy has no body in A's game" % closest
    misses = []
    if moved < MOVED_MIN_M:
        misses.append("A moved only %.2f m toward B (< %.1f): held by something else?" % (moved, MOVED_MIN_M))
    if closest < GAP_MIN_M:
        misses.append("A came %.2f m close to B's copy (< %.2f)" % (closest, GAP_MIN_M))
    shots(pair, "native-body-stop")

    # 2. A dropped over the copy stands on its top
    pair.act("A", "place %.2f %.2f %.2f" % (b1[0], b1[1] + DROP_M, b1[2]))
    ys, on_top, fell = [], False, False
    for _ in range(DROP_FRAMES // SAMPLE):
        pair.step_both(SAMPLE)
        p = pos(pair, "A")
        ys.append(p[1] - b1[1])
        if len(ys) >= 2 and ys[-1] < DROP_M - 0.3 and abs(ys[-1] - ys[-2]) <= STILL_M:   # (fallen a bit, then still)
            if abs(ys[-1] - CHILD_HEIGHT_M) <= TOP_TOL_M:
                on_top = True
                break
            if abs(ys[-1]) <= TOP_TOL_M:
                fell = True
            break                                  # (still: on the top, at the feet, or somewhere between)
    pair.notes.append("drop: A over B's feet %s" % ["%.2f" % y for y in ys])
    assert not fell, "A dropped over B went down to B's feet (no top on the copy): heights over B's feet %s" % (
        ["%.2f" % y for y in ys])
    if not on_top:
        misses.append("A never stood still %.2f +- %.2f m over B's feet (heights %s)" % (
            CHILD_HEIGHT_M, TOP_TOL_M, ["%.2f" % y for y in ys]))
    shots(pair, "native-body-top")

    # 3. B walks away: the body follows through the setter, the game keeps it at our place
    pair.act("A", "place %.2f %.2f %.2f" % (b1[0] - WALK_FROM_M, b1[1], b1[2]))
    pair.step_both(10)
    mark = pair.log["A"].mark()
    for _ in range(AWAY_FRAMES // SAMPLE):      # (B's walk and A's frames in turn: A's copy moves while B walks,
        pair.call("B", "move", yaw=0.0, strength=1.0, frames=SAMPLE)   # so the summary's window holds the walk)
        pair.step_other("A", SAMPLE)
    b2 = pos(pair, "B")
    assert flat(b2, b1) >= 1.0, "B did not walk away (%.2f m)" % flat(b2, b1)
    line = pair.until("A's next body summary of P%d" % n,
                      lambda: pair.log["A"].find(SUMMARY % n, pos=mark), max_frames=SUMMARY_FRAMES)
    m = re.search(SUMMARY % n, line)
    rest, warp, calls, waited, gave = (int(m.group(i)) for i in range(1, 6))
    drift = float(m.group(6))
    pair.notes.append("summary: rest %d, moved %d, calls %d, waited %d, gave way %d, drift %.3f m" % (
        rest, warp, calls, waited, gave, drift))
    assert warp > 0 and calls > 0, "B walked but A's copy body made no move/setter call: %s" % line
    assert drift <= BODY_DRIFT_MAX_M, "the game had the copy %.3f m from our place (> %.2f): %s" % (
        drift, BODY_DRIFT_MAX_M, line)

    # 4. nothing left at B's old spot: A walks through it
    seen = walk_at(pair, (b1[0] + PAST_M, b1[1], b1[2]), WALK_FRAMES)
    went = max(p[0] for p in seen) - b1[0]
    pair.notes.append("old spot: A got %.2f m past it" % went)
    assert went > 0, "A stopped %.2f m short of B's old spot: a body left behind there" % -went
    if went < PASSED_M:
        misses.append("A got only %.2f m past B's old spot (< %.1f)" % (went, PASSED_M))
    pair.never_log("A", NOT_MOVED % n)
    if not MEASURED:
        pair.unproven("the N1 thresholds are not measured yet%s (MEASURED: this run's notes: %s)" % (
            "; misses: " + "; ".join(misses) if misses else "", "; ".join(pair.notes[-4:])))
    assert not misses, "; ".join(misses)
