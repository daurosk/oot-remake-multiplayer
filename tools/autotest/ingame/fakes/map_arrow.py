"""Fake-game reactions for G2a's port (ingame/test_map_arrow.py; owner port-sweep-c131-6). Names are the module's
(test_map_arrow.py cites each writer).

- A's log says 'coop: hud: game look on Hookshot: the game font, heart pictures and map arrows loaded'
  (coop_hookshot.cpp:1075) once its co-op pack holds coop_hud/map-arrow.png (the harness's --pack hud put it there).
- In InsideTheDekuTree with DungeonInfo.InsideTheDekuTree.IsGetMap set, A's state `map_icons` (coop_map_drawn.h) has
  view 'minimap' with B (P2) as an arrow whose turn follows B's facing (the facing of B's last `move`); MINUS opens
  the Map page (view 'page'), B closes it. Elsewhere: view 'none'.
Selftest switch BT_FAKE_MAP_ARROW_BUG (tests/test_ingame_map_arrow.py): still -> the arrow never turns; noarrow -> the
name alone (no arrow); nopage -> MINUS leaves the minimap drawn.
"""
import math
import os

SCENE, MAP_FLAG = "InsideTheDekuTree", "DungeonInfo.InsideTheDekuTree.IsGetMap"
LOADED = "coop: hud: game look on Hookshot: the game font, heart pictures and map arrows loaded"
ARROW = os.path.join("mods", "contents", "0400486024DEC000", "coop-puppet", "coop_hud", "map-arrow.png")  # testpack.PACK_REL


def _bug():
    return os.environ.get("BT_FAKE_MAP_ARROW_BUG", "")


def setup(game):
    game.ma = {"page": False, "said": False}


def move_step(game, pos):
    dx, dz = pos[0] - game.pos[0], pos[2] - game.pos[2]
    if dx or dz:
        game.facing = math.degrees(math.atan2(dx, dz)) % 360.0   # (yaw: 0 toward +z, 90 toward +x)
    return pos


def press(game, buttons):
    if game.tag != "A" or game.scene != SCENE:
        return False
    if "MINUS" in buttons:
        game.ma["page"] = _bug() != "nopage"
        return True
    if "B" in buttons and game.ma["page"]:
        game.ma["page"] = False
        return True
    return False


def state(game, st):
    me = 1 if game.tag == "A" else 2
    for p in st["coop"]["players"]:
        p["self"] = p["number"] == me
    if game.tag == "A" and not game.ma["said"] and os.path.exists(os.path.join(game.folder, ARROW)):
        game.ma["said"] = True
        game.say(LOADED)
    view = "none"
    icons = []
    if game.tag == "A" and game.scene == SCENE and game.flags.get(MAP_FLAG) and game.in_play():
        view = "page" if game.ma["page"] else "minimap"
        face = 0.0 if _bug() == "still" else game.other.facing
        icons = [{"number": 2, "x": 1728.0, "y": 885.0, "turn": math.radians((face + 180.0) % 360.0),
                  "arrow": _bug() != "noarrow", "clamped": False, "alpha": 1.0, "floor_delta": 0, "label": "B"}]
    st["map_icons"] = {"view": view, "presents": game.frame, "width": 1920, "height": 1080, "icons": icons}
