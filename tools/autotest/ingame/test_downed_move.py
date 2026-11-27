"""0.12.1 owner issue (docs/known-issues.md "To be fixed in 0.12.1"): a downed player plays the crawl animation but
cannot move, and the view does not go first person. The fix under test: downed-move-0121 89133f1ee (patch 0046 wrap 31
= the crawl's room-to-stand check, hookshot/src/coop_hookshot.cpp WrapCall HOS_COOP_WRAP_CRAWL_ROOM ->
lichii/hook/coop_dbno_body.cpp Crawl::NoRoom; Crawl::Skip lets the game's Walk / Wait through for a Link held lying).

| Test | Checks | Kind |
|---|---|---|
| test_downed_child_moves | the room answer, no flicker lying still, B moves under the stick, crawl camera, turning probe, revive + walk | log + positions + view + LOOK |
| test_downed_adult_moves | no flicker lying still, the lying ground move, B moves under the stick, revive (LDamageStandUp) | log + positions + LOOK |

B is the one downed, A (the host) sets the rules and revives. The set-up, the hits, the revive and the restore are
test_dbno_crawl.py's own (imported, never copied): same place, same lines, same lane state after the test.
Every log pattern is a line the module writes (file:line cited where it is defined).
"""
import math
import os
import re
import shutil
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_dbno_crawl as dc   # noqa: E402  (to_place, rules, no_fairy, down, stick_move, revive, restored, ...)

# hookshot/src/coop_hookshot.cpp:2331 (every boot, once the world hooks are known): patch 0046 is in this game
ROOM_WRAPPED = r"coop: down but not out: the crawl's room check wrapped \(a downed child keeps crawling\)"
ROOM_NOT_WRAPPED = r"coop: down but not out: the crawl's room check not wrapped"
# lichii/hook/coop_dbno_body.cpp:382 Crawl::NoRoom (once a down), logged as "coop: dbno: <note>" (coop_dbno_game.cpp:453)
NO_ROOM = (r"coop: dbno: our downed Link: the game's crawl asks for room to stand up: answered no room, "
           r"the crawl goes on")
# lichii/hook/coop_dbno_body.cpp:359 Crawl::Skip (once a down): a Link held lying keeps the game's Walk / Wait
GROUND_MOVE = r"coop: dbno: our downed Link: the game's action verb \d+ let through: he moves lying, in the held pose"
# The minimum move: test_dbno_crawl.CRAWL_MIN_M (the test's own: "the stick moves the downed Link at all").
MOVE_MIN_M = dc.CRAWL_MIN_M
CRAWL_FRAMES = 120     # hs-dbno-child.B.txt 'stick 0 1 120' (test_dbno_crawl's crawl)
TURN_FRAMES = 60       # the turning probe: the stick held right this long (the HANDOFF's steering probe: 60 frames)


def eye(st):
    """The camera's place [x, y, z] in metres from the state's `view` (docs/playtest.md: `clip` row-major, clip =
    m * [x y z 1], in the metres of `pos`: kit_move.on_screen uses it with `pos` as it is). The eye is the one point
    with clip x = y = w = 0 (rows 0, 1, 3): solved by Cramer's rule. None without a view or for a singular matrix."""
    v = st.get("view")
    if not isinstance(v, dict) or not isinstance(v.get("clip"), list) or len(v["clip"]) != 16:
        return None
    m = [float(c) for c in v["clip"]]
    rows = [m[0:4], m[4:8], m[12:16]]
    a = [r[:3] for r in rows]
    b = [-r[3] for r in rows]

    def det(q):
        return (q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0])
                + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]))
    d = det(a)
    if abs(d) < 1e-12:
        return None
    out = []
    for k in range(3):
        q = [list(r) for r in a]
        for i in range(3):
            q[i][k] = b[i]
        out.append(det(q) / d)
    return out


def eye_from_link(pair, x):
    """(metres from Link's feet to the camera, the camera's height over them), or None with no view."""
    st = pair.state(x)
    e = eye(st)
    if e is None or st.get("pos") is None:
        return None
    p = st["pos"]
    return math.dist(e, p), e[1] - p[1]


def yaw_of(a, b):
    """The world yaw from a to b (0 = +z, 90 = +x: the state's `facing` convention)."""
    return math.degrees(math.atan2(b[0] - a[0], b[2] - a[2])) % 360


def off_axis(yaw, facing):
    """How far a move's yaw is from the line of `facing` (either way along it), 0 to 90 degrees."""
    d = abs((yaw - facing + 180) % 360 - 180)
    return min(d, 180 - d)


