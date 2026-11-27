"""Fake-game reactions for row 299's probe (ingame/test_pause_world.py; hookshot/src/coop_pause_world.h). Hooks:
fakegame.py docstring.

- state: `pause_world` as the module gives it: a manager with 3 holds; while the + menu is open (the fake's own
  menu_pause, toggled by PLUS in fakegame.py) hold 1 (key 0x5a5a) has count 1, unless a test let it go.
- act `menu hold release N`: the module's log line, and the hold's count to 0 until the menu shuts.
BT_FAKE_PAUSE_WORLD=nohold: the + menu takes no hold (the probe must FAIL: "took no PauseMgr hold").
Only the probe reads `pause_world` and only it sends `menu hold release`, so no other row's test sees a change.
"""
import os

KEY = 0x5a5a
INDEX = 1


def setup(game):
    game.pw_released = False


def act(game, words):
    w = [x.lower() for x in words]
    if w[:3] != ["menu", "hold", "release"] or len(w) != 4:
        return False
    game.say("coop: test menu hold release %s: asked" % w[3])
    was = 1 if (game.menu_pause and not game.pw_released and w[3] == str(INDEX)) else 0
    game.say("coop: pause world: test release %s key 0x%x: count %d -> 0, notify 0x80001000 called" % (w[3], KEY, was))
    game.pw_released = True
    return True


def press(game, buttons):
    if "PLUS" in buttons and game.menu_pause:   # (fakegame.py toggles menu_pause after the hooks: this press shuts it)
        game.pw_released = False
    return False


def state(game, st):
    held = []
    if game.menu_pause and not game.pw_released and os.environ.get("BT_FAKE_PAUSE_WORLD", "") != "nohold":
        held.append({"index": INDEX, "key": "0x%x" % KEY, "count": 1})
    st["pause_world"] = {"ok": True, "manager": "0x86000000", "holds": 3, "words": [0, 0, 0, 0], "held": held}
