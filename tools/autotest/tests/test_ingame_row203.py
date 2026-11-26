"""Selftests of row 203's bridge test (tools/autotest/ingame/test_small_keys.py) on the fake games
(ingame/fakes/row203.py). Run by test_ingame.py's load_tests (CI) or alone. Collects test_small_keys.py only (never the
whole folder: party-hud-test-c126's selftest root cause)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

ARGS = ["test_small_keys.py", "--fake", "-q", "--no-rerun"]


class Row203SmallKeyShared(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_it(self, env=None):
        out = os.path.join(self.tmp, "o")
        T.pytest_run(ARGS + ["--out", out], env=env or {})
        s = T.summary(out)
        self.assertEqual(s["collected"] if "collected" in s else len(s["tests"]), 1, s)
        return s

    def test_passes_when_the_count_and_flag_are_shared(self):
        s = self.run_it()
        self.assertEqual(s["counts"]["PASS"], 1, s)
        self.assertTrue(any("after use A 0 B 0" in n for n in s["tests"][0]["notes"]), s["tests"][0])

    def test_fails_when_the_key_never_reaches_the_other_game(self):
        s = self.run_it({"BT_FAKE_KEYS": "lost"})
        self.assertEqual(s["counts"]["FAIL"], 1, s)
        self.assertIn("A took a key", s["tests"][0]["why"])

    def test_fails_when_the_key_lands_twice(self):
        s = self.run_it({"BT_FAKE_KEYS": "double"})
        self.assertEqual(s["counts"]["FAIL"], 1, s)


if __name__ == "__main__":
    unittest.main()
