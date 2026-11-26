"""Fake-game reactions for row 203's small key test (ingame/test_small_keys.py; owner proof-rows-0120). Hooks:
fakegame.py docstring.

A `flag` of DungeonInfo.GerudoFortress.SmallKeyNum in one game moves the other's by the same delta 12 frames later
(coop/host/src/game_items.cpp:93 R::count; flag_watcher.cpp FlagWatcher::add adds the delta); a 0 -> 1 of
IsGet_SmallKey_GerudoFortress.SmallKey01 is set in the other (coop_story.cpp:272, Kind::world: shared 0 -> 1 only).
`flag_read` of the count answers kind "int" (docs/playtest.md "Commands": flag_read).
BT_FAKE_KEYS=lost: the count's delta never reaches the other game (the bug as reported); =double: it lands twice.
"""
import os

COUNT = "DungeonInfo.GerudoFortress.SmallKeyNum"
KEY01 = "IsGet_SmallKey_GerudoFortress.SmallKey01"


def setup(game):
    game.flags.setdefault(COUNT, 0)
    game.flags.setdefault(KEY01, 0)


def command(game, m):
    if m.get("cmd") != "flag" or m.get("name") not in (COUNT, KEY01):
        return None
    name, value = m["name"], m["value"]
    was = game.flags.get(name, 0)
    o = game.other
    if o is not None and name == COUNT:
        delta, mode = int(value) - int(was), os.environ.get("BT_FAKE_KEYS")
        if delta and mode != "lost":
            for _ in range(2 if mode == "double" else 1):
                o.later(12, lambda d=delta: o.flags.__setitem__(COUNT, o.flags.get(COUNT, 0) + d))
    if o is not None and name == KEY01 and value in (1, True) and was in (0, False):
        o.later(12, lambda: o.flags.__setitem__(KEY01, 1))
    return None   # (the built-in answer sets this game's own flag and replies `was`)


def after(game, m, r):
    if m.get("cmd") == "flag_read" and m.get("name") == COUNT:
        r["kind"] = "int"
