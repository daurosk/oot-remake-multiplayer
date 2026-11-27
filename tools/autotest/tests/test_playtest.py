"""Tests for the live play-test kit's pieces that need no game: the desync checker, the MCP server's protocol, the
script parser's `dump` line, and live.py's lane set-up, watchdog (lane lock, maximum and idle end, clean-up), save
restore default and checkpoint screenshots. Run: python -I tools/autotest/tests/test_playtest.py"""
import argparse
import io
import json
import os
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import desync_check  # noqa: E402
import hookshot  # noqa: E402
import lanes  # noqa: E402
import live  # noqa: E402
import mcp_playtest  # noqa: E402
import playtest_client as pc  # noqa: E402
import playtest_helpers as ph  # noqa: E402


def write_checkpoint(d, tag, a_flags, b_flags, when):
    os.makedirs(os.path.join(d, "checkpoints"), exist_ok=True)
    path = os.path.join(d, "checkpoints", tag + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"tag": tag, "A": {"status": {"map": "ForestTemple"}, "flags": a_flags},
                   "B": {"status": {"map": "ForestTemple"}, "flags": b_flags}}, f)
    os.utime(path, (when, when))


class DesyncCheck(unittest.TestCase):
    def test_agree_then_first_desync_named(self):
        with tempfile.TemporaryDirectory() as d:
            now = time.time()
            write_checkpoint(d, "t1", {"F1": 0, "F2": 1}, {"F1": 0, "F2": 1}, now - 30)
            write_checkpoint(d, "t2", {"F1": 1, "F2": 1}, {"F1": 0, "F2": 1}, now - 20)
            write_checkpoint(d, "t3", {"F1": 1, "F2": 0}, {"F1": 0, "F2": 1}, now - 10)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 1)
            self.assertEqual(lines[0], "t1: agree (A ForestTemple, B ForestTemple)")
            self.assertIn("t2: 1 flag(s) differ", lines[1])
            self.assertIn("<- first desync", lines[1])
            self.assertEqual(lines[2], "    F1: A 1, B 0")
            self.assertEqual(len(lines), 3)  # stops at the first desync
            lines = []
            self.assertEqual(desync_check.check(d, show_all=True, out=lines.append), 1)
            self.assertTrue(any(ln.startswith("t3: 2 flag(s) differ") for ln in lines))

    def test_a_flag_one_game_cannot_read_is_no_desync(self):
        diff, note = desync_check.compare({"A": {"flags": {"F": 1, "G": 0}}, "B": {"flags": {"G": 0}}})
        self.assertEqual(diff, {})
        self.assertIsNone(note)
        diff, note = desync_check.compare({"A": {"flags": None, "error": "no dump"}, "B": {"flags": {}}})
        self.assertIn("no dump", note)

    def test_nothing_to_compare(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(desync_check.check(d, out=lambda s: None), 2)

    def test_no_agree_without_a_comparison(self):
        # A checkpoint where a game wrote no flags is not compared: alone it is "nothing compared" (2), never "agree".
        with tempfile.TemporaryDirectory() as d:
            now = time.time()
            write_checkpoint(d, "t1", None, {"F1": 0}, now - 20)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 2)
            self.assertIn("t1: not compared", lines[0])
            self.assertIn("nothing compared", lines[-1])
            # One agrees, another was not compared: incomplete (3), not agree.
            write_checkpoint(d, "t2", {"F1": 0}, {"F1": 0}, now - 10)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 3)
            self.assertIn("incomplete: 1 checkpoint(s) not compared", lines[-1])

    def test_shots_named_with_the_checkpoint(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "checkpoints"))
            with open(os.path.join(d, "checkpoints", "t1.json"), "w", encoding="utf-8") as f:
                json.dump({"tag": "t1", "A": {"status": {"map": "M"}, "flags": {"F": 1}, "shot": "t1-A.png"},
                           "B": {"status": {"map": "M"}, "flags": {"F": 1}, "shot": "t1-B.png"}}, f)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 0)
            self.assertEqual(lines, ["t1: agree (A M, B M; shots t1-A.png, t1-B.png)"])


class Mcp(unittest.TestCase):
    def run_server(self, requests):
        stdin, stdout = sys.stdin, sys.stdout
        sys.stdin = io.StringIO("".join(json.dumps(r) + "\n" for r in requests))
        sys.stdout = io.StringIO()
        try:
            mcp_playtest.serve("no-session")
            return [json.loads(ln) for ln in sys.stdout.getvalue().splitlines()]
        finally:
            sys.stdin, sys.stdout = stdin, stdout

    def test_handshake_tools_and_errors(self):
        out = self.run_server([
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "nope", "arguments": {}}},
            {"jsonrpc": "2.0", "id": 4, "method": "nope"},
        ])
        self.assertEqual([m["id"] for m in out], [1, 2, 3, 4])  # the notification is not answered
        self.assertEqual(out[0]["result"]["protocolVersion"], "2025-06-18")
        self.assertEqual([t["name"] for t in out[1]["result"]["tools"]],
                         ["state", "act", "shot", "checkpoint", "desync_check", "quit_game", "server", "relaunch",
                          "wait_log", "sleep"])
        self.assertTrue(out[2]["result"]["isError"])
        self.assertEqual(out[3]["error"]["code"], -32601)

    def test_desync_check_tool_says_incomplete(self):
        with tempfile.TemporaryDirectory() as d:
            now = time.time()
            write_checkpoint(d, "t1", None, {"F1": 0}, now - 20)
            write_checkpoint(d, "t2", {"F1": 0}, {"F1": 0}, now - 10)
            self.assertEqual(mcp_playtest.call(d, "desync_check", {})["result"], "incomplete")


class Parse(unittest.TestCase):
    def test_dump_is_a_module_step(self):
        steps = hookshot.parse("dump after_chest\n")
        self.assertEqual(steps[0].kind, "dump")
        self.assertIn("dump", hookshot.MODULE_ONLY)
        for bad in ("dump\n", "dump a b\n", "dump ../x\n"):
            with self.assertRaises(hookshot.ScriptError):
                hookshot.parse(bad)


def start_args(**kw):
    a = dict(session="s", lane=None, game_a=None, game_b=None, server="coop_server.exe", port=None, boot_a=None,
             boot_b=None, load=None, no_load=False, no_restore=False, env=[], boot_timeout=900, max_minutes=30,
             idle_minutes=10, lock_wait=0, only=None)
    a.update(kw)
    return argparse.Namespace(**a)


def write_session(d, **kw):
    s = {"started": 1000.0, "dir": d, "lane": None, "port": 27593, "booted": True, "max_s": 1800, "idle_s": 600,
         "lock_wait": 0, "offsets": {},
         "games": {x: {"pipe": "p-" + x, "commands": os.path.join(d, "commands-%s.txt" % x),
                       "log": os.path.join(d, "game-out-%s.txt" % x)} for x in live.TAGS}}
    s.update(kw)
    live.save(d, s)
    return s


