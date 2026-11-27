"""pause-press-0121 row, its own case FORCED: + right after a load whose play UI is still down pauses both games.

gate-pause-309d7180 (S 309d7180c, v0.12.0): A's boot load took its play UI down (game-out-A.txt :4323), the bridge said
in play, the test pressed +, and 'the game's play UI is up' came only at :5052: the game refuses + while its play UI is
down (crawl-337-12, after a warp), so A never paused. The fix (pause-press-0121 788c63940, Pair.ready / until_in_play)
waits for the play UI before a test's presses and notes 'in play with its play UI down: waited N frames'. The rerun
pause-press-021c17f5-2 passed, but its boot did not leave A's UI down, so the case itself was not hit (B #158).

This test does not wait for luck: in its set-up A is warped to the boot's own scene and entrance WITHOUT the warp's
play-UI wait (Pair.warp_one ui_wait=False), and the set-up proves the case from the game itself: a 'play UI is down'
line in A's log since the warp, no 'up' after it, and A's bridge state in play (Pair.in_play) at the same read. No hit:
SETUP-FAIL naming what was seen (never PASS). Then the fixed path runs: until_in_play must say A waited (its note),
then + on both games must pause both (state; on real games also PlusMenu_00 lines on each), and + again must bring
both back in play.

Sources of every value (worker-rules "no guessed values"):
- PLAY_UI line: pair.PLAY_UI = hookshot/src/coop_hookshot.cpp Frame ('the game's play UI is (up|down)'), seen in
  pause-press-021c17f5-2 game-out-A.txt :3821 down / :4406 up.
- BOOT_SCENE: bt_plugin.boot_scene(DEFAULT_BOOT) (the boot scripts' warp line, test_smoke.py's); ENTRANCE
  'Kakariko_WellSide': scenarios/live-kakariko-child.A.txt's warp, as test_f1.py / test_first_night.py BOOT_START.
- PAGES: hookshot/src/coop_pause_tab.cpp's PlusMenu_00 line (test_pause_probe_boot.py on pause-press-0121-run: 2 such
  lines on each game after +, pause-press-021c17f5-2; 7 per game over test_pause's open + close).
- PLUS: coop_playtest_core.cpp kButtons (test_smoke.py); a paused game has no fresh Link sample (not_ready set).
- HIT_FRAMES 120: the fake's warp leaves the UI down 20 frames (fakegame BT_FAKE_UI_LATE); the real A's boot case was
  down ~730 lines of log; the case must show at the warp's first reads, 120 frames is the ceiling before SETUP-FAIL.
"""
import re

import pytest

import pair as P
from bt_plugin import DEFAULT_BOOT, boot_scene

BOOT_SCENE = boot_scene(DEFAULT_BOOT)
ENTRANCE = "Kakariko_WellSide"
PAGES = re.compile(r"coop: pause tab: the pause menu's pages \(PlusMenu_00\)")
UI = re.compile(P.PLAY_UI)
HIT_FRAMES = 120
WAITED = "A: in play with its play UI down: waited "


def _ui_since(pair, x, pos):
    """The PLAY_UI words of game x's log since pos, in order."""
    return [m.group(1) for m in (UI.search(ln) for ln in pair.log[x].since(pos)) if m]


def _pages(pair, x, pos):
    return sum(1 for ln in pair.log[x].since(pos) if PAGES.search(ln))


@pytest.mark.budget(240)
@pytest.mark.row("pause-press-0121: + right after a load whose play UI is still down (forced) pauses both games")
def test_pause_after_play_ui_down(pair, request):
    with pair.setup():
        pair.until_in_play("both games in play before the forced load", max_frames=P.READY_FRAMES)
        pos = pair.log["A"].size()
        pair.warp_one("A", BOOT_SCENE, ENTRANCE, ui_wait=False)
        ran, hit = 0, None
        while True:
            words = _ui_since(pair, "A", pos)
            st = pair.state("A")
            if words and words[-1] == "down" and pair.in_play(st):
                hit = "A in play at frame %s with its play UI down (log since the warp: %s)" % (
                    st.get("frame"), " -> ".join(words))
                break
            if (words and words[-1] == "up") or ran >= HIT_FRAMES:
                raise P.SetupFail("the forced case was not hit: A's play UI since the warp %s, %s after %d frames"
                                  % (" -> ".join(words) or "(no line)", pair.why_not("A", st), ran))
            pair.step_both(P.CHUNK)
            ran += P.CHUNK
        pair.notes.append("forced case hit: " + hit)
    # (the fixed path, no step in between: the first read of until_in_play sees A in play with its UI down)
    pair.until_in_play("both games ready for + after A's load", max_frames=P.READY_FRAMES)
    waited = [n for n in pair.notes if n.startswith(WAITED)]
    assert waited, "until_in_play did not wait for A's play UI (no '%s...' note): %s" % (WAITED, pair.notes)
    at = {x: pair.log[x].size() for x in ("A", "B")}
    for x in ("A", "B"):
        pair.press(x, ["PLUS"], frames=4)
    pair.until("both games paused (no fresh Link sample)",
               lambda: all(pair.state(x).get("not_ready") is not None for x in ("A", "B")), max_frames=120)
    if not request.config.getoption("fake"):     # (the fake games write no PlusMenu_00 line: state only there)
        pair.until("both games show their pause page (PlusMenu_00 lines)",
                   lambda: all(_pages(pair, x, at[x]) > 0 for x in ("A", "B")), max_frames=120)
        pair.notes.append("pause pages: PlusMenu_00 lines A %d, B %d" % (_pages(pair, "A", at["A"]),
                                                                         _pages(pair, "B", at["B"])))
    for x in ("A", "B"):
        pair.press(x, ["PLUS"], frames=4)
    pair.until("both games back in play after the second +",
               lambda: all(pair.in_play(pair.state(x)) for x in ("A", "B")), max_frames=300)
