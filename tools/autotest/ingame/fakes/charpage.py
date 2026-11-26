"""Rows 288/207 (test_charpage.py) on the fake games: the - menu's game tabs and the Characters page hold.

Reacts only on game A with the boot switch A:OOT_TEST_SEED_LAYOUT=1.
- setup: a game folder as a lane's (romfs/UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs, as in
  coop-kit/ls/lane1-A; mods/coop-mods.list as lane1-A's: header + randomizer + Character Select rows), then the REAL
  hookshot.py seed_layout (so the selftests cover it too). The module's start line: SEED_WINS (coop_rando.cpp
  Rando::Start), or with A:OOT_COOP_START_MERGE=1 + A:OOT_COOP_RESTART=1 patch 0011's MERGING + MERGED lines and no
  SEED_WINS (the merge carried the layouts).
- press: MINUS opens the - menu on Map (2) / closes it; L: Map 2 -> Threads of Time 1 -> Key Items 6 -> Characters;
  R back (coop_pause_tab.h GameMinusCategory). L/ZL on Key Items with the seed unmerged: held (HELD line, once a run,
  as SayCharactersHeld), unless BT_FAKE_CHARPAGE=nohold (the bug: the page opens without its layouts = the crash: the
  FATAL line, then every bridge call fails).
- BT_FAKE_CHARPAGE=noseed: seed_layout is not run (a lane where the switch did nothing: the set-up's SETUP-FAIL).
"""
import os
import sys

_AUTOTEST = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_STATE = {}   # game -> {"open", "cat", "held_said", "crashed", "seed", "merged"}

SEED_WINS = ("coop: randomizer: the game reads the menus' layout archive (UI/Layout2Archive/Common) of the mod "
             "zz-test-seed-layout, without the Characters page's layouts (no _coop-archives stamp from the kit's start "
             "for it): opening the page would crash the game; it is held this run")
HELD = ("coop: pause tab: the Characters page is held (L on the tab left of it): the menus' layout archive the game "
        "reads has not its layouts (see the randomizer line at the start), opening it would crash the game")


def _on(game):
    return game.tag == "A" and game.env_on("OOT_TEST_SEED_LAYOUT=1")


def setup(game):
    if not _on(game):
        return
    how = os.environ.get("BT_FAKE_CHARPAGE", "")
    lay = os.path.join(game.folder, "romfs", "UI", "Layout2Archive")
    os.makedirs(lay, exist_ok=True)
    with open(os.path.join(lay, "Common.Product.100.Nin_Ounce_NVN2.blarc.zs"), "wb") as f:
        f.write(b"fake layout archive")
    os.makedirs(os.path.join(game.folder, "mods"), exist_ok=True)
    with open(os.path.join(game.folder, "mods", "coop-mods.list"), "w", encoding="utf-8", newline="\n") as f:
        f.write("coop-mods 1\non oot-remake-randomizer\non OoT Character Select\n")
    if how != "noseed":
        if _AUTOTEST not in sys.path:
            sys.path.insert(0, _AUTOTEST)
        import hookshot
        hookshot.seed_layout(game.folder, {"OOT_TEST_SEED_LAYOUT": "1"}, lambda s: None)
    merged = game.env_on("OOT_COOP_START_MERGE=1") and game.env_on("OOT_COOP_RESTART=1")
    if merged:
        game.say("[hos host] mods: the game was started again by the Insert menu's Restart now: merging the mods' "
                 "shared files as the kit's start does (fake, at most 180 s)")
        game.say("[hos host] mods: the merge ended with 0 after 900 ms")
    elif how != "noseed":
        game.say(SEED_WINS)
    _STATE[game] = {"open": False, "cat": 2, "held_said": False, "crashed": False, "merged": merged}


def press(game, buttons):
    s = _STATE.get(game)
    if s is None:
        return False
    if "MINUS" in buttons:
        s["open"], s["cat"] = not s["open"], 2
        return True
    if not s["open"]:
        return False
    left = "L" in buttons or "ZL" in buttons
    if left:
        if s["cat"] == 2:
            s["cat"] = 1
        elif s["cat"] == 1:
            s["cat"] = 6
        elif s["cat"] == 6:
            if s["merged"]:
                s["cat"] = 0          # (the Characters page, whole)
            elif os.environ.get("BT_FAKE_CHARPAGE", "") == "nohold":
                game.say("[hos host] FATAL: the game aborted (fake: guest read 0x58 in the Characters page's layout)")
                s["crashed"] = True
            elif not s["held_said"]:
                s["held_said"] = True
                game.say(HELD)
        return True
    if "R" in buttons or "ZR" in buttons:
        s["cat"] = {6: 1, 1: 2, 0: 6}.get(s["cat"], s["cat"])
        return True
    return False


def command(game, m):
    s = _STATE.get(game)
    if s is not None and s["crashed"]:
        return {"id": m.get("id"), "ok": False, "error": "fake: the game aborted"}
    return None
