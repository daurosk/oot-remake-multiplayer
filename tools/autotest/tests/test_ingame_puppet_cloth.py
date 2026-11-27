"""Selftests of tools/autotest/ingame/test_puppet_cloth.py (cloth-puppet-313) on the fake games
(ingame/fakes/puppet_cloth.py); loaded by test_ingame.py's load_tests."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_ingame import pytest_run, results, summary   # noqa: E402

ENV = "LICHII_COOP_DEBUG_CLOTH=1"
TESTS = ("test_cloth_child", "test_cloth_adult")


class PuppetCloth(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.refs = os.path.join(self.tmp, "refs")
        os.makedirs(self.refs)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_(self, name, extra=("--env", ENV), env=None):
        out = os.path.join(self.tmp, name)
        p = pytest_run(["test_puppet_cloth.py", "--fake", "--out", out, "-q", "-p", "no:cacheprovider", "--no-rerun"]
                       + list(extra), env=dict({"BT_FAKE_REFS": self.refs}, **(env or {})))
        return p, summary(out), out

    def test_passes_once_refs_exist(self):
        """Run 1: NO-REF (only the shots' refs missing); with the kept candidates as refs, run 2: PASS."""
        p, s, out = self.run_("c1")
        self.assertEqual(results(s), {t: "NO-REF" for t in TESTS}, p.stdout[-3000:])
        new = os.path.join(out, "lane1", "new")
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(self.refs, f))
        p, s, _ = self.run_("c2")
        self.assertEqual(results(s), {t: "PASS" for t in TESTS}, p.stdout[-3000:])

    def test_no_cloth_fails(self):
        """The bug (a copy's holder 0) FAILs both tests, naming row 313's cause."""
        p, s, _ = self.run_("c3", env={"BT_FAKE_CLOTH": "none"})
        self.assertEqual(results(s), {t: "FAIL" for t in TESTS}, p.stdout[-3000:])
        for t in s["tests"]:
            self.assertIn("holds NO cloth", t["why"])

    def test_needs_the_switch(self):
        """Without --env LICHII_COOP_DEBUG_CLOTH=1 both are deselected, never SETUP-FAIL."""
        p, s, _ = self.run_("c4", extra=())
        self.assertEqual(results(s), {}, p.stdout[-3000:])
        self.assertEqual(len(s.get("deselected") or []), 2, s.get("deselected"))


if __name__ == "__main__":
    unittest.main()
