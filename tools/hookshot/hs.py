"""The integrator's fast Hookshot loop: build cache, split build locks, compile-all.

    python -I tools/hookshot/hs.py key
    python -I tools/hookshot/hs.py cache list|status|prune [--keep N]
    python -I tools/hookshot/hs.py cache store --from <built game folder> [--force]
    python -I tools/hookshot/hs.py cache restore --to <pair folder>
    python -I tools/hookshot/hs.py lock status|acquire|release --name emu|hs [--owner WHO] [--wait S]
    python -I tools/hookshot/hs.py compile-all [--build DIR] [--emu-build DIR] [--jobs N] [--dry-run]
    python -I tools/hookshot/hs.py build --pair <hookshot-A> --pair <hookshot-B> [--kit DIR --game FILE] [--dry-run]
    python -I tools/hookshot/hs.py warmup --pair <hookshot-A1> --to <hookshot-B1> ... [--timeout S] [--dry-run]

The game build (Hookshot's make_game.py on the player's game file, about an hour) depends only on Hookshot's tools
release (tools/release/kit/hookshot/TOOLS.txt), the pinned commit (hookshot/PIN) and our patches
(hookshot/patches/*.patch). Those three make the key; the kit's Build-Hookshot-Coop writes the same three lines to
the game's coop-built-from.txt, so a game folder built by a kit can seed the cache. With the key unchanged, `build`
copies the cached game into each pair folder and rebuilds only hookshot_coop.dll.

The cache is outside the repo: %LOCALAPPDATA%\\oot-coop\\hs-cache\\<key>\\ (or $OOT_COOP_HS_CACHE). Each entry has
game\\ (the game folder without anything per player), stamp.txt (the three lines) and used.txt (last use).

Locks are folders in the main checkout's root, made with mkdir (atomic), with owner.txt inside:
build-emu.lock (the emulator build) and build-hs.lock (the unit build, hookshot_coop and the Hookshot game). The
old build.lock, when present and not stale, blocks both. A lock is stale after --max-age hours (default 4) or when
its owner's process on this PC is gone.

Nothing here runs a game. Every command that changes something has --dry-run.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

WINDOWS = os.name == "nt"
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LOCK_NAMES = {"emu": "build-emu.lock", "hs": "build-hs.lock"}
OLD_LOCK = "build.lock"
# What is per player or per run in a game folder: never cached, never overwritten in a pair.
PER_PLAYER = {"hookshot_coop.dll", "coop.ini", "coop-options.ini", "user", "save", "autotest-save", "mods",
              "hookshot_log.txt", "hookshot_log.old.txt", "control.json"}
CACHE_GAME = "game"
CACHE_NAME = re.compile(r"^[0-9a-f]{20}(\.part|\.build)?$")
# Caches the game writes while it runs (shaders, pipelines): copied into a pair that has none, never overwritten,
# except when the pair's game was built from another stamp (cache_restore): then they are another build's.
RUNTIME_CACHE = re.compile(r"^(hookshot_(pipelines|pipeline_states|programs|shaders|computes|prepared)\.(bin|idx)"
                           r"|hookshot_pipelines\.bin\.(built|predicted)|hookshot_damaged_files\.txt"
                           r"|mkw_vk_pipelines(_play_\d+)?\.bin)$", re.I)
# What one start's shader preparation leaves (warmup): the noted pipelines built into the pipeline cache. The game build
# prepares in a separate process that does not write hookshot_pipelines.bin.built (on NVIDIA that process's pipeline
# cache does not answer the game's), so each new game folder's first start builds every noted pipeline (35269, about
# 18 min; six at once took 9+ min per load in wave 1). One pair warmed, these files go to the others and the cache.
WARM_FILES = re.compile(r"^(hookshot_pipelines\.bin(\.built|\.predicted)?|hookshot_(pipeline_states|computes|prepared)\.bin"
                        r"|hookshot_programs\.(bin|idx)|mkw_vk_pipelines(_play_\d+)?\.bin)$", re.I)
WARM_DONE = "vk: start-up: the shaders took"


class Fail(Exception):
    pass


# --- the key -----------------------------------------------------------------------------------------------------

def read_keys(path):
    """key=value lines of a small pin file (PIN, TOOLS.txt); # comments ignored."""
    keys = {}
    try:
        f = open(path, encoding="utf-8")
    except OSError:
        raise Fail("no %s" % path)
    with f:
        for line in f:
            m = re.match(r"^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$", line)
            if m and not line.lstrip().startswith("#"):
                keys[m.group(1)] = m.group(2)
    return keys


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def stamp(repo=REPO):
    """The game's build stamp, as hookshot-build.ps1 writes it to coop-built-from.txt (same lines, same order)."""
    tools = read_keys(os.path.join(repo, "tools", "release", "kit", "hookshot", "TOOLS.txt"))
    pin = read_keys(os.path.join(repo, "hookshot", "PIN"))
    for name, d, k in (("TOOLS.txt", tools, "release"), ("PIN", pin, "commit")):
        if k not in d:
            raise Fail("%s has no %s=" % (name, k))
    pdir = os.path.join(repo, "hookshot", "patches")
    patches = sorted(p for p in os.listdir(pdir) if p.endswith(".patch")) if os.path.isdir(pdir) else []
    if not patches:
        raise Fail("no hookshot/patches/*.patch")
    hashes = ",".join(sha256_file(os.path.join(pdir, p)) for p in patches)
    return "release=%s\ncommit=%s\npatches=%s" % (tools["release"], pin["commit"], hashes)


