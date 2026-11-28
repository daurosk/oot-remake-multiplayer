"""The session server's start checks and the watchdog's kill-on-close job (exefs-xplay-182cd53b-2 SETUP-FAIL, 11-28).

An orphan coop_server of gate-host170-182cd53b (its watchdog tree-killed at the 01:40 owner pause) held lane 1's UDP
27520 for 6 h. Every later lane-1 session's own server printed 'coop_server: cannot listen on port 27520' and exited,
hookshot.py run slept 1 s and went on, and both games joined the orphan. These tests hold each layer of the fix:
- hookshot.py run: a held port is a SETUP-FAIL naming the holder before the server starts (never killed);
- hookshot.py run: a server that exited at start is a SETUP-FAIL with its own last line;
- live.py server start: the same two checks;
- hs.udp_port_holder: free = '' and held = non-empty (the pid on Windows);
- hs.KillOnCloseJob: a process put in it dies when its holder is killed outright (TerminateProcess);
- live.py adopt: every pid of pids.json (games + server) goes in the job only while it is still that process (its
  image = the session's game/server exe, made no later than the pids.json write: review-tests-153-7).
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import types
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import hookshot  # noqa: E402
import live  # noqa: E402
import lanes  # noqa: E402

hs = lanes.hs


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Dead:
    returncode = 1

    def poll(self):
        return 1


class Alive:
    returncode = None

    def poll(self):
        return None


class StartChecks(unittest.TestCase):
    def test_bind_host(self):
        self.assertEqual(hookshot.server_bind_host(["s", "--port", "1"]), "127.0.0.1")
        self.assertEqual(hookshot.server_bind_host(["s", "--bind", "10.0.0.5", "--port", "1"]), "10.0.0.5")

    def test_held_port_is_setup_fail_naming_holder(self):
        asked = []
        with self.assertRaises(RuntimeError) as c:
            hookshot.server_port_free(27520, ["s", "--bind", "127.0.0.1"],
                                      holder=lambda p, h: asked.append((p, h)) or "pid 35824 (coop_server.exe: x)")
        self.assertEqual(asked, [(27520, "127.0.0.1")])
        self.assertIn("SETUP-FAIL server: port 27520 is already held by pid 35824 (coop_server.exe: x)", str(c.exception))
        self.assertIn("not killed", str(c.exception))

    def test_free_port_passes(self):
        hookshot.server_port_free(27520, ["s"], holder=lambda p, h: "")

    def test_exited_server_is_setup_fail_with_its_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "server-out.txt")
            with open(out, "w") as f:
                f.write("coop_server: cannot listen on port 27520\n")
            with self.assertRaises(RuntimeError) as c:
                hookshot.server_alive(Dead(), out, 27520, ["s"], holder=lambda p, h: "pid 35824 (coop_server.exe: x)")
            msg = str(c.exception)
            self.assertIn("SETUP-FAIL server: coop_server exited at start (exit 1): coop_server: cannot listen on port 27520",
                          msg)
            self.assertIn("port 27520 is held by pid 35824", msg)

    def test_live_server_passes(self):
        hookshot.server_alive(Alive(), "nowhere", 1, ["s"], holder=lambda p, h: self.fail("asked"))
        hookshot.server_alive(None, "nowhere", 1, ["s"], holder=lambda p, h: self.fail("asked"))


class PortHolder(unittest.TestCase):
    def test_free_and_held(self):
        port = free_port()
        self.assertEqual(hs.udp_port_holder(port), "")
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", port))
        try:
            who = hs.udp_port_holder(port)
        finally:
            s.close()
        self.assertTrue(who)
        if hs.WINDOWS:
            self.assertIn("pid %d " % os.getpid(), who)
        self.assertEqual(hs.udp_port_holder(port), "")


class RunChecks(unittest.TestCase):
    """hookshot.py run end to end up to the server: games are empty folders (the server checks come first)."""

    def run_main(self, tmp, port, server):
        code = hookshot.main(["run", "--game-a", os.path.join(tmp, "A"), "--game-b", os.path.join(tmp, "B"),
                              "--out", os.path.join(tmp, "out"), "--name", "t", "--every", "0",
                              "--server", server, "--port", str(port), "--no-restore"])
        with open(os.path.join(tmp, "out", "FAILED.txt"), encoding="utf-8") as f:
            return code, f.read()

    def test_held_port(self):
        with tempfile.TemporaryDirectory() as tmp:
            for x in "AB":
                os.makedirs(os.path.join(tmp, x))
            port = free_port()
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind(("127.0.0.1", port))
            try:
                code, failed = self.run_main(tmp, port, sys.executable)
            finally:
                s.close()
            self.assertEqual(code, 1)
            self.assertIn("SETUP-FAIL server: port %d is already held by" % port, failed)

    def test_server_that_exits_at_start(self):
        # (the Python interpreter as the 'server': it refuses --port and exits 2 at once, as coop_server does on a
        # port it cannot bind)
        with tempfile.TemporaryDirectory() as tmp:
            for x in "AB":
                os.makedirs(os.path.join(tmp, x))
            code, failed = self.run_main(tmp, free_port(), sys.executable)
            self.assertEqual(code, 1)
            self.assertIn("SETUP-FAIL server: coop_server exited at start (exit 2)", failed)


class LiveServerStart(unittest.TestCase):
    def session(self, tmp):
        d = os.path.join(tmp, "s")
        os.makedirs(d)
        with open(os.path.join(d, "session.json"), "w") as f:
            json.dump({"started": time.time(), "dir": d, "lane": None, "port": 27520, "booted": True, "max_s": 0,
                       "idle_s": 0, "games": {}, "runner": {"server": "coop_server.exe", "port": 27520,
                                                            "password": "pw", "pipe": "p"}}, f)
        with open(os.path.join(d, "pids.json"), "w") as f:
            json.dump({"games": {}, "server": None}, f)
        return d

    def test_held_port_refused_no_start(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = self.session(tmp)
            a = types.SimpleNamespace(session=d, action="start", server_arg=None)
            out = live.server_cmd(a, alive=lambda p: False, popen=lambda *x, **k: self.fail("started"),
                                  sleep=lambda s: None, holder=lambda p, h: "pid 35824 (coop_server.exe: x)")
            self.assertIn("SETUP-FAIL server: port 27520 is already held by pid 35824", out["error"])

    def test_exited_server_is_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = self.session(tmp)
            a = types.SimpleNamespace(session=d, action="start", server_arg=None)
            calls = []

            def holder(p, h):
                calls.append(p)
                return ""   # (free before the start; after it, the dead server's line alone)

            dead = Dead()
            dead.pid = 4242
            out = live.server_cmd(a, alive=lambda p: False, popen=lambda *x, **k: dead, sleep=lambda s: None,
                                  holder=holder)
            self.assertIn("SETUP-FAIL server: coop_server exited at start (exit 1)", out["error"])
            with open(os.path.join(d, "pids.json")) as f:
                self.assertIsNone(json.load(f)["server"])


class FakeJob:
    def __init__(self, refuse=()):
        self.calls, self.refuse, self.skipped = [], set(refuse), {}

    def add(self, pid, image, not_after):
        self.calls.append((pid, image, not_after))
        if pid in self.refuse:
            self.skipped[pid] = "image other.exe, not %s" % image
            return False
        return True


def session_dir(d, games, server, runner_server="C:/k/coop_server.exe"):
    with open(os.path.join(d, "session.json"), "w") as f:
        json.dump({"started": 0, "max_s": 0, "booted": False,
                   "games": {x: {"folder": os.path.join(d, "lane1-" + x)} for x in games},
                   "runner": {"server": runner_server}}, f)
    with open(os.path.join(d, "pids.json"), "w") as f:
        json.dump({"games": games, "server": server}, f)


class Adopt(unittest.TestCase):
    def test_games_and_server_with_their_images(self):
        with tempfile.TemporaryDirectory() as d:
            session_dir(d, {"A": 11, "B": 12}, 13)
            job = FakeJob()
            self.assertEqual(live.adopt(d, job), {})
            written = os.path.getmtime(os.path.join(d, "pids.json"))
            exe = "longshot.exe" if hs.WINDOWS else "longshot"
            self.assertEqual(sorted(job.calls), [(11, os.path.join(d, "lane1-A", exe), written),
                                                 (12, os.path.join(d, "lane1-B", exe), written),
                                                 (13, "C:/k/coop_server.exe", written)])

    def test_skipped_pid_named(self):
        with tempfile.TemporaryDirectory() as d:
            session_dir(d, {"A": 11}, 13)
            self.assertEqual(live.adopt(d, FakeJob(refuse={11})), {11: "image other.exe, not %s"
                                                                   % os.path.join(d, "lane1-A", "longshot.exe"
                                                                                  if hs.WINDOWS else "longshot")})

    def test_game_without_folder_never_added(self):
        with tempfile.TemporaryDirectory() as d:
            session_dir(d, {"A": 11}, None)
            with open(os.path.join(d, "pids.json"), "w") as f:
                json.dump({"games": {"A": 11, "B": 99}, "server": None}, f)
            job = FakeJob()
            live.adopt(d, job)
            self.assertIn((99, None, os.path.getmtime(os.path.join(d, "pids.json"))), job.calls)

    def test_watch_adopts_each_look(self):
        with tempfile.TemporaryDirectory() as d:
            session_dir(d, {"A": 21}, 22)
            job = FakeJob()
            looks = []

            def sleep(s):
                looks.append(s)
                if len(looks) == 2:
                    open(live.stop_file(d), "w").close()
            self.assertEqual(live.watch(d, sleep=sleep, job=job), "stop asked")
            self.assertEqual(sorted(c[0] for c in job.calls), [21, 21, 21, 22, 22, 22])

    def test_watch_without_job_adopts_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            session_dir(d, {"A": 21}, 22)
            open(live.stop_file(d), "w").close()
            self.assertEqual(live.watch(d, sleep=lambda s: None), "stop asked")


HOLDER = r'''
import subprocess, sys, time
sys.path.insert(0, sys.argv[1])
import hs
child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"])
job = hs.KillOnCloseJob()
print(child.pid, job.add(child.pid, sys.executable, time.time()), flush=True)
time.sleep(120)
'''


@unittest.skipUnless(hs.WINDOWS, "Windows job objects")
class KillOnClose(unittest.TestCase):
    def test_child_dies_with_its_hard_killed_holder(self):
        hsdir = os.path.dirname(os.path.abspath(hs.__file__))
        holder = subprocess.Popen([sys.executable, "-I", "-c", HOLDER, hsdir], stdout=subprocess.PIPE, text=True)
        try:
            pid, ok = holder.stdout.readline().split()
            pid = int(pid)
            self.assertEqual(ok, "True")
            self.assertTrue(hs.pid_alive(pid))
            holder.kill()                     # (TerminateProcess: no finally, no close() runs in the holder)
            holder.wait(10)
            end = time.monotonic() + 10
            while hs.pid_alive(pid) and time.monotonic() < end:
                time.sleep(0.2)
            self.assertFalse(hs.pid_alive(pid), "the job's child outlived its hard-killed holder")
        finally:
            if holder.poll() is None:
                holder.kill()

    def test_add_gone_pid_is_false(self):
        job = hs.KillOnCloseJob()
        p = subprocess.Popen([sys.executable, "-c", "pass"])
        p.wait()
        self.assertFalse(job.add(p.pid, sys.executable, time.time()))

    def child(self):
        p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        self.addCleanup(lambda: (p.kill(), p.wait()))
        return p

    def test_wrong_image_not_added(self):
        # (a reused pid: some other program now has the number pids.json kept)
        job = hs.KillOnCloseJob()
        p = self.child()
        other = os.path.join(tempfile.gettempdir(), "lane1-A", "longshot.exe")
        self.assertFalse(job.add(p.pid, other, time.time() + 5))
        self.assertIn("not %s" % other, job.skipped[p.pid])
        self.assertNotIn(p.pid, job.added)

    def test_made_after_pids_write_not_added(self):
        # (the same exe, but started after pids.json named this pid: not the process it named)
        job = hs.KillOnCloseJob()
        before = time.time() - 30
        p = self.child()
        self.assertFalse(job.add(p.pid, sys.executable, before))
        self.assertIn("after its pids.json write", job.skipped[p.pid])

    def test_matching_process_added(self):
        job = hs.KillOnCloseJob()
        p = self.child()
        time.sleep(0.2)
        self.assertTrue(job.add(p.pid, sys.executable, time.time()), job.skipped)


if __name__ == "__main__":
    unittest.main()
