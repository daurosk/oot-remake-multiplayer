"""The rc smoke tier (the first 10 minutes of an rc: a broken rc stops here): boot, co-op connect, warp, pause,
back to the title, load again. One boot per lane (bt_plugin.lane_pair); the session loads the save first
(live.py start: the runner's to_play, A until play), so 'menu -> load' is proven here by the title + load again test.

Sources of every value (worker-rules "no guessed values"):
- 'coop: connected': tools/autotest/groups/hookshot-smoke.txt [g1] walk's expect (the module's connect line).
- state members scene/loading/event/not_ready/coop.players: docs/playtest.md "State" (hookshot/src/coop_playtest.cpp).
- 'KakarikoVillage_Child': a map of Longshot's teleport list (villages; docs/playtest.md `warp`), the warp of
  9 boot scripts (scenarios/*.txt 'warp KakarikoVillage_Child ...').
- PLUS = the Switch '+' (the game's pause screen): coop_playtest_core.cpp kButtons; a held pause runs no Link update,
  so the snapshot goes stale (docs/playtest.md "State": 'a title, a pause or a load: no Link update runs').
- to-title.now + its two log lines: hookshot/src/coop_pause.cpp (LICHII_COOP_TEST_TO_TITLE=1, set by bt_plugin).
- 'back to the title: given up': hookshot/src/coop_pause_tab_module.cpp ToTitleStep (after kToTitleMs = 25000 ms of
  wall clock, coop_pause_tab.h); seen on both games every round of test-bridge-pilot-fa63e90f (game-out-A.txt :6614).
- AT_TITLE 'coop: live events: the title: ...': lichii/hook/coop_hook.cpp (no scene loaded); written at the boot's
  title (pilot game-out-A.txt :696, game-out-B.txt :715) and never after the given-up way back. Counted only since the
  to-title ask, with the state not in play.
- BOOT_SCENE: the boot scripts' warp line (bt_plugin.boot_scene, scenarios/live-kakariko-child.A/B.txt).
- the title test is marked leaves_play: it runs last in its round (bt_plugin order) and the next test starts after
  the lane's recovery (Pair.boot_ready), so a miss here cannot cascade into the next round.
- TITLE_UP: hookshot/src/coop_hookshot.cpp (the frame site, CoopNativeUi::SetTitleUp); MAP_TITLE: coop_map_page.cpp
  Frame (party-hud-crash-c126-6: the title reached from play has no scene load; both games crashed there in
  party-hud-bridge-ece368dd-2, ~2 s after the title came up: game-out-A.txt :6784 'loading -> menu' to :6829 CRASH).
- CRASH_LINE: bt_plugin.CRASH_LINE (the module's crash handler line).
- TITLE_HOLD 600 frames (~20 s at pair.FPS = 30) at the title: 10x that crash's ~2 s.
"""
import os

import pytest

from bt_plugin import CRASH_LINE, DEFAULT_BOOT, boot_scene
from pair import READY_FRAMES

pytestmark = pytest.mark.smoke

CONNECTED = r"coop: connected"
TO_TITLE_ON = r"coop: multiplayer tab: test: to-title\.now in .* starts the way back to the title"
TO_TITLE_ASKED = r"coop: multiplayer tab: test: back to the title asked \(to-title\.now\)"
TO_TITLE_GAVE_UP = r"coop: pause tab: back to the title: given up"
AT_TITLE = r"coop: live events: the title: "
TITLE_FRAMES = 1500   # 50 s of game for the menu's way back (its own give-up is 25 s of wall clock: kToTitleMs)
WARP_TO = "KakarikoVillage_Child"
TITLE_UP = r"coop: native ui: the title's menu is up: no play scene"
MAP_TITLE = r"coop: map: the title is up: the panes found are forgotten"
TITLE_HOLD = 600
BOOT_SCENE = boot_scene(DEFAULT_BOOT)
# The pause page's own part (left 56 %: tabs, item grid; the right half shows the dimmed live world behind the page,
# which moves). pause-press-0121-3: the smoke-pause shots of 4 runs (tb-pilot-f9f3b9c2, crawl-337-12
# run-warpui-1cce48cf, testkit-0120, pause-press-0121 run-021c17f5) score 0.78-0.92 whole (backdrop tiles), 0.974-0.998 in this box.
PAUSE_PAGE_BOX = (0.0, 0.0, 0.56, 1.0)


@pytest.mark.budget(60)
@pytest.mark.row("smoke: boot")
def test_boot(pair):
    with pair.setup():
        for x in ("A", "B"):
            assert pair.state(x).get("control") is True, "%s: Longshot's control bridge did not answer" % x
    # (stepped until both are in play, never one read: a stale sample right after the boot is not a FAIL. The lane's
    # boot proof, Pair.boot_ready, already put both in the boot scene, in play, with no event; this proves it held.)
    pair.until_in_play("both games in play after the boot", max_frames=READY_FRAMES)
    for x in ("A", "B"):
        s = pair.state(x)
        assert (s.get("scene") or "").lower() == BOOT_SCENE.lower(), "%s is in %s, not the boot's %s" % (
            x, s.get("scene"), BOOT_SCENE)


