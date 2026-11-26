"""Rows 288 + 207: the Characters page ("skin menu") after a randomizer seed made in the game must not crash the game.

Player reports (coord reports/player-reports.md): row 288 (mr.bons4i, v0.11.12, logs) "crashing whenever I ... access
the characters section"; row 207 (proyectskye, no logs) "game crashes when going to the skin menu"; the same player
made randomizer seeds in the game (rows 171/179). Root cause (reports/charpage-288.md): a seed made in the game is a
new mod at the top of the Mods list whose menus' layout archive (romfs/UI/Layout2Archive/Common.Product.*.blarc.zs,
the game's own) wins without Character Select's layouts when the game starts without the kit's merge (Insert menu
Restart now, longshot.exe); the page then reads a layout that is not there (guest null read 0x58).

This replaces the parked tools/autotest/proof-charpage-288.ps1 (+ scenarios/charpage-288-hold / -open):

| Old check | Here |
|---|---|
| the seed mod zz-row288-seed made in game A, no merge (OOT_TEST_NO_MERGE=1) | --env A:OOT_TEST_SEED_LAYOUT=1 A:OOT_TEST_NO_MERGE=1 (hookshot.py seed_layout: also its first row of coop-mods.list, which the old script lacked: an unlisted mod ranks under every listed one, hos_coop_mods.h hos_coop_mods_rank = why its run was VOID) |
| log "randomizer: the game reads the menus' layout archive" | set-up proof, the same line (coop_rando.cpp Rando::Start), since the boot |
| press MINUS, L x2 -> Key Items, L, L, ZL kept | the same keys and waits (150 frames each), through the bridge's input |
| log "coop: pause tab: the Characters page is held" | the same line (coop_pause_tab_module.cpp SayCharactersHeld) |
| no crash (FATAL: the game aborted) | the game answers in play after R + MINUS; never_log FATAL |
| shots held-1..3, threads-again, field | bridge shots charpage-held-1/-3 (A, the Key Items tab + chat line), charpage-field (A and B) |
| -Mode merge (OOT_COOP_START_MERGE=1, OOT_COOP_RESTART=1): merge log + page opens | test_characters_page_merged_at_restart (needs the kit's tools above the lane game: SETUP-FAIL names what is missing) |

The hold's line is said once a run (SayCharactersHeld's `said`), so a --repeat's later rounds judge it since the boot;
every round still proves no crash at the held L, which is the row.
"""
import os

import pytest

# The switches (hookshot.py seed_layout / pack_start; patch 0011 hos_coop_mods.h HOS_COOP_MODS_START_MERGE_ENV,
# HOS_COOP_MODS_RESTART_ENV): A only, B idles as in the old scenario.
SEED_ENV = ("A:OOT_TEST_SEED_LAYOUT=1", "A:OOT_TEST_NO_MERGE=1")
MERGE_ENV = SEED_ENV + ("A:OOT_COOP_START_MERGE=1", "A:OOT_COOP_RESTART=1")
SEED_MOD = "zz-test-seed-layout"   # (hookshot.py SEED_MOD)
WAIT = 150                          # (scenarios/charpage-288-hold.A.txt: 'wait 150' after every key)

SEED_WINS = r"randomizer: the game reads the menus' layout archive \(UI/Layout2Archive/Common\) of the mod %s" % SEED_MOD
HELD = r"coop: pause tab: the Characters page is held \("
FATAL = r"FATAL: the game aborted"
# patch 0011 host/mods.c coop_start_merge's lines
MERGING = r"mods: the game was started again by the Insert menu's Restart now: merging the mods' shared files"
MERGED = r"mods: the merge ended with 0 after \d+ ms"
KIT_UP = 5   # (hos_coop_mods.h HOS_COOP_MODS_KIT_UP)


def seed_in(pair):
    """Set-up proof that game A runs with the test seed's archive over Character Select's, and B without it."""
    # (steps to a fresh in-play sample: a held game's first state right after the boot has no Link sample yet, the
    # gifts-237 SETUP-FAIL 2026-11-26; this file may run alone with -k, without the smoke tier before it)
    pair.until_in_play("both games in play")
    a_mods = os.path.join(pair.games["A"]["folder"], "mods")
    assert os.path.isdir(os.path.join(a_mods, SEED_MOD)), "A: no %s (hookshot.py seed_layout did not run)" % SEED_MOD
    with open(os.path.join(a_mods, "coop-mods.list"), encoding="utf-8", errors="replace") as f:
        rows = f.read().splitlines()
    assert rows[1:2] == ["on " + SEED_MOD], "A: coop-mods.list's first row is %r, not the seed's" % rows[1:2]
    assert not os.path.isdir(os.path.join(pair.games["B"]["folder"], "mods", SEED_MOD)), "B has the test seed too"


