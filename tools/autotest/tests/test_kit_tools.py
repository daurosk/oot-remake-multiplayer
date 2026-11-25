"""Tests for the play-test kit helpers that need no game (kit_tools.py): step_until's condition and wait, against the
fake bridge of test_playtest_bridge.py. Run: python -I tools/autotest/tests/test_kit_tools.py"""
import argparse
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import kit_tools  # noqa: E402
import live  # noqa: E402
import mcp_playtest  # noqa: E402
import playtest_client as pc  # noqa: E402
from test_playtest_bridge import FakeGame  # noqa: E402


class Condition(unittest.TestCase):
    def ev(self, text, state):
        return kit_tools.evaluate(kit_tools.parse_condition(text), state)

    def test_parse(self):
        self.assertEqual(kit_tools.parse_condition("event == false"), (["event"], "==", False, False))
        self.assertEqual(kit_tools.parse_condition("equips.weapon!=null"), (["equips", "weapon"], "!=", None, False))
        self.assertEqual(kit_tools.parse_condition("scene == KokiriForest_Child")[2], "KokiriForest_Child")
        self.assertEqual(kit_tools.parse_condition('scene == "A B"')[2], "A B")
        self.assertEqual(kit_tools.parse_condition("life <= 4")[1:3], ("<=", 4))
        self.assertEqual(kit_tools.parse_condition("!prompt"), (["prompt"], None, None, True))
        self.assertEqual(kit_tools.parse_condition("pos.1 > -2.5")[2], -2.5)
        for bad in ("", None, "== 3", "event ==", "!life < 3", "a b", "1x == 2", "pos. == 1"):
            with self.assertRaises(kit_tools.ConditionError, msg=repr(bad)):
                kit_tools.parse_condition(bad)

    def test_equal_and_not_equal(self):
        st = {"event": False, "life": 12, "scene": "A", "prompt": None, "pos": [1.0, 2.0, 3.0], "equips": {"weapon": "S"}}
        self.assertTrue(self.ev("event == false", st)[0])
        self.assertFalse(self.ev("event == true", st)[0])
        self.assertTrue(self.ev("life == 12.0", st)[0])           # 12 and 12.0 are one number
        self.assertFalse(self.ev("event == 0", st)[0])            # false is not 0
        self.assertFalse(self.ev("life == true", st)[0])          # 12 is not true
        self.assertTrue(self.ev("prompt == null", st)[0])
        self.assertTrue(self.ev("scene != B", st)[0])
        self.assertTrue(self.ev("equips.weapon == S", st)[0])
        self.assertTrue(self.ev("pos.2 == 3", st)[0])

    def test_ordering_needs_numbers_and_a_null_is_unknown(self):
        st = {"life": None, "rupees": 7, "scene": "A"}
        met, got, why = self.ev("life < 3", st)
        self.assertFalse(met)
        self.assertIn("needs numbers", why)
        self.assertFalse(self.ev("life >= 0", st)[0])             # not "0 or more": nobody knows
        self.assertFalse(self.ev("scene > 1", st)[0])
        self.assertTrue(self.ev("rupees > 6", st)[0])
        self.assertTrue(self.ev("rupees <= 7", st)[0])
        self.assertFalse(self.ev("rupees < 7", st)[0])

    def test_bare_field_is_truthy(self):
        st = {"prompt": "Talk", "event": False, "life": 0, "equips": {}}
        self.assertTrue(self.ev("prompt", st)[0])
        self.assertFalse(self.ev("event", st)[0])
        self.assertTrue(self.ev("!event", st)[0])
        self.assertTrue(self.ev("!life", st)[0])
        self.assertTrue(self.ev("!equips", st)[0])

    def test_a_missing_field_is_said(self):
        met, got, why = self.ev("evnt == false", {"event": False})
        self.assertFalse(met)
        self.assertIn("no field evnt", why)


