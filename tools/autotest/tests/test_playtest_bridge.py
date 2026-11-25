"""Tests for the outside half of the play-test bridge, against a fake game behind a real 127.0.0.1 socket speaking the
bridge protocol (docs/playtest.md, "Bridge protocol"): the bridge client (playtest_client.py: one connection, the
hold, reconnects), walk_to's camera-relative steering, every tool of one game's MCP server (mcp_playtest.py --player)
and desync_check.py --clients. Run: python -I tools/autotest/tests/test_playtest_bridge.py"""
import io
import json
import math
import os
import socketserver
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import argparse  # noqa: E402
import desync_check  # noqa: E402
import kit_tools  # noqa: E402
import live  # noqa: E402
import mcp_playtest  # noqa: E402
import playtest_client as pc  # noqa: E402
import playtest_helpers as ph  # noqa: E402


class FakeGame:
    """A game's play-test bridge: held, it runs only the frames a command asks for; a closed connection lets it run.
    Link walks 0.1 m a frame at full stick toward world yaw camera + bias + sign * stick angle for an `input`, and
    toward the world yaw itself for a `move` (the module turns it into the stick with the frame's camera).
    `camera` None: the state reports no camera. `move_camera` False: a `move` is refused with no_camera."""

    def __init__(self, camera=0.0, sign=1, bias=0.0, pos=(0.0, 0.0, 0.0), scene="ForestTemple", dump_dir=None):
        self.camera, self.sign, self.bias = camera, sign, bias
        self.pos, self.scene, self.frame, self.paused = list(pos), scene, 1000, False
        self.flags, self.items, self.log = {"F1": 0, "F2": 0}, {}, []
        self.dump_dir = dump_dir
        self.world = None       # the `world` member of the state (None: this module has none)
        self.wall_x = None      # Link cannot pass this x
        self.scene_at = None    # (frame, scene): the scene changes once the frame count reaches it
        self.connections = 0
        self.rupees, self.prompt = 5, None
        self.on_press = None    # called with the buttons of each `input`
        self.dump_hook = None   # called at each `dump`
        self.solo = False       # kill/hit with an actor id are refused (no world list)
        self.strike, self.pending, self.strike_seq = None, [], 0     # `state.strike`, results on their way, last number
        self.strike_word, self.strike_delay = "done", 2   # the walk's answer (None: it never comes) and how many frames
        self.strike_seq_in_reply = True
        self.loading, self.queue_full = False, False
        self.stale = False      # Longshot's Link snapshot is old (held on a title, a pause, a load): Link members null
        self.event = False      # an event runs: the module's warp (with an entrance) is taken only then
        self.no_teleport = {"BossStage"}   # maps Longshot's teleport list does not have
        self.via = None
        self.flip_at = None     # (frame): the stick's sign turns over from this frame on (a model that changes under the walk)
        self.move_camera = True  # the module has a camera to turn a `move`'s yaw into the stick
        self.warp_frames = 0    # frames a warp runs before it answers
        self.cut = None         # an input or a step runs at most this many frames (cut short: an event, a load)
        self.away_from = None   # (x, z): Link ignores the stick and walks directly away from this point
        self.faced = None       # the (x, z) of the last `face`
        self.facing = 0.0       # Link's facing: the stick turns him to where it walks him
        self.kill_seqs, self.deaths, self.death_delay = set(), [], 3   # a struck kill's actor dies this many frames later
        game = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                game.connections += 1
                for line in self.rfile:
                    msg = json.loads(line)
                    game.log.append(msg)
                    reply = dict(game.answer(msg), id=msg.get("id"), frame=game.frame, paused=game.paused)
                    self.wfile.write((json.dumps(reply) + "\n").encode())
                game.paused = False     # the hold lifts when the tester's connection closes

        self.server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, args=(0.05,), daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def run(self, frames, stick=None, yaw=None):
        for _ in range(frames):
            self.frame += 1
            if self.flip_at is not None and self.frame == self.flip_at:
                self.sign = -self.sign
            if self.away_from is not None and stick and stick[1] > 0:
                dx, dz = self.pos[0] - self.away_from[0], self.pos[2] - self.away_from[1]
                n = math.hypot(dx, dz) or 1.0
                self.pos[0] += 0.1 * dx / n
                self.pos[2] += 0.1 * dz / n
            elif stick and stick[1] > 0:
                w = math.radians(yaw if yaw is not None else (self.camera or 0.0) + self.bias + self.sign * stick[0])
                self.facing = math.degrees(w) % 360.0
                nx = self.pos[0] + 0.1 * stick[1] * math.sin(w)
                if self.wall_x is None or nx < self.wall_x:
                    self.pos[0] = nx
                self.pos[2] += 0.1 * stick[1] * math.cos(w)
            if self.scene_at and self.frame >= self.scene_at[0]:
                self.scene = self.scene_at[1]
            for at, rec in [p for p in self.pending if self.frame >= p[0]]:
                self.strike = rec
                self.pending.remove((at, rec))
                if rec["seq"] in self.kill_seqs and rec["result"] == "done" and self.death_delay is not None:
                    self.deaths.append((self.frame + self.death_delay, rec["id"]))
            for at, ident in [d for d in self.deaths if self.frame >= d[0]]:
                self.deaths.remove((at, ident))
                for a in (self.world or {}).get("actors", []):
                    if a["id"] == ident:
                        a["alive"], a["life"] = False, 0

    def state(self):
        link = {"pos": None, "facing": None, "camera": None, "life": None, "max_life": None, "rupees": None,
                "age": None, "event": None, "loading": None, "prompt": None} if self.stale else {
            "pos": [round(v, 4) for v in self.pos], "facing": round(self.facing, 2), "camera": self.camera, "life": 12, "max_life": 12,
            "rupees": self.rupees, "age": "child", "event": self.event, "loading": self.loading, "prompt": self.prompt}
        return {"scene": self.scene, "room": None, **link, "equips": {}, "inventory": dict(self.items),
                "coop": {"state": "connected", "players": 2}, "snapshot_present": self.frame - (500 if self.stale else 0),
                "snapshot_stale": self.stale,
                "not_ready": "the Link sample is older than the frame" if self.stale else None,
                **({"world": self.world} if self.world is not None else {}),
                **({"strike": self.strike} if self.strike is not None else {})}

    def answer(self, m):
        cmd = m.get("cmd")
        if cmd == "hello":
            return {"ok": True, "protocol": 1, "control": True}
        if cmd == "pause":
            self.paused = True
            return {"ok": True}
        if cmd == "resume":
            self.paused = False
            return {"ok": True}
        if cmd == "state":
            return dict(self.state(), ok=True)
        if cmd in ("input", "step"):
            start = self.frame
            if cmd == "input" and self.on_press and m.get("buttons"):
                self.on_press(m["buttons"])
            frames = m.get("frames", 1) if self.cut is None else min(self.cut, m.get("frames", 1))
            self.run(frames, m.get("stick"))
            self.paused = True
            return {"ok": True, "from": start, "state": self.state()}
        if cmd == "move":
            if not self.move_camera:
                return {"ok": False, "error": "no_camera", "message": "no game camera to steer by"}
            start = self.frame
            self.run(m.get("frames", 1), [0.0, m.get("strength", 1.0)], yaw=m["yaw"])
            self.paused = True
            cam = self.camera or 0.0
            return {"ok": True, "from": start, "yaw": m["yaw"], "reads": m.get("frames", 1), "no_camera": 0,
                    "stick": [round((m["yaw"] - cam) % 360.0, 2), m.get("strength", 1.0)], "view": cam,
                    "state": self.state()}
        if cmd == "warp":
            # free play: Longshot's teleport (no start position, its own list of maps), else the module's map change event
            self.via = "event" if self.event else "map_event" if (m.get("entrance") or m["scene"] in self.no_teleport) else "teleport"
            self.run(self.warp_frames)
            self.scene = m["scene"]
            self.pos = [3.0, 0.0, -3.0]   # a load puts Link at the scene's start
            self.paused = True
            return {"ok": True, "arrived": True, "state": self.state()}
        if cmd == "flag":
            was = self.flags.get(m["name"])
            self.flags[m["name"]] = m["value"]
            return {"ok": True, "was": was, "value": m["value"]}
        if cmd == "flag_read":
            v = self.flags.get(m["name"])
            return {"ok": True, "name": m["name"], "kind": "unknown" if v is None else "int", "value": v}
        if cmd == "give":
            self.items[m["item"]] = self.items.get(m["item"], 0) + 1
            return {"ok": True, "given": m["item"]}
        if cmd == "act":
            reply = {"ok": True, "lines": m["lines"]}
            words = m["lines"].split()
            if words and words[0] == "place" and len(words) == 4:
                self.pos = [float(w) for w in words[1:]]
            if words and words[0] == "face" and len(words) == 3:
                self.faced = (float(words[1]), float(words[2]))
            if words and words[0] in ("kill", "hit"):
                if len(words) > 1 and words[1].isdigit() and self.solo:
                    return {"ok": False, "error": "refused", "message": "no world list: an actor id is resolved in "
                            "the objects walk, which runs only while connected and sharing"}
                if self.queue_full:
                    return {"ok": False, "error": "refused", "message": "the strike queue is full (16 wait for the "
                            "walk): step a few frames and send it again"}
                self.strike_seq += 1
                if words[0] == "kill":
                    self.kill_seqs.add(self.strike_seq)
                has_id = words[1].isdigit()
                if self.strike_word is not None:
                    self.pending.append((self.frame + self.strike_delay, {
                        "seq": self.strike_seq, "result": self.strike_word, "name": words[2 if has_id else 1],
                        "id": int(words[1]) if has_id else 0}))
                if self.strike_seq_in_reply:
                    reply["strike_seq"] = self.strike_seq
            return reply
        if cmd == "dump":
            if self.dump_hook:
                self.dump_hook()
            path = os.path.join(self.dump_dir, "%s.json" % m["tag"])
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"frame": self.frame, "tag": m["tag"], "state": self.state(), "flags": self.flags}, f)
            return {"ok": True, "path": path, "flags": len(self.flags)}
        if cmd == "shot":
            if "path" in m:
                return {"ok": False, "error": "bad_request", "message": "no client paths"}
            self.run(1)
            folder = os.path.join(self.dump_dir, "playtest", "shots")
            os.makedirs(folder, exist_ok=True)
            path = os.path.join(folder, "%s.png" % m.get("name", "shot"))
            with open(path, "wb") as f:
                f.write(b"PNG")
            return {"ok": True, "path": path, "width": 1280, "height": 720, "advanced": 1}
        return {"ok": False, "error": "bad_request", "message": "unknown cmd %s" % cmd}


class BridgeClient(unittest.TestCase):
    def setUp(self):
        self.game = FakeGame()
        self.c = pc.Client(self.game.port)

    def tearDown(self):
        self.c.close()
        self.game.close()

    def test_one_connection_held_from_the_first_call(self):
        a = self.c.state()
        self.assertTrue(self.game.paused)                    # held before the first command was answered
        self.assertEqual(self.game.log[0]["cmd"], "pause")
        self.assertEqual(self.c.state()["frame"], a["frame"])   # held: two looks, one frame
        r = self.c.step(60)
        self.assertEqual(r["frame"] - r["from"], 60)
        self.assertEqual(self.c.last_frame, a["frame"] + 60)
        self.assertEqual(self.game.connections, 1)
        ids = [m["id"] for m in self.game.log]
        self.assertEqual(ids, sorted(set(ids)))

    def test_closing_lets_the_game_run_and_a_reconnect_holds_it_again(self):
        self.c.state()
        self.c.close()
        deadline = time.monotonic() + 5
        while self.game.paused and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertFalse(self.game.paused)
        r = self.c.state()
        self.assertTrue(r.get("reconnected"))
        self.assertTrue(self.game.paused)
        self.assertEqual(self.game.connections, 2)

    def test_refusal_raises_with_the_code(self):
        with self.assertRaises(pc.BridgeError) as e:
            self.c.request("nope")
        self.assertEqual(e.exception.code, "bad_request")
        self.assertFalse(self.c.request("nope", check=False)["ok"])

    def test_bad_frames_refused_here(self):
        for n in (0, 3601):
            with self.assertRaises(ValueError):
                self.c.step(n)

    def test_local_only(self):
        with self.assertRaises(ValueError):
            pc.Client(5001, host="192.168.1.2")

    def test_ports(self):
        self.assertEqual([pc.port_for(p) for p in pc.PLAYERS], [5001, 5002, 5003, 5004])
        self.assertEqual(pc.port_for("P2", lane=1), 5012)
        self.assertEqual(pc.port_for("p4", lane=2), 5024)
        with self.assertRaises(ValueError):
            pc.port_for("p5")


