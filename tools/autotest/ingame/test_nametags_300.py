"""Row 300 (nametags-300, 0.12.1): the other player's name tag over his Link, on EACH game.

OWNER (0.12.0, his own 2-game local co-op): the tags do not show above the other player's model (only the party HUD
list). Lane proof of the same on 0.12.0 (offline/gate-ui-2f734963 regress-tags-1-A.png): A's log says "name tags on the
game's balloons (1 shown, ...)" yet the shot has B's puppet with no tag (the "B Kakariko Village" line there is the party
HUD's). Cause (hookshot/src/coop_native_tags.h addr::kLayoutOpen): each balloon part is a layout with its own open
state (part+0x3a); the game's balloon step f_8022b900 opens one with f_80020d00 before it is seen and keeps the screen's
own root pane shown only while an NPC balloon of its own faces the camera. Co-op only showed the part's root pane: the
part stayed closed. The fix opens/closes our parts as the game does and keeps the screen root shown while ours are up.

Checks per viewer x (A, then B), each on its own state and its own framebuffer:
- set-up: the other Link TAGS_DIST m in front of x along x's own facing (`place` only: x is never `face`d, so the
  game's ZL reset puts x's camera behind x looking at the other; gate-ui-epona-0121 item 2: a `face`d game's ZL
  left its camera on the far side), x's camera behind x by ZL, proven on x's state (test_regress_ui.other_in_view).
- state `name_tags` (coop_playtest.cpp, CoopHookshot::TagsSeen): shown >= 1, open >= 1 (the game holds our part
  open), screen_root true (the balloon screen's root pane shown), within TAGS_WAIT frames.
- the module's once line TAGS_ON, never TAGS_NO_MEMORY; no crash.
- a framebuffer shot of x (nametags-300-<x>): the tag over the other Link; NO-REF until the owner OKs a reference.
"""
import pytest

import pair as P
from test_regress_ui import (CAM_TRIES, CAM_WAIT, CRASH, TAGS_DIST, TAGS_NO_MEMORY, TAGS_ON, TAGS_WAIT, other_in_view,
                             seen_not, tags_open)
from test_rows import need_child_in_play

def ahead_of(pair, x, other):
    """Puts `other`'s Link TAGS_DIST m in front of x's Link along x's facing (state `facing`: 0 = +z, 90 = +x,
    docs/playtest.md "State"; test_rows.place_in_front's maths, without its `face`)."""
    import math
    s = pair.state(x)
    px, py, pz = s["pos"]
    yaw = math.radians(s["facing"])
    pair.act(other, "place %.2f %.2f %.2f" % (px + TAGS_DIST * math.sin(yaw), py, pz + TAGS_DIST * math.cos(yaw)))
    pair.step_both(10)


def camera_behind(pair, x, other):
    why = ""
    for n in range(CAM_TRIES):
        pair.press(x, ["ZL"], frames=8)
        pair.step_both(CAM_WAIT)
        ok, why = other_in_view(pair, x, other)
        if ok:
            pair.notes.append("camera behind %s after %d ZL: %s" % (x, n + 1, why))
            return
    raise P.SetupFail("%s's camera not behind its Link facing %s after %d ZL: %s" % (x, other, CAM_TRIES, why))


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("#300 the other player's name tag over his Link on each game (game's balloon opened, screen root shown)")
def test_name_tag_over_other(pair):
    with pair.setup():
        need_child_in_play(pair)
    for x, other in (("A", "B"), ("B", "A")):
        with pair.setup():
            ahead_of(pair, x, other)
            camera_behind(pair, x, other)
        seen_not(pair, x, TAGS_ON, TAGS_NO_MEMORY, max_frames=TAGS_WAIT)
        t = tags_open(pair, x)
        ok, why = other_in_view(pair, x, other)
        assert ok, "%s: the other player's puppet and tag are not in view at the shot: %s" % (x, why)
        res = pair.check_shot(x, "nametags-300", tolerance=0.80)   # 0.80: Kakariko's people and birds move
        pair.notes.append("%s name_tags %s; shot: %s" % (x, t, res.get("why")))
    for x in ("A", "B"):
        pair.never_log(x, CRASH)
