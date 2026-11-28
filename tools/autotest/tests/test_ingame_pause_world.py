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


MODE = "test_pause_world_mode"
BOOT = "live-stalfos-weak-room"          # (test_enemy_turn.BOOT, the test's needs_boot)
SWITCHES = ["--env", "LICHII_COOP_DEBUG_OBJECTS=1", "--env", "LICHII_COOP_CHEATS=1"]   # (its needs_env)


class PauseWorldMode(unittest.TestCase):
    """Row 299 phase 1 (fakes/pause_world_mode.py, BT_FAKE_PAUSE_WORLD_MODE): NO-REF on a good game (its shots have no
    reference: never PASS before the owner approves refs) with each reading in its notes; FAIL naming each fault."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_mode(self, mode):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        e = {"BT_FAKE_REFS": os.path.join(self.tmp, "norefs"), "BT_FAKE_PAUSE_WORLD_MODE": mode}
        p = pytest_run(["test_pause_world.py", "--fake", "--no-rerun", "--out", out, "-q", "--boot", BOOT,
                        "-k", MODE] + SWITCHES, env=e)
        return p, results(summary(out)), out

    def test_good_game_no_ref_with_the_readings(self):
        p, r, out = self.run_mode("1")
        self.assertEqual(r, {MODE: "NO-REF"}, p.stdout[-3000:])
        notes = " ".join(summary(out)["tests"][0].get("notes") or [])
        self.assertIn('"label": "option on, + menu open"', notes, notes)
        self.assertIn("phase1: B's status bits of A, option off, + menu: 9", notes, notes)
        self.assertIn("phase1: B's status bits of A, option on, + menu: 0", notes, notes)
        self.assertIn('probe: - menu holds (option off): {"2": ["0x6b6b", 1]}', notes, notes)
        self.assertIn('"name": "World runs in menus"', notes, notes)

    def test_hold_kept_fails(self):
        p, r, _ = self.run_mode("held")
        self.assertEqual(r, {MODE: "FAIL"}, p.stdout[-3000:])
        self.assertIn("the + menu's hold still held", p.stdout, p.stdout[-3000:])

    def test_link_walking_under_the_menu_fails(self):
        p, r, _ = self.run_mode("walks")
        self.assertEqual(r, {MODE: "FAIL"}, p.stdout[-3000:])
        self.assertIn("Link moved under the + menu", p.stdout, p.stdout[-3000:])

    def test_status_still_in_menu_fails(self):
        p, r, _ = self.run_mode("status")
        self.assertEqual(r, {MODE: "FAIL"}, p.stdout[-3000:])
        self.assertIn("neither in a menu nor paused", p.stdout, p.stdout[-3000:])


if __name__ == "__main__":
    unittest.main()
