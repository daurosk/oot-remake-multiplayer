"""Row 297 (Geno, 0.12.0): "typing the server name isn't bringing up the in-game keyboard, and wasn't showing the text."

The path a player has in free play (co-op's home is the - menu, test_menu_pages): the Multiplayer tab's Host view, its
Host settings row (RowAction::kViewHostSettings: co-op's Settings page on the list, the list holding the pad), the Room
name row, A. Cause (hookshot/src/coop_pause_tab_module.cpp, fix menu-typing-297): co-op's menu typed there with no
on-screen keys (menu_typing took System's page of ours alone), and so nothing drew the row again either.
CoopPauseTab::MenuKeysOn now takes the - menu's tabs too.

| Check | Source |
|---|---|
| the Settings page on the - menu from Host > Host settings | state menu.minus {view "menu"}, menu.coop {page "settings"} (test_menu_pages.page_shown) |
| Room name focused | menu.coop.focused_name (coop_menu_state.cpp TabJson; co-op's own row names only) |
| A types into it | menu.coop.typing (CoopPauseMenu View::typing) |
| the keys show (the row 297 bug: they did not) | menu.minus.keys (coop_menu_state.h Minus::keys: the list is the typing keys) |
| the shot of the keys, on both games | check_shot A and B 'row297-room-keys' (NO-REF until the owner stores a reference) |
| B cancels: no typing, no keys, the name as it was | menu.coop.typing / menu.minus.keys false; the row's value is never in the state |

The other fields of co-op's menu (Your name, Port, Relay, the Join page's) take the same MenuKeysOn; the page's own
fields (Join's room and password, the search) type through its own keys on the - menu already (MinusPad -> TypingPad).
"""
import pytest

from test_menu_pages import close_menu, open_page, page_shown

PRESS = 8          # a press held 8 frames (test_menu_pages PRESS)
WAIT = 20          # frames after each press: the view, the list or the keys come (kTapGapMs 400 is 12 frames)
MOST_STEPS = 14    # Up, then Down, at most this many each to Room name (the Settings page has ~12 rows)
ROOM = "Room name"   # coop_pause_menu.cpp Page::kSettings' row


def minus(st):
    return ((st or {}).get("menu") or {}).get("minus") or {}


def coop(st):
    return ((st or {}).get("menu") or {}).get("coop") or {}


def tap(pair, x, button):
    pair.press(x, [button], frames=PRESS)
    pair.step_both(WAIT)


def to_room_name(pair, x):
    """Game x's focus on co-op's Settings page's Room name row: Up first (Host settings selects the host rules,
    below it), then Down. AssertionError naming the rows seen when it is not reached."""
    seen = []
    for button in ("UP", "DOWN"):
        for _ in range(MOST_STEPS):
            name = coop(pair.state(x)).get("focused_name")
            if name == ROOM:
                return
            seen.append(name)
            tap(pair, x, button)
    raise AssertionError("%s: %r never focused on co-op's Settings page (focused: %s)" % (x, ROOM, seen))


def room_name_keys(pair, x):
    """Game x: Host, Host settings, Room name, A: typing with the keys (and its shot); B: no typing, no keys."""
    home, _ = open_page(pair, x, "host")
    assert home == "minus", "%s: the Host page in free play showed in %s, not the - menu" % (x, home)
    tap(pair, x, "DOWN")    # (the Host view, not hosting: 0 Start hosting, 1 Host settings)
    tap(pair, x, "A")
    pair.until("%s: Host settings gave co-op's Settings page on the - menu (state menu.minus / menu.coop)" % x,
               lambda: page_shown(pair.state(x)) == ("minus", "settings") or None, max_frames=90)
    to_room_name(pair, x)
    tap(pair, x, "A")
    st = pair.until("%s: A on Room name types into it (state menu.coop.typing)" % x,
                    lambda: pair.state(x) if coop(pair.state(x)).get("typing") else None, max_frames=60)
    assert minus(st).get("keys") is True, (
        "%s: Room name is typed into but the - menu shows no typing keys (row 297): menu.minus %s" % (x, minus(st)))
    pair.check_shot(x, "row297-room-keys")
    tap(pair, x, "B")       # (B is typing's Cancel: the name as it was)
    st = pair.until("%s: B ended the typing (state menu.coop.typing false)" % x,
                    lambda: pair.state(x) if not coop(pair.state(x)).get("typing") else None, max_frames=60)
    assert not minus(st).get("keys"), "%s: the typing keys stayed after Cancel: menu.minus %s" % (x, minus(st))
    assert page_shown(st) == ("minus", "settings"), "%s: Cancel left the Settings page: %s" % (x, page_shown(st))
    close_menu(pair, x)


@pytest.mark.look
@pytest.mark.budget(300)   # (each game: the - menu's run <= 10 s, ~30 presses of 28 frames, the way back)
@pytest.mark.row("#297: typing the room name on the - menu's Multiplayer tab brings up the typing keys (Geno, 0.12.0)")
def test_room_name_keys_on_the_minus_menu(pair):
    with pair.setup():
        for x in ("A", "B"):
            assert pair.in_play(pair.state(x)), "%s not in play" % x
            assert not page_shown(pair.state(x)), "%s: co-op's page shows before the test" % x
    done = False
    try:
        for x in ("B", "A"):
            room_name_keys(pair, x)
        done = True
    finally:
        for x in ("B", "A"):
            try:
                if page_shown(pair.state(x)):
                    close_menu(pair, x)
            except Exception as e:
                if done:
                    raise
                pair.notes.append("%s: way back failed after the test's FAIL: %s: %s" % (x, type(e).__name__, e))
