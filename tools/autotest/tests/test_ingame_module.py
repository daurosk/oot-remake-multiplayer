"""Selftests of the lane's module proof (B #137 14:57, party-hud-bridge-85e3b255 VOID): a game whose module never started
(no 'coop: playtest: listening on 127.0.0.1:<its port>' line) makes every test of the lane SETUP-FAIL boot at once,
naming the game, instead of a ConnectionRefusedError minutes in. No game: the fake lane (BT_FAKE_NO_MODULE).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, INGAME)
sys.path.insert(0, os.path.dirname(HERE))
import pair as P          # noqa: E402

TESTS = '''
def test_one(pair):
    pass

def test_two(pair):
    pass
'''


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class ModuleProofRun(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        d = os.path.join(self.tmp, "t")
        os.makedirs(d)
        with open(os.path.join(d, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(d, "test_m.py"), "w", encoding="utf-8") as f:
            f.write(TESTS)
        self.dir, self.out = d, os.path.join(self.tmp, "o")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, env):
        e = dict(os.environ, **env)
        t0 = time.monotonic()
        p = subprocess.run([sys.executable, "-m", "pytest", ".", "--fake", "--out", self.out, "-q", "-p", "no:cacheprovider"],
                           cwd=self.dir, capture_output=True, text=True, timeout=600, env=e)
        with open(os.path.join(self.out, "summary.json"), encoding="utf-8") as f:
            return {r["test"].split("::")[-1]: r for r in json.load(f)["tests"]}, p, time.monotonic() - t0

    def test_no_module_on_b_is_setup_fail_boot_naming_b(self):
        rows, p, took = self.run_fake({"BT_FAKE_NO_MODULE": "B"})
        self.assertEqual({k: v["result"] for k, v in rows.items()}, {"test_one": "SETUP-FAIL", "test_two": "SETUP-FAIL"},
                         p.stdout[-3000:])
        self.assertIn("lane1 boot: B: the module never started", rows["test_one"]["why"])
        self.assertLess(took, 120)

    def test_both_modules_up_pass(self):
        rows, p, _ = self.run_fake({})
        self.assertEqual({k: v["result"] for k, v in rows.items()}, {"test_one": "PASS", "test_two": "PASS"},
                         p.stdout[-3000:])


class ModuleStarted(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.games = {}
        for x, port in (("A", 5021), ("B", 5022)):
            path = os.path.join(self.tmp, "game-out-%s.txt" % x)
            with open(path, "w", encoding="utf-8") as f:
                f.write("[hos host] vk: precompile: 34848 programs, 37691 graphics and 5006 compute pipelines, 0 mesh "
                        "fallback programs in 194134 ms (106290 ms of it the caches opened, the notes and programs read)\n")
            self.games[x] = {"port": port, "folder": self.tmp, "log": path}

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def pair(self):
        class NoClient:
            def __init__(self, port, timeout=None):
                pass

            def close(self):
                pass
        return P.Pair(self.games, client=NoClient)

    def say(self, x, line):
        with open(self.games[x]["log"], "a", encoding="utf-8") as f:
            f.write(line + "\n")

    def test_both_listening(self):
        for x in "AB":
            self.say(x, "[hos sead::MainThread] coop: playtest: listening on 127.0.0.1:%d (control on; ...)"
                     % self.games[x]["port"])
        c = Clock()
        self.assertTrue(self.pair().module_started(clock=c, sleep=c.sleep))
        self.assertEqual(c(), 1000.0)      # (no wait when the lines are there)

    def test_missing_line_names_the_game_and_its_last_line(self):
        self.say("A", "[hos sead::MainThread] coop: playtest: listening on 127.0.0.1:5021 (control on; ...)")
        c = Clock()
        with self.assertRaises(P.SetupFail) as e:
            self.pair().module_started(seconds=10, clock=c, sleep=c.sleep)
        self.assertIn("B: the module never started", str(e.exception))
        self.assertIn("127.0.0.1:5022", str(e.exception))
        self.assertIn("vk: precompile: 34848 programs", str(e.exception))   # (where the game stopped)
        self.assertGreaterEqual(c() - 1000.0, 10)

    def test_another_ports_line_does_not_count(self):
        """A's line must name A's own port: :50210 is not :5021, and B's port is not A's."""
        self.say("A", "[hos sead::MainThread] coop: playtest: listening on 127.0.0.1:50210 (control on; ...)")
        self.say("B", "[hos sead::MainThread] coop: playtest: listening on 127.0.0.1:5022 (control on; ...)")
        c = Clock()
        with self.assertRaises(P.SetupFail) as e:
            self.pair().module_started(seconds=2, clock=c, sleep=c.sleep)
        self.assertIn("A: the module never started", str(e.exception))


if __name__ == "__main__":
    unittest.main()