class WalkTo(unittest.TestCase):
    def walk(self, game, x, z, **kw):
        c = pc.Client(game.port)
        try:
            return pc.walk_to(c, x, z, **kw)
        finally:
            c.close()
            game.close()

    def test_arrives_for_any_camera(self):
        for cam in (0.0, 90.0, 200.0, -135.0):
            r = self.walk(FakeGame(camera=cam), 6.0, -4.0)
            self.assertTrue(r["arrived"], (cam, r["reason"], r["distance"]))
            self.assertLessEqual(r["distance"], 0.5)

    def test_walks_straight_in_every_direction_by_world_yaw(self):
        # The stick's sign and offset belong to the module (`move`): the walk goes the right way from its first push,
        # whatever a stick would have needed (a mirrored stick, an offset), in all four quadrants.
        for sign, bias in ((-1, 0.0), (1, 40.0), (-1, -120.0), (1, 180.0)):
            for tx, tz in ((5.0, 7.0), (-5.0, 7.0), (-5.0, -7.0), (5.0, -7.0)):
                game = FakeGame(camera=30.0, sign=sign, bias=bias)
                r = self.walk(game, tx, tz, tolerance=0.3, max_frames=1500)
                self.assertTrue(r["arrived"], (sign, bias, tx, tz, r["reason"], r["distance"]))
                self.assertLessEqual(r["frames"], 104)       # 8.6 m in a straight line: no wrong-way pushes
                self.assertEqual([m for m in game.log if m["cmd"] == "input"], [])

    def test_a_refused_move_stops_the_walk_and_says_why(self):
        game = FakeGame()
        game.move_camera = False
        r = self.walk(game, 4.0, 4.0)
        self.assertFalse(r["arrived"])
        self.assertTrue(r["reason"].startswith("move refused: no_camera"), r["reason"])
        self.assertEqual(r["frames"], 0)

    def test_no_camera_in_the_state(self):
        self.assertTrue(self.walk(FakeGame(camera=None, bias=70.0), 4.0, 4.0, max_frames=900)["arrived"])

    def test_held_between_chunks_and_frames_counted(self):
        game = FakeGame()
        r = self.walk(game, 3.0, 0.0, tolerance=0.2)
        self.assertTrue(r["arrived"])
        moves = [m for m in game.log if m["cmd"] == "move"]
        self.assertTrue(moves and all(m["frames"] <= 10 for m in moves))
        self.assertEqual(sum(m["frames"] for m in moves), r["frames"])
        self.assertTrue(all(-180.0 <= m["yaw"] < 180.0 and 0.0 < m["strength"] <= 1.0 for m in moves))
        self.assertAlmostEqual(moves[0]["yaw"], 90.0)                    # +x: world yaw 90, whatever the camera

    def test_a_stale_snapshot_is_not_an_arrival(self):               # F1: null pos, never an old place
        game = FakeGame()
        game.stale = True
        r = self.walk(game, 0.0, 0.0)
        self.assertFalse(r["arrived"])
        self.assertTrue(r["reason"].startswith("stale position"), r["reason"])

    def test_a_missing_first_sample_gives_its_reason_not_a_silent_null(self):    # S10 (d): fresh hold, no sample yet
        why = pc.no_position_reason({"pos": None, "snapshot_stale": False,
                                     "not_ready": "no Link sample has been taken yet"})
        self.assertTrue(why.startswith("stale position: not ready: no Link sample"), why)
        self.assertEqual(pc.no_position_reason({"pos": None, "snapshot_stale": False, "not_ready": None}),
                         "no position in the state (not in play?)")

    def test_stops_on_a_scene_change(self):
        game = FakeGame()
        game.scene_at = (1030, "ForestTemple_Room2")
        r = self.walk(game, 50.0, 0.0)
        self.assertFalse(r["arrived"])
        self.assertEqual(r["reason"], "scene changed")

    def test_times_out_against_a_wall(self):
        game = FakeGame()
        game.wall_x = 1.0
        r = self.walk(game, 5.0, 0.0, max_frames=200)
        self.assertFalse(r["arrived"])
        self.assertEqual(r["reason"], "timed out")
        self.assertEqual(r["frames"], 200)


