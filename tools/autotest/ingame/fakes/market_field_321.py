"""Fake-game reactions for row 321's test (ingame/test_market_field_321.py; owner crash-sweep-0120-5). Hooks: fakegame.py
docstring. Reacts only in this row's own runs (B #149): with BT_ROW321=1 in the boot's env (the pair test's switch) or in a
--solo boot (the solo test carries no switch: tests/test_ingame.py's solo run deselects only pair tests), and there only to a warp to HyruleField_Adult at HyruleField_FrontOfMarket made FROM Market_Adult (no other
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


ON = "BT_ROW321=1"
IDLE = "coop: no server in coop.ini: co-op idle"     # solo.IDLE's start: a --solo boot's first module line


def setup(game):
    game.mf321_from = None
    try:
        with open(game.log_path, encoding="utf-8", errors="replace") as fh:
            game.mf321_solo = IDLE in fh.read()
    except OSError:
        game.mf321_solo = False


def _on(game):
    return game.env_on(ON) or getattr(game, "mf321_solo", False)


def command(game, m):
    if _on(game) and m.get("cmd") == "warp":
        game.mf321_from = game.scene
    return None


def after(game, m, r):
    if not _on(game) or m.get("cmd") != "warp" or not r.get("ok"):
        return
    if (m.get("scene") == FIELD and m.get("entrance") == FIELD_START and
            (getattr(game, "mf321_from", None) or "") == MARKET):
        game.say(MEMORY)
        if os.environ.get("BT_FAKE_MARKET_FIELD") == "crash":
            game.say(CRASH)
