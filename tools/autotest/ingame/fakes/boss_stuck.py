"""Fake-game reactions for row 303's test (ingame/test_boss_stuck.py; owner boss-stuck-303). Lines are the module's
(test_boss_stuck.py / test_intro_seen.py / test_boss_away.py cite each writer).

Only on its own fake boot BT_FAKE_BOOT=live-deku-gohma-stuck (the selftest's; the run's --boot stays live-deku-gohma),
so it never meets boss_away.py's reactions (BT_FAKE_BOOT=live-deku-gohma):
- both games in InsideTheDekuTree, Get_StoneOfForest read back GoToBossRoom 0xb5df43cb, Gohma_DefeatBoss 0;
- A walking through the door first (x > 5) owns Queen Gohma: A logs 'native boss sync on (BSA ..., ours)', B '... a
  copy'; from then she is in both world lists, on her ceiling spot until a Link under it (x > 14) targets her (ZL),
  then 60 frames on she is down, walking a 3 m circle round (15, 47, 67.6) once every 754 frames;
- a `kill` of her on A: both log the death line 30 frames on and read Gohma_DefeatBoss 1; she leaves both lists.
Selftest switch BT_FAKE_BOSS_STUCK_BUG (tests/test_ingame_boss_stuck.py): ceiling -> only her owner's (A's) target
drops her (0.12.0: a copy's player under her waits for good); still -> 600 frames after her drop she stops in both
games (a frozen AI); apart -> B's copy stays on her ceiling spot while A's walks;
nodeath -> the kill never lands.
"""
import math
import os

SCENE, GOHMA, QUEST, ALIVE, DEFEAT = ("InsideTheDekuTree", "Enemy_DungeonBoss_Gohma", "0x7d776b61", "0xb5df43cb",
                                      "Gohma_DefeatBoss")
SPOT = {"A": [-14.5, 79.6, -6.5], "B": [-20.0, 48.5, 60.0]}
SYNC = "coop: boss: Enemy_DungeonBoss_Gohma: native boss sync on (BSA 0x7f3a20, %s)"
DEATH = "Dm_Deku_BossOut: our game started the death: the others in the area play it too"
CEILING = (18.49, 59.97, 67.79)   # boss_away.py's place of her (on the ceiling)
CENTRE, RADIUS, PERIOD = (15.0, 47.0, 67.6), 3.0, 120.0
STILL_AFTER = 600


def _bug():
    return os.environ.get("BT_FAKE_BOSS_STUCK_BUG", "")


def _on():
    return os.environ.get("BT_FAKE_BOOT", "") == "live-deku-gohma-stuck"


def _a(game):
    return game if game.tag == "A" else game.other


def setup(game):
    game.bstuck = {"owned_at": None, "alive": True, "down_at": None}
    if _on():
        game.scene, game.pos = SCENE, list(SPOT[game.tag])
        game.flags.update({QUEST: ALIVE, DEFEAT: 0})


def _here(game):
    return _on() and game.scene == SCENE and not game.at_title


def move_step(game, pos):
    if not _here(game):
        return
    a = _a(game)
    if game.tag == "A" and pos[0] > 5 and a.bstuck["owned_at"] is None:
        a.bstuck["owned_at"] = a.frame
        game.say(SYNC % "ours")
        game.other.say(SYNC % "a copy")


def press(game, buttons):
    a = _a(game)
    if (_here(game) and "ZL" in buttons and a.bstuck["owned_at"] is not None and a.bstuck["down_at"] is None and
            game.pos[0] > 14 and (game.tag == "A" or _bug() != "ceiling")):
        a.bstuck["down_at"] = a.frame + 60
    return False


def act(game, words):
    if not (_here(game) and words and words[0] == "kill" and words[-1] == GOHMA):
        return False
    a = _a(game)
    if game.tag == "A" and a.bstuck["owned_at"] is not None and a.bstuck["alive"] and _bug() != "nodeath":
        a.bstuck["alive"] = False
        for g in (game, game.other):
            g.flags[DEFEAT] = 1
            g.later(30, (lambda gg: lambda: gg.say(DEATH))(g))
    return True


def _pos(frame):
    t = frame / PERIOD
    return [CENTRE[0] + RADIUS * math.cos(t), CENTRE[1], CENTRE[2] + RADIUS * math.sin(t)]


def actors(game):
    if not _here(game):
        return []
    a = _a(game)
    since = a.bstuck["owned_at"]
    if since is None or not a.bstuck["alive"]:
        return []
    down = a.bstuck["down_at"]
    if down is None or a.frame < down:
        return [{"id": 703, "name": GOHMA, "kind": "enemy", "pos": list(CEILING), "owner": 1, "alive": True,
                 "life": 21, "max_life": 21}]
    f = a.frame
    if _bug() == "still":
        f = min(f, down + STILL_AFTER)
    if _bug() == "apart" and game.tag == "B":     # (held on her ceiling spot: 13 m above the floor she walks)
        return [{"id": 703, "name": GOHMA, "kind": "enemy", "pos": list(CEILING), "owner": 1, "alive": True,
                 "life": 21, "max_life": 21}]
    return [{"id": 703, "name": GOHMA, "kind": "enemy", "pos": [round(v, 2) for v in _pos(f)], "owner": 1,
             "alive": True, "life": 21, "max_life": 21}]
