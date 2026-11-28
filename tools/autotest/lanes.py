"""Two test lanes on one PC: two game pairs that run at the same time without touching each other.

    python -I tools/autotest/lanes.py info    [--lane N] [--json]
    python -I tools/autotest/lanes.py setup   --lane N --from-a <game folder> --from-b <game folder> [--dry-run]
    python -I tools/autotest/lanes.py nocs    --lane N [--tree <module commit checkout>] [--dry-run]
    python -I tools/autotest/lanes.py check   [--lane N]
    python -I tools/autotest/lanes.py install --lane N --dll <hookshot_coop.dll> [--dry-run]
    python -I tools/autotest/lanes.py restore --lane N   (the lane's own module back after a live.py --dll session)
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
import hashlib
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
NOCS = "-nocs"                         # lane<N>-B-nocs: lane B's game without Character Select (nocs below)
CS_MOD = "OoT Character Select"        # (puppet_pack/build.py CHARACTERS_MOD, the mod's folder name in mods/)


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
            "b_nocs": os.path.join(kit, "ls", "lane%d-B%s" % (n, NOCS)),
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


def fill(src, dst, dry=False, say=print, leave=None):
    """dst gets src's game: shared files hard-linked, the rest copied (links in src, as romfs junctions, followed:
    a lane holds its own copy or a hard link, never a junction into another folder that a game could write through).
    leave(rel path) -> True: that folder or file of src is not taken (nocs: Character Select, the pack, lane-server.txt)."""
    if not hs.find_exe(src):
        raise Fail("no game exe in %s" % src)
    if os.path.normcase(os.path.realpath(src)) == os.path.normcase(os.path.realpath(dst)):
        raise Fail("%s is the source itself" % dst)
    counts = {"link": 0, "copy": 0}
    for top, dirs, files in os.walk(src, followlinks=True):
        rel_top = os.path.relpath(top, src)
        if leave:
            dirs[:] = [d for d in dirs if not leave(d if rel_top == "." else os.path.join(rel_top, d))]
        for f in files:
            rel = f if rel_top == "." else os.path.join(rel_top, f)
            if rel_top == "." and (f.lower() in SKIP or WARMING.search(f)) or (leave and leave(rel)):
                continue
            put(os.path.join(top, f), os.path.join(dst, rel), shared(rel), dry, counts)
    say("%s: %d hard link(s), %d copied%s" % (dst, counts["link"], counts["copy"], " (dry run)" if dry else ""))
    return counts


def running_in(folder):
    """Game processes started from inside folder (Windows only; elsewhere none known)."""
    if not hs.WINDOWS:
        return []
    r = hs.run_quiet(["powershell", "-NoProfile", "-Command",
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
        for folder in (L["a"], L["b"]):   # (the source's module is the lane's own now)
            exe = hs.find_exe(folder)
            if exe:
                drop_swap(os.path.join(os.path.dirname(exe), MODULE))
    return L


# --- nocs: lane B's game without Character Select (on demand) ----------------------------------------------------
# For the tests that compare a game with the Character Select mod (lane A) and one without (gear-link, vest-link,
# leather-link, hair-link: tools/autotest/ingame/no_cs.py, `--env B:COOP_TEST_NO_CHARACTER_SELECT=1`). Made only when
# asked (`lanes.py nocs --lane N`), never by a lane prep, and booted only by a run with that switch (bt_plugin
# boot_args: --game-b). His code is compiled into longshot.exe (hs.py code_mods_line), so the folder needs its own
# game: the hs cache entry built with co-op's own Characters tab patch instead of his (kit-paths.ps1
# Sync-KitCharselNativePatch's rule), and a co-op pack made without him (puppet_pack build.py charsel_native_wanted:
# then the pack carries the native tab and its code patch). Everything else is lane B's: hard links for what no game
# writes (fill), copies of the rest. Its own lane-server.txt (written LAST: a half-made folder never passes
# pair.lane_check) names lane B's module, commit and server, plus nocs_of=lane<N>-B.
SERVER_TXT = "lane-server.txt"
PACK_TOOL_ARGS = ("--title-rows", "--pause-tab", "--puppet-body", "--puppet-liftable=auto")   # (kit-paths.ps1 Get-KitPackArgs: the kit's pack command)


def read_fields(path):
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        return [ln.rstrip("\r\n") for ln in f]


def stamp_head(text):
    """The release=, commit= and patches= lines of a stamp (coop-built-from.txt): which game build, without the
    mods' code patches (exefs=)."""
    return [ln.strip() for ln in text.replace("\r", "").split("\n") if ln.strip().startswith(("release=", "commit=",
                                                                                                  "patches="))]


