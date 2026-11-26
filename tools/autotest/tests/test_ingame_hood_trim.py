"""Selftest of ingame/test_hood_trim.py + ingame/fakes/hood_trim.py (row 278 C4-4; owner port-sweep-c131-2), run by
test_ingame.py's load_tests: each pass deselected without its A switch and NO-REF with it (no references; the trim values
not measured); FAIL when the hoods lose the trim in pass ON, take it in pass OFF, or B's puppet of A shows no trim."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

OFF = "A:LICHII_COOP_LOOK=cap_trim:yellow"
ON = "A:LICHII_COOP_LOOK=cap_trim:yellow,cap_trim_hoods:on"
ROW = ["--env", "LICHII_COOP_DEBUG_ANIM=1", "--env", "LICHII_COOP_CHEATS=1"]


class HoodTrim(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, look, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_HOOD_BUG"] = bug
        args = [".", "--fake", "--out", out] + (["--env", look] if look else []) + ROW + [
            "-q", "-p", "no:cacheprovider", "-k", "hood_trim"]
        p = T.pytest_run(args, env=env)
        return p, T.summary(out)

    def test_without_a_switch_both_deselected(self):
        p, s = self.run_fake("none", None)
        self.assertEqual(T.results(s), {}, p.stdout[-1500:])
        names = [d["test"] for d in s.get("deselected", [])]
        for t in ("test_hood_trim_off", "test_hood_trim_on"):
            self.assertTrue(any(n.endswith(t) for n in names), names)

    def test_pass_off_no_ref(self):
        p, s = self.run_fake("off", OFF)
        self.assertEqual(T.results(s), {"test_hood_trim_off": "NO-REF"}, p.stdout[-1500:])

    def test_pass_on_no_ref(self):
        p, s = self.run_fake("on", ON)
        self.assertEqual(T.results(s), {"test_hood_trim_on": "NO-REF"}, p.stdout[-1500:])

    def test_hoods_take_the_trim_when_off_fails(self):
        p, s = self.run_fake("always", OFF, bug="always")
        self.assertEqual(T.results(s), {"test_hood_trim_off": "FAIL"}, p.stdout[-1500:])
        self.assertIn("takes the Cap trim with look_cap_trim_hoods off", s["tests"][0]["why"])

    def test_hoods_lose_the_trim_when_on_fails(self):
        p, s = self.run_fake("lost", ON, bug="lost")
        self.assertEqual(T.results(s), {"test_hood_trim_on": "FAIL"}, p.stdout[-1500:])
        self.assertIn("has no Cap trim with look_cap_trim_hoods on", s["tests"][0]["why"])

    def test_puppet_without_trim_fails(self):
        p, s = self.run_fake("puppet", ON, bug="puppet")
        self.assertEqual(T.results(s), {"test_hood_trim_on": "FAIL"}, p.stdout[-1500:])
        self.assertIn("B's puppet of A shows no Cap trim", s["tests"][0]["why"])


if __name__ == "__main__":
    unittest.main()
