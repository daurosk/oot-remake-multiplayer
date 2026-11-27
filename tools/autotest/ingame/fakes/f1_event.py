"""Fake-game reactions for C3-1's F1 tests (ingame/test_f1.py; owner port-g7). Hooks: fakegame.py docstring.

What a real game logs and shows, each from the module source test_f1.py cites:
- "coop: free play: yes|no (title)|no (event)" at each change (coop_hookshot.cpp DrawScene), from the state hook.
- `key` F1 (hos_coop_key, coop_hookshot.cpp): free play -> "coop: F1: the - menu's Multiplayer tab (free play)", the
  F1 run's "to the Multiplayer tab" and, 40 frames on, "the Multiplayer view" (coop_pause_tab_module.cpp); the title or
  an event -> "coop: F1: Longshot's menu on co-op's Multiplayer tab (our - menu cannot open now)" and Longshot's menu
  on co-op's tab 2 frames on (the state's menu.coop); F1 with co-op's tab showing -> not taken, longshot "close".
- "coop: Longshot's menu: N of co-op's tabs" at a change (hos_coop_menu_tabs): 2 on the title / in an event / while
  co-op's tab shows, else 0.
- The Deku Tree chest (key64 0x0b66198d17d6584e): A pressed in front of it while it is closed -> the item get (event),
  A again -> the text ends, the chest is open. Minus with the - menu open -> "CoopMenu_00 closed: its page forgotten".
BT_FAKE_F1=title|event: F1 there is Longshot's own (not taken, no co-op tab: the rc-01113 bug); =free: F1 in free play
takes Longshot's route; =freetabs: co-op's tabs in Longshot's menu in free play. The matching test must FAIL.
=stale: the 0 tab line is never logged after a title or an event (Longshot's menu does not tick): every test PASSes.
"""
import math
import os

CHEST = "key64:0x0b66198d17d6584e"
DEKU = "InsideTheDekuTree"
CHEST_XZ = (-54.4, 12.2)


def setup(game):
    game.f1_event = False        # the chest's item get is up
    game.f1_tab = False          # Longshot's menu shows co-op's tab
    game.f1_minus = False        # the game's - menu on co-op's Multiplayer tab
    game.f1_free_told = None
    game.f1_tabs_told = None
    game.flags.setdefault(CHEST, 1)          # (the lane save's chest was open: f1-event-aebd16089)
    _told(game)


def _bug(where):
    return os.environ.get("BT_FAKE_F1", "") == where


def _word(game):
    return "no (title)" if game.at_title else "no (event)" if game.f1_event else "yes"


def _told(game):
    w = _word(game)
    if w != game.f1_free_told:
        game.f1_free_told = w
        game.say("coop: free play: %s" % w)
    tabs = 2 if (game.at_title or game.f1_event or game.f1_tab) else 0
    if tabs == 0 and _bug("stale") and game.f1_tabs_told is not None:
        return       # (Longshot's menu does not tick in free play: the last line still says 2, review-rows-c136 2)
    if tabs != game.f1_tabs_told:
        game.f1_tabs_told = tabs
        game.say("coop: Longshot's menu: %d of co-op's tabs (%s)" % (
            tabs, "our - menu cannot open now, or one shows" if tabs else "co-op's home is the - menu"))


def state(game, st):
    _told(game)
    if game.f1_event and st.get("scene"):
        st["event"] = True
    st["menu"] = {"coop": {"open": True, "tab": "multiplayer", "page": "main"} if game.f1_tab else {"open": False}}


def command(game, m):
    if m.get("cmd") == "warp":       # (a warp leaves the scene: its event, the item get, ends with it)
        game.f1_event = False
        return None
    if m.get("cmd") != "key":
        return None
    if m.get("key") != "F1":
        return {"ok": True, "key": m.get("key"), "vk": 0, "taken": False, "longshot": "none"}
    head = {"ok": True, "key": "F1", "vk": 0x70}
    if game.f1_tab:
        game.f1_tab = False
        return dict(head, taken=False, longshot="close")
    where = "title" if game.at_title else "event" if game.f1_event else "free"
    if _bug(where):
        if where == "free":      # (Longshot's route in free play)
            game.say("coop: F1: Longshot's menu on co-op's Multiplayer tab (our - menu cannot open now)")
            game.later(2, lambda: setattr(game, "f1_tab", True))
            return dict(head, taken=True)
        game.say("coop: F1: Longshot's own (no - tabs of ours)")
        return dict(head, taken=False, longshot="none")
    if where == "free":
        game.say("coop: pause tab: F1: to the Multiplayer tab (Minus opens the - menu)")
        game.say("coop: F1: the - menu's Multiplayer tab (free play)")
        if _bug("freetabs"):
            game.f1_tabs_told = 2
            game.say("coop: Longshot's menu: 2 of co-op's tabs (our - menu cannot open now, or one shows)")
        game.f1_minus = True
        game.later(40, lambda: game.say("coop: pause tab: F1: the Multiplayer view, after 1 Minus press(es) and 2 tap(s)"))
        return dict(head, taken=True)
    game.say("coop: F1: Longshot's menu on co-op's Multiplayer tab (our - menu cannot open now)")
    game.later(2, lambda: setattr(game, "f1_tab", True))
    return dict(head, taken=True)


def act(game, words):
    if words[:1] == ["setflag"] and len(words) == 3 and words[1].startswith("key64:"):
        game.flags[words[1]] = int(words[2])
        return True
    return False


def press(game, buttons):
    if "MINUS" in buttons and game.f1_minus:
        game.f1_minus = False
        game.later(10, lambda: game.say("coop: pause tab: CoopMenu_00 closed: its page forgotten"))
        return True
    if "A" in buttons and game.scene == DEKU and not game.at_title:
        if game.f1_event:
            game.later(20, lambda: (setattr(game, "f1_event", False), game.flags.__setitem__(CHEST, 1)))
            return True
        near = math.hypot(game.pos[0] - CHEST_XZ[0], game.pos[2] - CHEST_XZ[1]) < 2.0
        if near and game.flags.get(CHEST) == 0:
            game.later(60, lambda: setattr(game, "f1_event", True))
            return True
    return False