def module_tree(commit, repo=REPO):
    """The checkout of the lane's module commit (lane-prep's per-sha wt-module-<sha8> beside the repo)."""
    return os.path.join(os.path.dirname(os.path.abspath(repo)), "wt-module-%s" % commit[:8])


def git_head(tree, run=hs.run_quiet):
    r = run(["git", "-C", tree, "rev-parse", "HEAD"], capture_output=True, text=True)
    return (r.stdout or "").strip() if not r.returncode else ""


def make_pack(tree, romfs, pack, run=hs.run_quiet):
    """The kit's pack command (Get-KitPackArgs) with the module commit's own pack tool, into `pack`."""
    tool = os.path.join(tree, "tools", "puppet_pack", "build.py")
    if not os.path.isfile(tool):
        raise Fail("no %s" % tool)
    r = run([sys.executable, "-I", "-B", tool, "--romfs", romfs, "--mod-dir", pack] + list(PACK_TOOL_ARGS),
            capture_output=True, text=True, errors="replace")
    if r.returncode:
        raise Fail("pack build failed (exit %s): %s" % (r.returncode, ((r.stdout or "") + (r.stderr or "")).strip()[-400:]))
    return r


def drop_cs_rows(path):
    """coop-mods.list without the Character Select rows (his folder, and a copy kept aside as '_<name>.prev')."""
    if not os.path.isfile(path):
        return 0
    rows = read_fields(path)
    keep = [r for r in rows if r.partition(" ")[2].strip().lstrip("_").lower() not in
            (CS_MOD.lower(), (CS_MOD + ".prev").lower())]
    if len(keep) != len(rows):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(keep) + "\n")
    return len(rows) - len(keep)


