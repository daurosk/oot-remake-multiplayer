"""Where the test games' windows go: one monitor, chosen by its device name, and no focus stolen.

Every test run that starts games (tools/autotest/run.ps1 for the emulator, hookshot.py run for Longshot, which is also
what the live play-test kit and live.py start use) puts the game windows through this module, so a test game never
opens over what someone is working on.

The monitor is a device name, for example \\\\.\\DISPLAY2 (Settings > System > Display shows the number; "DISPLAY2" alone
works too). Pick it, strongest first:
  1. --display (hookshot.py run, live.py start, placement.py) or -Display (run.ps1, live_start.ps1);
  2. the environment variable OOT_TEST_DISPLAY;
  3. the default, \\\\.\\DISPLAY2.
"primary" names the primary monitor, whatever its number.
When the chosen monitor does not exist (DISPLAY2 unplugged or renumbered, a stale OOT_TEST_DISPLAY, a mistyped --display),
the games go to the primary monitor (owner 2026-11-24: tests may use it) and the run says so: the returned monitor has a
"note" that says which name was missing and which monitors exist. One rule for every caller (run.ps1, hookshot.py,
hs_steps.py, live.py, the kit's play-test start and the offline scripts). With no monitor at all it is an error.
The name is resolved to the monitor's work area (the screen minus the taskbar) when the games start, never as a
fixed 0,0, so a display added later, or the same display after the layout moved, only needs the setting changed.
Windows are placed side by side from the work area's top left, 960x600 each (smaller when the work area is too small,
so every window is fully on that monitor). Placing never activates a window (SWP_NOACTIVATE) and never restores a
minimized one with a call that activates it.

Command line (needs Windows for anything but `rects`):
  python placement.py list
  python placement.py rects [--display NAME] [--count N]
  python placement.py place --pid PID [--pid PID ...] [--display NAME] [--topmost] [--wait SECONDS] [--first-slot N --slots N]
  python placement.py launch --cwd DIR -- EXE [ARGS...]     (starts EXE without activating its window; prints its pid)
"""
import argparse
import json
import os
import subprocess
import sys
import time

ENV = "OOT_TEST_DISPLAY"
DEFAULT_DISPLAY = "\\\\.\\DISPLAY2"
WIN_W, WIN_H = 960, 600

SWP_NOACTIVATE = 0x0010
SWP_NOOWNERZORDER = 0x0200
SWP_SHOWWINDOW = 0x0040
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SW_SHOWNOACTIVATE = 4


class PlacementError(Exception):
    pass


def popen_flags():
    """Popen keywords for a game: its own process group, and (Windows) its window shown without being activated.
    STARTF_USESHOWWINDOW with SW_SHOWNOACTIVATE is what a game's first ShowWindow(SW_SHOWNORMAL / SHOWDEFAULT) uses, so
    a test game opening does not take the focus from the window being worked in. A game that activates itself
    (SetForegroundWindow, a raise) is not stopped by this: the lane check shows whether Longshot does."""
    if os.name != "nt":
        return {"start_new_session": True}
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = SW_SHOWNOACTIVATE
    return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP, "startupinfo": si}


def display_name(arg=None, env=None):
    """The chosen device name: arg, else $OOT_TEST_DISPLAY, else the default."""
    env = os.environ if env is None else env
    for v in (arg, env.get(ENV), DEFAULT_DISPLAY):
        if v and v.strip():
            return v.strip()


def normalize(name):
    """'DISPLAY2', 'display2' and '\\\\.\\DISPLAY2' are the same monitor: the comparison key."""
    n = name.strip().upper()
    if n.startswith("\\\\.\\"):
        n = n[4:]
    return n


PRIMARY = "primary"


