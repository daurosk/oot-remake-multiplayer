"""Two fake games for the in-game tests' selftests (--fake): each a real TCP bridge on 127.0.0.1 speaking the play-test
protocol (docs/playtest.md "Bridge protocol"), with a module log file, frames, hold/step, and the few reactions the
pilot tests look for (a connect, puppets, masks, the ocarina, a shared flag, /teleport, the pause, the title).

It proves the runner, not the game: results, budgets, SETUP-FAIL / FLAKY / NO-REF, the summary, shots and their
compare, the lane fixture. What a reaction logs is copied from the tests' own patterns (the old rows' expect=).

A row port adds its own reactions WITHOUT editing this file: a module ingame/fakes/<name>.py (one per port, its owner's
file) with any of these hooks, each called for every fake game (A and B), in file-name order:
    setup(game)                      after the game's own fields are set (game.env = the boot's --env list)
    lane(fakelane)                   once both games exist and know each other (shared state: fakelane.g["A"/"B"])
    command(game, m) -> dict|None    before the built-in answer of bridge message m; a dict = the whole reply
    after(game, m, r)                after the built-in answer r (change r in place)
    press(game, buttons) -> bool     before the built-in pad reactions; True = handled, the built-in ones are skipped
    act(game, words) -> bool         before the built-in `act` reactions (words = the line split); True = handled
    actors(game) -> list             more world actors for the state's world list
    move_step(game, pos) -> pos|None each frame of a `move`, Link's next [x, y, z]: return a changed one (a wall)
    state(game, st)                  change the state dict in place
A hook reacts only where its row is (its scene, its switch, its BT_FAKE_* selftest variable), so ports never collide.
"""
import importlib.util
import json
import math
import os
import socket
import threading

FAKES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fakes")


def load_fakes(folder=FAKES):
    """The row ports' fake modules (ingame/fakes/*.py, not _*.py), in file-name order."""
    mods = []
    if not os.path.isdir(folder):
        return mods
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".py") or name.startswith("_"):
            continue
        spec = importlib.util.spec_from_file_location("fakes_" + name[:-3], os.path.join(folder, name))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        mods.append(mod)
    return mods


def hooks(mods, name):
    return [getattr(m, name) for m in mods if callable(getattr(m, name, None))]

GATE = "Npc_Hylia_Soldier_003_GateOpen"


