"""Tests for tools/autotest/hs_steps.py: steps files on Hookshot, against a fake pair of games (no game, no server)."""
import glob
import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import hs_steps as hs  # noqa: E402

SCENARIOS = os.path.join(os.path.dirname(__file__), "..", "scenarios")


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class FakeGames:
    """Two games and a server as logs; `on_sleep` lets a test change them while the runner waits."""

    def __init__(self, clock):
        self.clock = clock
        self.logs = {"A": "", "B": ""}
        self.running = {"A": False, "B": False}
        self.server = ""
        self.calls = []
        self.shots = []
        self.join_rule = lambda x, password: "welcomed"   # what a start leads to
        self.ended = []

    def start(self, x, password, wait=True):
        self.calls.append(("start", x, password))
        self.running[x] = True
        self.logs[x] = "boot\n"
        outcome = self.join_rule(x, password)
        if outcome == "welcomed":
            self.logs[x] += "coop: welcomed as player 2 on 127.0.0.1:27560\n"
        elif outcome == "refused":
            self.server += "refused a connection, reason 6: wrong join password\n"

    def wait_joined(self, x):
        self.calls.append(("wait", x))
        if hs.JOINED not in self.logs[x]:
            raise TimeoutError("game %s did not reach it" % x)

    def load(self, xs):
        self.calls.append(("load",) + tuple(xs))
        return [(x, True, "") for x in xs]

    def play(self, files, name):
        self.calls.append(("play", tuple(sorted(files.items())), name))

    def close(self, x):
        self.calls.append(("close", x))
        self.running[x] = False

    def kill(self, x, keep_log=None):
        self.calls.append(("kill", x, keep_log))
        self.running[x] = False

    def server_stop(self):
        self.calls.append(("server", "stop"))

    def server_start(self):
        self.calls.append(("server", "start"))

    def server_log(self):
        return self.server

    def log(self, x):
        return self.logs[x]

    def alive(self, x):
        return self.running[x]

    def shot(self, name):
        self.shots.append(name)

    def ended_by_itself(self):
        out, self.ended = self.ended, []
        return out


class ParseTest(unittest.TestCase):
    def test_every_steps_file_parses(self):
        files = sorted(glob.glob(os.path.join(SCENARIOS, "*.steps.txt")))
        self.assertGreater(len(files), 10)
        for f in files:
            with open(f, encoding="utf-8") as fh:
                hs.parse_steps(fh.read())

    def test_verbs(self):
        steps = hs.parse_steps("""
start A B           # both
load A B
play A stray-out.A.txt B x.B.txt
close B
kill B
server stop
sleep 2.5
password B zelda-1
password B -
try B refused
expect both A=1 B=0
shot here
lines keep-number-A A max=0 coop: welcomed as player [2-4] on
""")
        self.assertEqual([s.verb for s in steps], ["start", "load", "play", "close", "kill", "server", "sleep",
                                                   "password", "password", "try", "expect", "shot", "lines"])
        self.assertEqual(steps[2].args, [("A", "stray-out.A.txt"), ("B", "x.B.txt")])
        self.assertEqual(steps[6].args, [2.5])
        self.assertEqual(steps[7].args, ["B", "zelda-1"])
        self.assertEqual(steps[8].args, ["B", ""])
        self.assertEqual(steps[10].args, ["both", {"A": 1, "B": 0}])
        self.assertEqual(steps[12].args, ["keep-number-A", "A", "max", 0])
        self.assertEqual(steps[12].pattern, "coop: welcomed as player [2-4] on")

    def test_errors(self):
        for text in ("start", "start C", "play A", "close A B", "server restart", "sleep x", "password B",
                     "try B maybe", "expect x", "expect x C=1", "lines x A most=1 r", "dance A"):
            with self.assertRaises(hs.StepsError, msg=text):
                hs.parse_steps(text)

    def test_header(self):
        h = hs.parse_header("#! switches A=1,B:C=2\n#! switches D=3\n#! start coop: test warp done\n#! timeout 300\n")
        self.assertEqual(h, {"switches": ["A=1", "B:C=2", "D=3"], "start": "coop: test warp done", "timeout": 300})


