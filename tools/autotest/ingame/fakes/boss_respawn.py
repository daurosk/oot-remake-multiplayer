"""Fake-game reactions of rows 289 + 234 (ingame/test_boss_respawn.py; boss-respawn-289-4).

Lines are the old row's patterns (offline boss-respawn-289-run.ps1 :344-:383), from the module:
coop_dbno_game.cpp:523 (dbno events), coop_teleport_game.cpp:433/:484/:600/:612 (respawn to the entrance, the map
change ask, starter emptied / kept), coop_objects.cpp EventLog and :6896 (event began, falling rocks),
coop_testinput.cpp:1758 (the chat line). Reacts only in a boss stage (hurt, rocks) or to the row's own switches /
BT_FAKE_BROKEN values:
  LICHII_COOP_ARRIVAL_STARTER=keep   the old behaviour: starter kept, the screen black, no arrival event
  LICHII_COOP_DEBUG_OBJECTS=1        rocks lines in VolvagiaStage
  BT_FAKE_BROKEN=starter             (selftest) the fixed lines, but the screen stays black
  BT_FAKE_BROKEN=rocks-<A|B>         (selftest) no rocks on that game
  BT_FAKE_BROKEN=beside             (selftest) A takes the beside move: B respawns next to it, not at the entrance
  BT_FAKE_BROKEN=no-arrival          (selftest) a warp to a boss stage never arrives (the bridge's 'timeout' answer)
  BT_FAKE_INTRO=<frames>             (selftest) a warp to a boss room (BOSS_ROOMS) starts an intro event of that length
"""
import os

# test_boss_respawn.STAGES + BOSS_ROOMS (the selftest checks they are the same)
BOSS_ROOMS = ("GohmaStage", "BigOctoStage", "BarinadeStage", "PhantomGanonStage", "MorphaStage", "BongoBongoStage",
              "TwinrovaStage", "GanondorfStage")
STAGES = ("VolvagiaStage", "DodongoStage", "LizalfosStage", "GanonStage") + BOSS_ROOMS


def broken():
    return os.environ.get("BT_FAKE_BROKEN", "")


def setup(game):
    game.br_black = False                      # (life: fakegame's link_life, 16/16 = the lane save, run 20261125-114351)


def command(game, m):
    if m.get("cmd") == "warp" and m.get("scene") in STAGES and broken() == "no-arrival":
        game.run(40)
        return {"ok": False, "error": "timeout", "message": "fake: Link never arrived in %s" % m["scene"]}
    return None


def after(game, m, r):
    cmd = m.get("cmd")
    if cmd == "warp" and m.get("scene") == "VolvagiaStage" and game.env_on("LICHII_COOP_DEBUG_OBJECTS=1") \
            and broken() != "rocks-" + game.tag:
        game.later(300, lambda: game.say("coop: objects: rocks: 2 new falling rocks in our game since the last line "
                                         "(2 live)"))
    elif cmd == "warp" and m.get("scene") not in STAGES:
        game.br_black = False
    elif cmd == "warp" and m.get("scene") in BOSS_ROOMS and os.environ.get("BT_FAKE_INTRO"):
        game.event = True                        # (the room's intro: the game is in an event this long)
        game.later(int(os.environ["BT_FAKE_INTRO"]), lambda: setattr(game, "event", False))
    elif cmd == "input" and m.get("stick") and not game.br_black:
        game.pos[2] += 1.5 * float(m["stick"][1])     # a stick moves Link while he has control
    elif cmd == "shot" and game.br_black and r.get("path"):
        from PIL import Image
        Image.new("RGB", (320, 180), (0, 0, 0)).save(r["path"])   # the arrival that never ends


def act(game, words):
    if words[:1] == ["hurt"] and game.scene in STAGES:
        downed(game)
        return True
    return False


def downed(game):
    s, keep = game.scene, game.env_on("LICHII_COOP_ARRIVAL_STARTER=keep")
    steps = [(30, "coop: dbno: our Link: event 0 (by P0)"), (330, "coop: dbno: our Link: event 2 (by P0)"),
             (360, "coop: dbno: our Link: event 7 (by P0)")]
    if broken() == "beside":
        steps.append((362, "coop: teleport: next to P1"))
        for at, line in steps:
            game.later(at, lambda line=line: game.say(line))
        return
    steps += [(362, "coop: teleport: respawn at the entrance %s of %s" % (s, s)),
              (364, "coop: teleport: asking the game's map change event Dm_SY_BancChangeGo for %s at %s "
                    "(arrival Dm_SY_BancChangeArrive)" % (s, s))]
    if keep:
        steps.append((365, "coop: teleport: %s has no entrance actor named %s: the starter is kept "
                           "(LICHII_COOP_ARRIVAL_STARTER=keep)" % (s, s)))
    steps.append((366, "coop: live event Dm_SY_BancChangeGo asked of the game (result 1)"))
    if not keep:
        steps.append((400, "coop: teleport: %s has no entrance actor named %s: the arrival event's starter emptied "
                           "(as the game's own entries to a boss stage reserve it)" % (s, s)))
    steps.append((420, "control: state loading -> play"))
    if not keep:
        steps.append((440, "coop: event: our Link's event began: 0x1 kind 1 (1 in the session); names seen: x"))
    for at, line in steps:
        game.later(at, lambda line=line: game.say(line))
    game.later(380, lambda: setattr(game, "br_black", keep or broken() == "starter"))