# No flicker while he lies on the game's Wait (review-c123-16 755cc55b4 open point: the game's Wait command vs our
# replayed lying pose): STILL_SHOTS bridge shots per game STILL_GAP frames apart, no input; each next to the one
# before by imagecheck.ssim. STILL_MIN = test_dbno_crawl.shots' 0.80 (moving people in an open field,
# docs/ingame-tests.md step 6): a Link standing up between two lying frames drops the tiles he is in far below it.
STILL_SHOTS, STILL_GAP, STILL_MIN = 4, 4, 0.80


def no_flicker(pair, name):
    """The still check on each game; the shots are kept in <out>/still/ for the owner. Also: B did not get up and did
    not move without a stick (state pos). Returns {game: lowest SSIM of a shot next to the one before}."""
    import imagecheck   # (Pillow + scikit-image, as pair.check_shot)
    keep = os.path.join(pair.out_dir or ".", "still")
    os.makedirs(keep, exist_ok=True)
    paths = {"A": [], "B": []}
    before = pair.state("B")["pos"]
    for i in range(STILL_SHOTS):
        for x in ("A", "B"):
            r = pair.call(x, "shot", name="%s-still%d-%s" % (name, i, x))
            dest = os.path.join(keep, os.path.basename(r["path"]))
            shutil.copyfile(r["path"], dest)
            paths[x].append(dest)
        pair.step_both(STILL_GAP)
    low = {}
    for x, ps in paths.items():
        low[x] = min(imagecheck.ssim(a, b)[0] for a, b in zip(ps, ps[1:]))
    moved = dc.dist(before, pair.state("B")["pos"])
    pair.notes.append("%s still: lowest SSIM of a shot next to the one before A %.3f, B %.3f; B moved %.2f m "
                      "without a stick" % (name, low["A"], low["B"], moved))
    pair.never_log("B", dc.UP_LINE)
    for x in ("A", "B"):
        assert low[x] >= STILL_MIN, "%s: the lying Link flickers on %s (shots %d frames apart: SSIM %.3f < %.2f)" % (
            name, x, STILL_GAP, low[x], STILL_MIN)
    assert moved <= MOVE_MIN_M, "%s: B moved %.2f m lying with no stick" % (name, moved)
    return low


def wrapped(pair, x):
    """Set-up: this game's hookshot.exe has patch 0046 (its hello line since the boot), else no fix to test."""
    lines = pair.log[x].since(0)
    if any(re.search(ROOM_NOT_WRAPPED, ln) for ln in lines):
        raise AssertionError("%s: the crawl's room check is not wrapped: this game lacks patch 0046" % x)
    assert any(re.search(ROOM_WRAPPED, ln) for ln in lines), "%s: no 'crawl's room check' hello line" % x


