"""Selftests of chartab-0121's bridge test (tools/autotest/ingame/test_chartab.py) on the fake games
(ingame/fakes/menu_chartab.py). Run by test_ingame.py's load_tests (CI) or alone."""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_ingame as T   # noqa: E402

TEST = "test_customize_keys_show"


class CustomizeKeys(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_k(self, env=None):
        out = os.path.join(self.tmp, "o")
        T.pytest_run([".", "--fake", "--out", out, "-q", "-k", TEST, "--no-rerun"], env=env)
        s = T.summary(out)
        return s, {r["test"].split("::")[-1]: r for r in s["tests"]}[TEST]

    def test_keys_shown_wants_the_shots(self):
        s, r = self.run_k()
        self.assertEqual(r["result"], "NO-REF", s)   # (every check passed; the shots wait for a person)
        self.assertEqual({sh["game"] for sh in r["shots"]}, {"A", "B"}, r["shots"])

    def test_fails_when_the_title_keeps_its_words(self):
        s, r = self.run_k(env={"BT_FAKE_CHARTAB": "words"})
        self.assertEqual(r["result"], "FAIL", s)
        self.assertIn("the title kept its words", r["why"])

    def test_fails_when_the_keys_draw_nothing(self):
        # (the 0.12.0 bug: the screen says its keys are shown, the footer has none)
        s, r = self.run_k(env={"BT_FAKE_CHARTAB": "blank"})
        self.assertEqual(r["result"], "FAIL", s)
        self.assertIn("nothing drawn", r["why"])


if __name__ == "__main__":
    unittest.main()
