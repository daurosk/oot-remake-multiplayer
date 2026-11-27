"""Fake-game reactions for native puppets N5 lift carry (ingame/test_puppet_lift_carry.py; puppet-body-5). Lines are
the module's (lichii/hook/coop_puppet.cpp DrawAt).

Scoped (B #149): acts ONLY in the scene InsideJabuJabuBelly (no other row's test goes there in the fakes).
- Each game lists the moving floor Obj_InsideJabu_Jabu_MoveFloor_A_01 at its placement; once B is placed on it
  (within 1.8 m), it goes down at 2 m/s from y 91.0 to 75.0 in each game (from that game's own frame), B's Link on it.
- A's copy of B stands on the lift (offset 0.0) and A logs `P2 lift carry: on our lift, carried 0.200 m (show delay
  100 ms)` once.
Selftest switch BT_FAKE_LIFT_BUG (tests/test_ingame_puppet_lift_carry.py): nocarry -> no carry line and the copy is where
the lift was 100 ms ago (sunk/floating: FAIL); noline -> the copy rides but no line (FAIL); stuck -> the lift never moves
(SETUP-FAIL); wobble -> the line, but the copy 0.08 m off while moving (a CARRY_TOL miss: NO-REF listing it).
"""
import math
import os

SCENE = "InsideJabuJabuBelly"
NAME = "Obj_InsideJabu_Jabu_MoveFloor_A_01"
FLOOR = (0.32, 91.0, -42.63)          # test_puppet_lift_carry.py FLOOR
BOTTOM = 75.0
SPEED = 2.0 / 30                      # m per frame (2 m/s at pair.FPS 30)
DELAY_FRAMES = 3                      # 100 ms at 30 fps
LINE = "coop: puppet: P2 lift carry: on our lift, carried 0.200 m (show delay 100 ms)"


def _bug():
    return os.environ.get("BT_FAKE_LIFT_BUG", "")


def _here(game):
    return (game.scene or "") == SCENE and not game.at_title


def _lc(game):
    if not hasattr(game, "lc"):
        game.lc = {"start": None, "said": False}
    return game.lc


def lift_y(game, back=0):
    start = _lc(game)["start"]
    if start is None or _bug() == "stuck":
        return FLOOR[1]
    return max(BOTTOM, FLOOR[1] - SPEED * max(0, game.frame - back - start))


def act(game, words):
    if game.tag != "B" or words[:1] != ["place"] or not _here(game):
        return False
    p = [float(v) for v in words[1:4]]
    if math.hypot(p[0] - FLOOR[0], p[2] - FLOOR[2]) < 1.8:
        game.pos = [p[0], FLOOR[1], p[2]]
        for g in (game, game.other):
            _lc(g)["start"] = g.frame
        return True
    return False


def actors(game):
    if not _here(game):
        return []
    return [{"id": 4242, "name": NAME, "kind": "other", "pos": [FLOOR[0], lift_y(game), FLOOR[2]]}]


def state(game, st):
    if not _here(game) or _lc(game)["start"] is None:
        return
    if game.tag == "B":
        game.pos[1] = lift_y(game)
        if st.get("pos"):
            st["pos"][1] = game.pos[1]
        return
    lc = _lc(game)
    if lift_y(game) != FLOOR[1] and not lc["said"]:
        lc["said"] = True
        if _bug() not in ("nocarry", "noline"):
            game.say(LINE)
    moving = BOTTOM < lift_y(game) < FLOOR[1]
    for a in (st.get("world") or {}).get("actors") or []:
        if a.get("kind") == "player" and (a.get("name") or "").startswith("CoopPuppet_"):
            if _bug() == "nocarry":
                a["pos"][1] = lift_y(game, back=DELAY_FRAMES)
            else:
                a["pos"][1] = lift_y(game) + (0.08 if _bug() == "wobble" and moving else 0.0)
