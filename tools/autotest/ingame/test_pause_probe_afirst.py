"""crawl-337-11 probe (B #146, measures only, never PASS for an rc row): is B's + lost when A is pressed FIRST?

test_smoke::test_pause presses A's + then B's + back to back (pair.press, 4 frames each) and waits for both games paused;
in tb-gate-ui-f9f3b9c2 and smoke-test-75f8e597 A's pause opened (game-out-A.txt 'the pause menu's pages (PlusMenu_00)',
coop_pause_tab.cpp:1873) and B's never did (no PlusMenu_00 line in game-out-B.txt). pause-probe-1fab757f-r2 pressed B
FIRST and both opened, so the A-first order is the one left to measure. Two orders, per game and per 10 frames:
- back_to_back: A's + then B's + at once (test_pause's own order);
- after_a_paused: A's +, 60 frames, then B's +;
- after_warp: test_smoke's sequence, warp_both to KakarikoVillage_Child (test_warp, test_smoke.py:42), then back to back
  (run 1 of this probe, 2f734963a, no warp before: B's + reached the pad and B paused in both orders).
Each line: the state's frame / snapshot_present / not_ready (coop_playtest_core.cpp:828), coop-test-state.txt
(LICHII_COOP_TEST_STATE=1, coop_hookshot.cpp:2105-2128: pad_came = the last pad with buttons that reached the module's
pad hook, pad_got = what the game got after co-op's pad order; PLUS = 1 << 10, lichii/hook/coop_testinput.h kPlus;
st_page) and how many PlusMenu_00 lines the game's log has since the test began. Every line goes to the notes and stderr.
Each test closes the pause on every game that shows one (a second +) and FAILs only on a set-up miss.
"""
import os
import re
import sys

import pytest

import pair as P

PLUS = 1 << 10
PAGES = r"coop: pause tab: the pause menu's pages \(PlusMenu_00\)"      # hookshot/src/coop_pause_tab.cpp:1873


def _test_state(pair, x):
    p = os.path.join(pair.games[x]["folder"], "coop-test-state.txt")
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            line = f.read().strip()
    except OSError as e:
        return "no coop-test-state.txt (%s)" % e.__class__.__name__
    words = dict(w.split("=", 1) for w in line.split() if "=" in w)
    came, got = words.get("pad_came", "?"), words.get("pad_got", "?")
    try:
        plus = "PLUS came=%d got=%d" % (bool(int(came, 16) & PLUS), bool(int(got, 16) & PLUS))
    except ValueError:
        plus = "PLUS ?"
    return "pad_came=%s pad_got=%s (%s) st_page=%s st_hold=%s" % (came, got, plus, words.get("st_page"),
                                                                 words.get("st_hold"))


def _pages(pair, x, pos):
    rx = re.compile(PAGES)
    return sum(1 for ln in pair.log[x].since(pos) if rx.search(ln))


def _record(pair, what, pos):
    out = {}
    for x in ("A", "B"):
        s = pair.state(x)
        out[x] = s
        line = "%s %s: frame=%s snapshot_present=%s not_ready=%s | %s | PlusMenu_00 lines %d" % (
            what, x, s.get("frame"), s.get("snapshot_present"), (s.get("not_ready") or "null")[:40],
            _test_state(pair, x), _pages(pair, x, pos[x]))
        pair.notes.append(line)
        print("pause-afirst: " + line, file=sys.stderr, flush=True)
    return out


def _probe(pair, gap, warp=None):
    with pair.setup():
        if warp:
            # (test_smoke::test_warp right before test_pause in both failing runs: warp_both(WARP_TO), the boot's own
            # scene, test_smoke.py:42; set-up 1.1 s after it in tb-gate-ui-f9f3b9c2)
            pair.warp_both(warp)
        pair.until_in_play("both games in play before the probe", max_frames=P.READY_FRAMES)
    pos = {x: pair.log[x].mark() for x in ("A", "B")}
    _record(pair, "before", pos)
    pair.press("A", ["PLUS"], frames=4)
    _record(pair, "after A's +", pos)
    for i in range(gap // 10):
        pair.step_both(10)
        _record(pair, "after A's + +%d" % ((i + 1) * 10), pos)
    pair.press("B", ["PLUS"], frames=4)
    _record(pair, "after B's +", pos)
    st = None
    for i in range(6):
        pair.step_both(10)
        st = _record(pair, "after B's + +%d" % ((i + 1) * 10), pos)
    # (put back: a second + only on a game that shows its pause, so none is left paused or newly opened)
    for x in ("A", "B"):
        if st[x].get("not_ready") is not None:
            pair.press(x, ["PLUS"], frames=4)
    pair.step_both(60)
    _record(pair, "after closing", pos)


@pytest.mark.budget(120)
@pytest.mark.needs_env("LICHII_COOP_TEST_STATE=1")
@pytest.mark.row("probe crawl-337-11: A's + then B's + back to back (test_pause's order) - measures only")
def test_pause_probe_afirst_back_to_back(pair):
    _probe(pair, gap=0)


@pytest.mark.budget(120)
@pytest.mark.needs_env("LICHII_COOP_TEST_STATE=1")
@pytest.mark.row("probe crawl-337-11: A's +, 60 frames, then B's + - measures only")
def test_pause_probe_afirst_after_a_paused(pair):
    _probe(pair, gap=60)


@pytest.mark.budget(150)
@pytest.mark.needs_env("LICHII_COOP_TEST_STATE=1")
@pytest.mark.row("probe crawl-337-11: warp_both (test_warp) then A's + and B's + back to back - measures only")
def test_pause_probe_afirst_after_warp(pair):
    _probe(pair, gap=0, warp="KakarikoVillage_Child")
