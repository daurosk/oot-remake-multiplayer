"""Row 285's selftest (ingame/test_levitate.py + ingame/fakes/levitate.py; owner port-sweep-c131-8), run by
test_ingame.py's load_tests: NO-REF on the fake (look shots, no references), FAIL when A never holds B's puppet,
SETUP-FAIL when B's character flag does not take, NO-REF (VOID) when the chest gives no get scene."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

NAME = "test_levitate_impa_chest"


class Levitate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_LEVITATE_BUG"] = bug
        p = T.pytest_run([".", "--fake", "--out", out] + (["--no-rerun"] if bug else []) +
                         ["-q", "-p", "no:cacheprovider", "-k", "test_levitate"], env=env)
        return p, T.summary(out)

    def result(self, s):
        return [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith(NAME)]

    def test_fake_no_ref(self):
        p, s = self.run_fake("ok")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "NO-REF", r)
        self.assertIn("no stored reference", r[0][1])

    def test_no_hold_fails(self):
        p, s = self.run_fake("nohold", bug="nohold")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "FAIL", r)
        self.assertIn("their cutscene", r[0][1])

    def test_no_character_is_setup_fail(self):
        p, s = self.run_fake("nochar", bug="nochar")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "SETUP-FAIL", r)
        self.assertIn("CharacterMenu_SelectedCharacter", r[0][1])

    def test_no_pack_anims_is_setup_fail(self):
        p, s = self.run_fake("noanims", bug="noanims")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "SETUP-FAIL", r)
        self.assertIn("lists no Character Select AnimationParam for its Child puppets", r[0][1])

    def test_opened_chest_is_no_ref(self):
        p, s = self.run_fake("opened", bug="opened")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "NO-REF", r)
        self.assertIn("started no event", r[0][1])
