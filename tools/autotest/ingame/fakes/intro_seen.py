"""Fake-game reactions for row 244 (a)'s port (ingame/test_intro_seen.py; owner port-sweep-c131-7). Lines are the
module's (test_intro_seen.py cites each writer); places are the old scenario's (test_intro_seen.py constants).

- Boot BT_FAKE_BOOT=live-deku-gohma-seen (as the selftest's --boot): both games in InsideTheDekuTree,
  HavePlayedEvent.Dm_Deku_BossIn read back 0 on A and 1 on B (the boot's setflag lines).
- A walking under her ceiling spot (x > 15) owns Queen Gohma: A logs 'native boss sync on (BSA ..., ours)' and B
  '... a copy'; from then she is in both world lists.
- B walking through the door (x > 5: the goto's 1.5 m of 6.7) while A owns her: its intro plays (state `event` on for
  INTRO_FRAMES), B logs 'not ready to own the boss: our game plays its intro now (Dm_Deku_BossIn)' at once, 'ready to
  own the boss (Dm_Deku_BossIn)' 10 frames after its event ends and 'the owner's verbs came' 30 frames after that.
Selftest switch BT_FAKE_INTRO_SEEN_BUG (tests/test_ingame_intro_seen.py): early -> B's ready line comes 300 frames into
its intro (the bug of row 244 (a)); taken -> B also logs 'Enemy_DungeonBoss_Gohma: now ours ...' after its intro;
nointro -> B's game plays no intro (no event, no not-ready line); noowner -> A never owns her.
"""
import os

SCENE, GOHMA, SEEN = "InsideTheDekuTree", "Enemy_DungeonBoss_Gohma", "HavePlayedEvent.Dm_Deku_BossIn"
SPOT = {"A": [-14.5, 79.6, -6.5], "B": [-20.0, 48.5, 60.0]}
INTRO_FRAMES = 900
SYNC = "coop: boss: Enemy_DungeonBoss_Gohma: native boss sync on (BSA 0x7f3a10, %s)"
NOT_READY = "coop: boss: not ready to own the boss: our game plays its intro now (Dm_Deku_BossIn)"
READY = "coop: boss: ready to own the boss (Dm_Deku_BossIn)"
VERBS = "coop: boss: Enemy_DungeonBoss_Gohma: the owner's verbs came (brain 3, action 1)"
NOW_OURS = "coop: boss: Enemy_DungeonBoss_Gohma: now ours: our game runs it (sends its verbs)"


def _bug():
    return os.environ.get("BT_FAKE_INTRO_SEEN_BUG", "")


def _on():
    return os.environ.get("BT_FAKE_BOOT", "") == "live-deku-gohma-seen"


def _a(game):
    return game if game.tag == "A" else game.other


def setup(game):
    game.iseen = {"owned": False, "intro": False, "event": False}
    if _on():
        game.scene, game.pos = SCENE, list(SPOT[game.tag])
        game.flags.update({SEEN: 1 if game.tag == "B" else 0})


def _here(game):
    return _on() and game.scene == SCENE and not game.at_title


def _intro_end(game):
    game.iseen["event"] = False

    def ready():
        game.say(READY)
        game.later(30, lambda: game.say(VERBS))
        if _bug() == "taken":
            game.later(40, lambda: game.say(NOW_OURS))
    game.later(10, ready)


def _walked(game, pos):
    if not _here(game):
        return
    a = _a(game)
    if game.tag == "A" and pos[0] > 15 and not a.iseen["owned"] and _bug() != "noowner":
        a.iseen["owned"] = True
        game.say(SYNC % "ours")
        game.other.say(SYNC % "a copy")
    if game.tag == "B" and pos[0] > 5 and a.iseen["owned"] and not game.iseen["intro"]:
        game.iseen["intro"] = True
        if _bug() != "nointro":
            game.iseen["event"] = True
            game.say(NOT_READY)
            if _bug() == "early":
                game.later(300, lambda: game.say(READY))
            game.later(INTRO_FRAMES, lambda: _intro_end(game))


def move_step(game, pos):
    _walked(game, pos)


def state(game, st):
    if _here(game) and game.iseen["event"]:
        st["event"] = True


def actors(game):
    if not (_here(game) and _a(game).iseen["owned"]):
        return []
    return [{"id": 702, "name": GOHMA, "kind": "enemy", "pos": [22.5, 47.0, 68.2], "owner": 1, "alive": True,
             "life": 21, "max_life": 21}]
