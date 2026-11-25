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
A miss builds in the pin's shared tools and work folders (cache\\pins\\<release>-<commit>), so only what
changed is translated and compiled again (hs-build-speed).

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
CREATE_NO_WINDOW = 0x08000000   # a console program started with this gets no window, and its own children none either


def no_window(kw=None):
    """Popen keywords with CREATE_NO_WINDOW added on Windows (other keywords and other creation flags kept).
    Why: a console program (powershell, tasklist, cmd, git, ninja, a python helper) started by a process that has no
    console of its own, such as the lane watchdog or a harness's shell, gets a NEW visible console window; started from
    a long run, they flash on the screen in a chain. Use run_quiet and popen_quiet for every such child. NEVER for a
    game (Longshot's exe): its window is the game's own and a hidden console would change how it starts."""
    kw = dict(kw or {})
    if WINDOWS:
        kw["creationflags"] = kw.get("creationflags", 0) | CREATE_NO_WINDOW
    return kw


def run_quiet(cmd, **kw):
    """subprocess.run of a console program with no window on Windows (see no_window)."""
    return subprocess.run(cmd, **no_window(kw))


def popen_quiet(cmd, **kw):
    """subprocess.Popen of a console program with no window on Windows (see no_window)."""
    return subprocess.Popen(cmd, **no_window(kw))
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
# build-game.lock: a game build (a cache miss) is running; it stops both test lanes (tools/autotest/lanes.py), whose
# runs hold lane1.lock / lane2.lock. A module build (build-hs.lock alone) stops no lane.
LOCK_NAMES = {"emu": "build-emu.lock", "hs": "build-hs.lock", "game": "build-game.lock", "lane1": "lane1.lock",
              "lane2": "lane2.lock"}
# Each game of a lane is a lane of its own too (a one-game live session): laneN-A.lock, laneN-B.lock.
LOCK_NAMES.update({"lane%d-%s" % (n, x): "lane%d-%s.lock" % (n, x) for n in (1, 2) for x in "AB"})
LANE_LOCKS = tuple(k for k in LOCK_NAMES if k.startswith("lane"))
OLD_LOCK = "build.lock"
# What is per player or per run in a game folder: never cached, never overwritten in a pair.
PER_PLAYER = {"hookshot_coop.dll", "coop.ini", "coop-options.ini", "user", "save", "autotest-save", "mods",
              "longshot_log.txt", "longshot_log.old.txt", "control.json"}
CACHE_GAME = "game"
CACHE_NAME = re.compile(r"^[0-9a-f]{20}(\.part|\.build)?$")
# Caches the game writes while it runs (shaders, pipelines): copied into a pair that has none, never overwritten,
# except when the pair's game was built from another stamp (cache_restore): then they are another build's.
# Longshot rc.4 keeps them in a cache\ folder beside longshot.exe (CACHE_DIR), with its own names for some
# (longshot_vk_pipelines*, longshot_compute_variants, longshot_module_ids); the game build already fills part of it.
RUNTIME_CACHE = re.compile(r"^(longshot_(pipelines|pipeline_states|programs|shaders|computes|prepared)\.(bin|idx)"
                           r"|longshot_pipelines\.bin\.(built|predicted)|longshot_damaged_files\.txt"
                           r"|longshot_(compute_variants|module_ids)\.bin|longshot_vk_pipelines(_play_\d+)?\.bin"
                           r"|longshot_vk_pipelines\.stamp|mkw_vk_pipelines(_play_\d+)?\.bin)$", re.I)
