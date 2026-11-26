"""Each game's commit charge, sampled from the lane's boot start (C #134: target ~20 GB per game; the pilot
test-bridge-pilot-fa63e90f saw A ~19 GB, B ~30 GB, flat over 35-38 min, B's size already there at the boot).

A thread reads the live.py session's pids.json (hookshot.py run --keep-open writes {"games": {"A": pid, "B": pid}})
as soon as it appears, then every `every` s takes each game's PrivateUsage (Windows GetProcessMemoryInfo with
PROCESS_MEMORY_COUNTERS_EX: the commit charge Task Manager calls "Commit size") into <out>/commit.csv, and at stop()
writes <out>/commit.json: per game the first, peak and last GB and the seconds after the boot start of each. Off
Windows (CI) nothing is read: the file says so. Measuring only: no limit is judged here.
"""
import csv
import json
import os
import sys
import threading
import time

GB = 1024.0 ** 3


def private_bytes(pid):
    """The commit charge of process `pid` in bytes, or None (not Windows, no such process, no access)."""
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes

    class PMCEX(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                    ("PrivateUsage", ctypes.c_size_t)]
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMCEX), wintypes.DWORD]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    h = k32.OpenProcess(0x1000, False, int(pid))      # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    try:
        c = PMCEX()
        c.cb = ctypes.sizeof(PMCEX)
        if not psapi.GetProcessMemoryInfo(h, ctypes.byref(c), c.cb):
            return None
        return int(c.PrivateUsage)
    finally:
        k32.CloseHandle(h)


class Sampler:
    def __init__(self, session_dir, out_dir, every=30.0, read=private_bytes, clock=time.monotonic):
        self.session, self.out, self.every, self.read, self.clock = session_dir, out_dir, float(every), read, clock
        self.t0 = clock()
        self.rows, self.pids = [], {}
        self.stop_ev = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def _pids(self):
        try:
            with open(os.path.join(self.session, "pids.json"), encoding="utf-8") as f:
                return {x: int(p) for x, p in ((json.load(f) or {}).get("games") or {}).items() if p}
        except (OSError, ValueError, TypeError):
            return {}

    def sample(self):
        """One row: seconds since the boot start and each game's GB (None: not read)."""
        self.pids = self._pids() or self.pids
        if not self.pids:
            return None
        row = {"s": round(self.clock() - self.t0, 1), "utc": time.strftime("%H:%M:%S", time.gmtime())}
        for x, pid in sorted(self.pids.items()):
            b = self.read(pid)
            row[x] = round(b / GB, 2) if b is not None else None
        self.rows.append(row)
        return row

    def _run(self):
        while not self.stop_ev.is_set():
            self.sample()
            self.stop_ev.wait(self.every)

    def summary(self):
        out = {"every_s": self.every, "samples": len(self.rows), "windows": sys.platform == "win32", "games": {}}
        for x in sorted({k for r in self.rows for k in r if k not in ("s", "utc")}):
            got = [(r["s"], r[x]) for r in self.rows if r.get(x) is not None]
            if not got:
                out["games"][x] = None
                continue
            peak = max(got, key=lambda t: t[1])
            out["games"][x] = {"first_gb": got[0][1], "first_s": got[0][0], "peak_gb": peak[1], "peak_s": peak[0],
                               "last_gb": got[-1][1], "last_s": got[-1][0]}
        return out

    def stop(self):
        """Stops the thread, takes one last sample, writes commit.csv + commit.json; returns the summary."""
        self.stop_ev.set()
        if self.thread.is_alive():
            self.thread.join(self.every + 5)
        self.sample()
        games = sorted({k for r in self.rows for k in r if k not in ("s", "utc")})
        try:
            with open(os.path.join(self.out, "commit.csv"), "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                w.writerow(["utc", "s_after_boot_start"] + ["%s_commit_gb" % x for x in games])
                for r in self.rows:
                    w.writerow([r["utc"], r["s"]] + ["" if r.get(x) is None else r[x] for x in games])
            s = self.summary()
            with open(os.path.join(self.out, "commit.json"), "w", encoding="utf-8") as f:
                json.dump(s, f, indent=1)
            return s
        except OSError:
            return self.summary()
