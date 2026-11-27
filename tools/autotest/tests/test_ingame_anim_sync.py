"""Row 310's selftest (ingame/test_anim_sync.py + ingame/fakes/anim_sync.py; owner anim-sync-310-2), run by
test_ingame.py's load_tests: NO-REF on the fake (look shots, no references), FAIL when B sends no effect or A never
starts one, SETUP-FAIL when the puppet has no effect user or B wears no sword, NO-REF when no end was sent, and
deselected without the test's switches."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

NAME = "test_spin_charge_effects_reach_the_puppet"
ENV = ["--env", "B:LICHII_COOP_DEBUG_SOUND=1", "--env", "A:LICHII_COOP_DEBUG_ANIM=1"]


class AnimSync(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, bug=None, env=ENV):
        out = os.path.join(self.tmp, name)
        e = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            e["BT_FAKE_ANIM_SYNC_BUG"] = bug
        p = T.pytest_run([".", "--fake", "--out", out] + env + (["--no-rerun"] if bug else []) +
                         ["-q", "-p", "no:cacheprovider", "-k", "test_anim_sync"], env=e)
        return p, T.summary(out)

    def result(self, s):
        return [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith(NAME)]

    def one(self, name, bug=None):
        p, s = self.run_fake(name, bug)
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        return r[0]

    def test_fake_no_ref(self):
        r = self.one("ok")
        self.assertEqual(r[0], "NO-REF", r)
        self.assertIn("no stored reference", r[1])

    def test_nothing_sent_fails(self):
        r = self.one("nosend", "nosend")
        self.assertEqual(r[0], "FAIL", r)
        self.assertIn("B sent no LINK_EFFECT start", r[1])

    def test_a_key_not_started_fails(self):
        r = self.one("nostart", "nostart")
        self.assertEqual(r[0], "FAIL", r)
        self.assertIn("A never started ['Charge_RotationCut']", r[1])

    def test_no_effect_user_is_setup_fail(self):
        r = self.one("nouser", "nouser")
        self.assertEqual(r[0], "SETUP-FAIL", r)
        self.assertIn("has no ELinkRef", r[1])

    def test_no_sword_is_setup_fail(self):
        r = self.one("nosword", "nosword")
        self.assertEqual(r[0], "SETUP-FAIL", r)
        self.assertIn("B wears no sword", r[1])

    def test_pack_without_effects_is_setup_fail(self):
        r = self.one("nopack", "nopack")
        self.assertEqual(r[0], "SETUP-FAIL", r)
        self.assertIn("built without row 310's ELinkRef", r[1])

    def test_no_end_is_no_ref(self):
        r = self.one("noend", "noend")
        self.assertEqual(r[0], "NO-REF", r)
        self.assertIn("no end sent", r[1])

    def test_deselected_without_switches(self):
        p, s = self.run_fake("plain", env=[])
        self.assertEqual(self.result(s), [], (p.stdout, s))
