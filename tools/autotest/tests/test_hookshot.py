"""Tests for tools/autotest/hookshot.py against a fake Hookshot bridge (no game needed).

The fake speaks the bridge's protocol (Hookshot docs/control.md: newline-delimited JSON, replies by id) over a socket
pair, keeps the inputs it was sent, serves guest memory from a dict and moves a fake Link by the left stick, turned by
a camera angle the player does not know."""
import glob
import json
import math
import os
import shutil
import socket
import struct
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import hookshot as hs  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
GAMEUI = 0x1000200000


class Clock:
    """Time that moves only when slept."""

    def __init__(self):
        self.t = 1000.0
        self.lock = threading.Lock()

    def __call__(self):
        with self.lock:
            return self.t

    def sleep(self, s):
        with self.lock:
            self.t += max(s, 0.001)


class FakeGame:
    def __init__(self, clock=None, mem=True, camera=0.0, refuse_teleports=0, drop_teleports=0, maps=None):
        self.clock = clock or Clock()
        self.maps = maps         # the bridge's own list of maps (None: any); another is not_found
        self.mem = mem
        self.camera = camera
        self.refuse = refuse_teleports
        self.drop = drop_teleports   # sent but dropped by the game (Longshot rc.3: "result 0")
        self.map = "KokiriForest_Child"
        self.got = []            # (time, request)
        self.held = []           # inputs not let go: requests
        self.held_by = {}        # id(request) -> the connection that sent it (a release lets go of its own only)
        self.pos = [0.0, 0.0, 0.0]
        self.moved_at = None
        self.state = "menu"
        self.presses_to_play = 3
        self.speed = 3.0         # metres a second at full stick
        self.lock = threading.Lock()
        self.conns = []
        self.bridges = []

    # Link moves by the newest left stick, turned by the camera (stick up = away from the camera = world -z at 0).
    def _move(self):
        now = self.clock()
        if self.moved_at is not None:
            dt = now - self.moved_at
            sticks = [r for r in self.held if r["cmd"] == "stick" and r.get("stick", "L") == "L"]
            if sticks:
                sx, sy = sticks[-1]["x"], sticks[-1]["y"]
                a = math.atan2(sy, sx) + self.camera
                m = min(1.0, math.hypot(sx, sy)) * self.speed * dt
                self.pos[0] += math.cos(a) * m
                self.pos[2] -= math.sin(a) * m
        self.moved_at = now

    def answer(self, req, conn=None):
        cmd = req.get("cmd")
        base = {"id": req.get("id"), "ok": True, "frame": 100}
        with self.lock:
            self.got.append((self.clock(), req))
            self._move()
            if cmd in ("press", "stick", "sequence"):
                if req.get("wait", True) is not False:
                    return dict(base, ok=False, error="bad_request", message="test wants wait false")
                if cmd == "press" and "A" in req.get("buttons", []) and self.state != "play":
                    self.presses_to_play -= 1
                    if self.presses_to_play <= 0:
                        self.state = "play"
                self.held.append(req)
                self.held_by[id(req)] = conn
                return dict(base, queued=1, frames=req.get("frames", 8))
            if cmd == "release":
                # Longshot docs/control.md: the caller's inputs are let go; another connected client's stay.
                kept = [r for r in self.held if self.held_by.get(id(r)) is not conn]
                n, self.held = len(self.held) - len(kept), kept
                return dict(base, released=n, all=False)
            if cmd == "status":
                return dict(base, state=self.state, map="KokiriForest_Child" if self.state == "play" else None,
                            game={"event": False, "loading": False, "age": "child"})
            if cmd == "mem_read":
                if not self.mem:
                    return dict(base, ok=False, error="forbidden", message="HOS_CONTROL_MEM")
                addr, size = req["addr"], req["size"]
                if addr == hs.MAIN_BASE + hs.GAMEUI_SLOT and size == 8:
                    return dict(base, data=struct.pack("<Q", GAMEUI).hex())
                if addr == GAMEUI + hs.POS_VALID:
                    data = b"\x01" + b"\x00" * (hs.POS - hs.POS_VALID - 1) + struct.pack("<3f", *self.pos)
                    return dict(base, data=data[:size].hex())
                return dict(base, ok=False, error="out_of_range", message="")
            if cmd == "teleport":
                if self.maps is not None and req["to"] not in self.maps:
                    return dict(base, ok=False, error="not_found", message="to: a destination's index or map")
                if self.refuse > 0:
                    self.refuse -= 1
                    return dict(base, ok=False, error="refused", message="a map loading")
                if self.drop > 0:
                    self.drop -= 1
                    return dict(base, teleport=1, sent=True, accepted=False, map=req["to"])
                self.map = req["to"]
                return dict(base, teleport=1, sent=True, accepted=True, map=req["to"])
            if cmd == "wait" and req.get("event") == "map":
                if req.get("map", "").lower() == self.map.lower():
                    return dict(base, state="play", waited=1, map=self.map)
                self.clock.t += req.get("timeout", 60)
                return dict(base, ok=False, error="timeout", message="map", state="play", map=self.map)
            if cmd in ("shot", "wait", "quit"):
                return dict(base)
            return dict(base, ok=False, error="unknown_cmd", message=cmd)

    def serve(self, sock):
        buf = b""
        while True:
            try:
                chunk = sock.recv(65536)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                reply = self.answer(json.loads(line), sock)
                sock.sendall((json.dumps(reply) + "\n").encode())

    def connect(self):
        a, b = socket.socketpair()
        self.conns += [b]
        threading.Thread(target=self.serve, args=(b,), daemon=True).start()
        bridge = hs.Bridge(a)
        self.bridges.append(bridge)
        return bridge

    def close(self):
        for b in self.bridges:
            b.close()
        for c in self.conns:
            c.close()

    def cmds(self, *names):
        return [r for _, r in self.got if r["cmd"] in names]


def play(text, game=None, **kw):
    game = game or FakeGame()
    log = hs.Log()
    shots = []
    b = game.connect()
    p = hs.Player(b, log, shot=shots.append, connect=game.connect, clock=game.clock, sleep=game.clock.sleep, **kw)
    end = p.play(hs.parse(text))
    for t in p.warps:
        t.join(5)
    game.close()
    return game, log, shots, p, end


class CopyLogTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        with open(os.path.join(self.tmp, "game-out-B.txt"), "w", encoding="utf-8") as f:
            f.write("coop: connected\n")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_a_held_file_is_tried_again(self):
        real, calls = shutil.copy2, []

        def flaky(src, dst):
            calls.append(src)
            if len(calls) == 1:
                raise PermissionError(32, "used by another process")
            return real(src, dst)

        said = []
        shutil.copy2 = flaky
        try:
            self.assertTrue(hs.copy_log(self.tmp, "B", self.tmp, said.append, sleep=lambda s: None))
        finally:
            shutil.copy2 = real
        self.assertEqual(len(calls), 2)
        self.assertEqual(said, [])
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, "hookshot_log-B.txt")))

    def test_a_copy_that_keeps_failing_is_said_and_does_not_raise(self):
        # testkit-0116-3 smoke: a failed copy raised out of run()'s finally after "scenario played" (exit 1).
        real = shutil.copy2

        def broken(src, dst):
            raise PermissionError(32, "used by another process")

        said = []
        shutil.copy2 = broken
        try:
            self.assertFalse(hs.copy_log(self.tmp, "B", self.tmp, said.append, tries=3, sleep=lambda s: None))
        finally:
            shutil.copy2 = real
        self.assertEqual(len(said), 1)
        self.assertIn("B: game log not copied to hookshot_log-B.txt after 3 tries (PermissionError", said[0])


class ParseTest(unittest.TestCase):
    def test_verbs(self):
        steps = hs.parse("""# comment
            wait 120
            hold LSTICK_UP+A 90 mid   # dash
            press ZR+A 10
            press B
            stick 0.5 -1 60 B
            rstick -1 0 30
            goto -7.5 71.1 1.5 600
            toward 1 2 8 A
            warp KakarikoVillage_Child Kakariko_WellSide
            chat /teleport  Saria
            mark done
            wear boots HoverBoot
            dress all
            setflag Rupee 50
            hit Enemy_DungeonBoss_Gohma 2
            stun Enemy_DungeonBoss_Gohma
        """)
        kinds = [s.kind for s in steps]
        self.assertEqual(kinds, ["timed", "timed", "timed", "timed", "timed", "timed", "timed", "timed", "goto",
                                 "toward", "warp", "chat", "mark", "wear", "dress", "setflag", "hit", "stun"])
        self.assertEqual(steps[0].ticks, 120)
        self.assertEqual((steps[1].buttons, steps[1].left, steps[1].ticks, steps[1].name), (["A"], (0, 1), 90, "mid"))
        self.assertEqual((steps[2].buttons, steps[2].ticks), (["ZR", "A"], 10))
        self.assertEqual((steps[3].buttons, steps[3].ticks), ([], 10))   # the let-go half
        self.assertEqual(steps[4].ticks, hs.PRESS_TICKS)
        self.assertEqual((steps[6].left, steps[6].buttons), ((0.5, -1.0), ["B"]))
        self.assertEqual(steps[7].right, (-1.0, 0.0))
        self.assertEqual((steps[8].x, steps[8].z, steps[8].radius, steps[8].ticks), (-7.5, 71.1, 1.5, 600))
        self.assertEqual((steps[9].ticks, steps[9].buttons), (8, ["A"]))
        self.assertEqual((steps[10].name, steps[10].start), ("KakarikoVillage_Child", "Kakariko_WellSide"))
        self.assertEqual(steps[11].name, "/teleport  Saria")
        self.assertEqual((steps[13].start, steps[13].name), ("boots", "HoverBoot"))
        self.assertEqual((steps[16].name, steps[16].start), ("Enemy_DungeonBoss_Gohma", "2"))
        self.assertEqual((steps[14].kind, steps[14].name), ("dress", "all"))
        show = hs.parse("dress head show" + chr(10))[0]   # row 197: the equip, then the game's head show call
        self.assertEqual((show.kind, show.name, show.start), ("dress", "head", "show"))
        self.assertEqual(steps[17].name, "Enemy_DungeonBoss_Gohma")
        self.assertIn("hit", hs.MODULE_ONLY)   # module steps, as coop_testinput.cpp reads them
        self.assertIn("stun", hs.MODULE_ONLY)

    def test_menu_step_goes_to_the_module(self):
        steps = hs.parse("menu Customize\nmenu close\n")
        self.assertEqual([(s.kind, s.name) for s in steps], [("menu", "customize"), ("menu", "close")])
        self.assertIn("menu", hs.MODULE_ONLY)

    def test_place_on_a_platform_goes_to_the_module(self):  # #196: a rider on a floor that waves
        s = hs.parse("place Obj_Deku_MoveFloor_A_01 0.3\n")[0]
        self.assertEqual((s.kind, s.name, s.y), ("place", "Obj_Deku_MoveFloor_A_01", 0.3))
        self.assertEqual(s.text.strip(), "place Obj_Deku_MoveFloor_A_01 0.3")
        for bad in ("place Obj_Deku_MoveFloor_A_01", "place Obj_Deku_MoveFloor_A_01 up", "place 1 2"):
            with self.assertRaises(hs.ScriptError) as e:
                hs.parse(bad)
            self.assertIn("want: place X Y Z, or place PLATFORM UP", str(e.exception))

    def test_face_and_aim_go_to_the_module(self):  # rows 175, 173: a turn on the spot, an arrow put on its way
        face, aim = hs.parse("face 66.96 -0.5\naim -56.5 131.5 -10.1\n")
        self.assertEqual((face.kind, face.x, face.z), ("face", 66.96, -0.5))
        self.assertEqual((aim.kind, aim.x, aim.y, aim.z), ("aim", -56.5, 131.5, -10.1))
        self.assertEqual(aim.text.strip(), "aim -56.5 131.5 -10.1")
        self.assertIn("face", hs.MODULE_ONLY)
        self.assertIn("aim", hs.MODULE_ONLY)
        for bad, want in (("face 1", "want: face X Z"), ("face 1 2 3", "want: face X Z"), ("face east 0", "want: face X Z"),
                          ("aim 1 2", "want: aim X Y Z"), ("aim 1 2 3 4", "want: aim X Y Z"), ("aim eye 2 3", "want: aim X Y Z")):
            with self.assertRaises(hs.ScriptError) as e:
                hs.parse(bad)
            self.assertIn(want, str(e.exception))

    def test_key_names(self):
        s = hs.parse("hold DUP+LCLICK+RCLICK+MINUS+RSTICK_LEFT 5")[0]
        self.assertEqual(s.buttons, ["UP", "LS", "RS", "MINUS"])
        self.assertEqual(s.right, (-1, 0))

    def test_errors(self):
        for text, want in (("wait", "line 1: want: wait TICKS"), ("\n\nhold Q 3", "line 3: unknown key Q"),
                           ("stick 2 0 5", "line 1: want: STICK"), ("goto 1 2 0.1", "radius at least 0.2"),
                           ("jump", "unknown command jump"), ("# only", "no steps"),
                           ("wear cape X", "want: wear"), ("dress hat", "want: dress"), ("dress shield show", "want: dress"), ("dress head show x", "want: dress"), ("menu", "want: menu customize|close"),
                           ("menu map", "want: menu customize|close"), ("hurt", "want: hurt QUARTER_HEARTS"),
                           ("hurt 0", "want: hurt QUARTER_HEARTS"), ("hurt 81", "want: hurt QUARTER_HEARTS"),
                           ("hurt 2x", "want: hurt QUARTER_HEARTS"), ("hit Enemy_DungeonBoss_Gohma", "want: hit ACTOR N"),
                           ("hit Enemy_DungeonBoss_Gohma 0", "want: hit ACTOR N"),
                           ("hit Enemy_DungeonBoss_Gohma 81", "want: hit ACTOR N"),
                           ("hit Enemy_DungeonBoss_Gohma 2x", "want: hit ACTOR N"), ("stun", "want: stun BOSS"),
                           ("stun A B", "want: stun BOSS")):
            with self.assertRaises(hs.ScriptError) as e:
                hs.parse(text)
            self.assertIn(want, str(e.exception))

    def test_repo_scenarios_parse(self):
        files = [f for f in glob.glob(os.path.join(HERE, "..", "scenarios", "*.txt")) if not f.endswith(".steps.txt")]
        self.assertGreater(len(files), 100)
        for f in files:
            hs.parse(hs.read_script(f))


    def test_include_expands_beside_the_file(self):
        d = tempfile.mkdtemp()
        try:
            with open(os.path.join(d, "a.txt"), "w", encoding="utf-8") as f:
                f.write("wait 10\ninclude b.inc.txt   # shared\nmark end\n")
            with open(os.path.join(d, "b.inc.txt"), "w", encoding="utf-8") as f:
                f.write("# the shared step\nkill Enemy_X\nwait 5\n")
            steps = hs.parse(hs.read_script(os.path.join(d, "a.txt")))
            self.assertEqual([s.text for s in steps if s.text], ["wait 10", "kill Enemy_X", "wait 5", "mark end"])
            for bad, want in (("include\n", "want: include FILE"), ("include nope.txt\n", "no nope.txt"),
                              ("include c.txt\n", "nest deeper")):
                with open(os.path.join(d, "c.txt"), "w", encoding="utf-8") as f:
                    f.write(bad)
                with self.assertRaises(hs.ScriptError) as e:
                    hs.read_script(os.path.join(d, "c.txt"))
                self.assertIn(want, str(e.exception))
        finally:
            shutil.rmtree(d)

    def test_include_is_no_step(self):
        with self.assertRaises(hs.ScriptError):
            hs.parse("include b.inc.txt")


