"""Readable names for Character Select's unnamed characters (puppet_pack/charsel_names.py; menu-customize-01113)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import charsel_names as cn, msbt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_ui_pack import make_msbt  # noqa: E402

# (shared with tests/hookshot/charsel_names_test.cpp: both sides give these names)
FIXTURE = os.path.join(HERE, "..", "..", "..", "tests", "hookshot", "charsel-names.txt")


def fixture():
    his, want = [], []
    with open(FIXTURE, encoding="utf-8") as f:
        for line in f.read().split("\n"):
            if not line or line.startswith("#"):
                continue
            a, b = line.split("\t")
            his.append(a)
            want.append(b)
    return his, want


class KindTest(unittest.TestCase):
    def test_his_names_stay(self):
        for n in ("Saria", "Zelda (child)", "Iron Knuckle (Nabooru, cutscene)", "Hylia_mn_001", "Hylia_MN_",
                  "Hylia_MN_001_", "_MN_001"):
            self.assertIsNone(cn.kind_of(n), n)

    def test_model_ids(self):
        for n, k in (("Hylia_MN_005", "Townsman"), ("Hylia_WF_002", "Townswoman"), ("Hylia_MO_001", "Old man"),
                     ("Hylia_WO_001", "Old woman"), ("Hylia_MS_001", "Soldier"), ("Kokiri_MC_003", "Kokiri boy"),
                     ("Kokiri_WC_001", "Kokiri girl"), ("Gerudo_GEN_011_Leader", "Gerudo leader"),
                     ("Gerudo_GEN\n011_Leader", "Gerudo leader"), ("Hylia_ZZ_001", "Hylian"),
                     ("Sheikah_SN_001", "Sheikah")):
            self.assertEqual(cn.kind_of(n), k, n)

    def test_numbered_by_the_ids_order_only_when_more_than_one(self):
        self.assertEqual(cn.readable(["Goron_GON_001", "Saria", "Goron_GON_006", "Zora_ZN_001", "Goron_GON_002"]),
                         ["Goron 1", "Saria", "Goron 3", "Zora", "Goron 2"])
        self.assertEqual(cn.readable(["Hylia_MN_010", "Hylia_MF_002", "Hylia_MN_9", "Gerudo_GEN_011_Leader",
                                      "Gerudo_GEN_011_Chief", "Gerudo_GEN_011_Leader"]),
                         ["Townsman 3", "Townsman 1", "Townsman 2", "Gerudo leader 1", "Gerudo chief",
                          "Gerudo leader 2"])
        self.assertIsNone(cn.kind_of("Hylia_MN_1234567890123456789"))   # (19 digits: not an id, as the C++ side)

    def test_another_order_names_each_character_alike(self):
        # (review-01112-h: his picker's slots and his characters.json are two lists; a release ordering one otherwise)
        his, _ = fixture()
        by_id = dict(zip(his, cn.readable(his)))
        other = list(reversed(his))
        other = other[7:] + other[:7]
        for name, shown in zip(other, cn.readable(other)):
            self.assertEqual(shown, by_id[name], name)

    def test_his_picker_in_another_slot_order_gives_the_same_names(self):
        a = make_msbt([("T_Slot_000", "Goron_GON_001"), ("T_CellName_000", "Goron_GON_001"),
                       ("T_Slot_001", "Goron_GON_006"), ("T_CellName_001", "Goron_GON_006"),
                       ("T_Slot_002", "Goron_GON_002"), ("T_CellName_002", "Goron_GON_002")])
        b = make_msbt([("T_Slot_000", "Goron_GON_002"), ("T_CellName_000", "Goron_GON_002"),
                       ("T_Slot_001", "Goron_GON_001"), ("T_CellName_001", "Goron_GON_001"),
                       ("T_Slot_002", "Goron_GON_006"), ("T_CellName_002", "Goron_GON_006")])
        ga, gb = msbt.read(cn.tidy_picker(a)), msbt.read(cn.tidy_picker(b))
        self.assertEqual([ga.text("T_Slot_%03d" % i)[0] for i in range(3)], ["Goron 1", "Goron 3", "Goron 2"])
        self.assertEqual([gb.text("T_Slot_%03d" % i)[0] for i in range(3)], ["Goron 2", "Goron 1", "Goron 3"])

    def test_his_v022_list_as_the_cpp_side(self):
        his, want = fixture()
        self.assertEqual(len(his), 117)
        self.assertEqual(cn.readable(his), want)
        # (every readable name of a model id fits one line of his label)
        for a, b in zip(his, want):
            if a != b:
                self.assertLessEqual(len(b), cn.LABEL_ROOM, b)

    def test_label_lines(self):
        self.assertEqual(cn.label_lines("Townswoman 14"), "Townswoman 14")
        self.assertEqual(cn.label_lines("Gerudo leader of the fortress"), "Gerudo leader\nof the fortress")
        self.assertEqual(cn.label_lines("Averyveryverylongname"), "Averyveryverylongname")


class PickerTest(unittest.TestCase):
    def picker(self):
        return make_msbt([("T_Title_00", "Character Select"), ("T_Slot_000", "Link"), ("T_CellName_000", "Link"),
                          ("T_Slot_001", "Gerudo_GEN\n011_Leader"), ("T_CellName_001", "Gerudo_GEN_011_Leader"),
                          ("T_Slot_002", "Hylia_WF_001"), ("T_CellName_002", "Hylia_WF_001"),
                          ("T_Slot_003", "Hylia_WF_002"), ("T_CellName_003", "Hylia_WF_002"),
                          ("T_Custom_00", "Customize")])

    def test_his_picker_gets_readable_names_his_own_kept(self):
        got = msbt.read(cn.tidy_picker(self.picker()))
        self.assertEqual(got.text("T_Slot_001"), ["Gerudo leader"])
        self.assertEqual(got.text("T_CellName_001"), ["Gerudo leader"])
        self.assertEqual(got.text("T_Slot_002"), ["Townswoman 1"])
        self.assertEqual(got.text("T_CellName_003"), ["Townswoman 2"])
        for label, text in (("T_Slot_000", "Link"), ("T_Title_00", "Character Select"), ("T_Custom_00", "Customize")):
            self.assertEqual(got.text(label), [text])

    def test_again_changes_nothing(self):
        once = cn.tidy_picker(self.picker())
        self.assertEqual(cn.tidy_picker(once), once)

    def test_a_picker_without_ids_is_the_same_bytes(self):
        d = make_msbt([("T_Slot_000", "Link"), ("T_CellName_000", "Link")])
        self.assertIs(cn.tidy_picker(d), d)


if __name__ == "__main__":
    unittest.main()