@pytest.mark.budget(420)
@pytest.mark.look
@pytest.mark.row("0.12.1 a downed child moves under the stick, crawl camera, turning probe, revived by A")
def test_downed_child_moves(pair):
    with dc.restored(pair, {}):
        with pair.setup():
            pair.need_boot_place("child")
            dc.to_place(pair, "child", "KokiriForest_Child", "FrontDekuTree")   # hs-dbno-child: open ground
            wrapped(pair, "B")
            hold = dc.rules(pair, 238)  # long (kDbnoMaxBleedS 240); not 240/239: a rules line only on a change
            dc.no_fairy(pair, "B")
            cam_up = eye_from_link(pair, "B")
            assert cam_up is not None, "B: no camera view in the state before the down"
        dc.down(pair)
        pair.expect_log("B", dc.LYING_CHILD, max_frames=600)
        pair.step_both(30)
        dc.shots(pair, "downed-child-lying")
        no_flicker(pair, "downed-child")
        a = pair.state("B")
        fwd_m = dc.stick_move(pair, "B", 0, CRAWL_FRAMES)
        b = pair.state("B")
        # after the stick (review-rows-c130-7): the game's crawl may ask for room only while he moves
        pair.expect_log("B", NO_ROOM, max_frames=60)     # the game's crawl asked for room and goes on
        dc.shots(pair, "downed-child-moved", twice=False)
        cam_down = eye_from_link(pair, "B")
        # Turning probe (no assert: what the game's crawl does is the finding): stick right, facing + move direction.
        turn_m = dc.stick_move(pair, "B", 90, TURN_FRAMES)
        c = pair.state("B")
        turned = abs((float(c.get("facing") or 0) - float(b.get("facing") or 0) + 180) % 360 - 180)
        side_axis = off_axis(yaw_of(b["pos"], c["pos"]), float(b.get("facing") or 0)) if turn_m > 0.01 else None
        fwd_axis = off_axis(yaw_of(a["pos"], b["pos"]), float(a.get("facing") or 0)) if fwd_m > 0.01 else None
        pair.notes.append("crawl: forward %.2f m in %d frames (%s deg off his facing line); stick right %d frames: "
                          "%.2f m, %s deg off the facing line, facing turned %.1f deg" % (
                              fwd_m, CRAWL_FRAMES, "-" if fwd_axis is None else "%.0f" % fwd_axis, TURN_FRAMES,
                              turn_m, "-" if side_axis is None else "%.0f" % side_axis, turned))
        pair.notes.append("camera: standing %.2f m from his feet (%.2f m up); downed %s" % (
            cam_up[0], cam_up[1], "no view" if cam_down is None else "%.2f m (%.2f m up)" % cam_down))
        pair.never_log("B", dc.CRAWL_REFUSED)
        pair.never_log("B", dc.event(dc.EV_BLED_OUT))
        assert fwd_m > MOVE_MIN_M, "B did not move: %.2f m in %d frames of full stick (downed child)" % (
            fwd_m, CRAWL_FRAMES)
        assert cam_down is not None, "B: no camera view in the state while downed"
        assert cam_down[0] < cam_up[0], "B's view did not come in to the crawl camera: %.2f m from him downed, " \
            "%.2f m standing" % (cam_down[0], cam_up[0])
        held = dc.revive(pair, hold)
        pair.notes.append("revived after A held A %d frames (hold %d tenths)" % (held, hold))
        pair.expect_log("B", dc.UP_LINE, max_frames=300)
        pair.never_log("B", dc.UP_REFUSED)
        pair.step_both(60)
        dc.shots(pair, "downed-child-revived", twice=False)
        walk_m = dc.stick_move(pair, "B", 0, 90)
        pair.notes.append("walk %.2f m in 90 frames after the revive" % walk_m)
        assert walk_m >= 1.0, "B has no control after the revive: %.2f m in 90 frames" % walk_m


@pytest.mark.budget(420)
@pytest.mark.look
@pytest.mark.row("0.12.1 a downed adult moves lying under the stick, revived by A")
def test_downed_adult_moves(pair):
    with dc.restored(pair, {}):
        with pair.setup():
            pair.need_boot_place("child")
            dc.to_place(pair, "adult", "HyruleField_Adult", "HyruleField_FrontOfKakarikoVillage")   # hs-dbno-adult
            wrapped(pair, "B")
            hold = dc.rules(pair, 237)  # long: the move and the revive before any bleed-out
            dc.no_fairy(pair, "B")
        field = "HyruleField_Adult"
        dc.down(pair)
        pair.expect_log("B", dc.CRAWL_TAKEN, max_frames=300)
        pair.expect_log("B", dc.LYING_ADULT, max_frames=300)
        pair.step_both(30)
        dc.shots(pair, "downed-adult-lying")
        no_flicker(pair, "downed-adult")
        fwd_m = dc.stick_move(pair, "B", 0, CRAWL_FRAMES)
        dc.shots(pair, "downed-adult-moved", twice=False)
        pair.notes.append("adult downed: %.2f m in %d frames of full stick" % (fwd_m, CRAWL_FRAMES))
        dc.same_scene(pair, "B", field, "after the stick")
        pair.expect_log("B", GROUND_MOVE, max_frames=30)
        pair.never_log("B", NO_ROOM)               # (Crawl::NoRoom is for the child's game crawl only)
        pair.never_log("B", dc.CRAWL_REFUSED)
        pair.never_log("B", dc.event(dc.EV_BLED_OUT))
        assert fwd_m > MOVE_MIN_M, "B did not move: %.2f m in %d frames of full stick (downed adult)" % (
            fwd_m, CRAWL_FRAMES)
        held = dc.revive(pair, hold)
        pair.notes.append("revived after A held A %d frames (hold %d tenths)" % (held, hold))
        pair.expect_log("B", dc.UP_LINE, max_frames=300)
        pair.never_log("B", dc.UP_REFUSED)
        pair.step_both(60)
        dc.shots(pair, "downed-adult-revived", twice=False)
        walk_m = dc.stick_move(pair, "B", 0, 90)
        pair.notes.append("adult walk %.2f m in 90 frames after the revive" % walk_m)
        assert walk_m >= 1.0, "B has no control after the revive: %.2f m in 90 frames" % walk_m
        assert fwd_m / CRAWL_FRAMES < walk_m / 90, "downed B moved as fast as a walk"
