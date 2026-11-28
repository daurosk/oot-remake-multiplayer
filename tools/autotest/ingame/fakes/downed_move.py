"""Fake-game reactions for test_downed_move.py (0.12.1 downed-move, its owner's file). They prove the test's logic, not
the game: each line said is the module's own text (file:line in test_downed_move.py's patterns). The down, the hits,
the revive and the stick movement are fakes/dbno_crawl.py's (this file runs after it, file-name order); this one adds
the patch 0046 hello line, the room answer / lying ground-move lines and a camera `view` once that row is running.

BT_FAKE_DOWNED_BUG (selftests, tests/test_ingame_downed_move.py):
  still   the v0.12.0 bug: the downed Link (child or adult) does not move under the stick, and neither the room
          answer nor the lying ground-move line is logged
  flicker the lying Link stands up in every other still-check shot (the Wait vs pose flicker), on both games
  farcam  the view stays the standing camera while downed (no crawl camera)
  nowrap  B's hookshot.exe has no patch 0046 / 0049 (its hellos say 'not wrapped'): a SETUP-FAIL, never a FAIL
  noskip  the held adult's slot-0 plays are not skipped (no patch 0049 skip line: the 89133f1ee flicker's cause)
"""
import math
import os

STAND_BACK, STAND_UP = 4.0, 2.0   # the fake's standing camera: metres behind his feet, metres up
CRAWL_BACK, CRAWL_UP = 1.0, 0.6   # the fake's crawl camera


def bug(name):
    return os.environ.get("BT_FAKE_DOWNED_BUG") == name


def setup(game):
    game.downed_move = {"seen": False, "pos": None, "shots": 0}
    if game.tag == "B" and bug("nowrap"):
        game.say("coop: down but not out: the crawl's room check not wrapped in this hookshot.exe (patch 0046)")
        game.say("coop: down but not out: the game's play request not wrapped in this hookshot.exe (patch 0049)")
    else:
        game.say("coop: down but not out: the crawl's room check wrapped (a downed child keeps crawling)")
        game.say("coop: down but not out: the game's play request wrapped (a downed Link's lying pose is held)")


def here(game):
    return getattr(game, "dbno", {}).get("on", False)   # (fakes/dbno_crawl.py: that row's 'chat /dbno' since the boot)


def downed(game):
    return here(game) and game.dbno.get("state") == "down"


def clip(eye, facing):
    """A camera at `eye` looking along `facing` (yaw 0 = +z): rows x = right, y = up, w = forward, each - its dot
    with the eye, so clip x = y = w = 0 only at the eye (test_downed_move.eye)."""
    f = (math.sin(math.radians(facing)), 0.0, math.cos(math.radians(facing)))
    r = (f[2], 0.0, -f[0])
    u = (0.0, 1.0, 0.0)

    def row(v):
        return [v[0], v[1], v[2], -sum(v[i] * eye[i] for i in range(3))]
    return row(r) + row(u) + [0.0, 0.0, 0.0, 1.0] + row(f)


def state(game, st):
    if st.get("pos") is None or not here(game):
        return
    dm = game.downed_move
    if downed(game) and not dm["seen"]:
        dm["seen"], dm["pos"] = True, list(game.pos)
        if game.age() == "adult" and not bug("noskip"):
            game.later(5, lambda: game.say("coop: dbno: our downed Link: the game's play of 'Wait' on slot 0 skipped "
                                           "(the lying pose is held; the game's ground verb still runs)"))
        if game.age() == "adult" and not bug("still"):
            game.later(25, lambda: game.say("coop: dbno: our downed Link: the game's action verb 1 let through: he "
                                            "moves lying, in the held pose"))
        elif game.age() != "adult" and not bug("still"):
            game.later(25, lambda: game.say("coop: dbno: our downed Link: the game's crawl asks for room to stand up: "
                                            "answered no room, the crawl goes on"))
    elif not downed(game):
        dm["seen"] = False
    close = downed(game) and not bug("farcam")
    back, up = (CRAWL_BACK, CRAWL_UP) if close else (STAND_BACK, STAND_UP)
    p, yaw = game.pos, math.radians(game.facing)
    e = [p[0] - back * math.sin(yaw), p[1] + up, p[2] - back * math.cos(yaw)]
    st["view"] = {"yaw": game.facing, "pitch": -10.0, "clip": clip(e, game.facing), "y_down": False}


def after(game, m, r):
    if m.get("cmd") == "shot" and bug("flicker") and "-still" in (m.get("name") or "") and r.get("path"):
        game.downed_move["shots"] += 1
        if game.downed_move["shots"] % 2:
            from PIL import Image, ImageDraw
            im = Image.open(r["path"]).convert("RGB")
            ImageDraw.Draw(im).rectangle([120, 10, 200, 170], fill=(250, 250, 250))   # (a Link standing)
            im.save(r["path"])
        return
    if m.get("cmd") == "input" and m.get("stick") and downed(game) and bug("still") and game.downed_move["pos"]:
        game.pos = list(game.downed_move["pos"])