class BridgeTest(unittest.TestCase):
    def test_replies_by_id_out_of_order(self):
        a, b = socket.socketpair()
        bridge = hs.Bridge(a)
        got = []

        def game():
            buf = b""
            while buf.count(b"\n") < 2:
                buf += b.recv(4096)
            reqs = [json.loads(x) for x in buf.split(b"\n") if x]
            got.extend(reqs)
            b.sendall(b'{"event":"present","frame":3}\n')
            for r in reversed(reqs):
                b.sendall((json.dumps({"id": r["id"], "ok": True, "frame": 9, "echo": r["cmd"]}) + "\n").encode())
        t = threading.Thread(target=game)
        t.start()
        out = {}
        ts = [threading.Thread(target=lambda c=c: out.__setitem__(c, bridge.request(c, reply_timeout=5)))
              for c in ("status", "saves")]
        for x in ts:
            x.start()
        for x in ts:
            x.join()
        t.join()
        self.assertEqual(out["status"]["echo"], "status")
        self.assertEqual(out["saves"]["echo"], "saves")
        self.assertEqual(len({r["id"] for r in got}), 2)
        bridge.close()
        b.close()

    def test_error_and_close(self):
        game = FakeGame()
        bridge = game.connect()
        with self.assertRaises(hs.BridgeError) as e:
            bridge.request("nonsense")
        self.assertEqual(e.exception.code, "unknown_cmd")
        self.assertFalse(bridge.request("nonsense", check=False)["ok"])
        game.conns[0].close()
        with self.assertRaises(ConnectionError):
            for _ in range(50):   # (until the reader saw the close)
                bridge.request("status", reply_timeout=1)
                game.clock.sleep(0)
        game.close()

    def test_teleport_timeout_is_an_argument(self):
        game = FakeGame()
        bridge = game.connect()
        bridge.request("teleport", reply_timeout=5, to="KokiriForest_Child", timeout=2)
        self.assertEqual(game.cmds("teleport")[0]["timeout"], 2)
        game.close()


