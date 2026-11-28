"""Fake-game reactions for row 321's test (ingame/test_market_field_321.py; owner crash-sweep-0120-5). Hooks: fakegame.py
docstring. Reacts only to a warp to HyruleField_Adult at HyruleField_FrontOfMarket made FROM Market_Adult (no other
test makes that change: test_enemies stays in the Market, the epona tests come to the field from elsewhere).

What a real game logs there, from the source the test cites:
- the 0.12.0 memory line once a minute, solo too (hookshot/src/coop_hookshot.cpp Frame, row 271:
  "coop: memory: " + CoopHostMenu::ProcessMemoryText(), seen as "coop: memory: process: private 28448 MB, working set
  13326 MB, ..." in player-logs/owner-p1-blackscreen-20261128-055459/longshot_log.txt). Said here at each such arrival.
BT_FAKE_MARKET_FIELD=crash: the arrival also writes the module's crash line (bt_plugin.CRASH_LINE): the test must FAIL.
"""
import os

FIELD, FIELD_START, MARKET = "HyruleField_Adult", "HyruleField_FrontOfMarket", "Market_Adult"
MEMORY = "coop: memory: process: private 20480 MB, working set 9000 MB (fake)"
CRASH = "CRASH: exception (fake: BT_FAKE_MARKET_FIELD=crash)"


def setup(game):
    game.mf321_from = None


def command(game, m):
    if m.get("cmd") == "warp":
        game.mf321_from = game.scene
    return None


def after(game, m, r):
    if m.get("cmd") != "warp" or not r.get("ok"):
        return
    if (m.get("scene") == FIELD and m.get("entrance") == FIELD_START and
            (getattr(game, "mf321_from", None) or "") == MARKET):
        game.say(MEMORY)
        if os.environ.get("BT_FAKE_MARKET_FIELD") == "crash":
            game.say(CRASH)
