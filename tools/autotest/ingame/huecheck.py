"""vest-link's colour check (test_vest_link.py; its owner's file, as kokiri-band's bandcheck.py): the pixels that CHANGED
between two bridge shots of the same place, and how many of them take a colour's hue. No guessed screen region: what a
recolour changes is found by the change itself.

The defaults (moved, saturated, near) are starting values, NOT MEASURED: test_vest_link.py ends NO-REF until its
MEASURED cites a lane run's huecheck lines (worker-rules: no guessed values).
"""
WIDTH = 320
# left_after_unset's FAIL line: share of the wear's own pixels still in its hue after the unset. MEASURED on the stored
# gate-look 309d7180c lane-1 shots (offline/gate-look-309d7180/lane1/new; before = *-unset, coloured = hair-link-red /
# vest-link-yellow, after = *-unset-again): unset that worked A hair 0.049, B hair 0.014, A vest 0.005, B vest 0.043;
# colour still on (after = the coloured shot / hair-link-red-2, 6 frames later) A hair 0.482/0.440, B hair 0.381/0.194,
# A vest 0.267, B vest 0.221. 0.12 sits between the two (2.4x the most a working unset left, 0.6x the least a wear
# still on kept).
LEFT_SHARE = 0.12


def hue_of(rgb):
    """The hue (0-360) of a 0xRRGGBB colour."""
    import colorsys
    r, g, b = ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0
    return colorsys.rgb_to_hsv(r, g, b)[0] * 360.0


def changed_hue(before, after, rgb, width=WIDTH, moved=0.15, saturated=0.25, near=30.0):
    """A pixel changed when one channel moved by more than `moved` (0..1) from `before` to `after` (both scaled to
    `width`); it counts as the colour when it is saturated in `after` (HSV S > `saturated`: grey and dark pixels have no
    hue) and its hue is within `near` degrees of rgb's. Returns {changed, coloured, share, hue}."""
    import numpy as np
    a, size = _load(before, width)
    b, _ = _load(after, width, size)
    moved_px = np.abs(b - a).max(axis=2) > moved
    coloured = moved_px & _in_hue(b, rgb, saturated, near)
    n, c = int(moved_px.sum()), int(coloured.sum())
    return {"changed": n, "coloured": c, "share": round(c / n, 3) if n else 0.0, "hue": round(hue_of(rgb), 1)}


def _load(path, width, size=None):
    import numpy as np
    from PIL import Image
    with Image.open(path) as im:
        im = im.convert("RGB")
        if size is None:
            size = (width, max(1, round(im.height * width / im.width)))
        return np.asarray(im.resize(size, Image.BILINEAR), dtype=np.float64) / 255.0, size


def _in_hue(img, rgb, saturated, near):
    import numpy as np
    from PIL import Image
    hsv = np.asarray(Image.fromarray((img * 255).astype("uint8"), "RGB").convert("HSV"), dtype=np.float64) / 255.0
    off = np.abs((hsv[..., 0] * 360.0 - hue_of(rgb) + 180.0) % 360.0 - 180.0)
    return (hsv[..., 1] > saturated) & (off <= near)


def left_after_unset(before, coloured, after, rgb, width=WIDTH, moved=0.15, saturated=0.25, near=30.0):
    """How much of a wear's colour is left after it was unset, judged ONLY on the pixels the wear painted (B #148,
    gate-look 309d7180c: the whole-frame count took animated torch flames and a moving Navi/puppet as 'colour left').
    `before` (unset), `coloured` (worn) and `after` (unset again) are shots of the same place. The wear's
    pixels: changed from `before` to `coloured` and in rgb's hue in `coloured`. Left: of those, still in rgb's hue in `after`, not in it
    in `before` (a flame already orange before is scene, not wear), and nearer `coloured` than `before` (a flame that
    flickers back to orange is not the wear's paint). Returns {wear, left, share, hue}: a wear still on keeps share near
    1, an unset that worked leaves only scene flicker."""
    import numpy as np
    a, size = _load(before, width)
    c, _ = _load(coloured, width, size)
    b, _ = _load(after, width, size)
    a_hue, c_hue, b_hue = (_in_hue(x, rgb, saturated, near) for x in (a, c, b))
    wear = (np.abs(c - a).max(axis=2) > moved) & c_hue
    nearer = np.abs(b - c).sum(axis=2) < np.abs(b - a).sum(axis=2)
    left = wear & b_hue & ~a_hue & nearer
    n, k = int(wear.sum()), int(left.sum())
    return {"wear": n, "left": k, "share": round(k / n, 3) if n else 0.0, "hue": round(hue_of(rgb), 1)}


