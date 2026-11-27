"""Fake-game reactions for row 308's probe (ingame/test_bomb_pocket_308.py; owner carry-sync-304). Only while
BT_FAKE_BOMB_308 is set (tests/test_ingame_bomb_pocket_308.py): nothing here acts in any other run (B #149).

- The switch line at the boot: coop_spawn.cpp:2381 'coop: bombs: LICHII_COOP_BOMB_SHARE=1: a lifted bomb changes hands'.
- give bombs: PouchData.Item_Bomb.StockNum 20 (docs/playtest.md give: 'the small bag and 20 bombs').
- ZR: a bomb in the presser's hands (1.2 m over its Link), its count -1; A while holding: put down 0.8 m ahead; A near a
  lying bomb: lifted; B while holding: put away (the count +1, the bomb gone on both games).
- place X Y Z: 'coop: test place X Y Z: done'; face X Z: the turn line (as fakes/carry_304.py).
Selftest switch BT_FAKE_BOMB_308: ok -> as above; nopocket -> B does nothing (the game has no put-away: a FAIL);
both -> the put-away also gives the thrower +1 (a FAIL: the thrower's count changed).
"""
import math
import os

COUNT = "PouchData.Item_Bomb.StockNum"
SHARE_LINE = "coop: bombs: LICHII_COOP_BOMB_SHARE=1: a lifted bomb changes hands"
REACH_M, UP = 1.2, 1.2


def mode():
    return os.environ.get("BT_FAKE_BOMB_308", "")


def _lane(game):
    return game if game.tag == "A" else game.other


def setup(game):
    game.b308 = {"bomb": None, "holder": None}    # (on A: the one shared bomb, [x, y, z] when lying; holder tag)
    if mode():
        game.say(SHARE_LINE)


def after(game, m, r):
    if mode() and m.get("cmd") == "give" and m.get("item") == "bombs":
        game.flags[COUNT] = 20


def act(game, words):
    if not mode():
        return False
    if words[:1] == ["place"] and len(words) == 4:
        game.pos = [float(v) for v in words[1:4]]
        game.say("coop: test place %s %s %s: done" % tuple(words[1:4]))
        return True
    if words[:1] == ["face"] and len(words) == 3:
        dx, dz = float(words[1]) - game.pos[0], float(words[2]) - game.pos[2]
        game.facing = math.degrees(math.atan2(dx, dz)) % 360
        game.say("coop: test face %s %s: Link at %.2f %.2f turned to yaw %.1f deg" % (
            words[1], words[2], game.pos[0], game.pos[2], game.facing))
        return True
    return False


def press(game, buttons):
    if not mode():
        return False
    s = _lane(game).b308
    if "ZR" in buttons and s["holder"] is None and s["bomb"] is None and game.flags.get(COUNT, 0) > 0:
        game.flags[COUNT] -= 1
        s["holder"] = game.tag
    elif "A" in buttons and s["holder"] == game.tag:
        yaw = math.radians(game.facing)
        s["bomb"], s["holder"] = [game.pos[0] + 0.8 * math.sin(yaw), game.pos[1], game.pos[2] + 0.8 * math.cos(yaw)], None
    elif "A" in buttons and s["holder"] is None and s["bomb"] and \
            math.hypot(game.pos[0] - s["bomb"][0], game.pos[2] - s["bomb"][2]) <= REACH_M:
        s["bomb"], s["holder"] = None, game.tag
    elif "B" in buttons and s["holder"] == game.tag and mode() != "nopocket":
        s["holder"] = None
        game.flags[COUNT] = game.flags.get(COUNT, 0) + 1
        if mode() == "both":
            game.other.flags[COUNT] = game.other.flags.get(COUNT, 0) + 1
    return False


def actors(game):
    if not mode():
        return []
    s = _lane(game).b308
    if s["holder"]:
        h = game if s["holder"] == game.tag else game.other
        return [{"id": 3081, "name": "Item_Bomb", "kind": "other", "pos": [h.pos[0], h.pos[1] + UP, h.pos[2]]}]
    if s["bomb"]:
        return [{"id": 3081, "name": "Item_Bomb", "kind": "other", "pos": list(s["bomb"])}]
    return []
