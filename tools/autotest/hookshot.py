"""Plays our autotest scripts in Hookshot games, through Hookshot's control bridge.

Hookshot (the static recomp, docs/hookshot.md) has a local control socket (its docs/control.md): a named pipe on
Windows, a Unix socket on Linux, newline-delimited JSON. This module is our client for it (standard library only):

- `Bridge`: one connection; requests matched to replies by id.
- `parse`: our test script format (lichii/hook/coop_testinput.h), the same files the emulator plays.
- `Player`: plays a parsed script into one game in real time (1 tick = 1/60 s, as the emulator's hook counts):
  wait, hold, press, stick and rstick as bridge inputs let go by `release` when their time is up; `mark` logged and
  shot; `warp` as the bridge's teleport (retried until the game takes it, as the hook does); `goto` and `toward`
  steered from Link's position read through guest memory (`HOS_CONTROL_MEM=ro`). Steps that need the co-op module
  inside the game (chat, give, wear, assign, place, kill, setflag, setenum) are written, as the script has them, to
  the game's LICHII_COOP_TEST_COMMANDS file: the module (lichii/hook/coop_testinput.cpp) plays them on its next
  check (4 a second) and empties the file, so the player knows they were taken. Without that file they are logged as
  skipped.
- `run`: the two-game test that tools/autotest/run.ps1 -Runtime hookshot starts: the server, both games with the bridge
  on, the save loaded (A pressed until the game plays), the scenario's A and B scripts at once, shots of both games
  at every mark and every few seconds, the logs, then everything closed.

Each player writes the lines the emulator's hook writes (`coop: test mark NAME`, `coop: test script done`, ...) to
its own log, so the runner reads both the same way.

    python -I tools/autotest/hookshot.py play --sock coop-auto-A --script walk.A.txt --log a.txt --shots out
    python -I tools/autotest/hookshot.py run --game-a <folder> --game-b <folder> --server coop_server.exe --out <dir>
                                             --script-a walk.A.txt [--script-b walk.B.txt] [--load load.txt]
"""
import argparse
import base64
import json
import math
import os
import queue
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

WINDOWS = os.name == "nt"
TICKS = 60.0              # script ticks a second
PRESS_TICKS = 6           # press KEYS: held this long, then let go as long
GOTO_TICKS = 600          # goto gives up after 10 s
MAIN_BASE = 0x80020000    # where Hookshot maps main (docs/hookshot.md)
GAMEUI_SLOT = 0x4e7cfa8   # main offset of the GameUI module pointer (integration docs/player-state-map.md)
POS_VALID = 0x58a         # GameUI +0x58a bit 0: the position below is Link's
POS = 0x590               # GameUI +0x590: Link's position, x y z floats

# Our key names (coop_testinput.h) to the bridge's (control.md, HOS_INPUT_SCRIPT's names).
KEYS = {
    "A": "A", "B": "B", "X": "X", "Y": "Y", "L": "L", "R": "R", "ZL": "ZL", "ZR": "ZR",
    "PLUS": "PLUS", "MINUS": "MINUS", "LCLICK": "LS", "RCLICK": "RS",
    "DUP": "UP", "DDOWN": "DOWN", "DLEFT": "LEFT", "DRIGHT": "RIGHT",
}
STICK_KEYS = {  # full stick directions: (stick, x, y)
    "LSTICK_UP": ("L", 0, 1), "LSTICK_DOWN": ("L", 0, -1), "LSTICK_LEFT": ("L", -1, 0), "LSTICK_RIGHT": ("L", 1, 0),
    "RSTICK_UP": ("R", 0, 1), "RSTICK_DOWN": ("R", 0, -1), "RSTICK_LEFT": ("R", -1, 0), "RSTICK_RIGHT": ("R", 1, 0),
}
WEAR_SLOTS = ("weapon", "shield", "head", "armor", "boots", "hand")
MODULE_ONLY = ("chat", "give", "wear", "assign", "place", "kill", "setflag", "setenum")


class ScriptError(Exception):
    pass


class BridgeError(Exception):
    def __init__(self, reply):
        super().__init__("%s: %s" % (reply.get("error"), reply.get("message", "")))
        self.reply = reply
        self.code = reply.get("error")


# ---------------------------------------------------------------------------------------------- the script format

