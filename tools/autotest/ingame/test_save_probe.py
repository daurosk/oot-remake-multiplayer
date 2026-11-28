"""PROBE: why did `--save no-ocarina` not show in game? (regress-port-c-4, songs-given-182cd53b 07:10 SETUP-FAIL; B #170 GO.)

songs-given-182cd53b: put_in read the profile back on all 6 slots of lane1-A/B's autotest-save and the lane start said
'save baseline restored' (steps.txt), yet in play game A read the Fairy Ocarina (5 flags 1, EquipOcarina text set).
Three suspects, told apart by ONE run (nothing here is judged; the product is <out>/save-probe.json + the notes):

  S1 the game loads a save from somewhere else than the file the profile patched: per game, every progress.sav the
     game folder holds (user/save/<id>/slot_NN, sibling user/save/<id>-coop/slot_NN, autotest-save/slot_NN) is read with
     save_flags.get for the profile's flags, next to the value in play. A file whose values equal the in-play ones is
     where the game got them ('source'); files that read the profile's values (0) while the game reads 1 mean the
     profile reached a file the game did not use.
  S2 something gives the ocarina after the load: every module log line since the game's start that is about a save,
     an item/give/gift or the ocarina, with its line number and the save-load line's number, so a give between the load
     and the first in-play frame shows (the boot script live-kakariko-child.A.txt is read too: its give/setflag lines).
  S3 the value is read too early / changes later: the flags are read in play, the pair stepped SECOND_AFTER frames, and
     read again ('changes' = they differ).

Values and sources, none guessed: the flags are testsave.load('no-ocarina') (saves/no-ocarina.py: save_flags.NO_OCARINA
+ EquipOcarina); the game folder and the save folder come from session.json (pair.read_session) and the module's own line
'coop: save: the game's save folder is ...' (game-out-X.txt:437 in songs-given-182cd53b-run); the load node's lines are
'coop: save: the game loaded a save (its own load nodes)' and 'coop: flags: save load seen'.
Ends Pair.unproven (NO-REF), never PASS: it is a probe.
"""
import glob
import hashlib
import json
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import save_flags as SF        # noqa: E402
import testsave                # noqa: E402

import pair as P               # noqa: E402
from test_songs import NO_OCARINA, ROW_ENV, ready_child   # noqa: E402  (the same boot/env as the failed run)

SECOND_AFTER = 120      # frames between the two in-play reads (2 s of game time)
SAVE_LINE = re.compile(r"coop: save: the game (starts loading|loaded) a save|coop: flags: save load seen")
FOLDER_LINE = re.compile(r"coop: save: the game's save folder is (.+)$")
ABOUT = re.compile(r"coop: .*(save|give|gift|item|ocarina|Ocarina|IsGet)", re.I)
SKIP = re.compile(r"vk:|vcores:|texture|traffic:")


def _expand(path):
    return os.path.expandvars(path.strip())


def _norm(kind, v):
    """A save word / an in-play value compared the same way: a Bool is its low byte (save_flags.set_values), None stays None."""
    if v is None:
        return None
    v = int(v)
    return (v & 0xff) if kind == SF.TYPES["bool"] else v


def _file_values(path, flags):
    """{hash: value or None} of the profile's flags in the progress.sav at `path` (save_flags.get)."""
    with open(path, "rb") as f:
        data = f.read()
    out = {}
    try:
        for kind, h, _want in flags:
            out["0x%08x" % h] = _norm(kind, SF.get(data, kind, h))
    except SF.SaveError as e:
        out = {"error": str(e)}
    return {"size": len(data), "sha8": hashlib.sha256(data).hexdigest()[:8], "mtime": os.path.getmtime(path),
            "values": out}


def _read_game(pair, x, flags, clear):
    r = {}
    for _kind, h, _want in flags:
        r["0x%08x" % h] = pair.flag_read(x, "0x%08x" % h).get("value")
    for h in clear:
        r["0x%08x" % h] = pair.flag_read(x, "0x%08x" % h).get("value")
    return r


