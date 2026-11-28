"""VOID gpu-reset: a test that failed only because the NVIDIA driver reset the whole GPU (B #161, 2026-11-28).

Source of the rule: crash-sweep-0120-4 (reports/crash-sweep-0120-4.md). banner-steady-53d40ab3-2 lane 1, 00:14:13 UTC: Windows
System log provider 'nvlddmkm' event 14 ("GPU recovery action changed from 0x0 (None) to 0x1 (PF FLR)") and event 153
("Restarting TDR occurred on GPUID:100"); then BOTH games' logs (session/game-out-A.txt / -B.txt, the runner's game output,
live.py) say 'vk: FATAL: the GPU device was lost (VK_ERROR_DEVICE_LOST)', and Explorer crashed in the same second. Same
signature on 11-22 16:38 and 11-23 15:02 UTC.

Guards (B #161: it may never hide a crash of ours):
  - VOID only when BOTH hold: a System nvlddmkm event with id 14 or 153 inside the session window (session.json 'started'
    .. now), AND a game of the session whose own log has VK_ERROR_DEVICE_LOST, last written (mtime: the game aborts on
    that FATAL line, so its last write is the FATAL) 0 .. MAX_GAP_S after that event (review-tests-153-3 00:37: bounded,
    so an earlier reset the games survived never VOIDs a later crash of ours).
  - MAX_GAP_S from the three real cases (System log ms stamps vs the game's own crash record 'when:' / log mtime; the 153s
    repeat every ~2 s for the whole reset, so the gap is to the nearest event before the device loss):
      11-22 16:38:53.088 first 153 .. 16:38:57.582 last; lane1-A/B crash records 16:38:59Z      -> 1.4 s (6 s to the first)
      11-23 15:02:13.118 first 153; lane1-A/B crash records 15:02:13Z                            -> < 1 s
      11-28 00:14:13.408 first 153 .. 00:14:49.467 last; A record 00:14:27Z, log mtime 00:14:27.6 (153 at 00:14:27.365),
            B log mtime 00:14:51.4                                                                -> 0.3 s / 2.0 s
    (records: coord offline/lane-prep-ae97cc77/pending-kept/{A,B}/crash-2026112{2T163859,3T150213}Z.txt, coop-kit
    lane1-A/bug-reports/pending/crash-20261128T001427Z.txt). MAX_GAP_S = 10 s covers them with room, an hour never.
  - DEVICE_LOST with no driver event in the window, a driver event with no DEVICE_LOST, an event before the session, after
    the log's last write or more than MAX_GAP_S before it: no VOID (the test keeps its FAIL / SETUP-FAIL).
  - A second gpu-reset of the same test on the same commit (the job's rerun) is FAIL 'gpu-reset twice: maybe ours'
    (a TDR is the driver's answer to a GPU hang, which our renderer could cause): the ledger below, keyed test + commit.
  - No event log (not Windows, Get-WinEvent missing or failing): no VOID.
  - Only FAIL and SETUP-FAIL rows can become VOID; a VOID is never a PASS (bt_plugin: verdict NOT PASS, exit 1).
"""
import json
import os
import subprocess
import sys
import time

PROVIDER = "nvlddmkm"
EVENT_IDS = (14, 153)          # 14: GPU recovery action (FLR); 153: "Restarting TDR occurred" (reports/crash-sweep-0120-4.md)
DEVICE_LOST = "VK_ERROR_DEVICE_LOST"
FAKE_ENV = "BT_FAKE_GPU_EVENTS"  # selftests only: a JSON file [{"t": <unix s>, "id": 153}], read only while this is set
SLACK_S = 2.0                  # clock slack between the event log's stamp and the log file's mtime
MAX_GAP_S = 10.0               # longest gap reset event -> DEVICE_LOST seen: 2.0 s (the three cases above)
LEDGER_ENV = "BT_GPU_VOID_LEDGER"  # the ledger file (selftests point it at a temp file); default under LOCALAPPDATA