class Step:
    def __init__(self, kind, line, ticks=0):
        self.kind = kind          # timed, mark, goto, toward, warp, or one of MODULE_ONLY
        self.line = line
        self.ticks = ticks
        self.buttons = []         # the bridge's names
        self.left = None          # (x, y), each -1..1
        self.right = None
        self.name = ""            # mark / warp scene / chat text / item ...
        self.start = ""
        self.x = self.y = self.z = 0.0
        self.radius = 1.0
        self.text = ""            # the line as written (no comment), for the module's commands file

    def __repr__(self):
        return "Step(%s line %d)" % (self.kind, self.line)


def _ticks(text):
    try:
        v = int(text)
    except ValueError:
        return None
    return v if v >= 0 else None


def _number(text):
    try:
        v = float(text)
    except ValueError:
        return None
    return v if math.isfinite(v) else None


def _axis(text):
    v = _number(text)
    return v if v is not None and -1.0 <= v <= 1.0 else None


def _keys(text, step):
    """KEYS joined with + into step.buttons / step.left / step.right. The bad key, or None."""
    for k in text.upper().split("+"):
        if k in KEYS:
            if KEYS[k] not in step.buttons:
                step.buttons.append(KEYS[k])
        elif k in STICK_KEYS:
            stick, x, y = STICK_KEYS[k]
            if stick == "L":
                step.left = (x, y)
            else:
                step.right = (x, y)
        else:
            return k
    return None


def parse(text):
    """Our test script (coop_testinput.h) as a list of Steps. Raises ScriptError("line N: ...")."""
    steps = []
    for number, raw in enumerate(text.split("\n"), 1):
        line = raw.split("#", 1)[0]
        w = line.split()
        if not w:
            continue
        cmd = w[0].upper()

        def fail(why):
            raise ScriptError("line %d: %s" % (number, why))

        s = Step("timed", number)
        s.text = line.strip()
        if cmd == "WAIT":
            if len(w) != 2 or _ticks(w[1]) is None:
                fail("want: wait TICKS")
            s.ticks = _ticks(w[1])
        elif cmd == "HOLD":
            if len(w) not in (3, 4) or _ticks(w[2]) is None:
                fail("want: hold KEYS TICKS [MARK]")
            s.ticks = _ticks(w[2])
            if len(w) == 4:
                s.name = w[3]
            bad = _keys(w[1], s)
            if bad:
                fail("unknown key " + bad)
        elif cmd == "PRESS":
            ticks = PRESS_TICKS
            if len(w) not in (2, 3) or (len(w) == 3 and _ticks(w[2]) is None):
                fail("want: press KEYS [TICKS]")
            if len(w) == 3:
                ticks = _ticks(w[2])
            bad = _keys(w[1], s)
            if bad:
                fail("unknown key " + bad)
            s.ticks = ticks
            steps.append(s)
            s = Step("timed", number, ticks)
        elif cmd in ("STICK", "RSTICK"):
            if len(w) not in (4, 5) or _axis(w[1]) is None or _axis(w[2]) is None or _ticks(w[3]) is None:
                fail("want: %s X Y TICKS [KEYS], X and Y from -1 to 1" % cmd)
            if len(w) == 5:
                bad = _keys(w[4], s)
                if bad:
                    fail("unknown key " + bad)
            if cmd == "STICK":
                s.left = (_axis(w[1]), _axis(w[2]))
            else:
                s.right = (_axis(w[1]), _axis(w[2]))
            s.ticks = _ticks(w[3])
        elif cmd == "GOTO":
            s.kind, s.ticks = "goto", GOTO_TICKS
            if len(w) < 3 or len(w) > 5 or _number(w[1]) is None or _number(w[2]) is None:
                fail("want: goto X Z [RADIUS] [TICKS], radius at least 0.2")
            s.x, s.z = _number(w[1]), _number(w[2])
            if len(w) >= 4:
                r = _number(w[3])
                if r is None or r < 0.2:
                    fail("want: goto X Z [RADIUS] [TICKS], radius at least 0.2")
                s.radius = r
            if len(w) == 5:
                if _ticks(w[4]) is None:
                    fail("want: goto X Z [RADIUS] [TICKS], radius at least 0.2")
                s.ticks = _ticks(w[4])
        elif cmd == "TOWARD":
            s.kind = "toward"
            if len(w) not in (4, 5) or _number(w[1]) is None or _number(w[2]) is None or _ticks(w[3]) is None:
                fail("want: toward X Z TICKS [KEYS]")
            s.x, s.z, s.ticks = _number(w[1]), _number(w[2]), _ticks(w[3])
            if len(w) == 5:
                bad = _keys(w[4], s)
                if bad:
                    fail("unknown key " + bad)
        elif cmd == "WARP":
            if len(w) not in (2, 3):
                fail("want: warp SCENE [START]")
            s.kind, s.name = "warp", w[1]
            s.start = w[2] if len(w) == 3 else ""
        elif cmd == "PLACE":
            if len(w) != 4 or any(_number(v) is None for v in w[1:]):
                fail("want: place X Y Z")
            s.kind = "place"
            s.x, s.y, s.z = (_number(v) for v in w[1:])
        elif cmd == "CHAT":
            if len(w) < 2:
                fail("want: chat TEXT")
            s.kind = "chat"
            s.name = line.strip()[len(w[0]):].strip()
        elif cmd in ("KILL", "GIVE"):
            if len(w) != 2:
                fail("want: %s %s" % (cmd.lower(), "ACTOR" if cmd == "KILL" else "ITEM"))
            s.kind, s.name = cmd.lower(), w[1]
        elif cmd == "SETFLAG":
            if len(w) != 3 or not w[2].lstrip("-").isdigit():
                fail("want: setflag NAME VALUE (an Int or Bool flag, a whole number)")
            s.kind, s.name, s.start = "setflag", w[1], w[2]
        elif cmd == "SETENUM":
            if len(w) != 3:
                fail("want: setenum NAME VALUE (an Enum flag, or 0xHASH, and a value's name)")
            s.kind, s.name, s.start = "setenum", w[1], w[2]
        elif cmd == "WEAR":
            if len(w) != 3 or w[1].lower() not in WEAR_SLOTS:
                fail("want: wear weapon|shield|head|armor|boots|hand NAME")
            s.kind, s.start, s.name = "wear", w[1], w[2]
        elif cmd == "ASSIGN":
            if len(w) != 2:
                fail("want: assign ITEM")
            s.kind, s.name = "assign", w[1]
        elif cmd == "MARK":
            if len(w) != 2:
                fail("want: mark NAME (one word)")
            s.kind, s.name = "mark", w[1]
        else:
            fail("unknown command " + w[0])
        steps.append(s)
    if not steps:
        raise ScriptError("no steps")
    return steps