def key_of(stamp_text):
    """The cache key: 20 hex digits of the SHA-256 of the stamp (line endings and trailing space ignored)."""
    norm = "\n".join(line.rstrip() for line in stamp_text.strip().splitlines())
    return hashlib.sha256(norm.encode("utf-8")).hexdigest()[:20]


# --- the cache ---------------------------------------------------------------------------------------------------

def cache_root():
    root = os.environ.get("OOT_COOP_HS_CACHE")
    if not root:
        base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), ".cache")
        root = os.path.join(base, "oot-coop", "hs-cache")
    return root


def is_link(path):
    """A symlink or (Windows) a junction: the game folders link romfs to a shared copy."""
    return os.path.islink(path) or (hasattr(os.path, "isjunction") and os.path.isjunction(path))


def entries(root=None):
    """Cache entries, newest use first: (key, folder, last use as epoch seconds, stamp)."""
    root = root or cache_root()
    out = []
    if not os.path.isdir(root):
        return out
    for k in os.listdir(root):
        d = os.path.join(root, k)
        st = os.path.join(d, "stamp.txt")
        if not (os.path.isdir(os.path.join(d, CACHE_GAME)) and os.path.isfile(st)):
            continue   # half stored (a store that was stopped): ignored, pruned
        used = os.path.join(d, "used.txt")
        t = os.path.getmtime(used if os.path.isfile(used) else st)
        with open(st, encoding="utf-8") as f:
            out.append((k, d, t, f.read()))
    out.sort(key=lambda e: e[2], reverse=True)
    return out


def touch_used(folder):
    with open(os.path.join(folder, "used.txt"), "w", encoding="utf-8") as f:
        f.write(datetime.datetime.now().isoformat(timespec="seconds") + "\n")


def find_exe(folder):
    for p in (os.path.join(folder, "bin", "hookshot.exe"), os.path.join(folder, "hookshot.exe")):
        if os.path.isfile(p):
            return p
    return None


def copy_tree(src, dst, skip_top=PER_PLAYER, dry=False, say=print, runtime_too=False):
    """Copies src into dst: new or changed files (size or mtime) only, links recreated as links (never followed),
    the top-level names in skip_top left alone on both sides; dst's runtime caches kept unless runtime_too.
    Returns (files copied, links made)."""
    copied = links = 0
    for name in sorted(os.listdir(src)):
        if name.lower() in skip_top:
            continue
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if is_link(s):
            target = os.readlink(s)
            if is_link(d) and os.path.normcase(os.readlink(d)) == os.path.normcase(target):
                continue
            say("  link %s -> %s" % (d, target))
            links += 1
            if not dry:
                make_link(d, target)
        elif os.path.isdir(s):
            if not dry:
                os.makedirs(d, exist_ok=True)
            c, n = copy_tree(s, d, set(), dry, say, runtime_too)
            copied, links = copied + c, links + n
        else:
            if os.path.isfile(d) and not is_link(d):
                if skip_top is PER_PLAYER and RUNTIME_CACHE.match(name) and not runtime_too:
                    continue
                a, b = os.stat(s), os.stat(d)
                if a.st_size == b.st_size and int(a.st_mtime) == int(b.st_mtime):
                    continue
            copied += 1
            if not dry:
                os.makedirs(dst, exist_ok=True)
                shutil.copy2(s, d)
    return copied, links


