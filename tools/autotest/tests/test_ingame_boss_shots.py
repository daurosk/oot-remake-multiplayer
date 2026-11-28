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

# -- fight_ready (twinrova-222-8, B #161; review-tests-153-3: the first ZL came after fixed frame steps with no play-UI
# proof, and since 7f23bf2d1 an arrive_in_event warp no longer waits for the play UI). On the fake lane (the same as
# test_ingame.WarpPlayUi): the fight starts only once both games' play UI is up again after the warp; a play UI that
# never comes up, or no play UI line at all, is a SETUP-FAIL naming the game, never a pass.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ingame"))
from unittest import mock           # noqa: E402
import fakegame                     # noqa: E402
import pair as P                    # noqa: E402
import test_boss_shots as BS        # noqa: E402

SCENE = "KakarikoVillage_Child"     # (a scene the fake warps to: test_ingame.WarpPlayUi)


class FightReady(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def lane(self, **env):
        with mock.patch.dict(os.environ, env):
            lane = fakegame.FakeLane(self.tmp)
        self.addCleanup(lane.close)
        pair = P.Pair(lane.games(), out_dir=self.tmp)
        self.addCleanup(pair.close)
        pair.begin(120)
        return pair

    def warp_raw(self, pair):
        # (the bridge's own warp verb, no Pair.warp_one play UI wait: what an arrive_in_event warp leaves since 7f23bf2d1)
        for x in ("A", "B"):
            pair.call(x, "warp", scene=SCENE)
        self.assertEqual(pair.play_ui("A"), "down")
        self.assertEqual(pair.play_ui("B"), "down")

    def test_waits_until_the_play_ui_is_up_on_both(self):
        pair = self.lane(BT_FAKE_UI_LATE="150")
        self.warp_raw(pair)
        before = pair.c["A"].last_frame or 0
        BS.fight_ready(pair, SCENE)
        self.assertEqual(pair.play_ui("A"), "up")
        self.assertEqual(pair.play_ui("B"), "up")
        self.assertGreaterEqual((pair.c["A"].last_frame or 0) - before, 140)   # (stepped, not judged from one read)

    def test_a_play_ui_never_up_is_setup_fail_naming_the_game(self):
        pair = self.lane(BT_FAKE_UI_NEVER="B")
        self.warp_raw(pair)
        with self.assertRaisesRegex(P.SetupFail, r"^%s: the boss intro not over %d frames .*B: " % (
                SCENE, BS.FIGHT_UI_FRAMES)):
            BS.fight_ready(pair, SCENE)

    def test_no_play_ui_line_is_setup_fail(self):
        pair = self.lane()
        self.assertIsNone(pair.play_ui("A"))           # (the fake's boot says no play UI line)
        with self.assertRaisesRegex(P.SetupFail, r"^%s: A: no 'the game's play UI is up' line" % SCENE):
            BS.fight_ready(pair, SCENE)

    def test_the_first_zl_comes_after_fight_ready_in_both_tests(self):
        # (source order: each test's first ZL press/hold after its fight_ready call, review-tests-153-3)
        with open(BS.__file__, encoding="utf-8") as f:
            src = f.read()
        for name, stage in (("def test_twinrova_shots", "TwinrovaStage"), ("def test_phantom_ganon_shots",
                                                                             "PhantomGanonStage")):
            body = src[src.index(name):]
            body = body[:body.find("\ndef ", 1) if "\ndef " in body[1:] else len(body)]
            ready = body.index('fight_ready(pair, "%s")' % stage)
            self.assertLess(ready, body.index('["ZL"]'), name)



if __name__ == "__main__":
    unittest.main()
