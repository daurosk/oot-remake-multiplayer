"""Boss rows in the in-game test system (docs/ingame-tests.md "Porting a .ps1 row").

| Test | Old row | Kind |
|---|---|---|
| test_phantom_ganon_hand_over_no_fall | hs-phantom-ganon-down (row 287; offline/pganon-cct-287-run.ps1) | log + world state + LOOK |

Every log pattern is the old run script's own (coop-multiplayer offline/pganon-cct-287-run.ps1 lines 10-12 and
128-147), cited to the module line that writes it; every frame count is the old scenario's
(tools/autotest/scenarios/hs-phantom-ganon-down.A/B.txt).

Cause (phantom-ganon-287-3): a game that takes him over shows, through the move probe (LICHII_COOP_MOVE_PROBE=1 in the
boot's '#! switches', live-phantom-ganon.A.txt:8), that its copy stood in another CCT move state than the owner's when
it was made ours (the knock-down left it there), and the fix's line asking the owner's state.
Shots (the old row's k1..k5, h1, h2, d1 on both games: players see him): paired framebuffer shots through
test_boss_shots.Shots (tolerance 0.80, a moving scene), compared to refs/; the first run gives NO-REF candidates.
"""
import os
import re
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pair as P                     # noqa: E402  (P.SetupFail: the knock-down's landing is set-up proof)
from test_boss_shots import Shots   # noqa: E402  (paired shots on both games, look misses asserted at the end)

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
# coop_move_probe.cpp Watch::Step (lines 113-128): "move probe: <name> 0x<actor> ours|a copy of player N: [HANDED OVER
# (was ..)] move state C (type T, before B)" or "... move state P (..) -> C (..)"; group 3 = C, the state it is in now.
PROBE = re.compile(r"move probe: %s 0x[0-9a-f]+ (ours|a copy of player \d+):(.*?) move state "
                   r"(?:\d+ \(type \d+, before \d+\) -> )?(\d+) \(type" % PG)
VERB_ANY = r"coop: test verb %s 139: " % PG    # coop_objects.cpp:7248 (either answer: ours or a copy's 'no boss')
CCT_ASKED = re.compile(r"%s: CCT move state (\d+) asked of the game: in \(" % PG)   # coop_brain_state.cpp:341
LANDED_ATTACK = r"%s 0x[0-9a-f]+ attack starts \(action verbs 139\b" % PG   # coop_objects.cpp:388
LANDED_MOVE = r"move probe: %s 0x[0-9a-f]+ ours:.* -> 1 \(type" % PG         # coop_move_probe.cpp Watch::Step


def cause(lines):
    """One game's cause, or None when this game never took him over with the fix asking: (the copy's move state, the
    owner's state the fix asked). The copy's state = its last probe line as a copy before the first fix line, read
    only after this game's first `verb PG 139` (the knock-down turns; an intro copy state is never the cause)."""
    copy, turns_on = None, False
    for ln in lines:
        turns_on = turns_on or bool(re.search(VERB_ANY, ln))
        m = PROBE.search(ln)
        if m and turns_on and m.group(1) != "ours":
            copy = int(m.group(3))
        a = CCT_ASKED.search(ln)
        if a:
            return copy, int(a.group(1))
    return None


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


def landed(lines):
    """The knock-down's start on the owner's game: 'attack starts (action verbs 139' (coop_objects.cpp:388) or the move
    probe's '-> 1' for him (coop_move_probe.cpp Watch::Step). pganon-287-182cd53b had neither after any of 6 asks."""
    return [ln for ln in lines if re.search(LANDED_ATTACK, ln) or re.search(LANDED_MOVE, ln)]