class LiveLane(unittest.TestCase):
    def test_a_lane_gives_its_pair_port_and_pipes(self):
        L = lanes.lane(1)
        self.assertEqual(live.resolve(start_args(lane=1)), ({"A": L["a"], "B": L["b"]}, L["port"], "coop-lane1"))
        self.assertEqual(live.resolve(start_args(lane=1, port=27999))[1], 27999)
        self.assertEqual(live.resolve(start_args(game_a="ga", game_b="gb")),
                         ({"A": "ga", "B": "gb"}, live.PORT, live.PIPE))
        with self.assertRaises(SystemExit):
            live.resolve(start_args())

    def test_one_game_alone(self):
        # each game is its own lane: one game starts without the other, and without a server
        L = lanes.lane(1)
        self.assertEqual(live.resolve(start_args(game_b="gb"))[0], {"B": "gb"})
        self.assertEqual(live.resolve(start_args(lane=1, only="B"))[0], {"B": L["b"]})
        with self.assertRaises(SystemExit):
            live.resolve(start_args(game_a="ga", only="B"))
        argv = live.runner_argv(start_args(server=None, boot_a="a.txt", boot_b="b.txt"), {"B": "gb"}, 1, "p", "dir")
        self.assertEqual(argv[argv.index("--game-b") + 1], "gb")
        for flag in ("--game-a", "--server", "--script-a"):
            self.assertNotIn(flag, argv)
        self.assertIn("--script-b", argv)
        with tempfile.TemporaryDirectory() as d:
            s = write_session(d)
            del s["games"]["A"]
            live.save(d, s)
            self.assertEqual(live.session_tags(s), ["B"])
            self.assertIn("error", live.do(argparse.Namespace(session=d, player="A", script="press A")))

    def test_runner_restores_the_saves_and_never_takes_the_lane_itself(self):
        argv = live.runner_argv(start_args(lane=1), {"A": "ga", "B": "gb"}, 27520, "coop-lane1", "dir")
        self.assertNotIn("--no-restore", argv)   # the baseline save, as lane runs
        self.assertNotIn("--lane", argv)         # the watchdog holds the lock for the whole session
        self.assertIn("--keep-open", argv)
        self.assertEqual(argv[argv.index("--pipe") + 1], "coop-lane1")
        self.assertEqual(argv[argv.index("--port") + 1], "27520")
        self.assertEqual(argv[argv.index("--password") + 1], live.SERVER_PASSWORD)   # (kept in session.json too)
        self.assertIn("--no-restore", live.runner_argv(start_args(no_restore=True), {"A": "ga", "B": "gb"}, 1, "p", "dir"))

    def test_the_runner_keeps_pids_for_the_watchdog(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "pids.json"), "w", encoding="utf-8") as f:
                json.dump({"games": {"A": 11, "B": 12}, "server": 13}, f)
            self.assertEqual(live.game_pids(d), ({"A": 11, "B": 12}, 13))
            os.remove(os.path.join(d, "pids.json"))
            self.assertEqual(live.game_pids(d), ({}, None))