def find_monitor(monitors, name, primary_if_gone=False):
    """The monitor dict (name, work=(l, t, r, b), primary) called name ("primary": the primary monitor), or a
    PlacementError that lists those found. primary_if_gone: a name no monitor has is the primary monitor (the
    chosen monitor was unplugged or renumbered; check_display always asks for it)."""
    want = normalize(name)
    for m in monitors:
        if normalize(m["name"]) == want or (want == PRIMARY.upper() and m.get("primary")):
            return m
    if primary_if_gone:
        for m in monitors:
            if m.get("primary"):
                return m
    found = ", ".join("%s (%dx%d at %d,%d%s)" % (m["name"], m["work"][2] - m["work"][0], m["work"][3] - m["work"][1],
                                                  m["work"][0], m["work"][1], ", primary" if m.get("primary") else "")
                      for m in monitors) or "none"
    raise PlacementError("no monitor called %s (monitors found: %s). Set the monitor with --display / -Display or %s."
                         % (name, found, ENV))


def layout(work, count, w=WIN_W, h=WIN_H):
    """count window rects (x, y, w, h) side by side from the work area's top left, all inside work (l, t, r, b).
    A window is w x h; when the area is narrower than count * w, or shorter than h, the windows shrink to fit."""
    left, top, right, bottom = work
    aw, ah = right - left, bottom - top
    if aw <= 0 or ah <= 0 or count <= 0:
        raise PlacementError("the work area %s cannot hold %d windows" % (work, count))
    w = min(w, aw // count)
    h = min(h, ah)
    return [(left + i * w, top, w, h) for i in range(count)]


# ---- Windows (ctypes; imported only where used so the placement math tests run anywhere) ----

def _user32():
    import ctypes
    from ctypes import wintypes
    u = ctypes.WinDLL("user32", use_last_error=True)
    u.GetMonitorInfoW.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
    u.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                               wintypes.UINT]
    u.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    u.IsIconic.argtypes = [wintypes.HWND]
    u.IsWindowVisible.argtypes = [wintypes.HWND]
    u.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    u.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    return u


def monitors():
    """The monitors now: [{name, work=(l, t, r, b), bounds=(l, t, r, b), primary}], in real pixels."""
    import ctypes
    from ctypes import wintypes
    u = _user32()
    try:
        u.SetProcessDPIAware()   # real pixels, not a scaled desktop's units (run.ps1 does the same)
    except OSError:
        pass

    class MONITORINFOEXW(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT), ("rcWork", wintypes.RECT),
                    ("dwFlags", wintypes.DWORD), ("szDevice", ctypes.c_wchar * 32)]

    out = []
    PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HANDLE, wintypes.HDC, ctypes.POINTER(wintypes.RECT),
                              wintypes.LPARAM)

    def cb(hmon, hdc, rect, lparam):
        mi = MONITORINFOEXW()
        mi.cbSize = ctypes.sizeof(MONITORINFOEXW)
        if u.GetMonitorInfoW(hmon, ctypes.byref(mi)):
            r, w = mi.rcMonitor, mi.rcWork
            out.append({"name": mi.szDevice, "bounds": (r.left, r.top, r.right, r.bottom),
                        "work": (w.left, w.top, w.right, w.bottom), "primary": bool(mi.dwFlags & 1)})
        return True

    u.EnumDisplayMonitors.argtypes = [wintypes.HDC, ctypes.c_void_p, PROC, wintypes.LPARAM]
    u.EnumDisplayMonitors(None, None, PROC(cb), 0)
    return sorted(out, key=lambda m: m["name"])


def game_window(pid):
    """The game's own window of process pid: the largest visible top-level window that is not the co-op cheats window
    (with LICHII_COOP_CHEATS=1 that window can be the one Windows reports first), or 0."""
    import ctypes
    from ctypes import wintypes
    u = _user32()
    best = [0, 0]
    PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(h, lparam):
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value != pid or not u.IsWindowVisible(h):
            return True
        buf = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, buf, 256)
        if buf.value.startswith("Co-op item cheats"):
            return True
        r = wintypes.RECT()
        u.GetClientRect(h, ctypes.byref(r))
        area = (r.right - r.left) * (r.bottom - r.top)
        if area > best[1]:
            best[0], best[1] = h, area
        return True

    u.EnumWindows.argtypes = [PROC, wintypes.LPARAM]
    u.EnumWindows(PROC(cb), 0)
    return best[0]


