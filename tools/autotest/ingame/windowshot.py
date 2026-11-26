"""WINDOW shots for the in-game tests (Pair.window_shot): the screen's copy of a game's window, Longshot's ImGui overlays
(its menu, co-op's map arrows, tabs, name tags) included. A bridge `shot` never has those: Longshot takes it before its
overlays are drawn (hs_steps.py wshot docstring: vk_replay.cpp shot_ask_record; run f1-event-aebd16089's s30-f1-title).

The ONE window-shot helper is coord-multiplayer/offline/lib/screen-shot.ps1, its Save-WindowShot (worker rule C #124:
the game raised, 9 points of its window checked to be its own process; a covered window is saved as -COVERED.png).
It is called exactly as hs_steps.py's `wshot` calls it (window_shot_command + run_window_shot: one command, its exit
codes, its hang limit), so there is one caller format for both. Helper: OOT_WINDOW_SHOT, else the default path below.

Every way it can go wrong is a SETUP-FAIL (pair.SetupFail), never a PASS and never a FAIL of the row: no helper file,
no pid for the game in the session's pids.json, a covered window (exit 3), no window / a failed copy (exit 4), no such
process (exit 2), a hang, or no png on disk after exit 0.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AUTOTEST = os.path.dirname(HERE)
if AUTOTEST not in sys.path:
    sys.path.insert(0, AUTOTEST)
import pair as P   # noqa: E402

ENV = "OOT_WINDOW_SHOT"      # (= hs_steps.WINDOW_SHOT_ENV; checked equal by tests/test_ingame.py)
LOG = "window-shots.txt"     # Save-WindowShot's -Log: one line per minimised cover, covered or failed copy


def default_helper():
    """<repo>/../coord-multiplayer/offline/lib/screen-shot.ps1: every worktree (wt-*, wt-module-<sha8>) and the main
    checkout sit beside coord-multiplayer in the same folder."""
    repo = os.path.dirname(os.path.dirname(AUTOTEST))
    return os.path.join(os.path.dirname(repo), "coord-multiplayer", "offline", "lib", "screen-shot.ps1")


def helper():
    return os.environ.get(ENV) or default_helper()


def session_pids(session):
    """{tag: pid} of a live.py session's games (pids.json, hookshot.py run --keep-open; = live.game_pids)."""
    try:
        with open(os.path.join(session, "pids.json"), encoding="utf-8") as f:
            games = (json.load(f) or {}).get("games") or {}
    except (OSError, ValueError) as e:
        raise P.SetupFail("window shot: no pids.json in the session %s (%s)" % (session, e))
    return {x: int(v) for x, v in games.items() if v}


def window_bounds(pid):
    """(window rect, client rect on screen) of pid's largest visible top-level window, as (l, t, r, b) pairs in ONE
    coordinate space (this process's: crop() scales to the png), else None. The window rect is what Save-WindowShot
    copies (GetWindowRect: title bar, frame and Windows 11's invisible borders, where the desktop shows through); the
    client rect is the game's picture with its overlays."""
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes
    u = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(h, _):
        owner = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(owner))
        if owner.value == pid and u.IsWindowVisible(h) and not u.IsIconic(h):
            w, c = wintypes.RECT(), wintypes.RECT()
            if u.GetWindowRect(h, ctypes.byref(w)) and u.GetClientRect(h, ctypes.byref(c)):
                o = wintypes.POINT(0, 0)
                u.ClientToScreen(h, ctypes.byref(o))
                found.append(((w.left, w.top, w.right, w.bottom), (o.x, o.y, o.x + c.right, o.y + c.bottom)))
        return True
    u.EnumWindows(each, 0)
    if not found:
        return None
    return max(found, key=lambda f: (f[0][2] - f[0][0]) * (f[0][3] - f[0][1]))


def crop(path, bounds):
    """Cuts the saved window shot down to its client area (in place). The offsets are scaled by png size / window
    rect size, so a DPI-virtualised rect (this process vs powershell's) still lands on the same pixels."""
    from PIL import Image
    (wl, wt, wr, wb), (cl, ct, cr, cb) = bounds
    with Image.open(path) as im:
        im = im.convert("RGB")
    sx, sy = im.width / float(max(1, wr - wl)), im.height / float(max(1, wb - wt))
    box = (round((cl - wl) * sx), round((ct - wt) * sy), round((cr - wl) * sx), round((cb - wt) * sy))
    if not (0 <= box[0] < box[2] <= im.width and 0 <= box[1] < box[3] <= im.height) or box[2] - box[0] < 100:
        raise P.SetupFail("window shot: client area %s does not fit the %dx%d shot (window %s)"
                          % (box, im.width, im.height, bounds[0]))
    im.crop(box).save(path)
    return box


class Shooter:
    """Pair.window_shooter of a real lane: (x, path) -> path of the saved png cut to the window's client area (the
    game's picture + overlays, no title bar, no desktop), else SetupFail."""

    def __init__(self, session, helper_path=None, run=None, bounds=window_bounds):
        self.session = session
        self.helper = helper_path or helper()
        self.run = run          # (tests: a fake subprocess.run)
        self.bounds = bounds    # (tests: a fake window_bounds)

    def __call__(self, x, path):
        import hs_steps as hs
        if not os.path.isfile(self.helper):
            raise P.SetupFail("window shot %s: no helper file %s (set %s)" % (x, self.helper, ENV))
        pid = session_pids(self.session).get(x)
        if not pid:
            raise P.SetupFail("window shot %s: the session's pids.json names no pid for game %s" % (x, x))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        cmd = hs.window_shot_command(self.helper, pid, path, os.path.join(os.path.dirname(path), LOG))
        try:
            code, text = (hs.run_window_shot(cmd, run=self.run) if self.run else hs.run_window_shot(cmd))
        except hs.SetupFail as e:          # (hs_steps' own SetupFail type: made the pair's, so bt_plugin sees SETUP-FAIL)
            raise P.SetupFail("window shot %s: %s" % (x, e))
        if code != 0:
            raise P.SetupFail("window shot %s: not taken (exit %s: %s)" % (x, code, text or "no output"))
        if not os.path.isfile(path):
            raise P.SetupFail("window shot %s: the helper said '%s' but %s is not on disk" % (x, text, path))
        b = self.bounds(pid)     # (right after the shot: the helper raised the window, nothing moves it)
        if not b:
            raise P.SetupFail("window shot %s: no visible window of pid %d to cut the client area from" % (x, pid))
        crop(path, b)            # (the title bar and the desktop at the edges would make every ref compare flaky)
        return path