def to_key_items_then_left(pair):
    """MINUS (the - menu opens on Map), L (Threads of Time), L (Key Items), then L: the step onto the Characters page
    (charpage-288-hold.A.txt's path; categories Map 2, Threads 1, Key Items 6: coop_pause_tab.h GameMinusCategory)."""
    pair.press("A", ["MINUS"])
    pair.step_both(WAIT)
    for _ in range(3):
        pair.press("A", ["L"])
        pair.step_both(WAIT)


def leave_menu(pair):
    pair.press("A", ["R"])          # (Key Items -> Threads of Time: the game's own keys still work)
    pair.step_both(WAIT)
    pair.check_shot("A", "charpage-threads-again", tolerance=0.85)   # (old shot threads-again: R went through)
    pair.press("A", ["MINUS"])
    pair.step_both(180)             # (charpage-288-hold.A.txt: 'wait 180' after the closing MINUS)
    pair.until("A back in play after the - menu", lambda: pair.in_play(pair.state("A")), max_frames=300)


@pytest.mark.budget(300)
@pytest.mark.needs_env(*SEED_ENV)
@pytest.mark.look
@pytest.mark.row("#288/#207 Characters page held after a seed made in the game (charpage-288-hold)")
def test_characters_page_held(pair):
    with pair.setup():
        seed_in(pair)
        assert pair.log["A"].find(SEED_WINS, pos=0), (
            "A: the module did not see the seed's archive win (no '%s' since the boot)" % SEED_WINS)
    to_key_items_then_left(pair)
    assert pair.log["A"].find(HELD, pos=0), "A: L on Key Items was not held (no '%s'): the page was asked for" % HELD
    pair.check_shot("A", "charpage-held-1", tolerance=0.85)   # (still UI + the chat line: 0.85)
    for key in (["L"], ["ZL"]):    # (old held-2, held-3: both left keys stay kept)
        pair.press("A", key)
        pair.step_both(WAIT)
    pair.check_shot("A", "charpage-held-3", tolerance=0.85)
    leave_menu(pair)
    pair.never_log("A", FATAL)
    for x in ("A", "B"):
        pair.check_shot(x, "charpage-field", tolerance=0.80)   # (Link in the field, moving water/grass: 0.80)


def kit_above(folder):
    """The kit root the game's merge looks for (patch 0011 coop_start_merge: tools/puppet_pack/merge_tables.py up to
    KIT_UP folders above the game, then <kit>/hookshot/tools/python/python.exe), or the reason there is none."""
    d = os.path.abspath(folder)
    for _ in range(KIT_UP + 1):
        if os.path.isfile(os.path.join(d, "tools", "puppet_pack", "merge_tables.py")):
            py = os.path.join(d, "hookshot", "tools", "python", "python.exe")
            return (d, None) if os.path.isfile(py) else (None, "no %s" % py)
        d = os.path.dirname(d)
    return None, "no tools/puppet_pack/merge_tables.py in %s or %d folders above" % (folder, KIT_UP)


@pytest.mark.budget(300)
@pytest.mark.needs_env(*MERGE_ENV)
@pytest.mark.look
@pytest.mark.row("#288/#207 Restart now merges a seed's menus: the Characters page opens (charpage-288-open merge)")
def test_characters_page_merged_at_restart(pair):
    with pair.setup():
        seed_in(pair)
        kit, why = kit_above(pair.games["A"]["folder"])
        assert kit, "A: the game's merge has no kit to run (%s)" % why
        assert pair.log["A"].find(MERGING, pos=0), "A: no '%s' at the start" % MERGING
    assert pair.log["A"].find(MERGED, pos=0), "A: the start's merge did not end with 0 (no '%s')" % MERGED
    assert not pair.log["A"].find(SEED_WINS, pos=0), "A: the seed's archive still wins unmerged after the merge"
    to_key_items_then_left(pair)
    pair.step_both(240 - WAIT)      # (charpage-288-open.A.txt: 'wait 240' on the page; WAIT of it already ran)
    pair.never_log("A", HELD)
    pair.never_log("A", FATAL)
    pair.check_shot("A", "charpage-page", tolerance=0.85)   # (the whole Characters page: portrait, names)
    pair.press("A", ["MINUS"])
    pair.step_both(180)
    pair.until("A back in play after the - menu", lambda: pair.in_play(pair.state("A")), max_frames=300)
    for x in ("A", "B"):
        pair.check_shot(x, "charpage-field-merged", tolerance=0.80)

