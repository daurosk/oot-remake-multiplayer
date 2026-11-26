"""Save profiles for one boot of the in-game tests (bt_plugin --save NAME, marker needs_save(NAME)): flags set in BOTH
lane games' save baseline (<game>/autotest-save/slot_NN/progress.sav, which hookshot.py restore_save copies over the
game's saves at its start) before live.py start, and the baseline put back after live.py stop, so the next job finds
the lane as it was.

A profile is ingame/saves/NAME.py (or $BT_SAVES_DIR/NAME.py) with

    FLAGS = ["enum:0x7d776b61=0xb5df43cb", "bool:0x6b093b51=0", ...]   # save_flags.py set's form, each cited
    CLEAR_TEXT = [0xb72cd3fc]   # optional: String64 flags emptied to "" (the 64 bytes of text zeroed), each cited


Only flags already in the save are changed (save_flags.set_values; a missing one is a SETUP-FAIL). Per game folder:
<game>/coop-test-save.json names the profile and the files. It is written BEFORE any save changes, so a run that dies
at any point leaves a manifest the next boot undoes (undo() at every lane_pair start). Each save is copied to
<game>/coop-test-save-kept/<rel> first, then replaced whole (a temp file and os.replace, never written into). Each
flag is read back on every save (SETUP-FAIL on a miss).
"""
import hashlib
import importlib.util
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import save_flags as SF  # noqa: E402

SAVES = os.path.join(HERE, "saves")
BASE = "autotest-save"
MANIFEST = "coop-test-save.json"
KEPT = "coop-test-save-kept"


class SaveProfileError(Exception):
    """A profile could not be read or put in (the boot's set-up: SETUP-FAIL)."""


def profile_path(name, saves_dir=None):
    return os.path.join(saves_dir or os.environ.get("BT_SAVES_DIR") or SAVES, name + ".py")


