"""Fake-game reactions of row 298 (ingame/test_party_wait.py; party-wait-298's file). They prove the test's logic, not
the game. Each line said is the module's own text (the patterns' writers, cited in test_party_wait.py).

Reacts only once A said 'chat /wait ...' this boot (the row's own command; every test of the row does, first): both
games then log the server's PARTY_WAIT_MODE line, and after every step / input / move / warp of a game with the setting
on, that game runs the rules of coop_party_wait.cpp Waiter::Frame on its own place: the boss door moment = in Gohma's
area ring of the Deku Tree (test_party_wait.ring, the module's sums), the other player here = same scene within 10 m
(CoopTeleport kNearMove, no room table in the fake). The clock is the fake's frames at pair.FPS 30.

BT_FAKE_PARTY_WAIT_BUG (selftests, tests/test_ingame_party_wait.py):
  nowait     no wait line ever (the moment never found)
  nogiveup   the wait never ends by time
  always     a wait line even with the setting off
  room       the corridor (inside Gohma's room) is a moment too
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import test_party_wait as T   # noqa: E402  (the one copy of the room's numbers and the ring sums)

FPS = 30
NEAR_M = 10.0
GIVE_UP_F, RECOUNT_F = 60 * FPS, 3 * FPS   # coop_party_wait.h:46-47 kGiveUpUs, kRecountUs
WHERE = "the boss door (Queen Gohma)"


def bug(name):
    return os.environ.get("BT_FAKE_PARTY_WAIT_BUG") == name


def setup(game):
    game.pw = {"here": False, "on": False, "scene": None, "done": set(), "wait": None}


def say(game, line):
    game.say("coop: party wait: %s" % line)


def act(game, words):
    if words[:2] != ["chat", "/wait"] or game.tag != "A" or len(words) < 3:
        return False
    on = words[2] == "on"
    for g in (game, game.other):
        g.pw["here"] = True
        g.pw["on"] = on
        if not on and g.pw["wait"]:
            g.pw["wait"], g.pw["done"] = None, set()
        g.later(5, lambda g=g: g.say("coop: party wait: %s (set by 1)" % ("on" if on else "off")))
    return False


def moment(game):
    if bug("nowait") or game.scene != T.SCENE or game.pos is None:
        return None
    p = game.pos
    if T.ring(p):
        return "boss:%s:0" % T.SCENE
    if bug("room") and math.hypot(p[0] - T.CX, p[2] - T.CZ) <= T.RADIUS:
        return "boss:%s:0" % T.SCENE
    return None


def frame(game):
    w = game.pw
    if not w["on"] and bug("always") and moment(game) and not w.get("said_off"):
        w["said_off"] = True
        say(game, "Waiting for the other players to sync... (1/2 here)")
    if not w["on"]:
        # coop_party_wait_game.cpp Frame: off -> waiter.Reset() every frame (no wait, no moment kept, no scene)
        w["wait"], w["done"], w["scene"] = None, set(), None
        return
    if game.scene != w["scene"]:
        w["scene"], w["done"], w["wait"] = game.scene, set(), None
    o = game.other
    here = 1 if (o.scene == game.scene and o.pos is not None and game.pos is not None and
                 math.hypot(o.pos[0] - game.pos[0], o.pos[2] - game.pos[2]) <= NEAR_M) else 0
    m, now = moment(game), game.frame
    if w["wait"]:
        if m != w["wait"]["id"]:
            w["wait"] = None
        elif here == 1:
            say(game, "Everyone is here.")
            w["wait"] = None
        elif now - w["wait"]["since"] >= GIVE_UP_F and not bug("nogiveup"):
            say(game, "Not everyone is here: going on.")
            w["wait"] = None
        return
    if not m or m in w["done"]:
        return
    w["done"].add(m)
    if here == 1:
        return
    w["wait"] = {"id": m, "since": now}
    say(game, "Waiting for the other players to sync... (1/2 here)")
    game.say("coop: party wait: said to the others: Waiting for the party at %s" % WHERE)


def after(game, m, r):
    if game.pw["here"] and m.get("cmd") in ("step", "input", "move", "warp"):
        frame(game)
