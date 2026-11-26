"""Tests for shared_anchors.shared_in (row 175): an Obj_MovableBox_BasePosAnchor that two or more placed movables link
to is listed (coop_world.cpp kSharedAnchors); a block's own anchor is not. No game files.

    python tools/puppet_pack/tests/test_shared_anchors.py   (CI runs it through test_puppet_pack.py)
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import shared_anchors  # noqa: E402


def actor(name, h, at, *links):
    a = {"Gyaml": name, "Hash": h, "Translate": list(at)}
    if links:
        a["Links"] = [{"Dst": d, "Name": "Reference"} for d in links]
    return a


class SharedAnchors(unittest.TestCase):
    def test_hyrule_castle_boxes(self):
        # Two Lon Lon boxes on one anchor, the first standing exactly on it: listed.
        acts = [actor("Obj_MovableBox_BasePosAnchor", 9, (31.6244, 117.0387, -146.6301)),
                actor("Obj_LonLonRanch_MovableBox_A_01", 1, (31.6244, 117.0387, -146.6301), 9),
                actor("Obj_LonLonRanch_MovableBox_A_01", 2, (31.6244, 117.0387, -145.1301), 9)]
        got = shared_anchors.shared_in(acts)
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0][0], [31.6244, 117.0387, -146.6301])
        self.assertEqual(len(got[0][1]), 2)
        line = shared_anchors.cpp_lines({"HyruleCastle": got})[0]
        self.assertIn('{"HyruleCastle", {31.6244f, 117.0387f, -146.6301f}}', line)
        self.assertIn("nearest 0.00 m", line)

    def test_own_anchor_and_non_movable_links_are_not_shared(self):
        # One block per anchor (Deku Tree, issue 147) is not shared; Amy's puzzle manager links too, but is no movable.
        acts = [actor("Obj_MovableBox_BasePosAnchor", 9, (1, 2, 3)),
                actor("Obj_MovableBox_A_05", 1, (1, 2, 3), 9),
                actor("ForestTemple_MovableBox_Puzzle_Manager", 3, (1, 9, 3), 9),
                actor("Obj_MovableBox_BasePosAnchor", 8, (5, 2, 3)),
                actor("Obj_ForestTemple_MovableBox_A_01", 4, (5, 2, 3), 8)]
        self.assertEqual(shared_anchors.shared_in(acts), [])


if __name__ == "__main__":
    unittest.main()
