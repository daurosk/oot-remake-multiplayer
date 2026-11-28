"""VOID gpu-reset: a test that failed only because the NVIDIA driver reset the whole GPU (B #161, 2026-11-28).

Source of the rule: crash-sweep-0120-4 (reports/crash-sweep-0120-4.md). banner-steady-53d40ab3-2 lane 1, 00:14:13 UTC: Windows
System log provider 'nvlddmkm' event 14 ("GPU recovery action changed from 0x0 (None) to 0x1 (PF FLR)") and event 153
("Restarting TDR occurred on GPUID:100"); then BOTH games' logs (session/game-out-A.txt / -B.txt, the runner's game output,
live.py) say 'vk: FATAL: the GPU device was lost (VK_ERROR_DEVICE_LOST)', and Explorer crashed in the same second. Same
signature on 11-22 16:38 and 11-23 15:02 UTC.

Guards (B #161: it may never hide a crash of ours):
  - VOID only when BOTH hold: a System nvlddmkm event with id 14 or 153 inside the session window (session.json 'started'
    .. now), AND a game of the session whose own log has VK_ERROR_DEVICE_LOST and was last written at or after that event
    (the FATAL line comes after the reset; its file's mtime is the latest write).
  - DEVICE_LOST with no driver event in the window, a driver event with no DEVICE_LOST, an event before the session or
    after the log's last write: no VOID (the test keeps its FAIL / SETUP-FAIL).
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
        ev = next((e for e in evs if started <= e[0] <= now and e[0] <= mtime + SLACK_S), None)
        if ev:
            return ("VOID gpu-reset: System log %s event %d at %s UTC (session window), then game %s's log %s has %s"
                    % (PROVIDER, ev[1], time.strftime("%H:%M:%S", time.gmtime(ev[0])), x, os.path.basename(log), DEVICE_LOST))
    return None
