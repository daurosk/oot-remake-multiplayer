"""The bridge's page step (B #138, owner port-g7; home rule C #145): `act menu open PAGE` opens any page of the Multiplayer
tab on its own, nothing pressed, WHERE A PLAYER HAS IT NOW: F1's rule (hookshot/src/coop_pause.cpp OpenAsPlayer,
coop_menu_tabs_rule.h OnF1). In free play co-op's home is the game's - menu: its Multiplayer tab on the page's view (F1's
own run, CoopPauseTab::OpenMultiplayerOn). Elsewhere (the title, an event, a load) Longshot's menu on co-op's tab
(CoopPause::Open). One home per thing: a page in the other home is a FAIL. The pages: lichii/hook/coop_testinput.h
kMenuPages (the one list; CoopPauseMenu::PageWord's words). `menu main` (row 170, testhost-170) is `menu open main`.
Owner rule: every shot of menu UI needs its page opened this way; a row test calls `open_page(pair, x, PAGE)` from here,
then takes its own shot (window_shot for Longshot's tab: its pages are Longshot's overlay).

Why (f9f3b9c26 gate, host170 + gate-ui-3, harness-c133-3): the step used to want Longshot's co-op tab in free play too,
where Longshot's menu carries no co-op tab (LongshotTabs 0) and takes a want only below its tab count (Longshot
gpu/vk/vk_menu.cpp coop_want < coop_tabs): the module said 'opened on a page' + 'test menu open join: asked' and no page
ever came.

Judged by state, not by picture (docs/playtest.md "menu"): `menu.minus` {open, tab, view} for the - menu (views main,
join, host, lan = Find a game, menu = Settings and look, which shows co-op's own page natively, so its page word is
`menu.coop.page`); `menu.coop` {open, tab, page} for Longshot's tab. Each page is shut again as a player shuts it: the
- menu with B (a view back to main, then the menu shut: coop_pause_tab.cpp BackOf), Longshot's tab with `menu close`
(HOS_COOP_MENU_WANT_CLOSE). No look of its own: the pages' looks are each row test's shots.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from hookshot import MENU_PAGES as PAGES   # noqa: E402  (coop_testinput.h kMenuPages, the runner's checked copy)
from pair import FPS, SetupFail           # noqa: E402
OPEN_FRAMES = 90      # Longshot's menu takes the module's want at its next menu tick (vk_menu.cpp coop::menu_wanted)
# F1's run gives up after kMostMs 8000 (hookshot/src/coop_players_open.h): its frames at FPS, and 2 s over
MINUS_OPEN_FRAMES = (8000 // 1000 + 2) * FPS
PRESS = 8             # a press held 8 frames (as test_key_items / test_map_arrow)
B_WAIT = 30           # after each B: the view or the strip's slide (kTapGapMs 400 is 12 frames)
MOST_B = 3            # Settings and look / Join / Host / the browser -> main -> shut (BackOf goes to main)
MINUS_VIEW_PAGE = {"main": "main", "join": "join", "host": "host", "lan": "find"}   # (view "menu": menu.coop.page)
# B #171 (gate-host170-182cd53b-2 07:28, row170-host-page-B.png = the Map tab, Kakariko's map): menu.minus said the
# Multiplayer tab (the game's tab index, CoopPauseTab MinusTab) while co-op's page still slid in over the Map page, whose
# content showed; the shot came at present 7980, game-out-B.txt.2:4325, before 'the Map page's content under the - menu's
# page hidden' at :4338. That hide (coop_pause_tab_module.cpp FollowMapUnder, MapUnderHidden: only once ours shows, not
# while it slides, kMapUnderAfterMs 450 after a native open) is the module's own "ours is drawn": state menu.map_page
# {up, panes, shown} (row 295, coop_menu_state.h MapPage). Until it, no - menu page may be shot.
DRAWN_FRAMES = 4 * FPS   # (kMapUnderAfterMs 450 + the strip's slide, at the lanes' slowest ~11 fps: 4 s of game)


def map_content_shown(st):
    """The Map page's content panes that show under the - menu now (menu.map_page.shown while its screen is up); 0 when
    its screen is not up or none show."""
    mp = ((st or {}).get("menu") or {}).get("map_page") or {}
    return int(mp.get("shown") or 0) if mp.get("up") else 0


def coop_tab(pair, x):
    return ((pair.state(x).get("menu") or {}).get("coop") or {})


def page_shown(st):
    """(home, page) of co-op's page that shows in state `st`: home "minus" (the game's - menu, its Multiplayer tab)
    or "longshot" (co-op's tab of Longshot's menu); None when neither shows."""
    menu = (st or {}).get("menu") or {}
    minus, coop = menu.get("minus") or {}, menu.get("coop") or {}
    if minus.get("open") and minus.get("tab") == "multiplayer":
        view = minus.get("view")
        return "minus", (coop.get("page") if view == "menu" and coop.get("open") else MINUS_VIEW_PAGE.get(view, view))
    if coop.get("open"):
        return "longshot", coop.get("page")
    return None


def home_now(pair, st):
    """Where F1's rule puts co-op's page now: the - menu in free play, else Longshot's tab (coop_menu_tabs_rule.h)."""
    return "minus" if pair.in_play(st) else "longshot"


