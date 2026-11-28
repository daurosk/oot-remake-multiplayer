"""Row 53 facing probe (owner row-53-ocarina-7): test_row53.py failed at p-warp-face on 182cd53b5 (run row53-tot-182cd53b):
after `face 118.38 -34.00` the state's facing read -180.0, not 90. This test finds out where the turn is lost.
At the same p-warp spot it reads facing before the face line, 1, 2, 30 and 120 frames after it (notes), and needs the
module's own log line 'coop: test face X Z: Link at ... turned to yaw' (coop_boss_game.cpp, Mover::Step) in game A's
log: that line proves the rotation write ran. Verdict by the notes:
- no log line            -> the face line never reached the Mover (act refused / glue off / Link actor not found)
- line, facing stays old -> the write ran but state.facing reads another field
- line, facing 90 at 1-2 frames but not at 120 -> the game turns Link back on the next frames
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_rows import need_child_in_play   # noqa: E402

SCENE = "Market_Child"
SPOT = (84.75, 105.5, -34.0)     # test_row53.SPOTS p-warp
DOOR = (118.38, -34.0)           # test_row53 DOOR_X, DOOR_Z
WAIT_WARP, WAIT_PLACE = 600, 240  # test_row53's frames


@pytest.mark.budget(120)
@pytest.mark.row("#53 facing probe (face X Z at p-warp, test_row53.py p-warp-face)")
def test_row53_face_probe(pair):
    with pair.setup():
        need_child_in_play(pair)
        pair.warp_both(SCENE)
    pair.step_both(WAIT_WARP)
    pair.act("A", "place %.2f %.2f %.2f" % SPOT)
    pair.step_both(WAIT_PLACE)
    reads = [("before", pair.state("A")["facing"])]
    pair.act("A", "face %.2f %.2f" % DOOR)
    done = 0
    for frames in (1, 1, 28, 90):
        pair.step_both(frames)
        done += frames
        reads.append(("+%d" % done, pair.state("A")["facing"]))
    pair.notes.append("facing readings (expect 90 after the face): " + ", ".join("%s %.1f" % r for r in reads))
    line = pair.log["A"].find(r"coop: test face .*turned to yaw")
    pair.notes.append("face log line: %s" % (line or "NONE"))
    assert line is not None, "no 'test face ... turned to yaw' line in A's log: the face line never ran (%s)" % (
        ", ".join("%s %.1f" % r for r in reads))
