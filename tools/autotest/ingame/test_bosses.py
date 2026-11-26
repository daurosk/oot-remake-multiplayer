"""Boss rows in the in-game test system (docs/ingame-tests.md "Porting a .ps1 row").

| Test | Old row | Kind |
|---|---|---|
| test_phantom_ganon_hand_over_no_fall | hs-phantom-ganon-down (row 287; offline/pganon-cct-287-run.ps1) | log + world state |

Every log pattern is the old run script's own (coop-multiplayer offline/pganon-cct-287-run.ps1 lines 10-12 and
128-147), cited to the module line that writes it; every frame count is the old scenario's
(tools/autotest/scenarios/hs-phantom-ganon-down.A/B.txt).
"""
import re

import pytest

PG = "Enemy_DungeonBoss_PhantomGanon"
SCENE = "PhantomGanonStage"            # hs-phantom-ganon-down.A.txt 'warp PhantomGanonStage PhantomGanonStage'
FALL_BELOW_M = 40.0                    # lichii/hook/coop_boss_native.h Table::kFallBelowStart (the fall guard's line)

# The module's lines (the old judge's patterns, same text):
ASKED_OURS = r"coop: test verb %s 139: asked of our boss" % PG          # coop_objects.cpp QueueTestVerb
HANDED = r"HANDED OVER \(was "                                         # coop_move_probe.cpp (the probe's line)
FELL = r"below its fight start"                                        # coop_boss_native.cpp Table::Fall (2 lines)
CCT_IN = r"%s: CCT move state \d+ asked of the game: in \(" % PG       # coop_brain_state.cpp:341 (the fix's line)
CCT_NOT_IN = r"asked of the game: NOT in"                              # the same line, refused
CRASH = r"CRASH:|FATAL"                                                # the old judge's crash patterns


def boss(pair, x):
    """Phantom Ganon in game x's world list (bridge state 'world', docs/playtest.md "world"), or None."""
    for a in (pair.state(x).get("world") or {}).get("actors") or []:
        if a.get("name") == PG:
            return a
    return None


def dead(b):
    """Gone from the list, 'alive' false or life 0 (docs/playtest.md "world": alive, life)."""
    return b is None or b.get("alive") is False or b.get("life") == 0


def after_first(lines, first_rx, rx):
    """The lines matching rx after the first line matching first_rx (the old judge's 'after the first HANDED OVER')."""
    for i, ln in enumerate(lines):
        if re.search(first_rx, ln):
            return [m for m in lines[i + 1:] if re.search(rx, m)]
    return None


def turns(pair, x_verbs):
    """The old scenario's k1..k5: both games ask `verb PG 139` (only the owner's game runs it, the knock-down; the
    other sees it as a copy), 60 frames, then 5 x 300 frames so his turn can go to the other player."""
    for _ in range(5):
        for x in x_verbs:
            pair.act(x, "verb %s 139" % PG)
        pair.step_both(60)
        pair.step_both(1500)


@pytest.mark.budget(900)
@pytest.mark.needs_boot("live-phantom-ganon")
@pytest.mark.row("#287 Phantom Ganon falls through the floor after a hand-over (hs-phantom-ganon-down)")
def test_phantom_ganon_hand_over_no_fall(pair):
    with pair.setup():
        if (pair.info or {}).get("fake"):
            pair.warp_both(SCENE)               # (the fake lane has no boot scripts)
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
            assert s.get("scene") == SCENE, "%s is in %s, not %s (the boot live-phantom-ganon)" % (x, s.get("scene"), SCENE)
        start = {}
        for x in ("A", "B"):
            b = pair.until("%s sees %s" % (x, PG), lambda x=x: boss(pair, x), max_frames=120)
            start[x] = b["pos"][1]
    # the old scenario after its boot: lock on (press ZL 10 on each), 600 frames
    pair.press("A", ["ZL"], frames=10)
    pair.press("B", ["ZL"], frames=10)
    pair.step_both(600)
    # t1..t5: A hits 6 a turn, 600 frames each (phase 2, 42 -> 12)
    for _ in range(5):
        pair.act("A", "hit %s 6" % PG)
        pair.step_both(600)
    pair.call("A", "dump", tag="pg287-phase2")
    pair.call("B", "dump", tag="pg287-phase2")
    turns(pair, ("A", "B"))
    pair.call("A", "dump", tag="pg287-turns")
    pair.call("B", "dump", tag="pg287-turns")
    # h1: the host goes down, the server hands him to B; 600 frames; B asks the knock-down again (h2); 3 x 300 to h5
    pair.act("A", "hurt 80")
    pair.step_both(600)
    pair.act("B", "verb %s 139" % PG)
    pair.step_both(900)
    # him in the arena on both games after the hand-overs (the old shots h1..h5: now by state)
    for x in ("A", "B"):
        b = boss(pair, x)
        assert b is not None and b.get("alive", True), "%s: %s not in the world list after the hand-over" % (x, PG)
        assert b["pos"][1] > start[x] - FALL_BELOW_M, "%s: %s at y %.1f, %.0f m below his fight start %.1f" % (
            x, PG, b["pos"][1], start[x] - b["pos"][1], start[x])
    # d1: B kills him; both games see him dead (1800 + 900 frames, the old waits)
    pair.act("B", "kill %s" % PG)
    pair.until("%s dead on both games" % PG, lambda: all(dead(boss(pair, x)) for x in ("A", "B")), max_frames=2700)
    pair.call("A", "dump", tag="pg287-dead")
    pair.call("B", "dump", tag="pg287-dead")

    # the old judge, per game (offline/pganon-cct-287-run.ps1:135-147)
    lines = {x: pair.log[x].since() for x in ("A", "B")}
    for x in ("A", "B"):
        assert any(re.search(ASKED_OURS, ln) for ln in lines[x]), \
            "%s: no '%s' line: no knock-down was played on this game (old: UNPROVEN)" % (x, ASKED_OURS)
        falls = after_first(lines[x], HANDED, FELL)
        assert not falls, "%s: %d fall line(s) after the first hand-over: %s" % (x, len(falls), falls[0])
        pair.never_log(x, CCT_NOT_IN)
        pair.never_log(x, CRASH)
    # the fix's own line after a hand-over, on either game (old: down UNPROVEN without it)
    seen = [after_first(lines[x], HANDED, CCT_IN) for x in ("A", "B")]
    assert any(seen), "no '%s' line after a HANDED OVER on either game: the fix's move-state write did not run" % CCT_IN
