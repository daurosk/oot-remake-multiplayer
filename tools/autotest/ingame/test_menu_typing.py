"""Row 297 (Geno, 0.12.0): "typing the server name isn't bringing up the in-game keyboard, and wasn't showing the text."

Cause (hookshot/src/coop_pause_tab_module.cpp, fix menu-typing-297): co-op's menu opens on the - menu's Multiplayer tab
(FollowMenu(shown || minus_tab >= 0)), co-op's home in free play since 0.12.0, but it typed there with no on-screen keys
(menu_typing took System's page of ours alone), and nothing drew the row again either. CoopPauseTab::MenuKeysOn now
takes the - menu's tabs too.

The path driven, the one a player has in or out of a session: `menu open settings` (test_menu_pages.open_page: the
- menu, view "menu", the rows' cursor on Co-op settings, coop_pause_tab_module.cpp ShowRunPage + MinusViewChanged),
A on that row (RowAction::kMenuTab: its rows on the list, which has the pad), Room name (coop_pause_menu.cpp
Page::kSettings, a row in or out of a session: Geno's "server name" is the room name the relay finds), A. Not the Host
view's Host settings row: the lane's default boot comes with co-op on (coop_pause_tab.cpp ActionFor View::kHost:
in a session row 1 is the Join view, not Host settings).

| Check | Source |
|---|---|
| co-op's Settings page on the - menu, its row the cursor's | state menu.minus {view "menu", row 1 (kTabSettings), list}, menu.coop.page "settings" |
| Room name focused | menu.coop.focused_name (coop_menu_state.cpp TabJson; co-op's own row names only) |
| A types into it | menu.coop.typing (CoopPauseMenu View::typing) |
| the keys show (the row 297 bug: they did not) | menu.minus.keys (coop_menu_state.h Minus::keys) |
| a letter typed shows (the bug: "wasn't showing the text") | menu.minus.typed_chars one more after A on the Letter key (TypeKey::kLetter, the keys' first) |
| the shot of the keys and the letter, on both games | check_shot A and B 'row297-room-keys' (NO-REF until the owner stores a reference) |
| B cancels: no typing, no keys, still the Settings page | menu.coop.typing / menu.minus.keys false (the name as it was: CoopPauseMenu's Cancel) |

The other fields of co-op's menu (Your name, Port, Relay, the Join page's) take the same MenuKeysOn; the page's own
fields (Join's room and password, the search) type through its own keys on the - menu already (MinusPad -> TypingPad).
"""
import pytest

from test_menu_pages import close_menu, open_page, page_shown

PRESS = 8          # a press held 8 frames (test_menu_pages PRESS)
WAIT = 20          # frames after each press: the view, the list or the keys come (kTapGapMs 400 is 12 frames)
MOST_STEPS = 14    # Up, then Down, at most this many each to Room name (the Settings page has ~12 rows)
ROOM = "Room name"   # coop_pause_menu.cpp Page::kSettings' row
SETTINGS_ROW = 1     # coop_pause_tab.h kTabSettings: Co-op settings, the rows' second (kMenuTabNames)


def minus(st):
    return ((st or {}).get("menu") or {}).get("minus") or {}


def coop(st):
    return ((st or {}).get("menu") or {}).get("coop") or {}


def tap(pair, x, button):
    pair.press(x, [button], frames=PRESS)
    pair.step_both(WAIT)


def to_room_name(pair, x):
    """Game x's focus on co-op's Settings page's Room name row: Down first (the list starts at its top), then Up.
    AssertionError naming the rows seen when it is not reached."""
    seen = []
    for button in ("DOWN", "UP"):
        for _ in range(MOST_STEPS):
            name = coop(pair.state(x)).get("focused_name")
            if name == ROOM:
                return
            seen.append(name)
            tap(pair, x, button)
    raise AssertionError("%s: %r never focused on co-op's Settings page (focused: %s)" % (x, ROOM, seen))


def room_name_keys(pair, x):
    """Game x: Co-op settings on the - menu, Room name, A: typing with the keys; a letter typed shows (and the shot);
    B: no typing, no keys."""
    home, page = open_page(pair, x, "settings")
    assert (home, page) == ("minus", "settings"), "%s: Co-op settings in free play showed in %s %s" % (x, home, page)
    m = minus(pair.state(x))
    assert m.get("row") == SETTINGS_ROW, "%s: the rows' cursor is not on Co-op settings: menu.minus %s" % (x, m)
    tap(pair, x, "A")         # (RowAction::kMenuTab: its rows on the list, which has the pad)
    pair.until("%s: the list has the pad on Co-op settings (state menu.minus.list)" % x,
               lambda: minus(pair.state(x)).get("list") and page_shown(pair.state(x)) == ("minus", "settings") or None,
               max_frames=60)
    to_room_name(pair, x)
    tap(pair, x, "A")
    st = pair.until("%s: A on Room name types into it (state menu.coop.typing)" % x,
                    lambda: pair.state(x) if coop(pair.state(x)).get("typing") else None, max_frames=60)
    assert minus(st).get("keys") is True, (
        "%s: Room name is typed into but the - menu shows no typing keys (row 297): menu.minus %s" % (x, minus(st)))
    before = minus(st).get("typed_chars")
    assert isinstance(before, int) and before >= 1, "%s: the keys show no typed line: menu.minus %s" % (x, minus(st))
    tap(pair, x, "A")         # (the keys' cursor starts on Letter: its letter typed)
    pair.until("%s: the letter typed shows on the keys' line (menu.minus.typed_chars %d -> %d)" % (x, before, before + 1),
               lambda: minus(pair.state(x)).get("typed_chars") == before + 1 or None, max_frames=60)
    pair.check_shot(x, "row297-room-keys")
    tap(pair, x, "B")         # (B is typing's Cancel: the name as it was)
    st = pair.until("%s: B ended the typing (state menu.coop.typing false)" % x,
                    lambda: pair.state(x) if not coop(pair.state(x)).get("typing") else None, max_frames=60)
    assert not minus(st).get("keys"), "%s: the typing keys stayed after Cancel: menu.minus %s" % (x, minus(st))
    assert page_shown(st) == ("minus", "settings"), "%s: Cancel left the Settings page: %s" % (x, page_shown(st))
    close_menu(pair, x)


@pytest.mark.look
@pytest.mark.budget(300)   # (each game: the - menu's run <= 10 s, ~20 presses of 28 frames, the way back)
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
