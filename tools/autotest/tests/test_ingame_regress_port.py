"""Selftests of regress-port-b's tests (ingame/test_regress_bosses.py, ingame/test_enemy_turn.py +
ingame/fakes/regress_port.py), no game: fake bridges. Run by test_ingame.py's load_tests (CI: python
tools/autotest/tests/test_ingame.py)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results

ENV = ["--env", "LICHII_COOP_DEBUG_BOSS=1", "--env", "LICHII_COOP_DEBUG_OBJECTS=1", "--env", "LICHII_COOP_CHEATS=1"]
BOSSES = "test_regress_bosses.py"
ENEMY = "test_enemy_turn.py"
CASES = {   # boot: (file, test, looks per game x 2 games)
    "live-barinade-room": (BOSSES, "test_barinade_native_sync", 3 * 2),
    "live-ganondorf-room": (BOSSES, "test_ganondorf_ball_sent_back", 2 * 2),
    "live-stalfos-weak-room": (ENEMY, "test_stalfos_turns_to_the_other_player", 4 * 2),
}


class RegressPort(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_boot(self, boot, tag, mode="", refs=None, env_args=ENV):
        out = os.path.join(self.tmp, tag)
        env = {"BT_FAKE_REGRESS_BOOT": boot, "BT_FAKE_REGRESS_MODE": mode}
        if refs:
            env["BT_FAKE_REFS"] = refs
        p = pytest_run([CASES[boot][0], "--fake", "--no-rerun", "--out", out, "-q", "--boot", boot] + list(env_args),
                       env=env)
        return p, summary(out)

    def one(self, boot, s):
        r = results(s)
        self.assertIn(CASES[boot][1], r, s)
        return r[CASES[boot][1]]

    def no_ref_then_pass(self, boot):
        refs = os.path.join(self.tmp, "refs-" + boot)
        p, s = self.run_boot(boot, boot + "-first", refs=refs)
        self.assertEqual(self.one(boot, s), "NO-REF", p.stdout[-3000:])
        new = os.path.join(self.tmp, boot + "-first", "lane1", "new")
        self.assertEqual(len(os.listdir(new)), CASES[boot][2], os.listdir(new))
        os.makedirs(refs)
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(refs, f))
        p, s = self.run_boot(boot, boot + "-ok", refs=refs)
        self.assertEqual(self.one(boot, s), "PASS", p.stdout[-3000:])

    # -- step 7: Barinade
    def test_barinade_no_ref_then_pass(self):
        self.no_ref_then_pass("live-barinade-room")

    def test_barinade_setup_fail_without_the_wraps(self):
        p, s = self.run_boot("live-barinade-room", "bar-off", mode="off")
        self.assertEqual(self.one("live-barinade-room", s), "SETUP-FAIL", p.stdout[-3000:])

    def test_barinade_fail_when_the_copy_takes_no_verbs(self):
        p, s = self.run_boot("live-barinade-room", "bar-copy", mode="copy")
        self.assertEqual(self.one("live-barinade-room", s), "FAIL", p.stdout[-3000:])

    # -- step 7: Ganondorf's ball
    def test_ganondorf_no_ref_then_pass(self):
        self.no_ref_then_pass("live-ganondorf-room")

    def test_ganondorf_no_ref_when_no_ball_is_sent_back(self):
        p, s = self.run_boot("live-ganondorf-room", "gan-noreflect", mode="noreflect")
        self.assertEqual(self.one("live-ganondorf-room", s), "NO-REF", p.stdout[-3000:])
        self.assertIn("no ball was sent back", p.stdout + str(s))

    def test_ganondorf_fail_when_the_copy_stays_up(self):
        p, s = self.run_boot("live-ganondorf-room", "gan-nostun", mode="nostun")
        self.assertEqual(self.one("live-ganondorf-room", s), "FAIL", p.stdout[-3000:])

    def test_ganondorf_fail_without_the_rally_line(self):
        p, s = self.run_boot("live-ganondorf-room", "gan-noball", mode="noball")
        self.assertEqual(self.one("live-ganondorf-room", s), "FAIL", p.stdout[-3000:])

    # -- step 9: the enemy turn
    def test_stalfos_no_ref_then_pass(self):
        self.no_ref_then_pass("live-stalfos-weak-room")

    def test_stalfos_fail_when_it_never_turns(self):
        p, s = self.run_boot("live-stalfos-weak-room", "st-stay", mode="stay")
        self.assertEqual(self.one("live-stalfos-weak-room", s), "FAIL", p.stdout[-3000:])
        self.assertIn("no turn in", p.stdout + str(s))

    def test_stalfos_fail_on_a_quick_hand_back(self):
        p, s = self.run_boot("live-stalfos-weak-room", "st-back", mode="back")
        self.assertEqual(self.one("live-stalfos-weak-room", s), "FAIL", p.stdout[-3000:])
        self.assertIn("handed back", p.stdout + str(s))

    # -- boots and switches
    def test_deselected_on_the_default_boot(self):
        out = os.path.join(self.tmp, "kakariko")
        p = pytest_run([BOSSES, ENEMY, "--fake", "--no-rerun", "--out", out, "-q"] + ENV)
        s = summary(out)
        self.assertEqual(s["tests"], [], p.stdout[-3000:])
        self.assertEqual(len(s["deselected"]), 3, s["deselected"])

    def test_deselected_without_the_switches(self):
        p, s = self.run_boot("live-stalfos-weak-room", "none-env", env_args=())
        self.assertEqual(s["tests"], [], p.stdout[-3000:])


if __name__ == "__main__":
    unittest.main()
