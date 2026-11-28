"""Selftest of ingame/test_tunic.py + ingame/fakes/tunic.py (rows #202 + 198; owner port-sweep-c131(-2)), run by
test_ingame.py's load_tests: deselected without A's colour switch, NO-REF with it (no references; the colour values not
measured), FAIL when the row 198 cross guard fires or B draws A's puppet green or the lane file's parts are worn
(gate-tunic-309d7180), SETUP-FAIL with both Links on one spot. A's colour alone (the switch of the first port) is not
enough: the test also needs both games' LICHII_COOP_LOOK=cap_trim: (no parts) and B's green."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

ARGS = ["-q", "-p", "no:cacheprovider", "-k", "test_own_colour_no_cross_guard"]
NAME = "test_own_colour_no_cross_guard"
ENV = ["--env", "A:LICHII_COOP_COLOR=blue", "--env", "B:LICHII_COOP_COLOR=green", "--env", "LICHII_COOP_LOOK=cap_trim:"]


class Tunic(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, env_on=True, bug=None, extra=None, switches=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        env.update(extra or {})
        if bug:
            env["BT_FAKE_TUNIC_BUG"] = bug
        args = [".", "--fake", "--out", out] + ((ENV if switches is None else switches) if env_on else []) + ARGS
        p = T.pytest_run(args, env=env)
        return p, T.summary(out)

    def test_without_the_switch_it_is_deselected(self):
        p, s = self.run_fake("off", env_on=False)
        self.assertNotIn(NAME, T.results(s), p.stdout[-1500:])
        self.assertTrue(any(d["test"].endswith(NAME) for d in s.get("deselected", [])), s.get("deselected"))

    def test_blue_alone_is_deselected(self):
        """The first port's switch alone: the lane file's look would be worn (gate-tunic-309d7180), so not run."""
        p, s = self.run_fake("blue-only", switches=["--env", "A:LICHII_COOP_COLOR=blue"])
        self.assertNotIn(NAME, T.results(s), p.stdout[-1500:])
        lack = [d for d in s.get("deselected", []) if d["test"].endswith(NAME)]
        self.assertTrue(lack, s.get("deselected"))
        self.assertEqual(sorted(lack[0]["needs_env"]), ["B:LICHII_COOP_COLOR=green", "LICHII_COOP_LOOK=cap_trim:"])

    def test_lane_parts_worn_fails(self):
        """gate-tunic-309d7180: the module wore the lane file's Cap trim (no LICHII_COOP_LOOK line): a FAIL that names
        it, not a wait for a tunic line that never comes."""
        p, s = self.run_fake("leftover", bug="leftover")
        self.assertEqual(T.results(s), {NAME: "FAIL"}, p.stdout[-1500:])
        self.assertIn("the module did not take LICHII_COOP_LOOK", s["tests"][0]["why"])

    def test_with_the_switch_no_ref(self):
        p, s = self.run_fake("on")
        self.assertEqual(T.results(s), {NAME: "NO-REF"}, p.stdout[-1500:])

    def test_cross_guard_fails(self):
        p, s = self.run_fake("cross", bug="cross")
        self.assertEqual(T.results(s), {NAME: "FAIL"}, p.stdout[-1500:])
        self.assertIn("row 198 cross guard", s["tests"][0]["why"])

    def test_puppet_drawn_green_fails(self):
        """review-rows-c130 10:38: A's puppet green on B's shot (same brightness kind of fault) is a FAIL by colour."""
        p, s = self.run_fake("green", bug="green")
        self.assertEqual(T.results(s), {NAME: "FAIL"}, p.stdout[-1500:])
        self.assertIn("B's shot: the puppet is not blue", s["tests"][0]["why"])

    def test_links_on_one_line_setup_fail(self):
        p, s = self.run_fake("line", extra={"BT_FAKE_TUNIC_ON_LINE": "1"})
        self.assertEqual(T.results(s), {NAME: "SETUP-FAIL"}, p.stdout[-1500:])


if __name__ == "__main__":
    unittest.main()
