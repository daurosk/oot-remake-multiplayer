"""Tests for designed_enemies.py (difficulty's placements an extra copy could spoil). Synthetic data only.

    python tools/puppet_pack/tests/test_designed_enemies.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import designed_enemies as de  # noqa: E402


def link(src, dst, name="Reference"):
    return {"Src": src, "Dst": dst, "Gyaml": "Reference", "Name": name}


class DesignedTest(unittest.TestCase):
    def test_reasons(self):
        scenes = {
            "Room": [
                {"Gyaml": "Enemy_Moblin", "Hash": 1, "Rails": [{"Dst": 99}]},
                {"Gyaml": "Enemy_Keese", "Hash": 2},
                {"Gyaml": "Enemy_Stalfos", "Hash": 3},
                {"Gyaml": "Obj_EnemyCounter_A_01", "Hash": 4, "Links": [link(4, 2)]},
                {"Gyaml": "Obj_IceCave_SwitchShutter", "Hash": 5, "Links": [link(5, 3)]},
                {"Gyaml": "Enemy_Lizalfos", "Hash": 6, "Links": [link(6, 7)]},
                {"Gyaml": "Enemy_Lizalfos", "Hash": 7},
                {"Gyaml": "EventRequestActor", "Hash": 8, "Links": [link(8, 9)]},
                {"Gyaml": "Enemy_Tektite_Blue", "Hash": 9},
                {"Gyaml": "Enemy_Wolfos", "Hash": 10, "Dynamic": {"Drop__DropActor": "Obj_Dungeon_SmallKey_X"}},
                {"Gyaml": "Enemy_Octorok", "Hash": 11, "Dynamic": {"Drop__DropActor": "Obj_Rupee_Blue"}},
                {"Gyaml": "Enemy_Scrub_Business", "Hash": 12, "Dynamic": {"TalkEventName": "Talk"}},
                {"Gyaml": "Enemy_Amos_Dictionary", "Hash": 13, "Rails": [{"Dst": 99}]},
            ]
        }
        found = de.designed(scenes)
        self.assertEqual(found[1][0], "rail")
        self.assertNotIn(2, found)  # only a counter links it: copies stay
        self.assertEqual(found[3][0], "shutter")
        self.assertEqual(found[6][0], "group")
        self.assertEqual(found[7][0], "group")
        self.assertEqual(found[9][0], "event")
        self.assertEqual(found[10][0], "drop")
        self.assertNotIn(11, found)  # a rupee is no set drop
        self.assertEqual(found[12][0], "event")
        self.assertNotIn(13, found)  # picture book
        self.assertNotIn(5, found)  # not an enemy

    def test_priority_and_render(self):
        scenes = {"A": [{"Gyaml": "Enemy_Stalfos", "Hash": 0x20, "Rails": [{}], "Links": [link(0x20, 0x21)]},
                        {"Gyaml": "Enemy_Stalfos", "Hash": 0x21}]}
        found = de.designed(scenes)
        self.assertEqual(found[0x20][0], "rail")  # rail before group
        text = de.render(found)
        lines = [ln for ln in text.splitlines() if ln.startswith("    {")]
        self.assertEqual(lines[0], "    {0x0000000000000020ull, Designed::rail},  // Enemy_Stalfos, A")
        self.assertEqual(len(lines), 2)  # sorted by hash

    def test_u64(self):
        self.assertEqual(de.u64(5), 5)
        self.assertEqual(de.u64("UInt64(9100301989726492447)"), 9100301989726492447)
        self.assertIsNone(de.u64(None))


if __name__ == "__main__":
    unittest.main()