CACHE_DIR = "cache"
# What one start's shader preparation leaves (warmup): the noted pipelines built into the pipeline cache. The game build
# prepares in a separate process that does not write longshot_pipelines.bin.built (on NVIDIA that process's pipeline
# cache does not answer the game's), so each new game folder's first start builds every noted pipeline (35269, about
# 18 min; six at once took 9+ min per load in wave 1). One pair warmed, these files go to the others and the cache.
# LIMIT (Windows + NVIDIA, Longshot rc.7, big-run-fix-a 2026-11-17): the copies do not warm the other folders there. Every
# longshot.exe shares the driver's one shader cache (%LOCALAPPDATA%\NVIDIA\GLCache) and only the first game running is
# served by it; a game started beside it logs "the driver's shader cache has 0 of 48 noted pipelines asked", drops
# longshot_pipelines.bin.built and compiles every pipeline again at each start (~440 s), copied files or not
# (__GL_SHADER_DISK_CACHE_PATH is ignored there). The harness's renderer wait follows that compile's progress instead
# (tools/autotest/hookshot.py wait_renderer).
WARM_FILES = re.compile(r"^(longshot_pipelines\.bin(\.built|\.predicted)?|longshot_(pipeline_states|computes|prepared)\.bin"
                        r"|longshot_programs\.(bin|idx)|mkw_vk_pipelines(_play_\d+)?\.bin"
                        r"|longshot_(compute_variants|module_ids)\.bin|longshot_vk_pipelines(_play_\d+)?\.bin"
                        r"|longshot_vk_pipelines\.stamp)$", re.I)
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


# --- the mods' code patches (patches 0034/0035) -------------------------------------------------------------------
# A mod with code patches (an exefs folder of .ips / .pchtxt: OoT Character Select) is compiled into the game when it is
# built, from the game folder's mods (or HOS_MODS, the game's own name for that folder: nx2/exefs_patches.py). A test
# pair's game is built in the pin's shared folder, which has none of the pairs' mods: before charsel-tab-0119 every pair
# got a game without them (0.11.9 test kit: no Characters tab, his code not in the game, while B1 kept an older build's
# exefs_patches.txt saying it was). Now the pairs' code mods go into the build (HOS_MODS) and into its key, by the kit's
# own rule (kit-paths.ps1 Get-KitExefsStampLine, the line hookshot-build.ps1 writes into coop-built-from.txt).
KIT_PATHS = os.path.join("tools", "release", "kit", "scripts", "kit-paths.ps1")
EXEFS_LIST = "exefs_patches.txt"   # (the game folder's list of the code patches it was built with: host/mods.c reads it)


def has_code_patches(mods):
    """Whether a mods folder holds a code patch the kit's rule counts: <mod>/exefs/*.ips or *.pchtxt, at most five
    folders down, no folder before exefs named with '-', '_' or '.' first (a mod kept aside: '_OoT Character Select.prev')."""
    if not mods or not os.path.isdir(mods):
        return False
    base = os.path.abspath(mods)
    for top, dirs, files in os.walk(base):
        rel = os.path.relpath(top, base)
        parts = [] if rel == "." else rel.split(os.sep)
        if len(parts) > 5:
            dirs[:] = []
            continue
        if len(parts) >= 2 and parts[-1].lower() == "exefs" and not any(x[:1] in "-_." for x in parts[:-1]):
            if any(f.lower().endswith((".ips", ".pchtxt")) for f in files):
                return True
    return False


def code_mods_line(mods, repo=REPO, run=run_quiet):
    """The code patches' part of a game's stamp for a mods folder: '' without code patches, else the kit's own line
    (Get-KitExefsStampLine: each patch file's path and SHA-256, and those mods' on/off rows in coop-mods.list)."""
    if not has_code_patches(mods):
        return ""
    q = lambda t: "'" + t.replace("'", "''") + "'"
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
           ". %s; [Console]::Out.Write((Get-KitExefsStampLine -Game '' -Mods %s))"
           % (q(os.path.join(repo, KIT_PATHS)), q(os.path.abspath(mods)))]
    r = run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode or not r.stdout.strip():
        raise Fail("the mods' code patches in %s could not be read (kit-paths.ps1 Get-KitExefsStampLine): %s"
                   % (mods, (r.stdout + r.stderr).strip()[-300:]))
    return r.stdout.replace("\r", "")


