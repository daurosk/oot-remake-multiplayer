"""The bridge's page step (B #138, owner port-g7): `act menu open PAGE` opens any page of the Multiplayer tab on its own,
nothing pressed, by the path the title's rows open it (CoopPause::TestMenu -> CoopPause::Open -> Menu::Open, the page's
own row decided: hookshot/src/coop_pause.cpp, coop_pause_menu.cpp). The pages: lichii/hook/coop_testinput.h kMenuPages
(the one list; CoopPauseMenu::PageWord's words). `menu main` (row 170, testhost-170) stays as the same as
`menu open main`. Owner rule: every shot of menu UI needs its page opened this way; a row test calls
`open_page(pair, x, PAGE)` from here, then takes its own shot (window_shot: co-op's pages are Longshot's overlay).

Judged by state, not by picture: the bridge state's `menu.coop` (docs/playtest.md "menu": co-op's tab of Longshot's
menu, `open`, `tab`, `page` = PageWord) and the step's own line "coop: test menu open PAGE: asked" (coop_testinput.cpp,
the menu request). Each page is shut again with `menu close` (HOS_COOP_MENU_WANT_CLOSE). No look of its own: the pages'
looks are each row test's shots.
"""
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from hookshot import MENU_PAGES as PAGES   # noqa: E402  (coop_testinput.h kMenuPages, the runner's checked copy)
OPEN_FRAMES = 90      # Longshot's menu takes the module's want at its next menu tick (vk_menu.cpp coop::menu_wanted)


def coop_tab(pair, x):
    return ((pair.state(x).get("menu") or {}).get("coop") or {})


def open_page(pair, x, page, max_frames=OPEN_FRAMES):
    """Game x's Multiplayer tab on `page` (a PAGES word) through the bridge's `act menu open PAGE`; returns the state's
    menu.coop once it shows that page. AssertionError if the step was not asked or the page did not come."""
    assert page in PAGES, "no page %r (coop_testinput.h kMenuPages)" % page
    pair.act(x, "menu open %s" % page)
    pair.expect_log(x, r"coop: test menu open %s: asked" % page, max_frames=30)
    return pair.until("%s: the Multiplayer tab on its %s page (state menu.coop)" % (x, page),
                      lambda: (lambda t: t if t.get("open") and t.get("page") == page else None)(coop_tab(pair, x)),
                      max_frames=max_frames)


def close_menu(pair, x):
    pair.act(x, "menu close")
    pair.until("%s: the menu shut (state menu.coop)" % x, lambda: not coop_tab(pair, x).get("open"),
               max_frames=OPEN_FRAMES)


@pytest.mark.budget(180)
@pytest.mark.row("B #138 bridge: every page of the Multiplayer tab opens on its own (menu open main, host, join, "
                 "local, settings, commands, customize, find)")
def test_every_page_opens_on_its_own(pair):
    with pair.setup():
        for x in ("A", "B"):
            assert pair.in_play(pair.state(x)), "%s not in play" % x
            assert not coop_tab(pair, x).get("open"), "%s: co-op's tab shows before the test" % x
    done = False
    try:
        for page in PAGES:
            got = open_page(pair, "B", page)       # (B: the game regress-lan251 shoots)
            assert got.get("tab") == "multiplayer", "B: %s opened co-op's %r tab" % (page, got.get("tab"))
            assert not coop_tab(pair, "A").get("open"), "A's menu opened with B's %s" % page
            close_menu(pair, "B")
        done = True
    finally:
        try:
            if coop_tab(pair, "B").get("open"):
                close_menu(pair, "B")
        except Exception as e:
            if done:
                raise
            pair.notes.append("way back failed after the test's FAIL: %s: %s" % (type(e).__name__, e))
