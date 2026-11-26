"""Selftests of tools/autotest/ingame/test_party_hud.py (party-hud-crash-c126) on two fake games (ingame/fakes/party_hud.py);
loaded by test_ingame.py's load_tests."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_ingame import pytest_run, results, summary   # noqa: E402

ENV = ["--env", "COOP_TEST_PARTY_HUD=1", "--env", "B:COOP_TEST_PARTY_HUD_NO_PACK=1"]
TESTS = ("test_party_hud_after_boot", "test_party_hud_title_and_load_again")


class PartyHud(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_suite(self, extra=(), env=None):
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider", "--no-rerun", "-k", "party_hud"]
                       + list(extra), env=env)
        return p, summary(out)

    def test_with_its_switches_no_fail(self):
        """A: the HUD on after the boot and after title -> load; B (no pack): hidden, never on. Only the look checks are
        left open (NO-REF: no stored reference yet), never a FAIL or SETUP-FAIL."""
        p, s = self.run_suite(ENV)
        got = results(s)
        self.assertEqual(sorted(got), sorted(TESTS), p.stdout[-2000:])
        for t in TESTS:
            self.assertIn(got[t], ("PASS", "NO-REF"), p.stdout[-2000:])
        shots = [sh for r in s["tests"] for sh in r["shots"]]
        self.assertEqual(len(shots), 4)   # (both tests, both games)

    def test_without_its_switches_deselected(self):
        p, s = self.run_suite()
        self.assertEqual(results(s), {}, p.stdout[-2000:])
        self.assertEqual(sorted(d["test"].split("::")[-1] for d in s["deselected"]), sorted(TESTS), s["deselected"])

    def test_a_crash_in_the_boot_is_fail(self):
        """B logs the game's crash report in the boot's load: both tests FAIL (not SETUP-FAIL), the crash named."""
        p, s = self.run_suite(ENV, env={"BT_FAKE_PARTY_CRASH": "B"})
        self.assertEqual(results(s), {t: "FAIL" for t in TESTS}, p.stdout[-2000:])
        self.assertIn("B crashed: ", p.stdout)
        self.assertNotEqual(p.returncode, 0)


if __name__ == "__main__":
    unittest.main()
