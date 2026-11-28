"""C3-1 of rc-01113 (where F1 takes the player) as bridge tests: the port of offline/f1-event-run.ps1 and its scenario
tools/autotest/scenarios/f1-event.steps.txt (+ f1-event.A.txt, f1-event-open.A.txt, f1-free.A.txt), reports/
port-sweep-c131.md G7. Owner: port-g7.

The rule (hookshot/src/coop_menu_tabs_rule.h, hos_coop_key in coop_hookshot.cpp): F1 in free play opens the game's
- menu on co-op's Multiplayer tab (owner O5); wherever the - menu cannot open now (an event, a load, the title) F1
opens Longshot's menu on co-op's Multiplayer tab (co-op's tabs are in it then), so Leave is reachable from a stuck
event. The rc-01113 bug: there F1 reached a Longshot menu with no co-op tab.

| Test | Old part (f1-event.steps.txt) | Proves |
|---|---|---|
| test_f1_title | title (session): `state A title`, `key A F1`, wshot f1-title, f1-title-route / -tabs | Longshot's menu on Multiplayer |
| test_f1_event | event (the Deku Tree chest's item get): f1-event.A.txt, flag gate, open, `state A event` | the same, over the event |
| test_f1_free_play | free play: f1-free.A.txt, `state A free`, shot f1-free, f1-free-route / -tabs | the game's - menu, 0 tabs |
| test_f1_solo_title | solo (co-op idle): the title | as the title, no server |
| test_f1_solo_event | solo: the event | as the event, no server |

How it differs from the old run, and why:
- F1 is the bridge's `key` (docs/playtest.md; hos_coop_key as Longshot's WM_KEYDOWN calls it, ls32-src/host/display.c),
  not a WM_KEYDOWN posted to the window: same module entry, same route lines, no OS key or focus.
- `state A title|event|free` read the module's LAST "coop: free play: ..." line (coop_hookshot.cpp DrawScene, logged at
  each change): here `last_free_play` reads the same line from the whole log (a change logged before this test began
  still says what holds now), and the event and the title are also read from the bridge's state.
- The menu's co-op tab is also judged by state: the state's `menu.coop` (docs/playtest.md "menu": co-op's tab of
  Longshot's menu, `open`, `tab` "multiplayer"; coop_menu_state / CoopPause::MenuTab), not only by the window shot.
- Longshot's menu is drawn by its ImGui over the picture, after the bridge's shot is taken (f1-event-c124: the VOID
  f1-event-aebd16089), so those checks take `pair.window_shot` (harness-c133); f1-free stays a bridge shot (the game's
  own - menu is in the picture). F1 is pressed on A; each shot is taken on both games (B must show none of it).
- `sleep 3` (wall clock) after F1 -> frames until the state shows the tab (MENU_FRAMES, ~3 s of game).
- The chest gate: the old `flag A key64:... 0` read the module's watch line (LICHII_COOP_DEBUG_KEY64); here
  `flag_read key64:0x...` reads it directly (docs/playtest.md flag_read), so no switch is needed.
- goto/toward to the chest -> place/face at the goto's end (docs/ingame-tests.md step 3).
- f1-free: the old row warped into the Deku Tree first; free play in the boot's Kakariko is the same state (the rule
  reads free play, not the scene), so no warp. The tests close what they opened (the boot is shared): F1 again (Longshot
  closes its menu: the bridge's `longshot` `close`) or the game's Minus (charpage-288-open.A.txt:25 "Minus out").
"""
import os
import re

import pytest

import solo
from pair import READY_FRAMES
# the way to the title and back, ONE source: test_smoke.py's lines (coop_pause.cpp to-title.now; coop_hook.cpp the
# title's own line; coop_pause_tab_module.cpp ToTitleStep's give-up) and its frame bound
from test_smoke import AT_TITLE, TITLE_FRAMES, TO_TITLE_ASKED, TO_TITLE_GAVE_UP, TO_TITLE_ON

# hookshot/src/coop_hookshot.cpp hos_coop_key (the routes) and hos_coop_menu_tabs (the tab count, logged at a change)
ROUTE_LONGSHOT = r"coop: F1: Longshot's menu on co-op's Multiplayer tab \(our - menu cannot open now\)"
ROUTE_MINUS = r"coop: F1: the - menu's Multiplayer tab \(free play\)"
ROUTE_MINUS_ANY = r"coop: F1: the - menu's Multiplayer tab"
TABS = re.compile(r"coop: Longshot's menu: (\d+) of co-op's tabs")
# coop_hookshot.cpp DrawScene: "coop: free play: {}" with yes / no (title) / no (event) / no (load, no Link)
FREE = re.compile(r"coop: free play: (.+?)\s*$")
# coop_pause_tab_module.cpp, the F1 run (OpenMultiplayer, then kShowPlayers with g_players_to_main)
MINUS_RUN = r"coop: pause tab: F1: to the Multiplayer tab"
MINUS_VIEW = r"coop: pause tab: F1: the Multiplayer view, after"
MINUS_GONE = r"coop: pause tab: CoopMenu_00 (closing|closed): its page forgotten"   # ForgetCoopMenu