def nocs(n, tree=None, dry=False, say=print, kit=None, busy=running_in, pack=make_pack, restore=hs.cache_restore,
         head=git_head, now=None, mods_line=hs.code_mods_line, mismatch=hs.code_mods_mismatch):
    """Makes lane<N>-B-nocs from lane<N>-B (see above). tree: the module commit's checkout (default wt-module-<sha8>).
    Returns the folder."""
    L = lane(n, kit)
    src, dst = L["b"], L["b_nocs"]
    for f in () if dry else (src, dst):   # (a dry run only reads: fine beside a lane run)
        if busy(f):
            raise Fail("lane %d: a game is running from %s; close it first" % (n, f))
    try:
        srv = read_fields(os.path.join(src, SERVER_TXT))
    except OSError:
        raise Fail("no %s in %s (install lane %d first: lane-game-for.ps1)" % (SERVER_TXT, src, n))
    keys = dict(ln.split("=", 1) for ln in srv if "=" in ln and not ln.startswith("#"))
    commit = keys.get("commit", "")
    if not re.fullmatch(r"[0-9a-f]{7,40}", commit) or not re.fullmatch(r"[0-9A-Fa-f]{64}", keys.get("dll_sha256", "")):
        raise Fail("%s has no commit= / dll_sha256= line" % os.path.join(src, SERVER_TXT))
    tree = os.path.abspath(tree or module_tree(commit))
    got = head(tree)
    if not got.startswith(commit) and not commit.startswith(got or "-"):
        raise Fail("%s is at %s, not lane %d's module commit %s (--tree the commit's checkout)"
                   % (tree, got or "(no git checkout)", n, commit[:12]))
    with open(os.path.join(src, "coop-built-from.txt"), encoding="utf-8-sig") as f:
        built = f.read()
    base = hs.stamp(tree)
    if stamp_head(base) != stamp_head(built):
        raise Fail("lane %d B's game is not the build of %s's patches (rebuilt since? run a lane prep first)"
                   % (n, commit[:8]))
    leave_rel = {os.path.normcase(os.path.join("mods", CS_MOD)), os.path.normcase(os.path.join("mods", "_" + CS_MOD + ".prev")),
                 os.path.normcase(hs.PACK_DIR), os.path.normcase(SERVER_TXT)}   # (lane B's own: never taken)
    tp, ts = _test_state()
    leave_rel |= {os.path.normcase(x) for x in (tp.MANIFEST, tp.KEPT, ts.MANIFEST, ts.KEPT)}
    if tests_left(src):
        raise Fail("%s has a test run's %s still in (its pack or saves are not lane B's own): lanes.py install, or a "
                   "bridge boot, undoes it first" % (src, ", ".join(tests_left(src))))
    if not dry and os.path.isdir(dst):
        undo_tests(dst, say)
    leave = lambda rel: os.path.normcase(rel) in leave_rel
    say("nocs: %s -> %s (module %s, tools %s)%s" % (src, dst, commit[:8], tree, " (dry run)" if dry else ""))
    if not dry:
        for p in [os.path.join(dst, SERVER_TXT)] + [os.path.join(dst, r) for r in leave_rel]:
            if os.path.isdir(p):
                shutil.rmtree(p)
            elif os.path.lexists(p):
                os.unlink(p)   # (lane-server.txt first: a half-made folder never passes lane_check)
    fill(src, dst, dry, say, leave=leave)
    mods, pack_dir = os.path.join(dst, "mods"), os.path.join(dst, hs.PACK_DIR)
    if dry:
        say("nocs: would drop Character Select's coop-mods.list rows, make the pack into %s from %s, restore the game"
            " for the native-tab key" % (pack_dir, os.path.join(src, "romfs")))
        return dst
    say("nocs: coop-mods.list: %d Character Select row(s) dropped" % drop_cs_rows(os.path.join(mods, "coop-mods.list")))
    say("nocs: making the pack (no Character Select: co-op's own Characters tab)")
    pack(tree, os.path.join(src, "romfs"), pack_dir)
    line = mods_line(mods, tree)
    if CS_MOD.lower() in line.lower() or "coop-puppet/exefs/" not in line.replace("\\", "/"):
        raise Fail("the no-CS pack's code patch line is not co-op's own tab's: %r" % line.strip()[:200])
    want = base + line
    if not restore(dst, repo=tree, say=say, want=want):
        raise Fail("no game for the no-CS key %s in the hs cache (%s): build it once (hs.py build with these mods)"
                   % (hs.key_of(want), hs.cache_root()))
    bad = mismatch(dst, tree)
    if bad:
        raise Fail(bad)
    lst = os.path.join(dst, hs.EXEFS_LIST)
    if os.path.isfile(lst) and any(r.lower().startswith(CS_MOD.lower() + "\t") for r in read_fields(lst)):
        raise Fail("%s still lists Character Select's code patch after the restore" % lst)
    dll = os.path.join(dst, MODULE)
    if hs.sha256_file(dll).upper() != keys["dll_sha256"].upper():
        raise Fail("%s is not lane B's module %s" % (dll, keys["dll_sha256"][:16]))
    drop_swap(dll)
    patches = next((ln for ln in read_fields(os.path.join(dst, "coop-built-from.txt")) if ln.startswith("patches=")), "")
    exefs = next((ln for ln in read_fields(os.path.join(dst, "coop-built-from.txt")) if ln.startswith("exefs=")), "")
    out = ["# written by lanes.py nocs %s UTC from %s" % (time.strftime("%Y-%m-%d %H:%M", time.gmtime(now)), src),
           "by=lanes.py-nocs", "nocs_of=%s" % os.path.basename(src)]
    out += ["%s=%s" % (k, keys[k]) for k in ("commit", "protocol", "server", "dll_sha256") if k in keys]
    out += ["patches_line_sha256=%s" % hashlib.sha256(patches.encode("utf-8")).hexdigest().upper(),
            "exefs_sha256=%s" % hashlib.sha256(exefs.encode("utf-8")).hexdigest().upper(),
            "pack_tool_sha256=%s" % pack_tool_hash(tree)]
    tmp = os.path.join(dst, SERVER_TXT + ".part")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    os.replace(tmp, os.path.join(dst, SERVER_TXT))
    say("nocs: %s ready (game %s, %s)" % (dst, hs.key_of(want), exefs[:90]))
    return dst


