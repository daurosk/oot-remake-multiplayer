"""Selftests of test_vest_link.py (vest-link, report 293 P2) on the fake games (ingame/fakes/vest_link.py) and of its
colour check (ingame/huecheck.py); run by tests/test_ingame.py's load_tests."""
import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ingame"))
import huecheck   # noqa: E402
from test_ingame import pytest_run, results, summary   # noqa: E402

NO_CS = ["--env", "B:COOP_TEST_NO_CHARACTER_SELECT=1"]
TESTS = ["test_vest_link.py"]


class HueCheck(unittest.TestCase):
    """The changed pixels and their share in a colour's hue: a recolour that did not happen, went to another hue, or
    went grey is caught."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def shot(self, name, vest, sway=0):
        from PIL import Image, ImageDraw
        im = Image.new("RGB", (640, 360), (30, 80, 30))
        d = ImageDraw.Draw(im)
        d.rectangle((300 + sway, 60, 340 + sway, 200), fill=(230, 220, 200))   # "Link", swaying a pixel
        d.rectangle((300, 200, 360, 280), fill=vest)                            # "the vest"
        path = os.path.join(self.tmp, name + ".png")
        im.save(path)
        return path

    def test_changed_hue(self):
        blue, yellow, red, grey = (0x30, 0x70, 0xFF), (0xFF, 0xD0, 0x00), (0xFF, 0x30, 0x30), (0x80, 0x80, 0x80)
        before = self.shot("before", blue)
        h = huecheck.changed_hue(before, self.shot("yellow", yellow, sway=1), 0xFFD000)
        self.assertGreater(h["changed"], 20, h)
        self.assertGreaterEqual(h["share"], 0.5, h)
        self.assertEqual(huecheck.changed_hue(before, self.shot("same", blue, sway=1), 0xFFD000)["coloured"], 0)
        self.assertLess(huecheck.changed_hue(before, self.shot("red", red), 0xFFD000)["share"], 0.5)
        self.assertLess(huecheck.changed_hue(before, self.shot("grey", grey), 0xFFD000)["share"], 0.5)
        self.assertAlmostEqual(huecheck.hue_of(0xFF0000), 0.0)
        self.assertAlmostEqual(huecheck.hue_of(0x0000FF), 240.0)


class VestLink(unittest.TestCase):
    """Without the no-CS switch its no-CS test is deselected; with it all three run to their shots and end NO-REF (no
    stored reference, and the colour thresholds are not measured: never PASS). A vest whose lines are said but whose
    texture is not repainted is a FAIL."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_it(self, name, env_args, env=None):
        out = os.path.join(self.tmp, name)
        p = pytest_run(TESTS + ["--fake", "--no-rerun", "--out", out, "-q", "-s", "-p", "no:cacheprovider"] + env_args,
                       env=env)
        return summary(out), p

    def test_runs_and_ends_no_ref(self):
        s, p = self.run_it("on", NO_CS)
        self.assertEqual(results(s), {"test_vest_own_and_puppet": "NO-REF", "test_vest_unset_is_tunic": "NO-REF",
                                      "test_vest_without_character_select": "NO-REF"}, p.stdout[-3000:])
        self.assertIn("the vest colour thresholds are not measured yet", p.stdout)
        self.assertNotIn("low:", p.stdout)   # (the fake's vest blocks: every share holds)
        # own+puppet 2 steps x 2 games, unset-again 2, no-CS 2 x 2
        # (no ^: pytest's progress letters may stand before a printed line)
        self.assertEqual(len(re.findall(r"huecheck [AB] [^\n]*changed=\d+", p.stdout)), 10, p.stdout[-3000:])
        s, p = self.run_it("off", [])
        self.assertEqual([d["test"] for d in s["deselected"]], ["test_vest_link.py::test_vest_without_character_select"],
                         p.stdout[-2000:])

    def test_vest_not_repainted_fails(self):
        """The row's bug (the vest keeps the tunic's colour though the module says it painted it) is a FAIL."""
        s, p = self.run_it("norepaint", NO_CS, env={"BT_FAKE_VEST": "norepaint"})
        r = results(s)
        self.assertEqual(r["test_vest_own_and_puppet"], "FAIL", p.stdout[-3000:])
        self.assertEqual(r["test_vest_without_character_select"], "FAIL", p.stdout[-3000:])
        self.assertIn("the vest was not repainted", p.stdout)


if __name__ == "__main__":
    unittest.main()