class PlayerServer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.games = {p: FakeGame(dump_dir=self.tmp.name if p == "p1" else tempfile.mkdtemp(dir=self.tmp.name))
                      for p in ("p1", "p2")}
        self.tools = {p: mcp_playtest.Game(p, pc.Client(g.port), self.tmp.name) for p, g in self.games.items()}

    def tearDown(self):
        for t in self.tools.values():
            t.client.close()
        for g in self.games.values():
            g.close()
        self.tmp.cleanup()

    def call(self, p, tool, **args):
        return self.tools[p].call(tool, args)

    def test_tools_listed_and_named_per_player(self):
        reqs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                 "params": {"name": "step", "arguments": {"frames": 5}}},
                {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "state", "arguments": {}}}]
        stdin, stdout = sys.stdin, sys.stdout
        sys.stdin = io.StringIO("".join(json.dumps(r) + "\n" for r in reqs))
        sys.stdout = io.StringIO()
        try:
            mcp_playtest.serve(None, self.tools["p2"])
            out = [json.loads(ln) for ln in sys.stdout.getvalue().splitlines()]
        finally:
            sys.stdin, sys.stdout = stdin, stdout
        self.assertEqual(out[0]["result"]["serverInfo"]["name"], "oot-p2")
        self.assertEqual([t["name"] for t in out[1]["result"]["tools"]],
                         ["get_state", "get_world", "look", "wait_until", "go_to", "record_route", "fight", "get_actor", "walk_to", "press", "step", "step_until", "warp", "flag", "flag_read", "setup", "preset", "save_point", "load_point", "give", "kill", "hit", "hurt", "place", "wear",
                          "dress", "assign", "chat", "menu", "go_to_actor", "face", "attack", "use_item", "talk", "get_text", "open",
                          "interact", "shoot", "climb", "push", "jump", "checkpoint",
                          "desync_check", "pause", "resume", "log", "check_logs", "session_status", "screenshot"])
        self.assertEqual(json.loads(out[2]["result"]["content"][0]["text"])["frame"], 1005)
        self.assertTrue(out[3]["result"]["isError"])          # the session tools are not a game's tools

    WORLD = {"frame": 1000, "age_ms": 40, "stale": False, "behind": False, "total": 5, "other_count": 1, "count": 4,
             "actors": [
                 {"id": 7, "addr": "0x80000007", "name": "Enemy_Far", "kind": "enemy", "pos": [30, 0, 0], "dist": 30.0,
                  "life": 4, "max_life": 4, "owner": "server", "alive": True},
                 {"id": 3, "addr": "0x80000003", "name": "Enemy_Near", "kind": "enemy", "pos": [2, 0, 0], "dist": 2.0,
                  "life": 4, "max_life": 4, "owner": 2, "alive": True},
                 {"id": 4, "addr": "0x80000004", "name": "Enemy_Dead", "kind": "enemy", "pos": [1, 0, 0], "dist": 1.0,
                  "life": 0, "max_life": 4, "owner": None, "alive": False},
                 {"id": 5, "addr": "0x80000005", "name": "Obj_TBox_Wood", "kind": "chest", "pos": [6, 0, 0],
                  "dist": 6.0, "life": None, "max_life": None, "owner": None, "alive": None}]}

    def test_get_state_has_words_and_null_enemies_when_there_is_no_list(self):
        s = self.call("p1", "get_state")
        self.assertEqual((s["life_word"], s["enemy_near"], s["danger"]), ("full", None, None))   # no world: unknown
        self.games["p1"].world = {"frame": None, "why": "no co-op session: the world list is read only while "
                                  "connected and sharing", "count": 0, "actors": []}
        s = self.call("p1", "get_state")
        self.assertIsNone(s["enemy_near"])
        self.games["p1"].world = self.WORLD
        s = self.call("p1", "get_state")
        self.assertEqual((s["enemy_near"], s["danger"]), (1, True))   # the dead one and the far one do not count
        self.assertNotIn("enemy_near_at_least", s)

    def test_look_is_one_short_text_and_says_what_changed(self):
        self.games["p1"].world = self.WORLD
        text = self.call("p1", "look")
        self.assertIsInstance(text, str)
        self.assertIn("enemy: Enemy_Near #3", text)
        self.assertIn("chest: Obj_TBox_Wood #5", text)
        self.assertNotIn("Enemy_Far", text)           # past 25 m
        self.assertIn("changed: first look", text)
        self.assertTrue(self.call("p1", "look").endswith("changed: nothing"))

    def test_wait_until_runs_frames_and_ends_with_a_look(self):
        out = self.call("p1", "wait_until", what="prompt", max_frames=16)
        self.assertFalse(out["met"])
        self.assertEqual(out["frames_run"], 16)
        self.assertIn("Link ", out["look"])
        self.games["p1"].prompt = "Talk"
        out = self.call("p1", "wait_until", what="prompt")
        self.assertEqual((out["met"], out["frames_run"]), (True, 0))

    def test_go_to_xz_ends_with_a_look_and_record_route_saves_the_walk(self):
        import shutil
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        g = self.tools["p1"]
        g.route_dir = folder
        out = self.call("p1", "go_to", x=3, z=0)
        self.assertIn("look", out)
        self.assertNotIn("state", out)
        with self.assertRaises(ValueError):
            self.call("p1", "go_to")
        with self.assertRaises(ValueError):
            self.call("p1", "record_route", action="save", name="a-walk")      # not started
        self.call("p1", "record_route", action="start")
        self.call("p1", "go_to", x=9, z=0)
        got = self.call("p1", "record_route", action="save", name="a-walk", note="Walked 9 m east.")
        self.assertGreaterEqual(got["points"], 2)
        self.assertTrue(os.listdir(folder))
        out = self.call("p1", "go_to", place="a-walk-1")                       # the saved data is used at once
        self.assertEqual(out["plan"][-1], "a-walk-1")

    def test_a_room_that_really_has_no_enemy_is_zero_not_null(self):
        self.games["p1"].world = {"frame": 5, "why": "empty: the walk ran and saw no actors", "count": 0, "actors": []}
        s = self.call("p1", "get_state")
        self.assertEqual((s["enemy_near"], s["danger"]), (0, False))

    def test_life_words_and_a_cut_list(self):
        for life, mx, word in [(12, 12, "full"), (8, 12, "ok"), (6, 12, "low"), (3, 12, "critical"), (0, 12, "critical"),
                               (None, 12, None), (4, 0, None)]:
            self.assertEqual(pc.life_word(life, mx), word, (life, mx))
        cut = dict(self.WORLD, total=60, other_count=0, count=2,
                   actors=[self.WORLD["actors"][1], self.WORLD["actors"][0]][:1])
        self.assertTrue(pc.world_words({"world": cut}).get("enemy_near_at_least"))   # 59 known unseen, last one at 2 m

    def test_a_list_cut_inside_four_metres_does_not_say_no_danger(self):
        chest = {"id": 9, "addr": "0x80000009", "name": "Obj_TBox_Wood", "kind": "chest", "pos": [1, 0, 0],
                 "dist": 3.0, "life": None, "max_life": None, "owner": None, "alive": None}
        cut = {"frame": 5, "why": None, "total": 60, "other_count": 0, "count": 1, "actors": [chest]}
        s = pc.world_words({"world": cut})
        self.assertIsNone(s["danger"])                      # an enemy may stand at 2 m, unlisted
        self.assertTrue(s["enemy_near_at_least"])
        near = dict(chest, id=8, name="Enemy_Stalchild", kind="enemy", dist=2.0, life=4, max_life=4, alive=True)
        self.assertTrue(pc.world_words({"world": dict(cut, actors=[near, chest])})["danger"])   # a True stays True
        whole = dict(cut, total=1)
        self.assertIs(pc.world_words({"world": whole})["danger"], False)                        # nothing was cut

    def test_the_walk_ran_words_match_by_prefix(self):
        for why in (None, "empty: the walk ran and saw no actors", "empty: some new wording",
                    "only other kinds near: dump lists them", "only other kinds near, listed by dump"):
            self.assertTrue(pc.walk_ran(why), why)
        for why in ("no co-op session: the world list is read only while connected and sharing", "stale: x", ""):
            self.assertFalse(pc.walk_ran(why), why)

    def test_kill_hit_and_get_world_say_that_a_name_picks_the_nearest(self):
        by = {t["name"]: t["description"] for t in mcp_playtest.ACT_TOOLS + mcp_playtest.PLAYER_TOOLS}
        for name in ("kill", "hit"):
            self.assertIn("nearest", by[name])
            self.assertIn("NAME", by[name])
        self.assertIn("exact actor", by["get_world"])

    def test_get_world_reads_the_whole_list_and_filters(self):
        self.games["p1"].world = self.WORLD
        r = self.call("p1", "get_world")
        self.assertEqual((r["matched"], r["of"], r["truncated"]), (4, 4, False))
        self.assertEqual([a["id"] for a in r["actors"]], [4, 3, 5, 7])           # nearest first
        self.assertEqual(self.games["p1"].log[-1]["cmd"], "dump")                  # the full list comes from a dump
        self.assertEqual(self.games["p1"].log[-1]["tag"], pc.WORLD_TAG)
        r = self.call("p1", "get_world", kind="enemy", within_m=10)
        self.assertEqual([a["id"] for a in r["actors"]], [4, 3])
        self.assertEqual(r["matched"], 2)
        r = self.call("p1", "get_world", limit=1)
        self.assertEqual((r["matched"], len(r["actors"]), r["truncated"]), (4, 1, True))
        with self.assertRaises(ValueError):
            self.call("p1", "get_world", kind="monster")
        with self.assertRaises(ValueError):
            self.call("p1", "get_world", limit=0)
        with self.assertRaises(ValueError):
            self.call("p1", "get_world", within_m=-1)

    def test_an_empty_list_keeps_its_why(self):
        why = "no co-op session: the world list is read only while connected and sharing"
        self.games["p1"].world = {"frame": None, "why": why, "count": 0, "actors": []}
        r = self.call("p1", "get_world", kind="enemy")
        self.assertEqual((r["matched"], r["why"]), (0, why))
        r = self.call("p1", "get_actor", id=3)
        self.assertFalse(r["found"])
        self.assertEqual(r["why"], why)

    def test_a_module_without_a_world_member_says_so(self):
        self.games["p1"].world = None
        r = self.call("p1", "get_world")
        self.assertEqual(r["actors"], [])
        self.assertIn("no world list", r["why"])
        self.assertIn("no world list", self.call("p1", "get_actor", id=3)["why"])

    def test_get_actor_by_id_text_or_address(self):
        self.games["p1"].world = self.WORLD
        for ident in (3, "3", "0x80000003", "0X80000003"):
            r = self.call("p1", "get_actor", id=ident)
            self.assertTrue(r["found"], ident)
            self.assertEqual(r["actor"]["name"], "Enemy_Near")
        r = self.call("p1", "get_actor", id=99)
        self.assertFalse(r["found"])
        self.assertIn("gone", r["why"])

    def test_verb_tools_send_one_checked_line_through_act(self):
        cases = [("kill", {"actor": "Enemy_Test"}, "kill Enemy_Test"),
                 ("hit", {"actor": "Enemy_Test", "amount": 3}, "hit Enemy_Test 3"),
                 ("hurt", {"quarter_hearts": 4}, "hurt 4"),
                 ("place", {"x": 1, "y": 0.5, "z": -2}, "place 1 0.5 -2"),
                 ("wear", {"part": "head", "name": "Hat_A"}, "wear head Hat_A"),
                 ("dress", {}, "dress all"),
                 ("dress", {"part": "boots"}, "dress boots"),
                 ("assign", {"item": "Bow"}, "assign Bow"),
                 ("chat", {"text": "hello there"}, "chat hello there"),
                 ("menu", {"page": "customize"}, "menu customize")]
        for tool, args, line in cases:
            r = self.call("p1", tool, **args)
            self.assertEqual(r["line"] if tool in ("kill", "hit") else r["lines"], line)
            sent = [m for m in self.games["p1"].log if m["cmd"] == "act"][-1]
            self.assertEqual(sent["lines"], line)

    def test_verb_tools_refuse_bad_arguments_before_sending(self):
        before = len(self.games["p1"].log)
        bad = [("kill", {"actor": "a b"}), ("kill", {}), ("hit", {"actor": "E", "amount": 0}),
               ("hit", {"actor": "E", "amount": 81}), ("hurt", {"quarter_hearts": "x"}),
               ("place", {"x": 1, "y": 2}), ("place", {"x": float("nan"), "y": 0, "z": 0}),
               ("wear", {"part": "tail", "name": "X"}), ("dress", {"part": "tail"}), ("assign", {"item": "A;B"}),
               ("chat", {"text": "a;b"}), ("chat", {"text": "a\nb"}), ("chat", {"text": " "}),
               ("chat", {"text": "x" * 201}), ("menu", {"page": "home"})]
        for tool, args in bad:
            with self.assertRaises(ValueError, msg=tool):
                self.call("p1", tool, **args)
        self.assertEqual(len(self.games["p1"].log), before)

    def test_setup_runs_a_spec_in_one_call_and_says_which_step_failed(self):
        r = self.call("p1", "setup", hearts=5, max_hearts=5, rupees=50, small_keys={"ForestTemple": 2})
        self.assertEqual(len(r["steps"]), 4)   # rupees, keys, max life, life
        f = self.games["p1"].flags
        self.assertEqual((f["PlayerStatus.Life"], f["PlayerStatus.CurrentRupee"], f["DungeonInfo.ForestTemple.SmallKeyNum"]),
                         (20, 50, 2))
        self.assertNotIn("PlayerStatus.Life", self.games["p2"].flags)   # only the asked game
        with self.assertRaises(Exception):
            self.call("p1", "setup", hearts=0)

    def test_preset_applies_a_named_start_and_a_wrong_name_lists_the_right_ones(self):
        r = self.call("p1", "preset", name="deku-before-gohma")
        self.assertEqual([s["call"] for s in r["steps"][-2:]], [["warp", "InsideTheDekuTree", "Entrance"],
                                                                ["place", -12.0, 48.5, 66.5]])
        self.assertEqual(self.games["p1"].pos, [-12.0, 48.5, 66.5])   # the corridor before Gohma's door
        self.assertEqual(self.games["p1"].flags["Gohma_DefeatBoss"], 0)
        with self.assertRaises(Exception) as e:
            self.call("p1", "preset", name="nope")
        self.assertIn("deku-start", str(e.exception))

    def test_save_point_and_load_point_restore_the_flags_and_say_when_exact(self):
        g = self.games["p1"]
        self.call("p1", "save_point", tag="before-boss")
        g.flags["F1"], g.flags["F2"] = 7, 9                       # the fight changed things
        r = self.call("p1", "load_point", tag="before-boss")
        self.assertEqual((g.flags["F1"], g.flags["F2"]), (0, 0))
        self.assertEqual((r["restored"], r["exact"], r["left"]), (2, True, []))
        with self.assertRaises(Exception) as e:
            self.call("p1", "load_point", tag="never-saved")
        self.assertIn("save_point first", str(e.exception))

    def test_press_warp_flag_give_step_resume(self):
        r = self.call("p1", "press", buttons=["a", "ZL"], stick_angle=90, stick_strength=1.0, frames=4)
        self.assertEqual(r["frame"], 1004)
        sent = self.games["p1"].log[-1]
        self.assertEqual((sent["cmd"], sent["buttons"], sent["stick"], sent["frames"]), ("input", ["A", "ZL"],
                                                                                       [90.0, 1.0], 4))
        self.assertEqual(self.call("p1", "press")["frame"], 1006)      # 2 frames by default, no stick
        self.assertNotIn("stick", self.games["p1"].log[-1])
        with self.assertRaises(ValueError):
            self.call("p1", "press", buttons=["Q"])
        self.games["p1"].event = True            # (an event runs: the module's own request, with an entrance)
        r = self.call("p1", "warp", scene="KokiriForest_Child", entrance="Start_Mido")
        self.assertEqual(r["state"]["scene"], "KokiriForest_Child")
        self.assertEqual((self.games["p1"].log[-1]["entrance"], self.games["p1"].log[-1]["timeout"]), ("Start_Mido", 60))
        self.call("p1", "warp", scene="DekuTree", timeout=25)
        self.assertNotIn("entrance", self.games["p1"].log[-1])
        self.assertEqual(self.games["p1"].log[-1]["timeout"], 25)      # F3: the tool's timeout reaches the bridge
        for bad in (0, 601, "x"):
            with self.assertRaises(ValueError):
                self.call("p1", "warp", scene="DekuTree", timeout=bad)
        self.games["p1"].event = False
        self.call("p1", "warp", scene="DekuTree")                       # F2: free play goes by Longshot's teleport
        self.assertEqual(self.games["p1"].via, "teleport")
        for kw in ({"entrance": "Start_Mido"}, {"scene": "BossStage"}):  # ... which has no start and a short list: the map change event
            self.call("p1", "warp", **dict({"scene": "DekuTree"}, **kw))
            self.assertEqual(self.games["p1"].via, "map_event")
        self.assertEqual(self.call("p1", "flag", name="F2", value=True)["was"], 0)
        self.assertIs(self.games["p1"].flags["F2"], True)
        self.assertEqual(self.call("p1", "flag_read", name="F2")["value"], True)
        self.assertEqual(self.call("p1", "flag_read", name="Nope")["kind"], "unknown")
        self.call("p1", "give", item="bombs")
        self.assertEqual(self.games["p1"].items, {"bombs": 1})
        self.assertEqual(self.call("p1", "step", frames=30)["frame"], 1036)
        self.assertFalse(self.call("p1", "resume")["paused"])
        self.assertTrue(self.call("p1", "pause")["paused"])

    def test_walk_to_tool(self):
        self.assertTrue(self.call("p1", "walk_to", x=2.0, z=2.0)["arrived"])

    def test_checkpoint_every_game_then_desync_check(self):
        for p in ("p1", "p2"):
            self.assertEqual(self.call(p, "checkpoint", tag="start")["flags"], 2)
        self.games["p1"].flags["F1"] = 1                       # p1 opened the chest ...
        self.call("p1", "step", frames=10)
        for p in ("p1", "p2"):
            self.call(p, "checkpoint", tag="after_chest")
        r = self.call("p2", "desync_check")
        self.assertEqual(r["result"], "desync")
        self.assertIn("start: agree", r["report"])
        self.assertIn("after_chest: 1 flag(s) differ", r["report"])
        self.assertIn("F1: p1 1, p2 0", r["report"])
        self.assertIn("p1 frame 1010", r["report"])
        self.games["p2"].flags["F1"] = 1                       # ... and it reached p2
        for p in ("p1", "p2"):
            self.call(p, "checkpoint", tag="after_chest")
        self.assertIn("after_chest#2: agree", self.call("p1", "desync_check", all=True)["report"])
        with self.assertRaises(ValueError):
            self.call("p1", "checkpoint", tag="bad tag")

    def test_checkpoint_without_a_dump_is_not_compared(self):
        self.games["p2"].dump_dir = os.path.join(self.tmp.name, "missing")
        self.call("p1", "checkpoint", tag="t")
        with self.assertRaises(OSError):                       # the fake cannot write: the bridge would say failed
            self.call("p2", "checkpoint", tag="t")
        self.assertEqual(self.call("p1", "desync_check")["result"], "nothing to compare")

    def test_screenshot(self):
        r = self.call("p2", "screenshot", name="door")
        self.assertNotIn("path", self.games["p2"].log[-1])     # a name only: the game picks the folder
        self.assertTrue(os.path.exists(r["path"]))
        self.assertTrue(os.path.basename(r["path"]).startswith("p2-door-"))
        self.assertEqual(r["advanced"], 1)
        self.assertEqual(os.path.dirname(r["copy"]), os.path.join(self.tmp.name, "shots"))   # beside the logs
        self.assertTrue(os.path.exists(r["copy"]))
        with self.assertRaises(ValueError):
            self.call("p2", "screenshot", name="../x")

    def test_a_live_session_is_kept_awake_and_an_ended_one_refused(self):
        d = os.path.join(self.tmp.name, "session")
        os.makedirs(d)
        live.save(d, {"started": 1000.0, "games": {}, "offsets": {}})
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        live.touch(d)
        os.utime(live.active_file(d), (1000.0, 1000.0))          # the last command long ago
        g.call("step", {"frames": 1})
        self.assertGreater(live.activity(d, {}), 1000.0 + 3600)   # touched now: the watchdog's idle clock restarts
        live.write_json(live.watchdog_file(d), {"ended": "idle"})
        with self.assertRaises(SystemExit):
            g.call("step", {"frames": 1})

    def test_log_lines_since_the_last_call(self):
        d = os.path.join(self.tmp.name, "session")
        os.makedirs(d)
        logs = {x: os.path.join(d, "game-out-%s.txt" % x) for x in live.TAGS}
        with open(logs["A"], "w", encoding="utf-8", newline="") as f:
            f.write("boot line coop: before\n")
        live.save(d, {"started": 1000.0, "offsets": {"A": os.path.getsize(logs["A"]), "B": 0},
                      "games": {x: {"log": logs[x]} for x in live.TAGS}})
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        with open(logs["A"], "a", encoding="utf-8", newline="") as f:
            f.write("[x] coop: objects: rocks: 3 new falling rocks in our game since the last line (3 live)\n"
                    "[x] coop: other\nno co-op here\n")
        out = g.call("log", {"match": "rocks:"})
        self.assertEqual(len(out["lines"]), 1)
        self.assertIn("3 new falling rocks", out["lines"][0])
        self.assertEqual(g.call("log", {})["lines"], [ln for ln in out["lines"]] + ["[x] coop: other"])   # the plain cursor stays
        self.assertEqual(g.call("log", {})["lines"], [])                 # nothing new since
        self.assertEqual(live.load(d)["offsets"]["A"], len("boot line coop: before\n"))   # session file untouched
        with self.assertRaises(ValueError):
            self.tools["p1"].call("log", {})                             # no --session
        with self.assertRaises(ValueError):
            mcp_playtest.Game("p3", self.tools["p1"].client, self.tmp.name, session=d).log_lines()

    CANNED = (
        "[hos 00:00:01] vk: noise coop: ignore\n"
        "[hos 00:00:02] coop: connected to 203.0.113.7:5000 as \"Zelda99\" (name=Zelda99)\n"
        "[hos 00:00:03] coop: fine line, nothing wrong\n"
        "[hos 00:00:04] coop: save: refused to write the host's save into C:\\Users\\zachb\\Downloads\\x\n"
        "[hos 00:00:05] coop: look: the look worker's pass failed (no pose): tried again\n"
        "plain error line without the module prefix\n"
        "[hos 00:00:06] coop: peer [2001:db8::1]:5000 sent resync for 0x80123456 password=hunter2\n")

    def make_session(self, text=None):
        d = os.path.join(self.tmp.name, "session")
        os.makedirs(d, exist_ok=True)
        logs = {x: os.path.join(d, "game-out-%s.txt" % x) for x in live.TAGS}
        with open(logs["A"], "w", encoding="utf-8", newline="") as f:
            f.write("old coop: failed before the boot\n")
        boot = os.path.getsize(logs["A"])
        live.save(d, {"started": time.time(), "booted": True, "max_s": 1800, "idle_s": 600,
                      "offsets": {"A": boot, "B": 0}, "games": {x: {"log": logs[x]} for x in live.TAGS}})
        if text is not None:
            with open(logs["A"], "a", encoding="utf-8", newline="") as f:
                f.write(text)
        return d, logs

    def test_check_logs_flags_the_four_words_and_masks_everything(self):
        d, _ = self.make_session(self.CANNED)
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        out = g.call("check_logs", {"names": ["Zelda99"]})
        self.assertEqual((out["verdict"], out["flagged"], out["read"]), ("flagged", 3, 5))   # 5 coop lines, 3 flagged
        text = "\n".join(x["line"] for x in out["lines"])
        for secret in ("203.0.113.7", "5000", "Zelda99", "zachb", "2001:db8", "0x80123456", "hunter2", "[hos"):
            self.assertNotIn(secret, text, secret)
        self.assertIn("<user>", text)
        self.assertIn("<ip>", text)
        self.assertIn("refused", text)
        self.assertIn("resync", text)
        self.assertNotIn("plain error line", text)                 # not a `coop:` line
        # the next call has nothing new, and "boot" reads it all again
        again = g.call("check_logs", {})
        self.assertEqual((again["verdict"], again["lines"]), ("clean", []))
        self.assertEqual(g.call("check_logs", {"since": "boot"})["flagged"], 3)
        self.assertNotIn("before the boot", "\n".join(x["line"] for x in g.call("check_logs", {"since": "boot"})["lines"]))

    def test_check_logs_and_log_never_take_lines_from_each_other(self):
        d, logs = self.make_session("[x] coop: one failed thing\n")
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        self.assertEqual(len(g.call("log", {})["lines"]), 1)
        self.assertEqual(g.call("check_logs", {})["flagged"], 1)
        with open(logs["A"], "a", encoding="utf-8", newline="") as f:
            f.write("[x] coop: another error\n")
        self.assertEqual(g.call("check_logs", {})["flagged"], 1)
        self.assertEqual(len(g.call("log", {})["lines"]), 1)

    def test_check_logs_is_clean_with_no_flagged_line_and_says_when_there_is_no_log(self):
        d, logs = self.make_session("[x] coop: all is well\n")
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        self.assertEqual(g.call("check_logs", {})["verdict"], "clean")
        out = mcp_playtest.Game("p2", self.tools["p2"].client, self.tmp.name, session=d).call("check_logs", {})
        self.assertEqual((out["verdict"], out["flagged"]), ("no log", 0))   # B's file was never written
        with self.assertRaises(ValueError):
            self.tools["p1"].call("check_logs", {})                            # no --session
        for bad in ({"since": "yesterday"}, {"names": "Zelda"}, {"names": ["a"] * 21}, {"names": [1]}):
            with self.assertRaises(ValueError, msg=str(bad)):
                g.call("check_logs", bad)

    def test_check_logs_keeps_the_newest_forty_and_counts_the_rest(self):
        d, _ = self.make_session("".join("[x] coop: failed %d\n" % i for i in range(55)))
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        out = g.call("check_logs", {})
        self.assertEqual((out["flagged"], len(out["lines"]), out["left_out"]), (55, 40, 15))
        self.assertIn("failed 54", out["lines"][-1]["line"])

    def test_session_status_without_a_session_and_with_one(self):
        out = self.call("p1", "session_status")
        self.assertIsNone(out["session"])
        self.assertTrue(out["bridge"]["connected"] is False and out["bridge"]["held"] is False)   # nothing asked yet
        self.call("p1", "get_state")
        out = self.call("p1", "session_status")
        self.assertEqual((out["bridge"]["connected"], out["bridge"]["held"]), (True, True))
        self.assertEqual(out["bridge"]["last_frame"], self.games["p1"].frame)
        self.call("p1", "resume")
        self.assertFalse(self.call("p1", "session_status")["bridge"]["held"])
        self.call("p1", "pause")
        self.assertTrue(self.call("p1", "session_status")["bridge"]["held"])

        d, _ = self.make_session()
        live.write_json(os.path.join(d, "pids.json"), {"games": {"A": 111, "B": 222}, "server": 333})
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        g.alive = lambda pid: pid != 222
        sent = len(self.games["p1"].log)
        s = g.call("session_status", {})["session"]
        self.assertEqual((s["running"], s["ended"]), (True, None))
        self.assertEqual(s["games"], {"A": {"pid": 111, "alive": True}, "B": {"pid": 222, "alive": False}})
        self.assertTrue(29 <= s["max_minutes_left"] <= 30)
        self.assertTrue(0 < s["idle_minutes_left"] <= 10)
        self.assertEqual(len(self.games["p1"].log), sent)                   # it asked the game nothing

    def test_session_status_answers_after_the_session_ended_and_is_no_tester_command(self):
        d, _ = self.make_session()
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        live.touch(d)
        os.utime(live.active_file(d), (1000.0, 1000.0))
        live.write_json(live.watchdog_file(d), {"ended": "game B ended"})
        s = g.call("session_status", {})["session"]
        self.assertEqual((s["running"], s["ended"]), (False, "game B ended"))
        self.assertEqual(os.path.getmtime(live.active_file(d)), 1000.0)   # the idle clock was not restarted
        with self.assertRaises(SystemExit):
            g.call("get_state", {})                                          # the games' tools still refuse

    def test_check_logs_flags_the_level_prefix_with_its_reason(self):
        # 78 of the 102 warning, error and critical lines of the module contain none of the four words
        d, _ = self.make_session(
            "[hos 00:00:01] warning: coop: connection lost; reconnecting ...\n"
            "[hos 00:00:02] error: coop: save: could not switch the save folder back\n"
            "[hos 00:00:03] critical: coop: voice: the microphone did not start\n"
            "[hos 00:00:04] coop: save: the file was refused\n"
            "[hos 00:00:05] coop: all fine\n"
            "[hos 00:00:06] warning: not a co-op line\n")
        out = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d).call("check_logs", {})
        self.assertEqual([x["why"] for x in out["lines"]],
                         ["level:warning", "level:error", "level:critical", "word:refused"])
        self.assertEqual((out["flagged"], out["read"], out["verdict"]), (4, 5, "flagged"))

    def test_check_logs_keeps_the_words_after_a_prefix_such_as_host(self):
        d, _ = self.make_session("[hos 1] coop: host: refused the join\n[hos 1] coop: server: refused the join\n"
                                 "[hos 1] coop: room: failed to open\n[hos 1] coop: key: error in the table\n")
        out = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d).call("check_logs", {})
        self.assertEqual([x["line"] for x in out["lines"]],
                         ["coop: host: refused the join", "coop: server: refused the join",
                          "coop: room: failed to open", "coop: key: error in the table"])

    def test_check_logs_hides_the_names_the_games_print_without_being_told(self):
        d, _ = self.make_session(
            "[x] coop: on, server myhouse.duckdns.org:7777, name 'Zelda99', join password hunter2\n"
            "[x] coop: player 2 'Link_Fan' joined\n"
            "[x] coop: Link_Fan lost the connection; failed to resync with Zelda99.\n"
            "[x] coop: connect to 203.0.113.7. failed\n")
        out = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d).call("check_logs", {})
        text = "\n".join(x["line"] for x in out["lines"])
        for secret in ("Zelda99", "Link_Fan", "203.0.113.7", "duckdns", "hunter2", "7777"):
            self.assertNotIn(secret, text, secret)
        self.assertIn("<name> lost the connection", text)
        self.assertEqual(out["names_hidden"], 2)

    def test_check_logs_answers_after_the_session_ended_and_does_not_restart_the_idle_clock(self):
        d, _ = self.make_session("[x] coop: failed before the end\n")
        live.touch(d)
        os.utime(live.active_file(d), (1000.0, 1000.0))
        live.write_json(live.watchdog_file(d), {"ended": "game B ended"})
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        self.assertEqual(g.call("check_logs", {})["flagged"], 1)
        self.assertEqual(os.path.getmtime(live.active_file(d)), 1000.0)

    def test_read_log_since_waits_for_a_whole_line_and_restarts_a_new_log(self):
        path = os.path.join(self.tmp.name, "game.txt")
        with open(path, "wb") as f:
            f.write(b"[hos 1] coop: one failed\n[hos 1] coop: two fail")
        lines, off = pc.read_log_since(path, 0)
        self.assertEqual((lines, off), (["coop: one failed"], len(b"[hos 1] coop: one failed\n")))
        with open(path, "ab") as f:
            f.write(b"ed\n")
        lines, off = pc.read_log_since(path, off)
        self.assertEqual(lines, ["coop: two failed"])
        with open(path, "wb") as f:                                   # a new game on the same path
            f.write(b"[hos 1] coop: fresh\n")
        self.assertEqual(pc.read_log_since(path, off)[0], ["coop: fresh"])
        self.assertEqual(pc.read_log_since(path + ".none", 7), ([], 7))

    def test_the_mask_leaks_nothing_in_the_review_cases(self):
        cases = [("connect to 203.0.113.7. failed", "connect to <ip>. failed"),
                 ("connect to 203.0.113.7, failed", "connect to <ip>, failed"),
                 ("connect to myhouse.duckdns.org:7777 failed", "connect to <host> failed"),
                 ("player 2 'Bob Smith' joined", "player 2 '<name>' joined"),
                 ("on, server 1.2.3.4:5, name 'Bob', join password abc", "on, server <ip>, name '<name>', join password <hidden>"),
                 ("at 12:34:56.789 it failed", "at 12:34:56.789 it failed"),
                 ("coop: host: refused", "coop: host: refused"), ("host=box1 refused", "host=<hidden> refused")]
        for raw, want in cases:
            self.assertEqual(pc.mask_text(raw), want, raw)
        self.assertEqual(pc.mask_text("A cat saw a dog and A.", names=["A"]), "<name> cat saw a dog and <name>.")
        self.assertEqual(pc.mask_text("the Bob", names=["bob"]), "the <name>")

    def test_check_logs_reads_the_voice_lines_and_a_level_after_one_more_tag(self):
        d, _ = self.make_session(
            "[hos 1] warning: coop voice: the microphone did not start\n"
            "[hos 1] coop voice: the encoder failed to open\n"
            "[hos 1] coop voice: opened\n"
            "[hos 1] [Warning] warning: coop: slow frame\n"
            "[hos 1] voice: not a co-op line, failed\n")
        out = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d).call("check_logs", {})
        self.assertEqual([(x["why"], x["line"]) for x in out["lines"]],
                         [("level:warning", "warning: coop voice: the microphone did not start"),
                          ("word:failed", "coop voice: the encoder failed to open"),
                          ("level:warning", "[Warning] warning: coop: slow frame")])
        self.assertEqual(out["read"], 4)

    def test_the_mask_hides_a_host_without_a_dot_and_a_name_with_an_apostrophe(self):
        for raw, want in (("connect to DESKTOP-K3J9:7777 failed", "connect to <host> failed"),
                          ("host=DESKTOP-K3J9:7777 refused", "host=<hidden> refused"),
                          ("lan MYPC:7777 up", "lan <host> up"),
                          ("player 2 'Pat O'Neil' joined", "player 2 '<name>' joined"),
                          ("name 'Rock'n'Roll' ok", "name '<name>' ok"),
                          ("name 'Dan' and 'Bob' failed", "name '<name>' and 'Bob' failed")):
            self.assertEqual(pc.mask_text(raw), want, raw)
        for keep in ("coop: error:10054 reading", "coop: port:7777 open", "coop: frame:12345", "at 12:34:56 it failed"):
            self.assertEqual(pc.mask_text(keep), keep)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "g.txt")
            with open(path, "w", encoding="utf-8") as f:
                f.write("coop: player 2 'Pat O'Neil' joined\ncoop: on, name 'Rock'n'Roll'\n")
            self.assertEqual(pc.session_names([path]), ["Pat O'Neil", "Rock'n'Roll"])
        self.assertEqual(pc.mask_text("Pat O'Neil lost it", names=["Pat O'Neil"]), "<name> lost it")

    def test_the_mask_keeps_ordinary_text_and_hides_the_rest(self):
        keep = ["coop: look: the puppets' tunics are green now", "coop: version 0.11.10 build 1.2.3", "coop: frame 12345"]
        for line in keep:
            self.assertEqual(pc.mask_text(line), line)
        cases = [("to 10.0.0.5:7777 ok", "to <ip> ok"), ("from 192.168.1.20 ok", "from <ip> ok"),
                 ("[::1]:5000 up", "<ip> up"), ("fe80::1ff:fe23:4567:890a lost", "<ip> lost"),
                 ("C:\\Users\\alex\\Downloads\\g", "C:\\Users\\<user>\\Downloads\\g"),
                 ("C:/Users/alex/Downloads/g", "C:/Users/<user>/Downloads/g"),
                 ("/home/alex/kit", "/home/<user>/kit"), ("/Users/alex/kit", "/Users/<user>/kit"),
                 ("at 0x8000A1B2", "at <addr>"), ("key " + "ab12" * 8, "key <hex>"),
                 ("nickname=Bob ok", "nickname=<hidden> ok"), ("password: s3cret!", "password: <hidden>"),
                 ('said "hello Bob"', 'said "<text>"')]
        for raw, want in cases:
            self.assertEqual(pc.mask_text(raw), want, raw)
        self.assertEqual(pc.mask_text("Bob and bobby and BOB.", names=["bob"]), "<name> and bobby and <name>.")

    def test_main_needs_session_or_player(self):
        err = sys.stderr
        sys.stderr = io.StringIO()
        try:
            for argv in ([], ["--player", "p9"], ["--player", "p1", "--lane", "x"]):   # (--session with --player is valid)
                with self.assertRaises(SystemExit):
                    mcp_playtest.main(argv)
        finally:
            sys.stderr = err


