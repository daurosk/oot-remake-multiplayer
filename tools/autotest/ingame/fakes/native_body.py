"""Fake-game reactions for native puppets N1 (ingame/test_native_body.py; native-puppets-f). Lines are the module's
(test_native_body.py cites each writer: lichii/hook/coop_puppet.cpp).

Scoped (B #149): acts ONLY while the boot has the test pack `native` in (the game folder's coop-test-pack.json,
testpack.MANIFEST, lists it: put in by --pack native before the boot, taken out after it), so no other row's test sees it.
- A logs `P2 body: CoopPuppet_Child_Green has a body; ...` at its first state.
- A's walk stops where A's and B's child capsules touch (0.46 m, flat), as a body does; A placed over B lands on the
  copy's top (B's feet + 1.17 m).
- B's first walk: A logs one body summary 300 frames later (moves, setter calls, drift 0.000 m).
Selftest switch BT_FAKE_NATIVE_BUG (tests/test_ingame_native_body.py): nobody -> no collision and no top (A walks
through: FAIL); noline -> no `has a body` line (SETUP-FAIL); left -> the body stays at B's old spot after B walks
(FAIL at step 4); drift -> the summary says 0.250 m (FAIL); lowtop -> A comes to rest 0.70 m over B's feet (a
threshold miss: NO-REF listing it while MEASURED is None).
"""
import json
import math
import os

MANIFEST = "coop-test-pack.json"      # testpack.MANIFEST
RADII_M = 0.46                        # two child capsules (test_native_body.py CHILD_RADIUS_M * 2)
TOP_M = 1.17                          # test_native_body.py CHILD_HEIGHT_M
HAS_BODY = "coop: puppet: P2 body: CoopPuppet_Child_Green has a body; it is moved through the game's setter"
SUMMARY = ("coop: puppet: P2 body: 240 frames at rest, 60 moved (60 setter calls made, 0 waited a frame for one), "
           "0 gave way to our Link; the game had it up to %.3f m from our place")


def _bug():
    return os.environ.get("BT_FAKE_NATIVE_BUG", "")


def on(game):
    try:
        with open(os.path.join(game.folder, MANIFEST), encoding="utf-8") as f:
            return "native" in json.load(f).get("packs", [])
    except (OSError, ValueError):
        return False


def _nb(game):
    if not hasattr(game, "nb"):
        game.nb = {"said": False, "walked": False, "old": None}
    return game.nb


def _bodies(game):
    """The places a copy's body stands in A's game: B now, and B's old spot with the `left` bug."""
    b = game.other
    out = [b.pos]
    if _bug() == "left" and _nb(b)["old"] is not None:
        out.append(_nb(b)["old"])
    return out


def state(game, st):
    if game.tag == "A" and on(game) and not _nb(game)["said"]:
        _nb(game)["said"] = True
        if _bug() != "noline":
            game.say(HAS_BODY)


def move_step(game, pos):
    if not on(game):
        return None
    if game.tag == "B":
        nb = _nb(game)
        if not nb["walked"]:
            nb["walked"], nb["old"] = True, list(game.pos)
            drift = 0.25 if _bug() == "drift" else 0.0
            game.other.later(300, lambda: game.other.say(SUMMARY % drift))
        return None
    if _bug() == "nobody":
        return None
    for b in _bodies(game):
        if math.hypot(pos[0] - b[0], pos[2] - b[2]) < RADII_M:
            return list(game.pos)        # (stopped against the copy)
    return None


def act(game, words):
    if game.tag != "A" or words[:1] != ["place"] or not on(game) or _bug() == "nobody":
        return False
    p = [float(v) for v in words[1:4]]
    b = game.other.pos
    if math.hypot(p[0] - b[0], p[2] - b[2]) < RADII_M and p[1] > b[1] + TOP_M:
        game.pos = [p[0], b[1] + (0.70 if _bug() == "lowtop" else TOP_M), p[2]]    # (fell onto the copy's top)
        return True
    return False