def code_mods_mismatch(game, repo=REPO, run=run_quiet):
    """None when a game folder's mods have the code patches its game was built with (coop-built-from.txt's exefs= line,
    none without), else why not. A code mod put into a pair after its build (lane-0119 2026-11-21: Character Select
    copied into lane1's mods) shows his menus without his code: no Characters tab, a pick does nothing."""
    try:
        with open(os.path.join(game, "coop-built-from.txt"), encoding="utf-8") as f:
            built = [l.strip() for l in f.read().splitlines() if l.strip().startswith("exefs=")]
    except OSError:
        return None   # (no build of ours there)
    want = code_mods_line(os.path.join(game, "mods"), repo, run).strip()
    have = built[0] if built else ""
    if want == have:
        return None
    return ("%s: its mods' code patches are not the ones its game was built with (game: %s; mods: %s): build it again"
            " (hs.py build --pair, or the kit's start)" % (game, have or "none", want or "none"))


PACK_DIR = os.path.join("mods", "contents", "0400486024DEC000", "coop-puppet")


def sync_charsel_native(pairs, repo=REPO, run=run_quiet):
    """Co-op's own Characters tab's code patch (charsel/data/charsel-native.ips) put into or taken out of each pair's
    pack folder before the pairs' code patches are read, by the kit's own rule (kit-paths.ps1
    Sync-KitCharselNativePatch, what hookshot-build.ps1 does before its stamp): a pair without Character Select's code
    gets ours in its game, a pair with his gets his only. Pairs without a pack folder: nothing to do (no call).
    Returns {pair: 'in' | 'out' | ''}."""
    packs = [(p, os.path.join(p, PACK_DIR)) for p in pairs if os.path.isdir(os.path.join(p, PACK_DIR))]
    if not packs:
        return {}
    q = lambda t: "'" + t.replace("'", "''") + "'"
    body = "; ".join("[Console]::Out.WriteLine('r=' + (Sync-KitCharselNativePatch %s %s))" % (q(os.path.abspath(repo)),
                                                                                       q(os.path.abspath(m)))
                     for _, m in packs)
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
           ". %s; %s" % (q(os.path.join(repo, KIT_PATHS)), body)]
    r = run(cmd, capture_output=True, text=True, errors="replace")
    got = [l[2:].strip() for l in (r.stdout or "").replace("\r", "").split("\n") if l.startswith("r=")]
    if r.returncode or len(got) != len(packs):
        raise Fail("co-op's Characters tab's code patch could not be put into the pairs' packs (kit-paths.ps1"
                   " Sync-KitCharselNativePatch): %s" % ((r.stdout or "") + (r.stderr or "")).strip()[-300:])
    return {p: g for (p, _), g in zip(packs, got)}


def pairs_code_mods(pairs, repo=REPO, run=run_quiet):
    """(line, mods folder) of the pairs' code patches; ('', None) when no pair has any. One game serves every pair,
    so pairs with different code patches cannot be built together (Fail)."""
    found = [(code_mods_line(os.path.join(p, "mods"), repo, run), os.path.join(p, "mods")) for p in pairs]
    if len({line for line, _ in found}) > 1:
        raise Fail("the pairs have different mods with code patches (one game serves them all): %s"
                   % "; ".join("%s: %s" % (m, line.strip() or "none") for line, m in found))
    return (found[0][0], found[0][1]) if found and found[0][0] else ("", None)


def pin_of(stamp_text):
    """'<release>-<commit 12>' of a stamp: what one shared tools and work folder serves (hs-build-speed)."""
    keys = dict(line.split("=", 1) for line in stamp_text.replace("\r", "").split("\n") if "=" in line)
    if not keys.get("release") or not keys.get("commit"):
        raise Fail("a stamp without release= and commit=")
    return "%s-%s" % (re.sub(r"[^A-Za-z0-9._-]", "_", keys["release"]), keys["commit"][:12])


def pin_dir(stamp_text):
    """The shared folder of a pin: tools/ (Hookshot's tools and patched source), work/ (its build files and object
    cache) and game/ (THE game folder of the pin, built again in place). Every build of that pin, whatever the patches,
    goes through it: Hookshot's own lift key and object cache decide what is done again. The game folder must stay the
    same path: it keeps .longshot_exefs and the prepared shaders (and before Longshot rc.6 the lift key hashed it, make_game.py:
    OOT_DUMP, OOT_ROMFS; rc.6 keys the contents only). Not a cache entry (cache_prune leaves it); the cache gets a copy of the game folder."""
    return os.path.join(cache_root(), "pins", pin_of(stamp_text))


