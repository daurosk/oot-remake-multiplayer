"""Selftests of row 316's Navi probe (ingame/test_probe_navi_316.py, fakes/navi_316.py) on the fake games, no game.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_ingame as T   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results
NAME = "test_probe_navi_316"
ENV = ["--env", "BT_PROBE_NAVI=1", "--env", "LICHII_COOP_CHEATS=1"]


class NaviProbe(unittest.TestCase):
    """NO-REF (a probe, never PASS) with each phase's slot names in its notes; SETUP-FAIL when `navi` stays null."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_file(self, env=None):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        e = dict(env or {})
        e["BT_FAKE_REFS"] = os.path.join(self.tmp, "norefs")
        p = pytest_run(["test_probe_navi_316.py", "--fake", "--no-rerun", "--out", out, "-q"] + ENV, env=e)
        return p, results(summary(out)), out

    def test_probe_records_each_phase(self):
        """NO-REF; per game the notes name Status_Red at one heart, LockOn_St with ZL held, Wait idle, no error; the
        JSON files are written."""
        p, r, out = self.run_file()
        self.assertEqual(r, {NAME: "NO-REF"}, p.stdout[-3000:])
        notes = " ".join(summary(out)["tests"][0].get("notes") or [])
        for x in ("A", "B"):
            self.assertIn("%s: slot names" % x, notes, notes)
            found = [f for _, _, fs in os.walk(out) for f in fs if f == "probe-navi-%s.json" % x]
            self.assertEqual(len(found), 1, list(os.walk(out)))
        self.assertIn("'low_life': {0: ['Wait'], 1: ['Status_Red']}", notes, notes)
        self.assertIn("'lock_on': {0: ['LockOn_St']}", notes, notes)
        self.assertIn("'idle': {0: ['Wait']}", notes, notes)
        self.assertNotIn("error: ", notes.replace("error None", ""), notes)

    def test_null_member_is_setup_fail(self):
        """A module without the probe (navi null): SETUP-FAIL naming the member, never a run on nothing."""
        p, r, _ = self.run_file(env={"BT_FAKE_NAVI": "null"})
        self.assertEqual(r, {NAME: "SETUP-FAIL"}, p.stdout[-3000:])
        self.assertIn("navi state member null", p.stdout + str(summary(_)), p.stdout[-3000:])


if __name__ == "__main__":
    unittest.main()