class StepUntil(unittest.TestCase):
    def setUp(self):
        self.game = FakeGame()
        self.c = pc.Client(self.game.port)

    def tearDown(self):
        self.c.close()
        self.game.close()

    def steps(self):
        return [m for m in self.game.log if m["cmd"] == "step"]

    def test_a_condition_that_holds_costs_no_frame(self):
        r = kit_tools.step_until(self.c, "scene == ForestTemple")
        self.assertTrue(r["met"])
        self.assertEqual((r["frames_run"], r["value"]), (0, "ForestTemple"))
        self.assertEqual(self.steps(), [])

    def test_steps_in_chunks_until_the_scene_changes(self):
        self.game.scene_at = (1020, "DekuTree")
        r = kit_tools.step_until(self.c, "scene == DekuTree", max_frames=100)
        self.assertTrue(r["met"])
        self.assertEqual(r["frames_run"], 24)                      # 3 chunks of 8: the first look past frame 1020
        self.assertEqual([m["frames"] for m in self.steps()], [8, 8, 8])
        self.assertEqual(r["state"]["scene"], "DekuTree")
        self.assertEqual(self.c.last_frame, 1024)

    def test_chunk_is_the_look_interval(self):
        self.game.scene_at = (1020, "DekuTree")
        r = kit_tools.step_until(self.c, "scene == DekuTree", max_frames=100, chunk=3)
        self.assertEqual(r["frames_run"], 21)

    def test_times_out_and_never_runs_past_max_frames(self):
        r = kit_tools.step_until(self.c, "scene == Nowhere", max_frames=30)
        self.assertFalse(r["met"])
        self.assertEqual(r["frames_run"], 30)
        self.assertEqual([m["frames"] for m in self.steps()], [8, 8, 8, 6])
        self.assertIn("not met after 30 frames", r["why"])
        self.assertEqual(self.c.last_frame, 1030)

    def test_waits_for_a_prompt_and_for_an_event_to_end(self):
        self.game.event = True
        self.game.prompt = None
        r = kit_tools.step_until(self.c, "prompt", max_frames=16)
        self.assertFalse(r["met"])
        self.game.prompt = "Talk"
        self.assertTrue(kit_tools.step_until(self.c, "prompt", max_frames=16)["met"])
        r = kit_tools.step_until(self.c, "event == false", max_frames=8)
        self.assertFalse(r["met"])
        self.game.event = False
        self.assertTrue(kit_tools.step_until(self.c, "event == false", max_frames=8)["met"])

    def test_a_stale_snapshot_is_not_a_match(self):
        self.game.stale = True                                     # Link members are null: "do not know"
        r = kit_tools.step_until(self.c, "rupees >= 0", max_frames=8)
        self.assertFalse(r["met"])
        self.assertIn("needs numbers", r["why"])

    def test_a_typo_is_an_error_not_a_time_out(self):
        with self.assertRaises(kit_tools.ConditionError) as e:
            kit_tools.step_until(self.c, "sene == Nowhere")
        self.assertIn("no field sene", str(e.exception))
        self.assertIn("scene", str(e.exception))                   # the fields it does have
        self.assertEqual(self.steps(), [])

    def test_bounds(self):
        for kw in ({"max_frames": 0}, {"max_frames": 3601}, {"chunk": 0}, {"chunk": 61}):
            with self.assertRaises(ValueError):
                kit_tools.step_until(self.c, "event", **kw)


class StepUntilTool(unittest.TestCase):
    def setUp(self):
        self.game = FakeGame()
        self.tool = mcp_playtest.Game("p1", pc.Client(self.game.port), os.getcwd())

    def tearDown(self):
        self.tool.client.close()
        self.game.close()

    def test_the_tool_is_listed_and_runs(self):
        names = [t["name"] for t in mcp_playtest.PLAYER_TOOLS]
        self.assertIn("step_until", names)
        tool = mcp_playtest.PLAYER_TOOLS[names.index("step_until")]
        self.assertEqual(tool["inputSchema"]["required"], ["condition"])
        self.game.scene_at = (1010, "DekuTree")
        r = self.tool.call("step_until", {"condition": "scene == DekuTree", "max_frames": 60})
        self.assertTrue(r["met"])
        self.assertEqual(r["frames_run"], 16)
        self.assertNotIn("inventory", r["state"])                  # the MCP layer leaves the bulk out
        json.dumps(r)

    def test_bad_arguments_are_refused(self):
        with self.assertRaises(ValueError):
            self.tool.call("step_until", {})
        with self.assertRaises(ValueError):
            self.tool.call("step_until", {"condition": "event", "max_frames": 99999})
        with self.assertRaises(ValueError):
            self.tool.call("step_until", {"condition": "event", "chunk": 0})
        self.assertEqual([m for m in self.game.log if m["cmd"] == "step"], [])


