"""Row 307's selftest (ingame/test_puppet_run.py + ingame/fakes/puppet_run.py), run by test_ingame.py's load_tests:
NO-REF on the fake (look shots have no references; the VARS_FOLLOW measure run is UNPROVEN on top), FAIL when B's probe gives no line, A never makes Malon's puppet
or A's Malon puppet holds 0 while B runs (before the fix), deselected without the probe switch."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

NAME = "test_puppet_run_swap"
ENV = ["--env", "LICHII_COOP_AS_VARS_PROBE=1", "--env", "LICHII_COOP_DEBUG_POSE=1"]


class PuppetRun(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, bug=None, env=ENV):
        out = os.path.join(self.tmp, name)
        e = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            e["BT_FAKE_PUPPET_RUN_BUG"] = bug
        p = T.pytest_run([".", "--fake", "--out", out] + env + (["--no-rerun"] if bug else []) +
                         ["-q", "-p", "no:cacheprovider", "-k", "test_puppet_run"], env=e)
        return p, T.summary(out)

    def result(self, s):
        return [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith(NAME)]

    def test_fake_no_ref(self):
        p, s = self.run_fake("ok")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "NO-REF", r)

    def test_no_probe_fails(self):
        p, s = self.run_fake("noprobe", bug="noprobe")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "FAIL", r)
        self.assertIn("probe did not run", r[0][1])

    def test_no_spawn_fails(self):
        p, s = self.run_fake("nospawn", bug="nospawn")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "FAIL", r)
        self.assertIn("made again as malon", r[0][1])

    def test_puppet_not_following_fails(self):
        p, s = self.run_fake("novars", bug="novars")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "FAIL", r)
        self.assertIn("does not follow", r[0][1])

    def test_deselected_without_switch(self):
        p, s = self.run_fake("noenv", env=[])
        self.assertEqual(self.result(s), [], (p.stdout, s))
        self.assertTrue(any(d.get("test", "").endswith(NAME) for d in s.get("deselected", [])), s)


if __name__ == "__main__":
    unittest.main()
