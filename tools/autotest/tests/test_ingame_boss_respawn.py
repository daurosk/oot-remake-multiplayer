"""Selftests of rows 289 + 234 (ingame/test_boss_respawn.py, fakes/boss_respawn.py) on the fake games, no game.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import ast
import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_ingame as T   # noqa: E402

pytest_run, summary, results, INGAME = T.pytest_run, T.summary, T.results, T.INGAME


class BossRespawn(unittest.TestCase):
    """Rows 289 + 234 (ingame/test_boss_respawn.py) on the fake games: each passes on a good game and FAILS when the
    thing it guards is broken (fakes/boss_respawn.py BT_FAKE_BROKEN; the keep switch = the old behaviour)."""
    OBJ = ["--env", "LICHII_COOP_DEBUG_OBJECTS=1"]

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_file(self, extra, env=None):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        p = pytest_run(["test_boss_respawn.py", "--fake", "--no-rerun", "--out", out, "-q"] + extra, env=env)
        return p, results(summary(out))

    def test_fixed_path_passes_on_every_stage(self):
        p, r = self.run_file(self.OBJ + ["-k", "shows"])
        self.assertEqual([r[k] for k in sorted(r)], ["PASS"] * 4, p.stdout[-2000:])

    def test_black_screen_fails_the_fixed_path(self):
        p, r = self.run_file(self.OBJ + ["-k", "shows"], env={"BT_FAKE_BROKEN": "starter"})
        self.assertEqual(set(r.values()), {"FAIL"}, p.stdout[-2000:])
        self.assertIn("not the stage", p.stdout)

    def test_keep_switch_shows_the_cause(self):
        p, r = self.run_file(self.OBJ + ["--env", "LICHII_COOP_ARRIVAL_STARTER=keep", "-k", "kept or shows"])
        self.assertEqual(r["test_respawn_black_with_starter_kept"], "PASS", p.stdout[-2000:])
        self.assertEqual({v for k, v in r.items() if "shows" in k}, {"SKIP"}, p.stdout[-2000:])

    def test_rocks_on_one_side_only_fails(self):
        p, r = self.run_file(self.OBJ + ["-k", "rocks"], env={"BT_FAKE_BROKEN": "rocks-B"})
        self.assertEqual(r["test_volvagia_rocks_each_game"], "FAIL", p.stdout[-2000:])
        p, r = self.run_file(self.OBJ + ["-k", "rocks"])
        self.assertEqual(r["test_volvagia_rocks_each_game"], "NO-REF", p.stdout[-2000:])   # (no refs yet: never PASS)

    def test_bottles_are_the_modules(self):
        """test_boss_respawn's nofairy list is hookshot.BOTTLE_ITEMS (one list), and that == the module's kBottleItems
        (coop_testinput.cpp NOFAIRY)."""
        src = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), "lichii", "hook", "coop_testinput.cpp")
        with open(src, encoding="utf-8") as f:
            m = re.search(r"kBottleItems\[\]\s*=\s*\{([^}]*)\}", f.read())
        self.assertIsNotNone(m, "no kBottleItems in %s" % src)
        sys.path.insert(0, os.path.dirname(HERE))
        import hookshot
        self.assertEqual(re.findall(r'"(0x[0-9a-f]+)"', m.group(1)), list(hookshot.BOTTLE_ITEMS))
        with open(os.path.join(INGAME, "test_boss_respawn.py"), encoding="utf-8") as f:
            tree = ast.parse(f.read())       # (read, not imported: its pytest marks belong to the in-game run)
        uses = [ast.unparse(n.value) for n in tree.body if isinstance(n, ast.Assign)
                and any(getattr(t, "id", None) == "BOTTLES" for t in n.targets)]
        self.assertEqual(uses, ["hookshot.BOTTLE_ITEMS"], "test_boss_respawn.BOTTLES must be hookshot's list, not a copy")

    def test_no_arrival_is_setup_fail_naming_the_step(self):
        """B #130 09:43: the boss-stage arrival is its own bounded step after the 30 s set-up check; a miss there is
        SETUP-FAIL 'boss arrival in <S>' (never FAIL: the row's own check did not run)."""
        p, r = self.run_file(self.OBJ, env={"BT_FAKE_BROKEN": "no-arrival"})
        self.assertEqual(set(r.values()), {"SETUP-FAIL"}, p.stdout[-2000:])
        for stage in ("VolvagiaStage", "DodongoStage", "LizalfosStage", "GanonStage"):
            self.assertIn("boss arrival in %s" % stage, p.stdout, p.stdout[-2000:])
        # (boss-respawn-bridge-e3e6e253: a bare 'TimeoutError: timed out' named no phase; now the phase and its time)
        self.assertIn("boss arrival in VolvagiaStage: in 'warp A' after", p.stdout, p.stdout[-2000:])

    def test_beside_respawn_is_no_ref_never_pass(self):
        """The old row's 'UNPROVEN ... not taken': A took the beside move, so the row 289 path never ran (pair.unproven)."""
        p, r = self.run_file(self.OBJ + ["-k", "shows"], env={"BT_FAKE_BROKEN": "beside"})
        self.assertEqual(set(r.values()), {"NO-REF"}, p.stdout[-2000:])
        self.assertIn("the row 289 path was not taken", p.stdout)

    def test_default_run_deselects_them(self):
        p, r = self.run_file([])
        self.assertEqual(r, {}, p.stdout[-2000:])
        self.assertEqual(p.stdout.count("deselected: needs_env LICHII_COOP_DEBUG_OBJECTS=1 "), 6, p.stdout[-2000:])


if __name__ == "__main__":
    unittest.main()
