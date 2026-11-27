"""Fake-game reactions for row 305's probe (ingame/test_carry_boulder_305.py; owner carry-boulder-305). Only while
BT_FAKE_CARRY_305 is set (tests/test_ingame_carry_305.py) AND the fake game is in GoronCity_Adult: nothing here acts
in any other run or scene (B #149).

- place X Y Z / face X Z: the module's own lines (coop_testinput.cpp:1800, coop_boss_game.cpp:152).
- The world list has one Obj_LiftRock_B_01 BOULDER_AT; A pressing A within REACH_M of it lifts it after LIFT_DELAY
  steps of the bridge (a big lift is slower), then A's boulder is HANDS_UP over A's Link.
Selftest switch BT_FAKE_CARRY_305: ok -> B's copy is in A's hands too (the fixed game); rest -> B's copy stays at its
rest place (row 305 as the owner saw it); nolift -> A never lifts it (SETUP-FAIL).
"""
import math
import os

SCENE = "GoronCity_Adult"
BOULDER_AT = [6.0, 116.0, 30.0]
REACH_M, HANDS_UP = 1.2, 1.6
LIFT_DELAY = 3


def mode():
    return os.environ.get("BT_FAKE_CARRY_305", "")


def _a(game):
    return game if game.tag == "A" else game.other


def _here(game):
    return bool(mode()) and _a(game).scene == SCENE


def setup(game):
    game.c305 = {"press": None, "dumps": 0}


def act(game, words):
    if not _here(game):
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
    if not _here(game) or game.tag != "A" or "A" not in buttons or game.c305["press"] is not None:
        return False
    if mode() != "nolift" and math.hypot(game.pos[0] - BOULDER_AT[0], game.pos[2] - BOULDER_AT[2]) <= REACH_M:
        game.c305["press"] = game.c305["dumps"]
    return False


def _held(a):
    return a.c305["press"] is not None and a.c305["dumps"] - a.c305["press"] >= LIFT_DELAY


def actors(game):
    if not _here(game):
        return []
    a = _a(game)
    if game.tag == "A":
        a.c305["dumps"] += 1
    hands = [a.pos[0], a.pos[1] + HANDS_UP, a.pos[2]]
    if game.tag == "A":
        return [{"id": 3051, "name": "Obj_LiftRock_B_01", "kind": "other", "pos": hands if _held(a) else list(BOULDER_AT)}]
    held = _held(a) and mode() == "ok"
    return [{"id": 3151, "name": "Obj_LiftRock_B_01", "kind": "other", "pos": hands if held else list(BOULDER_AT)}]
