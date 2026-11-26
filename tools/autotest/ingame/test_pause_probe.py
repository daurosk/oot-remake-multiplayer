"""pause-smoke-c143 probe (measures only, never PASS for an rc row): why test_smoke::test_pause never saw both games
paused (game-commit-c141 smoke-test-75f8e597 + smoke-stock2-75f8e597, both FAIL 'both games paused (no fresh Link
sample)'). In both runs A's pause menu opened (game-out-A: 'pause tab: the pause menu's pages (PlusMenu_00)') and B's
never did (no PlusMenu_00 line in game-out-B while the client was connected), so this probe records, per game and per
CHUNK of frames after each '+':
- the state's not_ready / snapshot_present / frame (coop_playtest_core.cpp LinkMembers),
- coop-test-state.txt (LICHII_COOP_TEST_STATE=1, coop_hookshot.cpp TestState): pad_came = the last pad with buttons
  that reached the module's pad hook (hos_coop_pad), pad_got = what the game got of it after co-op's pad order
  (CoopPadOrder::Run). PLUS = 1 << 10 (lichii/hook/coop_testinput.h kPlus).
B is pressed FIRST here (test_pause presses A first), so 'B's + is lost only after A paused' and 'B's + is lost always'
read differently. Every line goes to the run's notes and to stderr; the test fails only on a set-up miss.
"""
import os
import sys

import pytest

PLUS = 1 << 10
READY_FRAMES = 300


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
    return "pad_came=%s pad_got=%s (%s) st_page=%s st_hold=%s st_title=%s st_dbno=%s" % (
        came, got, plus, words.get("st_page"), words.get("st_hold"), words.get("st_title"), words.get("st_dbno"))


def _record(pair, what):
    for x in ("A", "B"):
        s = pair.state(x)
        line = "%s %s: frame=%s snapshot_present=%s not_ready=%s | %s" % (
            what, x, s.get("frame"), s.get("snapshot_present"), (s.get("not_ready") or "null")[:40], _test_state(pair, x))
        pair.notes.append(line)
        print("pause-probe: " + line, file=sys.stderr, flush=True)


@pytest.mark.budget(120)
@pytest.mark.needs_env("LICHII_COOP_TEST_STATE=1")
@pytest.mark.row("probe pause-smoke-c143: each game's + (pad hook in/out, Link sample) - measures only")
def test_pause_probe(pair):
    with pair.setup():
        pair.until_in_play("both games in play before the probe", max_frames=READY_FRAMES)
    _record(pair, "before")
    for x in ("B", "A"):
        pair.press(x, ["PLUS"], frames=4)
        _record(pair, "after %s's +" % x)
        for i in range(6):
            pair.step_both(10)
            _record(pair, "after %s's + +%d" % (x, (i + 1) * 10))
    for x in ("A", "B"):
        pair.press(x, ["PLUS"], frames=4)
    pair.step_both(60)
    _record(pair, "after the second + on each")