# ---------------------------------------------------------------------------------------------- the bridge

def pipe_name(name):
    return name if name.startswith("\\\\.\\pipe\\") else "\\\\.\\pipe\\" + name


def open_transport(path, timeout=10.0):
    """A file-like object for the bridge at path: the named pipe on Windows, the Unix socket elsewhere."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            if WINDOWS:
                return open(pipe_name(path), "r+b", buffering=0)
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.2)


class Bridge:
    """One connection to a game's bridge. `transport` is a connected socket or an open pipe (binary, unbuffered).
    A reader thread hands each reply to the request with its id; events (subscribe) are dropped."""

    def __init__(self, transport):
        self.t = transport
        self.is_sock = isinstance(transport, socket.socket)
        self.lock = threading.Lock()
        self.waiting = {}
        self.next_id = 1
        self.closed = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    @classmethod
    def connect(cls, path, timeout=10.0):
        return cls(open_transport(path, timeout))

    def _pipe_waiting(self):
        """(Windows) bytes waiting in the pipe, -1 when it is broken. A blocking read on a pipe opened without overlapped
        I/O holds the handle, so a write from another thread would wait for it: the reader peeks first."""
        import ctypes
        import msvcrt
        from ctypes import wintypes
        n = wintypes.DWORD()
        h = wintypes.HANDLE(msvcrt.get_osfhandle(self.t.fileno()))
        if not ctypes.windll.kernel32.PeekNamedPipe(h, None, 0, None, ctypes.byref(n), None):
            return -1
        return n.value

    def _chunk(self):
        if self.is_sock:
            return self.t.recv(65536)
        while not self.closed:
            n = self._pipe_waiting()
            if n < 0:
                return b""
            if n:
                return self.t.read(min(n, 65536))
            time.sleep(0.005)
        return b""

    def _read(self):
        buf = b""
        try:
            while True:
                chunk = self._chunk()
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    try:
                        msg = json.loads(line)
                    except ValueError:
                        continue
                    if "ok" not in msg:
                        continue
                    with self.lock:
                        q = self.waiting.pop(msg.get("id"), None)
                    if q:
                        q.put(msg)
        except (OSError, ValueError):
            pass
        self.closed = True
        with self.lock:
            for q in self.waiting.values():
                q.put(None)
            self.waiting.clear()

    def request(self, cmd, reply_timeout=30.0, check=True, **args):
        """Sends one request and waits for its reply (a dict). check: an error reply raises BridgeError."""
        q = queue.Queue()
        with self.lock:
            if self.closed:
                raise ConnectionError("bridge closed")
            rid = self.next_id
            self.next_id += 1
            self.waiting[rid] = q
            data = (json.dumps(dict(args, id=rid, cmd=cmd), separators=(",", ":")) + "\n").encode()
            if self.is_sock:
                self.t.sendall(data)
            else:
                self.t.write(data)
        try:
            reply = q.get(timeout=reply_timeout)
        except queue.Empty:
            with self.lock:
                self.waiting.pop(rid, None)
            raise TimeoutError("%s: no reply in %.0f s" % (cmd, reply_timeout))
        if reply is None:
            raise ConnectionError("bridge closed during " + cmd)
        if check and not reply.get("ok"):
            raise BridgeError(reply)
        return reply

    def close(self):
        self.closed = True
        try:
            if self.is_sock:
                self.t.shutdown(socket.SHUT_RDWR)
            self.t.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------------------------- the player

class Log:
    """The player's log: lines like the emulator hook's, each with the local time."""

    def __init__(self, path=None, echo=None):
        self.path = path
        self.echo = echo
        self.lock = threading.Lock()
        self.lines = []

    def __call__(self, text):
        line = "%s %s" % (time.strftime("%H:%M:%S"), text)
        with self.lock:
            self.lines.append(text)
            if self.path:
                with open(self.path, "a", encoding="utf-8") as f:
                    f.write(line + "\n")
        if self.echo:
            self.echo(line)


def _wrap(a):
    while a > math.pi:
        a -= 2 * math.pi
    while a < -math.pi:
        a += 2 * math.pi
    return a


class Player:
    """Plays steps into one game. clock and sleep are for the tests."""

    def __init__(self, bridge, log, shot=None, connect=None, clock=time.monotonic, sleep=time.sleep,
                 main_base=MAIN_BASE, steer_every=0.1, warp_for=90.0, commands=None, take_for=5.0):
        self.b = bridge
        self.commands = commands      # the game's LICHII_COOP_TEST_COMMANDS file, or None: module steps skipped
        self.take_for = take_for      # seconds the module has to take them (it checks 4 times a second)
        self.sent = 0
        self.log = log
        self.shot = shot              # shot(name): shots at a mark
        self.connect = connect        # a second connection (the warp's own), or None: warps share this one
        self.clock = clock
        self.sleep = sleep
        self.main_base = main_base
        self.steer_every = steer_every
        self.warp_for = warp_for
        self.mem_ok = None            # guest memory reads answered (None: not tried)
        self.warps = []
        self.angle = 0.0              # camera model: world direction = stick direction turned by this
        self.learned = False

    # time
    def _until(self, t):
        while True:
            left = t - self.clock()
            if left <= 0:
                return
            self.sleep(min(left, 0.25))

    # inputs
    def _hold(self, buttons, left, right, ticks):
        """Starts the inputs (held well past ticks: let go by release)."""
        frames = max(1, min(216000, int(ticks * 4) + 60))
        sent = False
        if left is not None:
            self.b.request("stick", stick="L", x=left[0], y=left[1], buttons=buttons or [], frames=frames,
                           wait=False)
            sent = True
        if right is not None:
            self.b.request("stick", stick="R", x=right[0], y=right[1],
                           buttons=[] if sent else (buttons or []), frames=frames, wait=False)
            sent = True
        if not sent and buttons:
            self.b.request("press", buttons=buttons, frames=frames, wait=False)
            sent = True
        return sent

    def _release(self):
        self.b.request("release")

    def timed(self, s, t0):
        """A wait, hold, press half or stick: inputs from t0 for s.ticks, let go at the end."""
        end = t0 + s.ticks / TICKS
        held = self._hold(s.buttons, s.left, s.right, s.ticks) if s.ticks else False
        if s.name:   # hold KEYS TICKS MARK: the mark halfway, keys still down
            self._until(t0 + s.ticks / TICKS / 2)
            self.mark(s.name)
        self._until(end)
        if held:
            self._release()
        return end

    def mark(self, name):
        self.log("coop: test mark " + name)
        if self.shot:
            try:
                self.shot(name)
            except Exception as e:   # (a shot that fails does not stop the script)
                self.log("coop: test shot %s failed: %s" % (name, e))

    # Link's position
    def link_pos(self):
        """(x, y, z) of Link from guest memory, or None (no memory access, or not known yet)."""
        if self.mem_ok is False:
            return None
        try:
            r = self.b.request("mem_read", addr=self.main_base + GAMEUI_SLOT, size=8)
            ptr = struct.unpack("<Q", bytes.fromhex(r["data"]))[0]
            if not ptr:
                return None
            r = self.b.request("mem_read", addr=ptr + POS_VALID, size=POS + 12 - POS_VALID)
            data = bytes.fromhex(r["data"])
        except BridgeError as e:
            if e.code in ("forbidden", "unavailable"):
                self.mem_ok = False
                self.log("coop: test no guest memory (%s): start the game with HOS_CONTROL_MEM=ro" % e.code)
                return None
            if e.code == "out_of_range":
                return None
            raise
        self.mem_ok = True
        if not data[0] & 1:
            return None
        return struct.unpack("<3f", data[POS - POS_VALID:])

    def steer(self, s, t0):
        """goto (until within s.radius or s.ticks) or toward (for s.ticks). The stick is camera-relative: the camera
        model (an angle) is learnt from how Link moves for a push; a push that gets nowhere for a second steps aside."""
        end = t0 + s.ticks / TICKS
        if self.link_pos() is None and self.mem_ok is False:
            self.log("coop: test %s skipped (no position on hookshot without guest memory)" % s.kind)
            return t0
        last_pos, last_push, stuck_since, detour_until, detour = None, None, None, 0.0, 1
        held = False
        while True:
            now = self.clock()
            if now >= end:
                if s.kind == "goto":
                    self.log("coop: test goto timed out")
                break
            pos = self.link_pos()
            if pos is None:
                self._until(now + self.steer_every)
                continue
            dx, dz = s.x - pos[0], s.z - pos[2]
            dist = math.hypot(dx, dz)
            if s.kind == "goto" and dist <= s.radius:
                self.log("coop: test goto reached %.1f %.1f" % (pos[0], pos[2]))
                break
            if last_pos is not None and last_push is not None:
                mx, mz = pos[0] - last_pos[0], pos[2] - last_pos[2]
                moved = math.hypot(mx, mz)
                if moved > 0.05:
                    # world (x, -z) is the stick's frame turned by self.angle (stick up = away from the camera)
                    seen = _wrap(math.atan2(-mz, mx) - math.atan2(last_push[1], last_push[0]))
                    self.angle = seen if not self.learned else _wrap(self.angle + 0.5 * _wrap(seen - self.angle))
                    self.learned = True
                    stuck_since = None
                elif stuck_since is None:
                    stuck_since = now
                elif now - stuck_since >= 1.0 and now >= detour_until:
                    detour_until, detour, stuck_since = now + 0.5, -detour, None
            want = math.atan2(-dz, dx) - self.angle
            if now < detour_until:
                want += detour * math.pi / 2
            mag = 1.0 if s.kind == "toward" else max(0.45, min(1.0, dist / 3.0))
            push = (round(math.cos(want) * mag, 3), round(math.sin(want) * mag, 3))
            if held:
                self._release()
            self._hold(s.buttons, push, None, self.steer_every * TICKS * 2)
            held = True
            last_pos, last_push = pos, push
            self._until(min(end, now + self.steer_every))
        if held:
            self._release()
        return self.clock()

    # warps: the bridge's teleport, asked again until the game takes it (it refuses during a load or an event)
    def warp(self, s):
        if s.start:
            self.log("coop: test warp %s: start %s not used on hookshot (the map's own)" % (s.name, s.start))

        def ask():
            b = self.connect() if self.connect else self.b
            deadline = self.clock() + self.warp_for
            why = "no reply"
            try:
                while self.clock() < deadline:
                    try:
                        r = b.request("teleport", reply_timeout=15.0, to=s.name, timeout=2)
                    except BridgeError as e:
                        why = e.code
                        if e.code == "bad_request":
                            self.log("coop: test warp %s failed: %s" % (s.name, e))
                            return
                        self.sleep(0.5)
                        continue
                    if r.get("sent"):
                        self.log("coop: test warp %s: sent (map %s)" % (s.name, r.get("map")))
                        return
                    self.sleep(0.5)
                self.log("coop: test warp %s gave up after %.0f s (%s)" % (s.name, self.warp_for, why))
            finally:
                if b is not self.b:
                    b.close()

        t = threading.Thread(target=ask, daemon=True)
        t.start()
        self.warps.append(t)

    # the module's test commands: written to its file, taken when it is empty again
    def module(self, batch):
        if not self.commands:
            for s in batch:
                self.log("coop: test %s skipped on hookshot (line %d: needs the co-op module's test commands)"
                         % (s.kind, s.line))
            return
        deadline = self.clock() + self.take_for
        while self._waiting() and self.clock() < deadline:   # the last batch not taken yet
            self.sleep(0.05)
        self.sent += 1
        tmp = "%s.%d.tmp" % (self.commands, self.sent)
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write("# hookshot.py batch %d\n" % self.sent)
            for s in batch:
                f.write(s.text + "\n")
        os.replace(tmp, self.commands)
        deadline = self.clock() + self.take_for
        while self._waiting() and self.clock() < deadline:
            self.sleep(0.05)
        taken = not self._waiting()
        for s in batch:
            self.log("coop: test %s %s (line %d: %s)" % (s.kind, "sent" if taken else "NOT taken", s.line,
                                                         "the module's log says what it did" if taken else
                                                         "no module reading LICHII_COOP_TEST_COMMANDS?"))

    def _waiting(self):
        try:
            return os.path.getsize(self.commands) > 0
        except OSError:
            return False

    def play(self, steps):
        self.log("coop: test script %d steps" % len(steps))
        t = self.clock()
        i = 0
        while i < len(steps):
            s = steps[i]
            i += 1
            if s.kind in MODULE_ONLY:
                # Steps in a row go together: the emulator plays them in one frame.
                batch = [s]
                while i < len(steps) and steps[i].kind in MODULE_ONLY:
                    batch.append(steps[i])
                    i += 1
                self._until(t)
                self.module(batch)
                t = max(t, self.clock())
                continue
            if s.kind == "timed":
                t = self.timed(s, t)
            elif s.kind == "mark":
                self.mark(s.name)
            elif s.kind in ("goto", "toward"):
                t = max(t, self.steer(s, max(t, self.clock())))
            elif s.kind == "warp":
                self.warp(s)
        self.log("coop: test script done")
        return t


# ---------------------------------------------------------------------------------------------- the two-game run

def to_play(bridge, log, timeout, clock=time.monotonic, sleep=time.sleep):
    """Title and file select to the world: A pressed every 2 s until the bridge says play with a map known."""
    deadline = clock() + timeout
    while clock() < deadline:
        try:
            s = bridge.request("status")
        except BridgeError:
            s = {}
        game = s.get("game") or {}
        if s.get("state") == "play" and s.get("map") and not game.get("loading") and not game.get("event"):
            log("coop: test loaded (map %s)" % s.get("map"))
            return s
        if s.get("frame", 0) > 0:
            bridge.request("press", buttons=["A"], frames=8, wait=False, check=False)
        sleep(2.0)
    raise TimeoutError("not in play after %d s" % timeout)


SAVE_USER = "5a3c9e1f0b7d24680000000000000001"   # the user folder a Hookshot game reads its saves from


def restore_save(game):
    """<game>/autotest-save (the baseline) copied over the game's saves, as the emulator kits' saves are. The baseline
    is either a copy of <game>/user/save (user folders inside) or the save files themselves (account.sav, option.sav,
    slot_NN/: the same files as an emulator kit's autotest-save), which go to user/save/<SAVE_USER>."""
    base = os.path.join(game, "autotest-save")
    if not os.path.isdir(base):
        return False
    names = os.listdir(base)
    flat = any(n.endswith(".sav") or n.startswith("slot_") for n in names)
    dest = os.path.join(game, "user", "save", SAVE_USER) if flat else os.path.join(game, "user", "save")
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    shutil.copytree(base, dest)
    return True


def exe_dir(game):
    return os.path.join(game, "bin") if os.path.isfile(os.path.join(game, "bin", "longshot.exe")) else game


def find_log(game):
    for d in (exe_dir(game), game):
        p = os.path.join(d, "longshot_log.txt")
        if os.path.isfile(p):
            return p
    return None


def run(a):
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    say = Log(os.path.join(out, "steps.txt"), echo=print)
    tags = ["A", "B"]
    games = {"A": os.path.abspath(a.game_a), "B": os.path.abspath(a.game_b)}
    scripts = {"A": a.script_a, "B": a.script_b}
    parsed = {}
    for x in tags:
        if scripts[x]:
            with open(scripts[x], encoding="utf-8") as f:
                parsed[x] = parse(f.read())
    load = None
    if a.load:
        with open(a.load, encoding="utf-8") as f:
            load = parse(f.read())   # (checked; the load itself is "A until play", see to_play)
    procs, bridges, server, commands = {}, {}, None, {}
    failed = None
    try:
        if a.server:
            with open(os.path.join(out, "server-out.txt"), "w") as so:
                # Every session has a join password (it encrypts the traffic): in the
                # environment, never on the command line.
                senv = dict(os.environ, OOT_COOP_TEST_PASSWORD=a.password)
                server = subprocess.Popen([a.server, "--port", str(a.port), "--password-env", "OOT_COOP_TEST_PASSWORD"],
                                          stdout=so, stderr=subprocess.STDOUT, env=senv)
            time.sleep(1)
        for i, x in enumerate(tags):
            g = games[x]
            if not os.path.isfile(os.path.join(exe_dir(g), "longshot.exe" if WINDOWS else "longshot")):
                raise FileNotFoundError("no Hookshot game in %s" % g)
            if not a.no_restore:
                say("%s: save baseline %s" % (x, "restored" if restore_save(g) else "none (autotest-save missing)"))
            sock = "%s-%s" % (a.pipe, x) if WINDOWS else os.path.join(out, "control-%s.sock" % x)
            commands[x] = os.path.join(out, "commands-%s.txt" % x)
            open(commands[x], "w").close()
            env = dict(os.environ, HOS_CONTROL=sock, HOS_CONTROL_MEM="ro", LICHII_COOP_TEST_COMMANDS=commands[x])
            if a.server:
                env["LICHII_COOP_SERVER"] = "127.0.0.1:%d" % a.port
                env["LICHII_COOP_NAME"] = "Hook" + x
                env["LICHII_COOP_PASSWORD"] = a.password
            for kv in a.env:
                k, v = kv.split("=", 1)
                if k[:2] in ("A:", "B:"):
                    if k[0] != x:
                        continue
                    k = k[2:]
                env[k] = v
            exe = os.path.join(exe_dir(g), "longshot.exe" if WINDOWS else "longshot")
            with open(os.path.join(out, "game-out-%s.txt" % x), "wb") as go:
                kw = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if WINDOWS else {"start_new_session": True}
                procs[x] = subprocess.Popen([exe], cwd=exe_dir(g), env=env, stdin=subprocess.DEVNULL, stdout=go,
                                            stderr=subprocess.STDOUT, **kw)
            say("%s: started pid %d, bridge %s" % (x, procs[x].pid, sock))
            bridges[x] = (sock, Bridge.connect(sock, timeout=120))
            if i == 0:
                time.sleep(3)

        def shooter(x, name):
            def shot(mark):
                for y in tags:
                    stem = "%s-%s-%s" % (name, x, mark) if mark else name
                    path = os.path.join(out, "%s-%s.png" % (stem, y))
                    try:
                        bridges[y][1].request("shot", reply_timeout=30.0, path=path, timeout=20)
                    except Exception as e:
                        say("%s shot %s: %s" % (y, os.path.basename(path), e))
            return shot

        def both(name):
            shooter("", name)(None)

        if not a.no_load:
            errs = {}

            def loader(x):
                try:
                    lb = Bridge.connect(bridges[x][0])
                    try:
                        lb.request("wait", reply_timeout=a.timeout + 30, event="frame", frame=1, timeout=a.timeout)
                        to_play(lb, Log(os.path.join(out, "player-%s.txt" % x)), a.timeout)
                    finally:
                        lb.close()
                except Exception as e:
                    errs[x] = e
            ts = [threading.Thread(target=loader, args=(x,)) for x in tags]
            for t in ts:
                t.start()
            for t in ts:
                t.join()
            if errs:
                raise RuntimeError("load: " + ", ".join("%s %s" % kv for kv in errs.items()))
            both("loaded")
            say("save loaded")
        errs, players = {}, {}

        def play(x):
            try:
                pb = Bridge.connect(bridges[x][0])
                try:
                    p = Player(pb, Log(os.path.join(out, "player-%s.txt" % x)), shot=shooter(x, a.name),
                               connect=lambda: Bridge.connect(bridges[x][0]), commands=commands[x])
                    players[x] = p
                    p.play(parsed[x])
                finally:
                    pb.close()
            except Exception as e:
                errs[x] = e
        ts = [threading.Thread(target=play, args=(x,), daemon=True) for x in tags if x in parsed]
        for t in ts:
            t.start()
        start, n = time.monotonic(), 0
        while any(t.is_alive() for t in ts):
            for x in tags:
                if procs[x].poll() is not None:
                    raise RuntimeError("game %s ended by itself (exit code %s)" % (x, procs[x].returncode))
            if time.monotonic() - start > a.scenario_timeout:
                raise TimeoutError("scenario did not finish in %d s" % a.scenario_timeout)
            if a.every > 0 and time.monotonic() - start >= n * a.every:
                both("%s-%03d" % (a.name, n))
                n += 1
            time.sleep(0.25)
        if errs:
            raise RuntimeError("play: " + ", ".join("%s %s" % kv for kv in errs.items()))
        time.sleep(1)
        both("end")
        say("scenario played")
    except Exception as e:
        failed = str(e) or e.__class__.__name__
        say("FAILED: " + failed)
        try:
            both("failed")
        except Exception:
            pass
    finally:
        if not a.keep_open:
            for x, (_, b) in bridges.items():
                try:
                    b.request("quit", reply_timeout=10.0, check=False)
                except Exception:
                    pass
                b.close()
            for x, p in procs.items():
                try:
                    p.wait(15)
                except subprocess.TimeoutExpired:
                    p.kill()
            if server:
                server.terminate()
        for x in tags:
            # Hookshot writes longshot_log.txt only when its stderr goes nowhere; here stderr is game-out-<x>.txt, so
            # that is the log (an old longshot_log.txt in the folder is from another run). The game's file otherwise.
            go = os.path.join(out, "game-out-%s.txt" % x)
            lp = go if os.path.isfile(go) and os.path.getsize(go) > 0 else find_log(games[x])
            if lp:
                shutil.copy2(lp, os.path.join(out, "hookshot_log-%s.txt" % x))
    if failed:
        with open(os.path.join(out, "FAILED.txt"), "w", encoding="utf-8") as f:
            f.write(failed + "\n")
        return 1
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="what", required=True)
    c = sub.add_parser("check", help="parse scripts and print their steps")
    c.add_argument("scripts", nargs="+")
    pl = sub.add_parser("play", help="play one script into one running game")
    pl.add_argument("--sock", required=True)
    pl.add_argument("--script", required=True)
    pl.add_argument("--log")
    pl.add_argument("--shots", help="folder for a shot at each mark")
    pl.add_argument("--commands", help="the game's LICHII_COOP_TEST_COMMANDS file (none: module steps skipped)")
    r = sub.add_parser("run", help="the two-game test (tools/autotest/run.ps1 -Runtime hookshot)")
    r.add_argument("--game-a", required=True)
    r.add_argument("--game-b", required=True)
    r.add_argument("--server", help="coop_server to start (none: the games use their coop.ini)")
    r.add_argument("--port", type=int, default=27560)
    r.add_argument("--password", default="autotest-pass",
                   help="the join password of the server it starts, given to both games (every session has one)")
    r.add_argument("--out", required=True)
    r.add_argument("--name", default="scenario")
    r.add_argument("--script-a", required=True)
    r.add_argument("--script-b")
    r.add_argument("--load", help="load.txt: checked only; the load presses A until the game plays")
    r.add_argument("--no-load", action="store_true")
    r.add_argument("--no-restore", action="store_true")
    r.add_argument("--keep-open", action="store_true")
    r.add_argument("--env", action="append", default=[], help="NAME=VALUE (or A:NAME=VALUE) for the games")
    r.add_argument("--every", type=float, default=3.0)
    r.add_argument("--timeout", type=int, default=900, help="seconds for the boot and the load")
    r.add_argument("--scenario-timeout", type=int, default=600)
    r.add_argument("--pipe", default="coop-auto")
    a = p.parse_args(argv)
    if a.what == "check":
        bad = 0
        for path in a.scripts:
            with open(path, encoding="utf-8") as f:
                try:
                    steps = parse(f.read())
                except ScriptError as e:
                    print("%s: %s" % (path, e))
                    bad += 1
                    continue
            module = sorted({s.kind for s in steps if s.kind in MODULE_ONLY})
            print("%s: %d steps%s" % (path, len(steps), (", through the module: " + " ".join(module)) if module
                                       else ""))
        return 1 if bad else 0
    if a.what == "play":
        with open(a.script, encoding="utf-8") as f:
            steps = parse(f.read())
        b = Bridge.connect(a.sock)

        def shot(name):
            b.request("shot", reply_timeout=30.0, path=os.path.abspath(os.path.join(a.shots, name + ".png")))
        player = Player(b, Log(a.log, echo=print), shot=shot if a.shots else None,
                        connect=lambda: Bridge.connect(a.sock), commands=a.commands)
        player.play(steps)
        for t in player.warps:
            t.join(player.warp_for)
        b.close()
        return 0
    for kv in a.env:
        if "=" not in kv:
            p.error("--env NAME=VALUE")
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
