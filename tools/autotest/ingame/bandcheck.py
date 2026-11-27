"""The Cap trim band on a Link in a bridge shot (test_look.py): how many trim-coloured pixels a Link's screen box holds.

A Link's box comes from the shot's camera, the bridge state's `view.clip` (docs/playtest.md "State": clip = m * [x y z
1] row-major, on the screen at x/w, y/w in -1..1, y pointing down the picture when `y_down`; the overlay's own use:
lichii/hook/coop_hook.cpp, the camera debug block), from the feet (`pos`) to HEAD m above them.
"""
import colorsys

# m: a child Link's height and a little (feet at `pos`). Every user (test_look, test_hood_trim, test_tunic) runs child
# Link (need_child_in_play). MEASURED on run tunic-look-53d40ab3-2 (lane 1, 2026-11-27, lane1/new/tunic-blue-A.png):
# with the old 1.9 (an adult's) A's own Link's box was 437 px tall (y 215-656) and the Link 269 px (cap top y 385 to
# the feet), B's puppet's box 272 px (y 295-571) and the puppet 167 px (cap top y 402): 1.17 m both. The adult height
# made each box 1.6x too tall and, by the half-width (0.35 x tall), too wide: the two side-by-side Links' boxes
# overlapped (x 486-792 and 736-927; the Links themselves are x 569-710 and 800-895) and the run ended SETUP-FAIL.
HEAD = 1.25


def project(view, p, w, h):
    m = view["clip"]
    v = [sum(m[r * 4 + c] * (list(p) + [1.0])[c] for c in range(4)) for r in range(4)]
    if v[3] <= 1e-6:
        return None                    # behind the camera
    nx, ny = v[0] / v[3], v[1] / v[3]
    py = (ny + 1) / 2 * h if view.get("y_down") else (1 - ny) / 2 * h
    return (nx + 1) / 2 * w, py


def box(view, pos, w, h):
    """The screen box (x0, y0, x1, y1) of a Link standing at `pos`, or None off the screen."""
    feet = project(view, pos, w, h)
    head = project(view, (pos[0], pos[1] + HEAD, pos[2]), w, h)
    if not feet or not head:
        return None
    tall = abs(feet[1] - head[1])
    half = max(2.0, tall * 0.35)
    x0, x1 = feet[0] - half, feet[0] + half
    y0, y1 = min(feet[1], head[1]) - 2, max(feet[1], head[1]) + 2
    x0, y0, x1, y1 = max(0, int(x0)), max(0, int(y0)), min(w, int(x1) + 1), min(h, int(y1) + 1)
    return (x0, y0, x1, y1) if x1 - x0 >= 2 and y1 - y0 >= 2 else None


def is_trim(rgb):
    """The yellow trim FFC020 (hue 45) as the game shades it: hue 38-56, saturated, not dark. Wood and roofs are
    redder (hue < 35) or greyer; the blue tunic 3070FF is hue 222."""
    hh, s, v = colorsys.rgb_to_hsv(*(c / 255.0 for c in rgb[:3]))
    return 38 <= hh * 360 <= 56 and s >= 0.55 and v >= 0.30


def trim_pixels(im, b):
    x0, y0, x1, y1 = b
    px = im.load()
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if is_trim(px[x, y]))


def side_box(b, w, away_from=None):
    """The same-size box beside b (the background's own yellow), on the side away from the screen x `away_from` (the
    other Link), else on the right; None when it does not fit on the shot."""
    bw = b[2] - b[0]
    right = (b[2], b[1], b[2] + bw, b[3])
    left = (b[0] - bw, b[1], b[0], b[3])
    first = left if away_from is not None and away_from > (b[0] + b[2]) / 2 else right
    for s in (first, right if first is left else left):
        if s[0] >= 0 and s[2] <= w:
            return s
    return None


def overlaps(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def measure(path, view, pos, other=None):
    """Trim pixels in the Link's box minus those in the same-size box beside it (the background's own yellow), the
    side box away from the Link at `other`; returns {box, side, trim, background, area, net} or None when the Link is
    off the shot."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    b = box(view, pos, w, h)
    if not b:
        return None
    ob = box(view, other, w, h) if other is not None else None
    side = side_box(b, w, (ob[0] + ob[2]) / 2 if ob else None)
    bg = trim_pixels(im, side) if side else 0
    t = trim_pixels(im, b)
    return {"box": b, "side": side, "trim": t, "background": bg, "area": (b[2] - b[0]) * (b[3] - b[1]),
            "net": max(0, t - bg)}


def disjoint(*boxes):
    """The first pair of the boxes that overlap (None-boxes skipped), or None when every pair is apart."""
    bs = [b for b in boxes if b]
    for i in range(len(bs)):
        for j in range(i + 1, len(bs)):
            if overlaps(bs[i], bs[j]):
                return bs[i], bs[j]
    return None
