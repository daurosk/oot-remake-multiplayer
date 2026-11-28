"""Row 305's probe selftest (ingame/test_carry_boulder_305.py + ingame/fakes/carry_305.py; owner carry-boulder-305),
run by test_ingame.py's load_tests: adult set-up + LiftGrade + place/face + a slow lift hold on the fake; B's copy in
A's hands is NO-REF (hands box not measured, never PASS); a copy left at rest is FAIL; no lift is SETUP-FAIL;
a boulder only at a later place is found there; none at any place is SETUP-FAIL naming each; deselected without the
row's switch; LiftGrade is put back on both games after a pass and after a failure."""
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

    def restored(self, s):
        """Both games' LiftGrade put back to the value read before (preflight review-tests-153-2 SHOULD)."""
        notes = [t["notes"] for t in s["tests"] if t["test"].endswith(TEST)][0]
        for x in ("A", "B"):
            self.assertIn("boulder: %s LiftGrade back to 0x0000305a" % x, notes)

    def test_in_hands_is_no_ref(self):
        p, s = self.run_fake("ok", "ok")
        self.assertEqual(T.results(s), {TEST: "NO-REF"}, p.stdout[-2000:])
        self.assertIn("hands box not measured yet: B's copy in A's hands 15 of 15", self.why(s))
        self.restored(s)

    def test_left_at_rest_fails(self):
        p, s = self.run_fake("rest", "rest")
        self.assertEqual(T.results(s), {TEST: "FAIL"}, p.stdout[-2000:])
        self.assertIn("still shows it at its rest place", self.why(s))
        self.restored(s)

    def test_no_lift_is_setup_fail(self):
        p, s = self.run_fake("nolift", "nolift")
        self.assertEqual(T.results(s), {TEST: "SETUP-FAIL"}, p.stdout[-2000:])
        self.assertIn("A did not lift the Obj_LiftRock_B_01", self.why(s))

    def test_boulder_at_a_later_place(self):
        """None in the city (the 04:23 run): the test goes on to GerudoValley_Adult, finds it there and judges."""
        p, s = self.run_fake("later", "later")
        self.assertEqual(T.results(s), {TEST: "NO-REF"}, p.stdout[-2000:])
        notes = [t["notes"] for t in s["tests"] if t["test"].endswith(TEST)][0]
        finds = [n for n in notes if n.startswith("boulder find: ")]
        self.assertTrue(finds[0].startswith("boulder find: GoronCity_Adult/GoronCity_MountainTrailSide:"), finds)
        self.assertIn("Obj_LiftRock_B_: none", finds[0])
        self.assertTrue(finds[-1].startswith("boulder find: GerudoValley_Adult/GerudoValley_Entrance:"), finds)
        self.restored(s)

    def test_no_boulder_anywhere_is_setup_fail(self):
        p, s = self.run_fake("nowhere", "none")
        self.assertEqual(T.results(s), {TEST: "SETUP-FAIL"}, p.stdout[-2000:])
        self.assertIn("no Obj_LiftRock_B_ within 120 m of A's Link at any of 6 places", self.why(s))
        self.assertIn("DesertColossus_FromSpiritTempleRightHand 1", self.why(s))
        notes = [t["notes"] for t in s["tests"] if t["test"].endswith(TEST)][0]
        self.assertEqual(len([n for n in notes if n.startswith("boulder find: ")]), 6, notes)
        self.restored(s)

    def test_without_the_switch_deselected(self):
        p, s = self.run_fake("none", "ok", row=False)
        self.assertEqual(T.results(s), {}, p.stdout[-1500:])
        names = [d["test"] for d in s.get("deselected", [])]
        self.assertTrue(any(n.endswith(TEST) for n in names), names)


if __name__ == "__main__":
    unittest.main()
