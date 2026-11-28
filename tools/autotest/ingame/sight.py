"""Is the other player's puppet IN SIGHT on a game's shot, not only on its screen box? (kokiri-band-c124-7, B #165)

Why: a `place` of the other player at a distance and bearing (test_look.put_in_front, test_tunic.stand_apart) proves the
spot by distance only. Run kokiri-look-182cd53b (2026-11-28 03:19, shot kokiri-band-140dm-B.png): A placed 14 m from B
at 35 degrees stood BEHIND the Kakariko gate wall; its projected box held a torch and the wall, no Link, and the band
check called that a FAIL of the band. The projection (bandcheck.box) cannot see a wall; the picture can.

The proof: the viewer's shot with the target where it stands, then the target moved behind the viewer to the nearest
AWAY_M spot whose box misses the target's and the side box (checked on the same view), a second shot, the target put
back (place + face + ZL + settle). The target's own state must show it in play in the same scene (else LeftPlay: a
loading zone; the caller stops) and gone (MOVED_M from its spot, its box clear) before the second shot, else SetupFail 'did not leave' (a refused or clamped place). Pixels changed by more than DIFF in the target's box, less the share changed in the
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

AWAY_M = (1.5, 3.0, 4.5, 6.0, 9.0, 12.0)   # m behind the viewer, nearest first: the first spot whose box (on the
# same view) misses the target's and the side box is used. Nearest first (kokiri-look2-182cd53b 09:47: 12 m behind B,
# who had just come in at the Kakariko gate, was the gate's loading zone: A's game loaded a scene, its Link gone).
MOVED_M = 1.0     # the target's own state after the away place: at least this far from its spot (else 'did not leave')
DIFF = 40         # a pixel changed: some channel moved by more than this (0-255)
MIN_SEEN = 0.08   # changed share in the target's box, less the side box's, for 'in sight'
SETTLE = 30       # frames after a place before a shot (test_look.SETTLE: the puppet follows the samples)
MIN_FREE = 0.25   # behind=True: share of the target's box NOT covered by the viewer's own Link's box, at least


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


class LeftPlay(P.SetupFail):
    """The target left play at the away spot (a scene load, its Link gone): the caller stops (no other try can run)."""


def _inside(x, y, m):
    return m is not None and m[0] <= x < m[2] and m[1] <= y < m[3]


def free_share(box, mask):
    """Share of box's pixels outside the box `mask` (None: all of them)."""
    if not box:
        return 0.0
    x0, y0, x1, y1 = box
    n = sum(1 for y in range(y0, y1) for x in range(x0, x1) if not _inside(x, y, mask))
    return n / float(max(1, (x1 - x0) * (y1 - y0)))


def changed_share(a, b, box, mask=None):
    """Share of box's pixels (less those inside `mask`: the viewer's own Link) where some channel differs by more
    than DIFF between images a and b."""
    if not box:
        return 0.0
    pa, pb = a.load(), b.load()
    x0, y0, x1, y1 = box
    px = [(x, y) for y in range(y0, y1) for x in range(x0, x1) if not _inside(x, y, mask)]
    n = sum(1 for x, y in px if max(abs(pa[x, y][i] - pb[x, y][i]) for i in range(3)) > DIFF)
    return n / float(max(1, len(px)))


