"""Row 53 (player report row 53, "reaching Hyrule Field gives the ocarina checks"): the Temple of Time placement probe,
ported from the old .ps1 row (coord-multiplayer offline/row-53-ocarina-2-probe.ps1 + scenario row53-tot-probe, branch
row-53-ocarina-2 05be5f1bb). The row itself shipped in 0.11.12 (kit presets open / open_fast set OOTRR's
door_of_time_escape "moat"); this probe proves where Link lands when placed inside the Temple of Time, the spots the
open_fast Door of Time run will stand on.

Old row -> this test (old and new judge the same thing):
| old (row53-tot-probe.steps.txt / .A.txt) | here |
|---|---|
| warp Market_Child, wait 600, `lines market-A min=1 coop: test mark market (Link at` | warp_both("Market_Child") in set-up; scene asserted |
| `lines place-A min=3 coop: test place X Y Z: done` | each of the 3 places is asserted by state: Link is at the spot (x, z) and has dropped onto a floor |
| `lines marks-A min=6 coop: test mark p-...` (p-door, p-warp, p-warp-face, p-altar, p-altar-face, p-at-door) | each mark is a state read: the 3 landings, the 2 faces (facing ~90), the door walk (a note: where Link stopped) |
| game B stands where the save put it | B is warped too (warp_both: both games in one scene) and only stepped |

The 11-23 failure it guards (p7-walk v2, shots-lane1/p7-walk/20261123-070414 hookshot_log-A.txt:29657, :30654): a
`place 105.80 100.00 -34.20` logged "done" while Link stayed at the market gate (-1.97 102.12 65.24), because y 100.00
is under the temple floor and the game put him back. A log line alone cannot see that; the state can.
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_rows import need_child_in_play   # noqa: E402  (the pilot rows' start-state check)

# The Temple of Time is an area of Market_Child (tools/puppet_pack/start_points.py header). `warp` takes the real scene
# name (docs/playtest.md "warp"); the runner's warp to it landed at the market gate on 11-23 (p7-walk v2
# hookshot_log-A.txt:29615 "test mark market (Link at -1.71 102.12 65.00)").
SCENE = "Market_Child"
# Dump romfs Banc/Market/Market_Child_Static: Land_TempleOfTime_Inside_A_01 at 95.58 102.10 -34.00 (the inside's floor
# model, the lowest temple height); the Door of Time Obj_TimeTemple_Door_A_01 at 118.38 104.86 -34.00 (OOTRR
# door_open.rs DOOR Obj_TimeTemple_Door_A_01_cb9f). Values as in row53-tot-probe.A.txt's header.
TEMPLE_FLOOR_Y = 102.10
DOOR_X, DOOR_Z = 118.38, -34.0
# The three places of row53-tot-probe.A.txt, each 1.5 m or more over the nearest dump placement's height so Link drops
# onto the floor (that header): (mark, x, y, z, the dump placement it stands over).
SPOTS = (
    ("p-door", 76.5, 105.5, -34.0, "Demo_Dm_TimeTemple_0000_Moving Position 76.50 103.93 -34.00"),
    ("p-warp", 84.75, 105.5, -34.0, "Obj_TimeTemple_WarpPoint_A_01 84.75 103.79 -34.00"),
    ("p-altar", 106.0, 105.5, -34.0, "spirit stone base 110.43 103.88 -34.00 (in front of the door)"),
)
FACE_AT = ("p-warp", "p-altar")      # the .A.txt faces the door after these two places
XZ_TOL = 0.5                         # place writes x, z at once (docs/playtest.md "act"); a fall is straight down
# Frames, copied from row53-tot-probe.A.txt: wait 600 after the warp, 240 after each place, 120 after each face,
# goto 116.0 -34.0 0.5 600 then wait 120 for the door walk.
WAIT_WARP, WAIT_PLACE, WAIT_FACE, WALK_FRAMES, WAIT_WALK = 600, 240, 120, 600, 120
WALK_TO_X = 116.0
# facing: 0 = +z, 90 = +x (docs/playtest.md "State"); the door is straight +x of every spot (all at z -34.0)
FACE_DOOR, FACE_TOL = 90.0, 10.0
STEADY_FRAMES = 30                   # a landing = the same height 30 frames apart (on the ground, not falling)


def move_one(pair, x, yaw, frames):
    """Game x walks toward world yaw `yaw` with the bridge's `move` (docs/playtest.md "move": the module makes the
    stick from that frame's camera); the other game runs the same frames (pair.press's rule)."""
    r = pair.call(x, "move", yaw=float(yaw), strength=1.0, frames=int(frames))
    for y in pair.c:
        if y != x:
            pair.step_other(y, int(frames))
    return r


def landed(pair, x, mark, tx, ty, tz, over):
    s1 = pair.state(x)
    pair.step_both(STEADY_FRAMES)
    s2 = pair.state(x)
    assert (s2.get("scene") or "").lower() == SCENE.lower(), "%s %s: scene %s, not %s" % (x, mark, s2.get("scene"), SCENE)
    px, py, pz = s2["pos"]
    assert abs(px - tx) <= XZ_TOL and abs(pz - tz) <= XZ_TOL, (
        "%s %s: Link at %.2f %.2f %.2f, not at the place %.2f %.2f (put back, as 11-23)" % (x, mark, px, py, pz, tx, tz))
    assert TEMPLE_FLOOR_Y - 0.3 <= py < ty - 0.5, (
        "%s %s: Link at height %.2f: not dropped onto the temple floor (placed at %.2f over %s; floor model %.2f)"
        % (x, mark, py, ty, over, TEMPLE_FLOOR_Y))
    assert abs(s1["pos"][1] - py) < 0.05, "%s %s: height %.2f -> %.2f in %d frames: still falling" % (
        x, mark, s1["pos"][1], py, STEADY_FRAMES)
    pair.notes.append("%s LANDED at %.2f %.2f %.2f" % (mark, px, py, pz))
    return s2


@pytest.mark.budget(180)   # the old row's frames: 600 + 3*(240+30) + 2*120 + 600 + 120 = 2370 (~40 s of game) + calls
@pytest.mark.row("#53 Temple of Time placement probe (row53-tot-probe, row-53-ocarina-2-probe.ps1)")
def test_temple_of_time_places(pair):
    with pair.setup():
        need_child_in_play(pair)
        st = pair.warp_both(SCENE)
        for x, s in st.items():
            assert (s.get("scene") or "").lower() == SCENE.lower(), "%s is in %s after the warp" % (x, s.get("scene"))
    pair.step_both(WAIT_WARP)
    s = pair.state("A")
    pair.notes.append("market at %.2f %.2f %.2f" % tuple(s["pos"]))
    for mark, tx, ty, tz, over in SPOTS:
        pair.act("A", "place %.2f %.2f %.2f" % (tx, ty, tz))
        pair.step_both(WAIT_PLACE)
        landed(pair, "A", mark, tx, ty, tz, over)
        if mark in FACE_AT:
            # Turned by the stick (Pair.turn_to), not by `act face`: that line writes the yaw (the module logs 'test face
            # ... turned to yaw 90.0') but state.facing never follows it (run row53-tot-182cd53b: facing -180.0 after the
            # line; probe row53face-182cd53b: -180.0 at +1, +2, +30, +120 frames).
            turned, f, off = pair.turn_to("A", DOOR_X, DOOR_Z, tol=FACE_TOL)
            assert turned, "A %s-face: facing %.1f, %.0f degrees off the door (%.0f) after the stick pushes" % (
                mark, f, off, FACE_DOOR)
    # p-at-door: walk on toward the door from p-altar (the old goto 116.0 -34.0; it stops at the door's wall when the
    # door is shut). Not a pass/fail check in the old row either (its judge: "may read NOT THERE if the door is
    # shut"): where Link stopped is a note for the open_fast Door of Time run.
    used = 0
    while used < WALK_FRAMES and pair.state("A")["pos"][0] < WALK_TO_X - XZ_TOL:   # goto's radius 0.5, 600 frames
        move_one(pair, "A", FACE_DOOR, 8)                                          # 8 a push: walk_to's step
        used += 8
    pair.step_both(WAIT_WALK)
    s = pair.state("A")
    assert (s.get("scene") or "").lower() == SCENE.lower(), "A after the door walk: scene %s" % s.get("scene")
    px = s["pos"][0]
    pair.notes.append("p-at-door x %.2f (%s; the door's x %.2f)" % (
        px, "at the goto point" if px >= WALK_TO_X - XZ_TOL else "stopped short after %d frames" % used, DOOR_X))
