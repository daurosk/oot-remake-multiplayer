"""Selftest of ingame/test_regress_ui.py + ingame/fakes/regress_ui.py (regress vs 0.11.12 steps 5 + 6; owner
regress-port-a), run by test_ingame.py's load_tests: each test deselected without its switch, NO-REF with it (no
references yet; the dialog text not measured), FAIL under each fake bug (no native tags, no question, B's copy not
painted)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

TAGS, TITLE, HILT = "test_name_tags", "test_title_screen_dialog", "test_sword_handle"
ENV = {TITLE: "LICHII_COOP_TEST_STATE=1", HILT: "A:LICHII_COOP_LOOK=kokiri_hilt:blue"}


class RegressUi(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, test, env_on=True, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_REGRESS_BUG"] = bug
        args = [".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider", "-k", test]
        if env_on and test in ENV:
            args += ["--env", ENV[test]]
        p = T.pytest_run(args, env=env)
        return p, T.summary(out)

    def test_tags_no_ref(self):
        p, s = self.run_fake("tags", TAGS)
        self.assertEqual(T.results(s), {TAGS: "NO-REF"}, p.stdout[-1500:])

    def test_tags_drawn_fails(self):
        p, s = self.run_fake("tags-bug", TAGS, bug="tags")
        self.assertEqual(T.results(s), {TAGS: "FAIL"}, p.stdout[-1500:])
        self.assertIn("no memory for the names", s["tests"][0]["why"])

    def test_tags_b_camera_in_front_is_setup_fail(self):
        """c272735f8 (C #145): B's camera on A's side (B's shots of its own Link's face, no A, no tag): never NO-REF or
        PASS; the set-up's per-game view check names B."""
        p, s = self.run_fake("tags-cam", TAGS, bug="camera")
        self.assertEqual(T.results(s), {TAGS: "SETUP-FAIL"}, p.stdout[-1500:])
        self.assertIn("B's camera not behind its Link facing A", s["tests"][0]["why"])

    def test_title_deselected_without_test_state(self):
        p, s = self.run_fake("title-off", TITLE, env_on=False)
        self.assertNotIn(TITLE, T.results(s), p.stdout[-1500:])
        self.assertTrue(any(d["test"].endswith(TITLE) for d in s.get("deselected", [])), s.get("deselected"))

    def test_title_no_ref(self):
        p, s = self.run_fake("title", TITLE)
        self.assertEqual(T.results(s), {TITLE: "NO-REF"}, p.stdout[-1500:])
        # DIALOG_MEASURED cites gate-title-182cd53b: the text no longer holds it back, only the missing references do
        self.assertNotIn("DIALOG_MEASURED is not set", s["tests"][0]["why"])
        self.assertIn("no stored reference for regress-sys-tab", s["tests"][0]["why"])

    def test_title_no_question_not_pass(self):
        p, s = self.run_fake("title-bug", TITLE, bug="title")
        self.assertEqual(T.results(s), {TITLE: "NO-REF"}, p.stdout[-1500:])
        self.assertIn("no SystemWindow_00/T_Text_00 text", s["tests"][0]["why"])

    def test_hilt_deselected_without_switch(self):
        p, s = self.run_fake("hilt-off", HILT, env_on=False)
        self.assertNotIn(HILT, T.results(s), p.stdout[-1500:])

    def test_hilt_no_ref(self):
        p, s = self.run_fake("hilt", HILT)
        self.assertEqual(T.results(s), {HILT: "NO-REF"}, p.stdout[-1500:])

    def test_hilt_not_painted_fails(self):
        p, s = self.run_fake("hilt-bug", HILT, bug="hilt")
        self.assertEqual(T.results(s), {HILT: "FAIL"}, p.stdout[-1500:])
        self.assertIn("not painted", s["tests"][0]["why"])


if __name__ == "__main__":
    unittest.main()
