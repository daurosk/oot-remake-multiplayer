"""Row 148's selftest (ingame/test_gear.py + ingame/fakes/gear.py; owner port-sweep-c131), run by test_ingame.py's
load_tests: both rows NO-REF on the fake (look shots, no references), and FAIL (never PASS / NO-REF) under each bug."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

ARGS = ["-q", "-p", "no:cacheprovider", "-k", "test_child_gear_flags_not_drawn or test_adult_gear_shown"]


class Gear(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_GEAR_BUG"] = bug
        p = T.pytest_run([".", "--fake", "--out", out] + ARGS, env=env)
        return p, T.summary(out)

    def test_ok_is_no_ref(self):
        p, s = self.run_fake("ok")
        self.assertEqual(T.results(s), {"test_child_gear_flags_not_drawn": "NO-REF", "test_adult_gear_shown": "NO-REF"},
                         p.stdout[-1500:])

    def test_child_bug_fails(self):
        p, s = self.run_fake("child", bug="child")
        r = T.results(s)
        self.assertEqual(r["test_child_gear_flags_not_drawn"], "FAIL", p.stdout[-1500:])
        self.assertEqual(r["test_adult_gear_shown"], "NO-REF")
        why = [t["why"] for t in s["tests"] if t["test"].endswith("test_child_gear_flags_not_drawn")][0]
        self.assertIn("Armor_Adult_Gauntlet_Gold", why)

    def test_rewarp_loss_fails(self):
        p, s = self.run_fake("rewarp", bug="rewarp")
        r = T.results(s)
        self.assertEqual(r["test_adult_gear_shown"], "FAIL", p.stdout[-1500:])
        self.assertEqual(r["test_child_gear_flags_not_drawn"], "NO-REF")

    def test_boot_left_adult_fails(self):
        """review-rows-c130 (1): the way back must leave both games child, else the test FAILs (never PASS / NO-REF)."""
        p, s = self.run_fake("stuck", bug="stuck")
        r = T.results(s)
        self.assertEqual(r["test_adult_gear_shown"], "FAIL", p.stdout[-1500:])
        why = [t["why"] for t in s["tests"] if t["test"].endswith("test_adult_gear_shown")][0]
        self.assertIn("stays adult", why)


if __name__ == "__main__":
    unittest.main()
