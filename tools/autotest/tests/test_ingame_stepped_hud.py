"""Selftest of ingame/test_stepped_hud.py + ingame/fakes/stepped_hud.py (hud-fresh-0121's in-game proof), run by
test_ingame.py's load_tests: NO-REF on the fake (no reference shot yet), FAIL when a slow step shows nothing (the module
before hud-fresh-0121, BT_FAKE_STEPPED_BUG=1)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

TEST = "test_stepped_hud_and_tags"


class SteppedHud(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, bug=False, env_more=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_STEPPED_BUG"] = "1"
        env.update(env_more or {})
        p = T.pytest_run([".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider", "-k", TEST], env=env)
        return p, T.summary(out)

    def test_no_ref(self):
        p, s = self.run_fake("ok")
        self.assertEqual(T.results(s), {TEST: "NO-REF"}, p.stdout[-1500:])

    def test_before_the_fix_fails(self):
        p, s = self.run_fake("bug", bug=True)
        self.assertEqual(T.results(s), {TEST: "FAIL"}, p.stdout[-1500:])
        self.assertIn("showed no block", s["tests"][0]["why"])

    def test_tag_part_never_opened_fails(self):
        """nametags-300-2 (stepped-hud-c889af41-2 on 182cd53b5: B in A's shot with no tag, name_tags shown 1): a module
        without row 300's fix (the tag "shown", its balloon part never opened, the screen root hidden) must FAIL."""
        p, s = self.run_fake("closed", env_more={"BT_FAKE_NAMETAGS300": "closed"})
        self.assertEqual(T.results(s), {TEST: "FAIL"}, p.stdout[-1500:])
        self.assertIn("name_tags open and the screen root shown", s["tests"][0]["why"])
