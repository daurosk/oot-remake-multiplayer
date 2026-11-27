"""pause-press-0121 probe (measures only, never PASS for an rc row): is + refused right after the BOOT while the game's
play UI is still down?

gate-pause-309d7180 (S 309d7180c): test_pause ran first after the boot (live-kakariko-child); B paused (game-out-B.txt
:5249 PlusMenu_00 state 2), A never did. A's log: the boot's scene load took the play UI down (:4323 'the game's play UI
is down'), a soldier event (Npc_Hylia_Soldier_001) ran :4841-:4915, free play :4917, and 'the game's play UI is up'
came only at :5052, after the bridge had connected (:4855) and the test had pressed +. crawl-337-12 proved the same
refusal after a WARP (fixed by Pair._until_play_ui in warp_one); boot_ready / until_in_play have no such wait.

This probe does test_pause's own sequence as the first test after the boot, and records per game and per 10 frames the
state's frame / not_ready, coop-test-state.txt (LICHII_COOP_TEST_STATE=1: pad_came / pad_got PLUS bit, st_page), the
play UI's last line since the game's log start (up / down / none) and the PlusMenu_00 lines since the test began.
Then each game that did not pause: stepped until its play UI is up (at most 600 frames, recorded each 30), + again, 60
frames recorded. Every game that shows its pause gets a second + at the end. FAILs only on a set-up miss.
"""
import os
import re
import sys

import pytest

import pair as P

PLUS = 1 << 10
PAGES = r"coop: pause tab: the pause menu's pages \(PlusMenu_00\)"      # hookshot/src/coop_pause_tab.cpp
UI = re.compile(P.PLAY_UI)


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
    return "pad_came=%s pad_got=%s (%s) st_page=%s" % (came, got, plus, words.get("st_page"))


def _ui(pair, x):
    last = "none"
    for ln in pair.log[x].since(0):
        m = UI.search(ln)
        if m:
            last = m.group(1)
    return last


def _pages(pair, x, pos):
    rx = re.compile(PAGES)
    return sum(1 for ln in pair.log[x].since(pos) if rx.search(ln))


def _record(pair, what, pos, games=("A", "B")):
    out = {}
    for x in games:
        s = pair.state(x)
        out[x] = s
        line = "%s %s: frame=%s event=%s not_ready=%s | play UI %s | %s | PlusMenu_00 lines %d" % (
            what, x, s.get("frame"), s.get("event"), (s.get("not_ready") or "null")[:40], _ui(pair, x),
            _test_state(pair, x), _pages(pair, x, pos[x]))
        pair.notes.append(line)
        print("pause-boot: " + line, file=sys.stderr, flush=True)
    return out


def _paused(pair, x, pos):
    return _pages(pair, x, pos[x]) > 0


@pytest.mark.budget(900)
@pytest.mark.needs_env("LICHII_COOP_TEST_STATE=1")
@pytest.mark.row("probe pause-press-0121: test_pause's presses as the first test after the boot - measures only")
def test_pause_probe_after_boot(pair):
    with pair.setup():
        pair.until_in_play("both games in play before the probe", max_frames=P.READY_FRAMES)
    pos = {x: pair.log[x].mark() for x in ("A", "B")}
    _record(pair, "before", pos)
    for x in ("A", "B"):                        # (test_pause's order: A's + then B's +, 4 frames each)
        pair.press(x, ["PLUS"], frames=4)
    _record(pair, "after both +", pos)
    for i in range(12):                         # (test_pause waits 120 frames)
        pair.step_both(10)
        _record(pair, "after both + +%d" % ((i + 1) * 10), pos)
    missed = [x for x in ("A", "B") if not _paused(pair, x, pos)]
    pair.notes.append("missed after 120 frames: %s" % (missed or "none"))
    for x in missed:
        ran = 0
        while _ui(pair, x) != "up" and ran < 600:
            pair.step_both(30)
            ran += 30
            _record(pair, "%s waits for its play UI +%d" % (x, ran), pos, games=(x,))
        pair.notes.append("%s: play UI %s after %d more frames; + again" % (x, _ui(pair, x), ran))
        pair.press(x, ["PLUS"], frames=4)
        for i in range(6):
            pair.step_both(10)
            _record(pair, "%s after its second + +%d" % (x, (i + 1) * 10), pos, games=(x,))
    st = _record(pair, "before closing", pos)
    for x in ("A", "B"):
        if st[x].get("not_ready") is not None:
            pair.press(x, ["PLUS"], frames=4)
    pair.step_both(60)
    _record(pair, "after closing", pos)