class LiveDll(unittest.TestCase):
    """The lane's module: lanes.py owns it (install, setup, swap_in/swap_back); live.py start --dll swaps for a session."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.kit = self.tmp.name
        self.dsts = []
        for x in ("A", "B"):
            b = os.path.join(self.kit, "ls", "lane1-%s" % x, "bin")
            os.makedirs(b)
            open(os.path.join(b, "longshot.exe"), "w").close()
            dst = os.path.join(b, "hookshot_coop.dll")
            self.write(dst, "lane own")
            self.dsts.append(dst)
        self.new = os.path.join(self.kit, "new.dll")
        self.write(self.new, "session")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, p, text):
        with open(p, "w") as f:
            f.write(text)

    def read(self, p):
        with open(p) as f:
            return f.read()

    def lane(self):
        return [self.read(d) if os.path.exists(d) else None for d in self.dsts]

    def swap(self, s, dll=None):
        return live.install_dll(s, dll or self.new, swap=lambda n, p: lanes.swap_in(n, p, kit=self.kit,
                                                                                    busy=lambda _f: False))

    def back(self, s, **kw):
        return live.restore_dlls(s, swap_back=lambda n: lanes.swap_back(n, kit=self.kit, **kw))

    def test_swap_and_restore(self):
        s = {"lane": 1}
        self.swap(s)
        self.assertEqual(self.lane(), ["session", "session"])
        self.assertEqual([self.read(d + live.BACKUP) for d in self.dsts], ["lane own", "lane own"])
        self.assertEqual(len(lanes.swap_left(1, kit=self.kit)), 2)
        self.assertEqual(self.back(s), "restored, restored")
        self.assertEqual(self.lane(), ["lane own", "lane own"])
        self.assertEqual(lanes.swap_left(1, kit=self.kit), [])

    def test_a_left_backup_is_the_lane_own(self):
        # a session that could not put it back left its backup: the next one keeps it (the lane's own), not the test dll
        for d in self.dsts:
            self.write(d + live.BACKUP, "lane own")
            self.write(d, "left over")
        s = {"lane": 1}
        self.swap(s)
        self.back(s)
        self.assertEqual(self.lane(), ["lane own", "lane own"])

    def test_an_install_after_a_failed_restore_wins(self):
        # review-01112-h 1: the backup stayed (a game held the file), then lanes.py install put a newer module in. The
        # newer one is the lane's own now: no later session may put the old backup back.
        for d in self.dsts:
            self.write(d + live.BACKUP, "old own")
            self.write(d, "left over")
        newer = os.path.join(self.kit, "newer.dll")
        self.write(newer, "newer")
        lanes.install(1, newer, say=lambda *_: None, kit=self.kit, busy=lambda _f: False)
        self.assertEqual(lanes.swap_left(1, kit=self.kit), [])
        s = {"lane": 1}
        self.swap(s)
        self.back(s)
        self.assertEqual(self.lane(), ["newer", "newer"])

    def test_a_lane_with_no_module_gets_none_back(self):
        # review-01112-h 2: the lane had no module: the session's is removed at its end, not left behind
        for d in self.dsts:
            os.unlink(d)
        s = {"lane": 1}
        self.swap(s)
        self.assertEqual(self.lane(), ["session", "session"])
        self.assertEqual(self.back(s), "removed (the lane had none), removed (the lane had none)")
        self.assertEqual(self.lane(), [None, None])
        self.assertEqual(lanes.swap_left(1, kit=self.kit), [])

    def test_a_held_file_is_tried_again(self):
        s = {"lane": 1}
        self.swap(s)
        calls = {"n": 0}
        real = os.replace

        def flaky(a, b):   # the killed game holds the module for two tries
            calls["n"] += 1
            if calls["n"] <= 2:
                raise PermissionError("in use")
            return real(a, b)
        slept = []
        os.replace = flaky
        try:
            words = self.back(s, sleep=slept.append)
        finally:
            os.replace = real
        self.assertEqual(words, "restored, restored")
        self.assertEqual(len(slept), 2)
        self.assertEqual(self.lane(), ["lane own", "lane own"])

    def test_still_held_says_so_and_check_names_it(self):
        s = {"lane": 1}
        self.swap(s)
        real = os.replace

        def held(a, b):
            raise PermissionError("in use")
        os.replace = held
        try:
            words = self.back(s, tries=3, sleep=lambda _s: None)
        finally:
            os.replace = real
        self.assertIn("NOT restored", words)
        self.assertIn("lanes.py restore --lane 1", words)
        found = lanes.problems(1, kit=self.kit, busy=lambda _f: False)
        self.assertTrue(any("was not put back" in p for p in found), found)
        lanes.swap_back(1, kit=self.kit)   # (lanes.py restore)
        self.assertEqual(self.lane(), ["lane own", "lane own"])

    def test_setup_drops_a_left_swap(self):
        src = {}
        for x in ("A", "B"):
            src[x] = os.path.join(self.kit, "src-%s" % x, "bin")
            os.makedirs(src[x])
            open(os.path.join(src[x], "longshot.exe"), "w").close()
            self.write(os.path.join(src[x], "hookshot_coop.dll"), "source")
        for d in self.dsts:
            self.write(d + live.BACKUP, "old own")
        lanes.setup(1, os.path.dirname(src["A"]), os.path.dirname(src["B"]), say=lambda *_: None, kit=self.kit,
                    busy=lambda _f: False)
        self.assertEqual(lanes.swap_left(1, kit=self.kit), [])
        self.assertEqual(self.lane(), ["source", "source"])

    def test_missing_dll_changes_nothing(self):
        s = {"lane": 1}
        with self.assertRaises(lanes.Fail):
            self.swap(s, os.path.join(self.kit, "none.dll"))
        self.assertEqual(self.lane(), ["lane own", "lane own"])
        self.assertEqual(self.back(s), "nothing to put back, nothing to put back")

    def test_dll_needs_lane(self):
        a = argparse.Namespace(dll=self.new, lane=None, session=os.path.join(self.kit, "s"))
        with self.assertRaises(SystemExit):
            live.start(a)


RUNNER = {"game_a": "ga", "game_b": "gb", "port": 27520, "pipe": "coop-lane1", "server": "srv.exe",
          "env": ["LICHII_COOP_DEBUG_FLAGS=1", "B:X=1"], "display": "D2", "boot_timeout": 900, "playtest": 5010,
          "password": "pw-of-session"}


class FakeProc:
    def __init__(self, pid):
        self.pid = pid


class LiveLifecycle(unittest.TestCase):
    """quit / server / relaunch: a game or the server closed and started again inside one session."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name
        write_session(self.d, runner=dict(RUNNER), planned={})
        self.pids({"A": 11, "B": 22}, 33)
        self.live = {11, 22, 33}

    def tearDown(self):
        self.tmp.cleanup()

    def pids(self, games, server=None):
        with open(os.path.join(self.d, "pids.json"), "w", encoding="utf-8") as f:
            json.dump({"games": games, "server": server}, f)

    def alive(self, p):
        return p in self.live

    def args(self, **kw):
        return argparse.Namespace(session=self.d, **kw)

    def test_a_planned_exit_does_not_end_the_session(self):
        out = live.quit_one(self.args(player="B", kill=True), alive=self.alive, killer=self.live.discard,
                            sleep=lambda s: None)
        self.assertEqual(out["how"], "killed")
        self.assertEqual(live.game_pids(self.d)[0], {"A": 11})
        self.assertEqual(live.load(self.d)["planned"], {"B": "killed"})
        c = FakeClock(1000.0)

        def sleep(s):   # B's pid is gone and planned: only the stop request ends the watch
            c.sleep(s)
            open(live.stop_file(self.d), "w").close()
        live.touch(self.d)
        self.assertEqual(live.watch(self.d, clock=c, sleep=sleep, alive=self.alive), "stop asked")

    def test_an_unplanned_exit_still_ends_it(self):
        self.live.discard(22)
        c = FakeClock(1000.0)
        live.touch(self.d)
        self.assertEqual(live.watch(self.d, clock=c, sleep=c.sleep, alive=self.alive), "game B ended")

    def test_quit_asks_first_then_kills(self):
        asked = []
        c = FakeClock(0.0)
        out = live.quit_one(self.args(player="A", kill=False), alive=self.alive, quit=lambda s, x: asked.append(x),
                            killer=self.live.discard, wait=5, clock=c, sleep=c.sleep)
        self.assertEqual(asked, ["A"])
        self.assertEqual(out["how"], "quit asked, killed after 5 s")
        self.assertNotIn(11, self.live)

    def test_server_stop_and_start(self):
        out = live.server_cmd(self.args(action="stop"), alive=self.alive, killer=self.live.discard)
        self.assertEqual((out["server"], out["pid"]), ("stopped", 33))
        self.assertIsNone(live.game_pids(self.d)[1])
        self.assertEqual(live.load(self.d)["planned"], {"server": "stopped"})
        seen = {}

        def popen(cmd, **kw):
            seen["cmd"], seen["env"] = cmd, kw["env"]
            return FakeProc(44)
        out = live.server_cmd(self.args(action="start"), alive=self.alive, popen=popen, sleep=lambda s: None)
        self.assertEqual(out["pid"], 44)
        self.assertEqual(seen["cmd"], ["srv.exe", "--port", "27520", "--password-env", "OOT_COOP_TEST_PASSWORD", "--bind", "127.0.0.1"])
        self.assertEqual(seen["env"]["OOT_COOP_TEST_PASSWORD"], "pw-of-session")   # (the session's, review-01112-h 3)
        self.assertEqual(live.game_pids(self.d), ({"A": 11, "B": 22}, 44))
        self.assertEqual(live.load(self.d)["planned"], {})

    def test_server_start_refused_while_it_runs(self):
        out = live.server_cmd(self.args(action="start"), alive=self.alive, popen=lambda *a, **k: 1 / 0)
        self.assertIn("still runs", out["error"])

    def test_relaunch_argv_keeps_the_session_settings(self):
        argv = live.relaunch_argv(RUNNER, self.d, "B", boot="boot.B.txt", no_load=True)
        self.assertEqual(argv[:3], ["run", "--only", "B"])
        for pair in (["--port", "27520"], ["--pipe", "coop-lane1"], ["--out", self.d], ["--script-b", "boot.B.txt"],
                     ["--playtest-ports", "5010"], ["--display", "D2"], ["--password", "pw-of-session"]):
            i = argv.index(pair[0])
            self.assertEqual(argv[i:i + 2], pair)
        self.assertEqual([argv[i + 1] for i, w in enumerate(argv) if w == "--env"], RUNNER["env"])
        self.assertIn("--no-restore", argv)
        self.assertIn("--no-load", argv)
        self.assertNotIn("--server", argv)   # (the game joins the session's server; none is started)
        self.assertNotIn("--no-restore", live.relaunch_argv(RUNNER, self.d, "A", restore=True))

    def test_relaunch_refused_while_the_game_runs(self):
        out = live.relaunch(self.args(player="A", boot=None, no_load=False, restore=False), alive=self.alive,
                            runner=lambda argv: 1 / 0)
        self.assertIn("still runs", out["error"])

    def test_relaunch_keeps_the_old_log_and_merges_the_pid(self):
        live.quit_one(self.args(player="B", kill=True), alive=self.alive, killer=self.live.discard,
                      sleep=lambda s: None)
        log = os.path.join(self.d, "game-out-B.txt")
        with open(log, "w") as f:
            f.write("first run")

        def runner(argv):   # as hookshot.py run --only B does: B's new pid added to pids.json
            p = live.read_json(os.path.join(self.d, "pids.json"))
            p["games"]["B"] = 55
            live.write_json(os.path.join(self.d, "pids.json"), p)
            return 0
        s = live.load(self.d)
        s["offsets"] = {"A": 7, "B": 99}
        live.save(self.d, s)
        out = live.relaunch(self.args(player="B", boot=None, no_load=False, restore=False), alive=self.alive,
                            runner=runner)
        self.assertTrue(out["ok"])
        self.assertEqual(live.game_pids(self.d), ({"A": 11, "B": 55}, 33))
        with open(log + ".1") as f:
            self.assertEqual(f.read(), "first run")
        s = live.load(self.d)
        self.assertEqual(s["offsets"], {"A": 7, "B": 0})
        self.assertEqual(s["planned"], {})

    def test_relaunch_answers_a_held_old_log_as_its_error(self):
        # downed-move-53d40ab3-2: os.replace WinError 32 on game-out-B.txt was a traceback ('no JSON answer'); now the
        # rotate's reason is relaunch's JSON error, nothing is started and the plan is cleared
        live.quit_one(self.args(player="B", kill=True), alive=self.alive, killer=self.live.discard,
                      sleep=lambda s: None)
        out = live.relaunch(self.args(player="B", boot=None, no_load=False, restore=False), alive=self.alive,
                            runner=lambda argv: 1 / 0, rotate=lambda log: "game-out-B.txt still held")
        self.assertEqual(out, {"error": "game-out-B.txt still held"})
        self.assertEqual(live.load(self.d)["planned"], {})

    def test_rotate_log_waits_for_a_held_file_then_moves_it(self):
        log = os.path.join(self.d, "game-out-B.txt")
        with open(log, "w") as f:
            f.write("first run")
        c, tries = FakeClock(0.0), []

        def replace(src, dst):
            tries.append(dst)
            if len(tries) < 3:
                raise PermissionError(32, "The process cannot access the file because it is being used by another process")
            os.replace(src, dst)
        self.assertIsNone(live.rotate_log(log, replace=replace, sleep=c.sleep, clock=c))
        self.assertEqual(len(tries), 3)
        with open(log + ".1") as f:
            self.assertEqual(f.read(), "first run")
        self.assertIsNone(live.rotate_log(log + ".missing"))      # (no old log: nothing to move)

    def test_rotate_log_gives_up_with_its_reason(self):
        log = os.path.join(self.d, "game-out-B.txt")
        with open(log, "w") as f:
            f.write("held")
        c = FakeClock(0.0)

        def replace(src, dst):
            raise PermissionError(32, "being used by another process")
        why = live.rotate_log(log, held_s=3.0, replace=replace, sleep=c.sleep, clock=c)
        self.assertIn("game-out-B.txt still held by another process 3 s after the game ended", why)
        self.assertTrue(os.path.exists(log))

    def test_relaunch_stdout_is_only_its_json(self):
        # C #132 rule (review-c123-11): the runner's boot lines go to stderr; stdout = the ONE JSON answer _live reads
        import contextlib
        import io
        self.live.discard(22)
        live.plan(self.d, "B", "killed")

        def runner(argv):
            print("hookshot.py run: boot line on stdout")
            return 0
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            r = live.relaunch(self.args(player="B", boot=None, no_load=False, restore=False), alive=self.alive,
                              runner=runner)
        self.assertTrue(r["ok"])
        self.assertEqual(out.getvalue(), "")
        self.assertIn("boot line on stdout", err.getvalue())

    def test_a_failed_relaunch_stays_planned(self):
        self.live.discard(22)
        live.plan(self.d, "B", "killed")
        out = live.relaunch(self.args(player="B", boot=None, no_load=False, restore=False), alive=self.alive,
                            runner=lambda argv: 1)
        self.assertFalse(out["ok"])
        self.assertEqual(live.load(self.d)["planned"], {"B": "relaunch"})

    def test_older_session_says_so(self):
        write_session(self.d)
        with self.assertRaises(SystemExit):
            live.server_cmd(self.args(action="stop"))