def pack_tool_hash(tree):
    """The pack tool's own hash (puppet_pack build.py tool_hash, the coop_puppet.json 'tool_sha256'), '' if unknown."""
    folder = os.path.join(tree, "tools", "puppet_pack")
    if not os.path.isdir(folder):
        return ""
    names = sorted((x for x in os.listdir(folder) if x.lower().endswith(".py") and os.path.isfile(os.path.join(folder, x))),
                   key=lambda x: x.encode("utf-8"))
    text = ""
    for x in names:
        with open(os.path.join(folder, x), "rb") as f:
            text += "%s:%s\n" % (x, hashlib.sha256(f.read()).hexdigest())
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


# --- check -------------------------------------------------------------------------------------------------------

def problems(n, kit=None, now=None, busy=running_in):
    """What is wrong with lane n's pair ([] when it is ready)."""
    L = lane(n, kit)
    out = []
    for folder in (L["a"], L["b"]):
        if not hs.find_exe(folder):
            out.append("%s: no game (lanes.py setup)" % folder)
            continue
        try:
            mismatch = hs.code_mods_mismatch(os.path.dirname(hs.find_exe(folder)))
        except hs.Fail as e:
            mismatch = str(e)
        if mismatch:
            out.append(mismatch)
        for m in (LIVE_BACKUP, LIVE_ABSENT):
            if os.path.exists(os.path.join(os.path.dirname(hs.find_exe(folder)), MODULE + m)) and not busy(folder):
                out.append("%s: a live session's module was not put back (%s%s, no game running): lanes.py restore "
                           "--lane %d" % (folder, MODULE, m, n))
        for m in tests_left(folder):
            if not busy(folder):
                out.append("%s: a test run's %s is still in (no game running): lanes.py install undoes it, or the next"
                           " bridge boot" % (folder, m))
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


# --- a test's pack files or save profile left in a lane (bt_plugin --pack / --save of a run that died) ------------

def _test_state():
    sys.path.insert(0, os.path.join(HERE, "ingame"))
    import testpack
    import testsave
    return testpack, testsave


def tests_left(folder):
    """The manifests a bt_plugin run left in a game folder (a pack or a save profile still in)."""
    tp, ts = _test_state()
    return [m for m in (tp.MANIFEST, ts.MANIFEST) if os.path.exists(os.path.join(folder, m))]


def undo_tests(folder, say=print):
    """A left test pack and save profile out of a game folder (testpack.undo / testsave.undo: the lane as it was)."""
    tp, ts = _test_state()
    try:
        return tp.undo(folder, say=say) + ts.undo(folder, say=say)
    except (tp.PackError, ts.SaveProfileError) as e:
        raise Fail(str(e))


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
            undo_tests(folder, say)   # (review-c123-9 11:36 nit c: a lane prep never leaves a test's pack or save in)
            _put(dll, dst)
            drop_swap(dst)   # this module is the lane's own now: a session's leftover backup must never come back


# --- a module for one session (live.py start --dll), and the lane's own back -------------------------------------
# This file is the one owner of what module a lane runs. `install` and `setup` give the lane its own module (and drop
# any session swap left beside it); `swap_in` puts a test module in for one session with the lane's own kept beside
# it (MODULE + LIVE_BACKUP, or an empty MODULE + LIVE_ABSENT when the lane had none); `swap_back` puts it back.