_PS = ("$s=[DateTimeOffset]::FromUnixTimeMilliseconds({t0}).LocalDateTime;"
       "$e=[DateTimeOffset]::FromUnixTimeMilliseconds({t1}).LocalDateTime;"
       "Get-WinEvent -FilterHashtable @{{LogName='System';ProviderName='" + PROVIDER + "';Id=14,153;StartTime=$s;EndTime=$e}}"
       " -ErrorAction SilentlyContinue | ForEach-Object {{ '{{0}} {{1}}' -f ([DateTimeOffset]$_.TimeCreated).ToUnixTimeMilliseconds(),$_.Id }}")


def driver_resets(t0, t1):
    """[(unix s, id)] of the System log's nvlddmkm 14/153 events between t0 and t1 (unix s); [] when none or unknown."""
    fake = os.environ.get(FAKE_ENV)
    if fake:
        with open(fake, encoding="utf-8") as f:
            evs = [(float(e["t"]), int(e["id"])) for e in json.load(f)]
        return sorted(e for e in evs if t0 <= e[0] <= t1 and e[1] in EVENT_IDS)
    if sys.platform != "win32":
        return []
    cmd = _PS.format(t0=int(t0 * 1000), t1=int(t1 * 1000))
    try:
        p = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd],
                           capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return []
    out = []
    for ln in p.stdout.splitlines():
        parts = ln.split()
        if len(parts) == 2 and parts[0].isdigit() and parts[1].isdigit() and int(parts[1]) in EVENT_IDS:
            out.append((int(parts[0]) / 1000.0, int(parts[1])))
    return sorted(out)


def lost_games(session):
    """{game: (log path, mtime)} for each game of the live.py session whose log has VK_ERROR_DEVICE_LOST."""
    try:
        with open(os.path.join(session, "session.json"), encoding="utf-8") as f:
            s = json.load(f)
    except (OSError, ValueError, TypeError):
        return {}, None
    out = {}
    for x, g in sorted((s.get("games") or {}).items()):
        log = (g or {}).get("log")
        try:
            with open(log, encoding="utf-8", errors="replace") as f:
                hit = any(DEVICE_LOST in ln for ln in f)
            if hit:
                out[x] = (log, os.path.getmtime(log))
        except (OSError, TypeError):
            continue
    return out, float(s.get("started") or 0) or None


def judge(session, now, resets=driver_resets):
    """'VOID gpu-reset: ...' when the session's games lost the device right after a driver reset (rules above), else None."""
    if not session:
        return None
    lost, started = lost_games(session)
    if not lost or not started:
        return None
    evs = resets(started, now)
    for x, (log, mtime) in sorted(lost.items()):
        ev = next((e for e in reversed(evs) if started <= e[0] <= now and -SLACK_S <= mtime - e[0] <= MAX_GAP_S), None)
        if ev:
            return ("VOID gpu-reset: System log %s event %d at %s UTC (session window), then game %s's log %s has %s"
                    % (PROVIDER, ev[1], time.strftime("%H:%M:%S", time.gmtime(ev[0])), x, os.path.basename(log), DEVICE_LOST))
    return None


def ledger_path():
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    return os.environ.get(LEDGER_ENV) or os.path.join(base, "oot-autotest", "gpu-voids.json")


def second_reset(test, commit, run_id, why, path=None):
    """None the first time this test (on this commit) is VOID gpu-reset (it is recorded), else the earlier entry:
    the rerun of a VOID job reset the GPU again -> FAIL 'gpu-reset twice: maybe ours'. The same run (run_id) never
    counts twice (all tests after one reset are VOID once each)."""
    path = path or ledger_path()
    key = "%s@%s" % (test, commit or "")
    try:
        with open(path, encoding="utf-8") as f:
            book = json.load(f)
    except (OSError, ValueError):
        book = {}
    old = book.get(key)
    if old and old.get("run") != run_id:
        return old
    if not old:
        book[key] = {"run": run_id, "when": time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime()), "why": why[:300]}
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".tmp%d" % os.getpid()
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(book, f, indent=1)
            os.replace(tmp, path)
        except OSError:
            pass
    return None
