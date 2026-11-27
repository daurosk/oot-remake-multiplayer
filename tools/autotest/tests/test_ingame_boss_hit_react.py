"""Selftests of row 138 (2)(7) (ingame/test_boss_hit_react.py, fakes/boss_hit_react.py) on the fake games, no game.
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
NAME = "test_copy_hit_that_took_life_leaves_the_reaction_to_the_owner"


class BossHitReact(unittest.TestCase):
    """Each passes on a good fake game and FAILS (or is NO-REF / SETUP-FAIL) when the thing it guards is broken."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_file(self, env=None, refs=None):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        e = dict(env or {})
        e["BT_FAKE_REFS"] = refs or os.path.join(self.tmp, "norefs")
        p = pytest_run(["test_boss_hit_react.py", "--fake", "--no-rerun", "--out", out, "-q"], env=e)
        return p, results(summary(out)), out

    def test_good_game_is_no_ref_then_pass_with_refs(self):
        """The default run runs it (no needs_env); with no stored reference NO-REF (never PASS), with the approved
        shots stored: PASS. The judge's notes show the life fall and K."""
        p, r, out = self.run_file()
        self.assertEqual(r, {NAME: "NO-REF"}, p.stdout[-3000:])
        self.assertIn(T.MISSING_REF, str(summary(out)["tests"][0]["why"]), p.stdout[-3000:])
        refs = os.path.join(self.tmp, "refs")
        os.makedirs(refs)
        new = os.path.join(out, "lane1", "new")
        names = sorted(os.listdir(new))
        self.assertEqual(len(names), 8, names)        # four per game (test_ingame Suite: <= 8 shots, both games)
        for f in names:
            shutil.copyfile(os.path.join(new, f), os.path.join(refs, f))
        p, r, out = self.run_file(refs=refs)
        self.assertEqual(r, {NAME: "PASS"}, p.stdout[-3000:])
        notes = " ".join(summary(out)["tests"][0].get("notes") or [])
        self.assertIn("K = 4 hits", notes, p.stdout[-3000:])
        # the fake's stun ends before slash 5: a hit that took NO life runs the copy's own reaction, allowed (no FAIL)
        self.assertIn("slash 5: B's Enemy_DungeonBoss_Dodongo life 16 -> 16; own reaction True", notes, notes)

    def test_own_reaction_for_a_life_hit_fails(self):
        """The 0.11.4 bug (each slash that took life ran the copy's own brain) FAILS, also with no stored refs."""
        p, r, _ = self.run_file(env={"BT_FAKE_BROKEN": "bhr-ownreact"})
        self.assertEqual(r, {NAME: "FAIL"}, p.stdout[-3000:])
        self.assertIn("took life but ran the copy's own reaction", p.stdout, p.stdout[-3000:])

    def test_no_life_taken_is_void_never_fail(self):
        """No stun, so every slash takes no life: their own-reaction lines are allowed (a hit with no life), and the row
        is not proven: NO-REF naming the missing took-life line (never PASS, never FAIL)."""
        p, r, _ = self.run_file(env={"BT_FAKE_BROKEN": "bhr-nolife"})
        self.assertEqual(r, {NAME: "NO-REF"}, p.stdout[-3000:])
        self.assertIn("never logged", p.stdout, p.stdout[-3000:])

    def test_no_copy_sync_is_setup_fail(self):
        p, r, _ = self.run_file(env={"BT_FAKE_BROKEN": "bhr-nosync"})
        self.assertEqual(r, {NAME: "SETUP-FAIL"}, p.stdout[-3000:])
        self.assertIn("boss sync: B has no", p.stdout, p.stdout[-3000:])

    def test_counts_at_zero_is_never_pass(self):
        """The took-life line came but no counts line has K > 0: never PASS. On the fake the 1800 frames take less than
        the line's 10 s of wall clock, so it is NO-REF naming that (a game at 30 fps runs them in 60 s: FAIL)."""
        p, r, _ = self.run_file(env={"BT_FAKE_BROKEN": "bhr-nocount"})
        self.assertEqual(r, {NAME: "NO-REF"}, p.stdout[-3000:])
        self.assertIn("no counts line with K > 0", p.stdout, p.stdout[-3000:])

    def test_budget_is_built_from_its_parts(self):
        sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ingame"))
        import pair as P                    # noqa: E402
        import test_boss_respawn as BR      # noqa: E402
        import test_boss_hit_react as HR    # noqa: E402
        parts = BR.BOSS_ARRIVAL_S + 2 * P.SETUP_LIMIT_S + 2 * HR.FRAMES / P.FPS + 60.0
        self.assertGreaterEqual(HR.BUDGET_S, parts)
        self.assertLess(HR.BUDGET_S, parts + 2)
        self.assertEqual(HR.FRAMES, 240 + 1800 + 236 + 614 + 90 + 5 * 48 + 16 * 60 + 600 + 196 + 1800)
        marks = {m.name: m.args for m in HR.test_copy_hit_that_took_life_leaves_the_reaction_to_the_owner.pytestmark}
        self.assertEqual(marks["budget"], (HR.BUDGET_S,))
        self.assertNotIn("needs_env", marks)      # native boss sync is on by default (module docstring)

    def test_scenario_frames_match_the_test(self):
        """The ported waits are the scenarios' own (one source: the scenario files)."""
        scen = os.path.join(os.path.dirname(HERE), "scenarios")
        with open(os.path.join(scen, "hs-boss-hit-react-dodongo.A.txt"), encoding="utf-8") as f:
            a = f.read()
        with open(os.path.join(scen, "hs-boss-hit-react-dodongo.B.txt"), encoding="utf-8") as f:
            b = f.read()
        self.assertIn("wait 1800               # the intro", a)
        self.assertIn("press ZL 10\nwait 30\npress ZR\nwait 120\nhold ZL+LSTICK_UP 60\npress A\nmark a-bomb1", a)
        self.assertIn("mark w16\nwait 600\npress ZR", a)
        self.assertIn("wait 2350", b)
        self.assertIn("hold ZL+LSTICK_UP 90", b)
        self.assertEqual(b.count("hold ZL+Y 8"), 5)
        self.assertEqual(a.count("wait 1500"), 0)
        self.assertIn("wait 1500\nmark title", b)    # B's title wait: 300 frames longer than A's 1200
        self.assertIn("wait 1200\nmark title", a)


if __name__ == "__main__":
    unittest.main()
