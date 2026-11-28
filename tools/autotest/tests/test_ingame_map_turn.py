"""Selftests of tools/autotest/ingame/test_map_turn_probe.py (map-icons-255-2) on the fake SOLO game
(ingame/fakes/map_turn.py); loaded by test_ingame.py's load_tests."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_ingame import MISSING_REF, pytest_run, results, summary   # noqa: E402

TEST = "test_map_turn_probe"


class MapTurnProbe(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.refs = os.path.join(self.tmp, "refs")
        os.makedirs(self.refs)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_(self, name, extra=("--solo",), env=None):
        out = os.path.join(self.tmp, name)
        p = pytest_run(["test_map_turn_probe.py", "--fake", "--out", out, "-q", "-p", "no:cacheprovider", "--no-rerun"]
                       + list(extra), env=dict({"BT_FAKE_REFS": self.refs}, **(env or {})))
        return p, summary(out), out

    def test_turn_at_0x68_passes_once_refs_exist(self):
        """+0x68 follows the facing: run 1 NO-REF (only the 2 shots' refs missing), run 2 with them PASS, the notes
        say PROVEN; the shots are game A's only (solo), 2 of them."""
        p, s, out = self.run_("o1")
        self.assertEqual(results(s), {TEST: "NO-REF"}, p.stdout[-3000:])
        self.assertIn(MISSING_REF, s["tests"][0]["why"])
        new = os.path.join(out, "lane1", "new")
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(self.refs, f))
        p, s, _ = self.run_("o2")
        self.assertEqual(results(s), {TEST: "PASS"}, p.stdout[-3000:])
        r = s["tests"][0]
        self.assertTrue(any("kPaneTurn = 0x68 PROVEN on template" in n for n in r["notes"]), r["notes"])
        self.assertEqual({x["game"] for x in r["shots"]}, {"A"})
        self.assertEqual(len(r["shots"]), 2)

    def test_another_word_is_fail_naming_it(self):
        p, s, _ = self.run_("o", env={"BT_FAKE_MAP_TURN_WORD": "4"})
        self.assertEqual(results(s), {TEST: "FAIL"}, p.stdout[-3000:])
        self.assertIn("('template', 4, 'facing'", s["tests"][0]["why"])

    def test_no_word_is_unproven(self):
        p, s, _ = self.run_("o", env={"BT_FAKE_MAP_TURN_WORD": "none"})
        self.assertEqual(results(s), {TEST: "NO-REF"}, p.stdout[-3000:])
        self.assertIn("kPaneTurn stays 0", s["tests"][0]["why"])

    def test_v2_raw_place_is_fail_naming_it(self):
        """probe v2: the turn only at an own word outside +0x60..+0x7c (+0x84): FAIL naming '+0x84', drawn turn seen."""
        p, s, _ = self.run_("o", env={"BT_FAKE_MAP_TURN_WORD": "none", "BT_FAKE_MAP_TURN_RAW": "84"})
        self.assertEqual(results(s), {TEST: "FAIL"}, p.stdout[-3000:])
        why = s["tests"][0]["why"]
        self.assertIn("('template', '+0x84', 'facing'", why)
        self.assertIn("drawn turn follows on [('template', 'facing'", why)

    def test_v2_raw_0x68_is_proven(self):
        """probe v2: +0x68 found among the raw words (the v1 words silent): the notes say PROVEN (NO-REF here only for
        the 2 missing shot refs)."""
        p, s, _ = self.run_("o", env={"BT_FAKE_MAP_TURN_WORD": "none", "BT_FAKE_MAP_TURN_RAW": "68"})
        r = s["tests"][0]
        self.assertEqual(results(s), {TEST: "NO-REF"}, p.stdout[-3000:])
        self.assertIn(MISSING_REF, r["why"])
        self.assertTrue(any("kPaneTurn = 0x68 PROVEN on template" in n for n in r["notes"]), r["notes"])

    def test_v2_matrix_only_is_unproven_saying_drawn(self):
        """probe v2: drawn turned (matrix follows) but no own word: NO-REF naming the matrix / animation."""
        p, s, _ = self.run_("o", env={"BT_FAKE_MAP_TURN_WORD": "none", "BT_FAKE_MAP_TURN_RAW": "matrix"})
        self.assertEqual(results(s), {TEST: "NO-REF"}, p.stdout[-3000:])
        self.assertIn("drawn turned (matrix follows on [('template', 'facing'", s["tests"][0]["why"])

    def test_not_solo_it_is_deselected(self):
        p, s, _ = self.run_("o", extra=())
        self.assertEqual(results(s), {}, p.stdout[-2000:])
        self.assertEqual([d.get("needs_solo") for d in s["deselected"]], [True])


if __name__ == "__main__":
    unittest.main()
