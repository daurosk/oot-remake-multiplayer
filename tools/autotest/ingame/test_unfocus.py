"""Row 83 (#245): a game whose window is not in front must not lose frames in play.

Player report row 83 (coord reports/player-reports.md, 0.11.1): two games on one PC, "the second (unfocused) instance
is choppy". This replaces the parked probe coord-multiplayer/offline/row-83-gameplay-probe.ps1 (row-83-unfocus-2):

| Old probe check | Here |
|---|---|
| a save loaded, in play (to_play, the 'coop: test loaded' line) | the lane's boot (live-kakariko-child, in play after the smoke tier) + need_child_in_play |
| the 60 cap in play (frame_rate_cap=0 = 60, vk_menu.h:32) | set-up: each game's longshot_options.ini frame_rate_cap=0 |
| a game behind (Set-Front on another window) | set-up: each game's last 'hid: game window ...' line says 'not in front' (live.py opens the games behind: placement.py SW_SHOWNOACTIVATE) |
| fps per renderer ('vk: N presents, X fps') | Longshot's own 5 s windows 'vi: N frames presented (X fps over the last Y s, ...)' (nn_vi.c:704), the same presents counted by wall time |
| stutter ('vi: present intervals' buckets, > 22 ms share) | the same line (nn_vi.c:705), the same > 22 ms share |
| 4 games, CPU/GPU class, affinity (B1-B6, A4-A5) | NOT here: a lane is two games and OS knobs are outside the bridge (the report says so) |

The probe only measured; this test judges: each game behind holds >= 90 % of the 60 cap on average and misses vsync
(> 22 ms present) on <= 5 % of its presents. THE TWO NUMBERS ARE THE PORT AUTHOR'S DECISION (row-83-unfocus-3), not the
player report's: row 83 says only "choppy" and gives no figure. Reasoning: a 60 Hz frame is 16.7 ms; nn_vi.c's
17.8-22 ms bucket is still one late vsync, a present over 22 ms is a skipped one (the choppiness the player saw), so
over 1 in 20 skipped is visible stutter; and 90 % of the cap leaves room for the 5 s windows' own spread in play
(lane logs big-run-01113 epona-sync game-out-A: 59.9 / 59.3 fps windows, one 50.9). Coordinator's word on
the two numbers: see the report reports/row-83-unfocus-3.md (asked of B #130, COORD-LOG).

Fps is a wall-clock measure, so this one test lets both games run in real time (the bridge's `resume`, docs/playtest.md
"Bridge protocol") while it reads the logs, and holds them again after (`pause`). It looks at the logs every 0.5 s with
a sleep and no bridge call: a busy poll of the bridges would itself take CPU from the games it measures.
"""
import os
import re
import time

import pytest

from test_rows import need_child_in_play

CAP_FPS = 60          # frame_rate_cap=0 is the console's 60 (ls31-src gpu/vk/vk_menu.h:32 fps_cap; host/nn_vi.c fps_of)
MIN_SHARE = 0.90      # the row's pass line: mean fps of the full windows >= 90 % of the cap
MAX_LATE = 0.05       # the row's pass line: presents over 22 ms <= 5 %
WINDOWS = 4           # full 5 s windows judged per game (the first one after `resume` is partial: dropped)
POLL_S = 0.5

# ls31-src host/nn_hid.c:317 (logged when the game's window goes in front or behind; the first change after start)
FRONT = re.compile(r'hid: game window (in front|not in front) \(foreground window: process (\d+), class "([^"]*)"\)')
# ls31-src host/nn_vi.c:704 (every 5 s of presents)
WINDOW = re.compile(r"vi: \d+ frames presented \((\d+(?:\.\d+)?) fps over the last (\d+(?:\.\d+)?) s, interval (\d+)\)")
# ls31-src host/nn_vi.c:705 (right after it, the same window); the probe's own regex (row-83-gameplay-probe.ps1:250)
BUCKETS = re.compile(r"vi: present intervals: <15\.5 ms (\d+), 15\.5-17\.8 (\d+), 17\.8-22 (\d+), 22-30 (\d+), "
                     r"30-40 (\d+), over 40 (\d+)")


def frame_rate_cap(folder):
    """frame_rate_cap= of the game's longshot_options.ini (None if absent)."""
    try:
        with open(os.path.join(folder, "longshot_options.ini"), encoding="utf-8", errors="replace") as f:
            for ln in f:
                m = re.match(r"\s*frame_rate_cap\s*=\s*(\S+)", ln)
                if m:
                    return m.group(1)
    except OSError:
        return None
    return None


def last_front(pair, x):
    """The game's last 'hid: game window ...' line since the session began (pos 0), or None."""
    hits = [m.groups() for m in map(FRONT.search, pair.log[x].since(0)) if m]
    return hits[-1] if hits else None


def game_pids(pair):
    """{tag: pid} of the lane's games from the session's pids.json (as bt_plugin.py:624), so the SETUP-FAIL line
    shows whether the foreground window belongs to a game; the error text when unreadable."""
    try:
        import live
        return live.game_pids(pair.session)[0]
    except Exception as e:   # noqa: BLE001 (say so, never hide the real failure)
        return "game pids unreadable (%s)" % e


