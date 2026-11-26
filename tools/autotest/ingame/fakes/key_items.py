"""Fake-game reactions for row 223's port (ingame/test_key_items.py; owner port-sweep-c131-10). The state member is
the module's (hookshot/src/coop_menu_state.h Minus, docs/playtest.md "menu"); counts and labels come from the same
tables the test reads (test_key_items.tally_flags / labels).

- Only on row 223's own boot (game.boot == live-key-items-223): elsewhere no hook reacts.
- MINUS opens / shuts the - menu (none of co-op's tabs yet); R on the open menu shows co-op's Multiplayer page (main
  view, details page 0); ZR / ZL turn the details list's two pages (round).
- `menu.minus` in the state: on page 1 the four key-item lines, "n of N" from the game's own flags.
- A counted flag set on one game reaches the other 12 frames later (shared flags, game_items.hpp Reduce::any).
Selftest switch BT_FAKE_KEYITEMS_BUG (tests/test_ingame_key_items.py): nominus -> the state has no `minus` member
(an older module); nosync -> B never gets A's flags; badcount -> page 2 says one song fewer than the game holds;
nopage -> ZR does not turn the page.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # (ingame/: the test's tables)
import test_key_items as K   # noqa: E402


def _bug():
    return os.environ.get("BT_FAKE_KEYITEMS_BUG", "")


def setup(game):
    game.ki = {"menu": False, "tab": False, "page": 0}
    game.ki_flags = K.tally_flags()
    game.ki_names = K.labels()
    game.ki_all = {n for g in K.GROUPS for n in game.ki_flags[g]}


def press(game, buttons):
    if game.boot != K.BOOT:   # (only on row 223's own boot: the pad is every row's, B #138)
        return False
    k = game.ki
    if "MINUS" in buttons:
        k.update(menu=not k["menu"], tab=False, page=0)
        return True
    if not k["menu"]:
        return False
    if "R" in buttons:
        k["tab"] = True
        return True
    if k["tab"] and ("ZR" in buttons or "ZL" in buttons):
        if _bug() != "nopage":
            k["page"] = 1 - k["page"]
        return True
    return False


def after(game, m, r):
    if game.boot != K.BOOT or m.get("cmd") != "flag" or m.get("name") not in game.ki_all or _bug() == "nosync":
        return
    o, name, value = game.other, m["name"], m.get("value")
    if value:
        o.later(12, lambda: o.flags.__setitem__(name, value))


def state(game, st):
    if game.boot != K.BOOT or _bug() == "nominus":
        return
    k = game.ki
    menu = st.setdefault("menu", {"coop": {"open": False}, "customize_screen": {"open": False}})
    if not (k["menu"] and k["tab"]):
        menu["minus"] = {"open": False}
        return
    m = {"open": True, "tab": "multiplayer", "view": "main", "details_page": k["page"], "details_pages": 2,
         "key_items": None}
    if k["page"] == 1:
        lines = []
        for g in K.GROUPS:
            names = game.ki_flags[g]
            n = sum(1 for f in names if game.flags.get(f))
            if _bug() == "badcount" and g == "songs" and n:
                n -= 1
            lines.append({"name": game.ki_names[g], "value": "%d of %d" % (n, len(names))})
        m["key_items"] = lines
    menu["minus"] = m
