"""Screenshot proof for autotest runs: a check graded from the run's own shots, offline and deterministic.

Owner rule (2026-11-18): no PASS from log lines alone. A log line proves a message was sent; a test of something
visible (platforms, doors, ladders, actors, items, HUD, puppets) passes only when a shot of each game it is about
shows the result. groups.py's see=<check> names a check defined here, in tools/autotest/proofs.txt:

    # name           shot (in the run folder)  region x,y,w,h (of a 1280x720 frame)  how
    deku-lifts-A     A-up2-A                   340,200,480,180                         refs
    door-open-B      B-after-B                 500,100,300,400                         change from=B-before-B min=18

shot: the run's <scenario>-<shot>.png (hookshot.py run saves every `mark NAME` of script X as
<scenario>-X-NAME-A.png and -B.png: one shot of each game at the moment the script marks it), or end-A / end-B.
A '*' matches anything: s*-b-rejoined-A for a steps file's shot (hs_steps.py: s<step>-<name>-A.png).

Kinds:
- refs: the region is compared with reference shots of the same scenario, script, mark and game, taken from earlier
  runs: <kit root>\\proof-refs\\<name>\\good-*.png (the right result) and bad-*.png (the wrong one). Full frames are
  kept (cropped at grading), so a region can be moved later without new shots. Reference shots are screenshots of
  the game: they stay on this PC (never in the repo). OK when the nearest good reference is nearer than the nearest
  bad one by `margin` and within `max`; BAD when the nearest bad one is nearer by `margin` and within `max`; else
  UNCLEAR (also with only one kind of reference: then nothing can be told apart). A reference that is the graded shot
  itself (byte for byte) is left out.
  Add one: python -I tools/autotest/proof.py ref add <name> good|bad <png> [--kit-root DIR]
- change: no references: the region must differ from the same game's shot `from` (an earlier mark, same pose) by at
  least `min` (OK), or, with `max=` instead, by at most `max` (nothing moved: OK). For a thing that appears or moves
  in front of a still camera (a door opens, a chest lid, a puppet arrives). A camera that moves between the two
  marks makes it meaningless: use it only where the script holds the camera still.
- same: the region of game A's and game B's shot of one mark are compared: OK when they differ by at most `max`.
  Only for views that do not depend on the player's own pose (a fixed HUD element, a menu, a map).

Distance: the region is cut into a grid (default 24x12 cells), each cell's mean luma (0-255); the distance is the
mean absolute cell difference. Coarse cells keep Navi's sparkle, dust and a frame of animation from swinging it.

    python -I tools/autotest/proof.py grade <run folder> <check> [<check>...] [--kit-root DIR]
    python -I tools/autotest/proof.py dist <png> <png> --region x,y,w,h
    python -I tools/autotest/proof.py ref add|list <name> [good|bad <png>]
"""
import argparse
import glob
import os
import re
import shutil
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PROOFS = os.path.join(HERE, "proofs.txt")
GRID = (24, 12)
MARGIN = 6.0         # refs: the nearer kind must be nearer by this much (luma levels)
MAX_GOOD = 30.0      # refs: and the good one no further than this


class ProofError(Exception):
    pass


# --- PNG (stdlib only: CI and the kit's Python have no Pillow) ---------------------------------------------------