# ---- hair-check-0121 (hair-link-9, row 293): the SOLID colour count inside a Link's screen box ----------------------
# Why not changed_hue / left_after_unset for the hair (hair-link-309d7180, hair-link-8's 6 test-bug FAILs): the hair is
# small, the camera eases between shots (A unset->red: 21% of the frame changed), Link's head sways, and #DD4527's +-30
# degree band takes brown hair, skin and Kakariko's warm ground (hue 21-35), so a per-pixel diff of the whole frame
# judged the scene. This counts only the pixels of the colour that sit in a SOLID patch (SOLID_D of a SOLID_K square
# around them is the colour too: a painted hair is a solid patch, brown-hair, skin and ground specks are not) inside the
# Link's own screen box (bandcheck.box from the same frame's view.clip + the Link's pos), so camera moves, the windmill,
# the torch flames and Navi outside the box do not count.
# MEASURED on the stored hair-link-309d7180 lane-1 shots (coord-multiplayer/offline/hair-link-5/run-309d7180/lane1/new,
# 1280x720; A's own Link and B's puppet boxed by hand around the hair, hair-link-9 report): RED class hue -15..+2 deg
# of #DD4527 (red hair: hue 0-10 holds 872 of A's 1842 saturated hair pixels; brown hair, skin and the ground lie at
# 10-35), S >= 0.65 (red hair S median 0.79, brown hair 0.39); solid counts A unset 0 / red 593 / red 6 frames later 597
# / unset again 27, B puppet 0 / 91 / 94 / 0; Skull Mask on: A 530 -> 1200, B 0 -> 31. BLUE class hue +-15 of
# #4F8AE1, S >= 0.25 (blue hair S median 0.37): inside the red shot's hair patch (hair_patch, HAIR_GROW 8 px) A red 0
# -> blue 239, B 0 -> 25; Navi's glow beside the puppet gave B 39 in the whole box of the unset shot, 0 in the patch.
SOLID_K, SOLID_D = 5, 0.8            # px at REF_W, share: the patch test
REF_W = 1280                         # the bridge shot's width (the counts are scaled to it)
HAIR_GROW = 8                        # px at REF_W: the hair patch grown so a head that swayed is still inside
CLASSES = {0xDD4527: (-15.0, 2.0, 0.65), 0x4F8AE1: (-15.0, 15.0, 0.25)}
# A picture with nothing lit (a load's black frame: hair-link-309d7180 adult-red-A, every pixel 0.000) is no shot of
# the hair. MEASURED: the 99th percentile of the brightest channel is 0.000 on those, 0.26 on the darkest real frame
# (adult-unset-A, a dark forest) and 0.65-0.80 on Kakariko by day; 0.10 sits between black and the darkest real one.
LIT_P99 = 0.10


def lit(path):
    """True when the shot shows a lit picture (not a load's black frame)."""
    import numpy as np
    from PIL import Image
    with Image.open(path) as im:
        a = np.asarray(im.convert("RGB"), dtype=np.float64) / 255.0
    return float(np.percentile(a.max(axis=2), 99)) >= LIT_P99


def _scale(img_w):
    return img_w / float(REF_W)


def _class_mask(path, rgb):
    """(mask of the solid patches of rgb's class, image width, height)."""
    import numpy as np
    from PIL import Image
    from scipy import ndimage
    lo, hi, sat = CLASSES[rgb]
    with Image.open(path) as im:
        hsv = np.asarray(im.convert("RGB").convert("HSV"), dtype=np.float64) / 255.0
    off = (hsv[..., 0] * 360.0 - hue_of(rgb) + 180.0) % 360.0 - 180.0
    m = (off >= lo) & (off <= hi) & (hsv[..., 1] >= sat)
    h, w = m.shape
    k = max(3, int(round(SOLID_K * _scale(w))) | 1)
    return (ndimage.uniform_filter(m.astype(np.float64), k) >= SOLID_D) & m, w, h


def _in_box(shape, box):
    import numpy as np
    out = np.zeros(shape, dtype=bool)
    if box:
        x0, y0, x1, y1 = box
        out[y0:y1, x0:x1] = True
    return out


def outside(path, boxes):
    """Mask of the shot at `path`: True on every pixel outside all of `boxes` (None entries skipped). Used to leave the
    OTHER Link out of a Link's box (hair-measure-0defc551: B's red-tunic Link in front of A's puppet, in A's box)."""
    import numpy as np
    from PIL import Image
    with Image.open(path) as im:
        w, h = im.size
    keep = np.ones((h, w), dtype=bool)
    for b in boxes:
        if b:
            keep &= ~_in_box(keep.shape, b)
    return keep


def covered(box, boxes):
    """Share (0..1) of `box`'s top third (where a Link's hair is) that lies inside any of `boxes`."""
    import numpy as np
    x0, y0, x1, y1 = box
    y1 = y0 + max(1, (y1 - y0) // 3)
    area = np.zeros((y1 - y0, x1 - x0), dtype=bool)
    for b in boxes:
        if b:
            ax0, ay0, ax1, ay1 = max(x0, b[0]), max(y0, b[1]), min(x1, b[2]), min(y1, b[3])
            if ax1 > ax0 and ay1 > ay0:
                area[ay0 - y0:ay1 - y0, ax0 - x0:ax1 - x0] = True
    return float(area.sum()) / area.size


def hair_patch(path, box, rgb):
    """The solid rgb patch inside `box`, grown by HAIR_GROW: where that Link's hair is in this shot."""
    from scipy import ndimage
    m, w, _ = _class_mask(path, rgb)
    m &= _in_box(m.shape, box)
    grow = max(1, int(round(HAIR_GROW * _scale(w))))
    return ndimage.binary_dilation(m, iterations=grow) if m.any() else m


def solid_count(path, box, rgb, within=None):
    """How many pixels of rgb's class sit in a solid patch inside `box` (and inside the mask `within` when given),
    scaled to a REF_W-wide shot (a 320-wide fake shot counts 16x per pixel)."""
    m, w, _ = _class_mask(path, rgb)
    m &= _in_box(m.shape, box)
    if within is not None:
        m &= within
    return int(round(int(m.sum()) / (_scale(w) ** 2)))