class CountTest(unittest.TestCase):
    def test_lines(self):
        log = "x coop: welcomed as player 1 on y\nx coop: welcomed as player 2 on y\nother\n"
        self.assertEqual(hs.count_lines(log, "coop: welcomed as player [2-4] on"), 1)
        self.assertEqual(hs.count_lines(log, "coop: welcomed"), 2)

    @unittest.skipUnless(hs.WINDOWS, ".NET regex through PowerShell")
    def test_dotnet_only_pattern(self):
        # A variable-width lookbehind (cutscene.steps.txt's form) is not Python re.
        log = "hold start\nP1 snap 0.8 m (show)\n"
        self.assertEqual(hs.count_lines(log, r"P\d+ snap \S+ m(?<=hold start[\s\S]*)"), 1)

    def test_live_actors(self):
        log = ("coop: objects: + CoopPuppet_Child 0x10 state 4\ncoop: objects: + Dm_Npc_Navi 0x20 state 4\n"
               "coop: objects: + CoopPuppet_Adult 0x30\ncoop: objects: - CoopPuppet_Child 0x10\n")
        self.assertEqual(hs.puppet_count(log)[:2], (1, 1))
        self.assertIn("1 CoopPuppet_Adult", hs.puppet_count(log)[2])


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.out = tempfile.mkdtemp()
        self.clock = Clock()
        self.g = FakeGames(self.clock)
        self.lines = []
        self.r = hs.Runner(self.g, self.out, say=self.lines.append, expect_wait=30, try_wait=40, clock=self.clock,
                           sleep=self.clock.sleep, default_password="")

    def tearDown(self):
        shutil.rmtree(self.out, ignore_errors=True)

    def counts(self):
        with open(os.path.join(self.out, "counts.txt"), encoding="utf-8") as f:
            return f.read().splitlines()

    def test_join_password_as_run_ps1(self):
        right = "zelda-test-ocarina-42"
        self.g.join_rule = lambda x, pw: "welcomed" if pw == right else ("refused" if pw or x == "B" else "none")
        with open(os.path.join(SCENARIOS, "join-password.steps.txt"), encoding="utf-8") as f:
            steps = hs.parse_steps(f.read())
        fails = self.r.run(steps)
        c = self.counts()
        self.assertEqual(c[0], "PASS try B (want refused, got refused, password set: True)")
        self.assertEqual(c[1], "PASS try B (want welcomed, got welcomed, password set: True)")
        self.assertEqual(c[2], "PASS try B (want refused, got refused, password set: False)")
        # The last try wants "none" (the address turned away after 5 wrong proofs); this fake has no such rule: one FAIL.
        self.assertEqual(c[-1], "FAIL try B (want none, got welcomed, password set: True)")
        self.assertEqual(fails, 1)
        self.assertIn(("start", "A", right), self.g.calls)
        self.assertEqual(sum(1 for c in self.g.calls if c[0] == "kill" and str(c[2]).startswith("try")), 7)  # each try ends B, log kept

    def test_start_waits_for_both_and_password_reaches_the_game(self):
        self.r.run(hs.parse_steps("password B secret\nstart A B\n"))
        self.assertEqual(self.g.calls[:4], [("start", "A", ""), ("start", "B", "secret"), ("wait", "A"),
                                            ("wait", "B")])

    def test_a_game_without_a_password_line_sends_the_servers(self):
        # Every session may have a join password (OOT_COOP_TEST_PASSWORD, run.ps1 -ServerPassword): a game with no
        # password line sends it, as run.ps1's Start-Game does; "password X -" is none.
        r = hs.Runner(self.g, self.out, say=self.lines.append, clock=self.clock, sleep=self.clock.sleep,
                      default_password="autotest-pass")
        self.g.join_rule = lambda x, pw: "welcomed" if pw == "autotest-pass" else "refused"
        fails = r.run(hs.parse_steps("start A B\npassword B -\ntry B refused\npassword B wrong\ntry B refused\n"))
        self.assertEqual(self.g.calls[:2], [("start", "A", "autotest-pass"), ("start", "B", "autotest-pass")])
        self.assertEqual(self.counts(), ["PASS try B (want refused, got refused, password set: False)",
                                         "PASS try B (want refused, got refused, password set: True)"])
        self.assertEqual(fails, 0)
        old = os.environ.get("OOT_COOP_TEST_PASSWORD")
        try:
            os.environ["OOT_COOP_TEST_PASSWORD"] = "from-env"
            self.assertEqual(hs.Runner(self.g, self.out).password("A"), "from-env")
        finally:
            if old is None:
                os.environ.pop("OOT_COOP_TEST_PASSWORD", None)
            else:
                os.environ["OOT_COOP_TEST_PASSWORD"] = old

    def test_expect_waits_then_passes(self):
        self.g.running = {"A": True, "B": True}
        orig = self.clock.sleep

        def sleep(s):
            orig(s)
            if self.clock.t >= 5:   # the puppet and its fairy come after 5 s
                self.g.logs["A"] = "coop: objects: + CoopPuppet_Child 0x1\ncoop: objects: + Dm_Npc_Navi 0x2\n"
        self.r.sleep = sleep
        self.r.run(hs.parse_steps("expect both-loaded A=1 B=0\n"))
        c = self.counts()
        self.assertTrue(c[0].startswith("PASS both-loaded (want A=1 B=0, after 5 s): A: 1 puppet (1 CoopPuppet_Child), "
                                        "1 fairy; B: 0 puppet (), 0 fairy"), c[0])
        self.assertEqual(self.g.shots, ["s1-both-loaded"])

    def test_expect_fails_after_the_wait_and_a_closed_game_counts_zero(self):
        self.g.logs["B"] = "coop: objects: + CoopPuppet_Child 0x1\ncoop: objects: + Dm_Npc_Navi 0x2\n"
        self.g.running = {"A": True, "B": False}
        self.r.run(hs.parse_steps("expect b-left B=1\n"))
        c = self.counts()
        self.assertTrue(c[0].startswith("FAIL b-left (want B=1, after 30 s): B: 0 puppet"), c[0])

    def test_lines(self):
        self.g.logs["A"] = "coop: welcomed as player 1 on x\ncoop: welcomed as player 1 on x\n"
        fails = self.r.run(hs.parse_steps(
            "lines keep-number-A A max=0 coop: welcomed as player [2-4] on\n"
            "lines keep-number-A1 A min=2 coop: welcomed as player 1 on\n"
            "lines three A min=3 coop: welcomed as player 1 on\n"))
        c = self.counts()
        self.assertEqual(c[0], "PASS keep-number-A (A: 0 lines match 'coop: welcomed as player [2-4] on', at most 0)")
        self.assertEqual(c[1], "PASS keep-number-A1 (A: 2 lines match 'coop: welcomed as player 1 on', at least 2)")
        self.assertTrue(c[2].startswith("FAIL three (A: 2 lines"))
        self.assertEqual(fails, 1)

    def test_a_step_that_cannot_run_is_a_fail_and_the_rest_runs(self):
        self.g.join_rule = lambda x, pw: "none"   # A never joins: start's wait raises
        fails = self.r.run(hs.parse_steps("start A\nshot after\n"))
        c = self.counts()
        self.assertTrue(c[0].startswith("FAIL step 1 (start A): game A did not reach it"), c[0])
        self.assertEqual(self.g.shots, ["s2-after"])
        self.assertEqual(fails, 1)

    def test_a_game_that_ends_by_itself_is_a_fail(self):
        self.g.ended = ["B"]
        self.r.run(hs.parse_steps("sleep 1\n"))
        self.assertEqual(self.counts(), ["FAIL game B ended by itself during step 1"])

    def test_stray_runs_every_step_in_order(self):
        with open(os.path.join(SCENARIOS, "stray.steps.txt"), encoding="utf-8") as f:
            steps = hs.parse_steps(f.read())
        self.r.run(steps)
        verbs = [c[0] for c in self.g.calls]
        self.assertIn(("server", "stop"), self.g.calls)
        self.assertLess(self.g.calls.index(("server", "stop")), self.g.calls.index(("server", "start")))
        self.assertIn(("kill", "B", None), self.g.calls)
        self.assertEqual(verbs.count("load"), 3)
        self.assertTrue(any(c[0] == "play" and ("A", "stray-out.A.txt") in c[1] for c in self.g.calls))
        self.assertEqual(sum(1 for line in self.counts() if line.startswith(("PASS", "FAIL"))), 12)  # 10 expect, 2 lines


