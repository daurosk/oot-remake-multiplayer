"""Is the other player's puppet IN SIGHT on a game's shot, not only on its screen box? (kokiri-band-c124-7, B #165)

Why: a `place` of the other player at a distance and bearing (test_look.put_in_front, test_tunic.stand_apart) proves the
spot by distance only. Run kokiri-look-182cd53b (2026-11-28 03:19, shot kokiri-band-140dm-B.png): A placed 14 m from B
at 35 degrees stood BEHIND the Kakariko gate wall; its projected box held a torch and the wall, no Link, and the band
check called that a FAIL of the band. The projection (bandcheck.box) cannot see a wall; the picture can.

The proof: the viewer's shot with the target where it stands, then the target moved AWAY_M behind the viewer (behind
its camera: the away spot must project off the shot, checked on the same view), a second shot, the target put back
(place + face + ZL + settle). The target's own state must show it gone (more than AWAY_M/2 from its spot and
off the shot) before the second shot, else SetupFail 'did not leave' (a refused or clamped place). Pixels changed by more than DIFF in the target's box, less the share changed in the
same-size box beside it (people, birds, flames move too), must reach MIN_SEEN; else the target is not in sight
(behind a wall, or not drawn) = SetupFail, never a FAIL of the row. Both shots are copied to <out>/sight/.

DIFF / MIN_SEEN are a SET-UP guard's starting values, NOT MEASURED pass thresholds: every proof prints its numbers
('sight ...' note) so the lane runs measure them; a row's own verdict never rests on them.
"""
import math
import os
import shutil

import bandcheck
import pair as P

AWAY_M = 12.0     # m behind the viewer: past its camera (a camera behind a Link sits a few m back)
DIFF = 40         # a pixel changed: some channel moved by more than this (0-255)
MIN_SEEN = 0.08   # changed share in the target's box, less the side box's, for 'in sight'
SETTLE = 30       # frames after a place before a shot (test_look.SETTLE: the puppet follows the samples)


def _shot(pair, x, name):
    r = pair.call(x, "shot", name=name)
    path = r.get("path")
    if not path or not os.path.exists(path):
        raise P.SetupFail("sight: %s's shot %s gave no file (%s)" % (x, name, r))
    if pair.out_dir:
        keep = os.path.join(pair.out_dir, "sight")
        os.makedirs(keep, exist_ok=True)
        dst = os.path.join(keep, "%s.png" % name)
        shutil.copyfile(path, dst)
        return dst
    return path


def changed_share(a, b, box):
    """Share of box's pixels where some channel differs by more than DIFF between images a and b."""
    if not box:
        return 0.0
    pa, pb = a.load(), b.load()
    x0, y0, x1, y1 = box
    n = sum(1 for y in range(y0, y1) for x in range(x0, x1)
            if max(abs(pa[x, y][i] - pb[x, y][i]) for i in range(3)) > DIFF)
    return n / float(max(1, (x1 - x0) * (y1 - y0)))


def prove(pair, viewer, target, name):
    """The target game's Link (the viewer's puppet of it) is in sight on the viewer's shot. Returns the numbers
    (dict, also a note); SetupFail when it is off the shot or not in sight. Leaves the target where it was."""
    from PIL import Image
    s, t = pair.state(viewer), pair.state(target)
    view, vpos, tpos, tface = s.get("view"), s.get("pos"), t.get("pos"), t.get("facing")
    if not view or not vpos or not tpos or tface is None:
        raise P.SetupFail("sight %s: no view/place (view %s, %s pos %s, %s pos %s facing %s)" % (
            name, bool(view), viewer, vpos, target, tpos, tface))
    with_t = _shot(pair, viewer, "%s-sight-with-%s" % (name, viewer))
    yaw = math.radians(s["facing"] + 180.0)
    away = (vpos[0] + AWAY_M * math.sin(yaw), vpos[1], vpos[2] + AWAY_M * math.cos(yaw))
    a = Image.open(with_t).convert("RGB")
    w, h = a.size
    tb = bandcheck.box(view, tpos, w, h)
    if not tb:
        raise P.SetupFail("sight %s: %s's Link is off %s's shot (box None at %s)" % (name, target, viewer, tpos))
    vb = bandcheck.box(view, vpos, w, h)
    if vb and bandcheck.overlaps(vb, tb):
        raise P.SetupFail("sight %s: the boxes overlap (%s's own Link %s, %s's %s): %s is on %s's line of sight" % (
            name, viewer, vb, target, tb, target, viewer))
    if bandcheck.box(view, away, w, h):
        raise P.SetupFail("sight %s: the away spot %s is still on %s's shot: cannot prove sight" % (name, away, viewer))
    pair.act(target, "place %.2f %.2f %.2f" % away)
    pair.step_both(SETTLE)
    # (pair.act does not raise on a refused act: a place the game refused or clamped (a wall, a collision) leaves the
    # target in view, both shots alike, seen ~0 = a false 'not in sight'. The target's own state must show it left.)
    gone = pair.state(target).get("pos")
    if (not gone or math.hypot(gone[0] - tpos[0], gone[2] - tpos[2]) <= AWAY_M / 2.0
            or bandcheck.box(view, gone, w, h)):
        raise P.SetupFail("sight %s: %s did not leave (pos %s after place %s, was %s): cannot prove sight" % (
            name, target, gone, tuple(round(v, 2) for v in away), tpos))
    without = _shot(pair, viewer, "%s-sight-without-%s" % (name, viewer))
    pair.act(target, "place %.2f %.2f %.2f" % tuple(tpos))
    f = math.radians(tface)
    pair.act(target, "face %.2f %.2f" % (tpos[0] + math.sin(f), tpos[2] + math.cos(f)))
    pair.press(target, ["ZL"], frames=8)
    pair.step_both(SETTLE)
    b = Image.open(without).convert("RGB")
    side = bandcheck.side_box(tb, w, (vb[0] + vb[2]) / 2 if vb else None)
    box_ch, side_ch = changed_share(a, b, tb), changed_share(a, b, side)
    seen = box_ch - side_ch
    out = {"box": tb, "side": side, "changed": round(box_ch, 3), "side_changed": round(side_ch, 3),
           "seen": round(seen, 3), "min": MIN_SEEN}
    pair.notes.append("sight %s: %s sees %s: %s" % (name, viewer, target, out))
    print("sight %s: %s sees %s: %s" % (name, viewer, target, out))
    back = pair.state(target).get("pos") or tpos
    if math.hypot(back[0] - tpos[0], back[2] - tpos[2]) > 1.0:
        raise P.SetupFail("sight %s: %s not back at its spot (%s, was %s)" % (name, target, back, tpos))
    if seen < MIN_SEEN:
        raise P.SetupFail("sight %s: %s's puppet is NOT in sight on %s's shot (occluded or not drawn): %s" % (
            name, target, viewer, out))
    return out
