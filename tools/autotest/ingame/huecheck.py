"""vest-link's colour check (test_vest_link.py; its owner's file, as kokiri-band's bandcheck.py): the pixels that CHANGED
between two bridge shots of the same place, and how many of them take a colour's hue. No guessed screen region: what a
recolour changes is found by the change itself.

The defaults (moved, saturated, near) are starting values, NOT MEASURED: test_vest_link.py ends NO-REF until its
MEASURED cites a lane run's huecheck lines (worker-rules: no guessed values).
"""
WIDTH = 320


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
    from PIL import Image

    def load(path, size=None):
        with Image.open(path) as im:
            im = im.convert("RGB")
            if size is None:
                size = (width, max(1, round(im.height * width / im.width)))
            return np.asarray(im.resize(size, Image.BILINEAR), dtype=np.float64) / 255.0, size

    a, size = load(before)
    b, _ = load(after, size)
    moved_px = np.abs(b - a).max(axis=2) > moved
    hsv = np.asarray(Image.fromarray((b * 255).astype("uint8"), "RGB").convert("HSV"), dtype=np.float64) / 255.0
    hue, sat = hsv[..., 0] * 360.0, hsv[..., 1]
    off = np.abs((hue - hue_of(rgb) + 180.0) % 360.0 - 180.0)
    coloured = moved_px & (sat > saturated) & (off <= near)
    n, c = int(moved_px.sum()), int(coloured.sum())
    return {"changed": n, "coloured": c, "share": round(c / n, 3) if n else 0.0, "hue": round(hue_of(rgb), 1)}
