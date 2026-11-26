"""Tests for the shared play-test library (kit_library.py, tools/playtest): every script and data file in the repository
is read and checked, every script runs against the fake bridge of test_playtest_bridge.py, and the checks refuse what
they should. Run: python -I tools/autotest/tests/test_kit_library.py"""
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import kit_library  # noqa: E402
import kit_tools  # noqa: E402
import mcp_playtest  # noqa: E402
import playtest_client as pc  # noqa: E402
from test_playtest_bridge import FakeGame  # noqa: E402

HEAD = ["# what: Walk a bit.", "# scene: KokiriForest_Child", "# start: At the start.", "# expect: Frames pass.", "# kit: 1"]
STEP = '{"server":"oot-p1","tool":"step","args":{"frames":3}}'


class Folder(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.root, "scripts"))
        os.makedirs(os.path.join(self.root, "data"))
        self.table = kit_library.tool_table()

    def tearDown(self):
        shutil.rmtree(self.root)

    def script(self, lines, name="a-script.jsonl"):
        path = os.path.join(self.root, "scripts", name)
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        return path

    def data(self, d, name="a-scene.json"):
        path = os.path.join(self.root, "data", name)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(d, f)
        return path

    def says(self, problems, text):
        self.assertTrue(any(text in p for p in problems), "no %r in %s" % (text, problems))


class TheRepositoryLibrary(unittest.TestCase):
    def test_every_script_and_data_file_is_valid(self):
        problems, scripts, data = kit_library.check_all()
        self.assertEqual(problems, [])
        self.assertGreaterEqual(scripts, 1)
        self.assertGreaterEqual(data, 1)

    def test_every_script_runs_against_the_fake_bridge(self):
        folder = os.path.join(kit_library.ROOT, "scripts")
        names = [n for n in sorted(os.listdir(folder)) if n.endswith(".jsonl")]
        self.assertTrue(names)
        for name in names:
            tmp = tempfile.TemporaryDirectory()   # (the per-game checkpoint's dumps and the shots go there)
            games = {p: FakeGame(dump_dir=tmp.name) for p in ("p1", "p2", "p3", "p4")}
            players = {p: mcp_playtest.Game(p, pc.Client(g.port), tmp.name) for p, g in games.items()}

            pair = mcp_playtest.Pair({"p1": players["p1"], "p2": players["p2"]})
            session_calls = []

            def run(server, tool, args, players=players, pair=pair, session_calls=session_calls):
                if server == "oot-pair":
                    return pair.answer(tool, args)
                if server in ("live", "oot-playtest"):   # a live session's tools (games and server started and
                    session_calls.append(tool)          # closed): their arguments are checked by check_all
                    return {}
                if server.startswith("oot-p"):
                    return players[server[4:]].call(tool, args)
                self.fail("%s: a script call for %s is not run by this test" % (name, server))

            try:
                said = []
                result = kit_tools.replay([os.path.join(folder, name)], runner=run, say=said.append)
                self.assertEqual(result["failed"], [], "%s: %s" % (name, said))
                self.assertGreaterEqual(result["ran"], 1, name)
            finally:
                for p in players.values():
                    p.client.close()
                for g in games.values():
                    g.close()
                tmp.cleanup()

    def test_the_readmes_are_there(self):
        for sub in ("", "scripts", "data"):
            self.assertTrue(os.path.isfile(os.path.join(kit_library.ROOT, sub, "README.md")), sub)