def key_of(stamp_text):
    """The cache key: 20 hex digits of the SHA-256 of the stamp (line endings and trailing space ignored)."""
    norm = "\n".join(line.rstrip() for line in stamp_text.strip().splitlines())
    return hashlib.sha256(norm.encode("utf-8")).hexdigest()[:20]


# --- the cache ---------------------------------------------------------------------------------------------------

_packaged_warned = set()


def packaged(path):
    """Whether a path resolves into a packaged (MSIX) app's own copy of AppData (<...>/AppData/Local/Packages/):
    a process started from such an app writes its %LOCALAPPDATA% files there, where no other process sees them."""
    p = os.path.realpath(path).replace("\\", "/").lower()
    return "/appdata/local/packages/" in p


def cache_root():
    root = os.environ.get("OOT_COOP_HS_CACHE")
    if not root:
        base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), ".cache")
        root = os.path.join(base, "oot-coop", "hs-cache")
    if root not in _packaged_warned and packaged(root):
        _packaged_warned.add(root)
        sys.stderr.write("hs.py: WARNING: the cache %s resolves into a packaged app's own AppData (%s): its files are"
                         " invisible to processes started outside that app (the kit's start, Explorer) and go with"
                         " the app. Set OOT_COOP_HS_CACHE to a folder outside AppData (docs/testing.md).\n"
                         % (root, os.path.realpath(root)))
    return root


IO_REPARSE_TAG_MOUNT_POINT = 0xA0000003   # (a junction's reparse tag: stat.IO_REPARSE_TAG_MOUNT_POINT, Windows)


def is_link(path):
    """A symlink or (Windows) a junction: the game folders link romfs to a shared copy."""
    if os.path.islink(path):
        return True
    if hasattr(os.path, "isjunction"):   # (Python 3.12+)
        return os.path.isjunction(path)
    # Before 3.12 (CI's 3.10): the junction's reparse tag. Without this a junction looked like a real folder, and
    # remove_tree / cache_prune went into it and deleted the shared romfs it points to.
    try:
        return os.lstat(path).st_reparse_tag == IO_REPARSE_TAG_MOUNT_POINT
    except (OSError, AttributeError):   # (missing, or not Windows: no st_reparse_tag)
        return False


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
    for p in (os.path.join(folder, "bin", "longshot.exe"), os.path.join(folder, "longshot.exe")):
        if os.path.isfile(p):
            return p
    return None


SHARED_TOP = {"romfs"}   # (the game files, ~26 GB: never written by the game or by us; hard links, as lanes.py's)


def same_file(a, b):
    try:
        return os.path.samefile(a, b)
    except OSError:
        return False


NTFS_MAX_LINKS = 1023
LINKS_REFUSED = set()   # ((source drive, destination drive)): a link was refused there in this run (link_or_copy)


def can_link(a, b):
    """Whether a hard link to the file of stat a could replace the file of stat b: the same drive, a below the file
    system's limit of names (NTFS: 1023), and no link refused between those drives in this run"""
    return a.st_dev == b.st_dev and a.st_nlink < NTFS_MAX_LINKS and (a.st_dev, b.st_dev) not in LINKS_REFUSED


def link_or_copy(s, d):
    """d becomes a hard link to s (no new disk space); its own copy where a link can't be made (another drive, FAT).
    An old d is unlinked first, never written into: it may be shared with other folders. Returns True for a link."""
    if os.path.lexists(d):
        os.unlink(d)
    try:
        os.link(s, d)
        return True
    except OSError:
        shutil.copy2(s, d)
        LINKS_REFUSED.add((os.stat(s).st_dev, os.stat(d).st_dev))   # (the rest of this run keeps its copies as they are)
        return False