def foreground_info():
    """Who holds the foreground right now, read only (no focus change): 'pid N exe E class "C" exstyle 0xE'.
    (The exe basename, not the window title: a title can carry the user profile path into the summaries.)
    Row 83 SETUP-FAIL 03:59 UTC (row83-unfocus-182cd53b): neither game ever logged 'hid: game window ...', i.e. nn_hid.c
    game_in_front() said 'in front' on every read (was_front starts 1; a line only on a change). Longshot counts a
    WS_EX_TOOLWINDOW / WS_EX_NOACTIVATE foreground window as the game's own (nn_hid.c:309-312), so this names the window
    that decided it; the SETUP-FAIL message carries it."""
    try:
        import ctypes
        from ctypes import wintypes
        u = ctypes.windll.user32
        u.GetForegroundWindow.restype = wintypes.HWND
        w = u.GetForegroundWindow()
        if not w:
            return "no foreground window"
        pid = wintypes.DWORD(0)
        u.GetWindowThreadProcessId(w, ctypes.byref(pid))
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(w, cls, 64)
        exe = "?"
        k = ctypes.windll.kernel32
        k.OpenProcess.restype = wintypes.HANDLE
        h = k.OpenProcess(0x1000, False, pid.value)   # PROCESS_QUERY_LIMITED_INFORMATION
        if h:
            buf, n = ctypes.create_unicode_buffer(520), wintypes.DWORD(520)
            if k.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
                exe = os.path.basename(buf.value)
            k.CloseHandle(h)
        u.GetWindowLongPtrW.restype = ctypes.c_ssize_t
        ex = u.GetWindowLongPtrW(w, -20)   # GWL_EXSTYLE
        return 'pid %d exe %s class "%s" exstyle 0x%X' % (pid.value, exe, cls.value, ex & 0xFFFFFFFF)
    except Exception as e:   # not Windows / no desktop: say so, never hide the real failure
        return "foreground unreadable (%s)" % e


def windows(lines):
    """[(fps, seconds, buckets)] of each full window in `lines` (a 'vi: N frames presented' line + its buckets line)."""
    out, fps = [], None
    for ln in lines:
        m = WINDOW.search(ln)
        if m:
            fps = (float(m.group(1)), float(m.group(2)))
            continue
        b = BUCKETS.search(ln)
        if b and fps:
            out.append((fps[0], fps[1], [int(v) for v in b.groups()]))
            fps = None
    return out


@pytest.mark.budget(150)
@pytest.mark.row("#245 row 83: a game window not in front keeps its fps in play (row-83-gameplay-probe)")
def test_back_window_fps(pair):
    with pair.setup():
        need_child_in_play(pair)
        for x in ("A", "B"):
            cap = frame_rate_cap(pair.games[x]["folder"])
            assert cap == "0", "%s: frame_rate_cap=%s in %s (row 83 is the 60 cap: 0)" % (x, cap, pair.games[x]["folder"])
            front = last_front(pair, x)
            assert front and front[0] == "not in front", "%s: window not behind (last hid line: %s; foreground now: %s; game pids %s)" % (
                x, front, foreground_info(), game_pids(pair))
        marks = {x: pair.log[x].mark() for x in ("A", "B")}
    for x in ("A", "B"):
        pair.call(x, "resume")
    got = {}
    try:
        while True:
            got = {x: windows(pair.log[x].since(marks[x]))[1:] for x in ("A", "B")}   # [1:]: the partial first one
            if all(len(w) >= WINDOWS for w in got.values()):
                break
            pair.deadline.check("%d full vi windows per game (have %s)" % (
                WINDOWS, ", ".join("%s %d" % (x, len(w)) for x, w in sorted(got.items()))))
            time.sleep(POLL_S)
    finally:
        for x in ("A", "B"):
            pair.call(x, "pause")
    bad = []
    for x in ("A", "B"):
        w = got[x][:WINDOWS]
        mean = sum(f for f, _, _ in w) / len(w)
        presents = sum(sum(b) for _, _, b in w)
        late = sum(sum(b[3:]) for _, _, b in w) / max(1, presents)
        still = last_front(pair, x)
        note = "%s (behind: %s): fps %s mean %.1f, presents > 22 ms %.1f %% of %d" % (
            x, still[0] if still else "?", "/".join("%.1f" % f for f, _, _ in w), mean, 100 * late, presents)
        pair.notes.append(note)
        if still and still[0] != "not in front":
            bad.append("%s came in front during the measure (%s)" % (x, still))
        if mean < MIN_SHARE * CAP_FPS:
            bad.append("%s: mean %.1f fps < %.0f" % (note, mean, MIN_SHARE * CAP_FPS))
        elif late > MAX_LATE:
            bad.append("%s: > 22 ms presents over %.0f %%" % (note, 100 * MAX_LATE))
    assert not bad, "; ".join(bad)
