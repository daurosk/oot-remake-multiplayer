"""Selftests of row 234 by position (ingame/test_volvagia_rocks.py, fakes/volvagia_rocks.py) on the fake games, no game.
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
NAME = "test_volvagia_rocks_fall_at_each_link"


class VolvagiaRocks(unittest.TestCase):
    """Each passes on a good fake game and FAILS (or SETUP-FAILs) when the thing it guards is broken."""
    OBJ = ["--env", "LICHII_COOP_DEBUG_OBJECTS=1"]

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_file(self, extra, env=None):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        p = pytest_run(["test_volvagia_rocks.py", "--fake", "--no-rerun", "--out", out, "-q"] + extra, env=env)
        return p, results(summary(out))

    def test_rocks_at_each_link_is_no_ref_never_pass(self):
        """Good game: the positions agree, but the judge values are not measured yet (review-rows-c130 10:32 (1)) and
        the shots have no owner-approved reference: NO-REF (never PASS)."""
        p, r = self.run_file(self.OBJ)
        self.assertEqual(r, {NAME: "NO-REF"}, p.stdout[-3000:])
        self.assertIn("judge values not measured yet", p.stdout, p.stdout[-3000:])

    def test_one_target_for_both_games_fails(self):
        p, r = self.run_file(self.OBJ, env={"BT_FAKE_BROKEN": "vr-one-target"})
        self.assertEqual(r, {NAME: "FAIL"}, p.stdout[-3000:])
        self.assertIn("B: one target: only 0 of", p.stdout, p.stdout[-3000:])

    def test_no_falling_rock_is_no_ref_never_pass(self):
        p, r = self.run_file(self.OBJ, env={"BT_FAKE_BROKEN": "vr-asleep"})
        self.assertEqual(r, {NAME: "NO-REF"}, p.stdout[-3000:])
        self.assertIn("no falling rock seen in A and B", p.stdout, p.stdout[-3000:])

    def test_place_off_the_floor_is_setup_fail(self):
        p, r = self.run_file(self.OBJ, env={"BT_FAKE_BROKEN": "vr-lava"})
        self.assertEqual(r, {NAME: "SETUP-FAIL"}, p.stdout[-3000:])
        self.assertIn("stand apart", p.stdout, p.stdout[-3000:])

    def test_not_ready_sample_after_place_is_stepped_not_failed(self):
        """B not ready for 90 frames after its place (past the 60-frame settle): stepped until in play, never judged
        from one read (B #139 one-read class; review-rows-c130-2 16:31): the run goes on to the same NO-REF."""
        p, r = self.run_file(self.OBJ, env={"BT_FAKE_BROKEN": "vr-busy"})
        self.assertEqual(r, {NAME: "NO-REF"}, p.stdout[-3000:])
        self.assertNotIn("stand apart", p.stdout, p.stdout[-3000:])

    def test_budget_is_built_from_its_parts(self):
        """The budget holds the arrival ceiling + set-ups + ONE rocks wait + the sampling cap (a637fe404's cause: a fixed
        budget that a lane-2 arrival outran)."""
        sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ingame"))
        import pair as P                    # noqa: E402
        import test_boss_respawn as BR      # noqa: E402
        import test_volvagia_rocks as VR    # noqa: E402
        parts = BR.BOSS_ARRIVAL_S + 2 * P.SETUP_LIMIT_S + VR.ROCKS_FRAMES / P.FPS + VR.SAMPLE_S
        self.assertGreaterEqual(VR.BUDGET_S, parts)
        self.assertLess(VR.BUDGET_S, parts + 2)
        marks = {m.name: m.args for m in VR.test_volvagia_rocks_fall_at_each_link.pytestmark}
        self.assertEqual(marks["budget"], (VR.BUDGET_S,))

    def test_default_run_deselects_it(self):
        p, r = self.run_file([])
        self.assertEqual(r, {}, p.stdout[-3000:])


if __name__ == "__main__":
    unittest.main()
