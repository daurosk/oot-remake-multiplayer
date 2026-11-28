"""Row 301 (owner, 0.12.1): local co-op windows fill their part of the screen, a half each for 2 players, each game's
picture of its own window's shape (not a 16:9 band, not a tiny picture with bars).

The fix (Hookshot patch 0047 + the kit's tools/release/kit/scripts/local-coop.ps1):
- the kit lays the windows out in physical pixels (DPI aware: on a 4K screen at 150 % Windows PowerShell got scaled
  pixels before), each half the whole height, and starts each game with its window's client size and shape
  (HOS_OUTPUT_RES / HOS_ASPECT / HOS_WINDOW_SIZE: Get-KitLocalShapeEnv);
- Longshot takes those narrow shapes (down to 1:2; HOS_ASPECT "8:9" read as a shape, not atof's 8).

How (the kit's own code on the test display, through split_layout.ps1; nothing of the kit copied here):
| Check | Judged by |
|---|---|
| the kit's layout of the test display: 2 halves, each the whole work-area height | split_layout.ps1 -Mode layout (Get-KitLocalLayout) |
| each game started with its window's switches | Pair.rejoin(x, env=the layout's switches + RX_VK_SHOT_W/H = HOS_OUTPUT_RES, so the bridge shot is the game's own pixels, not Longshot's fixed 1280x720): live.py relaunch --env, the same session |
| the game took the shape | its new log: "oe: output resolution WxH" = HOS_OUTPUT_RES and "boot: widescreen: picture WxH" (host/hos_main.c, patch 0047), the picture's ratio within 1 % of the window's client |
| the window is where the kit put it | split_layout.ps1 -Mode place (Set-KitLocalWindowPlace) answers GetWindowRect = the rect asked, client = the layout's client (+-2 px) |
| the picture fills the window (user-facing) | the bridge's framebuffer shot of each game: its shape = the client's (1 %), no black bar at any side (each 2 % strip has picture), compared to refs/split-fill-half-<x>.png (NO-REF until the owner approves one) |

Its own boot (the games are relaunched in a narrow shape; nothing after it on that boot): needs_env BT_SPLIT_FILL=1,
e.g. pytest tools/autotest/ingame --lanes 1 -k test_split_fill --env BT_SPLIT_FILL=1 --out <dir>
"""
import json
import os
import re
import subprocess
import sys

import pytest

from test_rows import need_child_in_play

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import placement   # noqa: E402  (tools/autotest/placement.py: the test display's monitor, real pixels)

ONLY_HERE = "BT_SPLIT_FILL=1"    # (this test's own boot: the games end in a narrow shape)
HELPER = os.path.join(HERE, "split_layout.ps1")
OUTPUT = re.compile(r"oe: output resolution (\d+)x(\d+)")                                  # host/nn_applet.c output_mode
PICTURE = re.compile(r"boot: widescreen: picture (\d+)x(\d+), scene (\d+)x(\d+), camera ratio ([\d.]+)")   # hos_main.c
NOT_TAKEN = re.compile(r"oe: output_res=\S+ is not a size the game can use")                # nn_applet.c output_parse
NO_CRASH = r"unhandled exception|fault at|coop: null read"
BAR_STRIP = 0.02     # (each side's strip of the shot, of its width or height)
BLACK = 12           # (a pixel whose every channel is at most this: black)


