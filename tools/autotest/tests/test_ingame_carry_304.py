"""Rows 304-306's probe selftest (ingame/test_carry_304.py + ingame/fakes/carry_304.py; owner carry-sync-304), run by
test_ingame.py's load_tests: the set-up (place X Y Z, face X Z, lift) holds on the fake; B's copy in A's hands is
NO-REF (hands box not measured, never PASS); a copy beyond MISS_M is NO-REF (not measured); a copy left at its rest
place or missing on B is FAIL; Ruto never loaded in her room is SETUP-FAIL; deselected without the row's switch."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

ROW = ["--env", "LICHII_COOP_DEBUG_OBJECTS=1"]
ALL = ("test_carry_cucco", "test_carry_rock", "test_carry_ruto")


class Carry304(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, mode, row=True):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs"), "BT_FAKE_CARRY_304": mode}
        p = T.pytest_run(["test_carry_304.py", "--fake", "--out", out, "--no-rerun"] + (ROW if row else []) + [
            "-q", "-p", "no:cacheprovider"], env=env)
        return p, T.summary(out)

    def why(self, s, test):
        return [t["why"] for t in s["tests"] if t["test"].endswith(test)][0]

    def test_in_hands_is_no_ref(self):
        p, s = self.run_fake("ok", "ok")
        self.assertEqual(T.results(s), {t: "NO-REF" for t in ALL}, p.stdout[-2000:])
        for t in ALL:
            self.assertIn("hands box not measured yet: B's copy in A's hands 15 of 15", self.why(s, t))

    def test_left_at_rest_fails(self):
        p, s = self.run_fake("rest", "rest")
        self.assertEqual(T.results(s), {t: "FAIL" for t in ALL}, p.stdout[-2000:])
        for t in ALL:
            self.assertIn("still shows it at its rest place", self.why(s, t))

    def test_no_copy_fails(self):
        p, s = self.run_fake("absent", "absent")
        self.assertEqual(T.results(s)["test_carry_ruto"], "FAIL", p.stdout[-2000:])
        self.assertIn("B has NO copy in 15 of 15", self.why(s, "test_carry_ruto"))

    def test_ruto_not_loaded_is_setup_fail(self):
        p, s = self.run_fake("unloaded", "unloaded")
        self.assertEqual(T.results(s)["test_carry_ruto"], "SETUP-FAIL", p.stdout[-2000:])
        self.assertIn("of her banc place", self.why(s, "test_carry_ruto"))

    def test_far_copy_is_no_ref(self):
        p, s = self.run_fake("far", "far")
        self.assertEqual(T.results(s), {t: "NO-REF" for t in ALL}, p.stdout[-2000:])
        for t in ALL:
            self.assertIn("(MISS_M, not measured) in 15 of 15", self.why(s, t))

    def test_without_the_switch_deselected(self):
        p, s = self.run_fake("none", "ok", row=False)
        self.assertEqual(T.results(s), {}, p.stdout[-1500:])
        names = [d["test"] for d in s.get("deselected", [])]
        for t in ALL:
            self.assertTrue(any(n.endswith(t) for n in names), names)


if __name__ == "__main__":
    unittest.main()
