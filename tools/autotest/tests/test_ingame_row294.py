"""Selftests of row 294's bridge tests (tools/autotest/ingame/test_epona_294.py) on the fake games
(ingame/fakes/epona294.py). Run by test_ingame.py's load_tests (CI) or alone. Collects test_epona_294.py only."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

ARGS = ["test_epona_294.py", "--fake", "-q", "--no-rerun",
        "--env", "LICHII_COOP_PUPPET=1", "--env", "LICHII_COOP_DEBUG_ANIM=1", "--env", "LICHII_COOP_OBJECTS=1",
        "--env", "LICHII_COOP_OCARINA_PROBE=1"]   # test 3's needs_env


class Row294EponaWithHorse(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_it(self, env=None):
        out = os.path.join(self.tmp, "o")
        T.pytest_run(ARGS + ["--out", out], env=env or {})
        s = T.summary(out)
        self.assertEqual(s["collected"] if "collected" in s else len(s["tests"]), 3, s)
        return s

    def by_name(self, s, part):
        return [t for t in s["tests"] if part in (t.get("test") or "")][0]

    def test_passes_when_the_end_comes_with_the_horse_and_a_bare_save_is_mended(self):
        s = self.run_it()
        self.assertEqual(s["counts"]["PASS"], 2, s)
        # test 3: the song brought her on each game; no stored reference in the fake run's refs: NO-REF, never PASS
        self.assertEqual(s["counts"].get("NO-REF"), 1, s)
        self.assertEqual(len(self.by_name(s, "song_calls_her").get("shots") or []), 8, s)

    def test_fails_when_complete_comes_without_the_horse(self):
        s = self.run_it({"BT_FAKE_EPONA": "nohorse"})
        self.assertEqual(s["counts"]["FAIL"], 1, s)
        self.assertIn("without the horse", self.by_name(s, "reaches_the_other")["why"])

    def test_fails_when_a_bare_complete_save_is_not_mended(self):
        s = self.run_it({"BT_FAKE_EPONA": "nomend"})
        self.assertEqual(s["counts"]["FAIL"], 1, s)
        self.assertIn("mended", self.by_name(s, "is_mended")["why"])

    def test_fails_when_the_song_brings_no_epona(self):
        s = self.run_it({"BT_FAKE_EPONA": "nosong"})
        self.assertEqual(s["counts"]["FAIL"], 1, s)
        self.assertIn("did not come", self.by_name(s, "song_calls_her")["why"])


if __name__ == "__main__":
    unittest.main()
