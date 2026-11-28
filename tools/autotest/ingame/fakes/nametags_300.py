"""Fake-game reactions for row 300 (ingame/test_nametags_300.py). Hooks: fakegame.py docstring.

The state's `name_tags` (coop_playtest.cpp; CoopHookshot::TagsSeen), read by row 300 and by the regress gate's
test_regress_ui.test_name_tags (tags_open). The real module emits it in every state, so this hook does too (B #149:
no other fake writes the key, so fakegame's guard 1 holds; it reacts only to the near player, as the module): while the other
game's Link is within NEAR m of this game's (the module's tags show for a player near and in view), one tag of ours,
its part open, the balloon screen's root pane shown; else all 0 / false, as the module solo or with nobody near. The
TAGS_ON line comes from fakes/regress_ui.py's lane hook.
Selftest switch BT_FAKE_NAMETAGS300=closed: the module before row 300's fix (0.12.0): the tag "shown" but its part
never opened and the screen root hidden. The test must FAIL.
"""
import math
import os

NEAR = 6.0


def state(game, st):
    other = getattr(game, "other", None)
    near = other is not None and math.dist(game.pos, other.pos) <= NEAR
    closed = os.environ.get("BT_FAKE_NAMETAGS300") == "closed"
    st["name_tags"] = {"shown": 1 if near else 0, "open": 1 if near and not closed else 0,
                       "screen_root": near and not closed}