# The Deku Tree chest (f1-event.A.txt, its sources there): the slingshot room's chest, opened flag key64
# 0x0b66198d17d6584e (run f1-event-aebd16089 A log "+ Obj_TBox_A_02 ... hash 0xb66198d17d6584e"; deku-ladder.A.txt:12
# "0 -> 1" when it opens); the ledge -51.8 112.6 12.2 (cutscene-snap.A.txt place), the goto's end -53.3 x 12.2 (its
# 'goto -53.3 12.2 0.4 300'), the chest -54.4 112.0 12.2 (deku-ladder.A.txt Obj_TBox), faced as its 'toward -54.4 12.2'.
DEKU, DEKU_START = "InsideTheDekuTree", "Entrance"
CHEST = "key64:0x0b66198d17d6584e"
IN_FRONT = (-53.3, 112.6, 12.2)
CHEST_XZ = (-54.4, 12.2)
BOOT_SCENE, BOOT_START = "KakarikoVillage_Child", "Kakariko_WellSide"   # scenarios/live-kakariko-child.A.txt warp

MENU_FRAMES = 90          # the old 'sleep 3' after F1, in frames (FPS 30)
WINDOW_TOL = 0.85         # Longshot's menu is still UI over a still title or a held item get (ingame-tests.md step 6)
FREE_TOL = 0.85           # the game's - menu page: still UI


def last_free_play(pair, x):
    """The module's last 'coop: free play: ...' word for game x (the whole log: it is logged only at a change)."""
    word = None
    for ln in pair.log[x].since(0):
        m = FREE.search(ln)
        if m:
            word = m.group(1)
    return word


def last_tabs(pair, x, pos=0):
    """The last 'Longshot's menu: N of co-op's tabs' count of game x in its log from `pos` (0: the whole log; the line
    is logged at a change only), or None when there is none."""
    n = None
    for ln in pair.log[x].since(pos):
        m = TABS.search(ln)
        if m:
            n = int(m.group(1))
    return n


def coop_tab(pair, x):
    """The state's co-op tab of Longshot's menu: {'open': bool, 'tab': ...} (docs/playtest.md "menu")."""
    return ((pair.state(x).get("menu") or {}).get("coop") or {})


def f1(pair, x):
    r = pair.key(x, "F1")
    pair.notes.append("%s: F1 %s" % (x, {k: r.get(k) for k in ("vk", "taken", "longshot")}))
    return r


def f1_longshot_on_coop(pair, x, shot):
    """F1 where the - menu cannot open now: the route line, Longshot's menu showing co-op's Multiplayer tab (state),
    co-op's tabs in it (the tab line), never the - menu's route; the window shot. Then F1 again shuts it."""
    r = f1(pair, x)
    assert r.get("taken") is True, "%s: co-op did not take F1 here (%s): Longshot's own F1, no co-op tab" % (x, r)
    pair.expect_log(x, ROUTE_LONGSHOT, max_frames=MENU_FRAMES)
    tab = pair.until("%s: Longshot's menu on co-op's tab (state menu.coop)" % x,
                     lambda: (lambda t: t if t.get("open") else None)(coop_tab(pair, x)), max_frames=MENU_FRAMES)
    assert tab.get("tab") == "multiplayer", "%s: Longshot's menu opened on co-op's %r tab, not Multiplayer" % (
        x, tab.get("tab"))
    pair.until("%s: co-op's tabs in Longshot's menu (the tab line > 0)" % x,
               lambda: (last_tabs(pair, x) or 0) > 0, max_frames=MENU_FRAMES)
    pair.never_log(x, ROUTE_MINUS_ANY)          # (f1-event-minus: the - menu's route in an event)
    for y in games(pair):                       # each game (the screenshot rule): A's menu, and B with none of it
        if y != x:
            assert not coop_tab(pair, y).get("open"), "%s: F1 on %s opened co-op's tab on %s too" % (y, x, y)
        pair.window_shot(y, shot, tolerance=WINDOW_TOL)


def close_longshot_menu(pair, x):
    """F1 again with co-op's tab showing: Longshot's own F1, which closes its menu (the bridge's `longshot` close)."""
    r = f1(pair, x)
    assert r.get("taken") is False and r.get("longshot") == "close", "%s: F1 did not shut the menu: %s" % (x, r)
    pair.until("%s: Longshot's menu shut (state menu.coop closed)" % x,
               lambda: not coop_tab(pair, x).get("open"), max_frames=MENU_FRAMES)