def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """(width, height, channels, rows): rows = list of bytes, 8-bit RGB/RGBA/grey only (what the bridge writes)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ProofError("%s: not a PNG" % path)
    p, idat, hdr = 8, [], None
    while p + 8 <= len(data):
        n, t = struct.unpack(">I4s", data[p:p + 8])
        c = data[p + 8:p + 8 + n]
        p += 12 + n
        if t == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", c)
        elif t == b"IDAT":
            idat.append(c)
        elif t == b"IEND":
            break
    if not hdr:
        raise ProofError("%s: no IHDR" % path)
    w, h, depth, ctype, _, _, interlace = hdr
    ch = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or ch is None or interlace:
        raise ProofError("%s: only 8-bit non-interlaced grey/RGB/RGBA PNGs (depth %d type %d)" % (path, depth, ctype))
    raw = zlib.decompress(b"".join(idat))
    stride = w * ch
    rows, prev = [], bytes(stride)
    for y in range(h):
        o = y * (stride + 1)
        ft, line = raw[o], bytearray(raw[o + 1:o + 1 + stride])
        if ft == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                line[i] = (line[i] + ((line[i - ch] if i >= ch else 0) + prev[i]) // 2) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                c = prev[i - ch] if i >= ch else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 255
        elif ft != 0:
            raise ProofError("%s: bad filter %d" % (path, ft))
        rows.append(bytes(line))
        prev = rows[-1]
    return w, h, ch, rows


def write_png(path, w, h, rgb_rows):
    """A plain RGB PNG (tests make their frames with it)."""
    raw = b"".join(b"\x00" + bytes(r) for r in rgb_rows)

    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


_cache = {}


def grid(path, region, cells=GRID):
    """Mean luma of each cell of region (x, y, w, h; in a 1280x720 frame, scaled to the shot's size)."""
    key = (os.path.abspath(path), tuple(region), tuple(cells))
    if key in _cache:
        return _cache[key]
    w, h, ch, rows = read_png(path)
    sx, sy = w / 1280.0, h / 720.0
    x0, y0 = int(region[0] * sx), int(region[1] * sy)
    x1, y1 = min(w, int((region[0] + region[2]) * sx)), min(h, int((region[1] + region[3]) * sy))
    if x1 - x0 < cells[0] or y1 - y0 < cells[1]:
        raise ProofError("region %s is too small or outside %s (%dx%d)" % (region, path, w, h))
    gw, gh = cells
    sums, counts = [0.0] * (gw * gh), [0] * (gw * gh)
    step = 2   # every second pixel and row: the cells are means, 4x faster
    for y in range(y0, y1, step):
        r = rows[y]
        cy = (y - y0) * gh // (y1 - y0)
        for x in range(x0, x1, step):
            i = x * ch
            if ch >= 3:
                lum = (299 * r[i] + 587 * r[i + 1] + 114 * r[i + 2]) // 1000
            else:
                lum = r[i]
            k = cy * gw + (x - x0) * gw // (x1 - x0)
            sums[k] += lum
            counts[k] += 1
    g = [s / n if n else 0.0 for s, n in zip(sums, counts)]
    _cache[key] = g
    return g


def distance(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / float(len(a))


# --- check definitions --------------------------------------------------------------------------------------------

def parse_region(text):
    parts = text.split(",")
    if len(parts) != 4 or not all(p.strip().isdigit() for p in parts):
        raise ProofError("region %r: x,y,w,h" % text)
    return tuple(int(p) for p in parts)


def parse_proofs(text):
    """{name: check}: one check per line, 'name shot region kind [key=value...]'."""
    checks = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = line.split()
        if len(words) < 4:
            raise ProofError("proofs line %d: name shot region kind [key=value...]" % n)
        name, shot, region, kind = words[:4]
        if not re.match(r"^[A-Za-z0-9_.-]+$", name) or name in checks:
            raise ProofError("proofs line %d: bad or repeated name %r" % (n, name))
        if kind not in ("refs", "change", "same"):
            raise ProofError("proofs line %d: kind %r (refs, change, same)" % (n, kind))
        c = {"name": name, "shot": shot, "region": parse_region(region), "kind": kind, "margin": MARGIN,
             "max": None, "min": None, "from": None, "refs": name, "cells": GRID}
        for w in words[4:]:
            k, eq, v = w.partition("=")
            if not eq:
                raise ProofError("proofs line %d: %r is not key=value" % (n, w))
            if k in ("margin", "max", "min"):
                c[k] = float(v)
            elif k in ("from", "refs"):
                c[k] = v
            elif k == "cells":
                gw, _, gh = v.partition("x")
                c["cells"] = (int(gw), int(gh))
            else:
                raise ProofError("proofs line %d: unknown %s=" % (n, k))
        if kind == "change" and (not c["from"] or (c["min"] is None) == (c["max"] is None)):
            raise ProofError("proofs line %d: change needs from= and one of min= / max=" % n)
        if kind == "same" and c["max"] is None:
            raise ProofError("proofs line %d: same needs max=" % n)
        if kind == "refs" and c["max"] is None:
            c["max"] = MAX_GOOD
        checks[name] = c
    return checks


def load_proofs(path=PROOFS):
    with open(path, encoding="utf-8-sig") as f:
        return parse_proofs(f.read())


def find_shot(folder, shot):
    """<folder>/<scenario>-<shot>.png (or <shot>.png: end-A, end-B), else None."""
    exact = os.path.join(folder, shot + ".png")
    if os.path.isfile(exact):
        return exact
    pat = shot if "*" in shot else glob.escape(shot)   # '*': a steps file's s<N>-<name>-A (N: its step number)
    hits = sorted(set(glob.glob(os.path.join(glob.escape(folder), "*-%s.png" % pat))
                      + glob.glob(os.path.join(glob.escape(folder), "%s.png" % pat))))
    return hits[0] if len(hits) == 1 else None


def ref_dir(kit_root, name):
    return os.path.join(kit_root, "proof-refs", name)


def refs(kit_root, name):
    d = ref_dir(kit_root, name) if kit_root else None
    if not d or not os.path.isdir(d):
        return [], []
    good = sorted(glob.glob(os.path.join(glob.escape(d), "good-*.png")))
    bad = sorted(glob.glob(os.path.join(glob.escape(d), "bad-*.png")))
    return good, bad


def same_file(a, b):
    """A reference that is the graded shot itself (copied from this run) proves nothing: it is left out."""
    try:
        if os.path.getsize(a) != os.path.getsize(b):
            return False
        with open(a, "rb") as f, open(b, "rb") as g:
            return f.read() == g.read()
    except OSError:
        return False


def grade(folder, check, kit_root=None):
    """(state, evidence): state OK, BAD (the shot shows the wrong result), UNCLEAR or MISSING (no shot / refs)."""
    c = check
    shot = find_shot(folder, c["shot"])
    if not shot:
        return "MISSING", "%s: no shot %s" % (c["name"], c["shot"])
    try:
        g = grid(shot, c["region"], c["cells"])
        if c["kind"] == "refs":
            good, bad = refs(kit_root, c["refs"])
            good, bad = [r for r in good if not same_file(r, shot)], [r for r in bad if not same_file(r, shot)]
            if not good or not bad:
                return "MISSING", "%s: needs good and bad references in proof-refs\\%s (has %d good, %d bad)" % (
                    c["name"], c["refs"], len(good), len(bad))
            dg = min((distance(g, grid(r, c["region"], c["cells"])), os.path.basename(r)) for r in good)
            db = min((distance(g, grid(r, c["region"], c["cells"])), os.path.basename(r)) for r in bad)
            text = "%s: %s good %.1f (%s), bad %.1f (%s)" % (c["name"], c["shot"], dg[0], dg[1], db[0], db[1])
            if dg[0] + c["margin"] <= db[0] and dg[0] <= c["max"]:
                return "OK", text
            if db[0] + c["margin"] <= dg[0] and db[0] <= c["max"]:
                return "BAD", text
            return "UNCLEAR", text
        if c["kind"] == "change":
            before = find_shot(folder, c["from"])
            if not before:
                return "MISSING", "%s: no shot %s" % (c["name"], c["from"])
            d = distance(g, grid(before, c["region"], c["cells"]))
            text = "%s: %s vs %s changed %.1f" % (c["name"], c["shot"], c["from"], d)
            if c["min"] is not None:
                return ("OK" if d >= c["min"] else "BAD"), text + " (min %g)" % c["min"]
            return ("OK" if d <= c["max"] else "BAD"), text + " (max %g)" % c["max"]
        other = re.sub(r"-([AB])$", lambda m: "-" + "BA"[m.group(1) == "B"], c["shot"])
        oshot = find_shot(folder, other)
        if not oshot or other == c["shot"]:
            return "MISSING", "%s: no other game's shot %s" % (c["name"], other)
        d = distance(g, grid(oshot, c["region"], c["cells"]))
        return ("OK" if d <= c["max"] else "BAD"), "%s: %s vs %s %.1f (max %g)" % (
            c["name"], c["shot"], other, d, c["max"])
    except (OSError, ProofError, zlib.error) as e:
        return "MISSING", "%s: %s" % (c["name"], e)


# --- command line ---------------------------------------------------------------------------------------------------

def find_kit_root():
    d = os.path.dirname(os.path.dirname(HERE))
    while True:
        c = os.path.join(d, "coop-kit")
        if os.path.isdir(c):
            return c
        up = os.path.dirname(d)
        if up == d:
            return None
        d = up


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--kit-root", help="the folder with proof-refs\\ (default: the first coop-kit above the repo)")
    ap.add_argument("--proofs", default=PROOFS)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("grade")
    g.add_argument("folder")
    g.add_argument("checks", nargs="+")
    d = sub.add_parser("dist")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--region", required=True)
    r = sub.add_parser("ref")
    r.add_argument("op", choices=["add", "list"])
    r.add_argument("name")
    r.add_argument("kind", nargs="?", choices=["good", "bad"])
    r.add_argument("png", nargs="?")
    a = ap.parse_args(argv)
    kit = os.path.abspath(a.kit_root) if a.kit_root else find_kit_root()
    try:
        if a.cmd == "dist":
            reg = parse_region(a.region)
            print("%.1f" % distance(grid(a.a, reg), grid(a.b, reg)))
            return 0
        if a.cmd == "ref":
            if not kit:
                raise ProofError("no coop-kit found; pass --kit-root")
            if a.op == "list":
                good, bad = refs(kit, a.name)
                for p in good + bad:
                    print(p)
                return 0
            if not a.kind or not a.png:
                raise ProofError("ref add <name> good|bad <png>")
            read_png(a.png)
            dst = ref_dir(kit, a.name)
            os.makedirs(dst, exist_ok=True)
            n = len(glob.glob(os.path.join(glob.escape(dst), a.kind + "-*.png")))
            out = os.path.join(dst, "%s-%02d-%s" % (a.kind, n + 1, os.path.basename(a.png)))
            shutil.copy2(a.png, out)
            print(out)
            return 0
        checks = load_proofs(a.proofs)
        bad = 0
        for name in a.checks:
            if name not in checks:
                raise ProofError("no check %r in %s" % (name, a.proofs))
            state, text = grade(a.folder, checks[name], kit)
            print("%s %s" % (state, text))
            bad += state != "OK"
        return 1 if bad else 0
    except ProofError as e:
        print("proof.py: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