MODULE = "hookshot_coop.dll"
LIVE_BACKUP = ".live-backup"   # the lane's own module while a session runs another
LIVE_ABSENT = ".live-absent"   # the lane had no module: the session's is removed at its end


def module_paths(n, kit=None):
    """Where each game of lane n loads its module from."""
    L = lane(n, kit)
    out = []
    for folder in (L["a"], L["b"]):
        exe = hs.find_exe(folder)
        if not exe:
            raise Fail("%s: no game (lanes.py setup)" % folder)
        out.append(os.path.join(os.path.dirname(exe), MODULE))
    return out


def _put(dll, dst):
    if os.path.lexists(dst):
        os.unlink(dst)   # never write into a file that may be a hard link
    shutil.copy2(dll, dst)


def drop_swap(dst):
    """A plain install or setup: what is there now is the lane's own; a session's leftover markers go."""
    for m in (dst + LIVE_BACKUP, dst + LIVE_ABSENT):
        if os.path.lexists(m):
            os.unlink(m)


def swap_in(n, dll, kit=None, busy=running_in):
    """`dll` into both games of lane n for one session, the lane's own kept beside it. A backup or marker left by a
    session that could not put it back is still the lane's own (install and setup drop them), so it is kept."""
    if not os.path.isfile(dll):
        raise Fail("no %s" % dll)
    L = lane(n, kit)
    for folder in (L["a"], L["b"]):
        if busy(folder):
            raise Fail("%s: its game is running; close it first" % folder)
    paths = module_paths(n, kit)
    for dst in paths:
        if not (os.path.exists(dst + LIVE_BACKUP) or os.path.exists(dst + LIVE_ABSENT)):
            if os.path.isfile(dst):
                shutil.copy2(dst, dst + LIVE_BACKUP)
            else:
                open(dst + LIVE_ABSENT, "w").close()
        _put(dll, dst)
    return paths


def swap_back(n, kit=None, tries=10, wait=0.5, sleep=time.sleep):
    """The lane's own module back after a session (a killed game can hold the file a moment on Windows: tried again
    for tries x wait s). One word per game: restored, removed (the lane had none), nothing to put back, or NOT ..."""
    out = []
    for dst in module_paths(n, kit):
        bak, absent = dst + LIVE_BACKUP, dst + LIVE_ABSENT
        word = "nothing to put back"
        for i in range(tries):
            try:
                if os.path.exists(bak):
                    if os.path.lexists(dst):
                        os.unlink(dst)
                    os.replace(bak, dst)
                    if os.path.lexists(absent):
                        os.unlink(absent)
                    word = "restored"
                elif os.path.exists(absent):
                    if os.path.lexists(dst):
                        os.unlink(dst)
                    os.unlink(absent)
                    word = "removed (the lane had none)"
                break
            except OSError as e:
                word = "NOT restored (%s): lanes.py restore --lane %d" % (e, n)
                if i + 1 < tries:
                    sleep(wait)
        out.append(word)
    return out


def swap_left(n, kit=None):
    """The session swaps still waiting to be put back in lane n (paths of their markers)."""
    out = []
    for dst in module_paths(n, kit):
        out += [m for m in (dst + LIVE_BACKUP, dst + LIVE_ABSENT) if os.path.exists(m)]
    return out


# --- orphan games: a lane game up while no lock of its lane is live ---------------------------------------------
# C #133 2026-11-26 (STOP-OFFLINE-QUEUE 11:12): party-hud-bridge-fa63e90f's SETUP-FAIL at 11:05 left both lane-2 games
# running (47 GB commit) with lane2.lock gone and their parent dead: the next job saw a free lane. Every lane job start
# (lane-prep, the queue) runs `lanes.py orphans --lane N --close` first: each game process started from the lane's
# folders while neither laneN.lock nor laneN-<X>.lock is live is asked to close (its main window, as a player would),
# then killed if still up. Never while a game build holds build-game.lock (hs.py may run a lane's game to warm it).

