"""Fake-game reactions for rows 304-306's probe (ingame/test_carry_304.py; owner carry-sync-304). Only while
BT_FAKE_CARRY_304 is set (tests/test_ingame_carry_304.py): nothing here acts in any other run (B #149).

- place X Y Z / face X Z: the module's own lines, 'coop: test place X Y Z: done' (lichii/hook/coop_testinput.cpp:1800)
  and 'coop: test face X Z: Link at x z turned to yaw Y deg' (lichii/hook/coop_boss_game.cpp:152).
- The world list has the row's object: Animal_Cucco at the cucco spot outside the probe's warps, Obj_LiftRock_A_03 at
  rock.A.txt's place in Graveyard_Child, Npc_Zora_Ruto_Child_001 at her banc place in InsideJabuJabuBelly (test RUTO_AT),
  listed only while A's Link is within LOAD_M of it (probe carry-304-182cd53b-3: not loaded at the entrance, 77 m off).
- A pressing A within 1.2 m of it lifts it: from then A's object is 1.2 m over A's Link (the hands box of the test).
Selftest switch BT_FAKE_CARRY_304: ok -> B's copy is in A's hands too (the fixed game); absent -> B has no copy (row
304: Ruto is each game's own); rest -> B's copy stays at its rest place (row 305: a carried rock not shown); far -> B's
copy follows A 4 m off (beyond MISS_M: a NO-REF miss, the box is not measured); unloaded -> Ruto is never in any world
list (her room does not load: the test's to_ruto SETUP-FAILs).
"""
import math
import os

CUCCO_SPOT = [-96.5, 97.4, 51.3]
ROCK_SCENE, ROCK_AT = "Graveyard_Child", [-34.12, 0.0, 14.94]
RUTO_SCENE, RUTO_AT = "InsideJabuJabuBelly", [1.63, 91.25, -77.98]
LOAD_M = 40.0
REACH_M, HANDS_UP = 1.2, 1.2


def mode():
    return os.environ.get("BT_FAKE_CARRY_304", "")


def _a(game):
    return game if game.tag == "A" else game.other


def setup(game):
    game.c304 = {"held": False, "scene": None}


def _thing(game):
    """(id, name, rest place) of the row's object in A's scene, or None."""
    a = _a(game)
    if a.c304["scene"] != a.scene:      # a new map (each test warps): nothing held, Ruto placed again
        a.c304.update(held=False, scene=a.scene)
    if a.scene == ROCK_SCENE:
        return 3051, "Obj_LiftRock_A_03", ROCK_AT
    if a.scene == RUTO_SCENE:
        return 3041, "Npc_Zora_Ruto_Child_001", RUTO_AT
    return 3061, "Animal_Cucco", CUCCO_SPOT


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
    if not mode() or game.tag != "A" or "A" not in buttons or game.c304["held"]:
        return False
    _, _, rest = _thing(game)
    if math.hypot(game.pos[0] - rest[0], game.pos[2] - rest[2]) <= REACH_M:
        game.c304["held"] = True
    return False


def actors(game):
    if not mode():
        return []
    a = _a(game)
    aid, name, rest = _thing(game)
    if a.scene == RUTO_SCENE and mode() == "unloaded":
        return []
    if a.scene == RUTO_SCENE and math.hypot(a.pos[0] - rest[0], a.pos[2] - rest[2]) > LOAD_M:
        return []
    hands = [a.pos[0], a.pos[1] + HANDS_UP, a.pos[2]]
    if game.tag == "A":
        return [{"id": aid, "name": name, "kind": "other", "pos": hands if a.c304["held"] else list(rest)}]
    if mode() == "absent" and name.startswith("Npc_"):
        return []
    if mode() == "far" and a.c304["held"]:
        return [{"id": aid + 100, "name": name, "kind": "other", "pos": [hands[0] + 4.0, hands[1], hands[2]]}]
    held = a.c304["held"] and mode() == "ok"
    return [{"id": aid + 100, "name": name, "kind": "other", "pos": hands if held else list(rest)}]
