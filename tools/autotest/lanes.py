"""Two test lanes on one PC: two game pairs that run at the same time without touching each other.

    python -I tools/autotest/lanes.py info    [--lane N] [--json]
    python -I tools/autotest/lanes.py setup   --lane N --from-a <game folder> --from-b <game folder> [--dry-run]
    python -I tools/autotest/lanes.py check   [--lane N]
    python -I tools/autotest/lanes.py install --lane N --dll <hookshot_coop.dll> [--dry-run]
    python -I tools/autotest/lanes.py lock    status|acquire|release --lane N [--owner WHO] [--wait S]

Lane N has its own game pair (<kit root>\\ls\\laneN-A and -B), server port (27510 + 10*N, with +1 and +2 for
run.ps1's bad-connection relays; 27510 itself is the owner's own server and is never used), control pipes
(coop-laneN-A/-B), run folders (<kit root>\\ls\\shots-laneN) and run lock (laneN.lock in the main checkout's root).
run.ps1 -Lane N, hookshot.py run --lane N and groups.py --lane N fill all of those in.

What blocks a lane: a game BUILD (build-game.lock, taken by hs.py build on a cache miss; or the old build.lock, or a
build-hs.lock made by hand, whose owner.txt is not hs.py's), and the lane's own lock. A module build (hs.py
compile-all, or build on a cache hit) and `install` (a hookshot_coop.dll copied into a lane, seconds) block nothing.

`setup` fills a lane's pair from a built game folder (for example the warmed hookshot-A1 / -B1). Only files no game
ever writes are hard links (the game exe and its libraries, image.bin, the shipped pipeline states, romfs\\ and
.longshot_exefs\\); every cache, save, option and log is a copy of its own, so two lanes never write one file
(reports/b-instance-b.md: a shared marker made one game prepare every shader at each start). `check` proves that.
Nothing here starts a game.
"""
import argparse
import json
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "hookshot"))
import hs  # noqa: E402

LANES = (1, 2)
OWNER_PORT = 27510   # the owner's own coop_server: never a lane's
PORT_BASE, PORT_STEP, PORTS_PER_LANE = 27510, 10, 3
# Never written by a running game: safe to share between lanes as hard links. Everything else is copied.
SHARED_FILE = re.compile(r"^(longshot|hookshot)(\.exe)?$|^image\.bin$|^(longshot|hookshot)_pipeline_states_shipped\.bin$"
                         r"|\.dll$", re.I)
SHARED_DIR = {"romfs", ".longshot_exefs", ".hookshot_exefs"}
NEVER_SHARED = {"hookshot_coop.dll"}   # replaced by `install` (a copy over a hard link would change the other lane)
# Per run: never taken from the source folder (the lane's game makes its own).
SKIP = {"longshot_log.txt", "longshot_log.old.txt", "hookshot_log.txt", "control.json"}
WARMING = re.compile(r"\.warming$", re.I)
GAME_EXES = ("longshot", "hookshot")


class Fail(Exception):
    pass


def kit_root():
    d = REPO
    while True:
        c = os.path.join(d, "coop-kit")
        if os.path.isdir(c):
            return c
        up = os.path.dirname(d)
        if up == d:   # none yet: where a kit root beside the repo would be (setup makes it)
            return os.path.join(os.path.dirname(REPO), "coop-kit")
        d = up


def lane(n, kit=None):
    """Everything lane n owns."""
    if n not in LANES:
        raise Fail("lane %s: lanes are %s" % (n, ", ".join(map(str, LANES))))
    kit = kit or kit_root()
    port = PORT_BASE + PORT_STEP * n
    return {"lane": n, "a": os.path.join(kit, "ls", "lane%d-A" % n), "b": os.path.join(kit, "ls", "lane%d-B" % n),
            "port": port, "ports": list(range(port, port + PORTS_PER_LANE)), "pipe": "coop-lane%d" % n,
            "shots": os.path.join(kit, "ls", "shots-lane%d" % n), "lock": "lane%d" % n}


def check_distinct(lanes, groups_ports=range(27560, 27560 + 10 * 8)):
    """No two lanes share a folder, port or pipe; no lane uses the owner's port or a groups.py port."""
    seen = {}
    for L in lanes:
        for k in [("dir", os.path.normcase(L["a"])), ("dir", os.path.normcase(L["b"])), ("dir", os.path.normcase(L["shots"])),
                  ("pipe", L["pipe"])] + [("port", p) for p in L["ports"]]:
            if k in seen:
                raise Fail("lanes %d and %d share %s %s" % (seen[k], L["lane"], k[0], k[1]))
            seen[k] = L["lane"]
        bad = [p for p in L["ports"] if p == OWNER_PORT or p in groups_ports]
        if bad:
            raise Fail("lane %d uses port %s (the owner's server or groups.py)" % (L["lane"], bad))


# --- setup -------------------------------------------------------------------------------------------------------

def shared(rel):
    """Is this path (relative to a game folder) one no game writes?"""
    parts = rel.replace("\\", "/").split("/")
    if parts[0].lower() in SHARED_DIR:
        return True
    return len(parts) == 1 and SHARED_FILE.search(parts[0]) is not None and parts[0].lower() not in NEVER_SHARED