def turns(pair, x_verbs, shots):
    """The old scenario's k1..k5: both games ask `verb PG 139` (only the owner's game runs it, the knock-down; the
    other sees it as a copy), 60 frames, then 5 x 300 frames so his turn can go to the other player; then shot kN.
    SETUP-FAIL when no ask of k1..k5 started the knock-down on its owner's game within its 60 frames (NO REPEAT VOIDS,
    pganon-287-182cd53b: his CCT stayed 2, so no hand-over could show the bug's condition)."""
    seen = []
    for k in range(1, 6):
        pos = {x: pair.log[x].mark() for x in ("A", "B")}
        for x in x_verbs:
            pair.act(x, "verb %s 139" % PG)
        pair.step_both(60)
        for x in ("A", "B"):
            lines = pair.log[x].since(pos[x])
            if any(re.search(ASKED_OURS, ln) for ln in lines):
                seen += landed(lines)
        pair.step_both(1500)
        shots.both("k%d" % k)
    if not seen:
        raise P.SetupFail("knock-down did not land: verb 139 not started (CCT stayed 2) on the owner's game after "
                          "k1..k5 (no '%s' or '%s' line within 60 frames of an ask)" % (LANDED_ATTACK, LANDED_MOVE))


@pytest.mark.budget(900)
@pytest.mark.look
@pytest.mark.needs_boot("live-phantom-ganon")
@pytest.mark.row("#287 Phantom Ganon falls through the floor after a hand-over (hs-phantom-ganon-down)")
def test_phantom_ganon_hand_over_no_fall(pair):
    shots = Shots(pair, "pg287")
    with pair.setup():
        if (pair.info or {}).get("fake"):
            pair.warp_both(SCENE)               # (the fake lane has no boot scripts)
        # stepped, never one read (B #139 16:31, the gifts-237/row53 class): both in play in the boot's scene
        st = pair.need_boot_place()
        for x in ("A", "B"):
            s = st[x]
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
    turns(pair, ("A", "B"), shots)
    pair.call("A", "dump", tag="pg287-turns")
    pair.call("B", "dump", tag="pg287-turns")
    # h1: the host goes down, the server hands him to B; 600 frames; B asks the knock-down again (h2); 3 x 300 to h5
    pair.act("A", "hurt 80")
    pair.step_both(600)
    shots.both("h1")
    pair.act("B", "verb %s 139" % PG)
    pair.step_both(900)
    shots.both("h2")
    # him in the arena on both games after the hand-overs (the old shots h1..h5: now by state)
    for x in ("A", "B"):
        b = boss(pair, x)
        assert b is not None and b.get("alive", True), "%s: %s not in the world list after the hand-over" % (x, PG)
        assert b["pos"][1] > start[x] - FALL_BELOW_M, "%s: %s at y %.1f, %.0f m below his fight start %.1f" % (
            x, PG, b["pos"][1], start[x] - b["pos"][1], start[x])
    # d1: B kills him; both games see him dead (1800 + 900 frames, the old waits)
    pair.act("B", "kill %s" % PG)
    pair.until("%s dead on both games" % PG, lambda: all(dead(boss(pair, x)) for x in ("A", "B")), max_frames=2700)
    shots.both("d1")
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
    shots.check()
    # the cause (phantom-ganon-287-3), on each game that took him over with the fix asking. A probe result, not the
    # row's bug (that is the no-fall + fix-line asserts above): an unseen cause is NO-REF naming what was seen, never
    # a FAIL (review-rows-c130-10 20:53 MUST 1).
    taken = {x: c for x, c in ((x, cause(lines[x])) for x in ("A", "B")) if c is not None}
    unseen = []
    for x, (copy, owners) in sorted(taken.items()):
        line = "%s: cause: his copy stood in CCT move state %s when made ours; the owner's was %d" % (x, copy, owners)
        print(line)
        pair.notes.append(line)
        if copy is None:
            unseen.append("%s: no move probe line for his copy in the knock-down turns before the fix's line" % x)
        elif copy == owners:
            unseen.append("%s: his copy stood in move state %d, the owner's too: no cause seen" % (x, copy))
    if not taken:
        unseen.append("no game took him over with the fix asking a move state")
    if unseen:
        pair.unproven("cause not seen: " + "; ".join(unseen))