def prove(pair, viewer, target, name, behind=False):
    """The target game's Link (the viewer's puppet of it) is in sight on the viewer's shot. Returns the numbers
    (dict, also a note); SetupFail when it is off the shot or not in sight. Leaves the target where it was.
    behind=True: a set-up that puts the target AHEAD of the viewer's own Link on purpose (test_regress_ui's hilt:
    A's back seen past B's own Link), so the boxes may overlap: the viewer's own box is left out of both shares
    (the target must keep MIN_FREE of its box outside it, else SetupFail) instead of the overlap SetupFail, which
    stays for side-by-side set-ups (test_look's band check, test_tunic's colour check)."""
    from PIL import Image
    s, t = pair.state(viewer), pair.state(target)
    view, vpos, tpos, tface = s.get("view"), s.get("pos"), t.get("pos"), t.get("facing")
    if not view or not vpos or not tpos or tface is None:
        raise P.SetupFail("sight %s: no view/place (view %s, %s pos %s, %s pos %s facing %s)" % (
            name, bool(view), viewer, vpos, target, tpos, tface))
    with_t = _shot(pair, viewer, "%s-sight-with-%s" % (name, viewer))
    a = Image.open(with_t).convert("RGB")
    w, h = a.size
    tb = bandcheck.box(view, tpos, w, h)
    if not tb:
        raise P.SetupFail("sight %s: %s's Link is off %s's shot (box None at %s)" % (name, target, viewer, tpos))
    vb = bandcheck.box(view, vpos, w, h)
    mask = vb if behind else None
    if behind and vb and free_share(tb, vb) < MIN_FREE:
        raise P.SetupFail("sight %s: %s's Link is hidden behind %s's own (only %.2f of its box %s free of %s): "
                          "cannot prove sight" % (name, target, viewer, free_share(tb, vb), tb, vb))
    if not behind and vb and bandcheck.overlaps(vb, tb):
        raise P.SetupFail("sight %s: the boxes overlap (%s's own Link %s, %s's %s): %s is on %s's line of sight" % (
            name, viewer, vb, target, tb, target, viewer))
    side = bandcheck.side_box(tb, w, (vb[0] + vb[2]) / 2 if vb else None)

    def clear(p):   # (the target standing at p leaves its box and the side box as the background)
        b = bandcheck.box(view, p, w, h)
        return b is None or not (bandcheck.overlaps(b, tb) or (side and bandcheck.overlaps(b, side)))

    yaw = math.radians(s["facing"] + 180.0)
    spots = [(vpos[0] + d * math.sin(yaw), vpos[1], vpos[2] + d * math.cos(yaw)) for d in AWAY_M]
    away = next((p for p in spots if clear(p)), None)
    if away is None:
        raise P.SetupFail("sight %s: no away spot behind %s (%s m) clears %s's box %s: cannot prove sight" % (
            name, viewer, AWAY_M, target, tb))
    scene = t.get("scene")
    pair.act(target, "place %.2f %.2f %.2f" % away)
    pair.step_both(SETTLE)
    # (pair.act does not raise on a refused act: a place the game refused or clamped (a wall, a collision) leaves the
    # target in view, both shots alike, seen ~0 = a false 'not in sight'. The target's own state must show it left.)
    ts = pair.state(target)
    gone = ts.get("pos")
    if not pair.in_play(ts) or not gone or ts.get("scene") != scene:
        raise LeftPlay("sight %s: %s left play at the away spot %s (scene %s -> %s, loading %s, pos %s): a loading "
                       "zone or a fall; no other spot is tried" % (name, target, tuple(round(v, 2) for v in away),
                                                                   scene, ts.get("scene"), ts.get("loading"), gone))
    if math.hypot(gone[0] - tpos[0], gone[2] - tpos[2]) < MOVED_M or not clear(gone):
        raise P.SetupFail("sight %s: %s did not leave (pos %s after place %s, was %s): cannot prove sight" % (
            name, target, gone, tuple(round(v, 2) for v in away), tpos))
    without = _shot(pair, viewer, "%s-sight-without-%s" % (name, viewer))
    pair.act(target, "place %.2f %.2f %.2f" % tuple(tpos))
    f = math.radians(tface)
    pair.act(target, "face %.2f %.2f" % (tpos[0] + math.sin(f), tpos[2] + math.cos(f)))
    pair.press(target, ["ZL"], frames=8)
    pair.step_both(SETTLE)
    b = Image.open(without).convert("RGB")
    box_ch, side_ch = changed_share(a, b, tb, mask), changed_share(a, b, side, mask)
    seen = box_ch - side_ch
    out = {"box": tb, "side": side, "changed": round(box_ch, 3), "side_changed": round(side_ch, 3),
           "seen": round(seen, 3), "min": MIN_SEEN, "behind": behind}
    pair.notes.append("sight %s: %s sees %s: %s" % (name, viewer, target, out))
    print("sight %s: %s sees %s: %s" % (name, viewer, target, out))
    back = pair.state(target).get("pos") or tpos
    if math.hypot(back[0] - tpos[0], back[2] - tpos[2]) > 1.0:
        raise P.SetupFail("sight %s: %s not back at its spot (%s, was %s)" % (name, target, back, tpos))
    if seen < MIN_SEEN:
        raise P.SetupFail("sight %s: %s's puppet is NOT in sight on %s's shot (occluded or not drawn): %s" % (
            name, target, viewer, out))
    return out
