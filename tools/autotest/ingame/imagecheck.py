"""Compare a bridge shot with its stored reference: SSIM (scikit-image) on EACH colour channel (R, G, B), both scaled to
320 wide; the score is the worst tile of 8 x 8, taking each tile's worst channel (ssim()). In colour because a colour
fault (a tunic, cap trim, gear or HUD tint) keeps the grey picture the same: a grey SSIM would pass it
(review-rows-c130 10:38).

Only rows about looks take shots (worker-rules TEST SYSTEM). A shot is the game's framebuffer from the bridge's
`shot` (no overlay, no window, no monitor), so the same scene from the same place gives the same picture up to
animation and lighting noise; `tolerance` is the lowest SSIM a test accepts (each test states its own).

compare() never makes a reference: a missing one is kept as <keep_dir>/new/<name>.png and the result is 'NO-REF'
(not ok). A person looks at it and copies it into tools/autotest/ingame/refs/ once it shows the right thing.
"""
import os
import shutil

WIDTH = 320
GRID = 8


def load_grey(path, width=WIDTH):
    import numpy as np
    from PIL import Image
    with Image.open(path) as im:
        im = im.convert("L")
        h = max(1, round(im.height * width / im.width))
        im = im.resize((width, h), Image.BILINEAR)
        return np.asarray(im, dtype=np.float64) / 255.0


def load_rgb(path, width=WIDTH):
    import numpy as np
    from PIL import Image
    with Image.open(path) as im:
        im = im.convert("RGB")
        h = max(1, round(im.height * width / im.width))
        im = im.resize((width, h), Image.BILINEAR)
        return np.asarray(im, dtype=np.float64) / 255.0


def ssim(a_path, b_path, grid=GRID, colour=True):
    """The LOWEST mean SSIM of a grid x grid split of the SSIM map: a small thing missing (a mask, a HUD block) drops
    its own tile even when the rest of the picture is the same (the whole picture's mean SSIM would hide it). With
    colour (the default), SSIM per channel and each tile's worst channel: a recoloured thing drops its tile too.
    colour=False: the grey picture only (kept for the selftest that shows why grey is blind to colour)."""
    from skimage.metrics import structural_similarity
    a, b = (load_rgb(a_path), load_rgb(b_path)) if colour else (load_grey(a_path), load_grey(b_path))
    if a.shape != b.shape:
        return 0.0, "sizes differ %s vs %s (aspect ratio changed)" % (a.shape, b.shape)
    if colour:
        _, m = structural_similarity(a, b, data_range=1.0, full=True, channel_axis=2)
        m = m.min(axis=2)                  # (each pixel's worst channel)
    else:
        _, m = structural_similarity(a, b, data_range=1.0, full=True)
    h, w = m.shape
    tiles = [m[i * h // grid:(i + 1) * h // grid, j * w // grid:(j + 1) * w // grid].mean()
             for i in range(grid) for j in range(grid)]
    return float(min(tiles)), None


def compare(shot, ref, tolerance, keep_dir=None):
    out = {"path": shot, "ref": ref, "score": None, "ok": False, "tolerance": tolerance}
    if not shot or not os.path.exists(shot):
        out["why"] = "no shot file (%s)" % shot
        return out
    if keep_dir:
        os.makedirs(os.path.join(keep_dir, "shots"), exist_ok=True)
        kept = os.path.join(keep_dir, "shots", os.path.basename(shot))
        shutil.copyfile(shot, kept)
        out["path"] = kept
    if not os.path.exists(ref):
        if keep_dir:
            os.makedirs(os.path.join(keep_dir, "new"), exist_ok=True)
            shutil.copyfile(shot, os.path.join(keep_dir, "new", os.path.basename(ref)))
        out["why"] = "NO-REF: no reference %s yet (candidate kept under new/)" % os.path.basename(ref)
        return out
    score, err = ssim(shot, ref)
    out["score"] = round(score, 4)
    if err:
        out["why"] = err
        return out
    out["ok"] = score >= tolerance
    out["why"] = "colour SSIM %.4f %s %.2f" % (score, ">=" if out["ok"] else "<", tolerance)
    return out