class Scripts(Folder):
    def test_a_good_script_passes(self):
        self.assertEqual(kit_library.check_script(self.script(HEAD + [STEP]), self.table), [])

    def test_header_lines_are_all_needed(self):
        for i, key in enumerate(kit_library.HEADER_KEYS):
            lines = HEAD[:i] + HEAD[i + 1:] + [STEP]
            self.says(kit_library.check_script(self.script(lines), self.table), "no `# %s: ...` line" % key)

    def test_a_script_needs_a_calls_and_a_good_name(self):
        self.says(kit_library.check_script(self.script(HEAD), self.table), "no calls")
        self.says(kit_library.check_script(self.script(HEAD + [STEP], "Bad Name.jsonl"), self.table), "lowercase words")
        self.says(kit_library.check_script(self.script(HEAD + [STEP], "a-script.txt"), self.table), ".jsonl")

    def test_kit_and_scene_in_the_header(self):
        for bad in ("0", "2", "x", ""):
            lines = [l if not l.startswith("# kit") else "# kit: " + bad for l in HEAD] + [STEP]
            self.assertTrue(kit_library.check_script(self.script(lines), self.table), bad)
        lines = [l if not l.startswith("# scene") else "# scene: two words" for l in HEAD] + [STEP]
        self.says(kit_library.check_script(self.script(lines), self.table), "one map name")

    def test_a_pasted_log_in_the_header_is_refused(self):
        lines = [l if not l.startswith("# what") else "# what: " + "x" * 400 for l in HEAD] + [STEP]
        self.says(kit_library.check_script(self.script(lines), self.table), "longer than")

    def test_a_line_that_is_not_json_is_found(self):
        # (replay would skip it without a word: the check is where it shows)
        problems = kit_library.check_script(self.script(HEAD + [STEP, '{"server":"oot-p1","tool":"step",']), self.table)
        self.says(problems, "a-script.jsonl:7: not JSON")
        self.says(kit_library.check_script(self.script(HEAD + ["[1]"]), self.table), "JSON object")

    def test_calls_are_read_against_the_tool_schemas(self):
        def one(call):
            return kit_library.check_script(self.script(HEAD + [json.dumps(call)]), self.table)
        self.says(one({"server": "oot-p9", "tool": "step", "args": {"frames": 1}}), "is not oot-p1")
        self.says(one({"server": "oot-p1", "tool": "stpe", "args": {"frames": 1}}), "no tool 'stpe'")
        self.says(one({"server": "oot-p1", "tool": "step", "args": {}}), "missing argument frames")
        self.says(one({"server": "oot-p1", "tool": "step", "args": {"frames": 1, "speed": 2}}), "no argument speed")
        self.says(one({"server": "oot-p1", "tool": "step", "args": {"frames": "ten"}}), "should be a integer")
        self.says(one({"server": "oot-p1", "tool": "press", "args": {"buttons": ["Q"]}}), "is not one of")
        self.says(one({"server": "oot-p1", "tool": "press", "args": {"stick_strength": 3}}), "above 1")
        self.says(one({"server": "oot-p1", "tool": "step", "args": {"frames": 1}, "when": 5}), "unknown key when")
        self.says(one({"server": "oot-p1", "tool": "chat", "args": {"text": "x" * 301}}), "longer than 300")
        self.assertEqual(one({"server": "oot-p1", "tool": "chat", "args": {"text": "x" * 300}}), [])
        self.says(one({"server": "oot-p1", "tool": "step", "args": [1]}), "args must be an object")
        self.assertEqual(one({"server": "oot-p1", "tool": "press", "args": {"buttons": ["A", "UP"], "frames": 2}}), [])

    def test_session_tools_are_checked_for_the_session_servers(self):
        ok = {"server": "oot-playtest", "tool": "state", "args": {}}
        self.assertEqual(kit_library.check_call(ok, self.table), [])
        self.says(kit_library.check_call({"server": "live", "tool": "get_world", "args": {}}, self.table), "no tool")

    def test_pair_calls_are_checked_with_a_player(self):
        # mcp_playtest --pair writes server oot-pair: the player tools, each with the game it went to.
        one = lambda c: kit_library.check_call(c, self.table)  # noqa: E731
        self.assertEqual(one({"server": "oot-pair", "tool": "press", "args": {"player": "p2", "buttons": ["A"]}}), [])
        self.assertEqual(one({"server": "oot-pair", "tool": "step", "args": {"frames": 3}}), [])   # both games
        self.assertEqual(one({"server": "oot-pair", "tool": "resume", "args": {}}), [])
        self.says(one({"server": "oot-pair", "tool": "press", "args": {"buttons": ["A"]}}), "missing argument player")
        self.says(one({"server": "oot-pair", "tool": "press", "args": {"player": "p3"}}), "is not one of")
        self.says(one({"server": "oot-p1", "tool": "press", "args": {"player": "p1"}}), "no argument player")
        self.says(one({"server": "oot-pairs", "tool": "step", "args": {}}), "oot-pair")

    def test_a_pair_call_log_is_read_saved_and_replayed(self):
        log = os.path.join(self.root, "calls.jsonl")
        w = lambda tool, args: kit_tools.CallLog(log, "oot-pair").write(tool, args, True, None, 1)  # noqa: E731
        w("press", {"player": "p1", "buttons": ["A"], "frames": 2})
        w("get_state", {"player": "p2"})
        self.assertEqual([c["server"] for c in kit_tools.read_calls([log])], ["oot-pair", "oot-pair"])
        out = os.path.join(self.root, "scripts", "a-pair.jsonl")
        self.assertEqual(kit_library.save_script([log], out, "A press.", "KokiriForest_Child", "Start.", "Nothing."), 1)
        self.assertEqual(kit_library.check_script(out, self.table), [])
        ran = []
        kit_tools.replay([out], runner=lambda s, t, a: ran.append((s, t, a)), say=lambda _: None)
        self.assertEqual(ran, [("oot-pair", "press", {"player": "p1", "buttons": ["A"], "frames": 2})])


