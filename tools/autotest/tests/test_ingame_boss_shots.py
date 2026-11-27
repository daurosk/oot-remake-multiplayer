"""Selftests of rows 222/286 (ingame/test_boss_shots.py + ingame/fakes/boss_shots.py), no game: fake bridges.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results


class BossShots(unittest.TestCase):
    """Rows 222/286 (ingame/test_boss_shots.py): PASS on the fake pair with the rows' switches; FAIL when the copy makes
    none of the owner's shots, or counts them made with no bullet (fakes/boss_shots.py BT_FAKE_BOSS_ONE_SIDED); deselected
    without the switches."""
    ENV = ["--env", "LICHII_COOP_DEBUG_BOSS=1", "--env", "LICHII_COOP_DEBUG_OBJECTS=1", "--env", "LICHII_COOP_CHEATS=1"]

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_bosses(self, tag, env=None, more=()):
        out = os.path.join(self.tmp, tag)
        p = pytest_run(["test_boss_shots.py", "--fake", "--no-rerun", "--out", out, "-q"] + list(more), env=env)
        return p, summary(out)

    def test_no_ref_then_pass_with_refs(self):
        """First run: the log judge holds, the shots are NO-REF candidates (never PASS); a reviewer adopts them into
        refs/ (here: all) and the next run PASSes, comparing every paired shot + 'shot end' on each game."""
        p, s = self.run_bosses("first", env={"BT_FAKE_REFS": os.path.join(self.tmp, "refs")}, more=self.ENV)
        self.assertEqual(results(s), {"test_twinrova_shots": "NO-REF", "test_phantom_ganon_shots": "NO-REF"},
                         p.stdout[-2000:])
        new = os.path.join(self.tmp, "first", "lane1", "new")
        self.assertEqual(len(os.listdir(new)), 2 * (10 + 24 + 1) + 2 * (24 + 1))
        os.makedirs(os.path.join(self.tmp, "refs"))
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(self.tmp, "refs", f))
        p, s = self.run_bosses("ok", env={"BT_FAKE_REFS": os.path.join(self.tmp, "refs")}, more=self.ENV)
        self.assertEqual(results(s), {"test_twinrova_shots": "PASS", "test_phantom_ganon_shots": "PASS"}, p.stdout[-2000:])
        shots = [sh for r in s["tests"] for sh in r["shots"]]
        self.assertEqual(len(shots), 2 * 35 + 2 * 25)
        self.assertTrue(all(sh["ok"] for sh in shots))

    def test_fail_when_the_copy_makes_none(self):
        p, s = self.run_bosses("none", env={"BT_FAKE_BOSS_ONE_SIDED": "1"}, more=self.ENV)
        self.assertEqual(set(results(s).values()), {"FAIL"}, p.stdout[-2000:])
        self.assertIn("UNPROVEN: no owner's shot sent and made in a copy", p.stdout)

    def test_fail_when_a_bullet_is_in_one_game_only(self):
        p, s = self.run_bosses("bullets", env={"BT_FAKE_BOSS_ONE_SIDED": "bullets"}, more=self.ENV)
        self.assertEqual(set(results(s).values()), {"FAIL"}, p.stdout[-2000:])
        self.assertIn("made in one game only", p.stdout)

    def test_deselected_without_the_switches(self):
        p, s = self.run_bosses("none-env")
        self.assertEqual(s["tests"], [], p.stdout[-2000:])
        self.assertEqual(sorted(d["test"] for d in s["deselected"]),
                         ["test_boss_shots.py::test_phantom_ganon_shots", "test_boss_shots.py::test_twinrova_shots"])

    def test_no_warp_inside_the_set_up(self):
        """A two-game warp (26.9-31.0 s, pair.WARP_PAIR_MEASURED_S) cannot fit the 30 s set-up (tb-rows-f9f3b9c2-2:
        SETUP-FAIL in the boss warp): no warp call may sit inside a 'with pair.setup()' block of the row file."""
        import ast
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ingame", "test_boss_shots.py")
        tree = ast.parse(open(path, encoding="utf-8").read())
        bad = []
        for node in ast.walk(tree):
            if not isinstance(node, ast.With):
                continue
            if not any(isinstance(i.context_expr, ast.Call) and getattr(i.context_expr.func, "attr", "") == "setup"
                       for i in node.items):
                continue
            for sub in ast.walk(node):
                if isinstance(sub, ast.Call):
                    name = getattr(sub.func, "attr", None) or getattr(sub.func, "id", "")
                    if name in ("warp_in", "warp_both", "warp_one"):
                        bad.append("%s at line %d" % (name, sub.lineno))
        self.assertEqual(bad, [])


if __name__ == "__main__":
    unittest.main()
