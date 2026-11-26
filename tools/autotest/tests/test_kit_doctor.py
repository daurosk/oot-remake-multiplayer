"""Tests for kit_doctor.py: the one-line checks and the .mcp.json template, with no game and no network (ports and
process ids are given by the tests). Run: python -I tools/autotest/tests/test_kit_doctor.py"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import kit_doctor as kd  # noqa: E402
import live  # noqa: E402


def touch(*parts):
    p = os.path.join(*parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write("x")
    return p


def levels(r):
    return {name: level for level, name, _ in r.lines}


def said(r, name):
    return next(t for level, n, t in r.lines if n == name)


class Checks(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.d = self.dir.name

    def tearDown(self):
        self.dir.cleanup()

    def game(self, name, dll=True, warmed=False):
        folder = os.path.join(self.d, name)
        touch(folder, "bin", "longshot.exe")
        if dll:
            touch(folder, "bin", kd.DLL)
        return folder

    def test_python_version(self):
        r = kd.Report()
        kd.check_python(r, (3, 9, 1))
        self.assertEqual(levels(r), {"python": "FAIL"})
        self.assertIn("3.9.1 is too old", said(r, "python"))
        r = kd.Report()
        kd.check_python(r, (3, 12, 0))
        self.assertEqual(levels(r), {"python": "OK"})

    def test_the_kit_files(self):
        r = kd.Report()
        kd.check_repo(r)
        self.assertEqual(levels(r), {"kit files": "OK"})
        r = kd.Report()
        kd.check_repo(r, self.d)
        self.assertEqual(levels(r), {"kit files": "FAIL"})
        self.assertIn("mcp_playtest.py", said(r, "kit files"))

    def test_a_game_copy_with_everything(self):
        r = kd.Report()
        exe = kd.check_game(r, "A", self.game("a"))
        self.assertTrue(exe.endswith("longshot.exe"))
        self.assertEqual(levels(r), {"game A": "OK", "game A shaders": "WARN"})   # (no warm-up marker in a fake folder)

    def test_each_thing_that_can_be_missing_is_said(self):
        r = kd.Report()
        kd.check_game(r, "A", os.path.join(self.d, "none"))
        self.assertEqual(levels(r), {"game A": "FAIL"})
        self.assertIn("no folder", said(r, "game A"))
        os.makedirs(os.path.join(self.d, "empty"))
        r = kd.Report()
        self.assertIsNone(kd.check_game(r, "A", os.path.join(self.d, "empty")))
        self.assertIn("no longshot.exe", said(r, "game A"))
        r = kd.Report()
        kd.check_game(r, "B", self.game("nodll", dll=False), " (lanes.py install ...)")
        self.assertEqual(levels(r)["game B"], "FAIL")
        self.assertIn("hookshot_coop.dll", said(r, "game B"))
        self.assertIn("lanes.py install", said(r, "game B"))

    def test_two_games_must_be_two_folders(self):
        a, b = self.game("a"), self.game("b")
        r = kd.Report()
        kd.check_copies(r, a, b)
        self.assertEqual(levels(r), {"game copies": "OK"})
        r = kd.Report()
        kd.check_copies(r, a, a)
        self.assertEqual(levels(r), {"game copies": "FAIL"})

    def test_ports_before_a_run_must_be_free(self):
        r = kd.Report()
        kd.check_ports(r, ("p1", "p2"), 1, False, is_listening=lambda port: port == 5012)
        self.assertEqual(levels(r), {"port p1": "OK", "port p2": "WARN", "port server": "OK"})
        self.assertIn("5012 is taken", said(r, "port p2"))
        r = kd.Report()
        kd.check_ports(r, ("p1",), 1, False, is_listening=lambda port: port == 27521)
        self.assertEqual(levels(r)["port server"], "WARN")

    def test_ports_during_a_run_must_answer(self):
        r = kd.Report()
        kd.check_ports(r, ("p1", "p2"), None, True, is_listening=lambda port: port == 5001)
        self.assertEqual(levels(r), {"port p1": "OK", "port p2": "FAIL"})
        self.assertIn("5002: no game listens", said(r, "port p2"))

    def test_listening_uses_the_connection(self):
        class Sock:
            def close(self):
                pass

        def up(addr, timeout):
            return Sock()

        def down(addr, timeout):
            raise ConnectionRefusedError()
        self.assertTrue(kd.listening(5001, connect=up))
        self.assertFalse(kd.listening(5001, connect=down))

    def test_session(self):
        r = kd.Report()
        kd.check_session(r, os.path.join(self.d, "nope"))
        self.assertEqual(levels(r), {"session": "FAIL"})
        s = os.path.join(self.d, "s")
        os.makedirs(s)
        r = kd.Report()
        kd.check_session(r, s)
        self.assertIn("no readable session.json", said(r, "session"))
        live.save(s, {"games": {}})
        r = kd.Report()
        kd.check_session(r, s, alive=lambda pid: True)
        self.assertEqual(levels(r), {"session": "WARN"})
        with open(os.path.join(s, "pids.json"), "w", encoding="utf-8") as f:
            json.dump({"games": {"A": 11, "B": 22}}, f)
        r = kd.Report()
        kd.check_session(r, s, alive=lambda pid: True)
        self.assertEqual((levels(r)["session"], said(r, "session")), ("OK", "running, games A, B alive"))
        r = kd.Report()
        kd.check_session(r, s, alive=lambda pid: pid == 11)
        self.assertEqual(levels(r)["session"], "FAIL")
        self.assertIn("game B is gone", said(r, "session"))
        with open(os.path.join(s, "watchdog.json"), "w", encoding="utf-8") as f:
            json.dump({"ended": "idle"}, f)
        r = kd.Report()
        kd.check_session(r, s, alive=lambda pid: True)
        self.assertIn("ended (idle)", said(r, "session"))

    def test_the_report_text_is_one_line_each(self):
        r = kd.Report()
        r.ok("a", "fine")
        r.fail("b", "broken")
        self.assertEqual(r.text().splitlines(), ["OK   a: fine", "FAIL b: broken"])
        self.assertTrue(r.failed())


class Mcp(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.d = self.dir.name

    def tearDown(self):
        self.dir.cleanup()

    def write(self, **kw):
        data = kd.render_mcp(kd.TEMPLATE, kw.get("lane", 1), "sess", "logs")
        if "mutate" in kw:
            kw["mutate"](data["mcpServers"])
        path = os.path.join(self.d, ".mcp.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f)
        return path

    def good(self):
        """A .mcp.json whose script paths and session folder exist."""
        def point(servers):
            for s in servers.values():
                s["args"] = [os.path.join(kd.HERE, "mcp_playtest.py") if a == "tools/autotest/mcp_playtest.py" else a
                             for a in s["args"]]
        os.makedirs(os.path.join(self.d, "sess"), exist_ok=True)
        return self.write(mutate=point)

    def check(self, path, lane=1):
        r = kd.Report()
        kd.check_mcp(r, path, lane, which=lambda c: c)
        return r

    def test_the_template_has_no_local_paths_or_names(self):
        with open(kd.TEMPLATE, encoding="utf-8") as f:
            text = f.read()
        self.assertEqual(sorted(json.loads(text)["mcpServers"]), ["oot-p1", "oot-p2", "oot-playtest"])
        for bad in (":\\", ":/", "/Users/", "/home/", "\\\\", "@"):
            self.assertNotIn(bad, text)
        self.assertIn("{LANE}", text)

    def test_a_pair_server_needs_pair_and_no_player(self):
        def pair(servers, extra=(), drop=None):
            servers.clear()
            args = ["-I", os.path.join(kd.HERE, "mcp_playtest.py"), "--pair", "--lane", "1", "--log-dir", "logs"]
            servers["oot-pair"] = {"command": "python", "args": [x for x in args if x != drop] + list(extra)}
        r = self.check(self.write(mutate=pair))
        self.assertEqual(levels(r), {"mcp oot-pair": "OK"}, r.text())
        self.assertIn("bridge ports p1 %d, p2 %d" % (kd.pc.port_for("p1", 1), kd.pc.port_for("p2", 1)),
                      said(r, "mcp oot-pair"))
        r = self.check(self.write(mutate=lambda s: pair(s, ["--player", "p1"])))
        self.assertIn("takes no --player", said(r, "mcp oot-pair"))
        r = self.check(self.write(mutate=lambda s: pair(s, drop="--pair")))
        self.assertIn("do not have --pair", said(r, "mcp oot-pair"))

    def test_a_pair_server_next_to_the_per_game_servers_fails(self):
        def both(servers):
            for v in servers.values():
                v["args"] = [os.path.join(kd.HERE, "mcp_playtest.py") if x == "tools/autotest/mcp_playtest.py" else x
                             for x in v["args"]]
            servers.pop("oot-playtest", None)
            servers["oot-pair"] = {"command": "python", "args": ["-I", os.path.join(kd.HERE, "mcp_playtest.py"),
                                                                 "--pair", "--lane", "1", "--log-dir", "logs"]}
        os.makedirs(os.path.join(self.d, "sess"))
        r = self.check(self.write(mutate=both))
        self.assertTrue(r.failed(), r.text())
        fails = [t for level, n, t in r.lines if level == "FAIL"]
        self.assertEqual(len(fails), 1, r.text())
        self.assertIn("registered with oot-p1 and oot-p2", fails[0])

    def test_render_fills_the_placeholders_and_drops_what_is_not_given(self):
        data = kd.render_mcp(kd.TEMPLATE, 2, "sess", "logs")
        p1 = data["mcpServers"]["oot-p1"]["args"]
        self.assertEqual(p1, ["-I", "tools/autotest/mcp_playtest.py", "--player", "p1", "--lane", "2", "--session", "sess",
                              "--log-dir", "logs"])
        self.assertEqual(data["mcpServers"]["oot-playtest"]["args"][-2:], ["--session", "sess"])
        bare = kd.render_mcp(kd.TEMPLATE, None, None, None)
        self.assertEqual(sorted(bare["mcpServers"]), ["oot-p1", "oot-p2"])     # the session server needs a session
        self.assertEqual(bare["mcpServers"]["oot-p2"]["args"], ["-I", "tools/autotest/mcp_playtest.py", "--player", "p2"])
        self.assertNotRegex(json.dumps(bare), r"\{[A-Z_]+\}")

    def test_a_rendered_file_that_points_at_the_repo_passes(self):
        # the paths in the file are relative to the repository folder: put the file there in spirit by absolute path
        def point(servers):
            for s in servers.values():
                s["args"] = [os.path.join(kd.HERE, "mcp_playtest.py") if a == "tools/autotest/mcp_playtest.py" else a
                             for a in s["args"]]
        os.makedirs(os.path.join(self.d, "sess"))
        r = self.check(self.write(mutate=point))
        self.assertEqual(set(levels(r).values()), {"OK"}, r.text())
        self.assertIn("bridge port 5011", said(r, "mcp oot-p1"))
        self.assertIn("bridge port 5012", said(r, "mcp oot-p2"))

    def test_each_mistake_is_one_line(self):
        r = self.check(self.write())     # (relative script path: not under this temp folder)
        self.assertEqual(levels(r)["mcp oot-p1"], "FAIL")
        self.assertIn("does not exist", said(r, "mcp oot-p1"))
        self.assertIn("--session sess is not a folder yet", said(r, "mcp oot-p1"))

        def wrong(servers):
            servers["oot-p2"]["args"][servers["oot-p2"]["args"].index("p2")] = "p1"
            servers["oot-p1"]["args"][servers["oot-p1"]["args"].index("logs")] = "other"
            servers["oot-playtest"]["command"] = ""
        r = self.check(self.write(mutate=wrong), lane=2)
        self.assertIn("--player is p1, not p2", said(r, "mcp oot-p2"))
        self.assertIn("--lane is 1, not 2", said(r, "mcp oot-p1"))
        self.assertIn("command '' is not found", said(r, "mcp oot-playtest"))
        self.assertIn("different --log-dir", said(r, "mcp log-dir"))

    def test_a_missing_log_dir_counts_as_the_default_folder(self):
        def drop(servers):
            a = servers["oot-p2"]["args"]
            del a[a.index("--log-dir"):a.index("--log-dir") + 2]
        r = self.check(self.write(mutate=drop))
        self.assertIn("different --log-dir", said(r, "mcp log-dir"))
        self.assertIn(kd.DEFAULT_LOG_DIR, said(r, "mcp log-dir"))

        def both_default(servers):
            for n in ("oot-p1", "oot-p2"):
                a = servers[n]["args"]
                del a[a.index("--log-dir"):a.index("--log-dir") + 2]
        r = self.check(self.write(mutate=both_default))
        self.assertNotIn("mcp log-dir", [n for _, n, _ in r.lines])

    def test_a_lane_that_is_not_a_number_is_a_fail_not_a_crash(self):
        def junk(servers):
            a = servers["oot-p1"]["args"]
            a[a.index("--lane") + 1] = "one"
        r = self.check(self.write(mutate=junk), lane=None)
        self.assertEqual(levels(r)["mcp oot-p1"], "FAIL")
        self.assertIn("not a lane number", said(r, "mcp oot-p1"))

    def test_the_python_the_file_starts_must_be_3_10_or_newer(self):
        path = self.good()
        asked = []
        r = kd.Report()
        kd.check_mcp(r, path, 1, which=lambda c: c, probe=lambda c: asked.append(c) or (3, 9, 1))
        self.assertTrue(asked)
        self.assertIn("Python 3.9.1", said(r, "mcp oot-p1"))
        r = kd.Report()
        kd.check_mcp(r, path, 1, which=lambda c: c, probe=lambda c: (3, 12, 0))
        self.assertEqual(levels(r)["mcp oot-p1"], "OK")
        r = kd.Report()
        kd.check_mcp(r, path, 1, which=lambda c: c, probe=lambda c: None)
        self.assertIn("could not be asked", said(r, "mcp oot-p1"))

    def test_python_version_asks_the_real_interpreter(self):
        self.assertEqual(kd.python_version(sys.executable), tuple(sys.version_info[:3]))
        self.assertIsNone(kd.python_version(os.path.join(self.d, "no-such-python")))

    def test_a_missing_or_broken_file(self):
        r = kd.Report()
        kd.check_mcp(r, os.path.join(self.d, "none.json"))
        self.assertIn("--write-mcp", said(r, "mcp"))
        bad = os.path.join(self.d, "bad.json")
        with open(bad, "w", encoding="utf-8") as f:
            f.write("{nope")
        r = kd.Report()
        kd.check_mcp(r, bad)
        self.assertIn("not readable JSON", said(r, "mcp"))
        empty = os.path.join(self.d, "empty.json")
        with open(empty, "w", encoding="utf-8") as f:
            f.write('{"mcpServers": {"other": {"command": "x"}}}')
        r = kd.Report()
        kd.check_mcp(r, empty)
        self.assertIn("no oot-p1 / oot-p2 server", said(r, "mcp"))


class Cli(unittest.TestCase):
    def run_cli(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = kd.main(list(argv))
        return code, out.getvalue()

    def test_write_mcp_never_replaces_without_force(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, ".mcp.json")
            code, out = self.run_cli("--write-mcp", path, "--lane", "1", "--session", "S", "--log-dir", "L")
            self.assertEqual(code, 0)
            self.assertIn("oot-p1, oot-p2, oot-playtest", out)
            with open(path, encoding="utf-8") as f:
                self.assertEqual(json.load(f)["mcpServers"]["oot-p2"]["args"][-6:],
                                 ["--lane", "1", "--session", "S", "--log-dir", "L"])
            code, out = self.run_cli("--write-mcp", path)
            self.assertEqual(code, 1)
            self.assertIn("exists", out)
            self.assertEqual(self.run_cli("--write-mcp", path, "--force")[0], 0)

    def test_the_whole_run_names_what_is_wrong(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, "a"), os.path.join(d, "b")
            touch(a, "bin", "longshot.exe")
            touch(b, "bin", "longshot.exe")
            touch(b, "bin", kd.DLL)
            cwd = os.getcwd()
            os.chdir(d)       # (no .mcp.json here)
            try:
                code, out = self.run_cli("--game-a", a, "--game-b", b, "--players", "1")
                code_json, out_json = self.run_cli("--game-a", a, "--game-b", b, "--json")
            finally:
                os.chdir(cwd)
            self.assertEqual(code, 1)
            lines = out.splitlines()
            self.assertTrue(any(x.startswith("FAIL game A: no hookshot_coop.dll") for x in lines), out)
            self.assertTrue(any(x.startswith("OK   game B") for x in lines), out)
            self.assertTrue(any(x.startswith("WARN mcp: no .mcp.json") for x in lines), out)
            self.assertNotIn("port p2", out)
            self.assertEqual(code_json, 1)
            self.assertIn("game A", [x["name"] for x in json.loads(out_json)])


if __name__ == "__main__":
    unittest.main()