class PairServer(unittest.TestCase):
    """--pair: one server, both games; the frames one game runs, the other runs too."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.games = {p: FakeGame(dump_dir=tempfile.mkdtemp(dir=self.tmp.name)) for p in ("p1", "p2")}
        self.pair = mcp_playtest.Pair({p: mcp_playtest.Game(p, pc.Client(g.port), self.tmp.name)
                                       for p, g in self.games.items()})

    def tearDown(self):
        for g in self.pair.games.values():
            g.client.close()
        for g in self.games.values():
            g.close()
        self.tmp.cleanup()

    def frames(self):
        return {p: g.frame for p, g in self.games.items()}

    def test_setup_without_a_player_runs_on_both_games(self):
        r = self.pair.answer("setup", {"rupees": 7})
        self.assertEqual(set(r["games"]), {"p1", "p2"})
        self.assertEqual([g.flags["PlayerStatus.CurrentRupee"] for g in self.games.values()], [7, 7])

    def test_both_games_get_a_delta_shared_count_written_once(self):
        self.pair.answer("setup", {"small_keys": {"ForestTemple": 2}, "rupees": 7})
        key = "DungeonInfo.ForestTemple.SmallKeyNum"
        self.assertEqual(self.games["p1"].flags[key], 2)
        self.assertNotIn(key, self.games["p2"].flags)   # the share brings it, written here it would be added again
        self.assertEqual(self.games["p2"].flags["PlayerStatus.CurrentRupee"], 7)   # per player: written on each

    def test_every_tool_takes_a_player_and_only_the_both_tools_can_go_without(self):
        by = {t["name"]: t for t in self.pair.tools}
        self.assertEqual(list(by), [t["name"] for t in mcp_playtest.PLAYER_TOOLS])
        for name, t in by.items():
            if name in ("pause", "resume"):   # both games always: no player to give
                self.assertNotIn("player", t["inputSchema"]["properties"], name)
                self.assertNotIn("player", t["inputSchema"].get("required", []), name)
                continue
            self.assertEqual(t["inputSchema"]["properties"]["player"]["enum"], ["p1", "p2"])
            self.assertEqual("player" in t["inputSchema"].get("required", []), name not in mcp_playtest.BOTH_TOOLS, name)
            self.assertEqual("Both games run the same frames" in t["description"],
                             name not in mcp_playtest.NO_FRAME_TOOLS, name)
        self.assertEqual(by["walk_to"]["inputSchema"]["required"], ["player", "x", "z"])
        self.assertNotIn("player", mcp_playtest.PLAYER_TOOLS[0]["inputSchema"]["properties"])   # (a copy)

    def test_a_walk_runs_the_same_frames_in_the_other_game(self):
        r = self.pair.answer("walk_to", {"player": "p1", "x": 2.0, "z": 0.0})
        self.assertTrue(r["arrived"], r)
        ran = self.games["p1"].frame - 1000
        self.assertGreater(ran, 0)
        self.assertEqual(self.games["p2"].frame - 1000, ran)
        self.assertEqual(r["other"], {"player": "p2", "frames": ran, "caught_up": 0, "in_step": True})
        self.assertEqual(self.games["p2"].pos, [0.0, 0.0, 0.0])    # stepped, never moved

    def test_frames_outside_input_and_step_are_caught_up(self):
        self.games["p2"].warp_frames = 40
        r = self.pair.answer("warp", {"player": "p2", "scene": "KokiriForest_Child"})
        self.assertEqual(self.frames(), {"p1": 1040, "p2": 1040})
        self.assertEqual(r["other"]["caught_up"], 40)
        self.pair.answer("screenshot", {"player": "p1"})   # a shot runs 1 frame
        self.assertEqual(self.frames(), {"p1": 1041, "p2": 1041})

    def test_no_player_state_and_step_are_both_games(self):
        r = self.pair.answer("step", {"frames": 7})
        self.assertEqual(self.frames(), {"p1": 1007, "p2": 1007})
        self.assertEqual(sorted(r["games"]), ["p1", "p2"])
        self.assertNotIn("other", r)
        r = self.pair.answer("get_state", {})
        self.assertEqual(r["games"]["p2"]["scene"], "ForestTemple")
        self.assertEqual(self.frames(), {"p1": 1007, "p2": 1007})

    def test_a_look_runs_no_frame_and_a_missing_player_is_refused(self):
        r = self.pair.answer("get_world", {"player": "p1"})
        self.assertNotIn("other", r)
        self.assertEqual(self.frames(), {"p1": 1000, "p2": 1000})
        self.assertEqual([m["cmd"] for m in self.games["p2"].log], [])   # the other game was not even asked
        with self.assertRaisesRegex(ValueError, "player is one of p1, p2"):
            self.pair.answer("walk_to", {"x": 1, "z": 0})
        with self.assertRaisesRegex(ValueError, "player is one of"):
            self.pair.answer("press", {"player": "p3"})

    def test_pause_and_resume_are_both_games(self):
        # One game resumed alone would run free while the other is held: the two would drift apart.
        r = self.pair.answer("resume", {"player": "p1"})
        self.assertEqual(sorted(r["games"]), ["p1", "p2"])
        self.assertEqual({p: g.paused for p, g in self.games.items()}, {"p1": False, "p2": False})
        self.pair.answer("pause", {})
        self.assertEqual({p: g.paused for p, g in self.games.items()}, {"p1": True, "p2": True})

    def test_a_call_cut_short_runs_only_what_ran_in_the_other_game(self):
        self.games["p1"].cut = 4   # an event cuts every press of p1 to 4 frames
        r = self.pair.answer("press", {"player": "p1", "buttons": ["A"], "frames": 10})
        self.assertEqual(self.frames(), {"p1": 1004, "p2": 1004})   # p2 not ahead
        self.assertEqual(r["other"], {"player": "p2", "frames": 4, "caught_up": 0, "short": 6, "in_step": True})

    def test_a_pair_call_log_replays_in_step_on_the_same_bridges(self):
        # The replay runner runs oot-pair lines through a Pair over the very Games it uses for oot-p1 and oot-p2 lines
        # (a bridge serves one client at a time).
        log = os.path.join(self.tmp.name, "calls.jsonl")
        kit_tools.CallLog(log, "oot-p1").write("step", {"frames": 2}, True, None, 1)
        kit_tools.CallLog(log, "oot-pair").write("press", {"player": "p2", "buttons": ["A"], "frames": 3}, True, None, 2)
        ports = {p: g.port for p, g in self.games.items()}
        runner = kit_tools._Runner(None, None, self.tmp.name)
        with mock.patch.object(pc, "port_for", lambda player, lane=None: ports[player]):
            r = kit_tools.replay([log], runner=runner, say=lambda _: None)
        self.assertEqual((r["ran"], r["failed"]), (2, []))
        self.assertEqual(self.frames(), {"p1": 1005, "p2": 1003})
        self.assertIs(runner.pair.games["p1"], runner.games["p1"])

    def test_served_as_oot_pair(self):
        reqs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                 "params": {"name": "press", "arguments": {"player": "p2", "buttons": ["A"], "frames": 3}}}]
        stdin, stdout = sys.stdin, sys.stdout
        sys.stdin = io.StringIO("".join(json.dumps(r) + "\n" for r in reqs))
        sys.stdout = io.StringIO()
        try:
            mcp_playtest.serve(None, self.pair)
            out = [json.loads(ln) for ln in sys.stdout.getvalue().splitlines()]
        finally:
            sys.stdin, sys.stdout = stdin, stdout
        self.assertEqual(out[0]["result"]["serverInfo"]["name"], "oot-pair")
        body = json.loads(out[1]["result"]["content"][0]["text"])
        self.assertEqual(body["other"]["frames"], 3)
        self.assertEqual(self.frames(), {"p1": 1003, "p2": 1003})


class VerbGateNotInPlayersFiles(unittest.TestCase):
    """act, warp, flag and give are gated on LICHII_COOP_PLAYTEST_VERBS. A player's game must never get it: no file
    the kit starts a game from names it (the review of playtest S3: OOT_COOP_KIT_START was set for every player)."""
    ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    NAME = "LICHII_COOP_PLAYTEST_VERBS"

    def test_the_kits_start_files_and_options_never_name_it(self):
        kit = os.path.join(self.ROOT, "tools", "release", "kit")
        named = ["scripts/play.ps1", "linux-longshot/scripts/start.sh", "coop-options.ini"]
        for rel in named:
            self.assertTrue(os.path.isfile(os.path.join(kit, rel)), rel)   # (the test fails if a file moves)
        checked = 0
        for folder, _dirs, files in os.walk(kit):
            for f in files:
                path = os.path.join(folder, f)
                with open(path, "rb") as fh:
                    data = fh.read()
                checked += 1
                self.assertNotIn(self.NAME.encode(), data, os.path.relpath(path, kit))
        self.assertGreater(checked, 3)

    def test_the_runner_names_it_next_to_the_port(self):
        with open(os.path.join(self.ROOT, "tools", "autotest", "hookshot.py"), encoding="utf-8") as f:
            text = f.read()
        i = text.index('env["LICHII_COOP_PLAYTEST_PORT"]')
        self.assertIn(self.NAME, text[i:i + 300])


class DesyncClients(unittest.TestCase):
    def write(self, d, player, rows):
        with open(pc.checkpoint_file(d, player), "w", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")

    def test_codes(self):
        out = []
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(desync_check.check_clients(d, out=out.append), 2)      # no logs
            self.write(d, "p1", [{"tag": "t", "frame": 5, "flags": {"F": 1}}])
            self.assertEqual(desync_check.check_clients(d, out=out.append), 2)      # one game only
            self.write(d, "p2", [{"tag": "t", "frame": 9, "flags": {"F": 1}}])
            self.write(d, "p3", [{"tag": "t", "frame": 7, "flags": {"F": 1, "G": 2}}])
            self.assertEqual(desync_check.check_clients(d, out=out.append), 0)      # G only p3 reads: no desync
            self.write(d, "p2", [{"tag": "t", "frame": 9, "flags": {"F": 1}}, {"tag": "u", "frame": 20, "flags": {}}])
            self.assertEqual(desync_check.check_clients(d, out=out.append), 3)      # u missing in p1 and p3
            self.write(d, "p2", [{"tag": "t", "frame": 9, "flags": None, "error": "no dump"}])
            self.assertEqual(desync_check.check_clients(d, out=out.append), 2)      # nothing compared

    def test_frame_order_and_command_line(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, "p1", [{"tag": "b", "frame": 20, "flags": {"F": 2}},
                                 {"tag": "a", "frame": 10, "flags": {"F": 1}}])
            self.write(d, "p2", [{"tag": "a", "frame": 11, "flags": {"F": 1}},
                                 {"tag": "b", "frame": 21, "flags": {"F": 3}}])
            out = []
            self.assertEqual(desync_check.check_clients(d, out=out.append), 1)
            self.assertTrue(out[0].startswith("a: agree"), out)
            self.assertIn("first desync", out[1])
            stdout = sys.stdout
            sys.stdout = io.StringIO()
            try:
                self.assertEqual(desync_check.main([d, "--clients"]), 1)
            finally:
                sys.stdout = stdout



class StrikeAndHelpers(unittest.TestCase):
    """kill and hit by id (rule Y) and the helpers of playtest_helpers.py, against the fake game."""

    WORLD = {"frame": 1000, "age_ms": 40, "stale": False, "behind": False, "total": 3, "other_count": 0, "count": 3,
             "actors": [
                 {"id": 7, "addr": "0x80000007", "name": "Enemy_Far", "kind": "enemy", "pos": [30, 0, 0], "dist": 30.0,
                  "life": 4, "max_life": 4, "owner": "server", "alive": True},
                 {"id": 3, "addr": "0x80000003", "name": "Enemy_Near", "kind": "enemy", "pos": [6, 0, 0], "dist": 6.0,
                  "life": 4, "max_life": 4, "owner": 2, "alive": True},
                 {"id": 5, "addr": "0x80000005", "name": "Obj_TBox_Wood", "kind": "chest", "pos": [6, 0, 3],
                  "dist": 6.7, "life": None, "max_life": None, "owner": None, "alive": None}]}

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.game = FakeGame(dump_dir=self.tmp.name)
        self.game.world = json.loads(json.dumps(self.WORLD))
        self.other = FakeGame(dump_dir=tempfile.mkdtemp(dir=self.tmp.name))
        self.tool = mcp_playtest.Game("p1", pc.Client(self.game.port), self.tmp.name)
        self.c = self.tool.client

    def tearDown(self):
        self.tool.client.close()
        self.game.close()
        self.other.close()
        self.tmp.cleanup()

    def acts(self):
        return [m["lines"] for m in self.game.log if m["cmd"] == "act"]

    def actor(self, ident):
        return next(a for a in self.game.world["actors"] if a["id"] == ident)

    # --- kill and hit by id -------------------------------------------------------------------------------------
    def test_an_id_goes_before_the_name_and_ok_comes_when_the_walk_struck(self):
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertEqual(r["result"], "ok")
        self.assertEqual(self.acts(), ["kill 7 Enemy_Far"])
        r = self.tool.call("hit", {"actor": "Enemy_Far", "id": "0x80000007", "amount": 3})
        self.assertEqual(r["result"], "ok")
        self.assertEqual(self.acts()[-1], "hit %d Enemy_Far 3" % 0x80000007)
        r = self.tool.call("kill", {"actor": "Enemy_Far"})       # a name alone: it struck, its death is not checked
        self.assertTrue(r["result"].startswith("struck: the hit landed"), r["result"])
        self.assertIsNone(r["died"])
        self.assertEqual(self.acts()[-1], "kill Enemy_Far")

    def test_go_to_actor_refuses_on_a_stale_snapshot(self):           # F1
        self.game.stale = True
        r = ph.go_to_actor(self.c, 3)
        self.assertTrue(r["reason"].startswith("stale position"), r)
        self.assertFalse(r.get("arrived"))

    def test_a_kill_is_ok_only_when_the_actor_is_seen_dead(self):        # F4
        self.game.death_delay = 14                      # struck at frame +2, dies 14 frames after that
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertEqual((r["result"], r["died"]), ("ok", True))
        self.assertGreater(r["died_after"], 0)           # it waited for the death, it did not say ok at the strike
        self.assertFalse(self.actor(7)["alive"])

    def test_a_kill_that_struck_but_did_not_kill_says_struck(self):
        self.game.death_delay = None                    # a far actor that never dies
        start = self.game.frame
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertTrue(r["result"].startswith("struck: the hit landed, but Enemy_Far still lives after"), r["result"])
        self.assertIs(r["died"], False)
        self.assertLessEqual(self.game.frame - start, 2 + ph.STRIKE_FRAMES + ph.DEATH_FRAMES + ph.DEATH_LOOK)

    def test_wait_frames_sets_how_long_a_kill_waits_for_the_death(self):   # the 36-frame default is a parameter
        self.game.death_delay = 14
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7, "wait_frames": 0})
        self.assertIs(r["died"], False)                 # no wait: not seen dead yet
        self.game.death_delay = 14
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7, "wait_frames": 120})
        self.assertIs(r["died"], True)
        with self.assertRaises(Exception):
            self.tool.call("kill", {"actor": "Enemy_Far", "id": 7, "wait_frames": 601})

    def test_the_death_wait_is_exactly_wait_frames(self):             # reviewer-6 nit: no rounding up to a block of 6
        self.game.death_delay = None
        start = self.game.frame
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7, "wait_frames": 10})
        self.assertEqual(r["died_after"], 10)
        self.assertLessEqual(self.game.frame - start, 2 + ph.STRIKE_FRAMES + 10)

    def test_a_hit_does_not_wait_for_a_death(self):
        self.game.death_delay = None
        start = self.game.frame
        r = self.tool.call("hit", {"actor": "Enemy_Far", "id": 7, "amount": 1})
        self.assertEqual(r["result"], "ok")
        self.assertLessEqual(self.game.frame - start, ph.STRIKE_FRAMES + ph.STRIKE_LOOK)

    def test_a_kill_with_a_stale_world_list_says_it_could_not_check(self):
        self.game.death_delay = None
        self.game.dump_hook = lambda: self.game.world.update(stale=True, why="stale: no walk for 3000 ms")
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertTrue(r["result"].startswith("struck: the hit landed, but its death could not be checked"), r["result"])
        self.assertIsNone(r["died"])

    def test_gone_is_refused_in_words_and_nothing_is_retried(self):
        self.game.strike_word = "gone"
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertEqual(r["result"], "refused: actor 7 (Enemy_Far) is gone")
        self.assertEqual(self.acts(), ["kill 7 Enemy_Far"])     # no second line, no other actor
        self.game.strike_word = "none with a life"
        r = self.tool.call("hit", {"actor": "Enemy_Far", "id": 7, "amount": 2})
        self.assertEqual(r["result"], "refused: actor 7 (Enemy_Far) has no life left")
        r = self.tool.call("kill", {"actor": "Enemy_Far"})
        self.assertEqual(r["result"], "refused: none of that name has a life")

    def test_no_result_after_12_frames_is_unknown_and_stops_there(self):
        self.game.strike_word = None
        start = self.game.frame
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertTrue(r["result"].startswith("unknown: no result after 12 frames"), r["result"])
        self.assertEqual(self.game.frame - start, 12)
        self.assertEqual(self.acts(), ["kill 7 Enemy_Far"])

    def test_a_late_result_inside_12_frames_still_counts(self):
        self.game.strike_delay = 10
        self.assertEqual(self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})["result"], "ok")

    def test_a_newer_strike_in_the_book_is_not_taken_for_this_one(self):
        self.game.strike = {"seq": 9, "result": "done", "name": "X", "id": 0}
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertTrue(r["result"].startswith("unknown"), r["result"])

    def test_solo_refuses_an_id_but_not_a_name(self):
        self.game.solo = True
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertTrue(r["result"].startswith("refused: no world list"), r["result"])
        self.assertEqual(self.game.strike_seq, 0)               # nothing was queued
        self.assertTrue(self.tool.call("kill", {"actor": "Enemy_Far"})["result"].startswith("struck"))

    def test_a_module_without_strike_numbers_is_said_so(self):
        self.game.strike_seq_in_reply = False
        r = self.tool.call("kill", {"actor": "Enemy_Far"})
        self.assertTrue(r["result"].startswith("unknown: the bridge gave no strike number"), r["result"])

    def test_bad_ids_are_refused_before_anything_is_sent(self):
        before = len(self.game.log)
        for bad in (0, -3, "abc", True, 2 ** 64, 1.5, [7], "0x"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                self.tool.call("kill", {"actor": "Enemy_Far", "id": bad})
        with self.assertRaises(ValueError):
            self.tool.call("kill", {"id": 7})                    # an id needs the name beside it
        with self.assertRaises(ValueError):
            self.tool.call("hit", {"actor": "Enemy_Far", "id": 7, "amount": 0})
        self.assertEqual(len(self.game.log), before)

    def test_the_tool_texts_describe_the_id_and_the_wait(self):
        by = {t["name"]: t["description"] for t in mcp_playtest.ACT_TOOLS + mcp_playtest.PLAYER_TOOLS}
        for name in ("kill", "hit"):
            self.assertIn("id", by[name])
            self.assertIn("12 frames", by[name])
        self.assertIn("never falls back", by["kill"])
        self.assertIn("refused in solo", by["kill"])

    # --- the helpers --------------------------------------------------------------------------------------------
    def test_go_to_actor_arrives_within_the_bound(self):
        r = ph.go_to_actor(self.c, 3)
        self.assertTrue(r["arrived"], r)
        self.assertEqual(r["reason"], "near")
        self.assertLessEqual(r["frames"], ph.MAX_FRAMES)
        self.assertLessEqual(r["distance"], ph.NEAR_M)
        self.assertGreater(r["moved_m"], 3.0)

    def test_go_to_actor_stops_when_the_id_is_gone_or_has_another_name(self):
        r = ph.go_to_actor(self.c, 99)
        self.assertEqual((r["arrived"], r["reason"]), (False, "gone"))
        r = ph.go_to_actor(self.c, 3, name="Enemy_Other")        # the name get_world gave is not the one there
        self.assertEqual((r["arrived"], r["reason"]), (False, "gone"))
        seen = []

        def rename():
            seen.append(1)
            if len(seen) >= 3:
                self.actor(3)["name"] = "Enemy_Reused"          # the address now holds another actor
        self.game.dump_hook = rename
        r = ph.go_to_actor(self.c, 3, name="Enemy_Near")
        self.assertEqual((r["arrived"], r["reason"]), (False, "gone"))
        self.assertLess(r["frames"], ph.MAX_FRAMES)

    def test_one_missed_read_is_not_gone(self):
        n, hidden = [], {}

        def blink():
            n.append(1)
            acts = self.game.world["actors"]
            if len(n) == 2:                                     # one read without the actor
                hidden.update(a=self.actor(3))
                acts.remove(hidden["a"])
            elif hidden.get("a") is not None:
                acts.append(hidden.pop("a"))
        self.game.dump_hook = blink
        r = ph.go_to_actor(self.c, 3, name="Enemy_Near")
        self.assertTrue(r["arrived"], r)

    def test_go_to_actor_says_stuck_with_how_far_it_got(self):
        self.game.wall_x = 1.0
        r = ph.go_to_actor(self.c, 3, stuck_after=40)
        self.assertEqual((r["arrived"], r["reason"], r["stuck"]), (False, "stuck", True))
        self.assertLess(r["moved_m"], 3.0)
        self.assertLessEqual(r["frames"], ph.MAX_FRAMES)

    def test_go_to_actor_is_bounded_and_says_so(self):
        r = ph.go_to_actor(self.c, 7, max_frames=20)
        self.assertEqual((r["arrived"], r["reason"]), (False, "timed out"))
        self.assertLessEqual(r["frames"], 20)
        self.assertLessEqual(sum(m["frames"] for m in self.game.log if m["cmd"] == "input"), 20)

    def test_go_to_actor_with_no_world_list_says_why(self):
        self.game.world = {"frame": None, "why": "no co-op session: the world list is read only while connected and "
                           "sharing", "count": 0, "actors": []}
        r = ph.go_to_actor(self.c, 3)
        self.assertFalse(r["arrived"])
        self.assertTrue(r["reason"].startswith("no world list"), r["reason"])
        self.game.world = None
        self.assertTrue(ph.go_to_actor(self.c, 3)["reason"].startswith("no world list"))

    def test_an_actor_that_is_listed_but_not_shared_is_not_called_gone(self):
        self.game.strike_word = "not shared"
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertEqual(r["result"], "refused: actor 7 (Enemy_Far) is here, but co-op does not share it, so a test "
                                      "strike cannot reach it")
        self.assertNotIn("gone", r["result"])

    def test_a_full_queue_and_an_old_module_are_told_apart(self):
        self.game.queue_full = True
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertTrue(r["result"].startswith("refused: the strike queue is full"), r["result"])
        self.game.queue_full, self.game.strike_seq_in_reply = False, False
        r = self.tool.call("kill", {"actor": "Enemy_Far", "id": 7})
        self.assertTrue(r["result"].startswith("unknown: the bridge gave no strike number"), r["result"])
        self.assertNotIn("older module?", r["result"])

    def test_a_scene_change_is_not_called_gone(self):
        # a leg through a door: the new scene's list lacks the actor and the old list goes stale
        n = []

        def leave():
            n.append(1)
            if len(n) >= 2:
                self.game.scene = "KokiriForest"
                self.game.world = {"frame": 1010, "stale": True, "count": 0, "actors": [],
                                   "why": "stale: no walk for 2000 ms (a load, or the session ended)"}
        self.game.dump_hook = leave
        r = ph.go_to_actor(self.c, 7, name="Enemy_Far")
        self.assertEqual((r["arrived"], r["reason"]), (False, "scene changed"))
        # (the same with the actor simply missing from the new scene's list)
        self.game.scene, self.game.world, n[:] = "ForestTemple", json.loads(json.dumps(self.WORLD)), []

        def leave2():
            n.append(1)
            if len(n) >= 2:
                self.game.scene = "KokiriForest"
                self.game.world["actors"] = [a for a in self.game.world["actors"] if a["id"] != 7]
        self.game.dump_hook = leave2
        self.assertEqual(ph.go_to_actor(self.c, 7, name="Enemy_Far")["reason"], "scene changed")

    def test_a_load_and_a_stale_list_are_reported_before_gone(self):
        self.game.loading = True
        self.assertEqual(ph.go_to_actor(self.c, 3, name="Enemy_Near")["reason"], "loading")
        self.game.loading = False
        self.game.world = {"frame": 900, "stale": True, "count": 0, "actors": [],
                           "why": "stale: no walk for 2000 ms (a load, or the session ended)"}
        r = ph.go_to_actor(self.c, 3, name="Enemy_Near")
        self.assertTrue(r["reason"].startswith("stale world list"), r["reason"])
        self.assertEqual(ph.face(self.c, 3, "Enemy_Near")["reason"][:16], "stale world list")
        self.assertEqual(ph.attack(self.c, 3)["stopped"][:16], "stale world list")
        # a walk that ran and saw no such actor is gone
        self.game.world = {"frame": 1000, "stale": False, "count": 0, "actors": [],
                           "why": "empty: the walk ran and saw no actors"}
        self.assertEqual(ph.go_to_actor(self.c, 3, name="Enemy_Near")["reason"], "gone")

    def test_attack_stops_with_the_scene_change_not_gone(self):
        self.actor(3)["pos"] = [2, 0, 0]

        def door(buttons):
            self.game.scene = "KokiriForest"
            self.game.world["actors"] = []
        self.game.on_press = door
        r = ph.attack(self.c, 3)
        self.assertEqual((r["result"], r["stopped"]), ("stopped", "scene changed"))

    def test_attack_says_when_the_life_never_moved(self):
        self.actor(3)["pos"] = [2, 0, 0]
        r = ph.attack(self.c, 3, max_frames=60)
        self.assertEqual(r["result"], "alive")
        self.assertIn("never moved", r["hint"])
        self.assertIn("button Y", r["hint"])
        self.game.on_press = lambda buttons: self.actor(3).update(life=3)
        self.assertNotIn("hint", ph.attack(self.c, 3, max_frames=60))

    def test_go_to_actor_leaves_the_other_game_alone_unless_both(self):
        ph.go_to_actor(self.c, 3)
        self.assertEqual(self.other.frame, 1000)
        self.game.pos = [0.0, 0.0, 0.0]
        pair = ph.InStep(self.c, pc.Client(self.other.port))
        try:
            start = self.game.frame
            ph.go_to_actor(pair, 3)
            self.assertGreater(self.game.frame, start)
            self.assertEqual(self.other.frame - 1000, self.game.frame - start)   # the same frames in both games
        finally:
            pair.other.close()

    def test_attack_walks_faces_and_presses_until_the_life_is_0(self):
        self.game.on_press = lambda buttons: self.actor(3).update(life=max(0, self.actor(3)["life"] - 2)) \
            if "Y" in buttons else None
        r = ph.attack(self.c, 3)
        self.assertEqual(r["result"], "dead", r)
        self.assertEqual((r["life_before"], r["life_after"]), (4, 0))
        self.assertLessEqual(r["frames"], ph.MAX_FRAMES)
        self.assertFalse(any(m.get("lines", "").startswith("face ") for m in self.game.log if m["cmd"] == "act"))
        self.assertEqual(sum(1 for m in self.game.log if m["cmd"] == "input" and m["buttons"] == ["Y"]), 2)

    def test_attack_uses_the_button_it_is_given(self):
        self.game.on_press = lambda buttons: self.actor(3).update(life=0) if "B" in buttons else None
        self.assertEqual(ph.attack(self.c, 3, button="B")["result"], "dead")
        self.assertFalse(any(m["buttons"] == ["Y"] for m in self.game.log if m["cmd"] == "input"))

    def test_attack_never_presses_at_an_actor_that_is_gone(self):
        self.assertEqual(ph.attack(self.c, 99)["stopped"], "gone")
        self.assertFalse([m for m in self.game.log if m["cmd"] == "input"])
        # it leaves the list when the first press lands: the next round sees it gone and presses no more
        self.game.on_press = lambda buttons: self.game.world["actors"].remove(self.actor(3))
        r = ph.attack(self.c, 3)
        self.assertEqual(r["result"], "gone")
        self.assertEqual(sum(1 for m in self.game.log if m["cmd"] == "input" and m["buttons"] == ["Y"]), 1)

    def test_attack_reports_a_walk_that_got_stuck_and_is_bounded(self):
        self.game.wall_x = 1.0
        r = ph.attack(self.c, 3)
        self.assertEqual((r["result"], r["stopped"]), ("stopped", "stuck"))
        self.assertTrue(r["stuck"])
        self.assertLessEqual(r["frames"], ph.MAX_FRAMES)

    def test_attack_that_does_not_kill_stops_at_the_frame_limit(self):
        self.actor(3)["pos"] = [2, 0, 0]
        r = ph.attack(self.c, 3, max_frames=60)
        self.assertEqual(r["result"], "alive")
        self.assertLessEqual(r["frames"], 60 + 2 * ph.PRESS_FRAMES)
        self.assertEqual((r["life_before"], r["life_after"]), (4, 4))

    def test_fight_tool_locks_on_slashes_and_ends_with_a_look(self):
        self.actor(3)["pos"] = [1, 0, 0]
        self.game.on_press = lambda buttons: self.actor(3).update(life=0) if "Y" in buttons and "ZL" in buttons else None
        r = self.tool.call("fight", {"id": 3, "name": "Enemy_Near", "max_frames": 90})
        self.assertEqual(r["result"], "dead", r)
        self.assertIn("look", r)
        inputs = [m["buttons"] for m in self.game.log if m["cmd"] == "input"]
        self.assertEqual(inputs[:2], [["ZL", "Y"], ["ZL"]])        # Longshot's map: lock-on ZL, sword Y, let go
        self.assertFalse([m for m in self.game.log if m["cmd"] == "step"])   # ZL is never let go between inputs
        for args in ({"id": 3, "name": "Enemy_Near", "sword": "Q"}, {"id": 3}, {"name": "Enemy_Near"},
                     {"id": 3, "name": "Enemy_Near", "max_frames": 1201}):
            with self.assertRaises(ValueError, msg=args):
                self.tool.call("fight", args)

    def test_the_helper_tools_take_the_id_and_the_name_and_are_bounded(self):
        r = self.tool.call("go_to_actor", {"id": "3", "name": "Enemy_Near"})
        self.assertEqual((r["arrived"], r["reason"]), (True, "near"))
        self.actor(3)["pos"] = [3, 0, 0]
        self.game.on_press = lambda buttons: self.actor(3).update(life=0) if "B" in buttons else None
        r = self.tool.call("attack", {"id": 3, "name": "Enemy_Near", "button": "b", "max_frames": 90})
        self.assertEqual(r["result"], "dead")
        self.assertEqual(self.tool.call("face", {"id": 3, "name": "Enemy_Near"})["reason"], "ok")
        self.assertEqual(self.tool.call("go_to_actor", {"id": 3, "name": "Enemy_Wrong"})["reason"], "gone")
        before = len(self.game.log)
        for tool, args in (("go_to_actor", {"name": "Enemy_Near"}), ("go_to_actor", {"id": 3}),
                           ("attack", {"id": 3, "name": "Enemy_Near", "button": "Q"}),
                           ("attack", {"id": 3, "name": "Enemy_Near", "max_frames": 121}),
                           ("go_to_actor", {"id": 3, "name": "E", "max_frames": 0}),
                           ("go_to_actor", {"id": 0, "name": "E"}), ("use_item", {"item": "A;B"})):
            with self.assertRaises(ValueError, msg=tool):
                self.tool.call(tool, args)
        self.assertEqual(len(self.game.log), before)
        by = {t["name"]: t["description"] for t in mcp_playtest.PLAYER_TOOLS}
        for name in ("go_to_actor", "attack"):
            self.assertIn("120 frames", by[name])
            self.assertIn("never goes for another", by[name])
        self.assertIn("Longshot's attack button", by["attack"])
        self.assertIn("`shield` ZL", by["fight"])

    def test_talk_and_open_press_a_only_with_a_prompt(self):
        self.assertFalse(self.tool.call("talk", {})["pressed"])
        self.game.prompt = "Open"
        self.assertTrue(self.tool.call("open", {})["pressed"])

    def test_use_item_assigns_presses_and_says_what_changed(self):
        self.game.on_press = lambda buttons: setattr(self.game, "rupees", 9)
        r = ph.use_item(self.c, "Bow")
        self.assertEqual(r["changed"]["rupees"], {"was": 5, "now": 9})
        self.assertIn("assign Bow", self.acts())
        self.assertEqual([m["buttons"] for m in self.game.log if m["cmd"] == "input"], [["ZR"]])   # assign's button
        r = ph.use_item(self.c, "Hookshot", button="X")
        self.assertEqual([m["buttons"] for m in self.game.log if m["cmd"] == "input"][-1], ["X"])

    def test_interact_walks_to_the_actor_faces_it_and_presses_a(self):
        self.game.prompt = "Open"
        self.game.on_press = lambda buttons: setattr(self.game, "prompt", None)
        r = self.tool.call("interact", {"id": 5, "name": "Obj_TBox_Wood"})
        self.assertTrue(r["pressed"], r)
        self.assertTrue(r["walk"]["arrived"])
        self.assertLessEqual(r["walk"]["distance"], ph.INTERACT_M)
        self.assertFalse(any(a.startswith("face") for a in self.acts()), self.acts())   # turned with the stick
        self.assertEqual(r["prompt_was"], "Open")

    def test_interact_presses_nothing_when_it_did_not_get_there(self):
        self.game.prompt = "Open"
        r = self.tool.call("interact", {"id": 99, "name": "Obj_TBox_Wood"})
        self.assertEqual((r["pressed"], r["result"]), (False, "not reached: gone"))
        r = self.tool.call("interact", {"id": 7, "name": "Enemy_Far", "max_frames": 20})
        self.assertEqual((r["pressed"], r["result"]), (False, "not reached: timed out"))
        self.assertFalse([m for m in self.game.log if m["cmd"] == "input" and "A" in (m.get("buttons") or [])])

    def test_interact_presses_a_only_when_there_is_a_prompt(self):
        r = ph.interact(self.c)
        self.assertFalse(r["pressed"])
        self.assertFalse([m for m in self.game.log if m["cmd"] == "input"])
        self.game.prompt = "Open"
        self.game.on_press = lambda buttons: setattr(self.game, "prompt", None)
        r = ph.interact(self.c, "open")
        self.assertTrue(r["pressed"])
        self.assertEqual((r["prompt_was"], r["prompt"]), ("Open", None))
        self.assertEqual([m["buttons"] for m in self.game.log if m["cmd"] == "input"], [["A"]])


class LiveStartPlaytest(unittest.TestCase):
    def args(self, **kw):
        a = dict(session="s", lane=None, game_a="ga", game_b="gb", server="coop_server.exe", port=None, boot_a=None,
                 boot_b=None, load=None, no_load=False, no_restore=False, env=[], boot_timeout=900, max_minutes=30,
                 idle_minutes=10, lock_wait=0, playtest=False)
        a.update(kw)
        return argparse.Namespace(**a)

    def test_off_unless_asked(self):
        self.assertNotIn("--playtest-ports", live.runner_argv(self.args(), {"A": "ga", "B": "gb"}, 1, "p", "dir"))

    def test_ports_match_the_mcp_servers(self):
        for lane in (None, 1, 2):
            argv = live.runner_argv(self.args(playtest=True, lane=lane), {"A": "ga", "B": "gb"}, 1, "p", "dir")
            base = int(argv[argv.index("--playtest-ports") + 1])
            self.assertEqual((base + 1, base + 2), (pc.port_for("p1", lane), pc.port_for("p2", lane)))


class KitThreeFixes(unittest.TestCase):
    """The play-test kit made cheap and honest (playtest-kit-3): no bulk in a reply, an Empty prompt presses nothing, a
    match does not eat the log, a zero count is no refusal, place with a facing, a walk that says it walks away."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.game = FakeGame(dump_dir=self.tmp.name)
        self.game.items = {"Item_%03d" % i: 1 + i % 3 for i in range(290)}
        self.game.items["Item_Zero"] = 0
        self.game.world = {"frame": 1000, "age_ms": 40, "stale": False, "behind": False, "total": 1, "other_count": 0,
                           "count": 1, "actors": [{"id": 3, "addr": "0x80000003", "name": "Enemy_Near", "kind": "enemy",
                                                   "pos": [6, 0, 0], "dist": 6.0, "life": 4, "max_life": 4,
                                                   "owner": 2, "alive": True}]}
        self.tool = mcp_playtest.Game("p1", pc.Client(self.game.port), self.tmp.name)
        self.c = self.tool.client

    def tearDown(self):
        self.tool.client.close()
        self.game.close()
        self.tmp.cleanup()

    def acts(self):
        return [m["lines"] for m in self.game.log if m["cmd"] == "act"]

    # --- 1: the inventory only on request, in no verb reply ------------------------------------------------------------
    def test_get_state_has_no_inventory_unless_asked(self):
        r = self.tool.call("get_state", {})
        self.assertNotIn("inventory", r)
        self.assertNotIn("world", r)
        self.assertEqual(r["inventory_count"], 290)                      # the zero entry is not counted
        self.assertEqual(r["enemy_near"], 1)                             # the words still come from the world list
        self.assertLess(len(json.dumps(r)), 1500)
        r = self.tool.call("get_state", {"inventory": True})
        self.assertEqual(len(r["inventory"]), 290)                       # non-zero entries only
        self.assertNotIn("Item_Zero", r["inventory"])
        self.assertNotIn("world", r)

    def test_no_verb_reply_carries_the_inventory_or_the_world(self):
        self.game.event = True
        calls = [("warp", {"scene": "ForestTemple"}), ("step", {"frames": 2}), ("press", {"frames": 2}),
                 ("place", {"x": 1, "y": 0, "z": 2}), ("place", {"x": 1, "y": 0, "z": 2, "facing": 90}),
                 ("give", {"item": "bombs"}), ("hurt", {"quarter_hearts": 1}), ("flag", {"name": "F1", "value": 1}),
                 ("walk_to", {"x": 1, "z": 1}), ("use_item", {"item": "Bow"}), ("talk", {}), ("open", {})]
        self.game.prompt = "Open"
        for name, args in calls:
            text = json.dumps(self.tool.call(name, args))
            self.assertNotIn('"inventory"', text, name)
            self.assertNotIn('"world"', text, name)
            self.assertNotIn("Item_100", text, name)
            self.assertLess(len(text), 3000, name)

    def test_use_item_names_the_inventory_entries_that_changed(self):
        self.game.on_press = lambda buttons: self.game.items.update(Item_005=9, Item_New=1)
        r = self.tool.call("use_item", {"item": "Bow"})
        self.assertEqual(r["changed"]["inventory_changed"], {"Item_005": {"was": 3, "now": 9},
                                                              "Item_New": {"was": 0, "now": 1}})
        self.assertNotIn("inventory", r["state"])

    def test_slim_state_leaves_a_reply_without_a_state_alone(self):
        d = {"actors": [{"id": 1, "inventory": 5}], "frame": 3, "state": {"scene": "A", "inventory": {"x": 1, "y": 0}}}
        r = pc.slim_state(d)
        self.assertEqual(r["actors"], d["actors"])
        self.assertEqual(r["state"], {"scene": "A", "inventory_count": 1})
        self.assertEqual(d["state"]["inventory"], {"x": 1, "y": 0})      # the input is not changed

    # --- 3: Empty is no prompt -----------------------------------------------------------------------------------------
    def test_open_and_talk_press_nothing_for_an_empty_prompt(self):
        for word in ("Empty", "empty", " None ", "", None):
            self.game.prompt = word
            for name in ("open", "talk"):
                r = self.tool.call(name, {})
                self.assertFalse(r["pressed"], (word, name))
        self.assertFalse([m for m in self.game.log if m["cmd"] == "input"])
        self.game.prompt = "Open"
        self.assertTrue(self.tool.call("open", {})["pressed"])

    # --- 4: log with a match does not move the plain cursor -----------------------------------------------------------
    def session(self, text=""):
        d = os.path.join(self.tmp.name, "session")
        os.makedirs(d, exist_ok=True)
        logs = {x: os.path.join(d, "game-out-%s.txt" % x) for x in live.TAGS}
        with open(logs["A"], "w", encoding="utf-8", newline="") as f:
            f.write("boot coop: x\n")
        live.save(d, {"started": 1000.0, "offsets": {"A": os.path.getsize(logs["A"]), "B": 0},
                      "games": {x: {"log": logs[x]} for x in live.TAGS}})
        g = mcp_playtest.Game("p1", self.c, self.tmp.name, session=d)
        g.log_lines()          # the cursor starts after the boot
        return g, logs["A"]

    def write(self, path, text):
        with open(path, "a", encoding="utf-8", newline="") as f:
            f.write(text)

    def test_a_match_leaves_the_lines_it_did_not_return_for_the_next_look(self):
        g, path = self.session()
        self.write(path, "coop: a one\ncoop: b two\ncoop: a three\n")
        self.assertEqual(g.call("log", {"match": "a "})["lines"], ["coop: a one", "coop: a three"])
        self.assertEqual(g.call("log", {"match": "a "})["lines"], [])               # nothing new for this match
        self.write(path, "coop: a four\n")
        self.assertEqual(g.call("log", {"match": "a "})["lines"], ["coop: a four"])
        self.assertEqual(g.call("log", {})["lines"], ["coop: a one", "coop: b two", "coop: a three", "coop: a four"])

    def test_a_match_with_more_lines_than_the_limit_loses_none(self):
        g, path = self.session()
        n = live.MAX_LINES + 7
        self.write(path, "".join("coop: m %d\ncoop: other %d\n" % (i, i) for i in range(n)))
        first = g.call("log", {"match": "m "})["lines"]
        second = g.call("log", {"match": "m "})["lines"]
        self.assertEqual(len(first), live.MAX_LINES)
        self.assertEqual(first + second, ["coop: m %d" % i for i in range(n)])

    def test_a_half_written_line_waits_for_its_end(self):
        g, path = self.session()
        self.write(path, "coop: a one\ncoop: a tw")
        self.assertEqual(g.call("log", {"match": "a "})["lines"], ["coop: a one"])
        self.write(path, "o\n")
        self.assertEqual(g.call("log", {"match": "a "})["lines"], ["coop: a two"])

    # --- 5: a zero count is no refusal ---------------------------------------------------------------------------------
    def test_a_zero_count_is_not_a_flagged_word(self):
        quiet = ("coop: actors: in 10 s the game asked 4 creates (0 copies' shots refused), 2 deletes, 0 hits (life 0), "
                 "0 lost; owners' deaths past a copy's own life stops 0, past its revive 0 (later steps kept dead 0)")
        self.assertIsNone(pc.flag_reason(quiet))
        self.assertIsNone(pc.flag_reason("coop: save: 0 failed, 0 errors, 0 refusals, 0 resyncs"))
        self.assertEqual(pc.flag_reason(quiet.replace("(0 copies", "(3 copies")), "word:refused")
        self.assertEqual(pc.flag_reason("coop: save: 10 failed"), "word:failed")
        self.assertEqual(pc.flag_reason("coop: peer refused us (0 retries left)"), "word:refused")
        self.assertEqual(pc.flag_reason("warning: coop: actors: in 10 s (0 copies' shots refused)"), "level:warning")

    def test_the_flags_traffic_line_is_flagged_only_for_a_real_write_failure(self):
        # coop_hook.cpp LogFlagTraffic: "{} write failures ({} tries of held flags, not failures)" prints every 10 s
        line = ("coop: flags: traffic: 4 changes sent, 1 answers sent, 9 entries came, 7 written here (2 count deltas, "
                "0 load reverts, 0 one-way put back, 0 from a host reset), 0 kept over our own, 3 not shared, "
                "%s write failures (%s tries of held flags, not failures), 0 messages not for us, 0 bad snapshots; "
                "0 s holding a join snapshot (0 dropped unfinished), 0 s in load windows")
        self.assertIsNone(pc.flag_reason(line % (0, 0)))
        self.assertIsNone(pc.flag_reason(line % (0, 3)))                 # tries are not failures
        self.assertEqual(pc.flag_reason(line % (2, 3)), "word:failure")
        self.assertEqual(pc.flag_reason(line % (2, 0)), "word:failure")
        self.assertIsNone(pc.flag_reason("coop: net: no errors since the join"))
        self.assertEqual(pc.flag_reason("coop: net: 2 errors, not fatal"), "word:error")

    # --- 6: place with a facing, and a walk that says it walks away ---------------------------------------------------
    def test_place_with_a_facing_places_then_turns(self):
        r = self.tool.call("place", {"x": 10, "y": 1.5, "z": 20, "facing": 90})
        self.assertEqual(self.acts(), ["place 10 1.5 20", "face 11.00 20.00"])
        self.assertEqual(self.game.pos, [10.0, 1.5, 20.0])
        self.assertEqual(self.game.faced, (11.0, 20.0))
        self.assertEqual(r["frames"], 4)
        r = self.tool.call("place", {"x": 0, "y": 0, "z": 0, "facing": 180})
        self.assertEqual(self.acts()[-1], "face 0.00 -1.00")
        self.tool.call("place", {"x": 0, "y": 0, "z": 0, "face_x": 5, "face_z": 7.5})
        self.assertEqual(self.acts()[-2:], ["place 0 0 0", "face 5.00 7.50"])

    def test_place_without_a_facing_is_one_line_and_bad_facings_are_refused(self):
        self.tool.call("place", {"x": 1, "y": 2, "z": 3})
        self.assertEqual(self.acts(), ["place 1 2 3"])
        for extra in ({"facing": 90, "face_x": 1, "face_z": 1}, {"face_x": 1}, {"facing": 400},
                      {"facing": float("nan")}):
            with self.assertRaises(ValueError, msg=str(extra)):
                self.tool.call("place", dict({"x": 1, "y": 2, "z": 3}, **extra))
        self.assertEqual(self.acts(), ["place 1 2 3"])

    def test_walk_to_is_not_fooled_when_the_stick_turns_over_under_it(self):
        game = FakeGame(camera=0.0)
        game.flip_at = 1060
        c = pc.Client(game.port)
        try:
            r = pc.walk_to(c, 12.0, 9.0, tolerance=0.4, max_frames=1500)
        finally:
            c.close()
            game.close()
        self.assertTrue(r["arrived"], (r["reason"], r["distance"]))
        self.assertTrue(r["trace"])

    def test_walk_to_that_only_gets_farther_stops_and_says_so(self):
        game = FakeGame()
        game.away_from = (4.0, 4.0)
        c = pc.Client(game.port)
        try:
            r = pc.walk_to(c, 4.0, 4.0, tolerance=0.3, max_frames=2000)
        finally:
            c.close()
            game.close()
        self.assertFalse(r["arrived"])
        self.assertTrue(r["reason"].startswith("moving away"), r["reason"])
        self.assertLess(r["frames"], 400)                                # it did not walk on for 2000 frames
        self.assertLessEqual(len(r["trace"]), pc.TRACE_KEEP)
        self.assertEqual(len(r["trace"][0]), 7)

    def test_walk_to_reply_trace_is_bounded(self):
        game = FakeGame()
        c = pc.Client(game.port)
        try:
            r = pc.walk_to(c, 300.0, 0.0, tolerance=0.3, max_frames=800)
        finally:
            c.close()
            game.close()
        self.assertEqual(r["reason"], "timed out")
        self.assertEqual(len(r["trace"]), pc.TRACE_KEEP)

    def test_steering_side_steps_when_stuck_and_comes_back(self):
        st = pc.Steering()
        self.assertEqual(st.push([0.0, 0.0, 0.0], (5.0, 0.0)), [90.0, 1.0])
        for _ in range(3):                                               # three pushes that move Link nowhere
            yaw, _s = st.push([0.0, 0.0, 0.0], (5.0, 0.0))
        self.assertEqual(yaw, 0.0)                                       # 90 - 90: aside, toward +z
        st.push([0.0, 0.0, 0.5], (5.0, 0.0))
        st.push([0.0, 0.0, 1.0], (5.0, 0.0))
        yaw, _s = st.push([0.5, 0.0, 1.0], (5.0, 0.0))
        self.assertAlmostEqual(yaw, pc.yaw_to([0.5, 0.0, 1.0], 5.0, 0.0), places=1)   # straight at it again
        self.assertEqual(st.side_steps, 0)                               # stuck is not "getting farther"

    def test_steering_eases_off_near_the_target(self):
        self.assertEqual(pc.Steering().push([0.0, 0.0, 0.0], (0.0, 1.5))[1], 0.5)
        self.assertEqual(pc.Steering().push([0.0, 0.0, 0.0], (0.0, 0.2))[1], pc.MIN_STRENGTH)

    # --- 7: the docs name the right form of live.py stop ---------------------------------------------------------------
    def test_the_docs_say_stop_session_with_its_flag(self):
        root = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
        for rel in ("docs/playtest.md",):
            with open(os.path.join(root, rel), encoding="utf-8") as f:
                text = f.read()
            self.assertNotRegex(text, r"live\.py (?:do|state|shot|checkpoint|stop) (?!--)[<\w]", rel)


if __name__ == "__main__":
    unittest.main()
