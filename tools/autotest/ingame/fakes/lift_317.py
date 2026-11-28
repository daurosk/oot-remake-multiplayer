"""Fake-game reactions for test_lift_317.py's probe P1 (row 317, carry-revive-317's own file). They prove the probe's
logic, not the game: each line said is the module's own text (lichii/hook/coop_lift_player_game.cpp, the patterns in
test_lift_317.py); the labels "Lift" / "Throw" are only the fake's stand-ins for whatever the game shows.

Scoped (B #149): acts ONLY on a boot with LICHII_COOP_LIFT_PLAYER=1 (test_lift_317's needs_env; no other row's test
boots with it in the fakes). P2 needs nothing here (fakes/dbno_crawl.py downs B; no fake button changes A's state).
- A's state `prompt` is "Lift" with A's feet within NEAR of B (same scene) or of Ruto (InsideJabuJabuBelly), "Throw"
  while A carries; A's world list has Ruto at her placement in InsideJabuJabuBelly.
- A's A press near B: A asks, B answers yes, B's input off, B's carried pose; the next A press throws B 2 m ahead of A
  (B's input back). Near Ruto: the same carry, no lines.
Selftest switch BT_FAKE_LIFT317_BUG (tests/test_ingame_lift_317.py): noruto -> no Ruto in the world (SETUP-FAIL);
refuse -> B answers "down" (FAIL); noprompt -> no "Lift" at the copy (the probe still ends NO-REF, the notes say
DIFFERENT).
"""
import math
import os

SWITCH = "LICHII_COOP_LIFT_PLAYER=1"
JABU = "InsideJabuJabuBelly"
RUTO_AT = (1.6263792514801025, 91.25308990478516, -77.98184967041016)   # test_lift_317.RUTO_AT
NEAR = 1.0
THROW_M = 2.0


def bug(name):
    return os.environ.get("BT_FAKE_LIFT317_BUG") == name


def on(game):
    return game.env_on(SWITCH) and not game.at_title


def st317(game):
    if not hasattr(game, "l317"):
        game.l317 = {"carry": None}
    return game.l317


def flat(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def near(game):
    """'B', 'ruto' or None: what A's Link stands at."""
    if game.tag != "A" or not on(game):
        return None
    o = game.other
    if o is not None and o.scene == game.scene and flat(game.pos, o.pos) < NEAR:
        return "B"
    if game.scene == JABU and flat(game.pos, RUTO_AT) < NEAR:
        return "ruto"
    return None


def actors(game):
    if game.tag == "A" and on(game) and game.scene == JABU and not bug("noruto"):
        return [{"id": 317, "name": "Npc_Zora_Ruto_Child_001", "kind": "other", "pos": list(RUTO_AT),
                 "dist": round(flat(game.pos, RUTO_AT), 2)}]
    return []


def state(game, st):
    if game.tag != "A" or not on(game):
        return
    s = st317(game)
    if s["carry"]:
        st["prompt"] = "Throw"
        return
    at = near(game)
    if at == "ruto" or (at == "B" and not bug("noprompt")):
        st["prompt"] = "Lift"


def press(game, buttons):
    if game.tag != "A" or "A" not in buttons or not on(game):
        return False
    s, o = st317(game), game.other
    if s["carry"]:
        if s["carry"] == "B":
            yaw = math.radians(game.facing)
            o.pos = [game.pos[0] + THROW_M * math.sin(yaw), game.pos[1], game.pos[2] + THROW_M * math.cos(yaw)]
            o.later(20, lambda: o.say("coop: lift: our Link's input back (held by P1)"))
        s["carry"] = None
        return False
    at = near(game)
    if at == "ruto":
        s["carry"] = "ruto"
    elif at == "B":
        answer = "down" if bug("refuse") else "yes"
        game.later(4, lambda: game.say("coop: lift: our Link lifted P2's copy: asking"))
        o.later(6, lambda: o.say("coop: lift: P1 lifts our Link: %s" % answer))
        game.later(9, lambda: game.say("coop: lift: P2 answered our lift: %s" % answer))
        if answer == "yes":
            s["carry"] = "B"
            o.later(7, lambda: o.say("coop: lift: our Link's input off (held by P1)"))
            o.later(8, lambda: o.say("coop: lift: carried: our own Link plays 'Eaten' (its copies play 'Eaten')"))
    return False