class FakeGame:
    def __init__(self, tag, folder, lane, env=(), fakes=(), solo_log=None):
        self.tag, self.folder, self.lane = tag, folder, lane
        self.env, self.fakes = list(env), list(fakes)
        os.makedirs(os.path.join(folder, "playtest", "dumps"), exist_ok=True)
        os.makedirs(os.path.join(folder, "playtest", "shots"), exist_ok=True)
        self.log_path = os.path.join(folder, "game-out-%s.txt" % tag)
        self.lock = threading.RLock()
        self.frame, self.paused, self.scene = 100, False, "KokiriForest_Child"
        self.pos, self.facing = [0.0 if tag == "A" else 4.0, 0.0, 0.0], 0.0
        self.menu_pause, self.title_until, self.at_title = False, None, False
        self.head, self.ocarina, self.flags, self.pending = None, False, {GATE: 0}, []
        self.other = None
        # (selftests of the lane's boot proof, Pair.boot_ready: BT_FAKE_BOOT_EVENT=B leaves B where the save puts it,
        # Graveyard_Child, in an event a warp ends (test-bridge-pilot-fa63e90f r1); BT_FAKE_BOOT_STUCK=B the same but
        # the event never ends; BT_FAKE_STALE_FRAMES=N: no fresh Link sample for the first N frames, party-hud-bridge r1)
        self.event, self.stuck = False, tag in os.environ.get("BT_FAKE_BOOT_STUCK", "")
        if tag in os.environ.get("BT_FAKE_BOOT_EVENT", "") or self.stuck:
            self.scene, self.event = "Graveyard_Child", True
        self.stale_until = self.frame + int(os.environ.get("BT_FAKE_STALE_FRAMES", "0") or 0)
        open(self.log_path, "w").close()
        me, them = (1, 2) if tag == "A" else (2, 1)
        self.say("coop: multiplayer tab: test: to-title.now in %s starts the way back to the title" % folder)
        if solo_log:     # (bt_plugin --solo: game A alone, what the module logs with no server: solo.IDLE)
            self.say(solo_log)
        else:
            self.say("coop: connected to 127.0.0.1")
            self.say("coop: puppet: P%d CoopPuppet_Child_Green spawned after 120 ms" % them)
        if tag == "A":
            self.say("coop: drawing over Hookshot's picture")
        for h in hooks(self.fakes, "setup"):
            h(self)
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        # (the module's bridge line, hookshot/src/coop_playtest.cpp:836; BT_FAKE_NO_MODULE=B: B's module never started,
        # party-hud-bridge-85e3b255. Its bridge port still answers here, so only the log check can catch it)
        if tag not in os.environ.get("BT_FAKE_NO_MODULE", ""):
            self.say("coop: playtest: listening on 127.0.0.1:%d (control on; dumps and shots in %s)" % (self.port, folder))
        threading.Thread(target=self.serve, daemon=True).start()

    def say(self, line):
        with open(self.log_path, "a", encoding="utf-8") as f:
            f.write("[hos fake] %s\n" % line)

    def later(self, frames, fn):
        self.pending.append((self.frame + frames, fn))

    # -- frames -----------------------------------------------------------------------------------------------
    def run(self, n):
        for _ in range(int(n)):
            self.frame += 1
            due = [p for p in self.pending if p[0] <= self.frame]
            self.pending = [p for p in self.pending if p[0] > self.frame]
            for _, fn in due:
                fn()
            if os.path.exists(os.path.join(self.folder, "to-title.now")):
                os.remove(os.path.join(self.folder, "to-title.now"))
                self.say("coop: multiplayer tab: test: back to the title asked (to-title.now)")
                self.title_asked = True
                if self.tag in os.environ.get("BT_FAKE_TO_TITLE_GIVES_UP", ""):   # (the pilot's way back, both games)
                    self.later(60, lambda: self.say("coop: pause tab: back to the title: given up (the menu did not "
                                                    "get there)"))
                    continue
                self.at_title = True
                self.say("coop: live events: the title: the game's earlier event requests are forgotten")
                self.say("coop: native ui: the title's menu is up: no play scene, no screen is called into until play is back")
                self.say("coop: map: the title is up: the panes found are forgotten, looked for again after it")

    def in_play(self):
        return not self.at_title and not self.menu_pause and self.frame >= self.stale_until

    def env_on(self, want):
        """Is the switch `want` (NAME=VALUE) on for this game in the boot's env (the 'X=1' or 'A:X=1' form)?"""
        return any(e == want or e == "%s:%s" % (self.tag, want) for e in self.env)

    def state(self):
        st = self.base_state()
        for h in hooks(self.fakes, "actors"):
            st["world"]["actors"].extend(h(self) or [])
        for h in hooks(self.fakes, "state"):
            h(self, st)
        return st

    def base_state(self):
        live = self.in_play()
        return {"frame": self.frame, "paused": self.paused, "scene": self.scene if not self.at_title else None,
                "pos": list(self.pos) if live else None, "facing": self.facing if live else None,
                "age": "child" if live else None, "loading": False if live else None, "event": self.event if live else None,
                "not_ready": None if live else "no fresh Link sample", "control": True,
                "equips": {"weapon": "KokiriSword", "shield": None, "head": self.head, "armor": "Kokiri", "boots": "Normal"},
                "coop": {"players": [{"number": 1, "name": "A", "state": "connected"},
                                     {"number": 2, "name": "B", "state": "connected"}]},
                "world": {"actors": [{"id": 1, "name": "CoopPuppet_Child_Green", "kind": "player",
                                      "pos": list(self.other.pos)}]}}

    # -- commands ---------------------------------------------------------------------------------------------
    def answer(self, m):
        for h in hooks(self.fakes, "command"):
            r = h(self, m)
            if r is not None:
                r.setdefault("id", m.get("id"))
                r.setdefault("frame", self.frame)
                r.setdefault("paused", self.paused)
                return r
        r = self.builtin_answer(m)
        for h in hooks(self.fakes, "after"):
            h(self, m, r)
        return r

    def builtin_answer(self, m):
        cmd = m.get("cmd")
        r = {"id": m.get("id"), "ok": True}
        if cmd == "hello":
            r.update(protocol=1, control=True)
        elif cmd == "pause":
            self.paused = True
        elif cmd == "resume":
            self.paused = False
        elif cmd == "state":
            r.update(self.state())
        elif cmd in ("step", "input"):
            r["from"] = self.frame
            if cmd == "input":
                self.press(m.get("buttons") or [])
            self.run(m.get("frames", 1))
            self.paused = True
            r["state"] = self.state()
        elif cmd == "warp":
            self.run(40)
            if self.stuck:     # (the event never ends: the bridge's warp answers timeout, still in it)
                return {"id": m.get("id"), "ok": False, "error": "timeout", "message": "fake: still in the event",
                        "frame": self.frame, "paused": self.paused}
            self.scene, self.event = m["scene"], False
            r.update(arrived=True, via="teleport", map=m["scene"], state=self.state())
        elif cmd == "move":
            self.walk(float(m["yaw"]), float(m.get("strength", 1.0)), int(m.get("frames", 1)))
            r.update(reads=int(m.get("frames", 1)), no_camera=0, stick=[0.0, float(m.get("strength", 1.0))], view=0.0,
                     state=self.state())
        elif cmd == "act":
            self.act(m["lines"])
            r.update(lines=1, note="ok")
        elif cmd == "give":
            r["result"] = "given"
        elif cmd == "flag":
            r["was"] = self.flags.get(m["name"], 0)
            self.flags[m["name"]] = m["value"]
            if m["name"] == GATE and m["value"] == 1 and r["was"] == 0:
                self.say("coop: story: bool 0x19898d14 (entry 3) 0 -> 1")
                o = self.other
                o.later(12, lambda: (o.flags.__setitem__(GATE, 1), o.say("coop: story: bool 0x19898d14 (entry 3) 0 -> 1")))
            r["value"] = m["value"]
        elif cmd == "flag_read":
            r.update(kind="bool", value=self.flags.get(m["name"]))
        elif cmd == "dump":
            path = os.path.join(self.folder, "playtest", "dumps", "%s.json" % m["tag"])
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"frame": self.frame, "tag": m["tag"], "state": self.state(), "flags": dict(self.flags)}, f)
            r.update(path=path, flags=len(self.flags))
        elif cmd == "shot":
            path = os.path.join(self.folder, "playtest", "shots", "%s.png" % m.get("name", "shot"))
            self.picture(path)
            self.run(1)
            r.update(path=path, width=320, height=180, advanced=1)
        else:
            r = {"id": m.get("id"), "ok": False, "error": "bad_request", "message": "fake: no %s" % cmd}
        r.setdefault("frame", self.frame)
        r.setdefault("paused", self.paused)
        return r

    def press(self, buttons):
        if any(h(self, buttons) for h in hooks(self.fakes, "press")):
            return
        o, me = self.other, 1 if self.tag == "A" else 2
        if "PLUS" in buttons:
            self.menu_pause = not self.menu_pause
        if "A" in buttons and getattr(self, "title_asked", False) and not self.at_title:
            self.say("fake: A pressed in play after the way back was asked")   # (load_both must never: pilot A :7015)
        if self.at_title and "A" in buttons:
            self.later(30, lambda: (setattr(self, "at_title", False), setattr(self, "title_asked", False)))
            return
        if "LEFT" in buttons:
            self.ocarina = True
            self.later(20, lambda: self.say("coop: ocarina probe: our Link plays the ocarina"))
            return
        if "B" in buttons:
            self.ocarina = False
        notes = {"L": (4, "D"), "R": (8, "F"), "Y": (11, "B"), "X": (16, "HighD"), "A": (14, "A")}
        for b in buttons:
            if self.ocarina and b in notes:
                n, name = notes[b]
                self.later(3, lambda n=n, name=name: self.say("coop: ocarina note %d (Ocarina_%s) sent" % (n, name)))
                o.later(8, lambda name=name: o.say("coop: puppet: P%d plays sound 'Ocarina_%s'" % (me, name)))

    def walk(self, yaw, strength, frames):
        """`move` (docs/playtest.md): 0.1 m a frame toward world yaw (0 = +z, 90 = +x); a fakes/ module's move_step
        may change each step (a wall). From row-53-ocarina-5's b829adf93, made generic by the file's owner."""
        for _ in range(frames):
            pos = [self.pos[0] + 0.1 * strength * math.sin(math.radians(yaw)), self.pos[1],
                   self.pos[2] + 0.1 * strength * math.cos(math.radians(yaw))]
            for h in hooks(self.fakes, "move_step"):
                pos = h(self, pos) or pos
            self.pos = list(pos)
            self.run(1)
        self.paused = True

    def act(self, line):
        w = line.split()
        if any(h(self, w) for h in hooks(self.fakes, "act")):
            return
        o, me = self.other, 1 if self.tag == "A" else 2
        if w[0] == "place":
            self.pos = [float(v) for v in w[1:4]]
        elif w[0] == "face":
            dx, dz = float(w[1]) - self.pos[0], float(w[2]) - self.pos[2]
            self.facing = math.degrees(math.atan2(dx, dz)) % 360
        elif w[0] == "wear" and w[1] == "head":
            self.head = None if w[2] == "none" else w[2]
        elif w[0] == "dress":
            head = self.head
            if head:
                o.later(10, lambda: o.say("coop: puppet: P%d wears head '%s' (returned 0x1)" % (me, head)))
            else:
                o.later(10, lambda: o.say("coop: puppet: P%d takes off head (returned 0x1)" % me))
        elif w[0] == "chat" and w[1:] == ["/teleport", "B"]:
            self.later(30, lambda: setattr(self, "pos", [o.pos[0] + 1.0, o.pos[1], o.pos[2]]))

    def picture(self, path):
        from PIL import Image, ImageDraw
        im = Image.new("RGB", (320, 180), (40, 90, 40))
        d = ImageDraw.Draw(im)
        d.rectangle([20, 20, 120, 40], fill=(200, 30, 30))             # "hearts"
        d.ellipse([140, 60, 180, 140], fill=(230, 220, 200) if not self.menu_pause else (20, 20, 20))
        d.rectangle([150 + self.frame % 3, 50, 170, 70], fill=(240, 240, 240) if self.head else (40, 90, 40))
        im.save(path)

    # -- socket -----------------------------------------------------------------------------------------------
    def serve(self):
        while True:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            with conn, conn.makefile("rb") as rf:
                for line in rf:
                    with self.lock, self.other.lock:
                        reply = self.answer(json.loads(line))
                    try:
                        conn.sendall((json.dumps(reply) + "\n").encode())
                    except OSError:
                        break
            self.paused = False

    def close(self):
        self.srv.close()