@unittest.skipUnless(os.name == "nt", "named pipes are Windows only")
class PipeTest(unittest.TestCase):
    def test_write_while_a_reply_waits(self):
        """A request sent while another waits for its reply (the reader is in the pipe) still goes out at once."""
        import _winapi
        name = "coop-autotest-test-%d" % os.getpid()
        h = _winapi.CreateNamedPipe(hs.pipe_name(name), _winapi.PIPE_ACCESS_DUPLEX,
                                    _winapi.PIPE_WAIT,   # (byte mode: PIPE_TYPE_BYTE and PIPE_READMODE_BYTE are 0)
                                    1, 65536, 65536, 0, _winapi.NULL)

        def game():
            _winapi.ConnectNamedPipe(h, False)
            buf, reqs = b"", []
            while len(reqs) < 2:
                data, _ = _winapi.ReadFile(h, 65536)
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    reqs.append(json.loads(line))
            for r in reversed(reqs):   # the second first: the first was held back until the second came
                _winapi.WriteFile(h, (json.dumps({"id": r["id"], "ok": True, "cmd": r["cmd"]}) + "\n").encode())
        t = threading.Thread(target=game, daemon=True)
        t.start()
        bridge = hs.Bridge.connect(name, timeout=5)
        got = {}
        w = threading.Thread(target=lambda: got.__setitem__("wait", bridge.request("wait", reply_timeout=5)))
        w.start()
        got["status"] = bridge.request("status", reply_timeout=5)
        w.join(5)
        self.assertEqual((got["status"]["cmd"], got["wait"]["cmd"]), ("status", "wait"))
        bridge.close()
        t.join(5)
        _winapi.CloseHandle(h)


