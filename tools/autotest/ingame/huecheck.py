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
