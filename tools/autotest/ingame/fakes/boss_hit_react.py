"""Fake-game reactions of row 138 (2)(7) (ingame/test_boss_hit_react.py; boss-hit-react-138).

Only in DodongoStage. Lines copied from the module (lichii/hook/coop_boss_native.cpp, logged 'coop: bosses: ' by
coop_objects.cpp; the counts line from coop_hookshot.cpp LogBossNative). On the warp in: 'native boss sync on' (A ours,
B a copy) after 600 frames; the world list has King Dodongo with one life shared by both games (the owner's). A's
'A' press stuns him for 900 frames (the bomb in his mouth); a 'ZL+Y' press of B's while he is stunned takes 2 life
and logs the took-life line (once) and, 120 frames later, a counts line with K; outside the stun it takes no life and
logs the own reaction (allowed: a hit that took no life; slash 5 lands after the stun's 900 frames).
BT_FAKE_BROKEN values (selftests):
  bhr-ownreact   the 0.11.4 bug: B's slash that took life logs the copy's own reaction (no took-life line, K stays 0)
  bhr-nolife     A's bomb never stuns him: no slash takes life (run ef002962's K = 0)
  bhr-nosync     B never logs its copy's sync line (the row's roles not there)
  bhr-nocount    the took-life line comes, the counts lines stay at K = 0
"""
import os

STAGE = "DodongoStage"
BOSS = "Enemy_DungeonBoss_Dodongo"
STUN_FRAMES, SYNC_AFTER, COUNT_AFTER = 900, 600, 120


def broken():
    return os.environ.get("BT_FAKE_BROKEN", "")


def setup(game):
    game.bhr = None                            # this game's boss in DodongoStage (B's life is A's: one dict)


def lane(fakelane):
    shared = {"life": 24, "stun_until": -1, "k": 0}
    for g in fakelane.g.values():
        g.bhr_shared = shared


def counts(k):
    return ("coop: bosses: ours sent 0 brain + 0 action verbs (0 by plug); copies: 0 own skipped, 0 brain + 0 action "
            "verbs put in (0 waited for a node; 0 answered a busy slot, 0 of them asked again next frame), 0 own run in "
            "our event or after our hit (0 late, 0 hits by our shots, 0 own stuns; %d hits that took life left to the "
            "owner), 0 own minions not made" % k)


def after(game, m, r):
    if m.get("cmd") != "warp":
        return
    if m.get("scene") != STAGE:
        game.bhr = None
        return
    game.bhr = True
    role = "ours" if game.tag == "A" else "a copy"
    if not (broken() == "bhr-nosync" and game.tag == "B"):
        game.later(SYNC_AFTER, lambda: game.say("coop: bosses: %s: native boss sync on (BSA 0x1a2b, %s)" % (BOSS, role)))


def press(game, buttons):
    if not game.bhr or game.scene != STAGE:
        return False
    sh = game.bhr_shared
    if game.tag == "A" and "A" in buttons and broken() != "bhr-nolife":
        sh["stun_until"] = game.frame + STUN_FRAMES
    elif game.tag == "B" and "ZL" in buttons and "Y" in buttons:
        if game.other.frame <= sh["stun_until"] or game.frame <= sh["stun_until"]:
            sh["life"] -= 2
            if broken() == "bhr-ownreact":
                game.say("coop: bosses: %s: our own hit: the copy runs its own reaction" % BOSS)
                return False
            sh["k"] += 1
            if sh["k"] == 1:
                game.say("coop: bosses: %s: our own hit took life: its owner's game reacts (the copy keeps the "
                         "owner's verbs)" % BOSS)
            k = 0 if broken() == "bhr-nocount" else sh["k"]
            game.later(COUNT_AFTER, lambda: game.say(counts(k)))
        else:
            game.say("coop: bosses: %s: our own hit: the copy runs its own reaction" % BOSS)
    return False


def actors(game):
    if not game.bhr or game.scene != STAGE:
        return []
    return [{"id": 7100, "name": BOSS, "kind": "enemy", "pos": [0.0, 0.0, 10.0], "life": game.bhr_shared["life"],
             "max_life": 24}]