def load(name, saves_dir=None):
    """[(kind, hash, value)] of profile NAME."""
    if not name or not all(c.isalnum() or c in "-_" for c in name):
        raise SaveProfileError("--save %r: a profile name (ingame/saves/NAME.py)" % name)
    path = profile_path(name, saves_dir)
    if not os.path.isfile(path):
        raise SaveProfileError("--save %s: no %s" % (name, path))
    spec = importlib.util.spec_from_file_location("btsave_" + name.replace("-", "_"), path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    flags = getattr(mod, "FLAGS", None)
    if not flags or not isinstance(flags, (list, tuple)):
        raise SaveProfileError("--save %s: %s has no FLAGS list" % (name, path))
    out = []
    for f in flags:
        try:
            kind, h, value = SF.parse_flag(f)
        except (SF.SaveError, ValueError) as e:
            raise SaveProfileError("--save %s: %r: %s" % (name, f, e))
        if value is None:
            raise SaveProfileError("--save %s: %r has no value (type:hash=value)" % (name, f))
        out.append((kind, h, value))
    return out


def clear_text_of(name, saves_dir=None):
    """Profile NAME's CLEAR_TEXT: the String64 hashes whose text it empties ([] when it has none)."""
    load(name, saves_dir)                               # (its own checks: name, file, FLAGS)
    spec = importlib.util.spec_from_file_location("btsave_t_" + name.replace("-", "_"), profile_path(name, saves_dir))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    raw = getattr(mod, "CLEAR_TEXT", None) or []
    if not isinstance(raw, (list, tuple)) or not all(isinstance(h, int) and 0 < h <= 0xffffffff for h in raw):
        raise SaveProfileError("--save %s: CLEAR_TEXT is not a list of String64 hashes" % name)
    return list(raw)


def saves_of(folder):
    """The baseline's progress.sav files, relative to the game folder ('/' separated)."""
    base = os.path.join(folder, BASE)
    if not os.path.isdir(base):
        return []
    out = []
    for d in sorted(os.listdir(base)):
        if SF.SLOT.match(d) and os.path.isfile(os.path.join(base, d, "progress.sav")):
            out.append("%s/%s/progress.sav" % (BASE, d))
    return out


def _p(folder, rel):
    return os.path.join(folder, *rel.split("/"))


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def put_in(folders, name, changes, say=print, clear=None, saves_dir=None):
    """Profile NAME's flags into every save of each game's baseline. {folder name: {rel: sha256}} once every flag
    reads back on every save. `clear`: the String64 hashes to empty; default: profile NAME's CLEAR_TEXT, read from
    `saves_dir` (else BT_SAVES_DIR / saves/, where bt_plugin's --save finds it: bt_plugin passes neither) when the
    profile is there, else none (a caller that loaded `changes` from elsewhere passes its own)."""
    want = {(k, h): (v if k != SF.TYPES["bool"] else (1 if v else 0)) for k, h, v in changes}
    if clear is None:
        clear = clear_text_of(name, saves_dir) if os.path.isfile(profile_path(name, saves_dir)) else []
    got = {}
    for folder in folders:
        rels = saves_of(folder)
        if not rels:
            raise SaveProfileError("%s: no %s/slot_NN/progress.sav to set --save %s in" % (folder, BASE, name))
        if os.path.exists(os.path.join(folder, MANIFEST)):
            raise SaveProfileError("%s: a save profile is still in (%s): undo it first" % (folder, MANIFEST))
        with open(os.path.join(folder, MANIFEST), "w", encoding="utf-8") as f:
            json.dump({"profile": name, "files": rels}, f, indent=1)   # (first: undo() knows what to put back)
        for rel in rels:
            src, kept = _p(folder, rel), _p(os.path.join(folder, KEPT), rel)
            old = SF.read_file(src)
            data = bytearray(old)
            try:
                SF.set_values(data, changes)
                for h in clear:
                    at = SF.string64_text_at(data, h)
                    data[at:at + 64] = bytes(64)
                SF.index(bytes(data))
            except SF.SaveError as e:
                raise SaveProfileError("%s: --save %s: %s" % (src, name, e))
            os.makedirs(os.path.dirname(kept), exist_ok=True)
            if not os.path.exists(kept):            # (a kept copy already there is the true baseline: never replaced)
                shutil.copyfile(src, kept)
            tmp = src + ".tmp-test-save"
            with open(tmp, "wb") as f:
                f.write(bytes(data))
            os.replace(tmp, src)
        say("save: %s into %s (%d save(s))" % (name, folder, len(rels)))
    for folder in folders:
        for rel in saves_of(folder):
            data = SF.read_file(_p(folder, rel))
            for (k, h), v in want.items():
                have = SF.get(data, k, h)
                if have is None or (have & 0xff if k == SF.TYPES["bool"] else have) != v:
                    raise SaveProfileError("%s %s: flag %#010x is %s, not %s after --save %s"
                                           % (folder, rel, h, have, v, name))
            for h in clear:
                try:
                    at = SF.string64_text_at(data, h)
                except SF.SaveError as e:
                    raise SaveProfileError("%s %s: --save %s: %s" % (folder, rel, name, e))
                if any(data[at:at + 64]):
                    raise SaveProfileError("%s %s: String64 %#010x is not empty after --save %s" % (folder, rel, h, name))
            got.setdefault(os.path.basename(folder), {})[rel] = sha256(_p(folder, rel))
    return got


def undo(folder, say=print):
    """The baseline back in a game folder (each kept save over the profile's); [] when none was in. Safe to run again
    after a partial undo."""
    mf = os.path.join(folder, MANIFEST)
    if not os.path.isfile(mf):
        return []
    try:
        with open(mf, encoding="utf-8") as f:
            plan = json.load(f)
    except (OSError, ValueError) as e:
        raise SaveProfileError("%s: %s unreadable (%s): put %s back by hand from %s" % (folder, MANIFEST, e, BASE, KEPT))
    done = []
    for rel in plan.get("files", []):
        kept, dst = _p(os.path.join(folder, KEPT), rel), _p(folder, rel)
        if os.path.isfile(kept):
            os.replace(kept, dst)
            done.append(rel)
    shutil.rmtree(os.path.join(folder, KEPT), ignore_errors=True)
    os.remove(mf)
    say("save: %s out of %s (%d put back)" % (plan.get("profile"), folder, len(done)))
    return done


# -- the setup proof in the game (port-g3-save, B #136 brief step 1) -------------------------------------------------
# A profile may also say what a game that LOADED it must show (optional; every key is checked only when given):
#
#     EXPECT = {"age": "child", "scene": "InsideTheDekuTree", "items": ["Weapon_Sword_Kokiri"], "max_life": 12}
#
# prove() reads it in a test's set-up (inside pair.setup(), so a miss is SETUP-FAIL, never FAIL): each game in play,
# its age / scene / max_life from the bridge state, each item's IsGet.<item> flag set (flag_read), and every FLAGS
# entry read back from the game by its hash (so the game runs the profile's save, not the bare baseline: the profile
# is the only writer of those values when the boot sets none of them). A per-game override: EXPECT = {"A": {...},
# "B": {...}} (keys of the top level apply to both, a game's own keys win).

EXPECT_KEYS = ("age", "scene", "items", "max_life")


def expect_of(name, saves_dir=None):
    """Profile NAME's EXPECT as {game: {key: value}} for A and B ({} each when the profile has none)."""
    load(name, saves_dir)                               # (its own checks: name, file, FLAGS)
    path = profile_path(name, saves_dir)
    spec = importlib.util.spec_from_file_location("btsave_x_" + name.replace("-", "_"), path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    raw = getattr(mod, "EXPECT", None) or {}
    if not isinstance(raw, dict):
        raise SaveProfileError("--save %s: EXPECT is not a dict" % name)
    both = {k: v for k, v in raw.items() if k not in ("A", "B")}
    out = {}
    for x in ("A", "B"):
        own = raw.get(x) or {}
        if not isinstance(own, dict):
            raise SaveProfileError("--save %s: EXPECT[%r] is not a dict" % (name, x))
        out[x] = dict(both, **own)
        bad = sorted(set(out[x]) - set(EXPECT_KEYS))
        if bad:
            raise SaveProfileError("--save %s: EXPECT key(s) %s not one of %s" % (name, ", ".join(bad), EXPECT_KEYS))
    return out


def _same(kind, want, have):
    """A FLAGS value against a bridge flag_read value (an enum as '0x' + 8 hex digits, a bool true/false or 0/1)."""
    if have is None:
        return False
    if kind == SF.TYPES["bool"]:
        return bool(have) == bool(want)
    if isinstance(have, str):
        try:
            have = int(have, 0)
        except ValueError:
            return False
    return int(have) == int(want)


def prove(pair, name, games=None, saves_dir=None):
    """The in-game proof that games `games` (default all of the pair) run save profile NAME: raises pair.SetupFail
    naming every miss of every game; returns the notes it appended to pair.notes."""
    import pair as P
    try:
        flags = load(name, saves_dir)
        expect = expect_of(name, saves_dir)
        clear = clear_text_of(name, saves_dir)
    except SaveProfileError as e:
        raise P.SetupFail(str(e)) from e
    miss, notes = [], []
    for x in (games or list(pair.c)):
        s = pair.state(x)
        if not pair.in_play(s):
            miss.append("%s not in play (%s)" % (x, s.get("not_ready") or s.get("scene")))
            continue
        e = expect.get(x, {})
        if "age" in e and s.get("age") != e["age"]:
            miss.append("%s age %s, not %s" % (x, s.get("age"), e["age"]))
        if "scene" in e and (s.get("scene") or "").lower() != e["scene"].lower():
            miss.append("%s in %s, not %s" % (x, s.get("scene"), e["scene"]))
        if "max_life" in e and s.get("max_life") != e["max_life"]:
            miss.append("%s max_life %s, not %s" % (x, s.get("max_life"), e["max_life"]))
        for item in e.get("items") or []:
            v = pair.flag_read(x, "IsGet." + item).get("value")
            if not v:
                miss.append("%s lacks %s (IsGet.%s = %s)" % (x, item, item, v))
        for kind, h, want in flags:
            v = pair.flag_read(x, "0x%08x" % h).get("value")
            if not _same(kind, want, v):
                miss.append("%s flag 0x%08x = %s, not %s (the profile's save not loaded?)" % (x, h, v, want))
        for h in clear:                                 # (flag_read gives a String64 as kind 'text')
            v = pair.flag_read(x, "0x%08x" % h).get("value")
            if v != "":
                miss.append("%s String64 0x%08x = %r, not '' (the profile's save not loaded?)" % (x, h, v))
        notes.append("save %s proven on %s (%d flag(s)%s)" % (
            name, x, len(flags), "".join(", %s" % k for k in EXPECT_KEYS if k in e)))
    if miss:
        raise P.SetupFail("--save %s: %s" % (name, "; ".join(miss)))
    pair.notes.extend(notes)
    return notes