def game_procs(folder):
    """[(pid, exe path)] of the game processes started from inside folder (Windows only; elsewhere none known)."""
    if not hs.WINDOWS:
        return []
    r = hs.run_quiet(["powershell", "-NoProfile", "-Command",
                      "Get-Process %s -ErrorAction SilentlyContinue | ForEach-Object { '{0} {1}' -f $_.Id, $_.Path }"
                      % ",".join(GAME_EXES)], capture_output=True, text=True)
    want = os.path.normcase(os.path.abspath(folder)) + os.sep
    out = []
    for ln in (r.stdout or "").splitlines():
        pid, _, path = ln.strip().partition(" ")
        if pid.isdigit() and path and os.path.normcase(path.strip()).startswith(want):
            out.append((int(pid), path.strip()))
    return out


def live_lock(root, name, alive=hs.pid_alive, now=None):
    """The owner of a live lock named name ('' when it is absent or stale). An owner pid of this host that is alive
    = live at ANY age (C #133 / review-c123-9 11:29: bt_plugin's default session is 240 min; hs.lock_stale's 4 h
    rule would call a longer run's lock stale and its games orphans); otherwise hs.lock_stale decides."""
    p = os.path.join(root, hs.LOCK_NAMES[name])
    if not os.path.isdir(p):
        return ""
    info = hs.lock_info(p)
    if info.get("pid") and info.get("host") in (hs.host_id(), hs.socket.gethostname()) and alive(int(info["pid"])):
        return info.get("owner", "?")
    if not hs.lock_stale(p, now=now, alive=alive):
        return info.get("owner", "?")
    return ""


def orphans(n, kit=None, root=None, procs=game_procs, alive=hs.pid_alive, now=None):
    """(orphan games [{'game', 'pid', 'path'}], why none may be closed now or ''). A game is an orphan when neither the
    pair's lock nor its own game lock is live."""
    L = lane(n, kit)
    root = root or hs.main_root(REPO)
    build = live_lock(root, "game", alive, now)
    found = []
    for x in ("A", "B"):
        held = live_lock(root, lock_name(n), alive, now) or live_lock(root, lock_name(n, x), alive, now)
        if held:
            continue
        for folder in [L[x.lower()]] + ([L["b_nocs"]] if x == "B" else []):   # (B-nocs boots as the lane's B)
            for pid, path in procs(folder):
                found.append({"game": x, "pid": pid, "path": path})
    return found, ("build-game.lock is held by %s (a game build may run a lane's game)" % build) if build else ""


def close_window(pid):
    """Asks a game to close as a player would (its main window: the crash watch stays quiet; memory close-games-gently)."""
    hs.run_quiet(["powershell", "-NoProfile", "-Command",
                  "$p = Get-Process -Id %d -ErrorAction SilentlyContinue; if ($p) { [void]$p.CloseMainWindow() }" % pid],
                 capture_output=True, text=True)


def close_orphans(found, wait=30.0, closer=close_window, killer=None, alive=hs.pid_alive, clock=time.monotonic,
                  sleep=time.sleep):
    """Each orphan asked to close, killed if still up after wait s. {pid: 'closed' | 'killed' | 'still up'}."""
    import signal
    killer = killer or (lambda pid: os.kill(pid, signal.SIGTERM))
    out = {}
    for o in found:
        try:
            closer(o["pid"])
        except Exception:
            pass
    end = clock() + wait
    while clock() < end and any(alive(o["pid"]) for o in found):
        sleep(0.5)
    for o in found:
        if not alive(o["pid"]):
            out[o["pid"]] = "closed"
            continue
        try:
            killer(o["pid"])
        except OSError:
            pass
        sleep(1)
        out[o["pid"]] = "killed" if not alive(o["pid"]) else "still up"
    return out


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


def lock_name(n, game=None):
    """laneN (the pair) or laneN-A / laneN-B (one game of it, a lane of its own)."""
    return "lane%d" % n if not game else "lane%d-%s" % (n, game)