def games(pair):
    return [x for x in ("A", "B") if x in pair.c]



def to_title(pair):
    """test_smoke.py's way to the title: to-title.now in each game's folder, its line, then each game AT the title (the
    title's own line since the ask, not merely out of play: an open menu is out of play too) and the module's free play
    word 'no (title)' on A. Returns at_title(x) for load_again (Pair.load_both presses only a game at the title)."""
    for x in games(pair):
        assert pair.log[x].find(TO_TITLE_ON, pos=0), "%s: LICHII_COOP_TEST_TO_TITLE is not on" % x
    asked = {x: pair.log[x].mark() for x in games(pair)}
    for x in games(pair):
        pair.trigger(x, "to-title.now")
    for x in games(pair):
        pair.expect_log(x, TO_TITLE_ASKED, max_frames=300)

    def at_title(x):
        return pair.log[x].find(AT_TITLE, pos=asked[x]) is not None

    def all_at_title():
        for x in games(pair):
            gave_up = pair.log[x].find(TO_TITLE_GAVE_UP, pos=asked[x])
            assert gave_up is None, "%s: the way back to the title gave up: %s" % (x, gave_up.strip())
        return all(at_title(x) and not pair.in_play(pair.state(x)) for x in games(pair))
    pair.until("every game at the title", all_at_title, max_frames=TITLE_FRAMES)
    pair.until("A on the title (free play: no (title))", lambda: last_free_play(pair, "A") == "no (title)",
               max_frames=300)
    return at_title


def load_again(pair, at_title):
    """test_smoke.py: back to play from the title; with a session, both connected again."""
    pair.load_both(at_title, max_frames=3600)
    pair.until_in_play("in play after the load", games=games(pair), max_frames=READY_FRAMES)
    if len(games(pair)) == 2:
        pair.until("both games list two connected players again",
                   lambda: all(len([p for p in ((pair.state(x).get("coop") or {}).get("players") or [])
                                    if p.get("state") == "connected"]) >= 2 for x in ("A", "B")), max_frames=900)


def run_title(pair, shot):
    with pair.setup():
        pair.need_boot_place()
    done, at_title = False, None
    try:
        at_title = to_title(pair)
        f1_longshot_on_coop(pair, "A", shot)
        done = True
    finally:
        def back():
            if coop_tab(pair, "A").get("open"):
                close_longshot_menu(pair, "A")
            if at_title:
                load_again(pair, at_title)
        way_back(pair, done, back)


def run_event(pair, shot):
    with pair.setup():
        pair.need_boot_place()
        before = pair.flag_read("A", CHEST).get("value")
        # f1-event.A.txt: the chest put back CLOSED, then a warp into the Deku Tree loads it closed (its chests spawn
        # at the scene load: that run's '+ Obj_TBox' came before 'mark after'). The old row set the flag after its
        # first warp only because that warp was also the save's load ("the load puts the save's value back"); here
        # the save is loaded already, so the flag goes first and ONE warp loads the room (its reload warp not needed).
        pair.act("A", "setflag %s 0" % CHEST)
        pair.warp_one("A", DEKU, DEKU_START)
        got = pair.flag_read("A", CHEST)
        assert got.get("value") in (0, False), "the chest is not closed after the warp: %s (the old gate)" % got
        pair.act("A", "place %.1f %.1f %.1f" % IN_FRONT)
        pair.act("A", "face %.1f %.1f" % CHEST_XZ)
        pair.step_both(10)
        assert last_free_play(pair, "A") == "yes", "A not in free play before the chest (%s)" % last_free_play(pair, "A")
    done = False
    try:
        # f1-event-open.A.txt: 'press A', 'wait 300' -> the item get, an event whose text waits for A
        pair.press("A", ["A"], frames=8)
        pair.step_both(300)
        pair.until("A in the chest's event (free play: no (event); state event)",
                   lambda: last_free_play(pair, "A") == "no (event)" and pair.state("A").get("event") is True,
                   max_frames=300)
        f1_longshot_on_coop(pair, "A", shot)
        done = True
    finally:
        def back():
            if coop_tab(pair, "A").get("open"):
                close_longshot_menu(pair, "A")
            # the item get's text: A until free play again (each press 8 frames, then 52: load_both's rhythm)
            for _ in range(20):
                if last_free_play(pair, "A") == "yes":
                    break
                pair.press("A", ["A"], frames=8)
                pair.step_both(52)
            assert last_free_play(pair, "A") == "yes", "A not back in free play after the item get (%s)" % (
                last_free_play(pair, "A"))
            if before is not None:
                pair.act("A", "setflag %s %d" % (CHEST, int(before)))
            pair.warp_one("A", BOOT_SCENE, BOOT_START)
        way_back(pair, done, back)


