"""Fake-game reactions for the page step (ingame/test_menu_pages.py; owner port-g7, home rule C #145). Hooks: fakegame.py
docstring.

`act menu open PAGE` (coop_testinput.cpp, the menu request) says "coop: test menu open PAGE: asked" at once; `menu main`
is `menu open main` (row 170), `menu customize` `menu open customize`. Where the page then shows is F1's rule
(coop_pause.cpp OpenAsPlayer, coop_menu_tabs_rule.h OnF1):
- free play (a scene, not the title, no event): the game's - menu, co-op's Multiplayer tab, 10 frames later (F1's run:
  Minus, the strip, the taps), on the page's view: state menu.minus {open, tab "multiplayer", view} with view main,
  join, host, lan (find) or menu (customize, settings, commands, local: Settings and look, which shows co-op's own page
  natively, so menu.coop {open, page} too). B goes back a view (coop_pause_tab.cpp BackOf: to main), B on main shuts
  the - menu.
- elsewhere (the title, an event): co-op's tab of Longshot's menu, 3 frames later (CoopPause::Open's want): menu.coop
  {open, tab "multiplayer", page}; `menu close` shuts it (HOS_COOP_MENU_WANT_CLOSE, which never shuts the - menu).
BT_FAKE_MENU=old: the f9f3b9c26 path: every page wants Longshot's co-op tab, which Longshot's menu does not carry in free
play (LongshotTabs 0; vk_menu.cpp takes a want only below its tab count), so in free play nothing shows.
BT_FAKE_MENU=nopage: a page step is asked but no page comes; =wrong: every page step opens the main page;
=longshot: in free play the page shows in Longshot's tab (a second home). test_every_page_opens_on_its_own must FAIL in
all four.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from hookshot import MENU_PAGES as PAGES   # noqa: E402  (coop_testinput.h kMenuPages, the runner's checked copy)

MINUS_VIEW = {"main": "main", "join": "join", "host": "host", "find": "lan",
              "customize": "menu", "settings": "menu", "commands": "menu", "local": "menu"}


def setup(game):
    game.mp_page = None        # co-op's tab of Longshot's menu: its page, or None (shut)
    game.mp_minus = None       # the - menu's Multiplayer tab: (view, page), or None (shut)


def _free_play(game):
    return bool(game.scene) and not game.at_title and not game.event


def _open(game, page, said):
    game.say("coop: test menu %s: asked" % said)
    mode = os.environ.get("BT_FAKE_MENU", "")
    if mode == "nopage":
        return
    shown = "main" if mode == "wrong" else page
    if not _free_play(game):
        game.later(3, lambda: setattr(game, "mp_page", shown))
    elif mode == "longshot":
        game.later(3, lambda: setattr(game, "mp_page", shown))
    elif mode != "old":   # (old: the want is dropped, nothing shows)
        game.later(10, lambda: setattr(game, "mp_minus", (MINUS_VIEW[shown], shown)))


def act(game, words):
    if words[:1] != ["menu"]:
        return False
    w = [x.lower() for x in words[1:]]
    if w == ["close"]:
        game.say("coop: test menu close: asked")
        game.later(3, lambda: setattr(game, "mp_page", None))
        return True
    if w in (["main"], ["customize"]):
        _open(game, w[0], w[0])
        return True
    if len(w) == 2 and w[0] == "open" and w[1] in PAGES:
        _open(game, w[1], "open " + w[1])
        return True
    return False


def press(game, buttons):
    if "B" not in buttons or not game.mp_minus:
        return False
    if getattr(game, "mt_typing", False):   # (row 297, menu_typing.py: B is the typing keys' Cancel, not a view back)
        return False
    view, _ = game.mp_minus
    game.mp_minus = ("main", "main") if view != "main" else None
    return True


def state(game, st):
    menu = st.setdefault("menu", {})
    if game.mp_minus and st.get("scene"):
        view, page = game.mp_minus
        menu["minus"] = {"open": True, "tab": "multiplayer", "view": view}
        if view == "menu":   # (Settings and look: co-op's own page shown natively, CoopPause::OpenNative)
            menu["coop"] = {"open": True, "tab": "multiplayer", "page": page}
    if game.mp_page and st.get("scene"):
        menu["coop"] = {"open": True, "tab": "multiplayer", "page": game.mp_page}
    menu.setdefault("coop", {"open": False})   # (minus only while ours shows: key_items' nominus case has none)
