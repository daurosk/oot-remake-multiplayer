"""Fake-game reactions of row 234 by position (ingame/test_volvagia_rocks.py; volvagia-rocks-234).

In VolvagiaStage with LICHII_COOP_DEBUG_OBJECTS=1 (the rocks lines themselves come from fakes/boss_respawn.py): both
games arrive at the stage's start (coop_start_points.inc), `place` logs the module's line (coop_testinput.cpp
"coop: test place x y z: done"), and the world list has the arena's pool: 30 sleeping Obj_FallingRock_A_06 (they never
move) plus, from 300 frames after the arrival, one rock every 40 frames falling at this game's own Link
(0.5 m a frame from 15 m up). BT_FAKE_BROKEN values (selftests):
  vr-one-target   both games drop their rocks at A's Link (one target for both: the bug the row looks for)
  vr-asleep       the pool never wakes: no rock falls (the rocks lines still come)
  vr-lava         B's place lands it 10 m down (not the floor): SETUP-FAIL 'stand apart'
  vr-busy         B has no fresh Link sample for 90 frames after its place (an attack/landing): past the 60-frame
                  settle, so one read would SETUP-FAIL; the set-up must step until both are in play
"""
import os

STAGE = "VolvagiaStage"
START = [-64.51, 102.58, -0.01]               # coop_start_points.inc
POOL_AT = [-80.0, 80.0, 16.0]                 # (fake) where the sleeping pool waits
FALL_FROM, FALL_SPEED, EVERY, FIRST = 15.0, 0.5, 40, 300


def broken():
    return os.environ.get("BT_FAKE_BROKEN", "")


def on(game):
    return game.scene == STAGE and game.env_on("LICHII_COOP_DEBUG_OBJECTS=1")


def setup(game):
    game.vr_t0 = None


def after(game, m, r):
    if m.get("cmd") == "warp" and m.get("scene") == STAGE:
        game.pos = list(START)
        game.vr_t0 = game.frame
        if isinstance(r.get("state"), dict):
            r["state"]["pos"] = list(START)


def act(game, words):
    if words[:1] == ["place"] and game.scene == STAGE and len(words) == 4:
        game.pos = [float(v) for v in words[1:4]]
        if broken() == "vr-lava" and game.tag == "B":
            game.pos[1] -= 10.0
        if broken() == "vr-busy" and game.tag == "B":
            game.stale_until = game.frame + 90
        game.say("coop: test place %s %s %s: done" % tuple(words[1:4]))
        return True
    return False


def actors(game):
    if not on(game) or game.vr_t0 is None:
        return []
    out = [{"id": 9000 + i, "name": "Obj_FallingRock_A_06", "kind": "other", "pos": list(POOL_AT)} for i in range(30)]
    if broken() == "vr-asleep":
        return out
    target = game.pos if broken() != "vr-one-target" else (game.pos if game.tag == "A" else game.other.pos)
    since = game.frame - game.vr_t0 - FIRST
    if since < 0:
        return out
    k = since // EVERY                         # the rock woken last: falls for 30 frames, then lies
    age = since - k * EVERY
    y = target[1] + max(0.0, FALL_FROM - FALL_SPEED * age)
    out[k % 30] = {"id": 9000 + k % 30, "name": "Obj_FallingRock_A_06", "kind": "other",
                   "pos": [target[0] + 0.4, y, target[2] - 0.3]}
    return out
