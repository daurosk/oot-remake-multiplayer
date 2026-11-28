"""Two fake games for the in-game tests' selftests (--fake): each a real TCP bridge on 127.0.0.1 speaking the play-test
protocol (docs/playtest.md "Bridge protocol"), with a module log file, frames, hold/step, and the few reactions the
pilot tests look for (a connect, puppets, masks, the ocarina, a shared flag, /teleport, the pause, the title).

It proves the runner, not the game: results, budgets, SETUP-FAIL / FLAKY / NO-REF, the summary, shots and their
compare, the lane fixture. What a reaction logs is copied from the tests' own patterns (the old rows' expect=).

A row port adds its own reactions WITHOUT editing this file: a module ingame/fakes/<name>.py (one per port, its owner's
file) with any of these hooks, each called for every fake game (A and B), in file-name order:
    setup(game)                      after the game's own fields are set (game.env = the boot's --env list,
                                     game.boot = the boot's name, --boot: a hook of a needs_boot row reacts only there)
    lane(fakelane)                   once both games exist and know each other (shared state: fakelane.g["A"/"B"])
    command(game, m) -> dict|None    before the built-in answer of bridge message m; a dict = the whole reply
    after(game, m, r)                after the built-in answer r (change r in place)
    press(game, buttons) -> bool     before the built-in pad reactions; EVERY module's hook gets the press (none can
                                     hide it from another row's: B #138); any True = the built-in ones are skipped
    act(game, words) -> bool         before the built-in `act` reactions (words = the line split); True = handled
    actors(game) -> list             more world actors for the state's world list
    move_step(game, pos) -> pos|None each frame of a `move`, Link's next [x, y, z]: return a changed one (a wall)
    state(game, st)                  change the state dict in place
A hook reacts only where its row is (its scene, its switch, its BT_FAKE_* selftest variable), so ports never collide.
B #149 (banner-steady-0121, chartab-0121): a hook never answers every call nor writes a shared state key for other
rows. Two guards: (1) a state key (st['menu'], st['banner'], ...) set by one module's hook and then replaced by another
module's in the same state is a 'fake-guard:' line (module log + <fake>/fake-guard.txt, tests/test_ingame.py fails on
it); (2) tests/test_ingame.py FakeIsolation: each test file's result alone == its result in the whole fake suite.
"""
import importlib.util
import json
import math
import os
import re
import socket
import threading

import game_names   # (the ONE item name table: the state's equips are the game's own value names)

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
# Values the game keeps in our Link's Life component, the flags being only its mirror (coop_cheat_give.h LinkBacked):
# the module writes the component first and the bridge reads back after STICK_FRAMES (coop_playtest_core.h
# kStickFrames). BT_FAKE_LIFE_FLAG_ONLY=1 plays the module before give-life-fix: the flag alone is written, the next
# frame puts Link's value back, and the bridge answers did_not_stick.
LINK_LIFE = ("PlayerStatus.Life", "PlayerStatus.MaxLife")
STICK_FRAMES = 2


