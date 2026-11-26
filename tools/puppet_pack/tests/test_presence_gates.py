"""Tests for puppet_pack.presence_gates (row 233). No romfs here: the filter on made-up placements, and the
committed coop_presence_gates.inc checked against the shared flag lists it reads."""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))

from puppet_pack import presence_gates as pg  # noqa: E402
from puppet_pack.boss_beaten import murmur3  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


class PresenceGatesTest(unittest.TestCase):
    def test_keeps_room_pieces_gated_by_shared_flags(self):
        shared = {"WaterTemple_WaterLevelChange01"}
        rows = pg.gates([
            ("WaterTemple", "Obj_WaterTemple_WaterPlane_A_02",
             {"FlagName": "WaterTemple_WaterLevelChange01", "IsNegation": True}),
            ("WaterTemple", "Obj_WaterTemple_WaterPlane_A_02",
             {"FlagName": "WaterTemple_WaterLevelChange01", "IsNegation": True}),
            ("WaterTemple", "Enemy_Tektite_Blue", {"FlagName": "WaterTemple_WaterLevelChange01"}),  # an enemy
            ("WaterTemple", "EftLoc_WaterFall_Basin_L_00", {"FlagName": "WaterTemple_WaterLevelChange01"}),
            ("WaterTemple", "Obj_Pot_A_01", {"FlagName": "NotShared"}),
            ("WaterTemple", "Obj_Pot_A_01", None),
        ], shared)
        self.assertEqual(dict(rows), {("WaterTemple_WaterLevelChange01", True, "WaterTemple",
                                       "Obj_WaterTemple_WaterPlane_A_02"): 2})

    def test_committed_table_names_shared_flags_by_their_hash(self):
        shared = pg.shared_flags(REPO)
        with open(os.path.join(REPO, "lichii", "hook", "coop_presence_gates.inc"), encoding="utf-8") as f:
            rows = re.findall(r'\{0x([0-9a-f]{8}), (true|false), "(\w+)", "(\w+)", (\d+), "([^"]+)"\}', f.read())
        self.assertTrue(rows)
        for value, _neg, area, actor, _count, flag in rows:
            self.assertEqual(murmur3(flag), int(value, 16))
            self.assertIn(flag, shared)
            self.assertIn(area, pg.DUNGEONS)
            self.assertTrue(pg.is_piece(actor))


if __name__ == "__main__":
    unittest.main()