def same_file(a, b):
    try:
        return os.path.samefile(a, b)
    except OSError:
        return False


def put(src, dst, link, dry, counts):
    """dst becomes a hard link to src (link) or its own copy. An old dst is unlinked first, never written into: it may
    be a hard link shared with another folder."""
    if link and same_file(src, dst):
        return
    if not link and os.path.isfile(dst) and not same_file(src, dst):
        a, b = os.stat(src), os.stat(dst)
        if a.st_size == b.st_size and int(a.st_mtime) == int(b.st_mtime) and b.st_nlink == 1:
            return
    counts["link" if link else "copy"] += 1
    if dry:
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.unlink(dst)
    if link:
        os.link(src, dst)
    else:
        shutil.copy2(src, dst)


def fill(src, dst, dry=False, say=print):
    """dst gets src's game: shared files hard-linked, the rest copied (links in src, as romfs junctions, followed:
    a lane holds its own copy or a hard link, never a junction into another folder that a game could write through)."""
    if not hs.find_exe(src):
        raise Fail("no game exe in %s" % src)
    if os.path.normcase(os.path.realpath(src)) == os.path.normcase(os.path.realpath(dst)):
        raise Fail("%s is the source itself" % dst)
    counts = {"link": 0, "copy": 0}
    for top, dirs, files in os.walk(src, followlinks=True):
        rel_top = os.path.relpath(top, src)
        for f in files:
            rel = f if rel_top == "." else os.path.join(rel_top, f)
            if rel_top == "." and (f.lower() in SKIP or WARMING.search(f)):
                continue
            put(os.path.join(top, f), os.path.join(dst, rel), shared(rel), dry, counts)
    say("%s: %d hard link(s), %d copied%s" % (dst, counts["link"], counts["copy"], " (dry run)" if dry else ""))
    return counts


def running_in(folder):
    """Game processes started from inside folder (Windows only; elsewhere none known)."""
    if not hs.WINDOWS:
        return []
    import subprocess
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        "Get-Process %s -ErrorAction SilentlyContinue | ForEach-Object { $_.Path }" % ",".join(GAME_EXES)],
                       capture_output=True, text=True)
    want = os.path.normcase(os.path.abspath(folder)) + os.sep
    return [p for p in r.stdout.splitlines() if os.path.normcase(p.strip()).startswith(want)]


def setup(n, from_a, from_b, dry=False, say=print, kit=None, busy=running_in):
    L = lane(n, kit)
    for side, src, dst in (("A", from_a, L["a"]), ("B", from_b, L["b"])):
        if busy(dst):
            raise Fail("lane %d %s: a game is running from %s; close it first" % (n, side, dst))
        fill(os.path.abspath(src), dst, dry, say)
    if not dry:
        os.makedirs(L["shots"], exist_ok=True)
    return L


# --- check -------------------------------------------------------------------------------------------------------

def problems(n, kit=None, now=None, busy=running_in):
    """What is wrong with lane n's pair ([] when it is ready)."""
    L = lane(n, kit)
    out = []
    for folder in (L["a"], L["b"]):
        if not hs.find_exe(folder):
            out.append("%s: no game (lanes.py setup)" % folder)
            continue
        if not os.path.isfile(os.path.join(hs.warm_dir(folder), "longshot_pipelines.bin.built")):
            out.append("%s: not warmed (no longshot_pipelines.bin.built: the first start prepares every shader)" % folder)
        for top, dirs, files in os.walk(folder):
            for d in list(dirs):
                if hs.is_link(os.path.join(top, d)):
                    out.append("%s: a link into another folder (a game could write through it)" % os.path.join(top, d))
                    dirs.remove(d)
            for f in files:
                p = os.path.join(top, f)
                rel = os.path.relpath(p, folder)
                if WARMING.search(f) and not busy(folder):
                    out.append("%s: stale shader-preparation marker (no game running): remove it" % p)
                try:
                    if os.stat(p).st_nlink > 1 and not shared(rel):
                        out.append("%s: hard-linked but the game writes it" % p)
                except OSError:
                    pass
    return out


# --- install (module only) ---------------------------------------------------------------------------------------

def install(n, dll, dry=False, say=print, kit=None, busy=running_in):
    """hookshot_coop.dll into both games of lane n: seconds, no build lock (the game build stays as it is)."""
    L = lane(n, kit)
    if not os.path.isfile(dll):
        raise Fail("no %s" % dll)
    for folder in (L["a"], L["b"]):
        exe = hs.find_exe(folder)
        if not exe:
            raise Fail("%s: no game (lanes.py setup)" % folder)
        if busy(folder):
            raise Fail("%s: its game is running; close it first" % folder)
        dst = os.path.join(os.path.dirname(exe), "hookshot_coop.dll")
        say("copy %s -> %s" % (dll, dst))
        if not dry:
            if os.path.lexists(dst):
                os.unlink(dst)   # never write into a file that may be a hard link
            shutil.copy2(dll, dst)


