"""Fake Phantom Ganon for test_bosses.py (row 287, pganon-cct-287-3's file; hooks: ../fakegame.py docstring).

One boss shared by both fakes (lane hook), only in PhantomGanonStage. Its lines are the module's own texts, as
test_bosses.py reads them: 'coop: test verb ... asked of our boss' (coop_objects.cpp QueueTestVerb), the probe's
'HANDED OVER (was ...)' (coop_move_probe.cpp), the fix's 'CCT move state N asked of the game: in (...)'
(coop_brain_state.cpp), the fall guard's 'fell N m below its fight start' (coop_boss_native.cpp Table::Fall).
BT_FAKE_PG_FALL=1 (selftest): the bug, he drops 60 m at every hand-over and the fall line is logged.
The copy's game logs the probe's move state lines (coop_move_probe.cpp): the owner's knock-down leaves the copy in
state 1 (the cause, phantom-ganon-287-3). BT_FAKE_PG_SAME=1 (selftest): the copy stays in the owner's state 2.
"""
import os

PG = "Enemy_DungeonBoss_PhantomGanon"
SCENE = "PhantomGanonStage"


def lane(fakelane):
    boss = {"owner": "A", "life": 42, "alive": True, "y": 10.0}
    for g in fakelane.g.values():
        g.pg_boss = boss


def actors(game):
    b = getattr(game, "pg_boss", None)
    if b is None or game.scene != SCENE:
        return []
    return [{"id": 7, "name": PG, "kind": "enemy", "pos": [0.0, b["y"], 0.0], "life": b["life"], "max_life": 42,
             "owner": b["owner"], "alive": b["alive"]}]


def hand(game, to):
    """Phantom Ganon to game `to`: the probe's HANDED OVER line on both games, the fix's CCT line on the new owner."""
    b = game.pg_boss
    was = b["owner"]
    if was == to:
        return
    b["owner"] = to
    for g in (game, game.other):
        ours = g.tag == to
        whose = "ours" if ours else "a copy of player %d" % (1 if to == "A" else 2)
        before = "a copy of player %d" % (1 if was == "A" else 2) if ours else "ours"
        g.say("coop: move probe: %s 0x80000007 %s: HANDED OVER (was %s) move state %s, verbs 139"
              % (PG, whose, before, copy_state(game) if ours else "2 (type 1, before 1)"))
        if ours:
            g.say("coop: bosses: %s: CCT move state 2 asked of the game: in (result 0x80000002)" % PG)
    if os.environ.get("BT_FAKE_PG_FALL"):
        b["y"] -= 60.0
        (game if game.tag == to else game.other).say(
            "coop: bosses: %s: fell 60 m below its fight start (row 178): put back at its fight start, its AI "
            "chooses again" % PG)


def copy_state(game):
    """The copy's move state text as the probe writes it: 1 after the owner's knock-down (the cause), 2 with SAME."""
    return "2 (type 1, before 1)" if os.environ.get("BT_FAKE_PG_SAME") else "1 (type 2, before 2)"


def act(game, words):
    b = getattr(game, "pg_boss", None)
    if b is None or game.scene != SCENE or not words:
        return False
    w, o = words, game.other
    if w[0] == "hit" and w[1:2] == [PG]:
        b["life"] = max(0, b["life"] - int(w[2]))
    elif w[0] == "kill" and w[1:2] == [PG]:
        game.later(30, lambda: b.update(alive=False, life=0))
    elif w[0] == "verb" and w[1:2] == [PG]:
        mine = b["owner"] == game.tag
        game.say("coop: test verb %s %s: %s" % (PG, w[2], "asked of our boss" if mine else
                                                 "no boss of ours by that name"))
        if mine:
            o.say("coop: move probe: %s 0x80000007 a copy of player %d: move state 2 (type 1, before 1) -> %s, "
                  "verbs 139" % (PG, 1 if game.tag == "A" else 2, copy_state(o)))   # the copy's knock-down start
            game.later(900, lambda: hand(game, o.tag))       # (his turn goes to the other player)
    elif w[0] == "hurt":
        if b["owner"] == game.tag:
            game.later(60, lambda: hand(game, o.tag))        # the downed owner's boss goes to the other
    else:
        return False
    return True
