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
  BT_FAKE_BROKEN=wipe                (selftest) A is downed in the fight too and B never respawns (a party wipe)
  BT_FAKE_INTRO=<frames>             (selftest) a warp to a boss room (BOSS_ROOMS) starts an intro event of that length
  BT_FAKE_BROKEN=a-free              (selftest) A's warp into the stage again starts no event (A not busy)
  BT_FAKE_BROKEN=no-fight            (selftest) FireTemple room 33: 'in the boss room' but the fight never begins
Every other stage warp starts the arrival event (ARRIVAL frames), as the game's map change event does. B's beside ask
(ASK frames after the hurt) is refused when A is in an event ('is busy') or, in FireTemple, when B's room is in its fight
('not during your boss fight'); else B goes beside A (the real game with A idle: br289-6-keep-182cd53b-2 NO-REF).
After the respawn's load B stands at the stage's start (coop_start_points.inc, read, never copied).
"""
import os
import re

FIRE = "FireTemple"                          # test_boss_respawn.FIGHT_ROOM (kRooms row 4)
ARRIVAL, ASK = 900, 400          # (arrival: run 20261125-114351 B 48 s; shorter here, inside IN_PLAY_FRAMES)
START_POINTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "lichii", "hook",
                            "coop_start_points.inc")

# test_boss_respawn.STAGES + BOSS_ROOMS (the selftest checks they are the same)
BOSS_ROOMS = ("GohmaStage", "BigOctoStage", "BarinadeStage", "PhantomGanonStage", "MorphaStage", "BongoBongoStage",
              "TwinrovaStage", "GanondorfStage")
STAGES = ("VolvagiaStage", "DodongoStage", "LizalfosStage", "GanonStage") + BOSS_ROOMS


def broken():
    return os.environ.get("BT_FAKE_BROKEN", "")


def start(scene):
    with open(START_POINTS, encoding="utf-8") as f:
        for sc, e, x, y, z in re.findall(r'\{"(\w+)", "(\w+)", (-?[\d.]+)f, (-?[\d.]+)f, (-?[\d.]+)f', f.read()):
            if sc == scene and e == scene:
                return [float(x), float(y), float(z)]
    return None


def in_room(pos):                            # coop_boss.cpp kRooms row 4: centre (-82, 0) r 38, height 88-125
    return (pos[0] + 82.0) ** 2 + pos[2] ** 2 <= 38.0 ** 2 and 88.0 <= pos[1] <= 125.0


def setup(game):
    game.br_fight = False
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
        game.later(ARRIVAL + 300, lambda: game.say("coop: objects: rocks: 2 new falling rocks in our game since the last line "
                                         "(2 live)"))
    elif cmd == "warp" and m.get("scene") not in STAGES:
        game.br_black = False
    elif cmd == "warp" and m.get("scene") in STAGES and broken() == "wipe" and game.tag == "A":
        game.later(1500, lambda: game.say("coop: dbno: our Link: event 0 (by the boss)"))
    elif cmd == "warp" and m.get("scene") in BOSS_ROOMS and os.environ.get("BT_FAKE_INTRO"):
        game.event = True                        # (the room's intro: the game is in an event this long)
        game.later(int(os.environ["BT_FAKE_INTRO"]), lambda: setattr(game, "event", False))
    a_free = broken() == "a-free" and game.tag == "A" and getattr(game, "br_down_seen", False)
    if cmd == "warp" and m.get("scene") in STAGES and not game.event and not a_free:
        game.event = True                        # (the map change event's arrival)
        game.later(ARRIVAL, lambda: setattr(game, "event", False))
    if cmd == "warp":
        game.br_fight = False
    elif cmd == "input" and m.get("stick") and not game.br_black:
        game.pos[2] += 1.5 * float(m["stick"][1])     # a stick moves Link while he has control
    elif cmd == "shot" and game.br_black and r.get("path"):
        from PIL import Image
        Image.new("RGB", (320, 180), (0, 0, 0)).save(r["path"])   # the arrival that never ends


def act(game, words):
    if words[:1] == ["hurt"] and (game.scene in STAGES or game.scene == FIRE):
        game.other.br_down_seen = True
        downed(game)
        return True
    if words[:1] == ["place"] and game.scene == FIRE and in_room([float(v) for v in words[1:4]]):
        game.later(30, lambda: game.say("coop: boss: in the boss room of FireTemple: Enemy_DungeonBoss_Volvagia"))
        if broken() != "no-fight":
            game.later(630, lambda: (setattr(game, "br_fight", True),
                                     game.say("coop: boss: no intro: the fight has begun here (the others are not "
                                              "moved in)")))
    return False                              # (fakegame places Link)


def downed(game):
    s, keep = game.scene, game.env_on("LICHII_COOP_ARRIVAL_STARTER=keep")
    steps = [(30, "coop: dbno: our Link: event 0 (by P0)"), (330, "coop: dbno: our Link: event 2 (by P0)")]
    if broken() == "wipe":                     # (no one left up: B is downed and bleeds out, never respawns)
        for at, line in steps:
            game.later(at, lambda line=line: game.say(line))
        return
    for at, line in steps:
        game.later(at, lambda line=line: game.say(line))
    game.later(ASK, lambda: ask(game, s, keep))


def ask(game, s, keep):
    """B's beside ask (coop_dbno_game.cpp:551) and what the module does with it."""
    game.say("coop: dbno: respawn: moving beside P1")
    if game.br_fight and broken() != "beside":
        steps = [(1, "coop: teleport: not during your boss fight")]
    elif game.other.state().get("event") and broken() != "beside":   # (A's event as its state says: AnswerFor's local.event)
        steps = [(1, "coop: teleport: Alpha is busy (loading, a cutscene, a talk or in the air); try again soon")]
    else:
        for at, line in ((2, "coop: teleport: next to P1"), (30, "coop: dbno: our Link: event 7 (by P1)")):
            game.later(at, lambda line=line: game.say(line))
        return
    steps += [(2, "coop: dbno: respawn: not beside them: to the entrance"),
              (5, "coop: teleport: respawn at the entrance %s of %s" % (s, s)),
              (7, "coop: teleport: asking the game's map change event Dm_SY_BancChangeGo for %s at %s "
                  "(arrival Dm_SY_BancChangeArrive)" % (s, s))]
    if keep:
        steps.append((8, "coop: teleport: %s has no entrance actor named %s: the starter is kept "
                         "(LICHII_COOP_ARRIVAL_STARTER=keep)" % (s, s)))
    steps.append((9, "coop: live event Dm_SY_BancChangeGo asked of the game (result 1)"))
    if not keep and s != FIRE:
        steps.append((40, "coop: teleport: %s has no entrance actor named %s: the arrival event's starter emptied "
                          "(as the game's own entries to a boss stage reserve it)" % (s, s)))
    steps.append((60, "control: state loading -> play"))
    steps.append((62, "coop: dbno: our Link: event 7 (by P1)"))
    if not keep:
        steps.append((80, "coop: event: our Link's event began: 0x1 kind 1 (1 in the session); names seen: x"))
    for at, line in steps:
        game.later(at, lambda line=line: game.say(line))
    at = start(s)
    if at:
        game.later(60, lambda: setattr(game, "pos", list(at)))
    game.later(50, lambda: setattr(game, "br_black", keep or broken() == "starter"))