class SavingAScript(Folder):
    def test_a_call_log_becomes_a_script_without_looks_and_failures(self):
        log = os.path.join(self.root, "calls.jsonl")
        w = lambda server, tool, args, ok=True, t=0: kit_tools.CallLog(log, server).write(tool, args, ok, None, t)  # noqa: E731
        w("oot-p1", "step", {"frames": 4}, t=1)
        w("oot-p1", "get_state", {}, t=2)
        w("oot-p2", "step", {"frames": 4}, ok=False, t=3)
        w("oot-p2", "press", {"buttons": ["A"]}, t=4)
        out = os.path.join(self.root, "scripts", "from-a-run.jsonl")
        n = kit_library.save_script([log], out, "Two calls.", "KokiriForest_Child", "At the start.", "Nothing.")
        self.assertEqual(n, 2)
        with open(out, encoding="utf-8") as f:
            text = f.read().splitlines()
        self.assertEqual(text[:5], ["# what: Two calls.", "# scene: KokiriForest_Child", "# start: At the start.",
                                    "# expect: Nothing.", "# kit: %d" % kit_library.KIT_VERSION])
        self.assertEqual([json.loads(l) for l in text[5:]],
                         [{"server": "oot-p1", "tool": "step", "args": {"frames": 4}},
                          {"server": "oot-p2", "tool": "press", "args": {"buttons": ["A"]}}])
        self.assertEqual(kit_library.check_script(out, self.table), [])
        # and it runs again as a replay
        ran = []
        r = kit_tools.replay([out], runner=lambda s, t, a: ran.append((s, t, a)), say=lambda _: None)
        self.assertEqual((r["ran"], r["failed"]), (2, []))
        self.assertEqual(ran[0], ("oot-p1", "step", {"frames": 4}))

    def test_keep_reads_keeps_the_looks(self):
        log = os.path.join(self.root, "calls.jsonl")
        kit_tools.CallLog(log, "oot-p1").write("get_state", {}, True, None, 1)
        out = os.path.join(self.root, "scripts", "looks.jsonl")
        self.assertEqual(kit_library.save_script([log], out, "A look.", "A", "x", "y", keep_reads=True), 1)

    def test_the_command_line(self):
        log = os.path.join(self.root, "calls.jsonl")
        kit_tools.CallLog(log, "oot-p1").write("step", {"frames": 2}, True, None, 1)
        out = os.path.join(self.root, "scripts", "cli.jsonl")
        self.assertEqual(kit_library.main(["save", log, "--out", out, "--what", "w", "--scene", "S", "--start", "s", "--expect", "e"]), 0)
        self.assertEqual(kit_library.main(["check", "--root", self.root]), 0)
        with open(out, "a") as f:
            f.write('{"server":"oot-p1","tool":"nope"}' + chr(10))
        self.assertEqual(kit_library.main(["check", "--root", self.root]), 1)


