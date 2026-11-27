"""Selftests of test_hair_link.py (hair-link, report 293 P1) on the fake games (ingame/fakes/hair_link.py); its colour
check is vest-link's huecheck (selftested in test_ingame_vest_link.py). Run by tests/test_ingame.py's load_tests."""
import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ingame"))
from test_ingame import pytest_run, results, summary   # noqa: E402

NO_CS = ["--env", "B:COOP_TEST_NO_CHARACTER_SELECT=1"]
TESTS = ["test_hair_link.py"]
ALL = ["test_hair_own_and_puppet", "test_hair_unset_is_game", "test_hair_under_mask",
       "test_hair_without_character_select", "test_hair_adult"]


class HairLink(unittest.TestCase):
    """Without the no-CS switch its no-CS test is deselected; with it all five run to their shots and end NO-REF (no
    stored reference, and the colour thresholds are not measured: never PASS). A hair whose lines are said but whose
    texture is not repainted is a FAIL."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_fake_bridge_refuses_hash_like_the_real_one(self):
        # hair-link-4217f901 16:48: 'menu look hair=#RRGGBB' was refused on both lane games (coop_playtest_core.cpp
        # ParseRequest "act": no '#', ';' or control character) while the fakes took it. The fake refuses it now too.
        import fakegame
        g = fakegame.FakeGame("A", os.path.join(self.tmp, "game"), 1)
        for bad in ("menu look hair=#FF0000", "menu look hair=FF0000; kill X", "menu\nlook"):
            r = g.builtin_answer({"id": 1, "cmd": "act", "lines": bad})
            self.assertFalse(r["ok"], bad)
            self.assertEqual(r["error"], "bad_request")
        self.assertTrue(g.builtin_answer({"id": 2, "cmd": "act", "lines": "menu look hair=FF0000"})["ok"])

    def run_it(self, name, env_args, env=None):
        out = os.path.join(self.tmp, name)
        p = pytest_run(TESTS + ["--fake", "--no-rerun", "--out", out, "-q", "-s", "-p", "no:cacheprovider"] + env_args,
                       env=env)
        return summary(out), p

    def test_runs_and_ends_no_ref(self):
        s, p = self.run_it("on", NO_CS)
        self.assertEqual(results(s), {t: "NO-REF" for t in ALL}, p.stdout[-3000:])
        self.assertIn("the hair colour thresholds are not measured yet", p.stdout)
        self.assertNotIn("low:", p.stdout)   # (the fake's hair blocks: every share holds)
        # own+puppet 2 steps x 2 games, unset-again 2, mask 2, no-CS 2 x 2, adult 2
        # (no ^: pytest's progress letters may stand before a printed line)
        self.assertEqual(len(re.findall(r"huecheck [AB] [^\n]*changed=\d+", p.stdout)), 14, p.stdout[-3000:])
        self.assertNotIn("put back:", p.stdout)   # (the adult test's way back to child worked)
        s, p = self.run_it("off", [])
        self.assertEqual([d["test"] for d in s["deselected"]], ["test_hair_link.py::test_hair_without_character_select",
                                                                "test_hair_link.py::test_hair_solo"], p.stdout[-2000:])

    def test_solo_deselected_until_the_harness_has_it(self):
        """No solo.py (harness-c133 item 7) yet: test_hair_solo is deselected on every run, never run, never SETUP-FAIL."""
        import importlib.util
        if importlib.util.find_spec("solo") is not None:
            self.skipTest("solo.py is in the harness: its own selftests cover the solo boot")
        s, _ = self.run_it("solo", NO_CS)
        self.assertIn("test_hair_link.py::test_hair_solo", [d["test"] for d in s["deselected"]])
        self.assertNotIn("test_hair_solo", results(s))

    def test_hair_not_repainted_fails(self):
        """The row's bug (the hair keeps the game's colour though the module says it painted it) is a FAIL."""
        s, p = self.run_it("norepaint", NO_CS, env={"BT_FAKE_HAIR": "norepaint"})
        r = results(s)
        for t in ("test_hair_own_and_puppet", "test_hair_under_mask", "test_hair_without_character_select",
                  "test_hair_adult"):
            self.assertEqual(r[t], "FAIL", (t, p.stdout[-3000:]))
        self.assertIn("the hair was not repainted", p.stdout)


if __name__ == "__main__":
    unittest.main()