class Delta(unittest.TestCase):
    A = {"frame": 100, "scene": "S", "pos": [1.0, 0.0, 2.0], "life": 12, "event": False, "snapshot_present": 99,
         "equips": {"weapon": "Kokiri", "shield": None}, "coop": {"players": [{"n": 1}]}, "inventory": {"X": 1}}

    def test_only_what_changed_and_never_the_counters(self):
        b = json.loads(json.dumps(self.A))
        b.update(frame=160, snapshot_present=159)
        self.assertEqual(kit_tools.delta(self.A, b), {"changed": {}, "removed": []})   # nothing but the counters moved
        b["pos"] = [1.5, 0.0, 2.0]
        b["equips"]["shield"] = "Deku"
        b["inventory"] = {"X": 2}
        d = kit_tools.delta(self.A, b)
        self.assertEqual(d["changed"], {"pos": [1.5, 0.0, 2.0], "equips.shield": "Deku"})   # a list whole, an object by path
        self.assertEqual(d["removed"], [])

    def test_new_and_removed_fields(self):
        b = json.loads(json.dumps(self.A))
        del b["life"]
        b["strike"] = {"seq": 1}
        b["equips"] = {}
        d = kit_tools.delta(self.A, b)
        self.assertEqual(d["changed"], {"strike.seq": 1, "equips": {}})
        self.assertEqual(d["removed"], ["equips.shield", "equips.weapon", "life"])

    def test_history_gives_whole_then_delta_and_keeps_sixteen(self):
        h = kit_tools.StateHistory()
        first = h.reply({"A": dict(self.A)})["A"]
        self.assertEqual((first["state_id"], first["scene"]), (1, "S"))
        moved = dict(self.A, frame=130, life=8)
        r = h.reply({"A": moved}, since=1)["A"]
        self.assertEqual((r["state_id"], r["since"], r["frame"], r["changed"]), (2, 1, 130, {"life": 8}))
        self.assertNotIn("scene", r)
        r = h.reply({"A": moved}, since=2)["A"]                     # nothing moved
        self.assertEqual((r["changed"], r["removed"]), ({}, []))
        for _ in range(20):
            h.add({"A": moved})
        self.assertEqual(h.kept(), (8, 23))
        r = h.reply({"A": moved}, since=1)["A"]                     # too old: the whole state, and why
        self.assertEqual(r["scene"], "S")
        self.assertIn("no look 1 kept", r["why"])
        self.assertIn("9 to 24", r["why"])   # (the reply itself kept look 24)
        r = h.reply({"A": moved}, since=999)["A"]
        self.assertIn("no look 999 kept", r["why"])

    def test_a_player_the_old_look_did_not_have_gets_the_whole_state(self):
        h = kit_tools.StateHistory()
        h.add({"A": dict(self.A)})
        r = h.reply({"A": dict(self.A), "B": dict(self.A)}, since=1)
        self.assertIn("changed", r["A"])
        self.assertEqual(r["B"]["scene"], "S")
        self.assertIn("why", r["B"])

    def test_the_log_lines_always_come_whole_and_are_not_kept(self):
        h = kit_tools.StateHistory()
        h.add({"A": dict(self.A, log=["coop: one"])})
        self.assertNotIn("log", h.get(1)["A"])
        r = h.reply({"A": dict(self.A, log=["coop: two"])}, since=1)["A"]
        self.assertEqual((r["changed"], r["log"]), ({}, ["coop: two"]))

    def test_the_history_file_carries_over_between_runs(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "state-history.json")
            self.assertEqual(kit_tools.StateHistory(path).add({"A": dict(self.A)}), 1)
            h = kit_tools.StateHistory(path)                       # (live.py runs once per call)
            self.assertEqual(h.reply({"A": dict(self.A, life=4)}, since=1)["A"]["changed"], {"life": 4})
            with open(path, "w", encoding="utf-8") as f:
                f.write("{cut")
            self.assertEqual(kit_tools.StateHistory(path).add({"A": {}}), 1)   # a broken file starts again, no crash