def way_back(pair, done, fn):
    """The boot as it was for the lane's later tests: a failure here is raised only when the test body passed (`done`);
    after a real FAIL it goes into the notes, so the test's own message stays (test_gear.py's way_back)."""
    try:
        fn()
    except Exception as e:
        if done:
            raise
        pair.notes.append("way back failed after the test's FAIL: %s: %s" % (type(e).__name__, e))


@pytest.mark.look
@pytest.mark.leaves_play     # (the title: runs last in its round, the lane's recovery after it; test_smoke)
@pytest.mark.budget(300)
@pytest.mark.row("C3-1 rc-01113 F1 on the title: Longshot's menu on co-op's Multiplayer tab (f1-event title)")
def test_f1_title(pair):
    run_title(pair, "f1-title")


@pytest.mark.look
@pytest.mark.budget(300)
@pytest.mark.row("C3-1 rc-01113 F1 in an event: Longshot's menu on co-op's Multiplayer tab, Leave reachable (f1-event)")
def test_f1_event(pair):
    run_event(pair, "f1-event")


@pytest.mark.look
@pytest.mark.budget(240)
@pytest.mark.row("C3-1 rc-01113 F1 in free play: the game's - menu on co-op's Multiplayer tab (f1-event free)")
def test_f1_free_play(pair):
    with pair.setup():
        pair.need_boot_place()
        s = pair.state("A")
        assert s.get("event") is False, "A not in free play at the start (an event)"
        assert last_free_play(pair, "A") == "yes", "the module's free play word is %r, not yes (the old 'state A free')" % (
            last_free_play(pair, "A"))
        assert not coop_tab(pair, "A").get("open"), "Longshot's menu shows co-op's tab before F1"
    done = False
    try:
        r = f1(pair, "A")
        assert r.get("taken") is True, "co-op did not take F1 in free play: %s" % r
        pair.expect_log("A", ROUTE_MINUS, max_frames=MENU_FRAMES)
        pair.expect_log("A", MINUS_RUN, max_frames=MENU_FRAMES)
        pair.expect_log("A", MINUS_VIEW, max_frames=300)         # (the run's Minus and taps: give it ~10 s of game)
        # f1-free-tabs, judged from THIS test's lines (review-rows-c136 2): the tab line is logged at a change only, from
        # Longshot's menu tick (coop_hookshot.cpp hos_coop_menu_tabs), so an earlier test's last line may still say N > 0
        # here. Since this test began: no count above 0 (none, or 0), and the state's co-op tab is shut.
        counts = [int(m.group(1)) for m in (TABS.search(ln) for ln in pair.log["A"].since()) if m]
        assert not [n for n in counts if n > 0], "Longshot's menu had %s of co-op's tabs in free play (f1-free-tabs)" % (
            counts)
        assert not coop_tab(pair, "A").get("open"), "Longshot's menu opened on co-op's tab in free play"
        pair.never_log("A", ROUTE_LONGSHOT)
        pair.step_both(MENU_FRAMES)                              # (the old 'sleep 5' before its shot: the page drawn)
        for x in games(pair):                                    # A's - menu page; B plays on (the screenshot rule)
            pair.check_shot(x, "f1-free", tolerance=FREE_TOL)
        done = True
    finally:
        def back():
            if coop_tab(pair, "A").get("open"):                  # (a FAIL: F1 took Longshot's route; shut it first,
                close_longshot_menu(pair, "A")                   # the lane's recovery does not, and the next test
                return                                           # would start with it open)
            pair.press("A", ["MINUS"], frames=8)                 # charpage-288-open.A.txt:25 'press MINUS' = out
            pair.step_both(180)                                  # (its 'wait 180')
            pair.expect_log("A", MINUS_GONE, max_frames=300)
        way_back(pair, done, back)


@solo.MARK
@pytest.mark.look
@pytest.mark.leaves_play     # (the title: runs last in its round, the lane's recovery after it; test_smoke)
@pytest.mark.budget(300)
@pytest.mark.row("C3-1 rc-01113 F1 solo (co-op idle) on the title (f1-event solo title)")
def test_f1_solo_title(pair):
    with pair.setup():
        solo.prove(pair)
    run_title(pair, "f1-solo-title")


@solo.MARK
@pytest.mark.look
@pytest.mark.budget(300)
@pytest.mark.row("C3-1 rc-01113 F1 solo (co-op idle) in an event (f1-event solo event)")
def test_f1_solo_event(pair):
    with pair.setup():
        solo.prove(pair)
    run_event(pair, "f1-solo-event")