class PlayerTest(unittest.TestCase):
    def test_timed_inputs_and_marks(self):
        game, log, shots, _, end = play("""
            mark start
            stick 0 0.5 90
            hold LSTICK_UP+A 60 halfway
            wait 30
            press B
            rstick 1 0 12 ZL
        """)
        sent = [r for _, r in game.got if r["cmd"] in ("press", "stick", "release")]
        self.assertEqual([r["cmd"] for r in sent],
                         ["stick", "release", "stick", "release", "press", "release", "stick", "release"])
        self.assertEqual((sent[0]["x"], sent[0]["y"], sent[0]["stick"]), (0.0, 0.5, "L"))
        self.assertEqual((sent[2]["y"], sent[2]["buttons"]), (1, ["A"]))
        self.assertEqual(sent[4]["buttons"], ["B"])
        self.assertEqual((sent[6]["stick"], sent[6]["buttons"]), ("R", ["ZL"]))
        for r in sent:
            if r["cmd"] != "release":
                self.assertIs(r["wait"], False)
        # Real time: each input let go when its ticks are up (1 tick = 1/60 s), the script's total.
        t = {id(r): at for at, r in game.got}
        rel = [t[id(r)] for r in sent if r["cmd"] == "release"]
        start = game.got[0][0]
        self.assertAlmostEqual(rel[0] - start, 90 / 60, places=2)
        self.assertAlmostEqual(rel[1] - start, 150 / 60, places=2)
        self.assertAlmostEqual(rel[2] - start, (150 + 30 + 6) / 60, places=2)
        self.assertAlmostEqual(end - start, (150 + 30 + 12 + 12) / 60, places=2)
        self.assertEqual(shots, ["start", "halfway"])
        self.assertIn("coop: test mark halfway", log.lines)
        self.assertEqual(log.lines[-1], "coop: test script done")
        # The halfway mark came while A was still held.
        marks_at = [i for i, line in enumerate(log.lines) if line == "coop: test mark halfway"]
        self.assertTrue(marks_at)

    def test_goto_learns_the_camera(self):
        for camera in (0.0, 2.0, -2.6):
            game = FakeGame(camera=camera)
            game.pos = [10.0, 0.0, 5.0]
            _, log, _, p, _ = play("goto -4 -6 1 900", game)
            self.assertTrue(any(line.startswith("coop: test goto reached") for line in log.lines), (camera, log.lines))
            self.assertLess(math.hypot(game.pos[0] + 4, game.pos[2] + 6), 1.2)
            self.assertEqual(game.held, [])   # let go at the end

    def test_goto_gives_up(self):
        game = FakeGame()
        game.speed = 0.0
        _, log, _, _, _ = play("goto 50 50 1 120", game)
        self.assertIn("coop: test goto timed out", log.lines)
        self.assertEqual(game.held, [])

    def test_toward_pushes_for_its_ticks(self):
        game = FakeGame()
        _, _, _, _, end = play("toward 0 -100 60", game)
        self.assertGreater(-game.pos[2], 2.5)
        self.assertLess(abs(game.pos[0]), 0.5)

    def test_a_stopped_toward_keeps_pushing_straight(self):
        # #147 (big-run-0115 deku-block): Link against a block stands still until the push starts; a step
        # aside slid him off its face. A stopped toward holds one direction for all its ticks.
        game = FakeGame()
        game.speed = 0.0
        play("toward 0 -100 180", game)
        sticks = {(r["x"], r["y"]) for r in game.cmds("stick")}
        self.assertEqual(sticks, {(0.0, 1.0)})

    def test_toward_with_keys_holds_them_for_the_whole_step(self):
        # #147 (deku-block-147-probe): `toward X Z TICKS A` is a grab held while Link pushes or pulls. The stick is
        # let go and sent again every steer_every; the keys go once on a connection of their own, so that release
        # never lets go of the grab. Before, A was dropped and pressed again 4 times a second: B's pull never
        # happened (Link let go of the block and walked 3 m back).
        game = FakeGame()
        a_held_at = []
        answer = game.answer

        def watch(req, conn=None):
            reply = answer(req, conn)
            if req.get("cmd") == "release":
                a_held_at.append(any("A" in r.get("buttons", []) for r in game.held))
            return reply

        game.answer = watch
        play("toward 0 100 120 A", game)
        presses = game.cmds("press")
        self.assertEqual([r["buttons"] for r in presses], [["A"]])          # pressed once
        self.assertGreaterEqual(presses[0]["frames"], 120)                   # for the whole step
        self.assertTrue(all(not r.get("buttons") for r in game.cmds("stick")))  # the sticks carry no keys
        self.assertGreater(len(a_held_at), 3)
        self.assertTrue(all(a_held_at[:-1]), a_held_at)   # A still down at every stick release in the step
        self.assertEqual(game.held, [])                   # and let go at its end
        self.assertGreater(game.pos[2], 2.5)              # Link moved the whole step (a pull toward +z)

    def test_toward_without_a_second_connection_keeps_keys_on_the_stick(self):
        game = FakeGame()
        log = hs.Log()
        b = game.connect()
        p = hs.Player(b, log, clock=game.clock, sleep=game.clock.sleep)
        p.play(hs.parse("toward 0 -100 30 A"))
        game.close()
        self.assertEqual(game.cmds("press"), [])
        self.assertTrue(all(r["buttons"] == ["A"] for r in game.cmds("stick")))
        self.assertEqual(game.held, [])

    def test_a_stopped_goto_still_steps_aside(self):
        game = FakeGame()
        game.speed = 0.0
        play("goto 0 -100 1 180", game)
        self.assertGreater(len({(r["x"], r["y"]) for r in game.cmds("stick")}), 1)

    def test_goto_without_memory_is_skipped(self):
        game, log, _, _, _ = play("goto 1 1\nwait 6", FakeGame(mem=False))
        self.assertTrue(any("HOS_CONTROL_MEM=ro" in line for line in log.lines))
        self.assertTrue(any("goto skipped" in line for line in log.lines))
        self.assertEqual(log.lines[-1], "coop: test script done")

    def test_warp_retries_until_sent(self):
        game, log, _, _, _ = play("warp KakarikoVillage_Child Kakariko_WellSide\nwait 6", FakeGame(refuse_teleports=2))
        self.assertEqual(len(game.cmds("teleport")), 3)
        self.assertEqual(game.cmds("teleport")[-1]["to"], "KakarikoVillage_Child")
        self.assertTrue(any(line.startswith("coop: test warp KakarikoVillage_Child: sent") for line in log.lines))
        self.assertTrue(any("start Kakariko_WellSide not used" in line for line in log.lines))
        self.assertIn("coop: test warp done (map KakarikoVillage_Child)", log.lines)

    def test_warp_asks_again_when_the_map_does_not_come(self):
        """Sent is not moved: a teleport the game drops (asked during the load's arrival event) is asked again."""
        game, log, _, _, _ = play("warp KakarikoVillage_Child\nwait 6", FakeGame(drop_teleports=1))
        self.assertEqual(len(game.cmds("teleport")), 2)
        self.assertEqual(game.map, "KakarikoVillage_Child")
        self.assertTrue(any("not accepted" in line for line in log.lines))
        self.assertTrue(any("map KakarikoVillage_Child not reached, asking again" in line for line in log.lines))
        self.assertEqual(sum(line.startswith("coop: test warp done") for line in log.lines), 1)

    def test_warp_gives_up_when_the_map_never_comes(self):
        game, log, _, _, _ = play("warp KakarikoVillage_Child\nwait 6", FakeGame(drop_teleports=99))
        self.assertTrue(any("gave up" in line for line in log.lines))
        self.assertFalse(any(line.startswith("coop: test warp done") for line in log.lines))

    def test_warp_to_a_map_the_bridge_lacks_goes_to_the_module(self):
        """big-run-0115: `warp BarinadeStage BarinadeStage` (the boss rush) is not in the bridge's list (not_found):
        the line goes to the module's own warp, as written (its start too), and the bridge is not asked again."""
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "commands-A.txt")
            open(path, "w").close()
            game = FakeGame(maps={"InsideTheDekuTree"})
            taken = []
            sleep = game.clock.sleep

            def module_sleep(sec):
                sleep(sec)
                if os.path.getsize(path):
                    with open(path, encoding="utf-8") as f:
                        taken.append(f.read())
                    open(path, "w").close()
            game.clock.sleep = module_sleep
            game, log, _, _, _ = play("warp BarinadeStage BarinadeStage\nwait 6", game, commands=path)
            self.assertEqual(len(game.cmds("teleport")), 1)
            self.assertEqual([t.splitlines()[1:] for t in taken], [["warp BarinadeStage BarinadeStage"]])
            self.assertTrue(any("not one of the bridge's maps (not_found): asked through the module's own warp"
                                in line and "start BarinadeStage" in line for line in log.lines))
            self.assertFalse(any("not used on hookshot" in line for line in log.lines))
            self.assertFalse(any("gave up after" in line for line in log.lines))

    def test_warp_to_a_map_the_bridge_lacks_without_the_module(self):
        game, log, _, _, _ = play("warp GohmaStage GohmaStage\nwait 6", FakeGame(maps={"InsideTheDekuTree"}))
        self.assertEqual(len(game.cmds("teleport")), 1)
        self.assertTrue(any("gave up: the bridge has no such map" in line for line in log.lines))

    def test_warp_to_a_bridge_map_still_uses_the_bridge(self):
        game, log, _, _, _ = play("warp InsideTheDekuTree Entrance\nwait 6", FakeGame(maps={"InsideTheDekuTree"}))
        self.assertEqual(game.map, "InsideTheDekuTree")
        self.assertIn("coop: test warp done (map InsideTheDekuTree)", log.lines)
        self.assertEqual(sum("start Entrance not used" in line for line in log.lines), 1)

    def test_module_steps_are_skipped_without_the_file(self):
        game, log, _, _, _ = play("give bombs\nchat /teleport B\nplace 1 2 3\nmark m")
        self.assertEqual(sum("skipped on hookshot" in line for line in log.lines), 3)
        self.assertEqual(game.cmds("press", "stick"), [])
        self.assertEqual(log.lines[-2:], ["coop: test mark m", "coop: test script done"])

    def test_module_steps_go_to_the_commands_file(self):
        """Module steps in a row are one batch, written as the script has them; the next waits until it is taken."""
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "commands-A.txt")
            open(path, "w").close()
            game = FakeGame()
            taken = []
            sleep = game.clock.sleep

            def module_sleep(sec):   # the module: reads the file on its check, plays it, empties it
                sleep(sec)
                if os.path.getsize(path):
                    with open(path, encoding="utf-8") as f:
                        taken.append(f.read())
                    open(path, "w").close()
            game.clock.sleep = module_sleep
            game, log, _, _, _ = play("""
                give bombs
                chat /teleport B   # to B
                place 1 2.5 -3
                press A
                setflag PouchData.Item_Bomb.StockNum 20
                wear head Mask_Keaton
                assign Bow
                setenum 0x1234abcd Done
                kill Enemy_Skulltula_Gold
                hurt 4
                mark m
            """, game, commands=path)
            self.assertEqual(len(taken), 3)   # the mark too: the module logs it in the game's log
            self.assertEqual(taken[0].splitlines()[1:], ["give bombs", "chat /teleport B", "place 1 2.5 -3"])
            self.assertEqual(taken[1].splitlines()[1:], [
                "setflag PouchData.Item_Bomb.StockNum 20", "wear head Mask_Keaton", "assign Bow",
                "setenum 0x1234abcd Done", "kill Enemy_Skulltula_Gold", "hurt 4"])
            self.assertEqual(taken[2].splitlines()[1:], ["mark m"])
            self.assertTrue(all(t.startswith("# ") for t in taken))
            self.assertEqual(sum(" sent (line" in line for line in log.lines), 9)
            self.assertFalse(any("skipped" in line for line in log.lines))
            self.assertEqual(len(game.cmds("press")), 1)
            self.assertEqual(log.lines[-2:], ["coop: test mark m", "coop: test script done"])
            self.assertEqual(os.listdir(tmp), ["commands-A.txt"])   # no temporary files left

    def test_module_steps_not_taken(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "commands-A.txt")
            game, log, _, _, _ = play("give bombs\nwait 6", commands=path, take_for=1.0)
            self.assertTrue(any("give NOT taken" in line for line in log.lines))
            with open(path) as f:
                self.assertIn("give bombs", f.read())
            self.assertEqual(log.lines[-1], "coop: test script done")

    def test_module_lines_read_back_the_same(self):
        """The module gets each step's line as written: read alone, it is the same step."""
        n = 0
        for f in glob.glob(os.path.join(HERE, "..", "scenarios", "*.txt")):
            if f.endswith(".steps.txt"):
                continue
            steps = hs.parse(hs.read_script(f))
            for s in steps:
                if s.kind in hs.MODULE_ONLY:
                    again = hs.parse(s.text)
                    self.assertEqual(len(again), 1, s.text)
                    self.assertEqual((again[0].kind, again[0].name, again[0].start, again[0].x, again[0].y,
                                      again[0].z), (s.kind, s.name, s.start, s.x, s.y, s.z))
                    n += 1
        self.assertGreater(n, 0)


