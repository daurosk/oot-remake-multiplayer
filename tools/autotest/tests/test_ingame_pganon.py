"""Selftests of row 287's bridge test (ingame/test_bosses.py + ingame/fakes/phantom_ganon.py), no game: fake bridges.
Run by test_ingame.py's load_tests (pganon-cct-287-3's file)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_ingame as T   # noqa: E402

BOOT = "live-phantom-ganon"
TEST = "test_phantom_ganon_hand_over_no_fall"


class PhantomGanonRow(unittest.TestCase):
    """PASS as the fixed game logs it; a fall after the hand-over FAILs (and stays FAIL on its rerun: never FLAKY)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_row(self, env=None):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        p = T.pytest_run([".", "--fake", "--out", out, "-q", "--boot", BOOT, "-k", TEST], env=env)
        return p, T.summary(out)

    def test_pass(self):
        p, s = self.run_row()
        self.assertEqual(T.results(s), {TEST: "PASS"}, p.stdout[-2000:])
        self.assertEqual(s["identity"]["boot"]["name"], BOOT)

    def test_fall_after_hand_over_fails(self):
        p, s = self.run_row({"BT_FAKE_PG_FALL": "1"})
        self.assertEqual(T.results(s), {TEST: "FAIL"}, p.stdout[-2000:])
        self.assertIn("below his fight start", json.dumps(s["tests"]))
        self.assertNotEqual(p.returncode, 0)

    def test_default_boot_deselects_it(self):
        out = os.path.join(self.tmp, "d")
        p = T.pytest_run([".", "--fake", "--out", out, "-q", "-k", TEST])
        s = T.summary(out)
        self.assertIn({"test": "test_bosses.py::" + TEST, "needs_boot": BOOT, "needs_env": []}, s["deselected"],
                      p.stdout[-2000:])


class PhantomGanonBoot(unittest.TestCase):
    """The boot live-phantom-ganon through bt_plugin.boot_args + live.py start --dry-run (hookshot.py run's own parser
    and a parse of both scripts: nofairy, the boss-stage warp), with the lane's games swapped for two folders."""

    def setUp(self):
        self.helper = T.BootCommand("setUp")      # (its two-folder lane and live.py call; its own tests not rerun)
        self.helper.setUp()
        self.tmp = self.helper.tmp

    def tearDown(self):
        self.helper.tearDown()

    def test_boot_args_parse(self):
        import bt_plugin as B

        class Cfg:
            def getoption(self, name):   # (an option this test does not set: None, as pytest's unset default;
                return {"env": [], "session_minutes": 28, "boot": BOOT}.get(name)   # kit-smoke added kit_ls)
        args = B.boot_args(Cfg(), 1, os.path.join(self.tmp, "coop_server.exe"), os.path.join(self.tmp, "s"))
        i = args.index("--lane")
        args[i:i + 2] = self.helper.games()
        out = self.helper.live(*(args + ["--dry-run"]))
        self.assertTrue(out["ok"], out)
        a, b = B.boot_paths(BOOT)
        for want in ("--script-a", a, "--script-b", b, "--no-load"):
            self.assertIn(want, out["runner"])


if __name__ == "__main__":
    unittest.main()