@pytest.mark.budget(420)
@pytest.mark.needs_save(NO_OCARINA)
@pytest.mark.needs_env(*ROW_ENV)
@pytest.mark.row("PROBE --save no-ocarina: which save does the game load, and does the ocarina come back (not a row)")
def test_save_probe(pair):
    flags = testsave.load(NO_OCARINA)
    clear = testsave.clear_text_of(NO_OCARINA)
    with pair.setup():
        ready_child(pair)
    session = P.read_session(pair.session) if pair.session else {}
    rec = {"profile": NO_OCARINA, "flags": ["0x%08x" % h for _k, h, _w in flags],
           "clear_text": ["0x%08x" % h for h in clear], "second_after_frames": SECOND_AFTER, "games": {}}
    for x in pair.c:
        lines = pair.log[x].since(0)
        # the folder the game says it reads (S1): the module's own line, env vars expanded
        said = None
        for ln in lines:
            m = FOLDER_LINE.search(ln)
            if m:
                said = _expand(m.group(1))
        folder = (session.get(x) or {}).get("folder")
        g = {"game_folder": folder, "save_folder_said": said, "files": {}}
        roots = set()
        if said:
            roots.add(said)
            roots.add(said + "-coop")
        if folder:
            roots.add(os.path.join(folder, "user", "save"))
        cand = []
        for r in sorted(roots):
            cand += sorted(glob.glob(os.path.join(r, "**", "slot_*", "progress.sav"), recursive=True))
        if folder:
            cand += sorted(glob.glob(os.path.join(folder, "autotest-save", "slot_*", "progress.sav")))
        seen = set()
        for path in cand:
            if path not in seen:
                seen.add(path)
                g["files"][path] = _file_values(path, flags)
        g["in_play_1"] = _read_game(pair, x, flags, clear)
        # S2: the module's lines about a save / a give / the ocarina, with line numbers, and the load nodes' line numbers
        g["save_load_lines"] = [(i, ln[:200]) for i, ln in enumerate(lines) if SAVE_LINE.search(ln)]
        g["about_lines"] = [(i, ln[:220]) for i, ln in enumerate(lines)
                            if ABOUT.search(ln) and not SKIP.search(ln)][:300]
        rec["games"][x] = g
    pair.step_both(SECOND_AFTER)     # S3
    for x in pair.c:
        g = rec["games"][x]
        g["in_play_2"] = _read_game(pair, x, flags, clear)
        g["changes"] = {h: (g["in_play_1"][h], g["in_play_2"][h]) for h in g["in_play_1"]
                        if g["in_play_1"][h] != g["in_play_2"][h]}
        want = {"0x%08x" % h: _norm(k, 1 if k == SF.TYPES["bool"] and w else w) for k, h, w in flags}
        kinds = {"0x%08x" % h: k for k, h, _w in flags}
        g["match_files"] = [p for p, f in g["files"].items()
                            if all(f["values"].get(h) == _norm(kinds[h], g["in_play_1"][h]) for h in want)]
        g["profile_files"] = [p for p, f in g["files"].items()
                              if all(f["values"].get(h) == w for h, w in want.items())]
        pair.notes.append("%s: in play %s; files read as in play: %s; files read as the profile: %s; changes later: %s"
                          % (x, {h: v for h, v in g["in_play_1"].items() if h in want},
                             [os.path.relpath(p, g["game_folder"] or "") for p in g["match_files"]],
                             [os.path.relpath(p, g["game_folder"] or "") for p in g["profile_files"]],
                             g["changes"] or "none"))
    with open(os.path.join(pair.out_dir or ".", "save-probe.json"), "w", encoding="utf-8") as f:
        json.dump(rec, f, indent=1, default=str)
    pair.unproven("probe only: see save-probe.json (S1 files vs in play, S2 about_lines, S3 changes) and the notes")
