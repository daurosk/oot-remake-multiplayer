"""A lane game folder's co-op files put back to a known set at every boot, as its save is (hookshot.py restore_save).

Why (gate-tunic-309d7180, 0.12.0; harness-kept-difficulty 9aef275d7): the module reads these files once at start and a
player's choice made in a run (the owner's play pair on lane 1, a test's menu step) stays in them, so every later
automatic run on that lane played with it: lane1-A's coop-options.ini held look_cap_trim = blue and 10 more look parts,
public_game = on and a relay_room; lane1-B color = pink + look_cap_trim = orange; lane1-A/lane2-A a kept difficulty
(coop-difficulty.ini, Custom life 300%). The boot restored only the save.

The known set, per file (in the game folder: on Hookshot ConfigDir = GameDir, hookshot/compat/common/fs/path_util.h:13):
- coop-options.ini: the kit's own file as this repo ships it (tools/release/kit/coop-options.ini: the active lines
  auto_update, pvp, LICHII_COOP_PUPPET/OBJECTS/SPAWN). The module reads it beside longshot.exe (coop_title.cpp:1431) for
  the look parts (coop_look_live.cpp:786), color, every LICHII_COOP_* line, gold/race/floor switches, original_game,
  public_game/relay_room/max_players (the in-game Host). Lane 2's file had exactly the kit's active lines (11-27).
- coop.ini: only its name and client_id lines are kept (the lane game's identity: coop_pause_files.cpp:207-217 replaces
  a missing name with a random one; client_id coop_title.cpp:2092-2110). Every other key line goes: server/port/password
  (a leftover server starts co-op on an idle boot: coop_hookshot.cpp:2088-2098), last_invite/last_host/rejoin_once/
  join_lan (rejoin and join on an idle boot: coop_pause.cpp:634-719, 1610-1613), look_*/color, host_port.
- none (moved aside, never deleted): coop-player.ini + coop-kit.txt (a Nucleus instance: other options file,
  coop_title.cpp:1381-1417, coop_local_start_os.cpp:55), coop-difficulty.ini (coop_difficulty_game.cpp:58),
  coop-ngplus.ini (coop_ngplus.cpp:69), coop-saves.ini (saves = multiplayer bypasses the restored save,
  coop_save.cpp:108), coop-character.txt + coop-character-ages.txt (a kept Character Select pick; the first is
  only left by 0.12.0 and older kits since row 327, the second coop_hook.cpp CharAgesFile), coop-bans.txt (coop_pause.cpp:789), coop-test-state.txt (a stale state file test_regress_ui reads; the module
  writes it again with LICHII_COOP_TEST_STATE=1), gold-race\\ (a host's kept Gold Skulltula race, coop_gold_mode.h:92).
Not here, each with its own owner: mods\\ (coop-mods.list, coop-mods.settings.json, coop-one-time-keys.txt, the pack:
hookshot.py pack_start/seed_layout, lane-mods-check.ps1, testpack.py), the save (restore_save, testsave.py),
bug-reports\\ (LICHII_COOP_PROBLEM_ASK=0, TEST_GAME_DEFAULTS), player-key.txt (identity), exefs_patches.txt (the build),
coop-name-checked.txt (with the kept name), coop-start-merge.txt (log only).

A file is never written into (a lane file may be a hard link: testpack.py): a new file is put in its place. What is
moved aside goes to <kept>/<name>, beside the run's other output.
"""
import os
import re
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
KIT_OPTIONS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools", "release", "kit", "coop-options.ini")
OPTIONS = "coop-options.ini"
COOP_INI = "coop.ini"
COOP_INI_KEEP = ("name", "client_id")
NONE = ("coop-player.ini", "coop-kit.txt", "coop-difficulty.ini", "coop-ngplus.ini", "coop-saves.ini",
        "coop-character.txt", "coop-character-ages.txt", "coop-bans.txt", "coop-test-state.txt", "gold-race")
_KEY = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=")


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def _active(data):
    """The key lines of an ini text (no comments, no blank lines), as text lines."""
    text = data.decode("utf-8", errors="replace").lstrip("﻿")
    return [ln.strip() for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith(("#", ";"))]


def _put(path, data):
    tmp = path + ".lane-tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def _coop_ini(data):
    """coop.ini with only the kept key lines (comments kept): (new bytes, [dropped keys])."""
    text = data.decode("utf-8", errors="replace")
    keep, dropped = [], []
    for ln in text.splitlines(True):
        m = _KEY.match(ln)
        if m and not ln.lstrip().startswith(("#", ";")) and m.group(1) not in COOP_INI_KEEP:
            dropped.append(m.group(1))
            continue
        keep.append(ln)
    return "".join(keep).encode("utf-8"), dropped


def leftovers(game, kit_options=KIT_OPTIONS):
    """What restore() would change in `game`, changing nothing: a list of short texts (empty: the known set)."""
    out = []
    p = os.path.join(game, OPTIONS)
    try:
        kit = _active(_read(kit_options))
    except OSError:
        kit = None
    if os.path.isfile(p) and kit is not None:
        extra = [ln for ln in _active(_read(p)) if ln not in kit]
        if extra:
            out.append("%s +%d line(s): %s" % (OPTIONS, len(extra), ", ".join(extra)[:160]))
    elif not os.path.isfile(p):
        out.append("%s missing" % OPTIONS)
    c = os.path.join(game, COOP_INI)
    if os.path.isfile(c):
        _, dropped = _coop_ini(_read(c))
        if dropped:
            out.append("%s keys %s" % (COOP_INI, ", ".join(dropped)))
    out += ["%s present" % n for n in NONE if os.path.exists(os.path.join(game, n))]
    return out


def restore(game, kept, kit_options=KIT_OPTIONS):
    """Puts `game`'s co-op files back to the known set (module docstring). Returns one line for the run's steps; raises
    OSError when the kit's options file cannot be read or a file cannot be replaced or moved (the boot must not start
    on a lane it could not put back)."""
    kit = _read(kit_options)
    done = []
    p = os.path.join(game, OPTIONS)
    old = _read(p) if os.path.isfile(p) else None
    if old != kit:
        extra = [ln for ln in _active(old)] if old is not None else None
        extra = [ln for ln in extra if ln not in _active(kit)] if extra is not None else None
        if old is not None:
            os.makedirs(kept, exist_ok=True)
            _put(os.path.join(kept, OPTIONS), old)
        _put(p, kit)
        done.append("%s = the kit's (%s)" % (OPTIONS, "was missing" if extra is None else
                                             "was +%d line(s): %s" % (len(extra), ", ".join(extra)[:200])
                                             if extra else "comments only"))
    c = os.path.join(game, COOP_INI)
    if os.path.isfile(c):
        data = _read(c)
        new, dropped = _coop_ini(data)
        if dropped:
            os.makedirs(kept, exist_ok=True)
            _put(os.path.join(kept, COOP_INI), data)
            _put(c, new)
            done.append("%s without %s" % (COOP_INI, ", ".join(dropped)))
    for n in NONE:
        f = os.path.join(game, n)
        if os.path.exists(f):
            os.makedirs(kept, exist_ok=True)
            dst = os.path.join(kept, n)
            if os.path.isdir(dst):
                shutil.rmtree(dst)
            elif os.path.exists(dst):
                os.remove(dst)
            shutil.move(f, dst)
            done.append("%s moved aside" % n)
    if not done:
        return "lane files: the known set (nothing changed)"
    return "lane files: put back: %s (kept in %s)" % ("; ".join(done), kept)