class DeltaTools(unittest.TestCase):
    def test_get_state_since_on_a_player_server(self):
        game = FakeGame()
        tool = mcp_playtest.Game("p1", pc.Client(game.port), os.getcwd())
        try:
            whole = tool.call("get_state", {})
            self.assertEqual(whole["state_id"], 100001)   # (p1 counts from 100000)
            self.assertEqual(whole["scene"], "ForestTemple")
            same = tool.call("get_state", {"since": 100001})
            self.assertEqual((same["changed"], same["removed"], same["state_id"]), ({}, [], 100002))   # a held game: no change
            game.rupees = 9
            game.prompt = "Talk"
            r = tool.call("get_state", {"since": 100002, "inventory": True})
            self.assertEqual(r["changed"], {"rupees": 9, "prompt": "Talk"})
            self.assertEqual(r["frame"], 1000)
            self.assertNotIn("scene", r)
            for bad in (0, -1, "1", True, 1.5):
                with self.assertRaises(ValueError, msg=repr(bad)):
                    tool.call("get_state", {"since": bad})
            self.assertEqual(tool.call("get_state", {})["state_id"], 100004)   # a refused call took no id
        finally:
            tool.client.close()
            game.close()

    def test_an_id_from_another_players_server_is_not_kept(self):
        game, other = FakeGame(), FakeGame()
        p1 = mcp_playtest.Game("p1", pc.Client(game.port), os.getcwd())
        p2 = mcp_playtest.Game("p2", pc.Client(other.port), os.getcwd())
        try:
            id1 = p1.call("get_state", {})["state_id"]
            p2.call("get_state", {})
            r = p2.call("get_state", {"since": id1})   # p1's id on p2's server: the whole state and a why
            self.assertEqual(r["scene"], "ForestTemple")
            self.assertIn("why", r)
            self.assertNotEqual(id1, r["state_id"])
        finally:
            p1.client.close()
            p2.client.close()
            game.close()
            other.close()

    def test_live_state_since_and_the_cli_flag(self):
        looks = iter([{"map": "M", "life": 12, "log": ["coop: a"]}, {"map": "M", "life": 8, "log": []}])
        with tempfile.TemporaryDirectory() as d:
            live.save(d, {"games": {"A": {}}, "offsets": {}})
            saved = live.look
            live.look = lambda d, s, x: dict(next(looks))
            try:
                first = live.state(argparse.Namespace(session=d, player="A"))
                self.assertEqual((first["A"]["state_id"], first["A"]["life"]), (1, 12))
                r = live.state(argparse.Namespace(session=d, player="A", since=1))
            finally:
                live.look = saved
            self.assertEqual((r["A"]["changed"], r["A"]["since"], r["A"]["log"]), ({"life": 8}, 1, []))
            self.assertTrue(os.path.isfile(os.path.join(d, "state-history.json")))
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):   # --since takes a number
            live.main(["state", "--session", "x", "--since", "soon"])

    def test_the_session_state_tool_takes_since(self):
        props = next(t for t in mcp_playtest.TOOLS if t["name"] == "state")["inputSchema"]["properties"]
        self.assertIn("since", props)
        with self.assertRaises(ValueError):
            mcp_playtest.call("x", "state", {"since": 0})


def serve_lines(game, calllog, requests):
    """Runs mcp_playtest.serve on the requests (tool, args) and returns the replies' results."""
    lines = [json.dumps({"jsonrpc": "2.0", "id": i + 1, "method": "tools/call", "params": {"name": n, "arguments": a}})
             for i, (n, a) in enumerate(requests)]
    stdin, stdout = sys.stdin, sys.stdout
    sys.stdin, sys.stdout = io.StringIO("\n".join(lines) + "\n"), io.StringIO()
    try:
        mcp_playtest.serve(None, game, calllog=calllog)
        out = sys.stdout.getvalue()
    finally:
        sys.stdin, sys.stdout = stdin, stdout
    return [json.loads(x)["result"] for x in out.splitlines()]


class CallLogTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "calls.jsonl")

    def tearDown(self):
        self.dir.cleanup()

    def lines(self):
        with open(self.path, encoding="utf-8") as f:
            return [json.loads(x) for x in f]

    def test_a_line_per_call(self):
        log = kit_tools.CallLog(self.path, "oot-p1")
        log.write("step", {"frames": 5}, t=10.5)
        log.write("warp", {"scene": "X"}, False, "ValueError: no scene " + "x" * 400, t=11)
        a, b = self.lines()
        self.assertEqual(a, {"t": 10.5, "server": "oot-p1", "tool": "step", "args": {"frames": 5}, "ok": True})
        self.assertFalse(b["ok"])
        self.assertTrue(b["error"].startswith("ValueError: no scene"))
        self.assertEqual(len(b["error"]), 200)

    def test_an_unwritable_log_never_stops_a_test(self):
        kit_tools.CallLog(os.path.join(self.dir.name, "no", "such", "calls.jsonl"), "live").write("act", {})

    def test_read_merges_files_by_time_and_skips_junk(self):
        other = os.path.join(self.dir.name, "p2.jsonl")
        kit_tools.CallLog(self.path, "oot-p1").write("step", {"frames": 1}, t=3)
        kit_tools.CallLog(other, "oot-p2").write("step", {"frames": 2}, t=1)
        kit_tools.CallLog(self.path, "oot-p1").write("step", {"frames": 3}, t=2)
        with open(self.path, "a", encoding="utf-8") as f:
            f.write('{"t":9,"server":"evil","tool":"step"}\n{"t":9,"server":"oot-p1"}\n[1]\n{"t": 5, "server": "oot-p1", "to')
        calls = kit_tools.read_calls([self.path, other])
        self.assertEqual([c["args"]["frames"] for c in calls], [2, 3, 1])
        self.assertEqual(calls[0]["_at"], "p2.jsonl:1")
        self.assertEqual(calls[2]["_at"], "calls.jsonl:1")

    def test_a_t_that_is_not_a_number_does_not_stop_the_read(self):
        with open(self.path, "w", encoding="utf-8") as f:
            for t, n in (('"soon"', 1), ("null", 2), ("true", 3), ("4", 4)):
                f.write('{"t":%s,"server":"oot-p1","tool":"step","args":{"frames":%d}}\n' % (t, n))
        self.assertEqual([c["args"]["frames"] for c in kit_tools.read_calls(self.path)], [1, 2, 3, 4])

    def test_serve_logs_calls_and_failures(self):
        game = FakeGame()
        tool = mcp_playtest.Game("p1", pc.Client(game.port), self.dir.name)
        try:
            res = serve_lines(tool, kit_tools.CallLog(self.path, "oot-p1"),
                              [("step", {"frames": 4}), ("get_state", {}), ("step", {}),
                               ("step_until", {"condition": "event == false"})])
        finally:
            tool.client.close()
            game.close()
        self.assertEqual([bool(r.get("isError")) for r in res], [False, False, True, False])
        got = self.lines()
        self.assertEqual([(c["tool"], c["ok"]) for c in got],
                         [("step", True), ("get_state", True), ("step", False), ("step_until", True)])
        self.assertEqual(got[0]["args"], {"frames": 4})
        self.assertEqual(got[0]["server"], "oot-p1")
        self.assertIn("error", got[2])
        self.assertEqual(got, sorted(got, key=lambda c: c["t"]))

    def test_serve_without_a_log_writes_none(self):
        game = FakeGame()
        tool = mcp_playtest.Game("p1", pc.Client(game.port), self.dir.name)
        try:
            serve_lines(tool, None, [("step", {"frames": 1})])
        finally:
            tool.client.close()
            game.close()
        self.assertFalse(os.path.exists(self.path))

    def test_the_default_place(self):
        self.assertEqual(mcp_playtest.call_log_path("S", "L"), os.path.join("S", "calls.jsonl"))
        self.assertEqual(mcp_playtest.call_log_path(None, "L"), os.path.join("L", "calls.jsonl"))