def copy_tree(src, dst, skip_top=PER_PLAYER, dry=False, say=print, runtime_too=False, in_cache=False, share=False):
    """Copies src into dst: new or changed files (size or mtime) only, links recreated as links (never followed),
    the top-level names in skip_top left alone on both sides; dst's runtime caches (at the top, or in the top's
    cache folder: in_cache) kept unless runtime_too. Returns (files copied, links made).
    A top-level folder in SHARED_TOP (romfs: the game files) is hard-linked file by file, not copied (share): every
    game folder made from one (a pin, the cache's entries, the test pairs) shares one copy of its ~26 GB. Copied in
    full to every one of them, they filled the disk (longshot-rc6-a's offline run 2: no space left for the next pin)."""
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
            c, n = copy_tree(s, d, set(), dry, say, runtime_too,
                             in_cache=skip_top is PER_PLAYER and name.lower() == CACHE_DIR,
                             share=share or (skip_top is PER_PLAYER and name.lower() in SHARED_TOP))
            copied, links = copied + c, links + n
        else:
            if os.path.isfile(d) and not is_link(d):
                if (skip_top is PER_PLAYER or in_cache) and RUNTIME_CACHE.match(name) and not runtime_too:
                    continue
                if share and same_file(s, d):
                    continue   # (already the same file)
                a, b = os.stat(s), os.stat(d)
                # (a separate copy of the same file: left as it is unless a link can take its place, so a cache on
                # another drive, a FAT disk or a file at NTFS's 1023 links is not copied again on every run)
                if a.st_size == b.st_size and int(a.st_mtime) == int(b.st_mtime) and not (share and can_link(a, b)):
                    continue
            copied += 1
            if not dry:
                os.makedirs(dst, exist_ok=True)
                if share:
                    link_or_copy(s, d)
                    continue
                if os.path.isfile(d) and os.stat(d).st_nlink > 1:
                    os.unlink(d)   # a hard link shared with another folder (a test lane): never written into
                shutil.copy2(s, d)
    return copied, links


def make_link(path, target):
    if os.path.lexists(path):
        if is_link(path) or os.path.isfile(path):
            os.unlink(path) if not os.path.isdir(path) else os.rmdir(path)
        else:
            raise Fail("%s is a real folder; will not replace it with a link to %s" % (path, target))
    if WINDOWS and os.path.isdir(target):
        r = run_quiet(["cmd", "/c", "mklink", "/J", path, target], capture_output=True, text=True)
        if r.returncode:
            raise Fail("mklink /J %s failed: %s" % (path, r.stdout + r.stderr))
    else:
        os.symlink(target, path, target_is_directory=os.path.isdir(target))


def cache_store(src, repo=REPO, force=False, dry=False, say=print, want=None):
    """Stores a built game folder under the current key (want: the stamp; default the repo's with the folder's own
    mods' code patches). The folder's coop-built-from.txt must match the stamp. A pin's game folder has no mods of a
    player (its mods hold the last kit build's puppet pack): its want must be given (the pairs': cache store --pair)."""
    if not want and in_pins(src):
        raise Fail("%s is a pin's game folder: its mods are the last kit build's, not a pair's; name the pairs whose"
                   " game it is (cache store --pair <pair>)" % src)
    want = want or stamp(repo) + code_mods_line(os.path.join(src, "mods"), repo)
    k = key_of(want)
    if not find_exe(src):
        raise Fail("no longshot.exe in %s" % src)
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


def cache_restore(dst, repo=REPO, dry=False, say=print, want=None):
    """Copies the cached game for the current key into a pair folder (its per-player files kept). False on a miss.
    want: the stamp (default the repo's with the pair's own mods' code patches)."""
    k = key_of(want or stamp(repo) + code_mods_line(os.path.join(dst, "mods"), repo))
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
    # (a build without code patches has no list; an older build's would say the game holds patches it does not: the
    # kit's own restore takes it away too, kit-paths.ps1 Restore-KitGameVariant)
    stale = os.path.join(dst, EXEFS_LIST)
    if not os.path.isfile(os.path.join(folder, CACHE_GAME, EXEFS_LIST)) and os.path.isfile(stale):
        say("cache: %s: an older build's %s removed (this game has no mods' code patches)" % (dst, EXEFS_LIST))
        if not dry:
            os.remove(stale)
    if not dry:
        touch_used(folder)
    return True


