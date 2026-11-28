"""Difficulty #114 (test_difficulty.py): what a real lane game gives the two tests, on the fake games. Only with
BT_FAKE_DIFFICULTY set (the selftests, tests/test_ingame_difficulty.py), so no other row sees any of it (B #149):
  hard      the lobby made with Hard: 'coop: difficulty: Hard: ...' (lichii/hook/coop_difficulty_game.cpp:205, Describe
            coop_difficulty.cpp:109), the title's own menu and the Host page's kept lines
  auto      the bug: the lobby's choice lost, the game says 'Automatic (2 players): ...'
  refused   Hard, but the idle Host page refuses each change (coop_pause.cpp:1230 'difficulty refused')
BT_FAKE_DIFFICULTY_BEFORE=TEXT: A's coop-difficulty.ini holds TEXT before the run (the restore check).
BT_FAKE_DIFFICULTY_LEFT=A: game A starts in play but out of the session (no connected players), as a test run after
test_host_page_difficulty_kept would find it: the set-up's connected check must make it SETUP-FAIL (review-rows-c136).

In play, co-op's Settings page is menu_pages.py's and menu_typing.py's (the - menu's Multiplayer tab, state menu.minus /
menu.coop, its Difficulty row): nothing of it here. On the title (port-g6-rest-5, as the module has it since 0.11.9):
the title's own menu (coop_title.cpp:1492 'Continue, New Game, Multiplayer, Options', rows CoopTitleMenu::MenuFor:
Multiplayer on button 3; its rows Join 0, Host 1, Co-op Settings 2, Back 3), DOWN moves the focus ('the title's focus is
on button N', :1517), A on a row 'row N pressed' (:1972); Host opens co-op's Host page on the title's own buttons
(coop_pause.cpp:2132 'its page on the title's own rows'; state menu.coop page "host", CoopPause::MenuTab); there
UP/DOWN move menu.coop's focus, RIGHT on Difficulty = one step, kept (coop-difficulty.ini written, as Keep()) unless
refused, B leaves the page ('the page left: Multiplayer's rows again', coop_title.cpp:1771) and menu.coop shuts.
"""
import os

KEPT_FILE = "coop-difficulty.ini"
PRESETS = ["Automatic", "Easy", "Normal", "Hard", "Custom"]
TITLE_MULTIPLAYER, SUB_HOST = 3, 1


def _mode():
    return os.environ.get("BT_FAKE_DIFFICULTY")


def setup(game):
    if not _mode():
        return
    game.dl_index, game.dl_title, game.dl_sub, game.dl_host, game.dl_focus, game.dl_row = 0, False, False, False, 0, 0
    game.dl_left = game.tag in (os.environ.get("BT_FAKE_DIFFICULTY_LEFT") or "").split(",")
    before = os.environ.get("BT_FAKE_DIFFICULTY_BEFORE")
    if game.tag == "A" and before is not None:
        with open(os.path.join(game.folder, KEPT_FILE), "w", encoding="utf-8") as f:
            f.write(before)
    if _mode() == "auto":
        game.say("coop: difficulty: Automatic (2 players): life 140%, damage 120%, enemies 125%; bosses: life 200%, "
                 "damage 120% (set by 0)")
    else:
        game.say("coop: difficulty: Hard: life 150%, damage 150%, enemies 150% (set by 0)")


def _host_rows(game):
    out = ["Who can join", "Players", "Difficulty"]
    if PRESETS[game.dl_index] == "Custom":
        out += ["Enemy life", "Enemy damage", "Enemies"]
    return out + ["Start hosting"]


def _focus(game, f):
    if f != game.dl_focus:
        game.dl_focus = f
        game.say("coop: title: the title's focus is on button %d" % f)


def _follow(game):
    """The title's own menu once a title comes; all of it gone when the game is back in play."""
    if game.at_title and not game.dl_title:
        game.dl_title, game.dl_sub, game.dl_host = True, False, False
        game.say("coop: title: the title's own menu: Continue, New Game, Multiplayer, Options (4 buttons)")
        game.dl_focus = -1
        _focus(game, 0)
    elif not game.at_title:
        game.dl_title, game.dl_sub, game.dl_host = False, False, False


def state(game, st):
    if not _mode():
        return
    _follow(game)
    if game.dl_host:
        rows = _host_rows(game)
        st.setdefault("menu", {})["coop"] = {
            "open": True, "tab": "multiplayer", "page": "host",
            "rows": [{"name": n, "kind": "action"} for n in rows], "focused": game.dl_row,
            "focused_name": rows[game.dl_row], "typing": False}
    if game.dl_left:
        st["coop"]["players"] = []


def press(game, buttons):
    if not _mode():
        return False
    _follow(game)
    if not game.dl_title:
        return False
    if game.dl_host:
        rows = _host_rows(game)
        if "DOWN" in buttons:
            game.dl_row = min(len(rows) - 1, game.dl_row + 1)
        if "UP" in buttons:
            game.dl_row = max(0, game.dl_row - 1)
        if "RIGHT" in buttons and rows[game.dl_row] == "Difficulty" and game.dl_index < len(PRESETS) - 1:
            if _mode() == "refused":
                game.later(2, lambda: game.say("coop: multiplayer tab: difficulty refused"))
            else:
                game.dl_index += 1
                with open(os.path.join(game.folder, KEPT_FILE), "w", encoding="utf-8") as f:
                    f.write("preset = %s\n" % PRESETS[game.dl_index].lower())
                game.later(2, lambda: game.say("coop: multiplayer tab: difficulty kept for the next lobby"))
        if "B" in buttons:
            game.dl_host, game.dl_sub = False, True
            game.say("coop: title: menu: the page left: Multiplayer's rows again")
            _focus(game, SUB_HOST)
        return True   # (the title gets none of it: coop_title.cpp NativePad)
    if "DOWN" in buttons:
        _focus(game, min(3, game.dl_focus + 1))
    if "UP" in buttons:
        _focus(game, max(0, game.dl_focus - 1))
    if "A" in buttons:
        if not game.dl_sub and game.dl_focus == TITLE_MULTIPLAYER:
            game.say("coop: title: menu: row %d pressed (0104)" % game.dl_focus)
            game.dl_sub = True
            _focus(game, 0)
        elif game.dl_sub and game.dl_focus == SUB_HOST:
            game.say("coop: title: menu: row %d pressed (0100)" % game.dl_focus)
            game.later(2, lambda: game.say("coop: multiplayer tab: its page on the title's own rows (no overlay)"))
            game.dl_host, game.dl_row = True, 0
        else:
            return False   # (Continue / New Game: the game's own A)
    return True


def act(game, words):
    if not _mode():
        return False
    if words[:2] == ["menu", "leave"]:
        game.dl_left = True
        game.later(5, lambda: game.say("coop: multiplayer tab: left the session (Leave)"))
        return True
    return words[:2] == ["chat", "/difficulty"]