class DryRunTest(unittest.TestCase):
    def test_dry_run_starts_nothing(self):
        out = io.StringIO()
        with redirect_stdout(out):
            code = hs.main(["run", "--steps", os.path.join(SCENARIOS, "stray.steps.txt"), "--game-a", "GA",
                            "--game-b", "GB", "--server", "S.exe", "--out", "O", "--port", "27570", "--pipe",
                            "coop-auto-g2", "--dry-run"])
        text = out.getvalue()
        self.assertEqual(code, 0)
        self.assertIn("port 27570", text)
        self.assertIn("step 17: kill B", text)
        self.assertIn("would stop the server", text)
        self.assertIn("would wait until A=1 B=1 puppets and fairies", text)
        self.assertFalse(os.path.exists("O"))

    def test_check(self):
        out = io.StringIO()
        with redirect_stdout(out):
            code = hs.main(["check", os.path.join(SCENARIOS, "join-password.steps.txt")])
        self.assertEqual(code, 0)
        self.assertIn("14 steps (password start try)", out.getvalue())


class HookshotGamesTest(unittest.TestCase):
    """The real backend's files: a stand-in game (a copy of cmd.exe (or this Python), which ends at once on an empty stdin), a fake
    bridge and a fake server log. No Hookshot, no server."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.out = os.path.join(self.tmp, "out")
        os.makedirs(self.out)
        self.games = {}
        for x in "AB":
            g = os.path.join(self.tmp, "hookshot-" + x)
            os.makedirs(os.path.join(g, "autotest-save", "slot_00"))
            with open(os.path.join(g, "autotest-save", "slot_00", "progress.sav"), "w") as f:
                f.write("base")
            stand_in = (os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "cmd.exe") if hs.WINDOWS
                        else sys.executable)   # ends at once on an empty stdin
            shutil.copy2(stand_in, os.path.join(g, "hookshot.exe" if hs.WINDOWS else "hookshot"))
            self.games[x] = g
        restored, said = [], []

        class Bridge:
            @staticmethod
            def connect(path, timeout=10.0):
                return Bridge()

            def request(self, *a, **k):
                return {}

            def close(self):
                pass

        hk = type("hk", (), {})()
        hk.exe_dir = lambda g: g
        hk.restore_save = lambda g: restored.append(g) or True
        hk.Bridge = Bridge
        self.restored = restored
        self.g = hs.HookshotGames(hk, self.games, self.out, None, 27560, "coop-test", ["X=1"], 30,
                                  {"switches": [], "start": None, "timeout": None}, say=said.append)

    def tearDown(self):
        for p in self.g.procs.values():
            if p.poll() is None:
                p.kill()
                p.wait(10)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_fresh_log_each_start_and_the_old_one_kept(self):
        log = os.path.join(self.games["B"], "hookshot_log.txt")
        with open(log, "w") as f:
            f.write("an old run\n")
        self.g.start("B", "pw", wait=False)
        self.assertFalse(os.path.exists(log))            # the first start: the old file goes, nothing kept
        self.assertEqual(self.restored, [self.games["B"]])
        with open(log, "w") as f:
            f.write("coop: welcomed as player 2\n")
        self.assertEqual(self.g.log("B"), "coop: welcomed as player 2\n")
        self.g.kill("B", keep_log="try9")
        self.assertTrue(os.path.isfile(os.path.join(self.out, "hookshot_log-B-try9.txt")))
        self.g.start("B", "", wait=False)               # a second start keeps the first run's log
        self.assertTrue(os.path.isfile(os.path.join(self.out, "hookshot_log-B-1.txt")))
        self.g.procs["B"].wait(30)
        self.assertFalse(self.g.alive("B"))
        self.assertEqual(self.g.ended_by_itself(), ["B"])   # it ended without a close or kill
        self.assertEqual(self.g.ended_by_itself(), [])
        self.g.close("B")
        self.g.finish()

    def test_server_log_and_password_option(self):
        with open(os.path.join(self.out, "server-out.txt"), "w") as f:
            f.write("refused a connection, reason 6\n")
        with open(os.path.join(self.out, "server-out-2.txt"), "w") as f:
            f.write("refused a connection, reason 6\n")
        self.assertEqual(self.g.server_log().count("refused a connection"), 2)
        started = []
        self.g.server_exe = "coop_server.exe"
        real = hs.subprocess.Popen

        class P:
            def __init__(self, args, **k):
                started.append(args)

            def poll(self):
                return 0
        hs.subprocess.Popen = P
        old = os.environ.get("OOT_COOP_TEST_PASSWORD")
        try:
            os.environ["OOT_COOP_TEST_PASSWORD"] = "zelda"
            self.g.server_start()
        finally:
            hs.subprocess.Popen = real
            if old is None:
                os.environ.pop("OOT_COOP_TEST_PASSWORD", None)
            else:
                os.environ["OOT_COOP_TEST_PASSWORD"] = old
        self.assertEqual(started[0], ["coop_server.exe", "--port", "27560", "--password-env", "OOT_COOP_TEST_PASSWORD"])
        self.assertNotIn("zelda", " ".join(started[0]))   # the password never on the command line


if __name__ == "__main__":
    unittest.main()