def overlaps(root, n, game=None, alive=hs.pid_alive):
    """The live locks that share a game with lock_name(n, game): a pair and its games exclude each other, A and B
    do not."""
    out = []
    for name in ([lock_name(n)] if game else [lock_name(n, x) for x in "AB"]):
        p = os.path.join(root, hs.LOCK_NAMES[name])
        if os.path.isdir(p) and not hs.lock_stale(p, alive=alive):
            out.append("%s (%s)" % (hs.LOCK_NAMES[name], hs.lock_info(p).get("owner", "?")))
    return out


def lock_acquire(n, owner, wait=0.0, root=None, say=print, sleep=time.sleep, clock=time.time, pid=0, check=None,
                 game=None):
    """Takes laneN.lock (laneN-A/B.lock for one game) FIRST, then looks for a game build and for a lock that shares a
    game; blocked: lets it go again and retries. A game build takes build-game.lock first, then looks for lane locks
    (hs.build), so of the two one always sees the other; the same holds for a pair and one of its games."""
    root = root or hs.main_root(REPO)
    check = check or (lambda r: blockers(r) + overlaps(r, n, game))
    name = lock_name(n, game)
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
            say("lane %d: not free: %s" % (n, "; ".join(b) if b else "%s.lock is held" % name))
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
    nc = sub.add_parser("nocs", help="make lane<N>-B-nocs: lane B's game without Character Select (on demand; booted "
                        "by a run with --env B:COOP_TEST_NO_CHARACTER_SELECT=1)")
    nc.add_argument("--lane", type=int, required=True)
    nc.add_argument("--tree", help="the lane module commit's checkout (default: wt-module-<sha8> beside the repo)")
    nc.add_argument("--dry-run", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("--lane", type=int)
    rs = sub.add_parser("restore", help="put the lane's own module back after a live session that could not")
    rs.add_argument("--lane", type=int, required=True)
    n = sub.add_parser("install")
    n.add_argument("--lane", type=int, required=True)
    n.add_argument("--dll", required=True)
    n.add_argument("--dry-run", action="store_true")
    o = sub.add_parser("orphans", help="lane games running while no lock of the lane is live (a job that died); "
                       "--close: close them (main window, then kill). Exit 0 when none is left up")
    o.add_argument("--lane", type=int, required=True)
    o.add_argument("--close", action="store_true")
    o.add_argument("--wait", type=float, default=30.0, help="s to wait for a closed window before the kill")
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
        if a.cmd == "nocs":
            nocs(a.lane, a.tree, a.dry_run)
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
        if a.cmd == "restore":
            for p, w in zip(module_paths(a.lane), swap_back(a.lane)):
                print("%s: %s" % (p, w))
            return 0
        if a.cmd == "orphans":
            found, hold = orphans(a.lane)
            res = {"lane": a.lane, "orphans": found, "closed": {}, "not_closed": hold}
            if found and a.close and not hold:
                res["closed"] = {str(k): v for k, v in close_orphans(found, wait=a.wait).items()}
                res["swap_left"] = swap_left(a.lane)   # (a live session's module swap: lanes.py restore --lane N)
                if res["swap_left"]:
                    res["restored"] = swap_back(a.lane)
            print(json.dumps(res))
            up = [o for o in found if res["closed"].get(str(o["pid"])) not in ("closed", "killed")]
            return 1 if up else 0
        root = hs.main_root(REPO)
        if a.action == "status":
            b = blockers(root)
            print("blocking every lane: %s" % ("; ".join(b) if b else "nothing"))
            for x in which:
                for g in (None, "A", "B"):
                    p = os.path.join(root, hs.LOCK_NAMES[lock_name(x, g)])
                    info = hs.lock_info(p) if os.path.isdir(p) else {}
                    if g and not info:
                        continue   # (a free game of a lane: the pair's line says it)
                    print("lane %d%s: %s" % (x, "-" + g if g else "", "held by %s since %s" % (
                        info.get("owner", "?"), hs.fmt_time(info.get("time"))) if os.path.isdir(p) else "free"))
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