class FakeLane:
    def __init__(self, out_dir, lane=1, env=(), fakes_dir=FAKES, solo_log=None):
        root = os.path.join(out_dir, "fake")
        mods = load_fakes(fakes_dir)
        self.g = {x: FakeGame(x, os.path.join(root, "lane%d-%s" % (lane, x)), lane, env, mods, solo_log=solo_log)
                  for x in ("A", "B")}
        self.g["A"].other, self.g["B"].other = self.g["B"], self.g["A"]
        for h in hooks(mods, "lane"):
            h(self)

    def games(self):
        return {x: {"port": g.port, "folder": g.folder, "log": g.log_path} for x, g in self.g.items()}

    def window_picture(self, x, path):
        """The fake of a WINDOW shot (bt_plugin's fake Pair.window_shooter): game x's picture plus a bar the bridge
        shot never has (Longshot's ImGui overlay is only in the window). Does not step the game."""
        from PIL import Image, ImageDraw
        g = self.g[x]
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with g.lock:
            g.picture(path)
        with Image.open(path) as im:
            im = im.convert("RGB")
        ImageDraw.Draw(im).rectangle([200, 150, 300, 170], fill=(250, 200, 0))   # ("the overlay")
        im.save(path)
        return path

    def close(self):
        for g in self.g.values():
            g.close()