def place_window(hwnd, rect, topmost=False):
    """Moves and sizes the window to rect (x, y, w, h) without activating it. Topmost only when asked (a screen copy
    needs the game above other windows; it stays on the chosen monitor, so it does not cover anything elsewhere)."""
    u = _user32()
    if u.IsIconic(hwnd):
        u.ShowWindow(hwnd, SW_SHOWNOACTIVATE)   # (never SW_RESTORE: it activates)
    x, y, w, h = rect
    if not u.SetWindowPos(hwnd, HWND_TOPMOST if topmost else HWND_NOTOPMOST, x, y, w, h,
                          SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW):
        raise PlacementError("SetWindowPos failed for window 0x%x" % hwnd)


def check_display(arg=None):
    """The chosen monitor (dict), or a PlacementError when the PC has no monitor; call it before starting games. A name
    no monitor has gives the primary monitor, with mon["note"] saying so (callers print it)."""
    name = display_name(arg)
    mons = monitors()
    mon = find_monitor(mons, name, primary_if_gone=True)
    if normalize(mon["name"]) != normalize(name) and normalize(name) != PRIMARY.upper():
        mon = dict(mon, note="no monitor called %s: the windows go to the primary monitor %s (monitors found: %s)"
                   % (name, mon["name"], ", ".join(m["name"] for m in mons)))
    return mon


def place_pids(pids, display=None, topmost=False, wait=0.0, step=0.25, slots=None, count=None):
    """Places the game windows of pids side by side on the chosen monitor: pid i goes in slot slots[i] (default i) of
    count slots (default: as many as the biggest slot needs, at least len(pids)), so a game placed alone keeps the
    place it will have beside the other. Waits up to wait seconds for a pid that has no window yet.
    Returns [(pid, rect)] for those placed; a pid whose window never came is left out."""
    mon = check_display(display)
    slots = list(range(len(pids))) if slots is None else list(slots)
    rects = layout(mon["work"], max([count or 0, len(pids)] + [s + 1 for s in slots]))
    placed = []
    deadline = time.time() + wait
    for pid, slot in zip(pids, slots):
        h = game_window(pid)
        while not h and time.time() < deadline:
            time.sleep(step)
            h = game_window(pid)
        if h:
            place_window(h, rects[slot], topmost)
            placed.append((pid, rects[slot]))
    return placed


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    r = sub.add_parser("rects")
    r.add_argument("--display")
    r.add_argument("--count", type=int, default=2)
    pl = sub.add_parser("place")
    pl.add_argument("--pid", type=int, action="append", required=True)
    pl.add_argument("--display")
    pl.add_argument("--topmost", action="store_true")
    pl.add_argument("--wait", type=float, default=0.0)
    pl.add_argument("--first-slot", type=int, default=0, help="the first pid's place (0 = left); the next pids follow")
    pl.add_argument("--slots", type=int, help="how many places the monitor is shared in (default: as many as needed)")
    la = sub.add_parser("launch")
    la.add_argument("--cwd", required=True)
    la.add_argument("cmd_line", nargs=argparse.REMAINDER, help="-- EXE [ARGS...]")
    a = p.parse_args(argv)
    try:
        if a.cmd == "list":
            print(json.dumps(monitors(), indent=1))
        elif a.cmd == "rects":
            mon = check_display(a.display)
            out = {"display": mon["name"], "rects": layout(mon["work"], a.count)}
            if mon.get("note"):
                out["note"] = mon["note"]
            print(json.dumps(out))
        elif a.cmd == "launch":
            cmd = [c for i, c in enumerate(a.cmd_line) if not (i == 0 and c == "--")]
            if not cmd:
                raise PlacementError("launch: no program given")
            print(subprocess.Popen(cmd, cwd=a.cwd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, **popen_flags()).pid)
        else:
            slots = [a.first_slot + i for i in range(len(a.pid))]
            print(json.dumps({"placed": place_pids(a.pid, a.display, a.topmost, a.wait, slots=slots, count=a.slots)}))
    except PlacementError as e:
        print("placement: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
