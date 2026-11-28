"""Fake-game reactions for row 305's probe (ingame/test_carry_boulder_305.py; owner carry-boulder-305). Only while
BT_FAKE_CARRY_305 is set (tests/test_ingame_carry_305.py) AND (but for the LiftGrade answer) the fake game is in one of
the test's adult scenes: nothing here acts in any other run (B #149).

- place X Y Z / face X Z: the module's own lines (coop_testinput.cpp:1800, coop_boss_game.cpp:152).
- The world list has one Obj_LiftRock_B_01 BOULDER_AT; A pressing A within REACH_M of it lifts it after LIFT_DELAY
  steps of the bridge (a big lift is slower), then A's boulder is HANDS_UP over A's Link.
Selftest switch BT_FAKE_CARRY_305: ok -> B's copy is in A's hands too (the fixed game); rest -> B's copy stays at its
rest place (row 305 as the owner saw it); nolift -> A never lifts it (SETUP-FAIL); later -> as ok, but the boulder is
in GerudoValley_Adult only (the run of 04:23 UTC 11-28 saw none in the city: the test must go on to the next place);
none -> no boulder at any place (SETUP-FAIL naming each place's findings).
- PlayerStatus.LiftGrade is an enum: flag_read gives its value's hash (docs/playtest.md flag_read), starting at
  GRADE_START; flag takes a value name or a hash read before, so the test's restore after it is seen.
"""
import math
import os

SCENE = "GoronCity_Adult"
LATER_SCENE = "GerudoValley_Adult"
ADULT = ("GoronCity_Adult", "GerudoValley_Adult", "DesertColossus_Adult")
BOULDER_AT = [6.0, 116.0, 30.0]
REACH_M, HANDS_UP = 1.2, 1.6
LIFT_DELAY = 3
LIFT = "PlayerStatus.LiftGrade"
GRADE_START = "0x0000305a"     # (a fake hash: the real one is whatever the game's flag_read gives)


def mode():
    return os.environ.get("BT_FAKE_CARRY_305", "")


def _a(game):
    return game if game.tag == "A" else game.other


def _here(game):
    """The test's places (adult scenes) while the switch is on: place/face answer there."""
    return bool(mode()) and _a(game).scene in ADULT


def _boulder_here(game):
    m = mode()
    return _here(game) and m != "none" and _a(game).scene == (LATER_SCENE if m == "later" else SCENE)


def setup(game):
    game.c305 = {"press": None, "dumps": 0, "grade": GRADE_START}


def command(game, m):
    # (read before the warp, in the boot scene: the switch alone scopes it)
    if not mode() or m.get("name") != LIFT or m.get("cmd") not in ("flag", "flag_read"):
        return None
    if m["cmd"] == "flag_read":
        return {"ok": True, "kind": "enum", "value": game.c305["grade"]}
    was, game.c305["grade"] = game.c305["grade"], str(m["value"])
    return {"ok": True, "was": was, "value": game.c305["grade"]}


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
    if not _boulder_here(game) or game.tag != "A" or "A" not in buttons or game.c305["press"] is not None:
        return False
    if mode() != "nolift" and math.hypot(game.pos[0] - BOULDER_AT[0], game.pos[2] - BOULDER_AT[2]) <= REACH_M:
        game.c305["press"] = game.c305["dumps"]
    return False


def _held(a):
    return a.c305["press"] is not None and a.c305["dumps"] - a.c305["press"] >= LIFT_DELAY


def actors(game):
    if not _boulder_here(game):
        return []
    a = _a(game)
    if game.tag == "A":
        a.c305["dumps"] += 1
    hands = [a.pos[0], a.pos[1] + HANDS_UP, a.pos[2]]
    if game.tag == "A":
        return [{"id": 3051, "name": "Obj_LiftRock_B_01", "kind": "other", "pos": hands if _held(a) else list(BOULDER_AT)}]
    held = _held(a) and mode() in ("ok", "later")
    return [{"id": 3151, "name": "Obj_LiftRock_B_01", "kind": "other", "pos": hands if held else list(BOULDER_AT)}]