def open_page(pair, x, page):
    """Game x's Multiplayer tab on `page` (a PAGES word) through the bridge's `act menu open PAGE`, in the home F1's rule
    names now; returns (home, page). AssertionError if the step was not asked, or the page did not come there."""
    assert page in PAGES, "no page %r (coop_testinput.h kMenuPages)" % page
    home = home_now(pair, pair.state(x))
    pair.act(x, "menu open %s" % page)
    pair.expect_log(x, r"coop: test menu open %s: asked" % page, max_frames=30)
    want = (home, page)
    got = pair.until("%s: the Multiplayer tab on its %s page in %s (state menu.%s)"
                     % (x, page, "the - menu" if home == "minus" else "Longshot's menu",
                        "minus" if home == "minus" else "coop"),
                     lambda: want if page_shown(pair.state(x)) == want else None,
                     max_frames=MINUS_OPEN_FRAMES if home == "minus" else OPEN_FRAMES)
    if home == "minus":
        wait_page_drawn(pair, x, page)
    return got


def wait_page_drawn(pair, x, page):
    """B #171: co-op's - menu page drawn over the Map page (its content hidden: map_content_shown 0) and still the one
    asked; SETUP-FAIL after DRAWN_FRAMES (a shot then would be the Map tab, not the page)."""
    try:
        pair.until("%s: co-op's %s page drawn over the Map page (state menu.map_page shown 0)" % (x, page),
                   lambda: map_content_shown(pair.state(x)) == 0 and page_shown(pair.state(x)) == ("minus", page),
                   max_frames=DRAWN_FRAMES)
    except AssertionError as e:
        st = pair.state(x)
        raise SetupFail("%s: the - menu's %s page not drawn: %s (menu.map_page %s, page %s)"
                        % (x, page, e, ((st.get("menu") or {}).get("map_page")), page_shown(st)))


def close_menu(pair, x):
    """Co-op's page shut as a player shuts it: the - menu with B (MOST_B at most), Longshot's tab with `menu close`."""
    shown = page_shown(pair.state(x))
    if shown and shown[0] == "minus":
        for _ in range(MOST_B):
            pair.press(x, ["B"], frames=PRESS)
            pair.step_both(B_WAIT)
            if not page_shown(pair.state(x)):
                return
        raise AssertionError("%s: the - menu still shows co-op's page after %d B: %s" % (x, MOST_B,
                                                                                       page_shown(pair.state(x))))
    pair.act(x, "menu close")
    pair.until("%s: the menu shut (state menu.coop)" % x, lambda: not page_shown(pair.state(x)),
               max_frames=OPEN_FRAMES)


@pytest.mark.budget(300)   # (8 pages x (the - menu's run <= 10 s + 3 B) in free play)
@pytest.mark.row("B #138 bridge: every page of the Multiplayer tab opens on its own (menu open main, host, join, "
                 "local, settings, commands, customize, find), in the home F1's rule names (C #145)")
def test_every_page_opens_on_its_own(pair):
    with pair.setup():
        for x in ("A", "B"):
            assert pair.in_play(pair.state(x)), "%s not in play" % x
            assert not page_shown(pair.state(x)), "%s: co-op's page shows before the test" % x
    done = False
    try:
        for page in PAGES:
            home, _ = open_page(pair, "B", page)       # (B: the game regress-lan251 shoots)
            assert home == "minus", "B: %s in free play showed in %s, not the - menu (one home)" % (page, home)
            assert not page_shown(pair.state("A")), "A's menu opened with B's %s" % page
            close_menu(pair, "B")
        done = True
    finally:
        try:
            if page_shown(pair.state("B")):
                close_menu(pair, "B")
        except Exception as e:
            if done:
                raise
            pair.notes.append("way back failed after the test's FAIL: %s: %s" % (type(e).__name__, e))
