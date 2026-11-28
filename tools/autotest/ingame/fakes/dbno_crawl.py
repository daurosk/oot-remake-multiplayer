"""Fake-game reactions for test_dbno_crawl.py (row 44 / #337, crawl-337-9's file). They prove the test's logic, not
the game: each line said is the module's own text (file:line in test_dbno_crawl.py's patterns). Reacts only to this
row's inputs: 'chat /dbno ...', the bottle setenums, 'hurt', A held beside a downed B, Character Select's Int,
Sequence_IsChildhood (the age), and a stick while downed.

Only where this row is (fakegame.py: "A hook reacts only where its row is"): life, hurt, the bottles and stick
movement only once A said 'chat /dbno ...' this boot (every test's rules() does, before any of them). Before, state()
wrote st['life'] on EVERY boot and a stick moved Link at this row's walk rate in every row (row-286-wake-4 19:02 scan).

BT_FAKE_DBNO_BUG (selftests, tests/test_ingame_dbno_crawl.py):
  stand     the 0.11.13 FAIL: the downed child is given back to the game's Wait ('Crawl taken but not crawling'),
            never lies, walks at full speed
  norevive  A's held A never revives B
  stuck     no control after the revive (B does not move)
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import hookshot   # noqa: E402  (BOTTLE_ITEMS: the one bottle list, as the test uses it)
import playtest_client as pc   # noqa: E402  (REVIVE_BUTTON, as the test presses it)

BOTTLES = hookshot.BOTTLE_ITEMS
CHARSEL = "CharacterMenu_SelectedCharacter"
NO_CS = "COOP_TEST_NO_CHARACTER_SELECT=1"   # (ingame/no_cs.py SWITCH: lane game B booted without the mod)
FPS = 30                 # (pair.FPS: the fake's frames -> seconds)
CRAWL_M, WALK_M = 0.01, 0.08   # metres a frame (the fake's numbers: a crawl well under a walk)


def bug(name):
    return os.environ.get("BT_FAKE_DBNO_BUG") == name


def num(game):
    return 1 if game.tag == "A" else 2


def setup(game):
    game.dbno = {"state": "up", "life": 12, "fairy": game.tag == "B", "held": 0, "on": False}
    if not game.env_on("COOP_TEST_NO_CHARACTER_SELECT=1"):   # (no_cs.SWITCH: that game boots without the mod, the
        game.flags.setdefault(CHARSEL, 0)                     # flag unset = kind unknown/None: the fake module's ModPresent)
    game.dbno_rules = {"on": True, "bleed": 45, "hold": 50}


def adult(game):
    return game.age() == "adult"   # (fakegame.age: the one source of a fake game's age)


def here(game):
    """This row is running on this lane: A said 'chat /dbno ...' (act) since the boot."""
    return game.dbno["on"]


def say_rules(game):
    r = game.dbno_rules
    for g in (game, game.other):
        g.dbno_rules = dict(r)
        g.say("coop: dbno: %s (bleed %d s, hold %d tenths, 12 quarter hearts; set by 1)" % (
            "on" if r["on"] else "off", r["bleed"], r["hold"]))


def up(game, ev, by):
    d = game.dbno
    if d["state"] == "up":
        return
    d.update(state="up", life=12, held=0)
    game.say("coop: dbno: our Link: event %d (by P%d)" % (ev, by))
    if ev == 7:
        game.say("coop: dbno: respawn: moving beside P%d" % by)
        game.pos = [game.other.pos[0] + 1.0, game.other.pos[1], game.other.pos[2]]
    if adult(game):
        game.say("coop: dbno: our Link up: the game's AS 'DamageLStand' played on from the held pose (the game's "
                 "stand-up from lying)")
        game.say("coop: dbno: our Link up: LDamageStandUp (85) asked and taken")
    else:
        game.say("coop: dbno: our Link up: the game's own request stands it up (no LDamageStandUp (85) of ours needed)")
    game.other.later(3, lambda: game.other.say("coop: dbno: P%d is up" % num(game)))


def go_down(game):
    d, o = game.dbno, game.other
    d.update(state="down", life=1, held=0)
    game.say("coop: dbno: our Link: event 0 (by P0)")
    o.later(5, lambda: o.say("coop: dbno: P%d is down" % num(game)))

    def lie():
        game.say("coop: dbno: our downed Link: the game offers Crawl (its do action is 14): Crawl asked")
        game.say("coop: dbno: our downed Link: Crawl (234) asked and taken")
        if adult(game):
            game.say("coop: dbno: our downed adult Link: the game's AS 'DamageLStand' held at frame 0 (lying; the "
                     "crawl's own clips are the child's)")
        elif bug("stand"):
            game.say("coop: dbno: our downed Link: Crawl taken but not crawling ('CrouchLockOnMove')")
        else:
            game.say("coop: dbno: our downed Link: the game's own crawl runs ('CrouchLockOnMove')")
    game.later(20, lie)
    start = game.frame

    def bleed():
        if d["state"] == "down" and game.frame - start >= game.dbno_rules["bleed"] * FPS:
            d["state"] = "spectating"
            game.say("coop: dbno: our Link: event 2 (by P%d)" % num(o))
            o.later(3, lambda: o.say("coop: dbno: P%d is spectating" % num(game)))
            game.later(4 * FPS, lambda: up(game, 7, num(o)) if d["state"] == "spectating" else None)
    game.later(game.dbno_rules["bleed"] * FPS, bleed)


def act(game, words):
    if words[:2] == ["chat", "/dbno"] and game.tag == "A":
        for g in (game, game.other):
            g.dbno["on"] = True
        arg = words[2:]
        if arg == ["off"]:
            game.dbno_rules["on"] = False
            say_rules(game)
            for g in (game, game.other):
                up(g, 5, 0)
        elif arg == ["on"]:
            game.dbno_rules["on"] = True
            say_rules(game)
        elif len(arg) == 2 and arg[0] == "time":
            game.dbno_rules["bleed"] = int(arg[1])
            say_rules(game)
        return True
    if not here(game):
        return False
    if len(words) == 3 and words[0] == "setenum" and words[1] in BOTTLES and words[2] == "Empty":
        if game.dbno["fairy"]:
            game.dbno["fairy"] = False
            game.later(2, lambda: game.say("coop: dbno: no fairy in our bottles: a lethal hit downs us"))
        return True
    if len(words) == 2 and words[0] == "hurt":
        d = game.dbno
        if d["state"] == "up" and game.dbno_rules["on"]:
            if d["life"] - int(words[1]) > 0:
                d["life"] -= int(words[1])
            elif d["fairy"]:
                d["life"] = 12
            else:
                go_down(game)
        return True
    return False


def after(game, m, r):
    # The character flag goes through the built-in flag store (game.flags) and this hook only adds to its answer, so
    # another row's fake of the same flag (fakes/levitate.py nochar, later in file-name order) still takes effect;
    # a `command` hook here answered it for every test and hid theirs.
    if m.get("cmd") == "flag_read" and m.get("name") == CHARSEL:
        if r.get("value") is not None:     # (unset = no pick Int in the fake; no_cs reads the boot mod lines)
            r["kind"] = "int"
        return
    if m.get("cmd") == "flag" and m.get("name") == CHARSEL:
        o, n = game.other, int(m["value"])
        o.later(30, lambda: o.say("coop: look: P%d wears 11 part colour(s), character %d (PLAYER_LOOK)" % (num(game), n)))
        return
    if m.get("cmd") != "input" or not here(game):
        return
    frames = int(m.get("frames", 1))
    stick = m.get("stick")
    d = game.dbno
    if stick and d["state"] in ("up", "down"):
        downed = d["state"] == "down" and not bug("stand")
        rate = 0.0 if (bug("stuck") and d.get("revived")) else (CRAWL_M if downed else WALK_M)
        yaw = math.radians(game.facing + float(stick[0]))
        game.pos = [game.pos[0] + rate * frames * math.sin(yaw), game.pos[1],
                    game.pos[2] + rate * frames * math.cos(yaw)]
    o = game.other
    if pc.REVIVE_BUTTON in (m.get("buttons") or []) and o.dbno["state"] == "down" and not bug("norevive") and \
            math.dist(game.pos[::2], o.pos[::2]) < 2.5 * 1.3:
        o.dbno["held"] += frames
        if o.dbno["held"] >= o.dbno_rules["hold"] * FPS // 10:
            o.dbno["revived"] = True
            up(o, 1, num(game))
    r["state"] = game.state()


def state(game, st):
    if st.get("pos") is None or not here(game):
        return
    st["life"] = game.dbno["life"]