class LoadTest(unittest.TestCase):
    def test_presses_a_until_play(self):
        game = FakeGame()
        b = game.connect()
        log = hs.Log()
        s = hs.to_play(b, log, 120, clock=game.clock, sleep=game.clock.sleep)
        self.assertEqual(s["map"], "KokiriForest_Child")
        self.assertEqual(len([r for r in game.cmds("press") if r["buttons"] == ["A"]]), 3)
        self.assertIn("coop: test loaded (map KokiriForest_Child)", log.lines)
        game.close()

    def test_load_times_out(self):
        game = FakeGame()
        game.presses_to_play = 10 ** 6
        with self.assertRaises(TimeoutError):
            hs.to_play(game.connect(), hs.Log(), 30, clock=game.clock, sleep=game.clock.sleep)
        game.close()


class RendererWaitTest(unittest.TestCase):
    READY = "[hos host] vk: precompile: 18170 programs, 25346 graphics and 1132 compute pipelines, 0 mesh fallback programs in 172544 ms (8092 ms of it the caches opened)\n"

    def test_waits_for_the_precompile_line(self):
        c, reads = Clock(), []

        def log():
            reads.append(c())
            return "start\n" + (self.READY if len(reads) > 4 else "[hos host] vk: precompile: compute programs: those listed\n")
        waited = hs.wait_renderer(log, lambda: True, 60, clock=c, sleep=c.sleep)
        self.assertEqual(len(reads), 5)
        self.assertAlmostEqual(waited, 2.0, places=2)

    def test_rc4_start_up_line_is_ready_too(self):
        log = ("[hos host] vk: precompile: load at create (RX_VK_LOAD=create): 32623 noted pipelines found, none built\n"
               "[hos host] vk: start-up: the shaders took 6.5 s\n")
        self.assertEqual(hs.wait_renderer(lambda: log, lambda: True, 60, clock=Clock(), sleep=Clock().sleep), 0.0)
        self.assertFalse(hs.RENDERER_READY.search(log.splitlines()[0]))

    def test_times_out_and_notices_a_closed_game(self):
        c = Clock()
        with self.assertRaises(TimeoutError):
            hs.wait_renderer(lambda: "", lambda: True, 30, clock=c, sleep=c.sleep)
        with self.assertRaises(RuntimeError):
            hs.wait_renderer(lambda: "", lambda: False, 30, clock=c, sleep=c.sleep)

    def test_zero_is_no_wait(self):
        self.assertEqual(hs.wait_renderer(lambda: "", lambda: False, 0), 0.0)

    @staticmethod
    def compiling(c, t0, rate, stop_at=None, ready_at=None):
        """A cold compile's log (Longshot rc.7): a progress pair every 10 s, `rate` pipelines per 10 s, stuck from
        stop_at s, ready at ready_at s."""
        def log():
            t = c() - t0
            text = "[hos host] vk: precompile: compute programs: those listed (2256 keys listed)\n"
            for i in range(1, int(t // 10) + 1):
                n = rate * min(i, (stop_at // 10) if stop_at is not None else i)
                text += ("[hos host] vk: start-up: pipelines %d of 36753, 2%% of all\n"
                         "[hos host] vk: noted pipelines: %d of 32625 pipelines in %d s, about 500 s left\n"
                         % (n, n + 20, i * 10))
            if ready_at is not None and t >= ready_at:
                text += "[hos host] vk: start-up: the shaders took %d s\n" % ready_at
            return text
        return log

    def test_a_cold_compile_longer_than_the_wait_passes_while_it_moves(self):
        # big-run-0114: B always compiles cold (~440 s on rc.7), longer with several at once; the 600 s wait failed them
        c = Clock()
        waited = hs.wait_renderer(self.compiling(c, c(), 80, ready_at=900), lambda: True, 600, clock=c, sleep=c.sleep)
        self.assertAlmostEqual(waited, 900, delta=1)

    def test_a_stuck_compile_fails_after_the_stall(self):
        c = Clock()
        with self.assertRaises(TimeoutError) as e:
            hs.wait_renderer(self.compiling(c, c(), 80, stop_at=700), lambda: True, 600, stall=180, clock=c,
                             sleep=c.sleep)
        self.assertIn("no compile progress for 180 s", str(e.exception))
        self.assertIn("noted pipelines: 5620 of 32625", str(e.exception))   # the newest line, named
        self.assertAlmostEqual(c() - 1000, 700 + 180, delta=2)

    def test_a_stall_never_ends_the_wait_before_its_timeout(self):
        c = Clock()
        with self.assertRaises(TimeoutError):
            hs.wait_renderer(self.compiling(c, c(), 80, stop_at=20), lambda: True, 600, stall=60, clock=c,
                             sleep=c.sleep)
        self.assertAlmostEqual(c() - 1000, 600, delta=2)

    def test_the_limit_ends_even_a_moving_compile(self):
        c = Clock()
        with self.assertRaises(TimeoutError) as e:
            hs.wait_renderer(self.compiling(c, c(), 5), lambda: True, 600, stall=180, limit=1500, clock=c,
                             sleep=c.sleep)
        self.assertIn("not started in 1500 s", str(e.exception))
        self.assertAlmostEqual(c() - 1000, 1500, delta=2)

    def test_no_progress_lines_wait_the_timeout_as_before(self):
        c = Clock()
        with self.assertRaises(TimeoutError) as e:
            hs.wait_renderer(lambda: "booting\n", lambda: True, 600, clock=c, sleep=c.sleep)
        self.assertIn("no compile progress logged", str(e.exception))
        self.assertAlmostEqual(c() - 1000, 600, delta=2)

    def test_progress_is_the_newest_count_of_each_kind(self):
        text = ("vk: start-up: pipelines 743 of 36753, 2% of all\nvk: noted pipelines: 768 of 32625 pipelines in 13 s\n"
                "vk: start-up: pipelines 894 of 36753, 2% of all\n")
        self.assertEqual(hs.renderer_progress(text), (894 + 768, "vk: start-up: pipelines 894 of 36753"))
        self.assertEqual(hs.renderer_progress("nothing"), (0, ""))

    def test_run_options_have_the_progress_defaults(self):
        import argparse
        p = argparse.ArgumentParser()
        hs.add_renderer_options(p)
        a = p.parse_args([])
        self.assertEqual((a.renderer_wait, a.renderer_stall, a.renderer_limit), (600, hs.RENDERER_STALL,
                                                                                 hs.RENDERER_LIMIT))


class SaveTest(unittest.TestCase):
    def test_both_baseline_layouts(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            flat = os.path.join(tmp, "flat")
            os.makedirs(os.path.join(flat, "autotest-save", "slot_03"))
            open(os.path.join(flat, "autotest-save", "account.sav"), "w").close()
            os.makedirs(os.path.join(flat, "user", "save", hs.SAVE_USER, "slot_09"))
            self.assertTrue(hs.restore_save(flat))
            user = os.path.join(flat, "user", "save", hs.SAVE_USER)
            self.assertEqual(sorted(os.listdir(user)), ["account.sav", "slot_03"])
            tree = os.path.join(tmp, "tree")
            os.makedirs(os.path.join(tree, "autotest-save", hs.SAVE_USER, "slot_01"))
            self.assertTrue(hs.restore_save(tree))
            self.assertTrue(os.path.isdir(os.path.join(tree, "user", "save", hs.SAVE_USER, "slot_01")))
            self.assertFalse(hs.restore_save(os.path.join(tmp, "none")))


class RunTest(unittest.TestCase):
    def test_two_games(self):
        """run: both games started with the bridge and memory on, loaded, both scripts played, shots of both, quit."""
        import tempfile

        class Proc:
            pid, returncode = 1, None

            def __init__(self, args, **kw):
                self.env = kw["env"]
                if "stdout" in kw:   # the game's log: its renderer is ready (run waits for this line)
                    kw["stdout"].write(RendererWaitTest.READY.encode())
                started.append(self)

            def poll(self):
                return None

            def wait(self, t=None):
                return 0

        started, fakes = [], {}
        with tempfile.TemporaryDirectory() as tmp:
            for x in "AB":
                os.makedirs(os.path.join(tmp, x, "autotest-save", "u"))
                open(os.path.join(tmp, x, "longshot.exe" if hs.WINDOWS else "longshot"), "w").close()
                fakes["coop-auto-" + x if hs.WINDOWS else os.path.join(tmp, "out", "control-%s.sock" % x)] = \
                    FakeGame(clock=Clock())
                fakes[list(fakes)[-1]].presses_to_play = 1
            with open(os.path.join(tmp, "a.txt"), "w") as f:
                f.write("mark one\nstick 0 1 6\ngive bombs\n")
            with open(os.path.join(tmp, "b.txt"), "w") as f:
                f.write("press A\n")
            popen, connect = hs.subprocess.Popen, hs.Bridge.connect
            hs.subprocess.Popen = Proc
            hs.Bridge.connect = classmethod(lambda cls, path, timeout=10.0: fakes[path].connect())
            try:
                code = hs.main(["run", "--game-a", os.path.join(tmp, "A"), "--game-b", os.path.join(tmp, "B"),
                                "--out", os.path.join(tmp, "out"), "--name", "t", "--script-a",
                                os.path.join(tmp, "a.txt"), "--script-b", os.path.join(tmp, "b.txt"),
                                "--every", "0", "--env", "B:ONLY_B=1", "--env", "BOTH=1"])
            finally:
                hs.subprocess.Popen, hs.Bridge.connect = popen, connect
            out = os.path.join(tmp, "out")
            with open(os.path.join(out, "steps.txt")) as f:
                self.assertEqual(code, 0, f.read())
            self.assertTrue(os.path.isdir(os.path.join(tmp, "A", "user", "save", "u")))   # baseline put back
            self.assertEqual([p.env["HOS_CONTROL_MEM"] for p in started], ["ro", "ro"])
            self.assertEqual([p.env.get("ONLY_B") for p in started], [None, "1"])
            self.assertEqual([p.env["BOTH"] for p in started], ["1", "1"])
            self.assertEqual([p.env["LICHII_COOP_TEST_OWN_SAVE"] for p in started], ["1", "1"])   # each game its own save
            ga, gb = fakes.values()
            for g in (ga, gb):
                shots = [os.path.basename(r["path"]) for r in g.cmds("shot")]
                self.assertIn("loaded-%s.png" % ("A" if g is ga else "B"), shots)
                self.assertIn("t-A-one-%s.png" % ("A" if g is ga else "B"), shots)   # A's mark: both games
                self.assertEqual(len(g.cmds("quit")), 1)
                g.close()
            self.assertEqual(ga.cmds("stick")[0]["y"], 1)
            self.assertEqual([r["buttons"] for r in gb.cmds("press")][-1], ["A"])
            with open(os.path.join(out, "player-A.txt")) as f:
                a_log = f.read()
            self.assertIn("coop: test give NOT taken", a_log)   # no module here to take it
            self.assertEqual([p.env["LICHII_COOP_TEST_COMMANDS"] for p in started],
                             [os.path.join(out, "commands-%s.txt" % x) for x in "AB"])
            self.assertIn("coop: test script done", a_log)


class FindLogTest(unittest.TestCase):
    def test_rc3_and_rc4_places_newest_wins(self):
        import tempfile
        with tempfile.TemporaryDirectory() as game:
            self.assertIsNone(hs.find_log(game))
            rc3 = os.path.join(game, "longshot_log.txt")
            with open(rc3, "w") as f:
                f.write("rc3")
            self.assertEqual(hs.find_log(game), rc3)
            os.makedirs(os.path.join(game, "logs"))
            rc4 = os.path.join(game, "logs", "longshot_log.txt")
            with open(rc4, "w") as f:
                f.write("rc4")
            os.utime(rc3, (1000, 1000))  # an rc.3 log left from an older build
            self.assertEqual(hs.find_log(game), rc4)
            os.utime(rc4, (500, 500))
            self.assertEqual(hs.find_log(game), rc3)

    def test_bin_layout(self):
        import tempfile
        with tempfile.TemporaryDirectory() as game:
            os.makedirs(os.path.join(game, "bin", "logs"))
            open(os.path.join(game, "bin", "longshot.exe"), "w").close()
            log = os.path.join(game, "bin", "logs", "longshot_log.txt")
            with open(log, "w") as f:
                f.write("x")
            self.assertEqual(hs.find_log(game), log)
            self.assertIn(os.path.join(game, "logs", "longshot_log.txt"), hs.log_candidates(game))


class TestDelayTest(unittest.TestCase):
    """The server's TEST-ONLY delay (server/test_delay.hpp) from run.ps1 -Lag / -Jitter on Longshot."""

    def test_args(self):
        self.assertEqual(hs.test_delay_args(0, 0), [])
        self.assertEqual(hs.test_delay_args(250, 60), ["--test-delay-ms", "250", "--test-jitter-ms", "60"])
        self.assertEqual(hs.test_delay_args(0, 20), ["--test-delay-ms", "0", "--test-jitter-ms", "20"])

    def test_options(self):
        import argparse
        p = argparse.ArgumentParser()
        hs.add_test_delay_options(p)
        a = p.parse_args(["--test-delay-ms", "250", "--test-jitter-ms", "60"])
        self.assertEqual((a.test_delay_ms, a.test_jitter_ms), (250, 60))
        a = p.parse_args([])
        self.assertEqual((a.test_delay_ms, a.test_jitter_ms), (0, 0))


class NoFairyTest(unittest.TestCase):
    """A bottled fairy turns a lethal hit into the game's own death and revive, never down-but-out (coop_dbno), so
    scenarios that hurt a player empty the bottles first (3 VOID runs on 2026-11-20: the lane saves have a fairy)."""

    def test_expands_to_every_bottle_set_empty(self):
        steps = hs.parse("mark start\nnofairy\nwait 10\n")
        sets = [s for s in steps if s.kind == "setenum"]
        self.assertEqual(len(sets), 5)
        self.assertEqual([s.name for s in sets], list(hs.BOTTLE_ITEMS))
        self.assertTrue(all(s.start == "Empty" and s.text == "setenum %s Empty" % s.name for s in sets))
        self.assertTrue(all(s.line == 2 for s in sets))
        self.assertIn("setenum", hs.MODULE_ONLY)  # written to the module's commands file as each step's text
        with self.assertRaises(hs.ScriptError):
            hs.parse("nofairy now\n")

    def test_bottle_hashes_are_the_hosts(self):
        # coop/host/include/coop/host/game_items.hpp letters::kBottles: {IsValid, BottleItem} per bottle.
        header = os.path.join(HERE, "..", "..", "..", "coop", "host", "include", "coop", "host", "game_items.hpp")
        with open(header, encoding="utf-8") as f:
            text = f.read()
        block = text[text.index("kBottles{{"):]
        block = block[:block.index("}};")]
        items = [pair.split(",")[1].strip().rstrip("}") for pair in block.split("{")[3:]]
        self.assertEqual(tuple(i.lower() for i in items), hs.BOTTLE_ITEMS)

    def test_every_scenario_that_hurts_empties_the_bottles_first(self):
        for f in sorted(glob.glob(os.path.join(HERE, "..", "scenarios", "*.txt"))):
            if f.endswith(".steps.txt"):
                continue
            if f.endswith(".inc.txt"):
                continue  # a shared part, checked inside the scenarios that include it
            text = hs.read_script(f)  # with its `include` lines in place
            if "BottleItem Fairy" in text:
                continue  # puts a fairy in on purpose (hs-fairy-revive)
            steps = hs.parse(text)
            hurts = [i for i, s in enumerate(steps) if s.kind == "hurt"]
            if not hurts:
                continue
            emptied = {s.name for s in steps[:hurts[0]] if s.kind == "setenum" and s.start == "Empty"}
            with self.subTest(scenario=os.path.basename(f)):
                self.assertTrue(set(hs.BOTTLE_ITEMS) <= emptied, "add `nofairy` before the first hurt")


class DropTest(unittest.TestCase):
    """Row 178's fall guard in a game: `drop NAME METRES` (a boss of ours put METRES down, test only)."""

    def test_parses_and_goes_to_the_module(self):
        steps = hs.parse("drop Enemy_DungeonBoss_PhantomGanon 60\n")
        self.assertEqual((steps[0].kind, steps[0].name, steps[0].start), ("drop", "Enemy_DungeonBoss_PhantomGanon", "60"))
        self.assertEqual(steps[0].text, "drop Enemy_DungeonBoss_PhantomGanon 60")
        self.assertIn("drop", hs.MODULE_ONLY)
        for bad in ("drop\n", "drop X\n", "drop X 0\n", "drop X 501\n", "drop X 1.5\n"):
            with self.assertRaises(hs.ScriptError):
                hs.parse(bad)


if __name__ == "__main__":
    unittest.main()