@pytest.mark.budget(90)
@pytest.mark.row("smoke: co-op connect")
def test_connect(pair):
    for x in ("A", "B"):
        assert pair.log[x].find(CONNECTED, pos=0), "%s log has no '%s' since the start" % (x, CONNECTED)

    def both_listed():
        out = {}
        for x in ("A", "B"):
            players = ((pair.state(x).get("coop") or {}).get("players")) or []
            if len([p for p in players if p.get("state") == "connected"]) < 2:
                return None
            out[x] = players
        return out
    pair.until("both games list two connected players", both_listed, max_frames=600)


@pytest.mark.budget(150)
@pytest.mark.row("smoke: warp")
def test_warp(pair):
    st = pair.warp_both(WARP_TO)
    for x, s in st.items():
        assert pair.in_play(s), "%s after the warp: not in play (%s)" % (x, s.get("not_ready"))
        assert (s.get("scene") or "").lower() == WARP_TO.lower(), "%s is in %s, not %s" % (x, s.get("scene"), WARP_TO)


@pytest.mark.budget(90)
@pytest.mark.look
@pytest.mark.row("smoke: pause menu open / close (pause page shot on each game)")
def test_pause(pair):
    with pair.setup():
        pair.until_in_play("both games in play before the pause", max_frames=READY_FRAMES)
    for x in ("A", "B"):
        pair.press(x, ["PLUS"], frames=4)
    pair.until("both games paused (no fresh Link sample)",
               lambda: all(pair.state(x).get("not_ready") is not None for x in ("A", "B")), max_frames=120)
    pair.step_both(30)                     # (the page's open animation; the shot is the open page)
    for x in ("A", "B"):
        pair.check_shot(x, "smoke-pause", tolerance=0.90, box=PAUSE_PAGE_BOX)
    for x in ("A", "B"):
        pair.press(x, ["PLUS"], frames=4)
    pair.until("both games back in play after the second +",
               lambda: all(pair.in_play(pair.state(x)) for x in ("A", "B")), max_frames=300)


@pytest.mark.budget(330)   # (+30 s: TITLE_HOLD, ~20 s at the title)
@pytest.mark.leaves_play
@pytest.mark.row("smoke: back to the title, menu -> load again")
def test_title_and_load_again(pair):
    with pair.setup():
        for x in ("A", "B"):
            assert pair.log[x].find(TO_TITLE_ON, pos=0), "%s: LICHII_COOP_TEST_TO_TITLE is not on (no '%s')" % (
                x, "to-title.now in ... starts")
        pair.until_in_play("both games in play before the way back", max_frames=READY_FRAMES)
    asked = {x: pair.log[x].mark() for x in ("A", "B")}
    for x in ("A", "B"):
        with open(os.path.join(pair.games[x]["folder"], "to-title.now"), "w") as f:
            f.write("bt\n")
    for x in ("A", "B"):
        pair.expect_log(x, TO_TITLE_ASKED, max_frames=300)

    def at_title(x):
        return pair.log[x].find(AT_TITLE, pos=asked[x]) is not None

    def both_at_title():
        for x in ("A", "B"):
            gave_up = pair.log[x].find(TO_TITLE_GAVE_UP, pos=asked[x])
            assert gave_up is None, "%s: the way back to the title gave up: %s" % (x, gave_up.strip())
        return all(at_title(x) and not pair.in_play(pair.state(x)) for x in ("A", "B"))
    # (left play = the title's own line since the ask, never 'not in play': an open pause menu is not in play too,
    # pilot test-bridge-pilot-fa63e90f: 'left play' passed on A's menu, then A was pressed in play for 3600 frames)
    pair.until("both games at the title", both_at_title, max_frames=TITLE_FRAMES)
    for x in ("A", "B"):   # (party-hud-crash-c126-6: the title from play is no play scene: nothing is called into)
        pair.expect_log(x, TITLE_UP, max_frames=300)
        pair.expect_log(x, MAP_TITLE, max_frames=300)
    pair.step_both(TITLE_HOLD)   # (frames run at the title: a game that crashed there stops answering)
    for x in ("A", "B"):
        pair.never_log(x, CRASH_LINE)
    pair.load_both(at_title, max_frames=3600)
    pair.until_in_play("both games in play after the load", max_frames=READY_FRAMES)
    for x in ("A", "B"):
        pair.never_log(x, CRASH_LINE)
    pair.until("both games list two connected players again",
               lambda: all(len([p for p in ((pair.state(x).get("coop") or {}).get("players") or [])
                                if p.get("state") == "connected"]) >= 2 for x in ("A", "B")), max_frames=900)