class Replay(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "calls.jsonl")

        def w(server, tool, args, ok=True, t=0):
            kit_tools.CallLog(self.path, server).write(tool, args, ok, None, t)
        w("oot-p1", "warp", {"scene": "A"}, t=1)
        w("oot-p1", "get_state", {}, t=2)
        w("oot-p2", "step", {"frames": 3}, False, t=3)
        w("live", "act", {"player": "A", "script": "give bow"}, t=4)
        w("oot-p1", "step", {"frames": 9}, t=5)
        self.ran = []
        self.said = []

    def tearDown(self):
        self.dir.cleanup()

    def runner(self, server, tool, args):
        self.ran.append((server, tool, args))

    def go(self, **kw):
        return kit_tools.replay([self.path], runner=self.runner, say=self.said.append, **kw)

    def test_runs_what_changed_the_game_in_order(self):
        r = self.go()
        self.assertEqual(self.ran, [("oot-p1", "warp", {"scene": "A"}),
                                    ("live", "act", {"player": "A", "script": "give bow"}),
                                    ("oot-p1", "step", {"frames": 9})])
        self.assertEqual((r["ran"], r["skipped"], r["failed"]), (3, 2, []))   # the look and the failed step stay out
        self.assertTrue(self.said[0].startswith("calls.jsonl:1 oot-p1 warp "))

    def test_include_reads_and_failed(self):
        r = self.go(include_reads=True, include_failed=True)
        self.assertEqual((r["ran"], r["skipped"]), (5, 0))
        self.assertEqual([x[1] for x in self.ran], ["warp", "get_state", "step", "act", "step"])

    def test_dry_run_runs_nothing(self):
        r = self.go(dry_run=True)
        self.assertEqual((self.ran, r["ran"]), ([], 3))
        self.assertEqual(len(self.said), 3)

    def test_record_route_and_look_are_never_run_again(self):
        self.assertIn("record_route", kit_tools.NOT_REPLAYED)
        self.assertIn("look", kit_tools.READ_ONLY)

    def test_stops_at_the_first_failure_unless_told_to_go_on(self):
        def boom(server, tool, args):
            self.ran.append(tool)
            if tool == "act":
                raise ValueError("no game")
        r = kit_tools.replay([self.path], runner=boom, say=self.said.append)
        self.assertEqual(self.ran, ["warp", "act"])
        self.assertEqual((r["ran"], len(r["failed"])), (1, 1))
        self.assertEqual(r["failed"][0], ["calls.jsonl:4", "ValueError: no game"])
        self.ran.clear()
        r = kit_tools.replay([self.path], runner=boom, say=self.said.append, keep_going=True)
        self.assertEqual(self.ran, ["warp", "act", "step"])
        self.assertEqual((r["ran"], len(r["failed"])), (2, 1))

    def test_the_default_runner_drives_each_games_bridge(self):
        game = FakeGame()
        saved = pc.port_for
        pc.port_for = lambda player, lane=None: game.port
        try:
            r = kit_tools.replay([self.path], session=None, lane=3, log_dir=self.dir.name, keep_going=True,
                                 say=self.said.append)
        finally:
            pc.port_for = saved
            game.close()
        steps = [m for m in game.log if m["cmd"] == "step"]
        self.assertEqual([m["frames"] for m in steps], [9])        # the bridge got the step that had worked
        self.assertEqual(len(r["failed"]), 1)                      # live act: it needs a --session
        self.assertIn("--session", r["failed"][0][1])


class ActRefusesFace(unittest.TestCase):
    def test_a_face_line_is_refused_and_nothing_is_sent(self):
        saved = live.do
        seen = []
        live.do = lambda a: seen.append(a.script) or {"ok": True}
        try:
            for script in ("face 1 2", "wait 5; face 1 2", "wait 5\n  FACE 1 2  # turn"):
                with self.assertRaises(ValueError) as e:
                    mcp_playtest.call("x", "act", {"player": "A", "script": script})
                self.assertIn("`face` tool", str(e.exception))
            self.assertEqual(seen, [])
            mcp_playtest.call("x", "act", {"player": "A", "script": "wait 5; place 1 2 3"})   # other lines still go
            self.assertEqual(seen, ["wait 5; place 1 2 3"])
            self.assertNotIn("`face X Z`", mcp_playtest.SCRIPT_HELP)
        finally:
            live.do = saved


class LiveCli(unittest.TestCase):
    def test_cli_calls_are_logged_and_replayed(self):
        with tempfile.TemporaryDirectory() as d:
            saved = live.do, live.state
            seen = []
            live.do = lambda a: {"ok": True}
            live.state = lambda a: {"A": {"map": "M"}}
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    live.main(["do", "--session", d, "A", "give bow"])
                    live.main(["state", "--session", d, "B", "--since", "3"])
                    live.do = lambda a: {"error": "no such line"}
                    self.assertEqual(live.main(["do", "--session", d, "B", "bad"]), 1)
                calls = kit_tools.read_calls(os.path.join(d, "calls.jsonl"))
                self.assertEqual([(c["server"], c["tool"], c["args"], c["ok"]) for c in calls],
                                 [("live", "act", {"player": "A", "script": "give bow"}, True),
                                  ("live", "state", {"player": "B", "since": 3}, True),
                                  ("live", "act", {"player": "B", "script": "bad"}, False)])
                live.do = lambda a: seen.append((a.player, a.script)) or {"ok": True}
                err = io.StringIO()
                with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(err):
                    code = live.main(["replay", "--session", d])
                self.assertEqual(code, 0)
                self.assertEqual(json.loads(out.getvalue())["ran"], 1)
                self.assertEqual(seen, [("A", "give bow")])   # only the call that worked, not the look
                self.assertIn("live act", err.getvalue())
                self.assertEqual(len(kit_tools.read_calls(os.path.join(d, "calls.jsonl"))), 3)   # a replay is not logged
            finally:
                live.do, live.state = saved

    def test_replay_needs_a_file_or_a_session(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                live.main(["replay"])


if __name__ == "__main__":
    unittest.main()