def most_max_life():
    """The game's most hearts in quarters, read from the module's one value (coop/host/include/coop/host/
    game_items.hpp kMostMaxLife; never a copy): our Link's component keeps no more (volvagia-rocks-309d7180)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "coop", "host", "include",
                        "coop", "host", "game_items.hpp")
    with open(path, encoding="utf-8") as f:
        m = re.search(r"kMostMaxLife\s*=\s*(\d+)\s*\*\s*(\d+)\s*;", f.read())
    if not m:
        raise RuntimeError("fakegame: no 'kMostMaxLife = A * B;' in %s" % path)
    return int(m.group(1)) * int(m.group(2))


MOST_MAX_LIFE = most_max_life()


class FakeGame:
    def __init__(self, tag, folder, lane, env=(), fakes=(), solo_log=None, boot=""):
        self.tag, self.folder, self.lane, self.boot = tag, folder, lane, boot
        self.env, self.fakes = list(env), list(fakes)
        os.makedirs(os.path.join(folder, "playtest", "dumps"), exist_ok=True)
        os.makedirs(os.path.join(folder, "playtest", "shots"), exist_ok=True)
        self.log_path = os.path.join(folder, "game-out-%s.txt" % tag)
        self.folder_path, self.guarded = os.path.abspath(folder), set()
        self.lock = threading.RLock()
        self.frame, self.paused, self.scene = 100, False, "KokiriForest_Child"
        self.pos, self.facing = [0.0 if tag == "A" else 4.0, 0.0, 0.0], 0.0
        self.menu_pause, self.title_until, self.at_title = False, None, False
        self.head, self.ocarina, self.flags, self.pending = None, False, {GATE: 0}, []
        self.link_life = {"PlayerStatus.Life": 16, "PlayerStatus.MaxLife": 16}   # (our Link's Life component)
        self.gear_hidden = False   # (gear-link: part 29 worn by this game's player)
        self.leather, self.boots = None, None   # (leather-link: parts 28 and 15 worn by this game's player, RRGGBB)
        # (the module's ocarina re-equip after each load, Pair.need_ocarina: BT_FAKE_OCARINA_LATE=N frames after the
        # boot and after each warp, default 0 = at once; before it D-pad Left takes nothing out, songs-frogs-733c4d8f)
        self.ocarina_late, self.equipped = int(os.environ.get("BT_FAKE_OCARINA_LATE", "0") or 0), False
        # (the play UI up again BT_FAKE_UI_LATE frames after a warp, default 20; a tag in BT_FAKE_UI_NEVER: never)
        self.ui_late = int(os.environ.get("BT_FAKE_UI_LATE", "20") or 20)
        self.ui_never = tag in os.environ.get("BT_FAKE_UI_NEVER", "")
        # (the play UI down: + is refused, as the real game does, crawl-337-12 / gate-pause-309d7180. BT_FAKE_BOOT_UI_DOWN=A:
        # A's boot leaves its play UI down for BT_FAKE_BOOT_UI_FRAMES frames (default 120) after the bridge is up,
        # gate-pause-309d7180's A: its boot load's 'down' :4323, a soldier event, 'up' only at :5052 after the test's +.
        # That A was already in the default boot's scene (its script's own teleport), so the boot proof warped nothing:
        # the fake's A starts there too, else boot_ready's warp (and its own play UI wait) would hide the case)
        self.ui_down = False
        # (the game's first-use "Ocarina Audio Input" box on the boot's first D-pad Left, Pair.ocarina_out: the free-Ocarina
        # event line at once, 'plays the ocarina' only after B closes it; probe-ocarina-2f14b333)
        self.box, self.box_seen = False, False
        self.other = None
        # (selftests of the lane's boot proof, Pair.boot_ready: BT_FAKE_BOOT_EVENT=B leaves B where the save puts it,
        # Graveyard_Child, in an event a warp ends (test-bridge-pilot-fa63e90f r1); BT_FAKE_BOOT_STUCK=B the same but
        # the event never ends; BT_FAKE_STALE_FRAMES=N: no fresh Link sample for the first N frames, party-hud-bridge r1)
        self.event, self.stuck = False, tag in os.environ.get("BT_FAKE_BOOT_STUCK", "")
        if tag in os.environ.get("BT_FAKE_BOOT_EVENT", "") or self.stuck:
            self.scene, self.event = "Graveyard_Child", True
        self.stale_until = self.frame + int(os.environ.get("BT_FAKE_STALE_FRAMES", "0") or 0)
        open(self.log_path, "w").close()
        if tag in os.environ.get("BT_FAKE_BOOT_UI_DOWN", ""):
            self.scene, self.ui_down = "KakarikoVillage_Child", True
            self.say("coop: native ui: the game's play UI is down (a scene load or the title): no screen is live")
            self.later(int(os.environ.get("BT_FAKE_BOOT_UI_FRAMES", "120") or 120), self.ui_up)
        me, them = (1, 2) if tag == "A" else (2, 1)
        self.say("coop: multiplayer tab: test: to-title.now in %s starts the way back to the title" % folder)
        if solo_log:     # (bt_plugin --solo: game A alone, what the module logs with no server: solo.IDLE)
            self.say(solo_log)
        else:
            self.say("coop: connected to 127.0.0.1")
            self.say("coop: puppet: P%d CoopPuppet_Child_Green spawned after 120 ms" % them)
        if tag == "A":
            self.say("coop: drawing over Hookshot's picture")
        # (the host's boot mod list, as a real game-out: no_cs.has_character_select reads it; the no-CS switch drops
        # the mod on that game only, lanes.py nocs)
        cs = not self.env_on("COOP_TEST_NO_CHARACTER_SELECT=1")
        self.say("mods: contents/0400486024DEC000/coop-puppet: 41275 RomFS files")
        if cs:
            self.say("mods: OoT Character Select: 6903 RomFS files (20 replace the game's, 6883 added), ExeFS patches")
        self.say("mods: %d mods from %s: 48190 RomFS entries" % (2 if cs else 1, os.path.join(folder, "mods")))
        for h in hooks(self.fakes, "setup"):
            h(self)
        self.equip_later()
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        # (the module's bridge line, hookshot/src/coop_playtest.cpp:836; BT_FAKE_NO_MODULE=B: B's module never started,
        # party-hud-bridge-85e3b255. Its bridge port still answers here, so only the log check can catch it)
        if tag not in os.environ.get("BT_FAKE_NO_MODULE", ""):
            self.say("coop: playtest: listening on 127.0.0.1:%d (control on; dumps and shots in %s)" % (self.port, folder))
        threading.Thread(target=self.serve, daemon=True).start()

    def equip_later(self):
        """The module's ocarina re-equip after a load (Pair.need_ocarina), BT_FAKE_OCARINA_LATE frames later."""
        self.equipped = False

        def done():
            self.equipped = True
            self.say("coop: items: ocarina 'Item_Ocarina_Fairy' equipped by the game from its flags (slots 13, 16)")
        if self.ocarina_late > 0:
            self.later(self.ocarina_late, done)
        else:
            done()

    def ui_up(self):
        self.ui_down = False
        self.say("coop: native ui: the game's play UI is up (patch 0018's HUD screen ran since the load): its screens are "
                 "live")

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
        owner = {}
        for h in hooks(self.fakes, "state"):
            before = {k: id(v) for k, v in st.items()}
            h(self, st)
            for k, v in st.items():
                if before.get(k) == id(v):
                    continue
                if owner.get(k, h.__module__) != h.__module__:
                    self.guard("state key %r set by %s, then replaced by %s" % (k, owner[k], h.__module__))
                owner[k] = h.__module__
        return st

    def guard(self, what):
        """B #149: a fake module stepped on another's state (see the docstring's guards)."""
        line = "fake-guard: %s: %s" % (self.tag, what)
        if line in self.guarded:
            return
        self.guarded.add(line)
        self.say(line)
        with open(os.path.join(os.path.dirname(self.folder_path), "fake-guard.txt"), "a", encoding="utf-8") as f:
            f.write(line + "\n")

    def age(self):
        """Link's age, from the game's own flag as the real game keeps it (Sequence_IsChildhood: 1 child, 0 adult;
        unset = a child save). The ONE place a fake game's age comes from: a fake hook changes the flag (setflag, or
        its boot's save in setup), never st['age'] for every boot (dbno_crawl.py did, so row 286's adult boot read
        child: integrate-0120-4 18:59)."""
        return "adult" if self.flags.get("Sequence_IsChildhood", 1) in (0, False) else "child"

    def base_state(self):
        live = self.in_play()
        return {"frame": self.frame, "paused": self.paused, "scene": self.scene if not self.at_title else None,
                "pos": list(self.pos) if live else None, "facing": self.facing if live else None,
                "age": self.age() if live else None, "loading": False if live else None, "event": self.event if live else None,
                "not_ready": None if live else "no fresh Link sample", "control": True,
                "equips": {"weapon": game_names.KOKIRI_SWORD, "shield": None, "head": self.head, "armor": None, "boots": None},
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

    def write_link_life(self, name, value):
        """A give or flag of a Life flag: into Link's component (the fixed module), or, with BT_FAKE_LIFE_FLAG_ONLY=1,
        the flag alone, which the next frame puts back: then the bridge's did_not_stick reply (else None)."""
        was = self.link_life[name]
        if name == "PlayerStatus.MaxLife" and value > MOST_MAX_LIFE and value > was:
            # (the game keeps no more than its most: the frames put it back, the bridge's read-back says so)
            self.run(STICK_FRAMES)
            return {"ok": False, "error": "did_not_stick",
                    "message": "did not stick: %s written %d, the game put back %d within %d frames"
                               % (name, value, was, STICK_FRAMES)}
        if os.environ.get("BT_FAKE_LIFE_FLAG_ONLY") == "1":
            self.run(STICK_FRAMES)
            return {"ok": False, "error": "did_not_stick",
                    "message": "did not stick: %s written %d, the game put back %d within %d frames"
                               % (name, value, was, STICK_FRAMES)}
        self.link_life[name] = value
        return None

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
        elif cmd == "key" and m.get("key") is not None:
            # (a key no row's fake takes: co-op leaves it, Longshot's, never played: docs/playtest.md `key`. Here, not in
            # fakes/f1_event.py, which answered every key for every row: B #149)
            r.update(key=m.get("key"), vk=0, taken=False, longshot="none")
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
            # (the real verb: a name ending in the other age first sets Sequence_IsChildhood, docs/playtest.md 'warp';
            # so the age follows the flag, FakeGame.age, and no fake hook writes st['age'] for its scenes)
            want = {"_adult": "adult", "_child": "child"}.get(m["scene"][-6:].lower())
            if want and want != self.age():
                self.flags["Sequence_IsChildhood"] = 0 if want == "adult" else 1
            self.scene, self.event = m["scene"], False
            # (the real module's lines, hookshot/src/coop_hookshot.cpp Frame: the load takes the play UI down, the HUD
            # screen's run after the arrival puts it up; BT_FAKE_UI_LATE=N frames later, default 20; a tag in
            # BT_FAKE_UI_NEVER: never up again, Pair._until_play_ui's SETUP-FAIL)
            self.ui_down = True
            self.say("coop: native ui: the game's play UI is down (a scene load or the title): no screen is live")
            self.say("coop: native ui: scene load ended (Sequence_IsLoadingBanc): screens handed out once the game's "
                     "HUD screen has run")
            if not self.ui_never:
                self.later(self.ui_late, self.ui_up)
            self.equip_later()
            r.update(arrived=True, via="teleport", map=m["scene"], state=self.state())
        elif cmd == "move":
            self.walk(float(m["yaw"]), float(m.get("strength", 1.0)), int(m.get("frames", 1)))
            r.update(reads=int(m.get("frames", 1)), no_camera=0, stick=[0.0, float(m.get("strength", 1.0))], view=0.0,
                     state=self.state())
        elif cmd == "act":
            # The real bridge's rule (hookshot/src/coop_playtest_core.cpp ParseRequest "act"): one line, no control
            # character, '#' or ';'. Refused the same way here, so a test that sends one fails in the fakes, not on a
            # lane (hair-link-4217f901 16:48: 'menu look hair=#RRGGBB' was refused on both games).
            lines = m.get("lines") or ""
            if not lines or len(lines) > 256:
                return {"id": m.get("id"), "ok": False, "error": "bad_request",
                        "message": "lines: one test line of at most 256 characters (kill NAME, hit NAME N, hurt N, "
                                   "place X Y Z, ...)"}
            if any(ord(ch) < 0x20 or ch in "\x7f#;" for ch in lines):
                return {"id": m.get("id"), "ok": False, "error": "bad_request",
                        "message": "lines: one line, with no new line, ';' or '#'"}
            self.act(lines)
            r.update(lines=1, note="ok")
        elif cmd == "give":
            if m.get("item") in LINK_LIFE:
                name = m["item"]
                was = self.link_life[name]
                # (the module's give: max life +4 up to the game's most, coop_cheat_give.cpp Wanted)
                want = min(was + 4, max(was, MOST_MAX_LIFE)) if name == "PlayerStatus.MaxLife" else max(was, 1)
                if want == was:
                    r["result"] = "no change: %s already %d" % (name, was)
                else:
                    lost = self.write_link_life(name, want)
                    if lost:
                        return dict(lost, frame=self.frame, paused=self.paused)
                    r["result"] = "%s %d -> %d" % (name, was, self.link_life[name])
            else:
                r["result"] = "given"
            self.run(STICK_FRAMES)     # (the bridge's read-back)
        elif cmd == "flag" and m.get("name") in LINK_LIFE:
            name = m["name"]
            r["was"] = self.link_life[name]
            lost = self.write_link_life(name, int(m["value"]))
            if lost:
                return dict(lost, frame=self.frame, paused=self.paused)
            self.run(STICK_FRAMES)
            r["value"] = self.link_life[name]
        elif cmd == "flag":
            r["was"] = self.flags.get(m["name"], 0)
            self.flags[m["name"]] = m["value"]
            if m["name"] == GATE and m["value"] == 1 and r["was"] == 0:
                self.say("coop: story: bool 0x19898d14 (entry 3) 0 -> 1")
                o = self.other
                o.later(12, lambda: (o.flags.__setitem__(GATE, 1), o.say("coop: story: bool 0x19898d14 (entry 3) 0 -> 1")))
            r["value"] = m["value"]
        elif cmd == "flag_read" and m.get("name") in LINK_LIFE:
            # (the flag is the component's mirror, refreshed every frame: a read gives our Link's Life component)
            r.update(kind="int", value=self.link_life[m["name"]])
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
        taken = [bool(h(self, buttons)) for h in hooks(self.fakes, "press")]   # (all of them: no short-circuit)
        if any(taken):
            return
        o, me = self.other, 1 if self.tag == "A" else 2
        if "PLUS" in buttons and self.ui_down:
            self.say("fake: + refused: the play UI is down")    # (the real game's refusal, crawl-337-12)
        elif "PLUS" in buttons:
            # (BT_FAKE_PAUSE_STUCK=B: B's pause page opens and never closes again, the game a test left on a pause page,
            # tb-gate-ui-f9f3b9c2 test_pause FAIL; only a relaunch brings it back: Pair.recover, crawl-337-11)
            if not (self.menu_pause and self.tag in os.environ.get("BT_FAKE_PAUSE_STUCK", "")):
                self.menu_pause = not self.menu_pause
        if "A" in buttons and getattr(self, "title_asked", False) and not self.at_title:
            self.say("fake: A pressed in play after the way back was asked")   # (load_both must never: pilot A :7015)
        if self.at_title and "A" in buttons:
            self.later(30, lambda: (setattr(self, "at_title", False), setattr(self, "title_asked", False)))
            return
        if "LEFT" in buttons:
            if not self.equipped:
                return                                   # nothing on the D-pad yet: nothing comes out
            self.ocarina = True
            self.later(2, lambda: self.say("coop: event: the running event is our free Ocarina (by the game's request): "
                                           "our Link is not held by it, no hide, no hold, no claim"))
            if not self.box_seen:
                self.box, self.box_seen = True, True     # the first-use box: no play, no notes until it is closed
                return
            self.later(20, lambda: self.say("coop: ocarina probe: our Link plays the ocarina, 8 AS slots"))
            return
        if self.box:
            if "B" in buttons and self.tag not in os.environ.get("BT_FAKE_OCARINA_BOX_STUCK", ""):
                self.box = False
                self.later(20, lambda: self.say("coop: ocarina probe: our Link plays the ocarina, 8 AS slots"))
            return                                       # (the box eats every other button: no note)
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
        if w[:1] == ["chat"]:      # (every test chat is answered so, whatever it does: lichii/hook/coop_testinput.cpp:1789)
            text = line.split(None, 1)[1] if len(w) > 1 else ""
            self.later(2, lambda: self.say("coop: test chat '%s': sent" % text))
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
        elif w[0] == "menu" and len(w) == 3 and w[1] == "look" and w[2].startswith("link_gear="):
            # (gear-link: the wardrobe's lines, lichii/hook/coop_char_colours.cpp Wardrobe::Frame)
            hide = w[2] != "link_gear=none"
            if hide != self.gear_hidden:
                self.gear_hidden = hide
                what = "hidden" if hide else "shown again"
                self.later(2, lambda: self.say("coop: characters: equipment %s on your model (1 piece(s))" % what))
                o.later(10, lambda: o.say("coop: characters: equipment %s on a puppet (1 piece(s))" % what))
        elif w[0] == "menu" and len(w) == 3 and w[1] == "look" and w[2].split("=")[0] in ("link_leather", "boots"):
            # (leather-link: the gear wardrobe's and the puppets' boots wardrobe's lines, hookshot coop_look_tunic.cpp /
            # coop_look_live.cpp FollowBoots; names as CoopLookParts::Name / BootsPaintingName)
            key, value = w[2].split("=", 1)
            colour = None if value == "none" else value.lstrip("#").upper()
            setattr(self, "leather" if key == "link_leather" else "boots", colour)
            gear = "gear" if not (self.leather or self.boots) else "gear.%s%s" % (
                self.boots or "-", ".leather-" + self.leather if self.leather else "")
            boots = "boots" if not (self.leather or self.boots) else "boots.G%s%s" % (
                self.boots or "", "L" + self.leather if self.leather else "")
            if gear != "gear":
                self.later(2, lambda: self.say("coop: look: your Link's gear is %s now (1 texture(s) drawn)" % gear))
            o.later(10, lambda: o.say("coop: look: the puppets' boots are %s now" % boots))
        elif w[0] == "chat" and w[1:] == ["/teleport", "B"]:
            self.later(30, lambda: setattr(self, "pos", [o.pos[0] + 1.0, o.pos[1], o.pos[2]]))

    def picture(self, path):
        from PIL import Image, ImageDraw
        im = Image.new("RGB", (320, 180), (40, 90, 40))
        d = ImageDraw.Draw(im)
        d.rectangle([20, 20, 120, 40], fill=(200, 30, 30))             # "hearts"
        d.ellipse([140, 60, 180, 140], fill=(230, 220, 200) if not self.menu_pause else (20, 20, 20))
        d.rectangle([150 + self.frame % 3, 50, 170, 70], fill=(240, 240, 240) if self.head else (40, 90, 40))
        if not (self.gear_hidden or self.other.gear_hidden):   # (gear-link: a "sword" while A's gear is shown)
            d.rectangle([190, 80, 196, 130], fill=(170, 170, 190))
        # (leather-link: a player's leather/boots, on its own Link and its puppet in the other game: A's first, else
        # B's, as test_leather_without_character_select has the game without the mod (B) choose)
        a = self if self.tag == "A" else self.other
        shown = a.leather or a.boots or a.other.leather or a.other.boots
        if shown:
            d.rectangle([150, 140, 172, 156], fill=tuple(int(shown[i:i + 2], 16) for i in (0, 2, 4)))
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
    def __init__(self, out_dir, lane=1, env=(), fakes_dir=FAKES, solo_log=None, boot=""):
        root = os.path.join(out_dir, "fake")
        mods = load_fakes(fakes_dir)
        self.g = {x: FakeGame(x, os.path.join(root, "lane%d-%s" % (lane, x)), lane, env, mods, solo_log=solo_log,
                              boot=boot)
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
