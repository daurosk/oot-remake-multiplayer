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

    def start(self, x, password, wait=True, idle=False):
        self.calls.append(("start", x, password) + (("idle",) if idle else ()))
        self.running[x] = True
        self.logs[x] = "boot\n"
        outcome = "none" if idle else self.join_rule(x, password)
        if outcome == "welcomed":
            self.logs[x] += "coop: welcomed as player 2 on 127.0.0.1:27560\n"
        elif outcome == "refused":
            self.server += "refused a connection, reason 6: wrong join password\n"

    def wait_ready(self, x):
        self.calls.append(("ready", x))

    def key(self, x, vk, scan):
        self.calls.append(("key", x, vk, scan))
        if getattr(self, "landed", False):
            self.logs[x] += "coop: F1: Longshot's menu on co-op's Multiplayer tab (our - menu cannot open now)\n"

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

    def freeze(self, x):
        self.calls.append(("freeze", x))

    def thaw(self, x):
        self.calls.append(("thaw", x))

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

    def wshot(self, name):
        self.shots.append("window " + name)

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
freeze B
thaw B
""")
        self.assertEqual([s.verb for s in steps], ["start", "load", "play", "close", "kill", "server", "sleep",
                                                   "password", "password", "try", "expect", "shot", "lines",
                                                   "freeze", "thaw"])
        self.assertEqual(steps[13].args, ["B"])
        self.assertEqual(steps[2].args, [("A", "stray-out.A.txt"), ("B", "x.B.txt")])
        self.assertEqual(steps[6].args, [2.5])
        self.assertEqual(steps[7].args, ["B", "zelda-1"])
        self.assertEqual(steps[8].args, ["B", ""])
        self.assertEqual(steps[10].args, ["both", {"A": 1, "B": 0}, []])
        self.assertEqual(steps[12].args, ["keep-number-A", "A", "max", 0])
        self.assertEqual(steps[12].pattern, "coop: welcomed as player [2-4] on")

    def test_errors(self):
        for text in ("start", "start C", "play A", "close A B", "freeze", "thaw C", "freeze A B", "server restart",
                     "sleep x", "password B",
                     "try B maybe", "expect x", "expect x C=1", "lines x A most=1 r", "dance A"):
            with self.assertRaises(hs.StepsError, msg=text):
                hs.parse_steps(text)

    def test_header(self):
        h = hs.parse_header("#! switches A=1,B:C=2\n#! switches D=3\n#! start coop: test warp done\n#! timeout 300\n"
                            "#! server --owner-hold-ms 0\n")
        self.assertEqual(h, {"switches": ["A=1", "B:C=2", "D=3"], "start": "coop: test warp done", "timeout": 300,
                             "server": ["--owner-hold-ms", "0"]})


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

    def test_launch_waits_for_the_renderer_only(self):
        # Row 124 (save-refresh-list): the games start before the server, so `start` would wait for a welcome that
        # cannot come; `launch` stops at the renderer.
        self.g.join_rule = lambda x, pw: "none"
        steps = hs.parse_steps("launch A B")
        self.assertEqual([s.verb for s in steps], ["launch"])
        self.assertEqual(self.r.run(steps), 0)
        self.assertEqual([c for c in self.g.calls if c[0] in ("wait", "ready")], [("ready", "A"), ("ready", "B")])
        with self.assertRaises(hs.StepsError):
            hs.parse_steps("launch")

    def test_a_once_per_process_line_said_again_is_a_fail(self):
        # Row 267: "coop: objects: list head" at every object scan flooded a tester's log.
        head = "[hos] coop: objects: list head 0x105f302870, 8 entries, 7 actors\n"
        build = "[hos] coop: build hookshot v0\n"
        self.g.logs["A"] = build + head
        self.g.logs["B"] = build + head + build + head
        self.assertEqual(self.r.run([]), 0)  # once per game process: no line at all
        self.g.logs["A"] += head * 3
        self.assertEqual(self.r.run([]), 1)
        self.assertEqual(self.counts(), ["FAIL A: objects list head (row 267) said 4 times in 1 game process(es), "
                                         "want at most once each"])
        self.assertEqual(hs.once_floods(head * 2), [("objects list head (row 267)", 2, 1)])  # no build line: one
        self.assertEqual(hs.once_floods(""), [])

    def test_start_waits_for_both_and_password_reaches_the_game(self):
        self.r.run(hs.parse_steps("password B secret\nstart A B\n"))
        self.assertEqual(self.g.calls[:4], [("start", "A", ""), ("start", "B", "secret"), ("wait", "A"),
                                            ("wait", "B")])

    def test_idle_start_has_no_server_and_waits_for_the_renderer_only(self):
        # start A idle: co-op idle at the title (the Host page before hosting, difficulty-lobby); no join wait.
        steps = hs.parse_steps("start A idle\nstart A B idle\nstart B\n")
        self.assertEqual([(s.args, s.idle) for s in steps], [(["A"], True), (["A", "B"], True), (["B"], False)])
        self.assertEqual(self.r.run(steps[:1]), 0)
        self.assertEqual(self.g.calls[:2], [("start", "A", "", "idle"), ("ready", "A")])
        for text in ("start idle", "load A idle", "start idle A"):
            with self.assertRaises(hs.StepsError):
                hs.parse_steps(text)

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
        # the counts match; no see= check proves them on the shot: UNPROVEN (owner rule: no PASS from log lines)
        self.assertTrue(c[0].startswith("UNPROVEN both-loaded (want A=1 B=0, after 5 s): A: 1 puppet (1 CoopPuppet_Child), "
                                        "1 fairy; B: 0 puppet (), 0 fairy"), c[0])
        self.assertEqual(self.g.shots, ["s1-both-loaded"])

    def test_expect_fails_after_the_wait_and_a_closed_game_counts_zero(self):
        self.g.logs["B"] = "coop: objects: + CoopPuppet_Child 0x1\ncoop: objects: + Dm_Npc_Navi 0x2\n"
        self.g.running = {"A": True, "B": False}
        self.r.run(hs.parse_steps("expect b-left B=1\n"))
        c = self.counts()
        self.assertTrue(c[0].startswith("FAIL b-left (want B=1, after 30 s): B: 0 puppet"), c[0])

    def test_key_lands_or_setup_fail_stops_the_run(self):
        # (f1-event-c31) A posted F1 counts only once the module's "coop: F1: " line for it is in the log.
        self.g.landed = True
        self.r.run(hs.parse_steps("key A F1\nlines after A min=1 coop: F1: Longshot\n"))
        self.assertEqual(self.g.calls[-1], ("key", "A", 0x70, 0x3B))
        self.assertEqual(self.counts(), ["PASS after (A: 1 lines match 'coop: F1: Longshot', at least 1)"])
        self.g.landed = False
        fails = self.r.run(hs.parse_steps("key A F1\nshot never\n"))
        c = self.counts()
        self.assertTrue(c[-1].startswith("FAIL SETUP-FAIL step 1 (key A F1): F1 posted to A's window but no new"), c[-1])
        self.assertEqual(self.g.shots, [])   # (the run stopped at the failed setup)
        self.assertEqual(fails, 1)

    def test_state_gate(self):
        # (f1-event-c31, review-b119 7) the LAST "coop: free play:" line counts: an event that came and went is not one.
        self.g.logs["A"] = "coop: free play: no (title)\ncoop: free play: yes\ncoop: free play: no (event)\n"
        self.r.run(hs.parse_steps("state A event 5\nlines after A min=1 free play\n"))
        self.assertEqual(self.counts(), ["PASS after (A: 3 lines match 'free play', at least 1)"])
        self.g.logs["A"] += "coop: free play: yes\n"
        fails = self.r.run(hs.parse_steps("state A event 5\nshot never\n"))
        c = self.counts()
        self.assertTrue(c[-1].startswith("FAIL SETUP-FAIL step 1 (state A event 5): A is not in event after 5 s: "
                                         "its last 'coop: free play:' line says 'yes'"), c[-1])
        self.assertEqual(self.g.shots, [])
        self.assertEqual(fails, 1)
        self.assertEqual(hs.last_state("x coop: free play: no (load, no Link)\r\n"), "no (load, no Link)")
        self.assertIsNone(hs.last_state("nothing\n"))

    def test_state_parse(self):
        self.assertEqual(hs.parse_steps("state B free\n")[0].args, ["B", "free", hs.STATE_WAIT])
        for bad in ("state A\n", "state A cutscene\n", "state A free 0\n", "state A free x\n"):
            with self.assertRaises(hs.StepsError):
                hs.parse_steps(bad)

    def test_flag_gate(self):
        # (f1-event-c124) the expect-state gate: only a read logged AFTER the step began counts, and its value decides.
        watch = "coop: flags debug: get_key64(0xb66198d17d6584e) = %s\n"
        self.g.on_sleep = lambda: None

        def sleep(s):   # the game logs while the runner waits
            self.clock.sleep(s)
            self.g.on_sleep()
        self.r.sleep = sleep
        self.g.logs["A"] = watch % "0"      # an old read (before the scenario set it): never taken
        fails = self.r.run(hs.parse_steps("flag A key64:0x0b66198d17d6584e 0 5\nshot never\n"))
        c = self.counts()
        self.assertTrue(c[-1].startswith("FAIL SETUP-FAIL step 1 (flag A key64:0x0b66198d17d6584e 0 5): A's "
                                         "key64:0x0b66198d17d6584e is not 0 after 5 s: the module's last read says "
                                         "'0' (no read since this step began"), c[-1])
        self.assertEqual(self.g.shots, [])
        self.assertEqual(fails, 1)
        # a new read of 1 (the chest open) is a SETUP-FAIL too; another key's read is not this one's
        self.g.on_sleep = lambda: self.g.logs.__setitem__("A", self.g.logs["A"] + watch % "1" +
                                                          "coop: flags debug: get_key64(0x1234) = 0\n")
        self.r.run(hs.parse_steps("flag A key64:0x0b66198d17d6584e 0 2\n"))
        self.assertIn("the module's last read says '1'", self.counts()[-1])
        # a new read of 0: passes, the run goes on
        self.g.logs["A"] = ""
        self.g.on_sleep = lambda: self.g.logs.__setitem__("A", watch % "0")
        fails = self.r.run(hs.parse_steps("flag A key64:0x0b66198d17d6584e 0\nshot after\n"))
        self.assertEqual(fails, 2)   # (the count runs on: the two SETUP-FAILs above, none new)
        self.assertEqual(self.g.shots, ["s2-after"])
        self.assertEqual(hs.flag_reads("coop: flags debug: get_bool(0x0000abcd) = none\n", "bool", 0xabcd), ["none"])

    def test_flag_parse_and_watch_switch(self):
        s = hs.parse_steps("flag B bool:0xABCD 1 9\n")
        self.assertEqual(s[0].args, ["B", "bool:0xABCD", "1", 9.0])
        self.assertEqual(hs.parse_steps("flag A key64:0x1 0\n")[0].args[3], hs.FLAG_WAIT)
        for bad in ("flag A key64:0x0b66 2\n", "flag A key64:0 0\n", "flag A bool:0x100000000 0\n",
                    "flag A 0x0b66 0\n", "flag A key64:0x1 0 0\n", "flag A\n"):
            with self.assertRaises(hs.StepsError):
                hs.parse_steps(bad)
        steps = hs.parse_steps("flag A key64:0x0b66198d17d6584e 0\n")
        head = hs.parse_header("#! switches LICHII_COOP_DEBUG_FLAGS=1,A:LICHII_COOP_DEBUG_KEY64=0xb66198d17d6584e\n")
        self.assertEqual(hs.flag_watch_problems(steps, head), [])
        for sw in ("B:LICHII_COOP_DEBUG_KEY64=0x0b66198d17d6584e", "A:LICHII_COOP_DEBUG_KEY64=0x1234",
                   "A:LICHII_COOP_DEBUG_BOOL=0x0b66198d17d6584e", "LICHII_COOP_DEBUG_FLAGS=1"):
            problems = hs.flag_watch_problems(steps, hs.parse_header("#! switches %s\n" % sw))
            self.assertEqual(len(problems), 1, sw)
            self.assertIn("A:LICHII_COOP_DEBUG_KEY64=0x0b66198d17d6584e", problems[0])
        # a switch for every game counts too
        self.assertEqual(hs.flag_watch_problems(steps, hs.parse_header(
            "#! switches LICHII_COOP_DEBUG_KEY64=0x0b66198d17d6584e\n")), [])

    def test_wshot(self):
        self.r.run(hs.parse_steps("wshot f1-title\n"))
        self.assertEqual(self.g.shots, ["window s1-f1-title"])
        self.assertEqual(hs.parse_steps("wshot a.b-c_1\n")[0].args, ["a.b-c_1"])
        for bad in ("wshot\n", "wshot a b\n", "wshot a/b\n"):
            with self.assertRaises(hs.StepsError):
                hs.parse_steps(bad)

    def test_window_shot_command_calls_the_shared_helper(self):
        cmd = hs.window_shot_command(r"C:\A B\screen-shot.ps1", 42, r"C:\o\it's.png", r"C:\o\w.txt")
        self.assertEqual(cmd[:5], ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command"])
        self.assertIn(". 'C:\A B\screen-shot.ps1';", cmd[5])
        self.assertIn("Save-WindowShot -Process $p -Path 'C:\o\it''s.png' -Log 'C:\o\w.txt'", cmd[5])
        self.assertIn("Get-Process -Id 42 ", cmd[5])
        self.assertNotIn('"', cmd[5])

    def test_window_shot_hang_or_no_start_is_a_setup_fail(self):
        # (review-c124 on 8614cb1cc) a helper that hangs is ended and the step is a SETUP-FAIL, not an exception
        def hang(cmd, **kw):
            raise hs.subprocess.TimeoutExpired(cmd, kw["timeout"])

        def missing(cmd, **kw):
            raise FileNotFoundError("powershell")

        with self.assertRaisesRegex(hs.SetupFail, "did not end within 90 s"):
            hs.run_window_shot(["x"], run=hang)
        with self.assertRaisesRegex(hs.SetupFail, "did not start"):
            hs.run_window_shot(["x"], run=missing)

        class R:
            returncode, stdout, stderr = 3, "COVERED a.png: other\n", None
        self.assertEqual(hs.run_window_shot(["x"], run=lambda cmd, **kw: R()), (3, "COVERED a.png: other"))

    def test_key_parse(self):
        self.assertEqual(hs.parse_steps("key B F1\n")[0].args, ["B", "F1"])
        for bad in ("key A\n", "key A F6\n", "key A F1 F1\n"):
            with self.assertRaises(hs.StepsError):
                hs.parse_steps(bad)

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
        self.assertEqual(sum(1 for line in self.counts() if line.startswith(("PASS", "FAIL", "UNPROVEN"))), 12)  # 10 expect, 2 lines


    def test_freeze_and_thaw_reach_the_game_in_order(self):
        self.r.run(hs.parse_steps("freeze B\nserver start\nthaw B\n"))
        calls = [c for c in self.g.calls if c[0] in ("freeze", "server", "thaw")]
        self.assertEqual(calls, [("freeze", "B"), ("server", "start"), ("thaw", "B")])
        self.assertEqual(self.r.fails, 0)


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

    def test_dry_run_says_the_password_without_setting_it(self):
        old = os.environ.pop("OOT_COOP_TEST_PASSWORD", None)
        try:
            out = io.StringIO()
            with redirect_stdout(out):
                code = hs.main(["run", "--steps", os.path.join(SCENARIOS, "stray.steps.txt"), "--game-a", "GA",
                                "--game-b", "GB", "--server", "S.exe", "--out", "O", "--dry-run"])
            self.assertEqual(code, 0)
            self.assertIn("a random join password would be made", out.getvalue())
            self.assertNotIn("OOT_COOP_TEST_PASSWORD", os.environ)
        finally:
            if old is not None:
                os.environ["OOT_COOP_TEST_PASSWORD"] = old

    def test_check(self):
        out = io.StringIO()
        with redirect_stdout(out):
            code = hs.main(["check", os.path.join(SCENARIOS, "join-password.steps.txt")])
        self.assertEqual(code, 0)
        self.assertIn("14 steps (password start try)", out.getvalue())

    def test_check_reads_a_file_with_a_bom(self):
        # A steps file written by Windows PowerShell 5.1 (Out-File -Encoding utf8: BOM + CRLF) reads the same.
        with open(os.path.join(SCENARIOS, "join-password.steps.txt"), encoding="utf-8") as f:
            text = f.read()
        d = tempfile.mkdtemp()
        try:
            path = os.path.join(d, "bom.steps.txt")
            with open(path, "wb") as f:
                f.write(b"\xef\xbb\xbf" + text.replace("\n", "\r\n").encode("utf-8"))
            out = io.StringIO()
            with redirect_stdout(out):
                code = hs.main(["check", path])
            self.assertEqual(code, 0)
            self.assertIn("14 steps (password start try)", out.getvalue())
        finally:
            shutil.rmtree(d, ignore_errors=True)


class ServerPasswordTest(unittest.TestCase):
    # rejoin-host-273-2: a wrapper that ran `run` without OOT_COOP_TEST_PASSWORD started a server with no join
    # password (coop_server refuses to start) and games with none. Now one is always there for both.
    def test_unset_makes_a_random_one(self):
        env = {}
        self.assertTrue(hs.ensure_server_password(env))
        pw = env["OOT_COOP_TEST_PASSWORD"]
        self.assertTrue(1 <= len(pw) <= 64 and pw.isprintable() and " " not in pw)
        other = {}
        hs.ensure_server_password(other)
        self.assertNotEqual(pw, other["OOT_COOP_TEST_PASSWORD"])   # random, never a fixed one

    def test_empty_counts_as_unset(self):
        env = {"OOT_COOP_TEST_PASSWORD": ""}
        self.assertTrue(hs.ensure_server_password(env))
        self.assertTrue(env["OOT_COOP_TEST_PASSWORD"])

    def test_a_set_one_is_kept(self):
        env = {"OOT_COOP_TEST_PASSWORD": "zelda"}
        self.assertFalse(hs.ensure_server_password(env))
        self.assertEqual(env["OOT_COOP_TEST_PASSWORD"], "zelda")

    def test_the_games_send_the_servers(self):
        # The Runner's default (a game with no `password` line) is the one the server got.
        env = {}
        hs.ensure_server_password(env)
        old = os.environ.get("OOT_COOP_TEST_PASSWORD")
        try:
            os.environ["OOT_COOP_TEST_PASSWORD"] = env["OOT_COOP_TEST_PASSWORD"]
            d = tempfile.mkdtemp()
            self.addCleanup(shutil.rmtree, d, True)
            r = hs.Runner(FakeGames(Clock()), d, say=lambda *_: None)
            self.assertEqual(r.password("B"), env["OOT_COOP_TEST_PASSWORD"])
        finally:
            if old is None:
                os.environ.pop("OOT_COOP_TEST_PASSWORD", None)
            else:
                os.environ["OOT_COOP_TEST_PASSWORD"] = old


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
            shutil.copy2(stand_in, os.path.join(g, "longshot.exe" if hs.WINDOWS else "longshot"))
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
        import hookshot   # (the real rule, not a copy: hookshot.py TEST_LANE_PRECOMPILE, game-commit-c141)
        hk.test_lane_precompile = hookshot.test_lane_precompile
        hk.restore_save = lambda g: restored.append(g) or True
        hk.Bridge = Bridge
        self.packed = []   # (row 265: the kit's start of the pack, before each game start)
        hk.pack_start = lambda g, env, say: self.packed.append((g, env.get("X"))) or []
        self.restored = restored
        self.g = hs.HookshotGames(hk, self.games, self.out, None, 27560, "coop-test", ["X=1"], 30,
                                  {"switches": [], "start": None, "timeout": None, "server": []}, say=said.append)

    def tearDown(self):
        for p in self.g.procs.values():
            if p.poll() is None:
                p.kill()
                p.wait(10)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_each_start_places_its_window_on_the_chosen_monitor(self):
        """start() gives the game the no-activation start flags and places its window (hookshot.place_games)."""
        calls, flags = [], []
        self.g.hk.popen_flags = lambda: flags.append(1) or {}
        self.g.hk.place_games = lambda pids, display, say, wait=0.0, topmost=False: calls.append((dict(pids), display, wait))
        self.g.display = "DISPLAY7"
        self.g.start("B", "pw", wait=False)
        self.assertEqual(flags, [1])
        self.assertEqual(calls, [({"B": self.g.procs["B"].pid}, "DISPLAY7", 30.0)])

    def test_run_stops_before_any_game_when_the_monitor_is_missing(self):
        """main run: a PC with no monitor at all is an error (exit 2) before the server or any game starts."""
        import hookshot
        from unittest import mock
        with mock.patch.object(hs, "WINDOWS", True), \
                mock.patch.object(hookshot.placement, "monitors", lambda: []), \
                mock.patch.object(hs.HookshotGames, "server_start", side_effect=AssertionError("a start happened")):
            steps = os.path.join(self.tmp, "s.steps.txt")
            with open(steps, "w") as f:
                f.write("start A\n")
            out = io.StringIO()
            with redirect_stdout(out):
                code = hs.main(["run", "--steps", steps, "--game-a", self.games["A"], "--game-b", self.games["B"],
                                "--out", os.path.join(self.tmp, "o2"), "--display", "DISPLAY9"])
        self.assertEqual(code, 2)
        with open(os.path.join(self.tmp, "o2", "steps.txt")) as f:
            self.assertIn("no monitor is connected", f.read())   # (placement.py: with none connected, whatever --display names)

    def test_fresh_log_each_start_and_the_old_one_kept(self):
        log = os.path.join(self.games["B"], "longshot_log.txt")
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

    def test_each_start_puts_the_pack_as_the_kits_start_first(self):
        # (row 265: your own tunic files, then the merge, before the game reads its mods; a restart again)
        order, popen = [], hs.subprocess.Popen
        self.g.hk.pack_start = lambda g, env, say: order.append(("pack", g, env.get("X"))) or []
        hs.subprocess.Popen = lambda args, **k: order.append(("game",)) or popen(args, **k)
        try:
            self.g.start("A", "", wait=False)
        finally:
            hs.subprocess.Popen = popen
        self.assertEqual(order, [("pack", self.games["A"], "1"), ("game",)])

    def test_idle_start_sends_no_server_nor_password(self):
        seen, popen = [], hs.subprocess.Popen
        hs.subprocess.Popen = lambda args, **k: seen.append(k["env"]) or popen(args, **k)
        old = os.environ.get("LICHII_COOP_SERVER")
        os.environ["LICHII_COOP_SERVER"] = "10.0.0.1:1"   # (not from the environment either)
        try:
            self.g.start("A", "pw", wait=False, idle=True)
            self.g.start("B", "pw", wait=False)
        finally:
            hs.subprocess.Popen = popen
            if old is None:
                del os.environ["LICHII_COOP_SERVER"]
            else:
                os.environ["LICHII_COOP_SERVER"] = old
        self.assertNotIn("LICHII_COOP_SERVER", seen[0])
        self.assertNotIn("LICHII_COOP_PASSWORD", seen[0])
        self.assertEqual(seen[1]["LICHII_COOP_SERVER"], "127.0.0.1:%d" % self.g.port)
        self.assertEqual(seen[1]["LICHII_COOP_PASSWORD"], "pw")

    def test_each_start_gets_the_test_defaults_a_switch_wins(self):
        # Its own save and no crash ask on the title (a crash record of an earlier run took the load's A presses:
        # smoke hs-players, kit-0119); the same pairs as hookshot.py's, and a run's switch still wins.
        import hookshot
        self.assertEqual(hs.TEST_GAME_DEFAULTS, hookshot.TEST_GAME_DEFAULTS)
        seen, popen = [], hs.subprocess.Popen
        hs.subprocess.Popen = lambda args, **k: seen.append(k["env"]) or popen(args, **k)
        try:
            self.g.start("A", "", wait=False)
            self.g.env = ["LICHII_COOP_PROBLEM_ASK=1"]
            self.g.start("B", "", wait=False)
        finally:
            hs.subprocess.Popen = popen
        self.assertEqual([e["LICHII_COOP_PROBLEM_ASK"] for e in seen], ["0", "1"])
        self.assertEqual([e["LICHII_COOP_TEST_OWN_SAVE"] for e in seen], ["1", "1"])

    def test_each_start_has_no_precompile_unless_asked(self):
        # (game-commit-c141) a test game starts without the shader precompile (hookshot.py TEST_LANE_PRECOMPILE);
        # --precompile default (games.precompile) starts it as players run it; a run's switch wins over both
        seen, popen = [], hs.subprocess.Popen
        hs.subprocess.Popen = lambda args, **k: seen.append(k["env"]) or popen(args, **k)
        try:
            self.g.start("A", "", wait=False)
            self.g.precompile = "default"
            self.g.start("B", "", wait=False)
            self.g.precompile = "test"
            self.g.env = ["RX_VK_PRECOMPILE=1"]
            self.g.start("A", "", wait=False)
        finally:
            hs.subprocess.Popen = popen
        self.assertEqual([e.get("RX_VK_PRECOMPILE") for e in seen], ["0", None, "1"])

    def test_log_is_the_captured_output_when_the_game_writes_no_file(self):
        # Hookshot writes longshot_log.txt only when its stderr goes nowhere; here stderr is game-out-<x>-<n>.txt.
        self.g.start("A", "", wait=False)
        self.g.procs["A"].wait(30)
        with open(os.path.join(self.out, "game-out-A-1.txt"), "w") as f:
            f.write("coop: welcomed as player 1\n")
        self.assertEqual(self.g.log("A"), "coop: welcomed as player 1\n")
        self.g.kill("A", keep_log="try2")
        self.assertTrue(os.path.isfile(os.path.join(self.out, "hookshot_log-A-try2.txt")))
        self.g.finish()
        with open(os.path.join(self.out, "hookshot_log-A.txt")) as f:
            self.assertEqual(f.read(), "coop: welcomed as player 1\n")   # what groups.py judges

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
        self.assertEqual(started[0], ["coop_server.exe", "--port", "27560", "--password-env", "OOT_COOP_TEST_PASSWORD",
                                      "--bind", "127.0.0.1"])   # (test listeners bind 127.0.0.1: no firewall prompt)
        self.assertNotIn("zelda", " ".join(started[0]))   # the password never on the command line

    def test_server_bind_keeps_its_own(self):
        # (hs.py server_bind: 127.0.0.1 added once; a LAN test's own --bind wins)
        self.assertEqual(hs.lanes.hs.server_bind(["s", "--port", "1"]), ["s", "--port", "1", "--bind", "127.0.0.1"])
        self.assertEqual(hs.lanes.hs.server_bind(["s", "--bind", "192.168.1.5"]), ["s", "--bind", "192.168.1.5"])

    def test_server_test_delay_needs_the_test_environment(self):
        started = []
        self.g.server_exe = "coop_server.exe"
        self.g.server_delay = ["--test-delay-ms", "250", "--test-jitter-ms", "60"]
        real = hs.subprocess.Popen

        class P:
            def __init__(self, args, **k):
                started.append((args, k.get("env") or {}))

            def poll(self):
                return 0
        hs.subprocess.Popen = P
        try:
            self.g.server_start()
        finally:
            hs.subprocess.Popen = real
        args, env = started[0]
        self.assertEqual(args[-4:], ["--test-delay-ms", "250", "--test-jitter-ms", "60"])
        self.assertEqual(env.get("OOT_COOP_TEST"), "1")


class WaitJoinedTest(unittest.TestCase):
    """A game joins only once its renderer is up (rc.7: 'welcomed' after 'the shaders took'), so wait_joined waits for the
    renderer first (hookshot.wait_renderer: follows the compile's progress) and the join's timeout counts from there."""

    def games(self, renderer=None):
        calls = []
        hk = type("hk", (), {})()
        hk.RENDERER_STALL, hk.RENDERER_LIMIT = 180, 3600
        hk.wait_renderer = lambda read, alive, *args: calls.append((read(), args)) or 450.0
        g = hs.HookshotGames(hk, {"A": "a", "B": "b"}, "out", None, 27560, "p", [], 30,
                             {"switches": [], "start": None, "timeout": None, "server": []}, say=lambda s: None)
        g.renderer = renderer
        g.log = lambda x: "coop: welcomed as player 2 on 127.0.0.1:27560\n"
        g.alive = lambda x: True
        return g, calls

    def test_renderer_first_with_the_timeout_as_its_wait(self):
        g, calls = self.games()
        g.wait_joined("B")
        self.assertEqual(calls, [("coop: welcomed as player 2 on 127.0.0.1:27560\n", (30, 180, 3600))])

    def test_run_options_reach_the_renderer_wait(self):
        g, calls = self.games(renderer=(900, 240, 2400))
        g.wait_joined("B")
        self.assertEqual(calls[0][1], (900, 240, 2400))

    def test_join_timeout_counts_after_the_renderer(self):
        g, calls = self.games()
        g.log = lambda x: "vk: start-up: the shaders took 450 s\n"   # renderer up, never welcomed
        t = [0.0]
        real_mono, real_sleep = hs.time.monotonic, hs.time.sleep
        hs.time.monotonic = lambda: t[0]
        hs.time.sleep = lambda s: t.__setitem__(0, t[0] + s)
        try:
            with self.assertRaises(TimeoutError) as e:
                g.wait_joined("B")
        finally:
            hs.time.monotonic, hs.time.sleep = real_mono, real_sleep
        self.assertIn("in 30 s after its renderer started", str(e.exception))
        self.assertEqual(len(calls), 1)


if __name__ == "__main__":
    unittest.main()
