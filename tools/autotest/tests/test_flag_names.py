"""Every game flag name the in-game tests and boot scenarios use is a flag the game has (tools/autotest/ingame/
flag_names.py, B #168). No game, no fake, no romfs: the names are checked against flag_table.json, which
flag_names.py --romfs made from the game's GameDataList. With a romfs (OOT_ROMFS, or the dump beside the repo) the
table itself is checked against it too.

Cause (probe0121e-16ac9701, 2026-11-28 05:07 UTC): test_epona's GET_EPONA was 'Get_Epona', a name the game has no flag
for; every 'won' set-up wrote nothing and every read gave None, and no check saw it before a lane run.
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, INGAME)
import flag_names as F  # noqa: E402

ROMFS = os.environ.get("OOT_ROMFS") or os.path.join(os.path.dirname(HERE), "..", "..", "..", "dump", "romfs")

TABLE = {
    "Step_Get_Epona": {"type": "Enum", "hash": "0x92431c55", "values": ["NotReady", "Complete"],
                       "hashes": ["0x57b849f0"]},
    "PlayerStatus.Life": {"type": "Int", "hash": "0x31ab5580"},
    "0x31ab5580": {"type": "Int", "hash": "0x31ab5580"},
    "Some_Float": {"type": "Float", "hash": "0x00000001"},
    "OwnedHorseList.ActorName": {"type": "String64Array", "hash": "0x7bde80e9"},
    "key64:0x0b66198d17d6584e": {"type": "Bool64bitKey"},
}


def uses(*rows):
    return [F.Use(n, op, v, "t:%d" % i) for i, (op, n, v) in enumerate(rows)]


class RulesTest(unittest.TestCase):
    def test_good_uses_pass(self):
        self.assertEqual(F.problems(uses(
            ("read", "Step_Get_Epona", None), ("enum", "Step_Get_Epona", "Complete"),
            ("enum", "Step_Get_Epona", "0x57b849f0"), ("enum", "Step_Get_Epona", "?"),
            ("set", "PlayerStatus.Life", 12), ("read", "0x31ab5580", None),
            ("read", "OwnedHorseList.ActorName[0]", None), ("text", "OwnedHorseList.ActorName[0]", "Horse"),
            ("set", "key64:0x0B66198D17D6584E", 1), ("read", "key64:0x0b66198d17d6584e", None)), TABLE), [])

    def test_the_get_epona_case(self):
        bad = F.problems(uses(("enum", "Get_Epona", "Complete")), TABLE)
        self.assertEqual(len(bad), 1)
        self.assertIn("'Get_Epona': not in flag_table.json", bad[0])

    def test_wrong_enum_value(self):
        bad = F.problems(uses(("enum", "Step_Get_Epona", "Won"), ("enum", "Step_Get_Epona", "0x12345678")), TABLE)
        self.assertEqual(len(bad), 2)
        self.assertIn("not one of the Enum's values", bad[0])

    def test_setflag_by_hash_and_wrong_kinds(self):
        bad = F.problems(uses(("set", "0x31ab5580", 4), ("write", "0x31ab5580", None), ("set", "Step_Get_Epona", 1),
                              ("read", "Some_Float", None), ("enum", "PlayerStatus.Life", "Complete"),
                              ("read", "PlayerStatus.Life[0]", None)), TABLE)
        self.assertEqual(len(bad), 6, bad)
        self.assertIn("a 0x name is hashed as text by setflag", bad[0])
        self.assertIn("a 0x name is hashed as text by setflag", bad[1])
        self.assertIn("setflag writes only Int or Bool", bad[2])
        self.assertIn("ReadFlag does not read", bad[3])
        self.assertIn("not an Enum", bad[4])
        self.assertIn("needs a String64Array", bad[5])

    def test_murmur3_is_the_games(self):
        # coop_quests.h:70 kGetEpona = 0x92431c55 (Enum Step_Get_Epona); game_items.cpp:83 PlayerStatus.Life 0x31ab5580
        self.assertEqual(F.murmur3("Step_Get_Epona"), 0x92431c55)
        self.assertEqual(F.murmur3("PlayerStatus.Life"), 0x31ab5580)
        self.assertEqual(F.murmur3("PlayerStatus.CurrentRupee"), 0xa77921d7)


class ScanTest(unittest.TestCase):
    def scan(self, src, scenario=""):
        with tempfile.TemporaryDirectory() as d:
            os.mkdir(os.path.join(d, "scen"))
            with open(os.path.join(d, "test_x.py"), "w", encoding="utf-8") as fh:
                fh.write(src)
            with open(os.path.join(d, "scen", "x.A.txt"), "w", encoding="utf-8") as fh:
                fh.write(scenario)
            return F.scan(d, os.path.join(d, "scen"))

    def test_finds_every_form(self):
        got, unresolved = self.scan(
            'QUEST = "Step_Get_Epona"\n'
            'NAMES = ("A_Flag", "B_Flag")\n'
            'BOTTLES = {"a": "0x7d48df76"}\n'
            'def quest(pair, x):\n'
            '    return pair.flag_read(x, QUEST)\n'
            'def is_set(pair, x, name):\n'
            '    return pair.flag_read(x, name)\n'
            'def test_t(pair):\n'
            '    pair.flag("A", QUEST, "Complete")\n'
            '    for n in NAMES:\n'
            '        pair.flag("A", n, 0)\n'
            '    for h in BOTTLES.values():\n'
            '        pair.act("A", "setenum %s Empty" % h)\n'
            '    is_set(pair, "B", "C_Flag")\n',
            "setflag Sequence_IsChildhood 1\nsetenum Step_Get_Epona ?\n")
        self.assertEqual(unresolved, [])
        self.assertEqual(sorted((u.op, u.name, u.value) for u in got), sorted([
            ("read", "Step_Get_Epona", None), ("enum", "Step_Get_Epona", "Complete"),
            ("set", "A_Flag", 0), ("set", "B_Flag", 0), ("enum", "0x7d48df76", "Empty"),
            ("read", "C_Flag", None), ("set", "Sequence_IsChildhood", "1"), ("enum", "Step_Get_Epona", "?")]))

    def test_a_name_it_cannot_work_out_is_reported(self):
        _got, unresolved = self.scan('def test_t(pair):\n    pair.flag_read("A", "IsGet." + pair.item)\n')
        self.assertEqual(len(unresolved), 1)
        self.assertIn("test_x.py:2 test_t()", unresolved[0])


class RepoTest(unittest.TestCase):
    """The repo as it is: every use is a flag the game has, of a kind that use can take."""

    @classmethod
    def setUpClass(cls):
        cls.uses, cls.unresolved = F.scan()
        cls.table = F.load_table()

    def test_scan_sees_the_tests(self):
        names = {u.name for u in self.uses}
        for n in ("Step_Get_Epona", "Sequence_IsChildhood", "PlayerStatus.Life", "PlayerStatus.CurrentRupee"):
            self.assertIn(n, names)
        self.assertGreater(len(self.uses), 500)

    def test_every_name_is_worked_out(self):
        self.assertEqual(self.unresolved, [])

    def test_no_problems(self):
        self.assertEqual(F.problems(self.uses, self.table), [])

    def test_table_has_only_used_names(self):
        used = {F.key_of(u.name) for u in self.uses}
        self.assertEqual(sorted(set(self.table) - used), [], "stale rows: run flag_names.py --romfs")

    @unittest.skipUnless(os.path.isfile(os.path.join(ROMFS, F.GAME_LIST)), "no romfs here (CI): table taken as made")
    def test_table_is_the_romfs(self):
        made, missing = F.make_table(ROMFS, self.uses)
        self.assertEqual(missing, [])
        self.assertEqual(made, self.table)


if __name__ == "__main__":
    unittest.main()