def make_link(path, target):
    if os.path.lexists(path):
        if is_link(path) or os.path.isfile(path):
            os.unlink(path) if not os.path.isdir(path) else os.rmdir(path)
        else:
            raise Fail("%s is a real folder; will not replace it with a link to %s" % (path, target))
    if WINDOWS and os.path.isdir(target):
        r = subprocess.run(["cmd", "/c", "mklink", "/J", path, target], capture_output=True, text=True)
        if r.returncode:
            raise Fail("mklink /J %s failed: %s" % (path, r.stdout + r.stderr))
    else:
        os.symlink(target, path, target_is_directory=os.path.isdir(target))


def cache_store(src, repo=REPO, force=False, dry=False, say=print):
    """Stores a built game folder under the current key. The folder's coop-built-from.txt must match the stamp."""
    want = stamp(repo)
    k = key_of(want)
    if not find_exe(src):
        raise Fail("no hookshot.exe in %s" % src)
    sf = os.path.join(src, "coop-built-from.txt")
    have = ""
    if os.path.isfile(sf):
        with open(sf, encoding="utf-8") as f:
            have = f.read()
    if key_of(have) != k and not force:
        raise Fail("%s was built from\n%s\nnot from the current\n%s\n(--force stores it anyway)"
                   % (src, have.strip() or "(no coop-built-from.txt)", want))
    root = cache_root()
    final = os.path.join(root, k)
    if os.path.isdir(os.path.join(final, CACHE_GAME)) and os.path.isfile(os.path.join(final, "stamp.txt")):
        say("cache: %s already stored" % k)
        return k
    tmp = final + ".part"
    say("cache: storing %s as %s (%s)" % (src, k, final))
    if dry:
        copy_tree(src, os.path.join(tmp, CACHE_GAME), dry=True, say=lambda *_: None)
        return k
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(os.path.join(tmp, CACHE_GAME))
    c, n = copy_tree(src, os.path.join(tmp, CACHE_GAME), say=say)
    with open(os.path.join(tmp, "stamp.txt"), "w", encoding="utf-8") as f:
        f.write(want + "\n")
    touch_used(tmp)
    os.replace(tmp, final)   # the entry appears whole or not at all
    say("cache: stored %s (%d files, %d links)" % (k, c, n))
    return k


def cache_restore(dst, repo=REPO, dry=False, say=print):
    """Copies the cached game for the current key into a pair folder (its per-player files kept). False on a miss."""
    k = key_of(stamp(repo))
    folder = os.path.join(cache_root(), k)
    if not os.path.isfile(os.path.join(folder, "stamp.txt")):
        say("cache: miss for %s" % k)
        return False
    # A pair whose game was built from another stamp has that build's runtime caches: a prepared record and
    # pipeline notes of another translator would make the new game prepare everything again. Replaced then.
    have = ""
    sf = os.path.join(dst, "coop-built-from.txt")
    if os.path.isfile(sf):
        with open(sf, encoding="utf-8") as f:
            have = f.read()
    other = bool(have.strip()) and key_of(have) != k
    if other:
        say("cache: %s held another build (%s): its runtime caches are replaced" % (dst, key_of(have)))
    c, n = copy_tree(os.path.join(folder, CACHE_GAME), dst, dry=dry, say=lambda *_: None, runtime_too=other)
    say("cache: %s -> %s: %d file(s) %s, %d link(s)" % (k, dst, c, "to copy" if dry else "copied", n))
    if not dry:
        touch_used(folder)
    return True


def warm_dir(folder):
    """Where a game folder's caches are: beside its hookshot.exe."""
    exe = find_exe(folder)
    return os.path.dirname(exe) if exe else folder


def warm_files(folder):
    """The warm-up's files in a game folder, by name."""
    d = warm_dir(folder)
    if not os.path.isdir(d):
        return []
    return sorted(n for n in os.listdir(d) if WARM_FILES.match(n) and os.path.isfile(os.path.join(d, n)))


def warm_spread(src, dsts, dry=False, say=print):
    """Copies src's warm-up files over each destination game folder's. Returns the files copied."""
    names = warm_files(src)
    if "hookshot_pipelines.bin.built" not in [n.lower() for n in names]:
        raise Fail("%s has no hookshot_pipelines.bin.built: its start did not finish the shader preparation" % src)
    sdir = warm_dir(src)
    total = 0
    for dst in dsts:
        if not find_exe(dst):
            raise Fail("no hookshot.exe in %s" % dst)
        ddir = warm_dir(dst)
        if os.path.normcase(os.path.abspath(ddir)) == os.path.normcase(os.path.abspath(sdir)):
            continue
        for n in warm_files(dst):   # a warm file src has not (another play file) would mix two starts' caches
            if n not in names:
                say("  remove %s" % os.path.join(ddir, n))
                if not dry:
                    os.unlink(os.path.join(ddir, n))
        for n in names:
            if not dry:
                shutil.copy2(os.path.join(sdir, n), os.path.join(ddir, n))
            total += 1
        say("warmup: %d file(s) %s %s" % (len(names), "to copy into" if dry else "copied into", ddir))
    return total


