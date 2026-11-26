"""Difficulty #114 (test_difficulty.py): what a real lane game gives the two tests, on the fake games. Only with
BT_FAKE_DIFFICULTY set (the selftests, tests/test_ingame_difficulty.py), so no other row sees any of it:
  hard      the lobby made with Hard: 'coop: difficulty: Hard: ...' (lichii/hook/coop_difficulty_game.cpp:205, Describe
            coop_difficulty.cpp:109), the title's rows line, menus, and the Host page's kept lines
  auto      the bug: the lobby's choice lost, the game says 'Automatic (2 players): ...'
  refused   Hard, but the idle Host page refuses each change (coop_pause.cpp:1230 'difficulty refused')
BT_FAKE_DIFFICULTY_BEFORE=TEXT: A's coop-difficulty.ini holds TEXT before the run (the restore check).
BT_FAKE_DIFFICULTY_LEFT=A: game A starts in play but out of the session (no connected players), as a test run after
test_host_page_difficulty_kept would find it: the set-up's connected check must make it SETUP-FAIL (review-rows-c136).

Pages (the state's coop.page, CoopPauseMenu::PageWord): A in the pause -> main (System's Multiplayer button), A on main
-> settings (Co-op settings), B back one page; on the title after `menu leave`, A -> host (hs-title-menu.press.txt);
RIGHT on the host page = one difficulty step, kept (coop-difficulty.ini written, as Keep()) unless refused.
"""
import os

KEPT_FILE = "coop-difficulty.ini"
PRESETS = ["Automatic", "Easy", "Normal", "Hard", "Custom"]


def _mode():
    return os.environ.get("BT_FAKE_DIFFICULTY")


def setup(game):
    if not _mode():
        return
    game.dl_page, game.dl_index = None, 0
    game.dl_left = game.tag in (os.environ.get("BT_FAKE_DIFFICULTY_LEFT") or "").split(",")
    before = os.environ.get("BT_FAKE_DIFFICULTY_BEFORE")
    if game.tag == "A" and before is not None:
        with open(os.path.join(game.folder, KEPT_FILE), "w", encoding="utf-8") as f:
            f.write(before)
    game.say("coop: title: the title's own rows Host, Join, Co-op Settings (buttons 4 to 6)")
    if _mode() == "auto":
        game.say("coop: difficulty: Automatic (2 players): life 140%, damage 120%, enemies 125%; bosses: life 200%, "
                 "damage 120% (set by 0)")
    else:
        game.say("coop: difficulty: Hard: life 150%, damage 150%, enemies 150% (set by 0)")


def _rows(game):
    if game.dl_page == "main":
        return ["Host", "Join", "Co-op settings", "Leave"]
    if game.dl_page == "settings":
        return ["Difficulty", "Name", "Voice mode"]
    if game.dl_page == "host":
        out = ["Who can join", "Players", "Difficulty"]
        if PRESETS[game.dl_index] == "Custom":
            out += ["Enemy life", "Enemy damage", "Enemies"]
        return out + ["Start hosting"]
    return []


def state(game, st):
    if not _mode():
        return
    if game.dl_page:
        st["coop"]["page"] = game.dl_page
        st["coop"]["rows"] = [{"name": n, "kind": "action"} for n in _rows(game)]
    if game.dl_left:
        st["coop"]["players"] = []


def press(game, buttons):
    if not _mode():
        return False
    if game.dl_page == "host":
        if "RIGHT" in buttons and game.dl_index < len(PRESETS) - 1:
            if _mode() == "refused":
                game.later(2, lambda: game.say("coop: multiplayer tab: difficulty refused"))
            else:
                game.dl_index += 1
                with open(os.path.join(game.folder, KEPT_FILE), "w", encoding="utf-8") as f:
                    f.write("preset = %s\n" % PRESETS[game.dl_index].lower())
                game.later(2, lambda: game.say("coop: multiplayer tab: difficulty kept for the next lobby"))
        return True
    if game.at_title and game.dl_left and "A" in buttons:
        game.later(10, lambda: setattr(game, "dl_page", "host"))
        return True
    if game.menu_pause and "A" in buttons and game.dl_page in (None, "main"):
        game.dl_page = "main" if game.dl_page is None else "settings"
        return True
    if game.menu_pause and "B" in buttons and game.dl_page:
        game.dl_page = {"settings": "main", "main": None}[game.dl_page]
        return True
    return False


def act(game, words):
    if not _mode():
        return False
    if words[:2] == ["menu", "leave"]:
        game.dl_left = True
        game.later(5, lambda: game.say("coop: multiplayer tab: left the session (Leave)"))
        return True
    if words[:2] == ["menu", "close"]:
        game.dl_page = None
        return True
    return words[:2] == ["chat", "/difficulty"]
