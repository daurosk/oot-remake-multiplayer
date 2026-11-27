"""Fake-game reactions for row 297 (ingame/test_menu_typing.py). After menu_pages.py (file-name order): on its - menu
state (game.mp_minus) with view "menu" (Settings and look), co-op's pages as the module has them:
- the rows on the left are kMenuTabNames, the cursor on the page's own row as the view opens (MinusViewChanged):
  menu.minus.row; UP / DOWN move it (and the page shown, as the module's g_menu_tab follows the row); A on a row: its
  page's rows on the list, which has the pad (MinusDecide -> MinusEnterList): menu.minus.list.
- on Co-op settings' list: UP / DOWN move over ROWS (menu.coop rows, focused, focused_name, typing); A on Room name
  types (typing true, the list the keys: menu.minus.keys, typed_chars = the name + its cursor mark); while typing A
  types the Letter key's letter (typed_chars + 1), B cancels (menu_pages' B leaves it alone then).
BT_FAKE_TYPING=nokeys: the 0.12.0 bug, typing with no keys; =notext: a letter typed does not show (the bug's second
half); =nostart: A on Room name does nothing; =stuck: B does not end the typing.
test_room_name_keys_on_the_minus_menu must FAIL in all four.
"""
import os

TABS = ["customize", "settings", "commands", "local"]   # (coop_pause_tab.h kMenuTabNames' pages)
ROWS = ["Your name", "Players when you host", "Port when you host", "Relay", "Room name", "List my room",
        "Auto-rejoin", "Difficulty"]   # (coop_pause_menu.cpp Page::kSettings, shortened)
ROOM = ROWS.index("Room name")
NAME = "squad-1"   # (the fake's saved room name)


def _bug():
    return os.environ.get("BT_FAKE_TYPING", "")


def setup(game):
    game.mt_seen = None        # the mp_minus last seen (a new view puts the rows' cursor on its page's row)
    game.mt_row = -1
    game.mt_list = False
    game.mt_focus = 0
    game.mt_typing = False
    game.mt_chars = 0


def _menu(game):
    return bool(game.mp_minus) and game.mp_minus[0] == "menu"


def _follow(game):
    if game.mp_minus != game.mt_seen:
        if _menu(game) and (not game.mt_seen or game.mt_seen[0] != "menu"):
            game.mt_row = TABS.index(game.mp_minus[1]) if game.mp_minus[1] in TABS else 0
            game.mt_list, game.mt_focus, game.mt_typing = False, 0, False
        elif not _menu(game):
            game.mt_row, game.mt_list, game.mt_typing = -1, False, False
        game.mt_seen = game.mp_minus


def press(game, buttons):
    _follow(game)
    if not _menu(game):
        return False
    if game.mt_typing:
        if "A" in buttons and _bug() != "notext":
            game.mt_chars += 1
        if "B" in buttons and _bug() != "stuck":
            game.mt_typing = False
        return True   # (every press is the keys')
    if not game.mt_list:
        if "DOWN" in buttons or "UP" in buttons:
            game.mt_row = max(0, min(len(TABS) - 1, game.mt_row + (1 if "DOWN" in buttons else -1)))
            game.mp_minus = ("menu", TABS[game.mt_row])
            game.mt_seen = game.mp_minus
        if "A" in buttons:
            game.mt_list, game.mt_focus = True, 0
        return bool({"A", "UP", "DOWN"} & set(buttons))
    if game.mp_minus[1] != "settings":
        return False
    if "DOWN" in buttons:
        game.mt_focus = min(len(ROWS) - 1, game.mt_focus + 1)
    if "UP" in buttons:
        game.mt_focus = max(0, game.mt_focus - 1)
    if "A" in buttons and game.mt_focus == ROOM and _bug() != "nostart":
        game.mt_typing, game.mt_chars = True, len(NAME) + 1   # ("squad-1_": the name and the cursor's "_")
    if "B" in buttons:
        game.mt_list = False   # (the list's Back: the rows have the pad; menu_pages' B also goes a view back)
    return bool({"A", "UP", "DOWN"} & set(buttons))


def typing(game):
    """menu_pages.press asks it: while typing, B is the keys' Cancel, not a view back."""
    return bool(getattr(game, "mt_typing", False))


def state(game, st):
    _follow(game)
    menu = st.setdefault("menu", {})
    m = menu.get("minus")
    if m and m.get("open"):
        keys = _menu(game) and game.mt_typing and _bug() != "nokeys"
        m.update(keys=keys, typed_chars=game.mt_chars if keys else -1, row=game.mt_row if _menu(game) else 0,
                 list=bool(_menu(game) and game.mt_list))
    if _menu(game) and game.mp_minus[1] == "settings":
        menu["coop"] = {"open": True, "tab": "multiplayer", "page": "settings",
                        "rows": [{"name": n, "kind": "action"} for n in ROWS], "focused": game.mt_focus,
                        "focused_name": ROWS[game.mt_focus], "typing": game.mt_typing}