def warm_start(game, out, timeout, say=print, popen=subprocess.Popen, connect=None, clock=time.monotonic,
               sleep=time.sleep):
    """Starts the game in `game` until its shader preparation ends (WARM_DONE in its output), then quits it through
    its bridge so it writes its caches. True when the line came and the game closed after the quit."""
    exe = find_exe(game)
    if not exe:
        raise Fail("no hookshot.exe in %s" % game)
    os.makedirs(out, exist_ok=True)
    log = os.path.join(out, "warmup-out.txt")
    sock = "coop-warmup-%d" % os.getpid() if WINDOWS else os.path.join(out, "control.sock")
    if connect is None:
        sys.path.insert(0, os.path.join(REPO, "tools", "autotest"))
        import hookshot as hk  # noqa: E402 (the autotest's bridge client)
        connect = hk.Bridge.connect
    env = dict(os.environ, HOS_CONTROL=sock)
    with open(log, "wb") as go:
        kw = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if WINDOWS else {"start_new_session": True}
        p = popen([exe], cwd=os.path.dirname(exe), env=env, stdin=subprocess.DEVNULL, stdout=go,
                  stderr=subprocess.STDOUT, **kw)
    say("warmup: %s started (pid %s), output %s" % (exe, getattr(p, "pid", "?"), log))
    t0, done = clock(), False
    try:
        while clock() - t0 < timeout:
            with open(log, encoding="utf-8", errors="replace") as f:
                text = f.read()
            if WARM_DONE in text:
                done = True
                say("warmup: %s" % [ln for ln in text.splitlines() if WARM_DONE in ln][-1].strip())
                break
            if p.poll() is not None:
                say("warmup: the game ended before its shaders were prepared (exit %s)" % p.returncode)
                return False
            sleep(5)
        if not done:
            say("warmup: no '%s' in %d s" % (WARM_DONE, timeout))
        sleep(10)   # (the title screen's first frames, then a quit: the game writes its pipeline cache as it closes)
        try:
            b = connect(sock, timeout=60)
            b.request("quit", reply_timeout=10.0, check=False)
            b.close()
        except Exception as e:   # noqa: BLE001 (any bridge failure: the wait below ends it)
            say("warmup: quit through the bridge failed (%s)" % e)
        try:
            p.wait(120)
        except subprocess.TimeoutExpired:
            say("warmup: the game did not close in 120 s: ended (its caches may be incomplete)")
            p.kill()
            p.wait(15)
            return False
        return done
    finally:
        if p.poll() is None:
            p.kill()


def warmup(pair, to=(), timeout=3600, out=None, repo=REPO, dry=False, say=print, start=warm_start):
    """One pair's first start prepares the shaders; its warm files then go into the cache entry and the other pairs.
    A game run, not a build: no build lock (whoever runs it holds the test turn)."""
    k = key_of(stamp(repo))
    sf = os.path.join(pair, "coop-built-from.txt")
    have = ""
    if os.path.isfile(sf):
        with open(sf, encoding="utf-8") as f:
            have = f.read()
    if key_of(have) != k:
        raise Fail("%s holds the game of %s, not of the current key %s (hs.py build first)"
                   % (pair, key_of(have) if have.strip() else "nothing", k))
    entry = os.path.join(cache_root(), k, CACHE_GAME)
    say("warmup: key %s, pair %s, then %s" % (k, pair, ", ".join(to) or "no other pair"))
    if dry:
        say("warmup: would start %s until '%s' (at most %d s), quit it, and copy its warm files into %s%s"
            % (find_exe(pair) or pair, WARM_DONE, timeout, entry, "".join(" and " + t for t in to)))
        return True
    if not start(pair, out or os.path.join(pair, "warmup"), timeout, say=say):
        raise Fail("the warm-up start did not finish")
    warm_spread(pair, list(to) + ([entry] if os.path.isdir(entry) else []), say=say)
    say("warmup: OK (a pair's next start should log 'vk: precompile: ... in N ms', not 'start-up: pipelines 0 of')")
    return True


