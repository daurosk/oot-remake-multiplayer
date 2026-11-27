"""Fake-game reactions for ingame/test_stepped_hud.py (hud-fresh-0121's in-game proof; hud-stepped-0121).

- each game logs hookshot/src/coop_hookshot.cpp PartyStep's "coop: party hud: on the game's HUD" once both exist, but
  not in fakes/party_hud.py's row (COOP_TEST_PARTY_HUD=1: its own lines; there B must never say it). The name tags'
  line comes from fakes/regress_ui.py's lane hook.
- the state's `slow_steps` (docs/playtest.md; coop_hookshot.h SlowStep): every frame-running message (step, shot, input,
  move) whose first frame comes a second or more after the game's last frame is a slow step of the party HUD and of the
  name tags; it shows 1 block and 1 tag, as the module after hud-fresh-0121.
Selftest switch (tests/test_ingame_stepped_hud.py): BT_FAKE_STEPPED_BUG=1 -> a slow step shows 0 of both (the module
before hud-fresh-0121: testkit-0120 kit-in-play).
"""
import os
import time

SLOW_S = 1.0
RUNS = ("step", "shot", "input", "move")


def _bug():
    return os.environ.get("BT_FAKE_STEPPED_BUG") == "1"


def _sh(game):
    if not hasattr(game, "sh_slow"):
        game.sh_last = None
        game.sh_slow = {k: {"count": 0, "gap_ms": 0, "shown": 0} for k in ("party_hud", "name_tags")}
    return game


def setup(game):
    _sh(game)


def lane(fl):
    for g in (fl.g["A"], fl.g["B"]):
        if g.env_on("COOP_TEST_PARTY_HUD=1"):   # (fakes/party_hud.py's row: its own lines, B never on)
            continue
        g.later(5, lambda g=g: g.say("coop: party hud: on the game's HUD"))


def command(game, m):
    g = _sh(game)
    if m.get("cmd") in RUNS:
        now = time.monotonic()
        if g.sh_last is not None and now - g.sh_last >= SLOW_S:
            shown = 0 if _bug() else 1
            for k in g.sh_slow:
                g.sh_slow[k] = {"count": g.sh_slow[k]["count"] + 1, "gap_ms": int((now - g.sh_last) * 1000),
                                "shown": shown}
    return None


def after(game, m, r):
    if m.get("cmd") in RUNS:
        _sh(game).sh_last = time.monotonic()


def state(game, st):
    st["slow_steps"] = {k: dict(v) for k, v in _sh(game).sh_slow.items()}
