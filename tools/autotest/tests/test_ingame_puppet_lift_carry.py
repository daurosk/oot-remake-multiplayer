"""Selftests of native puppets N5 lift carry (ingame/test_puppet_lift_carry.py, fakes/lift_carry.py) on the fake games,
no game. Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_ingame as T   # noqa: E402

pytest_run, summary = T.pytest_run, T.summary
NAME = "test_puppet_lift_carry"


class PuppetLiftCarryFake(unittest.TestCase):
    """NO-REF on a good fake (MEASURED is None) with the ride's values in its notes; each bug is caught: an uncarried
    copy -> FAIL, no carry line -> FAIL, a lift that never moves -> SETUP-FAIL, a small offset -> NO-REF listing it."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, bug=None):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "norefs")}
        if bug:
            env["BT_FAKE_LIFT_BUG"] = bug
        p = pytest_run(["test_puppet_lift_carry.py", "--fake", "--no-rerun", "--out", out, "-q",
                        "-p", "no:cacheprovider"], env=env)
        s = summary(out)
        r = [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith(NAME)]
        self.assertEqual(len(r), 1, (p.stdout[-2000:], s))
        return r[0][0], r[0][1], json.dumps(s)

    def test_fake_no_ref_with_values(self):
        res, why, s = self.run_fake()
        self.assertEqual(res, "NO-REF", why)
        self.assertIn("the N5 lift carry thresholds are not measured yet", why)
        self.assertNotIn("misses", why)
        self.assertRegex(s, r"ride: \d+ samples \(0 lost\), \d+ still, \d+ moving, speed 2\.00 m/s, rest offset 0\.000 m, "
                            r"max deviation 0\.000 m, lift \d+\.\d\d -> 75\.00 m")

    def test_uncarried_copy_fails(self):
        res, why, _ = self.run_fake("nocarry")
        self.assertEqual(res, "FAIL", why)
        self.assertIn("lift carry: on our lift", why)

    def test_no_line_fails(self):
        res, why, _ = self.run_fake("noline")
        self.assertEqual(res, "FAIL", why)
        self.assertIn("the carry never took the copy", why)

    def test_lift_stuck_is_setup_fail(self):
        res, why, _ = self.run_fake("stuck")
        self.assertEqual(res, "SETUP-FAIL", why)
        self.assertIn("the lift did not move", why)

    def test_small_offset_is_no_ref_listed(self):
        res, why, _ = self.run_fake("wobble")
        self.assertEqual(res, "NO-REF", why)
        self.assertIn("max deviation 0.080 m > 0.05", why)


if __name__ == "__main__":
    unittest.main()
