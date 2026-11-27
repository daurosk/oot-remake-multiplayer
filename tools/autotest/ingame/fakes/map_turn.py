"""The map arrow's turn probe (test_map_turn_probe.py) on the fake SOLO game: what a real game with map-icons-255-2
gives the test with no other player.

- setup: the game knows it is a solo boot (its log says solo.IDLE: fakegame's solo_log, bt_plugin --solo).
- state (after fakes/map_icons.py, file-name order): on a solo boot in InsideTheDekuTree with its map flag set,
  `map_native` is the module's probe shape (coop_map_icons.cpp Icons::ProbeArrow + Json): nothing up or shown,
  `probe` true, the game's own arrow's words with our turn at index 2 (+0x68; radians, -facing - a quarter, as
  fakes/map_icons.py), no run-time copies. BT_FAKE_MAP_TURN_WORD=<i> puts the turn at another index (the FAIL path's
  selftest), BT_FAKE_MAP_TURN_WORD=none nowhere (the NO-REF path).
"""
import math
import os

import solo

SCENE = "InsideTheDekuTree"
MAP_FLAG = "DungeonInfo.InsideTheDekuTree.IsGetMap"   # (test_map_turn_probe.MAP_FLAG)


def setup(game):
    with open(game.log_path, encoding="utf-8", errors="replace") as f:
        game.map_turn_solo = solo.IDLE in f.read()


def _turn(facing_deg):
    t = -math.radians(facing_deg) - math.pi / 2
    return math.atan2(math.sin(t), math.cos(t))


def state(game, st):
    if not getattr(game, "map_turn_solo", False):
        return
    st["map_native"] = None
    if st.get("scene") != SCENE or not (getattr(game, "flags", None) or {}).get(MAP_FLAG):
        return
    where = os.environ.get("BT_FAKE_MAP_TURN_WORD", "2")
    words = [1.0, 1.0, 0.0, 0.5, 0.5, 1.0, 1.0, 0.0]
    if where != "none":
        words[int(where)] = _turn(game.facing or 0.0)
    st["map_native"] = {"up": False, "why": "", "table": True, "turn_at": 0, "game_arrow": True,
                        "game_arrow_words": words, "run_arrow_words": [], "probe": True, "shown": []}