def cache_prune(keep=3, repo=REPO, dry=False, say=print):
    """Removes all but the `keep` most recently used entries (the current key always kept) and half-stored ones."""
    root = cache_root()
    try:
        current = key_of(stamp(repo))
    except Fail:
        current = None
    good = entries(root)
    kept = [e for e in good if e[0] == current] + [e for e in good if e[0] != current]
    kept = {e[0] for e in kept[:max(keep, 1)]}
    gone = []
    for name in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        # Only our own entries: a key or a half-stored key.part, as folders. Anything else in the root stays.
        p = os.path.join(root, name)
        if name in kept or not CACHE_NAME.match(name) or is_link(p) or not os.path.isdir(p):
            continue
        gone.append(name)
        say("cache: prune %s" % name)
        if not dry:
            remove_tree(os.path.join(root, name))
    return gone


def remove_tree(path):
    """rmtree that removes links (junctions) without going into them."""
    for name in os.listdir(path):
        p = os.path.join(path, name)
        if is_link(p):
            os.rmdir(p) if os.path.isdir(p) else os.unlink(p)
        elif os.path.isdir(p):
            remove_tree(p)
        else:
            os.chmod(p, 0o666)
            os.unlink(p)
    os.rmdir(path)


# --- locks -------------------------------------------------------------------------------------------------------