def helper(*args, timeout=60):
    """split_layout.ps1's JSON (its stdout is that JSON only; its log goes to stderr)."""
    p = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", HELPER] + list(args),
                       capture_output=True, text=True, timeout=timeout)
    try:
        out = json.loads(p.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        raise AssertionError("split_layout.ps1 %s: no JSON (exit %s): %r %r" % (" ".join(args), p.returncode,
                                                                                p.stdout[-300:], p.stderr[-300:]))
    if not out.get("ok"):
        raise AssertionError("split_layout.ps1 %s: %s" % (" ".join(args), out.get("error")))
    return out


def display_work():
    """The test display's work area (placement.py: OOT_TEST_DISPLAY, else DISPLAY2, else the primary), real pixels."""
    m = placement.find_monitor(placement.monitors(), placement.display_name(), primary_if_gone=True)
    return m["name"], tuple(m["work"])


def bars(path):
    """The sides of a shot that are a black bar ([] for none): each side's BAR_STRIP strip all but 1 % black."""
    from PIL import Image, ImageChops   # (Pillow: as imagecheck)
    im = Image.open(path).convert("RGB")
    w, h = im.size
    sw, sh = max(1, int(w * BAR_STRIP)), max(1, int(h * BAR_STRIP))
    out = []
    for side, box in (("left", (0, 0, sw, h)), ("right", (w - sw, 0, w, h)),
                      ("top", (0, 0, w, sh)), ("bottom", (0, h - sh, w, h))):
        r, g, b = im.crop(box).split()
        hist = ImageChops.lighter(ImageChops.lighter(r, g), b).histogram()   # (each pixel's brightest channel)
        if sum(hist[:BLACK + 1]) >= 0.99 * sum(hist):
            out.append(side)
    return out


def relaunch_env(shape):
    """A game's relaunch switches: the kit's shape switches, and its bridge shots at the game's own output size.
    The bridge's shot is scaled to a fixed RX_VK_SHOT_W x RX_VK_SHOT_H (1280x720 when unset: Longshot
    gpu/vk/vk_replay.cpp kShotW/kShotH, shot_ask_record), whatever the window's shape (run 0f93b38b 09:38: "shot
    1280x720" on a 944x1041 client whose game drew 944x1040). At the output size the shot is the game's own pixels:
    a 16:9 picture in a narrow window then shows as bars, not as a squeeze."""
    ow, oh = shape["HOS_OUTPUT_RES"].split("x")
    return ["%s=%s" % (k, v) for k, v in sorted(shape.items())] + ["RX_VK_SHOT_W=" + ow, "RX_VK_SHOT_H=" + oh]


@pytest.mark.row("#301 local co-op windows fill their half, each picture of its window's shape (patch 0047 + kit)")
@pytest.mark.needs_env(ONLY_HERE)
@pytest.mark.budget(1500)   # two relaunches (quit <= 60 s + boot <= 420 s each, Pair.REJOIN_*) and the waits around them
def test_split_fill(pair):
    with pair.setup():
        need_child_in_play(pair)
        name, work = display_work()
        lay = helper("-Mode", "layout", "-Work", ",".join(str(v) for v in work), "-Count", "2")
        rects, clients, shapes = lay["rects"], lay["clients"], lay["shapes"]
        pair.notes.append("display %s work %s dpi %s frame %s: rects %s clients %s" % (
            name, work, lay["dpi"], lay["frame"], rects, clients))
        assert len(rects) == 2 and all(r[3] == work[3] - work[1] for r in rects), \
            "the kit's 2 windows are not each the whole work-area height: %s in %s" % (rects, work)
        assert all(c[0] > 0 and c[1] > 0 for c in clients) and all(s.get("HOS_OUTPUT_RES") for s in shapes), \
            "the kit gave no picture size for a half of %s: %s %s" % (work, clients, shapes)
        narrow = (work[2] - work[0]) > (work[3] - work[1]) and clients[0][0] / clients[0][1] < 1.25
        pair.notes.append("a half's shape %.4f%s" % (clients[0][0] / clients[0][1],
                                                     " (narrower than 5:4: patch 0047's own range)" if narrow else ""))

    for i, x in enumerate(("A", "B")):
        env = relaunch_env(shapes[i])
        r = pair.rejoin(x, env=env)
        pid = r.get("pid")
        assert pid, "%s: live.py relaunch gave no pid: %s" % (x, r)
        pair.until("%s in play after its relaunch" % x, lambda: pair.ready(x), max_frames=1800)
        lines = pair.log[x].since(0)
        out = next((OUTPUT.search(ln) for ln in lines if OUTPUT.search(ln)), None)
        pic = next((PICTURE.search(ln) for ln in lines if PICTURE.search(ln)), None)
        assert not any(NOT_TAKEN.search(ln) for ln in lines), "%s: the game did not take HOS_OUTPUT_RES=%s" % (
            x, shapes[i]["HOS_OUTPUT_RES"])
        assert out and "%sx%s" % out.groups() == shapes[i]["HOS_OUTPUT_RES"], \
            "%s: output resolution %s, the kit asked %s" % (x, out and out.group(0), shapes[i]["HOS_OUTPUT_RES"])
        assert pic, "%s: no 'boot: widescreen: picture' line: the game drew its own 16:9 picture" % x
        pw, ph, ratio = int(pic.group(1)), int(pic.group(2)), float(pic.group(5))
        want = clients[i][0] / clients[i][1]
        assert abs(pw / ph - want) / want < 0.01 and abs(ratio - want) / want < 0.01, \
            "%s: picture %dx%d (camera ratio %.4f), the window's client %dx%d (%.4f)" % (
                x, pw, ph, ratio, clients[i][0], clients[i][1], want)
        pair.notes.append("%s: %s; %s" % (x, out.group(0), pic.group(0)))
        placed = helper("-Mode", "place", "-GamePid", str(pid), "-Rect", ",".join(str(v) for v in rects[i]),
                        "-Player", str(i + 1))
        wr, cl = placed["window"], placed["client"]
        assert wr == [rects[i][0], rects[i][1], rects[i][0] + rects[i][2], rects[i][1] + rects[i][3]], \
            "%s: window at %s, the kit asked %s" % (x, wr, rects[i])
        assert abs(cl[0] - clients[i][0]) <= 2 and abs(cl[1] - clients[i][1]) <= 2, \
            "%s: client %s, the kit's layout %s" % (x, cl, clients[i])
        pair.notes.append("%s: window %s client %s" % (x, wr, cl))

    pair.step_both(30)   # (each game presents at its new size)
    for i, x in enumerate(("A", "B")):
        s = pair.call(x, "shot", name="split-fill-probe-%s" % x)
        sw, sh = s.get("width"), s.get("height")
        want = clients[i][0] / clients[i][1]
        assert "%sx%s" % (sw, sh) == shapes[i]["HOS_OUTPUT_RES"], \
            "%s: shot %sx%s, not the game's output size %s (RX_VK_SHOT_W/H not taken)" % (
                x, sw, sh, shapes[i]["HOS_OUTPUT_RES"])
        assert abs(sw / sh - want) / want < 0.01, "%s: shot %sx%s, the window's shape %.4f" % (x, sw, sh, want)
        side = bars(s.get("path"))
        assert not side, "%s: black bar(s) at %s of its picture (%sx%s)" % (x, ", ".join(side), sw, sh)
        pair.check_shot(x, "split-fill-half")
        pair.never_log(x, NO_CRASH)
