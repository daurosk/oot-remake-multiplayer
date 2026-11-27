"""Fake-game reactions for row 295 (ingame/test_map_back_295.py). Hooks: fakegame.py docstring.

Acts ONLY on row 295's own move: L while co-op's page shows in the - menu (menu_pages.py's game.mp_minus), which no
other row presses. Then co-op's page goes, and the game's Map page is up: state menu.map_page {up, panes 6, shown}
(coop_menu_state.h MapPage; the game's six content panes, coop_pause_tab_module.cpp kMapContent), 12 frames later
(the Map sequence's in animation). MINUS (or B) on that Map page shuts the - menu: no map_page member any more.
Selftest switch BT_FAKE_MAP295=blank: the module before row 295's fix, the Map page up with 0 of 6 shown (its
backdrop only) after L; =late: the panes come back only after 200 frames (past the test's wait). The test must FAIL
in both.
"""
import os

PANES = 6
LATE = 200


def setup(game):
    game.map295 = None   # the frame the Map page is up after L (its panes shown from then), or None


def press(game, buttons):
    if game.map295 is not None and ("MINUS" in buttons or "B" in buttons):
        game.map295 = None
        return True
    if "L" not in buttons or not getattr(game, "mp_minus", None):
        return False
    game.mp_minus = None   # (L: co-op's page out, the Map in)
    game.map295 = game.frame + 12
    return True


def state(game, st):
    if game.map295 is None or game.frame < game.map295:
        return
    bug = os.environ.get("BT_FAKE_MAP295", "")
    shown = PANES
    if bug == "blank" or (bug == "late" and game.frame < game.map295 + LATE):
        shown = 0
    st.setdefault("menu", {})["map_page"] = {"up": True, "panes": PANES, "shown": shown}