def main_root(repo=REPO):
    """The main checkout's root (locks are shared by all worktrees)."""
    try:
        common = subprocess.run(["git", "-C", repo, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        return os.path.dirname(common)
    except (OSError, subprocess.CalledProcessError):
        return repo


def pid_alive(pid):
    if pid <= 0:
        return False
    if WINDOWS:
        r = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/NH", "/FO", "CSV"], capture_output=True, text=True)
        return ('"%d"' % pid) in r.stdout
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def lock_info(folder):
    """owner.txt of a lock folder as a dict ({} when missing or unreadable)."""
    try:
        with open(os.path.join(folder, "owner.txt"), encoding="utf-8") as f:
            return json.loads(f.read())
    except (OSError, ValueError):
        return {}


def lock_stale(folder, max_age_h=4.0, now=None, alive=pid_alive):
    """Why a lock is stale ('' when it is live). Age from owner.txt's time, else the folder's mtime."""
    now = now if now is not None else time.time()
    info = lock_info(folder)
    t = info.get("time") or os.path.getmtime(folder)
    if now - t > max_age_h * 3600:
        return "older than %g h" % max_age_h
    if info.get("host") == socket.gethostname() and info.get("pid") and not alive(int(info["pid"])):
        return "owner pid %s is gone" % info["pid"]
    return ""


def lock_status(root, max_age_h=4.0, alive=pid_alive):
    """[(lock folder name, held?, owner dict, stale reason)] for the old lock and both split locks."""
    out = []
    for name in (OLD_LOCK,) + tuple(LOCK_NAMES.values()):
        p = os.path.join(root, name)
        if os.path.isdir(p):
            out.append((name, True, lock_info(p), lock_stale(p, max_age_h, alive=alive)))
        else:
            out.append((name, False, {}, ""))
    return out


def lock_acquire(root, which, owner, wait=0.0, max_age_h=4.0, alive=pid_alive, sleep=time.sleep, clock=time.time,
                 say=print, pid=None):
    """Takes build-<which>.lock. Waits up to `wait` s while it or a live build.lock is held; breaks stale ones.
    pid: the process that holds it (default this one, for compile-all and build, which hold it while they run).
    0 = none: the lock goes stale by age only (`hs.py lock acquire` from a shell, whose own process ends at once)."""
    pid = os.getpid() if pid is None else pid
    name = LOCK_NAMES[which]
    path = os.path.join(root, name)
    old = os.path.join(root, OLD_LOCK)
    end = clock() + wait
    while True:
        blocker = None
        if os.path.isdir(old):
            why = lock_stale(old, max_age_h, now=clock(), alive=alive)
            if why:
                say("lock: %s is stale (%s); left in place, not blocking" % (OLD_LOCK, why))
            else:
                blocker = OLD_LOCK
        if not blocker:
            try:
                os.mkdir(path)
                with open(os.path.join(path, "owner.txt"), "w", encoding="utf-8") as f:
                    json.dump({"owner": owner, "pid": pid or None, "host": socket.gethostname(), "time": clock()}, f)
                say("lock: %s taken by %s" % (name, owner))
                return True
            except FileExistsError:
                why = lock_stale(path, max_age_h, now=clock(), alive=alive)
                if why:
                    say("lock: %s held by %s is stale (%s): breaking it" % (name, lock_info(path).get("owner"), why))
                    shutil.rmtree(path, ignore_errors=True)
                    continue
                blocker = name
        if clock() >= end:
            info = lock_info(os.path.join(root, blocker))
            say("lock: %s is held by %s since %s" % (blocker, info.get("owner", "?"), fmt_time(info.get("time"))))
            return False
        sleep(min(10.0, max(0.1, end - clock())))


def lock_release(root, which, owner=None, say=print):
    """Removes build-<which>.lock if `owner` holds it (any owner when None). False when held by someone else."""
    path = os.path.join(root, LOCK_NAMES[which])
    if not os.path.isdir(path):
        return True
    held = lock_info(path).get("owner")
    if owner is not None and held != owner:
        say("lock: %s is held by %s, not %s; left alone" % (LOCK_NAMES[which], held, owner))
        return False
    shutil.rmtree(path, ignore_errors=True)
    say("lock: %s released" % LOCK_NAMES[which])
    return True


def fmt_time(t):
    return datetime.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S") if t else "?"


# --- compiling ---------------------------------------------------------------------------------------------------

def msvc_prefix():
    """[] when cl.exe is on PATH (a developer shell), else a cmd prefix that calls vcvars64.bat first."""
    if not WINDOWS or shutil.which("cl"):
        return []
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vswhere):
        raise Fail("no cl.exe on PATH and no vswhere.exe: run from a VS developer prompt")
    inst = subprocess.run([vswhere, "-latest", "-products", "*", "-requires",
                           "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                          capture_output=True, text=True).stdout.strip()
    bat = os.path.join(inst, "VC", "Auxiliary", "Build", "vcvars64.bat")
    if not os.path.isfile(bat):
        raise Fail("no vcvars64.bat under %s" % inst)
    # vcvars64.bat runs vswhere itself and prints "'vswhere.exe' is not recognized" without its folder on PATH.
    os.environ["PATH"] = os.path.dirname(vswhere) + os.pathsep + os.environ.get("PATH", "")
    return ["cmd", "/c", "call", bat, ">nul", "&&"]


def unit_steps(build, jobs, repo=REPO, target=None):
    """The unit build's commands: configure (Ninja, Release, MSVC) when the folder has no finished configure, then
    build everything or one target. A configure that failed leaves CMakeCache.txt but no build.ninja (ninja then fails
    with "loading 'build.ninja'"): configure again then too."""
    steps = []
    if not all(os.path.isfile(os.path.join(build, f)) for f in ("CMakeCache.txt", "build.ninja")):
        steps.append(["cmake", "-S", repo, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                      "-DCOOP_BUILD_TESTS=ON"])
    b = ["cmake", "--build", build, "--parallel", str(jobs)]
    if target:
        b += ["--target", target]
    steps.append(b)
    return steps


HOOK_OBJ = re.compile(r"^(\S*lichii[/\\]hook[/\\]\S+\.(?:obj|o)):", re.M)


def hook_objects(ninja_targets):
    """The co-op hook objects in the output of `ninja -t targets all` of the emulator's build folder."""
    return sorted(set(HOOK_OBJ.findall(ninja_targets)))


def run_steps(steps, prefix, dry, say=print, cwd=None):
    for s in steps:
        cmd = prefix + s
        say("$ " + " ".join('"%s"' % c if " " in c else c for c in s))
        if dry:
            continue
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace")
        if r.returncode:
            tail = (r.stdout + r.stderr).strip().splitlines()[-25:]
            say("\n".join(tail))
            return False
    return True


def compile_all(build, emu_build=None, jobs=8, repo=REPO, dry=False, say=print, root=None, owner="compile-all"):
    """Unit build (every hook file through the tests' build, plus hookshot_coop) and, with emu_build, the emulator's
    co-op hook objects only. One OK/FAIL line at the end. Takes build-hs.lock (and build-emu.lock with emu_build)."""
    root = root or main_root(repo)
    t0 = time.time()
    taken, ok, what = [], True, []
    try:
        for which in (["hs"] + (["emu"] if emu_build else [])):
            if dry:
                say("lock: would take %s" % LOCK_NAMES[which])
            elif lock_acquire(root, which, owner, wait=1800, say=say):
                taken.append(which)
            else:
                raise Fail("could not take %s" % LOCK_NAMES[which])
        prefix = [] if dry else msvc_prefix()
        ok = run_steps(unit_steps(build, jobs, repo), prefix, dry, say)
        what.append("unit+hookshot_coop")
        if ok and emu_build:
            if dry:
                say("$ ninja -C %s -t targets all  (then: ninja -C %s -j %d <lichii/hook objects>)"
                    % (emu_build, emu_build, jobs))
            else:
                r = subprocess.run(prefix + ["ninja", "-C", emu_build, "-t", "targets", "all"],
                                   capture_output=True, text=True, errors="replace")
                objs = hook_objects(r.stdout)
                if not objs:
                    raise Fail("no lichii/hook objects in %s (not a Ninja build of the emulator?)" % emu_build)
                ok = run_steps([["ninja", "-C", emu_build, "-j", str(jobs)] + objs], prefix, dry, say)
                what.append("emulator hook (%d objects)" % len(objs))
    except Fail as e:
        ok = False
        say(str(e))
    finally:
        for which in taken:
            lock_release(root, which, owner, say=say)
    line = "compile-all: %s (%s, %ds%s)" % ("OK" if ok else "FAIL", ", ".join(what) or "nothing",
                                             time.time() - t0, ", dry run" if dry else "")
    say(line)
    return ok


def find_dll(build):
    for p in (os.path.join(build, "hookshot", "hookshot_coop.dll"),
              os.path.join(build, "hookshot", "Release", "hookshot_coop.dll")):
        if os.path.isfile(p):
            return p
    return os.path.join(build, "hookshot", "hookshot_coop.dll")


def build(pairs, build_dir, kit=None, game=None, jobs=8, keep=3, repo=REPO, dry=False, say=print, root=None,
          owner="hs-build"):
    """Game from the cache (or built once through a kit and stored), hookshot_coop.dll rebuilt, both into each pair."""
    root = root or main_root(repo)
    st = stamp(repo)
    k = key_of(st)
    say("key %s (%s)" % (k, st.replace("\n", "; ")[:160]))
    hit = os.path.isfile(os.path.join(cache_root(), k, "stamp.txt"))
    if not dry and not lock_acquire(root, "hs", owner, wait=1800, say=say):
        return False
    try:
        if not hit:
            if not kit:
                raise Fail("cache miss for %s: pass --kit <a made kit> --game <your .nup/.xci> to build the game once,"
                           " or `cache store --from <a game folder built from this stamp>`" % k)
            out = os.path.join(cache_root(), k + ".build")
            cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                   os.path.join(kit, "scripts", "hookshot-build.ps1"), "-Out", out, "-Jobs", str(jobs)]
            if game:
                cmd += ["-Game", game]
            say("cache: miss; building the game once (about an hour)")
            if not run_steps([cmd], [], dry, say):
                raise Fail("hookshot-build.ps1 failed")
            if dry:
                say("cache: would store %s as %s" % (out, k))
            else:
                cache_store(out, repo, say=say)
                remove_tree(out)
        else:
            say("cache: hit %s" % k)
        prefix = [] if dry else msvc_prefix()
        if not run_steps(unit_steps(build_dir, jobs, repo, target="hookshot_coop"), prefix, dry, say):
            raise Fail("hookshot_coop did not build")
        dll = find_dll(build_dir)
        for p in pairs:
            if not dry:
                os.makedirs(p, exist_ok=True)
            if hit or not dry:
                cache_restore(p, repo, dry=dry, say=say)
            else:
                say("cache: would copy the new game into %s" % p)
            exe = find_exe(p) if not dry else os.path.join(p, "bin", "hookshot.exe")
            dest = os.path.dirname(exe) if exe else p
            say("copy %s -> %s" % (dll, dest))
            if not dry:
                shutil.copy2(dll, dest)
        cache_prune(keep, repo, dry=dry, say=say)
        say("build: OK%s" % (" (dry run)" if dry else ""))
        return True
    except Fail as e:
        say(str(e))
        say("build: FAIL")
        return False
    finally:
        if not dry:
            lock_release(root, "hs", owner, say=say)


# --- command line ------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", default=REPO)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("key")
    c = sub.add_parser("cache")
    c.add_argument("action", choices=["list", "status", "store", "restore", "prune"])
    c.add_argument("--from", dest="src")
    c.add_argument("--to")
    c.add_argument("--keep", type=int, default=3)
    c.add_argument("--force", action="store_true")
    c.add_argument("--dry-run", action="store_true")
    lk = sub.add_parser("lock")
    lk.add_argument("action", choices=["status", "acquire", "release"])
    lk.add_argument("--name", choices=sorted(LOCK_NAMES))
    lk.add_argument("--owner", default=os.environ.get("USERNAME", "someone"))
    lk.add_argument("--wait", type=float, default=0)
    lk.add_argument("--max-age", type=float, default=4.0, help="hours before a lock is stale")
    lk.add_argument("--pid", type=int, default=0,
                    help="acquire: the process that holds the lock (stale when it is gone); default none: by age only")
    lk.add_argument("--root", help="folder of the lock folders (default: the main checkout's root)")
    ca = sub.add_parser("compile-all")
    ca.add_argument("--build", help="unit build folder (default: <repo>/build)")
    ca.add_argument("--emu-build", help="the emulator's Ninja build folder: also compile its lichii/hook objects")
    ca.add_argument("--jobs", type=int, default=8)
    ca.add_argument("--dry-run", action="store_true")
    w = sub.add_parser("warmup", help="one pair prepares the shaders; its caches go to the other pairs and the cache")
    w.add_argument("--pair", required=True, help="the game folder that is started (built from the current key)")
    w.add_argument("--to", action="append", default=[], help="another pair that gets the warm files (repeat)")
    w.add_argument("--timeout", type=int, default=3600)
    w.add_argument("--dry-run", action="store_true")
    b = sub.add_parser("build")
    b.add_argument("--pair", action="append", required=True, help="a Hookshot game folder to fill (repeat)")
    b.add_argument("--build", help="unit build folder (default: <repo>/build)")
    b.add_argument("--kit", help="a made kit: its hookshot-build.ps1 builds the game on a cache miss")
    b.add_argument("--game", help="your game file for that build")
    b.add_argument("--jobs", type=int, default=8)
    b.add_argument("--keep", type=int, default=3)
    b.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    repo = os.path.abspath(a.repo)
    try:
        if a.cmd == "key":
            st = stamp(repo)
            print(key_of(st))
            print(st)
            print("cache: %s (%s)" % (os.path.join(cache_root(), key_of(st)),
                                     "stored" if os.path.isdir(os.path.join(cache_root(), key_of(st))) else "missing"))
            return 0
        if a.cmd == "cache":
            if a.action in ("list", "status"):
                cur = key_of(stamp(repo))
                es = entries()
                print("cache %s: %d entr%s" % (cache_root(), len(es), "y" if len(es) == 1 else "ies"))
                for k, d, t, st in es:
                    print("  %s%s  used %s  %s" % (k, " (current)" if k == cur else "", fmt_time(t),
                                                   st.strip().replace("\n", "; ")[:100]))
                if a.action == "status":
                    print("current key %s: %s" % (cur, "HIT" if any(e[0] == cur for e in es) else "MISS"))
                return 0
            if a.action == "store":
                if not a.src:
                    raise Fail("--from <game folder>")
                cache_store(os.path.abspath(a.src), repo, a.force, a.dry_run)
                return 0
            if a.action == "restore":
                if not a.to:
                    raise Fail("--to <pair folder>")
                return 0 if cache_restore(os.path.abspath(a.to), repo, a.dry_run) else 1
            cache_prune(a.keep, repo, a.dry_run)
            return 0
        if a.cmd == "lock":
            root = a.root or main_root(repo)
            if a.action == "status":
                for name, held, info, why in lock_status(root, a.max_age):
                    print("%-14s %s" % (name, "free" if not held else "held by %s since %s%s" % (
                        info.get("owner", "?"), fmt_time(info.get("time")), " (STALE: %s)" % why if why else "")))
                return 0
            if not a.name:
                raise Fail("--name emu|hs")
            if a.action == "acquire":
                return 0 if lock_acquire(root, a.name, a.owner, a.wait, a.max_age, pid=a.pid) else 1
            return 0 if lock_release(root, a.name, a.owner) else 1
        if a.cmd == "warmup":
            return 0 if warmup(os.path.abspath(a.pair), [os.path.abspath(t) for t in a.to], a.timeout,
                               repo=repo, dry=a.dry_run) else 1
        bdir = os.path.abspath(a.build) if a.build else os.path.join(repo, "build")
        if a.cmd == "compile-all":
            return 0 if compile_all(bdir, a.emu_build, a.jobs, repo, a.dry_run) else 1
        return 0 if build([os.path.abspath(p) for p in a.pair], bdir, a.kit, a.game, a.jobs, a.keep, repo,
                          a.dry_run) else 1
    except Fail as e:
        print("hs.py: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