# --- the lane's run lock -----------------------------------------------------------------------------------------

def blockers(root, alive=hs.pid_alive, now=None):
    """What stops any lane from starting a game now: a game build, the old whole-PC lock, a hand-made build-hs.lock."""
    out = []
    for name in (hs.OLD_LOCK, hs.LOCK_NAMES["game"]):
        p = os.path.join(root, name)
        if os.path.isdir(p) and not hs.lock_stale(p, now=now, alive=alive):
            out.append("%s (%s)" % (name, hs.lock_info(p).get("owner", "?")))
    p = os.path.join(root, hs.LOCK_NAMES["hs"])
    if os.path.isdir(p) and not hs.lock_info(p):   # not hs.py's JSON: someone's whole-PC game run, old style
        try:
            with open(os.path.join(p, "owner.txt"), encoding="utf-8", errors="replace") as f:
                who = f.read().strip()[:80]
        except OSError:
            who = "?"
        if time.time() - os.path.getmtime(p) < 4 * 3600:
            out.append("%s made by hand (%s): the old one-run-at-a-time meaning" % (hs.LOCK_NAMES["hs"], who))
    return out


def lock_acquire(n, owner, wait=0.0, root=None, say=print, sleep=time.sleep, clock=time.time, pid=0, check=None):
    """Takes laneN.lock FIRST, then looks for a game build; blocked: lets it go again and retries. A game build takes
    build-game.lock first, then looks for lane locks (hs.build), so of the two one always sees the other."""
    root = root or hs.main_root(REPO)
    check = check or blockers
    name = "lane%d" % n
    end = clock() + wait
    while True:
        b = []
        if hs.lock_acquire(root, name, owner, wait=0, say=lambda *_: None, pid=pid):
            b = check(root)
            if not b:
                say("lock: %s.lock taken by %s" % (name, owner))
                return True
            hs.lock_release(root, name, owner, say=lambda *_: None)
        if clock() >= end:
            say("lane %d: not free: %s" % (n, "; ".join(b) if b else "lane%d.lock is held" % n))
            return False
        sleep(min(10.0, max(0.1, end - clock())))


# --- command line ------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("info")
    i.add_argument("--lane", type=int)
    i.add_argument("--json", action="store_true")
    s = sub.add_parser("setup")
    s.add_argument("--lane", type=int, required=True)
    s.add_argument("--from-a", required=True)
    s.add_argument("--from-b", required=True)
    s.add_argument("--dry-run", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("--lane", type=int)
    n = sub.add_parser("install")
    n.add_argument("--lane", type=int, required=True)
    n.add_argument("--dll", required=True)
    n.add_argument("--dry-run", action="store_true")
    k = sub.add_parser("lock")
    k.add_argument("action", choices=["status", "acquire", "release"])
    k.add_argument("--lane", type=int)
    k.add_argument("--owner", default=os.environ.get("USERNAME", "someone"))
    k.add_argument("--wait", type=float, default=0)
    k.add_argument("--pid", type=int, default=0, help="acquire: the process that holds it (default: by age only)")
    a = ap.parse_args(argv)
    try:
        which = [a.lane] if getattr(a, "lane", None) else list(LANES)
        if a.cmd == "info":
            ls = [lane(x) for x in which]
            check_distinct([lane(x) for x in LANES])
            if a.json:
                print(json.dumps(ls[0] if a.lane else ls, indent=1))
            else:
                for L in ls:
                    print("lane %d: %s + %s, port %d (+1, +2), pipe %s-A/-B, runs in %s, lock %s.lock"
                          % (L["lane"], L["a"], L["b"], L["port"], L["pipe"], L["shots"], L["lock"]))
            return 0
        if a.cmd == "setup":
            setup(a.lane, a.from_a, a.from_b, a.dry_run)
            return 0
        if a.cmd == "check":
            bad = 0
            for x in which:
                ps = problems(x)
                print("lane %d: %s" % (x, "ready" if not ps else "%d problem(s)" % len(ps)))
                for p in ps:
                    print("  " + p)
                bad += len(ps)
            return 1 if bad else 0
        if a.cmd == "install":
            install(a.lane, os.path.abspath(a.dll), a.dry_run)
            return 0
        root = hs.main_root(REPO)
        if a.action == "status":
            b = blockers(root)
            print("blocking every lane: %s" % ("; ".join(b) if b else "nothing"))
            for x in which:
                p = os.path.join(root, hs.LOCK_NAMES["lane%d" % x])
                info = hs.lock_info(p) if os.path.isdir(p) else {}
                print("lane %d: %s" % (x, "held by %s since %s" % (info.get("owner", "?"), hs.fmt_time(info.get("time")))
                                       if os.path.isdir(p) else "free"))
            return 0
        if not a.lane:
            raise Fail("--lane N")
        if a.action == "acquire":
            return 0 if lock_acquire(a.lane, a.owner, a.wait, root, pid=a.pid) else 1
        return 0 if hs.lock_release(root, "lane%d" % a.lane, a.owner) else 1
    except (Fail, hs.Fail) as e:
        print("lanes.py: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
