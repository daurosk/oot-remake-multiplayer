"""Fake-game reactions for test_alone_318.py (rows 318 + 334, dbno-alone-318's file). They prove the test's logic, not
the game: each line said is the module's own text (cited in test_alone_318.py's patterns).

Only where this row is: A said 'chat /dbno time 12' (test_alone_318.py BLEED_S, which no other row sets) since the
boot; any other '/dbno time' (the restore's 45) ends it. Then this module, before fakes/dbno_crawl.py (file-name
order), takes the LETHAL hit only (the others, and a bottled fairy, stay dbno_crawl's) and plays rows 318 + 334 on
the same game.dbno dict dbno_crawl keeps (its 'state' hook writes the life; its '/dbno off' stands everyone up).
'Nearby' here = the other game in the same scene (the real rule: coop_dbno_game.cpp ShownHere + kNearbyGraceUs).

BT_FAKE_ALONE_BUG (selftests, tests/test_ingame_alone_318.py):
  down    before the fix: a lethal hit always downs (row 334 not done); the bleed-out then still goes as below
  watch   before the fix: bled out, B watches A wherever A is and comes back beside A (row 318 not done)
"""
import os

ROW_BLEED_S = 12   # test_alone_318.py BLEED_S
FPS = 30           # (pair.FPS)


def bug(name):
    return os.environ.get("BT_FAKE_ALONE_BUG") == name


def num(game):
    return 1 if game.tag == "A" else 2


def setup(game):
    game.alone318 = False


def nearby(game):
    return game.other.scene == game.scene and game.other.dbno["state"] == "up"


def stand_up(game, by):
    d, o = game.dbno, game.other
    d.update(state="up", life=12, held=0)
    game.say("coop: dbno: our Link: event 7 (by P%d)" % by)
    if by:
        game.say("coop: dbno: respawn: moving beside P%d" % by)
        game.scene = o.scene
        game.pos = [o.pos[0] + 1.0, o.pos[1], o.pos[2]]
    game.say("coop: dbno: our Link up: the game's own request stands it up (no LDamageStandUp (85) of ours needed)")
    o.later(3, lambda: o.say("coop: dbno: P%d is up" % num(game)))


def alone(game):
    """Rows 318 + 334: event 8, the entrance of our own scene, then up (event 7 by P0)."""
    d, o = game.dbno, game.other
    if d["state"] == "up":
        o.later(3, lambda: o.say("coop: dbno: P%d is spectating" % num(game)))
    d.update(state="spectating", life=1)
    game.say("coop: dbno: our Link: event 8 (by P0)")
    game.later(2, lambda: game.say("coop: teleport: respawn at the entrance Start_00 of %s" % game.scene))
    game.later(6, lambda: stand_up(game, 0) if d["state"] == "spectating" else None)


def watch(game, since):
    d = game.dbno
    if d["state"] != "spectating" or not game.alone318:
        return
    if not bug("watch") and not nearby(game):
        alone(game)
    elif game.frame - since >= 4 * FPS:
        stand_up(game, num(game.other))
    else:
        game.later(10, lambda: watch(game, since))


def go_down(game):
    d, o = game.dbno, game.other
    d.update(state="down", life=1, held=0)
    game.say("coop: dbno: our Link: event 0 (by P0)")
    o.later(5, lambda: o.say("coop: dbno: P%d is down" % num(game)))

    def bleed():
        if d["state"] != "down" or not game.alone318:
            return
        if nearby(game) or bug("watch"):
            d["state"] = "spectating"
            game.say("coop: dbno: our Link: event 2 (by P%d)" % num(o))
            o.later(3, lambda: o.say("coop: dbno: P%d is spectating" % num(game)))
            watch(game, game.frame)
        else:
            alone(game)
    game.later(game.dbno_rules["bleed"] * FPS, bleed)


def act(game, words):
    if words[:3] == ["chat", "/dbno", "time"] and game.tag == "A":
        mark = words[3:] == [str(ROW_BLEED_S)]
        for g in (game, game.other):
            g.alone318 = mark
        return False          # (the rules themselves: dbno_crawl's act)
    if not getattr(game, "alone318", False) or len(words) != 2 or words[0] != "hurt":
        return False
    d = getattr(game, "dbno", None)
    if d is None or d["state"] != "up" or not game.dbno_rules["on"]:
        return False
    if d["life"] - int(words[1]) > 0 or d["fairy"]:
        return False          # not lethal, or a bottled fairy: dbno_crawl's own
    if nearby(game) or bug("down"):
        go_down(game)
    else:
        alone(game)
    return True
