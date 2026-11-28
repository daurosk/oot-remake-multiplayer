"""Selftests of row 299's phase 0 probe (ingame/test_pause_world.py, fakes/pause_world.py) on the fake games, no game.
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
NAME = "test_pause_world_probe"


class PauseWorldProbe(unittest.TestCase):
    """PASS on a good fake game with every probe reading in its notes; FAIL naming the hold when the menu takes none."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_file(self, env=None):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        e = dict(env or {})
        e["BT_FAKE_REFS"] = os.path.join(self.tmp, "norefs")
        p = pytest_run(["test_pause_world.py", "--fake", "--no-rerun", "--out", out, "-q"], env=e)
        return p, results(summary(out)), out

    def test_good_game_passes_with_the_readings(self):
        """The probe only measures: PASS, and its notes carry the hold the + menu took (the fake's index 1, key
        0x5a5a, count 1), the module's release line, the scene of each reading and the holds back after the menu shut."""
        p, r, out = self.run_file()
        self.assertEqual(r, {NAME: "PASS"}, p.stdout[-3000:])
        notes = " ".join(summary(out)["tests"][0].get("notes") or [])
        self.assertIn('probe: holds the + menu took: {"1": ["0x5a5a", 1]}', notes, notes)
        self.assertRegex(notes, r"probe: release 1: .*coop: pause world: test release 1 key 0x5a5a: count 1 -> 0")
        self.assertIn('"label": "+ menu open, its holds let go"', notes, notes)
        self.assertIn('"scene": ', notes, notes)
        self.assertIn("probe: holds after the menu shut equal free play's: True", notes, notes)

    def test_menu_without_a_hold_fails(self):
        """The + menu taking no PauseMgr hold FAILS naming it (never PASS: there is nothing to let go)."""
        p, r, _ = self.run_file(env={"BT_FAKE_PAUSE_WORLD": "nohold"})
        self.assertEqual(r, {NAME: "FAIL"}, p.stdout[-3000:])
        self.assertIn("took no PauseMgr hold", p.stdout, p.stdout[-3000:])


if __name__ == "__main__":
    unittest.main()
