"""Fake-game reactions for the page step (ingame/test_menu_pages.py; owner port-g7). Hooks: fakegame.py docstring.

`act menu open PAGE` (coop_testinput.cpp, the menu request): "coop: test menu open PAGE: asked" at once, then 3 frames
later co-op's tab of Longshot's menu shows that page (CoopPause::Open's want, taken at Longshot's next menu tick), in
the state's menu.coop {open, tab "multiplayer", page}; `menu main` the same as `menu open main` (row 170); `menu close`
shuts it. Other menu words (host, paste, leave, customize) are other rows' steps and stay as they were.
BT_FAKE_MENU=nopage: a page step is asked but no page comes (the gap B #138 closed); =wrong: every page step opens the
main page. test_every_page_opens_on_its_own must FAIL in both.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from hookshot import MENU_PAGES as PAGES   # noqa: E402  (coop_testinput.h kMenuPages, the runner's checked copy)


def setup(game):
    game.mp_page = None


def _open(game, page, said):
    game.say("coop: test menu %s: asked" % said)
    mode = os.environ.get("BT_FAKE_MENU", "")
    if mode != "nopage":
        shown = "main" if mode == "wrong" else page
        game.later(3, lambda: setattr(game, "mp_page", shown))


def act(game, words):
    if words[:1] != ["menu"]:
        return False
    w = [x.lower() for x in words[1:]]
    if w == ["close"]:
        game.say("coop: test menu close: asked")
        game.later(3, lambda: setattr(game, "mp_page", None))
        return True
    if w == ["main"]:
        _open(game, "main", "main")
        return True
    if len(w) == 2 and w[0] == "open" and w[1] in PAGES:
        _open(game, w[1], "open " + w[1])
        return True
    return False


def state(game, st):
    if game.mp_page and st.get("scene"):
        st.setdefault("menu", {})["coop"] = {"open": True, "tab": "multiplayer", "page": game.mp_page}
    elif "menu" not in st:
        st["menu"] = {"coop": {"open": False}}