class LiveWaitLog(unittest.TestCase):
    """wait_log and the lifecycle tools as session tools (scripts: tools/playtest/scripts/leave-solo-273.jsonl)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name
        write_session(self.d, runner=dict(RUNNER), planned={})
        with open(os.path.join(self.d, "game-out-B.txt"), "w") as f:
            f.write("coop: save: the host: this game uses its own saves\nother\ncoop: null read at 0x1\n")

    def tearDown(self):
        self.tmp.cleanup()

    def wait(self, pattern, mn=1, mx=None, timeout=5):
        c = FakeClock(0.0)
        return live.wait_log(self.d, "B", pattern, mn, mx, timeout, clock=c, sleep=c.sleep)

    def test_found(self):
        out = self.wait("the host: this game uses its own saves")
        self.assertEqual((out["ok"], out["count"]), (True, 1))

    def test_missing_times_out(self):
        out = self.wait("now playing from the co-op save folder")
        self.assertFalse(out["ok"])
        self.assertIn("0 of 1", out["why"])

    def test_none_allowed(self):
        out = self.wait("unhandled exception|fault at|coop: null read", 0, 0, 0)
        self.assertFalse(out["ok"])
        self.assertEqual(self.wait("unhandled exception|fault at", 0, 0, 0)["ok"], True)

    def test_a_failed_session_tool_stops_a_script(self):
        with self.assertRaises(RuntimeError):
            mcp_playtest.call(self.d, "wait_log", {"player": "B", "pattern": "never there", "timeout_s": 0})
        self.assertEqual(mcp_playtest.call(self.d, "wait_log", {"player": "B", "pattern": "own saves",
                                                                "timeout_s": 0})["count"], 1)

    def test_the_tools_are_listed_for_scripts(self):
        names = {t["name"] for t in mcp_playtest.TOOLS}
        self.assertLessEqual({"quit_game", "server", "relaunch", "wait_log", "sleep"}, names)


class HookshotOnly(unittest.TestCase):
    def test_only_needs_no_script_a(self):
        with self.assertRaises(SystemExit):
            hookshot.main(["run", "--game-a", "a", "--game-b", "b", "--out", "o"])   # neither --script-a nor --only


class FakeClock:
    def __init__(self, t):
        self.t = t

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class LiveWatchdog(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def pids(self, games, server=None):
        with open(os.path.join(self.d, "pids.json"), "w", encoding="utf-8") as f:
            json.dump({"games": games, "server": server}, f)

    def test_maximum_duration_ends_the_session(self):
        write_session(self.d, idle_s=0)
        c = FakeClock(1000.0)
        self.assertIn("maximum duration (30 min)", live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True))
        self.assertGreaterEqual(c.t, 1000.0 + 1800)

    def test_maximum_minutes_0_means_no_limit(self):
        # 0 must not end the session at once: only a stop request (here after a simulated day) does.
        write_session(self.d, max_s=0, idle_s=0)
        c = FakeClock(1000.0)

        def sleep(s):
            c.sleep(s)
            if c.t > 1000.0 + 86400:
                open(live.stop_file(self.d), "w").close()
        self.assertEqual(live.watch(self.d, clock=c, sleep=sleep, alive=lambda p: True), "stop asked")
        self.assertGreater(c.t, 1000.0 + 86400)

    def test_no_limits_say_so_in_the_answer(self):
        self.assertEqual(live.ends_text(0, 0), "never by itself (live.py stop, or a game that ended)")
        self.assertEqual(live.ends_text(30, 10), "after 30 min, or 10 min without a command (live.py stop, or a game that ended)")
        self.assertEqual(live.ends_text(0, 5), "5 min without a command (live.py stop, or a game that ended)")

    def test_idle_ends_only_after_the_boot(self):
        write_session(self.d, booted=False)
        live.touch(self.d)
        os.utime(live.active_file(self.d), (1000.0, 1000.0))
        c = FakeClock(1000.0)
        # still booting: no idle end, only the maximum
        self.assertIn("maximum", live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True))
        write_session(self.d, booted=True)
        c = FakeClock(1000.0)
        self.assertEqual(live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True),
                         "no tester command for 10 min")

    def test_stop_request_and_a_game_that_ended(self):
        write_session(self.d)
        live.touch(self.d)
        self.pids({"A": 11, "B": 12})
        c = FakeClock(time.time())
        write_session(self.d, started=c.t)
        self.assertEqual(live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: p != 12), "game B ended")
        open(live.stop_file(self.d), "w").close()
        self.assertEqual(live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True), "stop asked")

    def test_close_quits_kills_what_stays_and_lets_the_lane_go(self):
        s = write_session(self.d, lane=1)
        self.pids({"A": 11, "B": 12}, server=13)
        quits, kills, released = [], [], []
        c = FakeClock(0.0)
        out = live.close(self.d, s, alive=lambda p: p in (12, 13) and p not in kills,
                         quit=lambda s, x: quits.append(x), killer=kills.append,
                         release=lambda n, o, g=None: released.append((n, o)) or True, clock=c, sleep=c.sleep)
        self.assertEqual(quits, ["A", "B"])
        self.assertEqual(kills, [12, 13])   # B did not quit; the server always goes
        self.assertEqual(released, [(1, live.lock_owner(self.d))])
        self.assertEqual(out["B"], "quit asked, killed")
        self.assertEqual(out["lock"], "lane1.lock released")

    def test_close_without_a_lane_releases_nothing(self):
        s = write_session(self.d)
        released = []

        def unreachable(s, x):
            raise OSError("no pipe")
        out = live.close(self.d, s, alive=lambda p: False, quit=unreachable, killer=lambda p: None,
                         release=lambda n, o, g=None: released.append(n), wait=0)
        self.assertEqual(released, [])
        self.assertIn("not reached", out["A"])

    def test_lane_lock_taken_held_and_released_also_on_an_error(self):
        write_session(self.d, lane=1, lock_wait=5)
        taken, closed = [], []

        def boom(d):
            w = live.read_json(live.watchdog_file(d))
            self.assertEqual((w["lock"], w["pid"], w["ended"]), ("held", 4242, None))
            raise RuntimeError("bridge gone")
        code = live.run_watchdog(self.d, acquire=lambda n, o, wait, g=None: taken.append((n, o, wait)) or True,
                                 watcher=boom, closer=lambda d, s: closed.append(s["lane"]) or {"lock": "released"},
                                 pid=4242)
        self.assertEqual(code, 0)
        self.assertEqual(taken, [(1, live.lock_owner(self.d), 5)])
        self.assertEqual(closed, [1])
        w = live.read_json(live.watchdog_file(self.d))
        self.assertIn("watchdog error: RuntimeError: bridge gone", w["ended"])
        self.assertEqual(w["closed"], {"lock": "released"})

    def test_a_signal_still_cleans_up(self):
        write_session(self.d)
        closed = []

        def signalled(d):
            raise SystemExit("signal 15")
        live.run_watchdog(self.d, watcher=signalled, closer=lambda d, s: closed.append(1) or {}, pid=1)
        self.assertEqual(closed, [1])
        self.assertIn("watchdog stopped (signal 15)", live.ended(self.d))

    def test_a_busy_lane_refuses_the_session(self):
        write_session(self.d, lane=1)
        closed = []
        code = live.run_watchdog(self.d, acquire=lambda n, o, wait, g=None: False, watcher=lambda d: "x",
                                 closer=lambda d, s: closed.append(1), pid=1)
        self.assertEqual(code, 1)
        self.assertEqual(closed, [])   # nothing started, nothing to close
        w = live.read_json(live.watchdog_file(self.d))
        self.assertEqual(w["lock"], "refused")
        self.assertIn("lane 1 is not free", w["ended"])

    def test_commands_refused_after_the_end(self):
        write_session(self.d)
        live.write_json(live.watchdog_file(self.d),
                        {"pid": 1, "lock": "none", "ended": "maximum duration (30 min) reached"})
        with self.assertRaises(SystemExit) as e:
            live.state(argparse.Namespace(session=self.d, player=None))
        self.assertIn("maximum duration", str(e.exception))

    def test_stop_without_a_watchdog_closes_here(self):
        write_session(self.d)
        live.write_json(live.watchdog_file(self.d), {"pid": 999999, "lock": "none", "ended": None})
        old = live.close
        live.close = lambda d, s, alive=None: {"A": "quit asked"}
        try:
            out = live.stop(argparse.Namespace(session=self.d), alive=lambda p: False)
        finally:
            live.close = old
        self.assertIn("without the watchdog", out["ended"])
        self.assertTrue(os.path.exists(live.stop_file(self.d)))


class FakeBridge:
    def __init__(self, x, shots):
        self.x, self.shots = x, shots

    def request(self, cmd, **kw):
        if cmd == "shot":
            self.shots.append((self.x, kw["path"]))
            return {}
        return {"map": "ForestTemple", "game": {}}

    def close(self):
        pass


class LiveCheckpoint(unittest.TestCase):
    def test_a_checkpoint_has_a_shot_of_each_game(self):
        with tempfile.TemporaryDirectory() as d:
            write_session(d)
            shots = []

            class Player:
                def __init__(self, b, log, commands=None, **kw):
                    self.commands = commands

                def play(self, steps):
                    with open(self.commands + ".dump-%s.json" % steps[0].name, "w", encoding="utf-8") as f:
                        json.dump({"flags": {"F": 1}}, f)
            saved = live.bridge, hookshot.Player, hookshot.Log
            live.bridge = lambda s, x: FakeBridge(x, shots)
            hookshot.Player = Player
            hookshot.Log = lambda path: None
            try:
                out = live.checkpoint(argparse.Namespace(session=d, tag="after_chest"))
            finally:
                live.bridge, hookshot.Player, hookshot.Log = saved
            cps = os.path.join(d, "checkpoints")
            self.assertEqual(shots, [("A", os.path.join(cps, "after_chest-A.png")),
                                     ("B", os.path.join(cps, "after_chest-B.png"))])
            self.assertEqual(out["A"]["shot"], os.path.join(cps, "after_chest-A.png"))
            cp = live.read_json(os.path.join(cps, "after_chest.json"))
            self.assertEqual(cp["B"]["shot"], os.path.join(cps, "after_chest-B.png"))
            self.assertEqual(cp["A"]["flags"], {"F": 1})


class LiveMenu(unittest.TestCase):
    """live.py state's `menu`: co-op's pages as text from the play-test bridge (a bridge shot leaves them out)."""

    class Fake:
        def __init__(self, reply=None, error=None):
            self.reply, self.error, self.asked, self.closed = reply, error, [], False

        def request(self, cmd):
            self.asked.append(cmd)
            if self.error:
                raise self.error
            return self.reply

        def close(self):
            self.closed = True

    def test_the_menu_member_of_the_state(self):
        menu = {"coop": {"open": True, "page": "customize"}, "customize_screen": {"open": False}}
        f = self.Fake({"ok": True, "frame": 3, "menu": menu})
        self.assertEqual(live.menu_of(5001, f), {"menu": menu})
        self.assertEqual(f.asked, ["state"])
        self.assertTrue(f.closed)

    def test_an_older_module_says_so(self):
        self.assertIn("no menu", live.menu_of(5001, self.Fake({"ok": True}))["menu_error"])

    def test_no_bridge_is_an_error_not_a_crash(self):
        f = self.Fake(error=ConnectionRefusedError("refused"))
        self.assertIn("refused", live.menu_of(5001, f)["menu_error"])
        self.assertTrue(f.closed)

    def test_a_game_without_the_bridge_is_not_asked(self):
        with tempfile.TemporaryDirectory() as d:
            s = {"games": {"A": {}}}
            asked = []
            orig = (live.bridge, live.snapshot, live.new_lines, live.menu_of)
            try:
                live.bridge = lambda s, x: type("B", (), {"close": lambda self: None})()
                live.snapshot = lambda b: {"map": "m"}
                live.new_lines = lambda d, s, x: []
                live.menu_of = lambda port: asked.append(port) or {"menu": {}}
                self.assertNotIn("menu", live.look(d, s, "A"))
                s["games"]["A"]["playtest_port"] = 5011
                self.assertEqual(live.look(d, s, "A")["menu"], {})
                self.assertEqual(asked, [5011])
            finally:
                live.bridge, live.snapshot, live.new_lines, live.menu_of = orig


class FakeTalk:
    """A game seen through the bridge for `talk`: an NPC whose talk is a list of boxes ("box", speaker, text) and
    choices ("choice", question, [options]). A shows the next one; Down moves the choice cursor (from 0). The texts go
    in a `text` log as patch 0041's wrap keeps them (one entry per pane set, the same text again not kept), laid out as
    a lane run read them: MessageDialog_00 (T_Name_00 the speaker), the reply buttons PaMessageBtn_00 (one pane,
    T_Btn_00, set once per button). `body` False: the box text does not come through the wrap (as in that run)."""

    def __init__(self, talk, hooked=True, verb=True, prompt="Talk", body=True, body_pane=None, stall=False,
                 name_after=False):
        self.talk, self.hooked, self.verb, self.prompt, self.body = talk, hooked, verb, prompt, body
        self.stall = stall               # (the last box stays up: A does not end the event)
        self.name_after = name_after     # (the body is set before the name pane, as the shop does)
        self.body_pane = body_pane   # (a pane name: the box text comes through patch 0042's wrap 26 in that pane)
        self.at, self.event, self.cursor, self.picked = None, False, 0, []
        self.frame, self.texts, self.shown, self.presses = 0, [], {}, []
        self.texts.append(self._entry("HUD_00", "T_Rupee_00", "5"))   # (a text before the talk: not a box)

    def _entry(self, layout, pane, text, wrap=25):
        return {"seq": len(self.texts) + 1, "frame": self.frame, "layout": layout, "pane": pane, "text": text,
                "length": len(text), "cut": False, "markup": False, "wrap": wrap}

    def _set(self, pane, text, layout="MessageDialog_00", wrap=25):
        if self.shown.get((layout, pane)) != text:
            self.shown[(layout, pane)] = text
            self.texts.append(self._entry(layout, pane, text, wrap))

    def _box(self, text):
        if self.body_pane:
            self._set(self.body_pane, text, wrap=26)
        elif self.body:
            self._set("T_MessageWhiteOut_00", text)

    def _show(self):
        item = self.talk[self.at]
        if item[0] == "box":
            if self.name_after:
                self._box(item[2])
            if item[1] is not None:
                self._set("T_Name_00", item[1])
            if not self.name_after:
                self._box(item[2])
        else:
            self._box(item[1])
            for option in item[2]:
                self._set("T_Btn_00", option, "PaMessageBtn_00")
            self.cursor = 0

    def state(self):
        return {"ok": True, "scene": "Kokiri", "loading": False, "event": self.event,
                "prompt": None if self.event else self.prompt}

    def step(self, frames=1):
        self.frame += frames
        return self.state()

    def input(self, stick=None, buttons=(), frames=1, rstick=None):
        self.presses.extend(buttons)
        self.frame += frames
        if "DOWN" in buttons and self.event and self.talk[self.at][0] == "choice":
            self.cursor = min(self.cursor + 1, len(self.talk[self.at][2]) - 1)
        if "A" in buttons:
            if not self.event and self.prompt:
                self.event, self.at = True, 0
                self._show()
            elif self.event:
                if self.talk[self.at][0] == "choice":
                    self.picked.append(self.talk[self.at][2][self.cursor])
                self.at += 1
                if self.at < len(self.talk):
                    self._show()
                elif self.stall:
                    self.at -= 1
                else:
                    self.event = False
                    self._box("")
        return {"ok": True}

    def text(self, since=0, layout=None):
        if not self.verb:
            raise pc.BridgeError({"ok": False, "error": "bad_request", "message": "cmd: hello, ..."})
        texts = [t for t in self.texts if t["seq"] > since and (not layout or layout in t["layout"])] if self.hooked else []
        return {"ok": True, "hooked": self.hooked, "last": len(self.texts) if self.hooked else 0, "lost": False,
                "texts": texts}


# Placeholders only: no text from the game (AGENTS.md).
SHOP = [("box", "Keeper", "Line 1"), ("choice", "Item: 40", ["Pick one", "Pick two"]),
        ("choice", "Sure?", ["Yes", "No"]), ("box", "Keeper", "Line 2")]


def tag(group, kind, params=b""):
    """A markup tag as the game's text holds it: 0x0e, then group, type and size (16-bit little-endian), then the
    params (one character a byte, as the bridge's `text` shows them)."""
    n = len(params)
    head = bytes([0x0e, group & 255, group >> 8, kind & 255, kind >> 8, n & 255, n >> 8])
    return (head + bytes(params)).decode("latin-1")


class TalkTest(unittest.TestCase):
    def test_reads_every_box_with_its_speaker_and_ends(self):
        g = FakeTalk([("box", "Speaker A", "Line 1"), ("box", "Speaker A", "Line 2")])
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual([(b["speaker"], b["text"]) for b in r["boxes"]],
                         [("Speaker A", "Line 1"), ("Speaker A", "Line 2")])
        self.assertEqual(r["speakers"], ["Speaker A"])
        self.assertFalse(g.event)
        self.assertNotIn("5", [b["text"] for b in r["boxes"]])     # (the HUD's text before the talk)

    def test_without_the_body_text_it_still_ends_with_the_speakers(self):
        g = FakeTalk([("box", "Speaker A", "Line 1"), ("box", "Speaker A", "Line 2"), ("box", "Speaker B", "x")],
                     body=False)
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual(r["boxes"], [])
        self.assertEqual(r["speakers"], ["Speaker A", "Speaker B"])
        self.assertFalse(g.event)

    def test_a_box_from_the_body_wrap_is_read_whatever_its_pane(self):
        g = FakeTalk([("box", "Speaker A", "Line 1"), ("box", "Speaker A", "Line 2")], body_pane="T_Pane_X")
        g._set("T_Pane_Y", "Not a box", layout="HUD_00", wrap=26)    # (a body-wrap text outside a message layout)
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual([(b["speaker"], b["text"]) for b in r["boxes"]],
                         [("Speaker A", "Line 1"), ("Speaker A", "Line 2")])

    def test_picks_each_reply_by_its_text_in_order(self):
        g = FakeTalk(SHOP)
        r = ph.talk(g, reply=["pick TWO", "Yes"])
        self.assertTrue(r["done"], r)
        self.assertEqual(g.picked, ["Pick two", "Yes"])
        self.assertEqual(r["picked"], [{"text": "Pick two", "index": 1}, {"text": "Yes", "index": 0}])
        self.assertEqual(g.presses.count("DOWN"), 1)
        self.assertEqual(r["boxes"][-1]["text"], "Line 2")

    def test_choices_with_no_reply_left_stop_and_list_them(self):
        g = FakeTalk(SHOP)
        r = ph.talk(g)
        self.assertFalse(r["done"])
        self.assertIn("no reply is left", r["result"])
        self.assertEqual(r["choices"], ["Pick one", "Pick two"])
        self.assertEqual(g.picked, [])                     # (nothing bought blindly)
        self.assertTrue(g.event)                           # (left open for the tester to decide)
        self.assertIsNotNone(r["since"])
        # going on from there: the same buttons read again, no new A first
        r2 = ph.talk(g, reply="Pick one", since=r["since"])
        self.assertFalse(r2["pressed"])
        self.assertEqual(r2["picked"], [{"text": "Pick one", "index": 0}])
        self.assertIn("no reply is left", r2["result"])
        self.assertEqual(r2["choices"], ["Yes", "No"])
        r3 = ph.talk(g, reply="No", since=r2["since"])
        self.assertTrue(r3["done"], r3)
        self.assertEqual(g.picked, ["Pick one", "No"])

    def test_a_reply_not_offered_stops_and_lists_the_choices(self):
        g = FakeTalk(SHOP)
        r = ph.talk(g, reply="Other")
        self.assertFalse(r["done"])
        self.assertIn("reply not offered", r["result"])
        self.assertIn("'Pick one'", r["result"])
        self.assertEqual(g.picked, [])
        self.assertTrue(g.event)

    def test_a_reply_with_no_choices_is_not_done(self):
        r = ph.talk(FakeTalk([("box", "Speaker A", "Line 1")]), reply="Yes")
        self.assertFalse(r["done"])
        self.assertIn("no choices showed", r["result"])

    def test_no_prompt_presses_nothing(self):
        g = FakeTalk(SHOP, prompt=None)
        r = ph.talk(g)
        self.assertFalse(r["pressed"])
        self.assertEqual(g.presses, [])

    def test_an_id_with_the_prompt_already_up_does_not_walk(self):
        g = FakeTalk([("box", "Speaker A", "Line 1")])
        r = ph.talk(g, ident=5, name="Npc_X")             # (FakeTalk has no world: a walk would fail)
        self.assertTrue(r["done"], r)
        self.assertNotIn("walk", r)

    def test_without_the_wrap_or_the_verb_one_a_and_why(self):
        for g, why in ((FakeTalk(SHOP, hooked=False), "patch 0041"), (FakeTalk(SHOP, verb=False), "no `text` verb")):
            r = ph.talk(g)
            self.assertTrue(r["pressed"])
            self.assertEqual(g.presses, ["A"])
            self.assertIn(why, r["result"])
            self.assertFalse(r["done"])

    def test_bounded_by_boxes(self):
        r = ph.talk(FakeTalk([("box", "Speaker A", "Line %d" % i) for i in range(40)]), max_boxes=5)
        self.assertIn("too many boxes", r["result"])
        self.assertLessEqual(len(r["boxes"]), 6)

    def test_a_box_that_grows_is_one_box(self):
        g = FakeTalk([("box", "Speaker A", "Line 1")])
        orig = g._show

        def typed():
            g._set("T_Name_00", "Speaker A")
            g._set("T_MessageWhiteOut_00", "Li")
            orig()
        g._show = typed
        r = ph.talk(g)
        self.assertEqual([b["text"] for b in r["boxes"]], ["Line 1"])

    # --- markup, repeats, the speaker and the frame cap (a lane run of patch 0042: Kokiri shop and Mido) -------------
    def test_strip_markup_counts_the_params_in_bytes(self):
        # A name with a character of more than one byte: the tag's size is bytes, the text has one character for it.
        name = "Gr\u0101t"                                     # 4 characters, 5 bytes in UTF-8
        n = len(name.encode("utf-8"))
        t = bytes([0x0e, 4, 0, 0, 0, n & 255, n >> 8]).decode("latin-1") + name
        self.assertEqual(ph.strip_markup("A" + t + "B"), "AB")
        self.assertEqual(ph.strip_markup("A" + t), "A")
        # a size that names more bytes than the text has still loses only the first byte
        self.assertEqual(ph.strip_markup("A" + t[:-2]), "A" + t[1:-2])

    def test_the_helper_source_has_no_literal_markup_byte(self):
        src = open(ph.__file__, "rb").read()
        self.assertNotIn(b"\x0e", src)                           # (the tag byte is written "\\x0e" in the source)

    def test_strip_markup_keeps_a_variable_as_its_name_in_braces(self):
        item = tag(2, 3, b"\x13\x00Shop_SelectItemName\xff\xff")
        price = tag(2, 1, b"\x14\x00Shop_SelectItemPrice\xff\xff\x00")
        span = tag(0, 3, b"\xff\xff")
        self.assertEqual(ph.strip_markup("The " + item + span + "? That'll cost\n" + price + " rupees."),
                         "The {Shop_SelectItemName}? That'll cost\n{Shop_SelectItemPrice} rupees.")
        self.assertEqual(ph.strip_markup("Hi " + tag(2, 23, b"\x05\x00Name\x00\xff\xff")), "Hi {Name}")
        self.assertEqual(ph.strip_markup("Hi " + tag(2, 23, b"\x05\x00Name\x00\xff\xff"), variables=False), "Hi")
        self.assertEqual(ph.strip_markup("Hi " + tag(2, 23, b"")), "Hi")             # (a variable with no name)

    def test_strip_markup_page_break_is_a_space(self):
        # The page break (group 0, type 4, no params) ends a page: its two sides are two sentences, not one word.
        self.assertEqual(ph.strip_markup("Link!" + tag(0, 4) + "Let's go."), "Link! Let's go.")
        # No second space when the text already has one, or a newline, at the break.
        self.assertEqual(ph.strip_markup("Link! " + tag(0, 4) + "Let's go."), "Link! Let's go.")
        self.assertEqual(ph.strip_markup("Link!\n" + tag(0, 4) + "Let's go."), "Link!\nLet's go.")
        # A break at the start or the end leaves no space at the edge.
        self.assertEqual(ph.strip_markup(tag(0, 4) + "Start" + tag(0, 4)), "Start")
        # Other tags still join without a space.
        self.assertEqual(ph.strip_markup("a" + tag(0, 3, b"\x00\x00") + "b"), "ab")

    def test_strip_markup_keeps_the_words(self):
        self.assertEqual(ph.strip_markup(tag(3, 0, b"\x0f\x00\x00") + tag(4, 0, b"\x03\x00Ab\x00") + "Hello there."),
                         "Hello there.")
        self.assertEqual(ph.strip_markup("One " + tag(0, 3, b"\x00\x00") + "two" + tag(0, 3, b"\xff\xff") + " three."),
                         "One two three.")
        self.assertEqual(ph.strip_markup("Line" + tag(5, 1) + " next\nrow — end"), "Line next\nrow — end")
        self.assertEqual(ph.strip_markup("No markup at all"), "No markup at all")
        self.assertEqual(ph.strip_markup(tag(2, 23, b"\x05\x00Name\x00\xff\xff"), variables=False), "")
        # a tag cut short, or one that names more bytes than there are, loses only its first byte
        self.assertEqual(ph.strip_markup("Words\x0e\x01\x00"), "Words\x01\x00")
        cut = ph.strip_markup("\x0e\x01\x00\x02\x00\x40\x00words")
        self.assertIn("words", cut)
        self.assertNotIn("\x0e", cut)

    def test_a_box_text_is_clean_and_the_raw_is_kept(self):
        raw = tag(3, 0, b"\x0f\x00\x00") + tag(4, 0, b"\x03\x00Ab\x00") + "Line 1" + tag(5, 1) + " more."
        g = FakeTalk([("box", "Speaker A", raw), ("box", "Speaker A", "Plain")])
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual(r["boxes"][0]["text"], "Line 1 more.")
        self.assertEqual(r["boxes"][0]["raw"], raw)
        self.assertTrue(r["boxes"][0]["markup"])
        self.assertEqual(r["boxes"][1]["text"], "Plain")
        self.assertNotIn("raw", r["boxes"][1])             # (nothing was cut: the text is the raw)
        self.assertFalse(r["boxes"][1]["markup"])

    def test_whitespace_at_the_edges_is_not_markup(self):
        g = FakeTalk([("box", "Speaker A", "  Hi  ")])
        r = ph.talk(g)
        self.assertEqual(r["boxes"][0]["text"], "Hi")
        self.assertFalse(r["boxes"][0]["markup"])
        self.assertNotIn("raw", r["boxes"][0])

    def test_a_text_of_only_markup_is_not_a_box(self):
        g = FakeTalk([("box", "Speaker A", tag(2, 23, b"\x05\x00Name\x00\xff\xff")), ("box", "Speaker A", "Line 2")])
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual([b["text"] for b in r["boxes"]], ["Line 2"])

    def test_a_box_set_twice_is_one_box_but_a_repeat_later_is_two(self):
        g = FakeTalk([("box", "Speaker A", "Line 1"), ("box", "Speaker A", "Line 2"), ("box", "Speaker A", "Line 1")])
        orig = g._show

        def twice():
            orig()
            g.texts.append(g._entry("MessageDialog_00", "T_Message_00", g.talk[g.at][2], 26))   # (the same box again)
        g._show = twice
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual([b["text"] for b in r["boxes"]], ["Line 1", "Line 2", "Line 1"])

    def test_the_same_line_twice_in_a_row_is_two_boxes(self):
        g = FakeTalk([("box", "Speaker A", "Line 1"), ("box", "Speaker A", "Line 1"), ("box", "Speaker A", "Line 2")])
        orig = g._show

        def again():
            g.shown.clear()                                # (the game sets a box's text every time, same or not)
            orig()
        g._show = again
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual([b["text"] for b in r["boxes"]], ["Line 1", "Line 1", "Line 2"])

    def test_a_box_with_a_variable_keeps_it_and_a_lone_variable_is_no_box(self):
        item = tag(2, 3, b"\x13\x00Shop_SelectItemName\xff\xff")
        g = FakeTalk([("box", "Speaker A", item), ("box", "Speaker A", "Buy " + item + "?")])
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual([b["text"] for b in r["boxes"]], ["Buy {Shop_SelectItemName}?"])

    def test_the_speaker_is_the_name_pane_even_when_it_is_set_after_the_body(self):
        g = FakeTalk([("box", "Speaker A", "Line 1"), ("box", "Speaker A", "Line 2")], name_after=True)
        r = ph.talk(g)
        self.assertEqual([(b["speaker"], b["text"]) for b in r["boxes"]],
                         [("Speaker A", "Line 1"), ("Speaker A", "Line 2")])
        self.assertEqual(r["speakers"], ["Speaker A"])

    def test_a_name_set_after_the_next_body_is_that_box_only(self):
        g = FakeTalk([("box", "Speaker A", "Line 1"), ("box", "Speaker B", "Line 2")], name_after=True)
        r = ph.talk(g)
        self.assertEqual([(b["speaker"], b["text"]) for b in r["boxes"]],
                         [("Speaker A", "Line 1"), ("Speaker B", "Line 2")])

    def test_no_name_pane_means_no_speaker(self):
        g = FakeTalk([("box", None, "Line 1"), ("box", None, "Line 2")])
        r = ph.talk(g)
        self.assertTrue(r["done"], r)
        self.assertEqual([b["speaker"] for b in r["boxes"]], [None, None])
        self.assertEqual(r["speakers"], [])

    def test_the_frame_cap_waits_while_boxes_keep_coming(self):
        r = ph.talk(FakeTalk([("box", "K", "Line %d" % i) for i in range(40)]), max_boxes=1000, max_frames=60)
        self.assertTrue(r["done"], r)
        self.assertEqual(len(r["boxes"]), 40)
        self.assertGreater(r["frames"], 60)

    def test_the_frame_cap_gives_up_after_frames_with_nothing_new(self):
        g = FakeTalk([("box", "K", "Line 1"), ("box", "K", "Line 2")], stall=True)
        r = ph.talk(g, max_frames=100)
        self.assertFalse(r["done"])
        self.assertIn("timed out", r["result"])
        self.assertIn("nothing new", r["result"])
        self.assertEqual([b["text"] for b in r["boxes"]], ["Line 1", "Line 2"])
        self.assertLess(r["frames"], 200)

    def test_a_talk_that_never_stops_is_cut_at_the_total(self):
        r = ph.talk(FakeTalk([("box", "K", "Line %d" % i) for i in range(400)]), max_boxes=1000, max_frames=100,
                    max_total=300)
        self.assertFalse(r["done"])
        self.assertIn("timed out", r["result"])
        self.assertIn("300 frames in all", r["result"])

    def test_the_mcp_tools_take_reply_and_since(self):
        names = {t["name"]: t for t in mcp_playtest.PLAYER_TOOLS}
        props = names["talk"]["inputSchema"]["properties"]
        self.assertIn("reply", props)
        self.assertIn("since", props)
        self.assertIn("since", names["get_text"]["inputSchema"]["properties"])


if __name__ == "__main__":
    unittest.main()
