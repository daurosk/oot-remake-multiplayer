"""Fake-game reactions for row 297 (ingame/test_menu_typing.py). After menu_pages.py (file-name order): on its - menu
state (game.mp_minus), the Host view's rows and co-op's Settings page reached by its Host settings row.

- view "host" (not hosting): row 0 Start hosting, row 1 Host settings; DOWN / UP move, A on row 1 gives ("menu",
  "settings") with the focus on the host rules (ROWS' last), as RowAction::kViewHostSettings + NativeSelectHostRules.
- ("menu", "settings"): UP / DOWN move over ROWS (menu.coop rows, focused, focused_name, typing); A on Room name types
  (typing true, the - menu's list the keys: menu.minus.keys); while typing B cancels (menu_pages' B does nothing then).
BT_FAKE_TYPING=nokeys: the 0.12.0 bug, typing with no keys; =nostart: A on Room name does nothing;
=stuck: B does not end the typing. test_room_name_keys_on_the_minus_menu must FAIL in all three.
"""
import os

ROWS = ["Your name", "Players when you host", "Port when you host", "Relay", "Room name", "List my room",
        "Auto-rejoin", "Difficulty"]   # (coop_pause_menu.cpp Page::kSettings, shortened; the host rules last)
ROOM = ROWS.index("Room name")


def _bug():
    return os.environ.get("BT_FAKE_TYPING", "")


def setup(game):
    game.mt_host_row = 0       # the Host view's cursor
    game.mt_focus = None       # co-op's Settings page's focused row (ROWS), None while it does not show
    game.mt_typing = False


def _view(game):
    return game.mp_minus[0] if game.mp_minus else None


def press(game, buttons):
    view = _view(game)
    if view != "menu" or game.mp_minus[1] != "settings":
        game.mt_focus, game.mt_typing = None, False
    if view == "host":
        if "DOWN" in buttons:
            game.mt_host_row = min(1, game.mt_host_row + 1)
        if "UP" in buttons:
            game.mt_host_row = max(0, game.mt_host_row - 1)
        if "A" in buttons and game.mt_host_row == 1:
            game.later(5, lambda: (setattr(game, "mp_minus", ("menu", "settings")),
                                   setattr(game, "mt_focus", len(ROWS) - 1)))
        return "A" in buttons or "DOWN" in buttons or "UP" in buttons
    if view == "menu" and game.mp_minus[1] == "settings" and game.mt_focus is not None:
        if game.mt_typing:
            if "B" in buttons and _bug() != "stuck":
                game.mt_typing = False
            return True   # (every press is the keys')
        if "DOWN" in buttons:
            game.mt_focus = min(len(ROWS) - 1, game.mt_focus + 1)
        if "UP" in buttons:
            game.mt_focus = max(0, game.mt_focus - 1)
        if "A" in buttons and game.mt_focus == ROOM and _bug() != "nostart":
            game.mt_typing = True
        return bool({"A", "UP", "DOWN"} & set(buttons))
    if view != "host":
        game.mt_host_row = 0
    return False


def typing(game):
    """menu_pages.press asks it: while typing, B is the keys' Cancel, not a view back."""
    return bool(getattr(game, "mt_typing", False))


def state(game, st):
    if _view(game) != "menu" or game.mp_minus[1] != "settings" or game.mt_focus is None:
        m = ((st.get("menu") or {}).get("minus"))
        if m and m.get("open"):
            m.setdefault("keys", False)
        return
    menu = st.setdefault("menu", {})
    menu["coop"] = {"open": True, "tab": "multiplayer", "page": "settings",
                    "rows": [{"name": n, "kind": "action"} for n in ROWS], "focused": game.mt_focus,
                    "focused_name": ROWS[game.mt_focus], "typing": game.mt_typing}
    menu.setdefault("minus", {"open": True, "tab": "multiplayer", "view": "menu"})
    menu["minus"]["keys"] = game.mt_typing and _bug() != "nokeys"