def warm_dir(folder):
    """Where a game folder's caches are: beside its longshot.exe, or in the cache folder there (Longshot rc.4)."""
    exe = find_exe(folder)
    d = os.path.dirname(exe) if exe else folder
    return os.path.join(d, CACHE_DIR) if os.path.isdir(os.path.join(d, CACHE_DIR)) else d


def warm_files(folder):
    """The warm-up's files in a game folder, by name."""
    d = warm_dir(folder)
    if not os.path.isdir(d):
        return []
    return sorted(n for n in os.listdir(d) if WARM_FILES.match(n) and os.path.isfile(os.path.join(d, n)))


def warm_spread(src, dsts, dry=False, say=print):
    """Copies src's warm-up files over each destination game folder's. Returns the files copied."""
    names = warm_files(src)
    if "longshot_pipelines.bin.built" not in [n.lower() for n in names]:
        raise Fail("%s has no longshot_pipelines.bin.built: its start did not finish the shader preparation" % src)
    sdir = warm_dir(src)
    total = 0
    for dst in dsts:
        if not find_exe(dst):
            raise Fail("no longshot.exe in %s" % dst)
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
        raise Fail("no longshot.exe in %s" % game)
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
    k = key_of(stamp(repo) + code_mods_line(os.path.join(pair, "mods"), repo))
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
        common = run_quiet(["git", "-C", repo, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        return os.path.dirname(common)
    except (OSError, subprocess.CalledProcessError):
        return repo


def _win_pid_probe(pid):
    """'alive', 'gone' or 'unknown' for a pid, from the process itself (OpenProcess and a zero wait on its handle).
    Gone only when Windows says so: no such process (ERROR_INVALID_PARAMETER) or its handle is signalled (exited).
    Anything else (access denied, an API error) is 'unknown'."""
    import ctypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.OpenProcess.restype = ctypes.c_void_p
    k.OpenProcess.argtypes = (ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong)
    k.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_ulong)
    k.WaitForSingleObject.restype = ctypes.c_ulong
    k.CloseHandle.argtypes = (ctypes.c_void_p,)
    h = k.OpenProcess(0x00100000 | 0x1000, False, pid)   # SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        err = ctypes.get_last_error()
        return "gone" if err == 87 else ("alive" if err == 5 else "unknown")   # 87 no such pid, 5 exists but denied
    try:
        w = k.WaitForSingleObject(h, 0)
        return "gone" if w == 0 else ("alive" if w == 0x102 else "unknown")   # WAIT_OBJECT_0: exited; WAIT_TIMEOUT
    finally:
        k.CloseHandle(h)


def _posix_pid_probe(pid):
    try:
        os.kill(pid, 0)
        return "alive"
    except PermissionError:
        return "alive"      # (exists, someone else's)
    except ProcessLookupError:
        return "gone"
    except OSError:
        return "unknown"


def pid_alive(pid, probe=None):
    """False only when the process is known to be gone. A probe that cannot tell (an API error, a busy PC) counts as
    alive: every caller ends a session, kills or takes a lock on False, so a wrong False is the harmful one (a live game
    once had its session closed under it when one tasklist call came back empty). The lock's age limit still frees a
    lock whose owner can never be probed."""
    if pid <= 0:
        return False
    probe = probe or (_win_pid_probe if WINDOWS else _posix_pid_probe)
    try:
        return probe(pid) != "gone"
    except Exception:
        return True


def host_id():
    """This PC in a lock's owner.txt: a short hash of its name, so the file never holds the PC name."""
    return hashlib.sha256(socket.gethostname().encode("utf-8")).hexdigest()[:12]


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
    if info.get("host") in (host_id(), socket.gethostname()) and info.get("pid") and not alive(int(info["pid"])):
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
                    json.dump({"owner": owner, "pid": pid or None, "host": host_id(), "time": clock()}, f)
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


def wait_free(folder, wait, say=print, alive=pid_alive, sleep=time.sleep, clock=time.time):
    """Waits up to `wait` s until the lock folder is gone or stale. True when free."""
    end = clock() + wait
    told = False
    while os.path.isdir(folder) and not lock_stale(folder, now=clock(), alive=alive):
        if clock() >= end:
            return False
        if not told:
            say("lock: waiting for %s (%s)" % (os.path.basename(folder), lock_info(folder).get("owner", "?")))
            told = True
        sleep(min(10.0, max(0.1, end - clock())))
    return True


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
    inst = run_quiet([vswhere, "-latest", "-products", "*", "-requires",
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


def run_steps(steps, prefix, dry, say=print, cwd=None, env=None):
    for s in steps:
        cmd = prefix + s
        say("$ " + " ".join('"%s"' % c if " " in c else c for c in s))
        if dry:
            continue
        r = run_quiet(cmd, cwd=cwd, env=env, capture_output=True, text=True, errors="replace")
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
                r = run_quiet(prefix + ["ninja", "-C", emu_build, "-t", "targets", "all"],
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


def build_script_args(script):
    """What the kit's hookshot-build.ps1 takes: a kit made before hs-build-speed has no -Tools/-Work."""
    try:
        with open(script, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return set()
    return {a for a in ("Tools", "Work", "RomfsFrom") if re.search(r"\[string\]\$%s\b" % a, text)}


def romfs_source(out, root=None):
    """For a pin's game folder with no game files of its own yet: the newest other pin's game folder that unpacked its
    own (romfs/ and Longshot's .longshot_romfs stamp), for Longshot's --romfs-from (rc.6, #360): it hard-links that
    folder's files when its stamp is this .nup's and they are on one drive, else reads the .nup as before. None when
    out has its own, or there is no other."""
    if os.path.isfile(os.path.join(out, ".longshot_romfs")):
        return None
    pins = os.path.join(root or cache_root(), "pins")
    found = []
    for name in os.listdir(pins) if os.path.isdir(pins) else ():
        game = os.path.join(pins, name, "game")
        stamp_file = os.path.join(game, ".longshot_romfs")
        if same_file(game, out) or not (os.path.isdir(os.path.join(game, "romfs")) and os.path.isfile(stamp_file)):
            continue
        found.append((os.path.getmtime(stamp_file), game))
    return max(found)[1] if found else None


NO_MODS = "no-mods"   # (in the cache root: an empty mods folder for builds whose pairs have no code mods)


def no_mods_dir(dry=False):
    """The empty folder a game build gets as HOS_MODS when the pairs have no mods with code patches (made empty again
    if anything was put into it: it must never decide what goes into a build)."""
    d = os.path.join(cache_root(), NO_MODS)
    if not dry:
        if os.path.isdir(d) and os.listdir(d):
            remove_tree(d)
        os.makedirs(d, exist_ok=True)
    return os.path.abspath(d)


def in_pins(folder):
    """Whether a folder is (in) a pin's shared folder of the cache: its game folder's mods are the last kit build's."""
    pins = os.path.normcase(os.path.realpath(os.path.join(cache_root(), "pins")))
    f = os.path.normcase(os.path.realpath(folder))
    return f.startswith(pins + os.sep)


def build(pairs, build_dir, kit=None, game=None, jobs=0, keep=3, repo=REPO, dry=False, say=print, root=None,
          owner="hs-build"):
    """Game from the cache (or built once through a kit and stored), hookshot_coop.dll rebuilt, both into each pair."""
    root = root or main_root(repo)
    for p, how in sync_charsel_native(pairs, repo).items():
        if how:
            say("co-op's Characters tab's code patch %s of %s's pack (kit-paths.ps1 Sync-KitCharselNativePatch)"
                % ("put into" if how == "in" else "taken out", p))
    mods_line, mods = pairs_code_mods(pairs, repo)
    st = stamp(repo) + mods_line
    k = key_of(st)
    if mods:
        say("mods' code patches: the pairs' (%s) go into the game (HOS_MODS)" % mods)
    say("key %s (%s)" % (k, st.replace("\n", "; ")[:160]))
    hit = os.path.isfile(os.path.join(cache_root(), k, "stamp.txt"))
    if not dry and not lock_acquire(root, "hs", owner, wait=1800, say=say):
        return False
    game_lock = False
    try:
        if not hit:
            if not dry:
                # A game build takes the whole PC: no lane starts a game while it runs, and it waits for running ones.
                if not lock_acquire(root, "game", owner, wait=1800, say=say):
                    raise Fail("build-game.lock is held")
                game_lock = True
                for ln in LANE_LOCKS:
                    if not wait_free(os.path.join(root, LOCK_NAMES[ln]), 1800, say=say):
                        raise Fail("%s is still held after 30 min: a test lane is running" % LOCK_NAMES[ln])
            if not kit:
                raise Fail("cache miss for %s: pass --kit <a made kit> --game <your .nup/.xci> to build the game once,"
                           " or `cache store --from <a game folder built from this stamp>`" % k)
            script = os.path.join(kit, "scripts", "hookshot-build.ps1")
            takes = build_script_args(script)
            shared = pin_dir(st) if takes >= {"Tools", "Work"} else None
            # (the pin's one game folder, built again in place and kept; an old kit: a new folder, removed after)
            out = os.path.join(shared, "game") if shared else os.path.join(cache_root(), k + ".build")
            cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, "-Out", out]
            if jobs:   # (0: Hookshot's own choice, one compiler per CPU thread as the free memory allows)
                cmd += ["-Jobs", str(jobs)]
            if game:
                cmd += ["-Game", game]
            if shared:
                cmd += ["-Tools", os.path.join(shared, "tools"), "-Work", os.path.join(shared, "work")]
                # (a new pin: the game files hard-linked from an older pin's, not the .nup's ~26 GB again)
                other = romfs_source(out) if "RomfsFrom" in takes and game and game.lower().endswith(".nup") else None
                if other:
                    cmd += ["-RomfsFrom", other]
                    say("cache: the new pin's game files come from %s (hard links where the drive allows)" % other)
                say("cache: miss; building the pin's game folder %s again in place (only what changed since its last"
                    " build)" % out)
            else:
                say("cache: miss; this kit's hookshot-build.ps1 has no -Tools/-Work (a kit from before hs-build-speed):"
                    " a full build in the kit's own folders")
            # HOS_MODS always set (nx2/exefs_patches.py and Get-KitExefsStampLine read it): the pairs' mods, or an
            # empty folder. Unset, both fall back to the OUTPUT folder's own mods: the pin's game folder keeps the
            # puppet pack of its last kit build, with whatever code patch that build had (rejoin-host-273: a pair
            # without code mods got a game with co-op's Characters-tab patch from it, and the store was refused).
            env = dict(os.environ)
            env["HOS_MODS"] = os.path.abspath(mods) if mods else no_mods_dir(dry)
            if not run_steps([cmd], [], dry, say, env=env):
                raise Fail("hookshot-build.ps1 failed")
            if dry:
                say("cache: would store %s as %s" % (out, k))
            else:
                cache_store(out, repo, say=say, want=st)
                if not shared:
                    remove_tree(out)
        else:
            say("cache: hit %s" % k)
        prefix = [] if dry else msvc_prefix()
        if not run_steps(unit_steps(build_dir, jobs or os.cpu_count() or 8, repo, target="hookshot_coop"),
                         prefix, dry, say):
            raise Fail("hookshot_coop did not build")
        dll = find_dll(build_dir)
        for p in pairs:
            if not dry:
                os.makedirs(p, exist_ok=True)
            if hit or not dry:
                cache_restore(p, repo, dry=dry, say=say, want=st)
            else:
                say("cache: would copy the new game into %s" % p)
            exe = find_exe(p) if not dry else os.path.join(p, "bin", "longshot.exe")
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
            if game_lock:
                lock_release(root, "game", owner, say=say)
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
    c.add_argument("--pair", action="append", default=[],
                   help="store: the pairs whose game it is (their mods' code patches are in its key; repeat)")
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
    b.add_argument("--jobs", type=int, default=0,
                   help="game compilers at once; 0 (default): Hookshot's choice, one per CPU thread as memory allows")
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
                want = None
                if a.pair:
                    want = stamp(repo) + pairs_code_mods([os.path.abspath(p) for p in a.pair], repo)[0]
                cache_store(os.path.abspath(a.src), repo, a.force, a.dry_run, want=want)
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
