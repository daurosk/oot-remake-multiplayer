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
    """The vest's colour classes in a Link's box (solid_count) and the unset check on the wear's own pixels
    (left_after_unset, leather's unset row)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def count(self, fill, rgb, where=(300, 200, 360, 280)):
        """A 'Link' box with blond hair (hue 40, S 0.69: the box's warm scene) and a block of `fill` at `where`."""
        from PIL import Image, ImageDraw
        im = Image.new("RGB", (640, 360), (90, 90, 90))   # (grey ground: a green one is in the green class)
        d = ImageDraw.Draw(im)
        d.rectangle((300, 60, 340, 120), fill=(0xD0, 0xA0, 0x40))
        d.rectangle(where, fill=fill)
        path = os.path.join(self.tmp, "c.png")
        im.save(path)
        return huecheck.solid_count(path, (280, 40, 380, 300), rgb)

    def test_vest_classes(self):
        """The vest's classes (huecheck.CLASSES, measured on gate-look 309d7180c): a solid yellow / green vest in the
        box counts; the warm scene, the tunics' slot colours and the white shirt do not; outside the box never."""
        self.assertGreater(self.count((0xFF, 0xD0, 0x00), 0xFFD000), 1000)
        self.assertGreater(self.count((0x40, 0xC8, 0x30), 0x20C020), 1000)   # (the game's lighter green: hue 115)
        self.assertEqual(self.count((0x30, 0x30, 0x30), 0xFFD000), 0)        # (the blond hair alone is not yellow)
        for fill in ((0x30, 0x70, 0xFF), (0xFF, 0x30, 0x30), (0xC0, 0xC0, 0xB8)):
            self.assertEqual(self.count(fill, 0xFFD000), 0, fill)
            self.assertEqual(self.count(fill, 0x20C020), 0, fill)
        self.assertEqual(self.count((0xFF, 0xD0, 0x00), 0xFFD000, where=(450, 200, 510, 280)), 0)
        self.assertAlmostEqual(huecheck.hue_of(0xFF0000), 0.0)
        self.assertAlmostEqual(huecheck.hue_of(0x0000FF), 240.0)

    def unset_shots(self, hair_before, hair_coloured, hair_after, flame):
        """'Link' with a hair block, and a small torch flame dark before and lit while coloured and after (the worst
        flicker for an unset check): B #148, the gate-look 309d7180c Kakariko torches."""
        from PIL import Image, ImageDraw
        out = []
        for i, (hair, lit) in enumerate(((hair_before, False), (hair_coloured, True), (hair_after, True))):
            im = Image.new("RGB", (640, 360), (30, 80, 30))
            d = ImageDraw.Draw(im)
            d.rectangle((300, 60, 340, 200), fill=(230, 220, 200))
            d.rectangle((300, 40, 360, 100), fill=hair)
            d.rectangle((20, 80, 31, 99), fill=flame if lit else (40, 30, 20))
            out.append(os.path.join(self.tmp, "unset-%d.png" % i))
            im.save(out[-1])
        return out

    def test_left_after_unset(self):
        """Only the wear's own pixels are judged after an unset: a lit torch flame in the wear's hue is not 'colour
        left' (the whole-frame changed_hue counted it), and a wear still on is."""
        blond, red, flame = (0xE0, 0xC0, 0x60), (0xDD, 0x45, 0x27), (0xE8, 0x50, 0x20)
        before, coloured, after = self.unset_shots(blond, red, blond, flame)
        h = huecheck.left_after_unset(before, coloured, after, 0xDD4527)
        self.assertGreater(h["wear"], 20, h)
        self.assertLess(h["share"], huecheck.LEFT_SHARE, h)
        h = huecheck.left_after_unset(*self.unset_shots(blond, red, red, flame), 0xDD4527)
        self.assertGreaterEqual(h["share"], huecheck.LEFT_SHARE, h)
        # never repainted: only the flame changed, so the 'wear' is the flame itself, which stays lit: never a pass
        h = huecheck.left_after_unset(*self.unset_shots(blond, blond, blond, flame), 0xDD4527)
        self.assertTrue(h["wear"] < 20 or h["share"] >= huecheck.LEFT_SHARE, h)
        h = huecheck.left_after_unset(*self.unset_shots(blond, blond, blond, (40, 30, 20)), 0xDD4527)
        self.assertLess(h["wear"], 20, h)


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
        # (judged()'s own "; low: " in the NO-REF reason: a bare "low:" also matched the vest's traceback
        # line "took the yellow: the vest", CI 45930967885)
        self.assertNotRegex(p.stdout, r"NO-REF: [^\n]*; low: ")   # (the fake's vest blocks: every share holds)
        # gained: own+puppet 2 steps, unset 1, no-CS 2, each x 2 games; left: own+puppet, unset, no-CS 1 each x 2
        # (no ^: pytest's progress letters may stand before a printed line)
        self.assertEqual(len(re.findall(r"huecheck [AB] [^\n]*before=\d+ after=\d+ gain=[1-9]\d*", p.stdout)), 10,
                         p.stdout[-3000:])
        self.assertEqual(len(re.findall(r"huecheck [AB] [^\n]*left=-?\d+ of [1-9]\d* share=", p.stdout)), 6,
                         p.stdout[-3000:])
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

    def test_vest_unset_judged_on_the_vest_only(self):
        """B #148: a flickering yellow torch flame is not 'yellow left' (gate-look 309d7180c false FAIL), and a vest
        whose unset line is said but whose yellow stays drawn is a FAIL."""
        s, p = self.run_it("flame", NO_CS, env={"BT_FAKE_FLAME": "F0C000"})
        self.assertEqual(results(s)["test_vest_unset_is_tunic"], "NO-REF", p.stdout[-3000:])
        s, p = self.run_it("nounset", NO_CS, env={"BT_FAKE_VEST": "nounset"})
        self.assertEqual(results(s)["test_vest_unset_is_tunic"], "FAIL", p.stdout[-3000:])
        self.assertIn("yellow left on the vest after unset", p.stdout)


if __name__ == "__main__":
    unittest.main()
