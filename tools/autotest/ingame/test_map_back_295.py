"""Row 295 (player report, 0.12.0; dauro saw it too): after co-op's page in the game's - menu, L back to the Map shows
the Map page BLANK (its backdrop only) until the menu is opened again.

Root cause (hookshot/src/coop_pause_tab_module.cpp FollowMapUnder): while co-op's page shows, the Map page's six content
panes are hidden under it. L from co-op's first tab (Multiplayer) with the tabs native closes CoopMenu_00 and the Map
sequence opens the SAME Map screen again (state 1 at the same addresses): LayoutLookAgain says look again, and the
look-up forgot "hidden" without showing a pane (b3eb6e965, v0.11.12 on). Fixed: the panes found where they were are
given back (CoopPauseTab::MapPaneOnLookAgain; also a slide cut by the opening, put back at its own x).

Judged by state: `menu.map_page` {up, panes, shown} (docs/playtest.md "menu", coop_menu_state.h MapPage), read from the
Map screen's own update. After L, within MAP_FRAMES, the Map page is up with every pane found shown (`shown == panes`,
`panes` > 0). Each case on each game: co-op's page opened as a player has it (test_menu_pages.open_page, F1's rule),
from the main view and from a view (Join: L leaves any view the same way, coop_pause_tab.cpp MinusPad's L), then L.
Shot (user-facing: the Map page a player sees) on each game after L, the game's framebuffer (bridge shot, the - menu's
pages are the game's Layout panes); NO-REF until the owner approves refs.
"""
import pytest

from test_menu_pages import open_page, page_shown

PRESS = 8             # a press held 8 frames (test_menu_pages / test_key_items)
# The Map back after L: the Map sequence's own in animation, the panes given back at the Map screen's first update
# (FollowMapUnder). 90 frames = 3 s at 30 fps bounds the wait only (a short one is a FAIL, never a PASS).
MAP_FRAMES = 90
SHUT_FRAMES = 150     # MINUS shuts the - menu (test_key_items OPEN_WAIT 150 for its opening)
TOL = 0.90
CASES = (("main", "main"), ("join", "join"))   # (the page opened, the shot's name part)


def map_page(pair, x):
    return ((pair.state(x).get("menu") or {}).get("map_page")) or {}


def map_full(pair, x):
    """The Map page up with each content pane found shown, or None."""
    m = map_page(pair, x)
    return m if m.get("up") and m.get("panes", 0) > 0 and m.get("shown") == m.get("panes") else None


@pytest.mark.budget(240)   # ESTIMATED: 4 x (the - menu's run <= 10 s + L + <= 3 s + a shot + MINUS 5 s)
@pytest.mark.look
@pytest.mark.row("#295 L from co-op's page in the - menu (its main view and a view) shows the game's Map page again in "
                 "full, never blank (backdrop only), on each game")
def test_map_back_after_coop_page(pair):
    with pair.setup():
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
            assert not page_shown(s), "%s: co-op's page shows before the test" % x
    # (no menu.map_page at all after L: a module before row 295's fix, which never said the Map page's panes: FAIL)
    for x in ("A", "B"):
        for page, name in CASES:
            open_page(pair, x, page)
            pair.press(x, buttons=["L"], frames=PRESS)
            got = pair.until("%s: L from co-op's %s page: the Map page up in full (menu.map_page shown == panes)"
                             % (x, page), lambda: map_full(pair, x), max_frames=MAP_FRAMES)
            assert not page_shown(pair.state(x)), "%s: co-op's page still shows after L: %s" % (
                x, page_shown(pair.state(x)))
            pair.notes.append("%s %s: Map page %d of %d panes shown after L" % (x, page, got["shown"], got["panes"]))
            pair.check_shot(x, "map-back-295-%s" % name, tolerance=TOL)
            pair.press(x, buttons=["MINUS"], frames=PRESS)
            pair.step_both(SHUT_FRAMES)
