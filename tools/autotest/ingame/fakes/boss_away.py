"""Fake-game reactions for row 136's port (ingame/test_boss_away.py; owner port-sweep-c131-5). Lines are the module's
(test_boss_away.py cites each writer); places are the old scenario's (test_boss_away.py constants).

- Boot BT_FAKE_BOOT=live-deku-gohma (as the selftest's --boot): both games in InsideTheDekuTree, Get_StoneOfForest
  (0x7d776b61) read back as GoToBossRoom 0xb5df43cb, Gohma_DefeatBoss 0 (the boot's setenum + setflag lines).
- Queen Gohma (Enemy_DungeonBoss_Gohma) is in B's world list once B stood under her ceiling spot (x > 12); a `kill` of her
  on B while she lives kills her in both games: B logs 'Dm_Deku_BossOut: our game started the death: ...'
  (coop_boss_scenes.cpp:458) 30 frames on, A logs 'coop: boss: Queen Gohma was beaten by the group, not in this game:
  Gohma_DefeatBoss = 1 (written)' (coop_hook.cpp:1712) 300 frames on and reads Gohma_DefeatBoss 1 from then.
Selftest switch BT_FAKE_BOSS_AWAY_BUG (tests/test_ingame_boss_away.py): late -> A's set waits for a load (its line 2000
frames on, A out of room E by then); alive -> A's world list keeps a live Gohma after her death; nobody -> no Gohma body
(the boot's set did not take); nodeath -> the kill never lands.
"""
import os

SCENE, GOHMA, QUEST, ALIVE, DEFEAT = ("InsideTheDekuTree", "Enemy_DungeonBoss_Gohma", "0x7d776b61", "0xb5df43cb",
                                      "Gohma_DefeatBoss")
SPOT = [-14.5, 79.6, -6.5], [-12.0, 48.5, 66.5]
DEATH = "Dm_Deku_BossOut: our game started the death: the others in the area play it too"
GIVEN = "coop: boss: Queen Gohma was beaten by the group, not in this game: Gohma_DefeatBoss = 1 (written)"


def _bug():
    return os.environ.get("BT_FAKE_BOSS_AWAY_BUG", "")


def _on():
    return os.environ.get("BT_FAKE_BOOT", "") == "live-deku-gohma"


def _a(game):
    return game if game.tag == "A" else game.other


def setup(game):
    game.ba = {"alive": True, "seen": False}
    if _on():
        game.scene, game.pos = SCENE, list(SPOT[0] if game.tag == "A" else SPOT[1])
        game.flags.update({QUEST: ALIVE, DEFEAT: 0})


def _here(game):
    return _on() and game.scene == SCENE and not game.at_title


def act(game, words):
    if not (_here(game) and words[0] == "kill" and words[-1] == GOHMA and game.tag == "B"):
        return False
    a = _a(game)
    if a.ba["alive"] and a.ba["seen"] and _bug() != "nodeath":
        a.ba["alive"] = False
        game.flags[DEFEAT] = 1
        game.later(30, lambda: game.say(DEATH))

        def given():
            a.flags[DEFEAT] = 1
            a.say(GIVEN)
        a.later(2000 if _bug() == "late" else 300, given)
    return True


def actors(game):
    if not _here(game):
        return []
    a = _a(game)
    if game.tag == "B" and game.pos[0] > 12 and _bug() != "nobody":
        a.ba["seen"] = True
    live = a.ba["alive"] or (game.tag == "A" and _bug() == "alive")
    if not a.ba["seen"] or not live:
        return []
    return [{"id": 701, "name": GOHMA, "kind": "enemy", "pos": [18.49, 59.97, 67.79], "owner": 2, "alive": True,
             "life": 21, "max_life": 21}]
