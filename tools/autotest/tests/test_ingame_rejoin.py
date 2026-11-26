"""Selftests of Pair.rejoin (regress step 4, tools/autotest/ingame/test_rejoin.py): no game, a fake live.py + bridge."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, INGAME)
import pair as P          # noqa: E402


class FakeClient:
    made = []

    def __init__(self, port, timeout=30.0):
        self.port, self.calls, self.closed = port, [], False
        FakeClient.made.append(self)

    def request(self, cmd, **args):
        self.calls.append(cmd)
        return {"ok": True, "frame": 1}

    def hello(self):
        self.calls.append("hello")
        return {"ok": True, "control": True}

    def resume(self):
        self.calls.append("resume")

    def pause(self):
        self.calls.append("pause")

    def close(self):
        self.closed = True


class Rejoin(unittest.TestCase):
    def setUp(self):
        FakeClient.made = []
        self.tmp = tempfile.mkdtemp()
        self.logs = {x: os.path.join(self.tmp, "game-out-%s.txt" % x) for x in "AB"}
        for p in self.logs.values():
            with open(p, "w") as f:
                f.write("old line\n")
        games = {x: {"port": 40000 + i, "folder": self.tmp, "log": self.logs[x]} for i, x in enumerate("AB")}
        self.pair = P.Pair(games, client=FakeClient)
        self.pair.session = self.tmp
        self.asked = []

    def live(self, answers):
        def run(args, timeout):
            self.asked.append((args, timeout))
            if args[0] == "relaunch":       # relaunch: the old log moved aside, a new one from line 1
                os.replace(self.logs["B"], self.logs["B"] + ".1")
                with open(self.logs["B"], "w") as f:
                    f.write("coop: save: our own 5 rupees/ammo/bottles/life/magic values kept in it\n")
            return answers[args[0]]
        return run

    def test_quit_relaunch_new_client_and_log_from_its_start(self):
        self.pair.live = self.live({"quit": {"player": "B", "how": "quit asked"}, "relaunch": {"ok": True, "boot": "played"}})
        old_b, a = self.pair.c["B"], self.pair.c["A"]
        self.pair.begin(600)
        r = self.pair.rejoin("B", boot="boot.B.txt")
        self.assertTrue(r["ok"])
        self.assertEqual([x[0] for x in self.asked], [["quit", "--session", self.tmp, "B"],
                                                     ["relaunch", "--session", self.tmp, "B", "--boot", "boot.B.txt",
                                                      "--no-load"]])
        self.assertTrue(old_b.closed)
        self.assertIsNot(self.pair.c["B"], old_b)
        self.assertEqual(self.pair.c["B"].port, old_b.port)
        self.assertEqual(a.calls, ["resume", "pause"])          # A runs in real time while B is away, held after
        self.assertTrue(self.pair.log["B"].find(r"values kept in it"))   # the new log is read from its first line
        self.assertLessEqual(self.asked[0][1], P.Pair.REJOIN_QUIT_S)
        self.assertLessEqual(self.asked[1][1], P.Pair.REJOIN_BOOT_S)

    def test_a_failed_relaunch_fails_and_still_holds_the_other_game(self):
        self.pair.live = self.live({"quit": {"player": "B", "how": "quit asked"},
                                    "relaunch": {"ok": False, "boot": "FAILED (see steps.txt): the game stays down"}})
        a = self.pair.c["A"]
        self.pair.begin(600)
        with self.assertRaises(AssertionError) as e:
            self.pair.rejoin("B")
        self.assertIn("relaunch", str(e.exception))
        self.assertEqual(a.calls, ["resume", "pause"])

    def test_a_quit_error_fails_before_any_relaunch(self):
        self.pair.live = self.live({"quit": {"error": "game B is not in this session"}, "relaunch": {"ok": True}})
        self.pair.begin(600)
        with self.assertRaises(AssertionError):
            self.pair.rejoin("B")
        self.assertEqual([x[0][0] for x in self.asked], ["quit"])

    def test_default_boot_is_the_lanes_own_with_no_load(self):
        # review-c123-11: the lane boots with --no-load (bt_plugin boot_args); the rejoin replays the lane's own boot
        self.pair.live = self.live({"quit": {"player": "B", "how": "quit asked"}, "relaunch": {"ok": True, "boot": "played"}})
        self.pair.boot = {"A": "lane.A.txt", "B": "lane.B.txt"}
        self.pair.begin(600)
        self.pair.rejoin("B")
        self.assertEqual(self.asked[1][0], ["relaunch", "--session", self.tmp, "B", "--boot", "lane.B.txt", "--no-load"])

    def test_no_boot_relaunch_has_no_no_load(self):
        # (no boot script: the loader takes the title, so no --no-load)
        self.pair.live = self.live({"quit": {"player": "B", "how": "quit asked"}, "relaunch": {"ok": True, "boot": "played"}})
        self.pair.begin(600)
        self.pair.rejoin("B")
        self.assertEqual(self.asked[1][0], ["relaunch", "--session", self.tmp, "B"])

    def test_a_hung_relaunch_is_a_labelled_fail(self):
        import subprocess

        def hung(args, timeout):
            self.asked.append((args, timeout))
            if args[0] == "relaunch":
                raise subprocess.TimeoutExpired(args, timeout)
            return {"player": "B", "how": "quit asked"}
        self.pair.live = hung
        a = self.pair.c["A"]
        self.pair.begin(600)
        with self.assertRaises(AssertionError) as e:
            self.pair.rejoin("B")
        self.assertIn("live.py relaunch did not answer", str(e.exception))
        self.assertEqual(a.calls, ["resume", "pause"])

    def test_no_session_is_setup_fail(self):
        self.pair.session = None
        with self.assertRaises(P.SetupFail):
            self.pair.rejoin("B")


class RejoinEnvAndServerRestart(Rejoin):
    def test_rejoin_env_goes_to_relaunch_as_one_token_each(self):
        self.pair.live = self.live({"quit": {"player": "B", "how": "quit asked"}, "relaunch": {"ok": True, "boot": "played"}})
        self.pair.begin(600)
        self.pair.rejoin("B", env=["LICHII_COOP_SERVER=192.168.1.20:27520", "LICHII_COOP_PASSWORD="])
        self.assertEqual(self.asked[1][0], ["relaunch", "--session", self.tmp, "B",
                                            "--env=LICHII_COOP_SERVER=192.168.1.20:27520", "--env=LICHII_COOP_PASSWORD="])

    def test_server_restart_stop_then_start_with_args_both_games_run_meanwhile(self):
        def run(args, timeout):
            self.asked.append((args, timeout))
            return {"server": "stopped"} if args[3] == "stop" else {"server": "started", "out": "server-out-1.txt"}
        self.pair.live = run
        self.pair.begin(600)
        r = self.pair.server_restart(["--same-network-open", "--bind", "192.168.1.20"])
        self.assertEqual(r["out"], "server-out-1.txt")
        self.assertEqual([a for a, _ in self.asked], [
            ["server", "--session", self.tmp, "stop"],
            ["server", "--session", self.tmp, "start", "--server-arg=--same-network-open", "--server-arg=--bind",
             "--server-arg=192.168.1.20"]])
        for x in "AB":
            self.assertEqual(self.pair.c[x].calls, ["resume", "pause"])
        self.assertLessEqual(self.asked[0][1], P.Pair.SERVER_RESTART_S)

    def test_server_restart_start_refused_fails_and_still_holds_the_games(self):
        self.pair.live = lambda args, timeout: ({"server": "stopped"} if args[3] == "stop"
                                                else {"error": "the server still runs (pid 9)"})
        self.pair.begin(600)
        with self.assertRaises(AssertionError):
            self.pair.server_restart()
        for x in "AB":
            self.assertEqual(self.pair.c[x].calls, ["resume", "pause"])

    def test_server_restart_without_session_is_setup_fail(self):
        self.pair.session = None
        with self.assertRaises(P.SetupFail):
            self.pair.server_restart()


class LiveServerArgsAndRelaunchEnv(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, os.path.dirname(HERE))
        import live
        self.live = live

    def test_relaunch_env_after_the_sessions_own(self):
        r = {"port": 27520, "pipe": "p", "boot_timeout": 900, "env": ["LICHII_COOP_PASSWORD=x", "A:K=1"]}
        argv = self.live.relaunch_argv(r, "D", "B", env=["LICHII_COOP_PASSWORD="])
        envs = [argv[i + 1] for i, a in enumerate(argv) if a == "--env"]
        self.assertEqual(envs, ["LICHII_COOP_PASSWORD=x", "A:K=1", "LICHII_COOP_PASSWORD="])   # (the last wins)
        with self.assertRaises(SystemExit):
            self.live.relaunch_argv(r, "D", "B", env=["A:K=2"])

    def test_relaunch_parses_env_with_an_empty_value(self):
        seen = {}
        orig = self.live.relaunch
        try:
            self.live.relaunch = lambda a: seen.update(env=a.env) or {"ok": True}
            self.live.main(["relaunch", "--session", tempfile.mkdtemp(), "B", "--env=LICHII_COOP_PASSWORD=",
                            "--env", "LICHII_COOP_SERVER=10.0.0.5:27520"])
        except SystemExit:
            pass
        finally:
            self.live.relaunch = orig
        self.assertEqual(seen.get("env"), ["LICHII_COOP_PASSWORD=", "LICHII_COOP_SERVER=10.0.0.5:27520"])

    def test_server_start_args_this_start_only_after_the_sessions_own(self):
        d = tempfile.mkdtemp()
        started = []

        class Proc:
            pid = 77

        def popen(argv, **kw):
            started.append(argv)
            return Proc()
        names = ("open_session", "game_pids", "write_pids", "plan")
        orig = {n: getattr(self.live, n) for n in names}
        try:
            self.live.open_session = lambda d: {"runner": {"server": "S.exe", "port": 27520, "password": "pw",
                                                           "server_args": ["--own"]}}
            self.live.game_pids = lambda d: ({}, None)
            self.live.write_pids = lambda *a, **k: None
            self.live.plan = lambda *a, **k: None

            class A:
                session, action, server_arg = d, "start", ["--same-network-open", "--bind", "192.168.1.20"]
            r = self.live.server_cmd(A, popen=popen, sleep=lambda s: None)
            with open(os.path.join(d, r["out"]), "w"):
                pass
            A.server_arg = []
            r2 = self.live.server_cmd(A, popen=popen, sleep=lambda s: None)
        finally:
            for n, f in orig.items():
                setattr(self.live, n, f)
        self.assertEqual(started[0][-4:], ["--own", "--same-network-open", "--bind", "192.168.1.20"])
        self.assertNotIn("127.0.0.1", started[0])        # (its own --bind: hs.server_bind adds none)
        self.assertEqual(started[1][-3:], ["--own", "--bind", "127.0.0.1"])
        self.assertEqual((r["out"], r2["out"]), ("server-out-0.txt", "server-out-1.txt"))
        self.assertEqual(r["args"], ["--own", "--same-network-open", "--bind", "192.168.1.20"])


class Lan251(Rejoin):
    """test_lan251.run_lan251 against a fake live.py: the server-out files and game logs a real one would get."""

    def fake_live(self):
        self.server_args, self.out = [], None

        def run(args, timeout):
            self.asked.append((args, timeout))
            if args[0] == "server":
                if args[3] == "stop":
                    return {"server": "stopped"}
                self.server_args = [a.split("=", 1)[1] for a in args[4:]]
                self.out = "server-out-%d.txt" % len([a for a, _ in self.asked if a[0] == "server" and a[3] == "start"])
                with open(os.path.join(self.tmp, self.out), "w") as f:
                    f.write("listening\n")
                return {"server": "started", "out": self.out, "args": self.server_args}
            if args[0] == "quit":
                return {"player": args[3], "how": "quit asked"}
            x = args[3]
            env = dict(a[len("--env="):].split("=", 1) for a in args if a.startswith("--env="))
            srv = ""
            if "--bind" in self.server_args:                                         # R1
                srv = "a player on this network joins without the join password (same-network key, private address)\n"
                line = "[hos host] coop: welcomed as player 1 on %s\n" % env.get("LICHII_COOP_SERVER", "?")
                if self.bad == x:
                    srv = "refused a connection, reason 6: wrong join password\n"
            elif env.get("LICHII_COOP_PASSWORD") == "":                               # R2 B
                srv = "refused a connection, reason 6: wrong join password\n"
                line = "[hos host] coop: server refused: wrong join password: ask the host for it\n"
            else:
                line = "[hos host] coop: welcomed as player 1 on 127.0.0.1:27520\n"
            with open(os.path.join(self.tmp, self.out), "a") as f:
                f.write(srv)
            os.replace(self.logs[x], self.logs[x] + ".%d" % len(self.asked))
            with open(self.logs[x], "w") as f:
                f.write(line)
            return {"ok": True, "boot": "played"}
        return run

    def go(self, bad=None):
        sys.path.insert(0, INGAME)
        import test_lan251 as T
        self.bad = bad
        self.pair.live = self.fake_live()
        self.pair.in_play = lambda st: True
        self.shots = []
        self.pair.check_shot = lambda x, name: self.shots.append((x, name))
        self.pair.begin(3000)
        T.run_lan251(self.pair, "192.168.1.20", 27520, boot_a="a.txt", boot_b="b.txt")

    def test_pass_on_the_old_scripts_lines(self):
        self.go()
        starts = [a for a, _ in self.asked if a[0] == "server" and a[3] == "start"]
        self.assertEqual([s[4:] for s in starts], [
            ["--server-arg=--same-network-open", "--server-arg=--bind", "--server-arg=192.168.1.20"],
            ["--server-arg=--same-network-open"], []])
        relaunch = [a for a, _ in self.asked if a[0] == "relaunch"]
        self.assertEqual(len(relaunch), 6)
        self.assertIn("--env=LICHII_COOP_SERVER=192.168.1.20:27520", relaunch[0])
        self.assertIn("--env=LICHII_COOP_PASSWORD=", relaunch[1])
        self.assertEqual(relaunch[3], ["relaunch", "--session", self.tmp, "B", "--env=LICHII_COOP_PASSWORD="])
        self.assertFalse([a for a in relaunch[4] + relaunch[5] if a.startswith("--env")])   # (the session back)
        self.assertEqual(self.shots, [("A", "lan251-joined"), ("B", "lan251-joined")])

    def test_a_password_ask_on_the_lan_server_fails(self):
        with self.assertRaises(AssertionError) as e:
            self.go(bad="B")
        self.assertIn("R1", str(e.exception))

    def test_lan_ip_takes_a_private_address_only(self):
        sys.path.insert(0, INGAME)
        import test_lan251 as T
        self.assertEqual(T.lan_ip(["127.0.0.1", "169.254.3.4", "25.1.2.3", "172.20.0.9", "192.168.1.20"]), "172.20.0.9")
        self.assertIsNone(T.lan_ip(["127.0.0.1", "8.8.8.8", "172.32.0.1"]))
        self.assertTrue(T.same_network("10.1.2.3"))


class RejoinTestCollects(unittest.TestCase):
    def test_deselected_without_the_guest_switch(self):
        import subprocess
        out = tempfile.mkdtemp()
        p = subprocess.run([sys.executable, "-m", "pytest", "-p", "no:cacheprovider", "-q", "--fake", "--out", out,
                            "-k", "test_own_values_kept_over_rejoin", "."], cwd=INGAME, capture_output=True, text=True,
                           timeout=300)
        self.assertIn("needs_env B:LICHII_COOP_TEST_OWN_SAVE=0", p.stdout + p.stderr)


if __name__ == "__main__":
    unittest.main()