class Data(Folder):
    GOOD = {"kit": 1, "title": "A scene", "scene": "KokiriForest_Child",
            "positions": {"mido-door": {"x": 1.5, "y": 0, "z": -2, "yaw": 90, "note": "Talk here."}, "bridge": {"x": 4, "z": 5}},
            "routes": {"to-bridge": {"start": "mido-door", "steps": ["bridge"], "note": "Straight."}},
            "menu_paths": {"pause": {"from": "In play", "presses": ["PLUS", "A"], "note": "Opens the map."}},
            "stuck_spots": [{"x": 1, "z": 2, "radius": 1.5, "note": "Link stops.", "fix": "Go left."}],
            "notes": ["From a run."]}

    def check(self, **change):
        d = json.loads(json.dumps(self.GOOD))
        for k, v in change.items():
            if v is None:
                d.pop(k, None)
            else:
                d[k] = v
        return kit_library.check_data(self.data(d))

    def test_a_good_file_passes(self):
        self.assertEqual(self.check(), [])
        self.assertEqual(self.check(routes=None, menu_paths=None, stuck_spots=None, notes=None, title=None), [])

    def test_the_base_is_needed(self):
        self.says(self.check(scene=None), "scene is one map name")
        self.says(self.check(scene="two words"), "scene is one map name")
        self.says(self.check(kit=None), "kit is a number")
        self.says(self.check(kit=2), "kit is a number")
        self.says(self.check(speed=3), "no key speed")

    def test_a_bad_file_name_and_bad_json(self):
        self.says(kit_library.check_data(self.data(self.GOOD, "Bad Name.json")), "lowercase words")
        bad = os.path.join(self.root, "data", "broken.json")
        with open(bad, "w") as f:
            f.write("{not json")
        self.says(kit_library.check_data(bad), "cannot read as JSON")
        self.says(kit_library.check_data(self.data([1], "list.json")), "JSON object")

    def test_positions(self):
        self.says(self.check(positions={"a": {"x": 1}}), "has no z")
        self.says(self.check(positions={"a": {"x": "1", "z": 1}}), "positions.a.x: should be a number")
        self.says(self.check(positions={"a": {"x": 1, "z": 1, "yaw": 400}}), "yaw")
        self.says(self.check(positions={"a": {"x": float("inf"), "z": 1}}), "should be a number")
        self.says(self.check(positions={"a": {"x": 1, "z": 1, "color": "red"}}), "no key color")
        self.says(self.check(positions={"Bad Name": {"x": 1, "z": 1}}), "lowercase words")

    def test_routes_use_the_files_positions(self):
        self.says(self.check(routes={"r": {"start": "nowhere", "steps": ["bridge"]}}), "routes.r.start")
        self.says(self.check(routes={"r": {"start": "bridge", "steps": ["nowhere"]}}), "routes.r.steps[0]")
        self.says(self.check(routes={"r": {"start": "bridge", "steps": []}}), "routes.r.steps")

    def test_menu_paths_use_the_bridge_buttons(self):
        self.says(self.check(menu_paths={"m": {"from": "x", "presses": ["START"]}}), "is not one of")
        self.says(self.check(menu_paths={"m": {"from": "x", "presses": []}}), "list of buttons")
        self.says(self.check(menu_paths={"m": {"presses": ["A"]}}), "menu_paths.m.from")

    def test_stuck_spots(self):
        self.says(self.check(stuck_spots=[{"x": 1, "z": 1, "radius": 0, "note": "n"}]), "radius")
        self.says(self.check(stuck_spots=[{"x": 1, "z": 1, "radius": 1}]), "note")
        self.says(self.check(stuck_spots=[{"x": 1, "radius": 1, "note": "n"}]), "has no z")
        self.says(self.check(stuck_spots=["here"]), "should be an object")

    def test_a_pasted_log_is_refused(self):
        self.says(self.check(notes=["line " * 100]), "longer than")
        self.says(self.check(title="t" * 400), "longer than")


class WholeFolder(Folder):
    def test_check_all_reads_both_folders_and_refuses_strangers(self):
        self.script(HEAD + [STEP])
        self.data(Data.GOOD)
        with open(os.path.join(self.root, "scripts", "README.md"), "w") as f:
            f.write("# readme\n")
        self.assertEqual(kit_library.check_all(self.root), ([], 1, 1))
        for sub, name in (("scripts", "shot.png"), ("data", "calls.log")):
            with open(os.path.join(self.root, sub, name), "wb") as f:
                f.write(b"x")
        problems, _, _ = kit_library.check_all(self.root)
        self.says(problems, "scripts/shot.png: only .jsonl")
        self.says(problems, "data/calls.log: only .json")

    def test_a_missing_folder_is_said(self):
        shutil.rmtree(os.path.join(self.root, "data"))
        self.says(kit_library.check_all(self.root)[0], "data: the folder is missing")


class Docs(unittest.TestCase):
    """The pages that explain the library: their relative links go somewhere, and the entry points name the page."""
    REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))

    def read(self, rel):
        with open(os.path.join(self.REPO, rel), encoding="utf-8") as f:
            return f.read()

    def test_links_resolve(self):
        import re
        pages = ["docs/playtest-agents.md", "tools/playtest/README.md", "tools/playtest/scripts/README.md",
                 "tools/playtest/data/README.md"]
        for rel in pages:
            base = os.path.dirname(os.path.join(self.REPO, rel))
            for target in re.findall(r"\]\(([^)#\s]+)", self.read(rel)):
                if "://" in target:
                    continue
                self.assertTrue(os.path.exists(os.path.normpath(os.path.join(base, target))), "%s links %s" % (rel, target))

    def test_entry_points_name_the_agents_page(self):
        self.assertIn("playtest-agents.md", self.read("AGENTS.md"))
        self.assertIn("playtest-agents.md", self.read("docs/README.md"))


if __name__ == "__main__":
    unittest.main()
