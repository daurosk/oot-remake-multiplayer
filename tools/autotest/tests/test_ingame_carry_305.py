"""Row 305's probe selftest (ingame/test_carry_boulder_305.py + ingame/fakes/carry_305.py; owner carry-boulder-305),
run by test_ingame.py's load_tests: adult set-up + LiftGrade + place/face + a slow lift hold on the fake; B's copy in
A's hands is NO-REF (hands box not measured, never PASS); a copy left at rest is FAIL; no lift is SETUP-FAIL;
deselected without the row's switch."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

ROW = ["--env", "LICHII_COOP_DEBUG_OBJECTS=1"]
TEST = "test_carry_boulder"


class Carry305(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, mode, row=True):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs"), "BT_FAKE_CARRY_305": mode}
        p = T.pytest_run(["test_carry_boulder_305.py", "--fake", "--out", out, "--no-rerun"] + (ROW if row else []) + [
            "-q", "-p", "no:cacheprovider"], env=env)
        return p, T.summary(out)

    def why(self, s):
        return [t["why"] for t in s["tests"] if t["test"].endswith(TEST)][0]

    def test_in_hands_is_no_ref(self):
        p, s = self.run_fake("ok", "ok")
        self.assertEqual(T.results(s), {TEST: "NO-REF"}, p.stdout[-2000:])
        self.assertIn("hands box not measured yet: B's copy in A's hands 15 of 15", self.why(s))

    def test_left_at_rest_fails(self):
        p, s = self.run_fake("rest", "rest")
        self.assertEqual(T.results(s), {TEST: "FAIL"}, p.stdout[-2000:])
        self.assertIn("still shows it at its rest place", self.why(s))

    def test_no_lift_is_setup_fail(self):
        p, s = self.run_fake("nolift", "nolift")
        self.assertEqual(T.results(s), {TEST: "SETUP-FAIL"}, p.stdout[-2000:])
        self.assertIn("A did not lift the Obj_LiftRock_B_01", self.why(s))

    def test_without_the_switch_deselected(self):
        p, s = self.run_fake("none", "ok", row=False)
        self.assertEqual(T.results(s), {}, p.stdout[-1500:])
        names = [d["test"] for d in s.get("deselected", [])]
        self.assertTrue(any(n.endswith(TEST) for n in names), names)


if __name__ == "__main__":
    unittest.main()
