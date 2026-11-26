"""One test lane's two games, driven only through the play-test bridge (docs/playtest.md, "Bridge protocol").

The in-game test system (tools/autotest/ingame, docs/ingame-tests.md): every check is a pytest test that gets a Pair.
A Pair holds both games' bridge clients (tools/autotest/playtest_client.py) and their module logs, and keeps the two
games in step: every frame A runs, B runs too (step_both), so nothing is timed by wall clock and no OS key, window focus
or screen is ever used. Set-up goes by warp / place / setflag / give, judging by the state JSON, log lines and
checkpoints; a shot only where a row is about looks (check_shot, compared to a stored reference by imagecheck.py).

Every wait is counted in game frames with a ceiling, and every call checks the test's deadline (Deadline), so a test
ends as FAIL 'timeout' on its own instead of hanging the lane.
"""
import contextlib
import hashlib
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
AUTOTEST = os.path.dirname(HERE)
sys.path.insert(0, AUTOTEST)
sys.path.insert(0, HERE)
import playtest_client as pc   # noqa: E402

CHUNK = 10            # frames per step while waiting for a condition (a log line, a state)
SETUP_LIMIT_S = 30.0  # owner rule (worker-rules TEST SYSTEM): a test's set-up proves itself in under 30 s
BOOT_FRAMES = 2400    # the boot proof's ceiling per wait (80 s of game: a boot script that ended with both games still
#                       loading out of the menu, party-hud-bridge r1, renderers ready after 168/180 s)
MODULE_WAIT_S = 10.0  # the module's bridge line in each game's log (written at the module's load, before live.py start
                      # returns; the wait only covers the log's flush)
AWAY_CHECK_S = 10.0   # after each test: the one state read per game of away_from_boot (a bridge reply is < 1 s)
READY_FRAMES = 300    # a set-up's step-until-in-play ceiling (10 s of game: a stale sample, an event's end; inside 30 s)
FPS = 30              # Ocarina of Time's calc rate on Hookshot: frames -> seconds in messages only, never for waiting
# A warp's wall time is the game's (a load), not ours: measured, not guessed. warp_both (A then B, Longshot's teleport)
# took 26.9 / 28.8 / 31.0 s on a full lane 2 (party-hud-bridge-6947aa3d summary.md test_warp r1-r3), i.e. one game's
# warp 13.5-15.5 s. So a set-up that warps cannot be proven in SETUP_LIMIT_S (the cause of that run's 3 SETUP-FAILs).
WARP_PAIR_MEASURED_S = 31.0
RECOVER_WARP_S = WARP_PAIR_MEASURED_S   # one game's warp in a recovery: the worst TWO-game warp = 2x one game's (margin)
RECOVER_BASE_S = SETUP_LIMIT_S          # a recovery's other part: out of a load, an event's end, in play (stepped)
# The shortest reply wait a call is ever sent with (boss-respawn-bridge-e3e6e253, CAUSE 2: a call sent with the 0.2 s
# left of a spent deadline timed the socket out; playtest_client.request closes it on any OSError and the bridge lets the
# game RUN FREE mid-arrival, so every later test's recovery met 'teleport still in progress'). Cited parts: a held
# game's `state` waits up to 300 ms for a snapshot (docs/playtest.md "world"), Longshot's status waits 250 ms for an
# update (docs/playtest.md, control.c cmd_status), a `shot` runs up to 5 frames; frames at the client's own worst rate,
# 20 a second (playtest_client.py input/move: timeout + frames / 20.0). 0.3 + 0.25 + 5/20 = 0.8 s, x2.5 margin.
REPLY_MIN_S = 2.0
CLIENT_FPS = 20.0     # the client's worst stepping rate (playtest_client.py input/move reply waits)
# The ocarina on the D-pad after a scene load: the module's equip (coop/host/src/game_items.cpp OcarinaEquip: arm() waits
# kSettleFrames = 30 frames, game_items.hpp:232, with Link known and not busy; the line at game_items.cpp:817; docs/items.md
# 'The game's own equip') re-equips it from the flags after each load and logs this line; a D-pad press before it takes
# nothing out (songs-frogs-733c4d8f SETUP-FAIL 11-26: B's line, hookshot_log-B.txt:4581, came after a 0.7 s event at the
# Kakariko gate and after the test's LEFT; no bridge run had taken an ocarina out before).
OCARINA_EQUIPPED = r"coop: items: ocarina '\w+' equipped by the game from its flags"
SCENE_LOADED = r"coop: native ui: scene load ended"     # (seen: songs-frogs-733c4d8f hookshot_log-B.txt:4289, then :4581)
OCARINA_FRAMES = 600  # 20 s of game: the mend's 30 frames + an arrival event's end (the frogs' boot event: 0.7 s)
# D-pad Left's two module lines (probe-ocarina-2f14b333, 11-26, both games, cited by its game-out-X.txt line): the press
# ALWAYS starts the game's free-Ocarina event (OCARINA_FREE: B :4810, A :5814; also both frogs SETUP-FAIL runs, 733c4d8f
# :4748, 58f13b22 :4898). The first time on a boot the game's first-use "Ocarina Audio Input" box is up
# (probe-oca-12f-30-B.png) and Link does not play yet: OCARINA_PLAYS came only after B closed that box (B :5540, A :6440;
# 500 frames and a second 30-frame LEFT before it brought nothing). The frogs runs waited for OCARINA_PLAYS alone.
OCARINA_FREE = r"coop: event: the running event is our free Ocarina"      # coop/host/include/coop/host/event_owner.hpp:58, docs/scenes.md:41
OCARINA_PLAYS = r"ocarina probe: our Link plays the ocarina"              # lichii/hook/coop_ocarina.cpp:320 (PROBE=1)
# The no-box wait for OCARINA_PLAYS after LEFT: 240 frames, the scenario's own 'press DLEFT 12' + 'wait 240'
# (scenarios/hs-ocarina.A.txt:23-24) that run 20261115-185256 proved (docs/items.md:340: the plays line after DLEFT
# within it). Not shortened: no run measured the no-box gap itself (review-c123-13 17:47). In the box case nothing
# closes it by itself (probe: 240 + 30 + 240 frames and no plays line until B).
OCARINA_PLAYS_FRAMES = 240


class SetupFail(Exception):
    """The test could not be set up (the lane, a warp, a give, the set-up took more than 30 s): SETUP-FAIL, not FAIL."""


class NoRef(Exception):
    """NO-REF, never PASS: a look check without a stored reference (the pair fixture's end), or a test that cannot
    prove its row yet (Pair.unproven). bt_plugin judges it by this TYPE, never by message text."""


class TestTimeout(AssertionError):
    """The test ran past its own deadline: FAIL 'timeout' (never retried as a pass)."""


class Deadline:
    def __init__(self, seconds, clock=time.monotonic):
        self.clock = clock
        self.end = clock() + seconds
        self.seconds = seconds

    def left(self):
        return self.end - self.clock()

    def check(self, what):
        if self.left() <= 0:
            raise TestTimeout("timeout after %.0f s: %s" % (self.seconds, what))


class Log:
    """One game's module log (the session's game-out-X.txt): new lines since a mark, read from the file's end."""

    def __init__(self, path):
        self.path = path
        self.pos = self.size()

    def size(self):
        try:
            return os.path.getsize(self.path)
        except OSError:
            return 0

    def mark(self):
        self.pos = self.size()
        return self.pos

    def since(self, pos=None):
        start = self.pos if pos is None else pos
        try:
            with open(self.path, "rb") as f:
                f.seek(start)
                data = f.read()
        except OSError:
            return []
        return data.decode("utf-8", "replace").splitlines()

    def find(self, pattern, pos=None):
        rx = re.compile(pattern)
        for ln in self.since(pos):
            if rx.search(ln):
                return ln
        return None


NOCS = "-nocs"                       # lane<N>-B-nocs: lane B's game without Character Select (lanes.py nocs)
CS_MOD = "OoT Character Select"


def lane_check(kit_ls, lane, nocs=False):
    """The lane's setup proof, the Python twin of offline/lib/lane-server.ps1 + lane-pair.ps1 (one source of the rule:
    both read lane-server.txt as offline/lane-game-for.ps1 writes it). Returns {server, commit, dll_sha256, game_b};
    raises SetupFail with the first thing wrong. nocs: game B is lane<N>-B-nocs (lanes.py nocs), whose own
    lane-server.txt must name the same module, commit and server as lane<N>-B's and whose game has no Character
    Select code (only its exefs= line differs: nocs_check)."""
    got = {}
    for x in ("A", "B") + (("B" + NOCS,) if nocs else ()):
        d = os.path.join(kit_ls, "lane%d-%s" % (lane, x))
        f = os.path.join(d, "lane-server.txt")
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                info = fh.read()
        except OSError:
            raise SetupFail("no %s (the lane was not installed by lane-game-for.ps1)" % f)
        def field(name, rx=r".+?"):
            m = re.search(r"(?m)^%s=(%s)\s*$" % (name, rx), info)
            return m.group(1) if m else ""
        srv, want, commit = field("server"), field("dll_sha256", "[0-9A-Fa-f]{64}").upper(), field("commit", "[0-9a-f]+")
        want_p = field("patches_line_sha256", "[0-9A-Fa-f]{64}").upper()
        if not srv or not want:
            raise SetupFail("%s has no %s= line" % (f, "server" if not srv else "dll_sha256"))
        if not want_p:
            raise SetupFail("%s has no patches_line_sha256= line (installed before 11-26: run a lane-prep)" % f)
        dll = os.path.join(d, "hookshot_coop.dll")
        have = _sha256_file(dll)
        if have != want:
            raise SetupFail("lane%d-%s hookshot_coop.dll %s is not lane-server.txt's %s (the lane changed after its "
                            "install)" % (lane, x, (have or "(none)")[:16], want[:16]))
        line = _patches_line(os.path.join(d, "coop-built-from.txt"))
        have_p = hashlib.sha256(line.encode("utf-8")).hexdigest().upper() if line is not None else ""
        if have_p != want_p:
            raise SetupFail("lane%d-%s game patches line %s is not lane-server.txt's %s (the game was rebuilt after the "
                            "lane's install)" % (lane, x, have_p[:8] or "(none)", want_p[:8]))
        got[x] = {"server": srv, "commit": commit, "dll_sha256": want, "patches": want_p, "folder": d,
                  "nocs_of": field("nocs_of")}
    if nocs:
        nocs_check(got["B"], got["B" + NOCS], lane)
        got["B"] = got["B" + NOCS]
    a, b = got["A"], got["B"]
    if a["dll_sha256"] != b["dll_sha256"]:
        raise SetupFail("lane%d A/B module differ: %s vs %s" % (lane, a["dll_sha256"][:16], b["dll_sha256"][:16]))
    if a["patches"] != b["patches"]:
        raise SetupFail("lane%d A and B games come from different patches builds" % lane)
    if a["server"] != b["server"]:
        raise SetupFail("lane%d A and B name different servers (%s / %s)" % (lane, a["server"], b["server"]))
    if not os.path.exists(a["server"]):
        raise SetupFail("no server %s (lane%d lane-server.txt)" % (a["server"], lane))
    return dict({k: v for k, v in a.items() if k not in ("folder", "nocs_of")}, game_b=b["folder"],
                game_b_exefs=_exefs_line(os.path.join(b["folder"], "coop-built-from.txt")))


def _exefs_line(path):
    """coop-built-from.txt's exefs= line (the mods' code patches the game was built with), '' when it has none."""
    try:
        with open(path, encoding="utf-8-sig", errors="replace") as f:
            return next((ln.rstrip("\r\n") for ln in f if ln.startswith("exefs=")), "")
    except OSError:
        return ""


def nocs_check(b, n, lane):
    """lane<N>-B-nocs is lane<N>-B's game without Character Select: made by lanes.py nocs from THIS lane B (nocs_of),
    the same module, commit and server, and a game whose code patches and code-patch list name no Character Select."""
    tag = "lane%d-B%s" % (lane, NOCS)
    if os.path.normcase(n["nocs_of"]) != os.path.normcase(os.path.basename(b["folder"])):
        raise SetupFail("%s lane-server.txt has nocs_of=%s, not %s (made by lanes.py nocs --lane %d?)"
                        % (tag, n["nocs_of"] or "(none)", os.path.basename(b["folder"]), lane))
    for k in ("dll_sha256", "commit", "server"):
        if n[k] != b[k]:
            raise SetupFail("%s %s %s is not lane%d-B's %s (lane B was installed again: lanes.py nocs --lane %d)"
                            % (tag, k, n[k][:16], lane, b[k][:16], lane))
    line = _exefs_line(os.path.join(n["folder"], "coop-built-from.txt"))
    if CS_MOD.lower() in line.lower():
        raise SetupFail("%s game was built with Character Select's code (%s)" % (tag, line[:80]))
    try:
        with open(os.path.join(n["folder"], "exefs_patches.txt"), encoding="utf-8", errors="replace") as f:
            rows = [ln for ln in f if not ln.startswith("#") and ln.lower().startswith(CS_MOD.lower() + "\t")]
    except OSError:
        rows = []
    if rows:
        raise SetupFail("%s exefs_patches.txt still lists Character Select's code patch" % tag)
    if os.path.isdir(os.path.join(n["folder"], "mods", CS_MOD)):
        raise SetupFail("%s still has mods/%s" % (tag, CS_MOD))


def _sha256_file(path):
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
        return h.hexdigest().upper()
    except OSError:
        return ""


def _patches_line(path):
    """The first 'patches=' line, without its line end (lane-server.ps1 hashes Get-Content's line the same way)."""
    try:
        with open(path, encoding="utf-8-sig", errors="replace") as f:
            for ln in f:
                ln = ln.rstrip("\r\n")
                if ln.startswith("patches="):
                    return ln
    except OSError:
        pass
    return None


def _loaded(st):
    """A scene and no load running (a stale sample may still be there: until_in_play steps that out)."""
    return bool(st.get("scene")) and not st.get("loading")


class Pair:
    """Both games of one lane. `games` = {"A": {"port", "folder", "log"}, "B": {...}} (the live.py session's)."""

    def __init__(self, games, client=pc.Client, out_dir=None, refs_dir=None):
        self.games = games
        self.c = {x: client(g["port"], timeout=30.0) for x, g in games.items()}
        self.log = {x: Log(g["log"]) for x, g in games.items()}
        self.out_dir = out_dir
        self.refs_dir = refs_dir
        self.deadline = Deadline(3600)
        self.shots = []        # [{"game", "name", "path", "ref", "score", "ok"}] of the running test
        self.boot_scene = None # the boot's scene (bt_plugin._boot_proof): where every default-boot test starts
        self.notes = []

    # -- per test ---------------------------------------------------------------------------------------------
    def begin(self, seconds):
        """A new test: its deadline, fresh log marks, no shots yet."""
        self.deadline = Deadline(seconds)
        self.shots, self.notes = [], []
        for lg in self.log.values():
            lg.mark()

    def close(self):
        for c in self.c.values():
            c.close()

    @contextlib.contextmanager
    def setup(self, limit=SETUP_LIMIT_S):
        """A test's set-up: anything that goes wrong in it, or more than `limit` seconds, is SETUP-FAIL (not FAIL).
        While it runs the deadline is min(the test's, `limit`), so every call inside it (reply waits included, see
        _wait) stops at the limit: a hung set-up is a SETUP-FAIL after `limit` s, not a FAIL at the test's budget."""
        outer = self.deadline
        clock = outer.clock
        self.deadline = Deadline(max(0.0, min(limit, outer.left())), clock=clock)
        t0 = clock()
        try:
            yield
        except SetupFail:
            raise
        except Exception as e:
            raise SetupFail("set-up: %s: %s" % (type(e).__name__, e)) from e
        finally:
            self.deadline = outer
        took = clock() - t0
        if took > limit:
            raise SetupFail("set-up took %.1f s (limit %.0f s)" % (took, limit))
        self.notes.append("set-up %.1f s" % took)

    # -- frames -----------------------------------------------------------------------------------------------
    def _need(self, cmd, frames=0):
        """The reply wait a call needs: REPLY_MIN_S, plus its frames at CLIENT_FPS (step / input / move run them)."""
        return REPLY_MIN_S + max(0, int(frames or 0)) / CLIENT_FPS

    def _wait(self, cmd="call", need=REPLY_MIN_S):
        """A reply's wait: at least what the call needs and at most what is left of the deadline (60 s when the call
        needs less). If the deadline cannot give it what it needs, the call is NOT sent: TestTimeout naming it, the
        connection and the hold kept (was max(0.2, left): a bare socket timeout that let the game run free)."""
        left = self.deadline.left()
        if left < need:
            raise TestTimeout("timeout after %.0f s: %s not sent (%.1f s left, its reply needs %.1f s; the game stays "
                              "held)" % (self.deadline.seconds, cmd, max(0.0, left), need))
        return min(max(60.0, need), left)

    def call(self, x, cmd, **args):
        self.deadline.check(cmd)
        wait = self._wait(cmd, self._need(cmd, args.get("frames")))
        return self._request(x, cmd, wait, **args)

    def _request(self, x, cmd, wait, **args):
        """One request; if its reply does not come (a socket timeout, a dropped connection), game x is held again at
        once before the error goes on (a reconnect holds: playtest_client hold=True), so no game runs free until the
        next test's recovery (B #138: re-hold after any step timeout)."""
        try:
            return self.c[x].request(cmd, reply_timeout=wait, **args)
        except (OSError, ValueError) as e:
            self.rehold(x, "%s: %s: %s" % (cmd, type(e).__name__, e))
            raise

    def rehold(self, x, why):
        """Game x held again after a failed request (its connection is closed: the next request reconnects and holds).
        Noted; a failed re-hold is noted too (the next test's recovery reconnects anyway, and SETUP-FAILs if it cannot)."""
        client = self.c[x]
        try:
            client.request("pause", reply_timeout=REPLY_MIN_S)
            self.notes.append("%s: held again after %s" % (x, why))
        except Exception as e:                 # noqa: BLE001
            self.notes.append("%s: NOT held again after %s: %s: %s" % (x, why, type(e).__name__, e))

    def step_both(self, frames=1):
        """Both games run `frames` frames, A first then B, in chunks (no game is ever more than CHUNK ahead)."""
        left = int(frames)
        while left > 0:
            n = min(CHUNK, left)
            for x in ("A", "B"):
                if x in self.c:
                    self.call(x, "step", frames=n)
            left -= n

    def press(self, x, buttons=(), stick=None, frames=8, rstick=None):
        """One game's pad for `frames` frames through the bridge's `input`; the other game runs the same frames."""
        args = {"buttons": list(buttons), "frames": int(frames)}
        if stick is not None:
            args["stick"] = list(stick)
        if rstick is not None:
            args["rstick"] = list(rstick)
        r = self.call(x, "input", **args)
        for y in self.c:
            if y != x:
                self.call(y, "step", frames=int(frames))
        return r

    def state(self, x):
        return self.call(x, "state")

    def until(self, what, pred, max_frames=600):
        """Steps both games CHUNK frames at a time until pred() is true (checked before the first step too).
        Returns its value; AssertionError naming `what` after max_frames frames."""
        ran = 0
        while True:
            v = pred()
            if v:
                return v
            if ran >= max_frames:
                raise AssertionError("%s: not after %d frames (~%d s of game)" % (what, ran, ran // FPS))
            self.step_both(CHUNK)
            ran += CHUNK

    def ocarina_ready(self, x):
        """True once game x's module log has OCARINA_EQUIPPED after its last SCENE_LOADED (the boot's or a warp's)."""
        lines = self.log[x].since(0)
        load = re.compile(SCENE_LOADED)
        last = max([i for i, ln in enumerate(lines) if load.search(ln)] or [-1])
        equip = re.compile(OCARINA_EQUIPPED)
        return any(equip.search(ln) for ln in lines[last + 1:])

    def need_ocarina(self, x, max_frames=OCARINA_FRAMES):
        """THE precondition of every D-pad ocarina press (call it inside `with pair.setup():` before the press, after
        the boot and after every warp): steps both games until game x's ocarina is equipped again since its last scene
        load (ocarina_ready), else AssertionError naming it (a SETUP-FAIL in a set-up). Without it the press raced the
        module's re-equip (songs-frogs-733c4d8f)."""
        return self.until("%s: the ocarina back on the D-pad after the load ('%s' since the last '%s')"
                          % (x, OCARINA_EQUIPPED, SCENE_LOADED), lambda: self.ocarina_ready(x), max_frames)

    def ocarina_out(self, x, max_frames=OCARINA_PLAYS_FRAMES):
        """THE way to take game x's ocarina out (test_songs, test_epona, test_rows; needs LICHII_COOP_OCARINA_PROBE=1):
        need_ocarina, D-pad Left (12 frames), then OCARINA_PLAYS within max_frames: done. Else OCARINA_FREE must be
        there (else AssertionError: nothing came out, the #190 bug's sign): the first-use box is up, B closes it (the
        probe's proven press; not A, a note key), and OCARINA_PLAYS must come AFTER the B press (the log is marked right
        before it), within max_frames. Ends with Link playing (no note sent by the set-up)."""
        self.need_ocarina(x)
        pos = self.log[x].mark()
        self.press(x, ["LEFT"], frames=12)
        try:
            return self.until("%s: '%s' after D-pad Left" % (x, OCARINA_PLAYS),
                              lambda: self.log[x].find(OCARINA_PLAYS, pos=pos), max_frames)
        except AssertionError:
            pass
        assert self.log[x].find(OCARINA_FREE, pos=pos), (
            "%s: nothing came out after D-pad Left: neither '%s' nor '%s' in %d frames"
            % (x, OCARINA_PLAYS, OCARINA_FREE, max_frames))
        self.notes.append("%s: ocarina out with the first-use 'Ocarina Audio Input' box up: closed with B" % x)
        at_b = self.log[x].mark()
        self.press(x, ["B"], frames=8)
        return self.until("%s: '%s' after B closed the first-use box" % (x, OCARINA_PLAYS),
                          lambda: self.log[x].find(OCARINA_PLAYS, pos=at_b), max_frames)

    def expect_log(self, x, pattern, max_frames=600):
        """A module log line of game x matching `pattern`, written since this test began."""
        return self.until("%s log /%s/" % (x, pattern), lambda: self.log[x].find(pattern), max_frames)

    def never_log(self, x, pattern):
        line = self.log[x].find(pattern)
        assert line is None, "%s log has /%s/: %s" % (x, pattern, line)

    # -- set-up -----------------------------------------------------------------------------------------------
    def in_play(self, st):
        return bool(st.get("scene")) and not st.get("loading") and not st.get("event") and st.get("not_ready") is None

    def why_not(self, x, st=None):
        """Game x's state in one line: what in_play looks at (for a FAIL / SETUP-FAIL that names each game)."""
        st = self.state(x) if st is None else st
        return "%s: scene=%s loading=%s event=%s not_ready=%s" % (x, st.get("scene"), st.get("loading"), st.get("event"),
                                                               st.get("not_ready"))

    def until_in_play(self, what, games=None, max_frames=600):
        """Steps both games until every one of `games` is in play (a fresh Link sample, a scene, no load, no event).
        `games` defaults to the pair's OWN games (self.c): a --solo pair has game A only, and a fixed ('A', 'B')
        read state('B') -> KeyError instead of a SETUP-FAIL (review-rows-c130-3 16:38, need_boot_place).
        A stale sample ('Link sample older than the frame'), a load or an event is a game not ready YET: it is stepped,
        never judged from one read (pilot test-bridge-pilot-fa63e90f: test_boot / test_pause read once, failed on a
        stale sample). After max_frames: AssertionError naming each game's state."""
        games = tuple(self.c) if games is None else tuple(games)
        try:
            return self.until(what, lambda: all(self.in_play(self.state(x)) for x in games), max_frames)
        except AssertionError as e:
            raise AssertionError("%s; %s" % (e, "; ".join(self.why_not(x) for x in games))) from None

    def boot_ready(self, scene=None, max_frames=BOOT_FRAMES):
        """The lane's boot proof (bt_plugin.lane_pair, NO REPEAT VOIDS after test-bridge-pilot-fa63e90f r1: B was left
        in an event in Graveyard_Child, the save's place, its boot warp never taken): each game is stepped until it is
        in play; a game in another scene than the boot's `scene` (its scripts' warp line) is warped there by the bridge
        (warp_one: during an event the module's own request, in free play Longshot's teleport); then both must be in
        play, no event, in `scene`. Anything else raises SetupFail('boot: ...') naming each game, so every test of
        the lane is SETUP-FAIL boot and nothing runs on a pair that is not where the tests assume."""
        try:
            self.until("both games out of the boot's load",
                       lambda: all(_loaded(self.state(x)) for x in self.c),
                       max_frames)
            for x in self.c:
                have = (self.state(x).get("scene") or "")
                if scene and have.lower() != scene.lower():
                    self.notes.append("boot: %s in %s, not the boot's %s: warped there by the bridge" % (x, have, scene))
                    # (arrive_in_event=None: a boss stage boot, live-twinrova-room / live-phantom-ganon, starts its intro
                    # on arrival, so the bridge answers 'timeout' with Link there; the event's end is stepped below)
                    self.warp_one(x, scene, arrive_in_event=None)
            self.until_in_play("both games in play after the boot", tuple(self.c), max_frames)
        except SetupFail:
            raise
        except (AssertionError, pc.BridgeError, OSError, TimeoutError) as e:
            raise SetupFail("boot: %s: %s" % (type(e).__name__, e)) from e
        wrong = [self.why_not(x) for x in self.c
                 if scene and (self.state(x).get("scene") or "").lower() != scene.lower()]
        if wrong:
            raise SetupFail("boot: not in the boot's scene %s: %s" % (scene, "; ".join(wrong)))
        return {x: self.state(x) for x in self.c}

    def module_started(self, seconds=MODULE_WAIT_S, clock=time.monotonic, sleep=time.sleep):
        """Each game's log has the module's 'coop: playtest: listening on 127.0.0.1:<its port>' line
        (hookshot/src/coop_playtest.cpp:836), else SetupFail naming the game: its module never started, so nothing
        a test sends can reach it. party-hud-bridge-85e3b255 (B #137 14:57): B's Longshot start-up never ended, no
        'coop:' line at all, yet live.py start said ok and test_boot died on ConnectionRefusedError after 336 s."""
        end = clock() + seconds
        for x, g in self.games.items():
            rx = r"coop: playtest: listening on 127\.0\.0\.1:%d(?!\d)" % int(g["port"])   # (not :50210 for :5021)
            while not self.log[x].find(rx, pos=0):
                if clock() >= end:
                    tail = self.log[x].since(0)[-1:] or ["(empty log)"]
                    raise SetupFail("%s: the module never started (no 'coop: playtest: listening on 127.0.0.1:%d' in %s; "
                                    "last line: %s)" % (x, int(g["port"]), g["log"], tail[0][:200]))
                sleep(0.5)
        return True

    def away_from_boot(self, seconds=AWAY_CHECK_S):
        """[why] for each game NOT at the boot's place (in play, in self.boot_scene), one state read each, on its own
        short deadline (the test's may be used up). [] when both are there, or no boot scene is known. A read that
        fails counts as away (the recovery is safe; a missed one poisons every later test: row 285 levitate)."""
        if not self.boot_scene:
            return []
        outer, out = self.deadline, []
        self.deadline = Deadline(seconds, clock=outer.clock)
        try:
            for x in self.c:
                try:
                    st = self.state(x)
                except Exception as e:
                    out.append("%s: no state (%s: %s)" % (x, type(e).__name__, e))
                    continue
                if (st.get("scene") or "").lower() != self.boot_scene.lower() or not self.in_play(st):
                    out.append("%s (age %s), not in play in the boot's %s" % (self.why_not(x, st), st.get("age"),
                                                                             self.boot_scene))
        finally:
            self.deadline = outer
        return out

    def need_boot_place(self, age=None, max_frames=READY_FRAMES):
        """THE default-boot precondition (call it inside `with pair.setup():`): both games in play (stepped, never one
        read), in the boot's scene, and of `age` ('child'/'adult') when given. Checking the age alone let a test run in
        whatever map the test before left (row 285 test_levitate stayed in the Deku Tree; review-rows-c130-2 14:38).
        The pair fixture already recovers a pair left away by a test that passed (away_from_boot after every test), so
        a miss here means that recovery did not hold: SetupFail naming each game."""
        self.until_in_play("both games in play before the test", max_frames=max_frames)
        bad = []
        for x in self.c:
            st = self.state(x)
            if self.boot_scene and (st.get("scene") or "").lower() != self.boot_scene.lower():
                bad.append("%s is in %s, not the boot's %s" % (x, st.get("scene"), self.boot_scene))
            if age and st.get("age") != age:
                bad.append("%s is %s, this test needs %s Link" % (x, st.get("age"), age))
        if bad:
            raise SetupFail("not at the boot's place: %s" % "; ".join(bad))
        return {x: self.state(x) for x in self.c}

    def recover(self, scene=None, max_frames=READY_FRAMES * 2):
        """The lane's recovery after a test that did not pass or left play (bt_plugin's pair fixture): boot_ready
        again, on game STATE. It is the lane's boot proof run again, not a test's set-up, so the 30 s set-up rule does
        not bound it; its bound is RECOVER_BASE_S plus RECOVER_WARP_S for each game the state shows away from `scene`
        (the only games boot_ready warps). Was one fixed 30 s for everything: two warps measure 27-31 s, so a
        recovery that warped both games timed out 3/3 (party-hud-bridge-6947aa3d). Every frame wait stays frame-
        bounded (max_frames); every bridge wait stays inside this bound. Anything wrong: SetupFail('recovery: ...')
        or boot_ready's SetupFail('boot: ...'). Returns boot_ready's states; notes the time and the bound."""
        outer = self.deadline
        clock = outer.clock
        t0 = clock()
        limit = RECOVER_BASE_S
        self.deadline = Deadline(max(0.0, min(limit, outer.left())), clock=clock)
        try:
            try:
                self.until("both games out of a load before the recovery",
                           lambda: all(_loaded(self.state(x)) for x in self.c), max_frames)
                away = [x for x in self.c
                        if scene and (self.state(x).get("scene") or "").lower() != scene.lower()]
            except SetupFail:
                raise
            except Exception as e:
                raise SetupFail("recovery: %s: %s" % (type(e).__name__, e)) from e
            limit = RECOVER_BASE_S + RECOVER_WARP_S * len(away)
            self.deadline = Deadline(max(0.0, min(limit - (clock() - t0), outer.left())), clock=clock)
            try:
                st = self.boot_ready(scene, max_frames)
            except SetupFail:
                raise
            except Exception as e:
                raise SetupFail("recovery: %s: %s" % (type(e).__name__, e)) from e
        finally:
            self.deadline = outer
        self.notes.append("recovery %.1f s (bound %.0f s, %d warp%s)" % (clock() - t0, limit, len(away),
                                                                        "" if len(away) == 1 else "s"))
        return st

    def load_both(self, at_title, max_frames=3600):
        """Title / file select to play: A pressed 8 frames, then 52 frames of wait (the runner's to_play,
        tools/autotest/hookshot.py, in frames: A every 2 s there), ONLY on a game that at_title(x) says is at the title
        and is not in play. A game in play is never pressed: an A there opens and shuts the menu (pilot
        test-bridge-pilot-fa63e90f game-out-A.txt :7015, a game in play cycled play -> menu -> play). A game neither
        in play nor at the title (a load, a stale sample) is only stepped."""
        ran = 0
        while True:
            todo = [x for x in self.c if not self.in_play(self.state(x))]
            if not todo:
                return
            if ran >= max_frames:
                raise AssertionError("not in play after %d frames: %s" % (ran, "; ".join(
                    "%s%s" % (self.why_not(x), "" if at_title(x) else " (never at the title)") for x in todo)))
            press = [x for x in todo if at_title(x)]
            for x in press:
                self.call(x, "input", buttons=["A"], frames=8)
            for x in self.c:
                if x not in press:
                    self.call(x, "step", frames=8)
            self.step_both(52)
            ran += 60

    TELEPORT_GAP_S = 8      # s: Longshot's one teleport per 8 s per game (docs/playtest.md `warp`, 'still in progress')
    ARRIVE_IN_EVENT_WAIT = 12   # s: the bridge's own wait per game for arrive_in_event; it answers 'timeout' in the
    #                             arrival event, then the arrival is waited for on state (warp_one), not judged here

    def warp_both(self, scene, entrance=None, arrive_in_event=False):
        """Both games to `scene` (the bridge's `warp`: Longshot's teleport in free play; with an `entrance` (a start of
        that scene) the game's map change event, which also takes a boss stage: coop_playtest_core.cpp PlanWarp). A
        first, then B; each warp runs its own game only, so the other is stepped to the same frame count after it.
        (entrance=: from boss-respawn 6438836b4, taken by the file's owner.)
        arrive_in_event (twinrova-shots-222-5): a stage whose arrival starts an event (a boss intro). The bridge's warp
        waits for Link in the scene with NO event (coop_playtest_core.cpp WarpArrived), so it answers 'timeout' with
        Link already there; then a fresh state with that scene, no load AND an event running counts as the arrival,
        anything else raises. The bridge waits ARRIVE_IN_EVENT_WAIT s per game; a game still loading then is stepped
        until that state (frame-bounded), and every wait stays inside the deadline."""
        for x in tuple(self.c):            # (the pair's own games: a --solo pair has A only)
            self.warp_one(x, scene, entrance, arrive_in_event)
        return {x: self.state(x) for x in self.c}

    def warp_one(self, x, scene, entrance=None, arrive_in_event=False):
        """Game x alone to `scene`, by warp_both's rules (entrance, arrive_in_event, waits inside the deadline); the
        other game is stepped to the same frame count after it (row-286-wake-3). Returns x's state.
        arrive_in_event=None (boot_ready: a boot's scene may or may not start an event on arrival): the bridge's own
        60 s wait, and a 'timeout' is judged on state like arrive_in_event=True (in the scene, not loading, the event
        running), anything else raises."""
        extra = {} if entrance in (None, "") else {"entrance": entrance}
        before = self.c[x].last_frame or 0
        wait = self.ARRIVE_IN_EVENT_WAIT if arrive_in_event is True else 60
        for attempt in (1, 2):
            t = max(1, int(min(wait, self.deadline.left() - 2)))   # (the bridge answers by t, a little under the deadline)
            self.deadline.check("warp")
            r = self._request(x, "warp", self._wait("warp", t + REPLY_MIN_S), check=False,
                              scene=scene, timeout=t, **extra)        # (the bridge answers by t; never sent short of it)
            if attempt == 1 and r.get("error") == "failed" and "still in progress" in (r.get("message") or ""):
                # (Longshot allows one teleport every TELEPORT_GAP_S per game, docs/playtest.md `warp`: 'another
                # teleport is still in progress'. After a game ran free mid-teleport (boss-respawn-bridge-e3e6e253's
                # recoveries) the gap is waited out, game x stepped, and the warp asked once more)
                self.notes.append("%s: warp %s: %s; the gap waited out, asked again" % (x, scene, r.get("message")))
                gap_end = self.deadline.clock() + self.TELEPORT_GAP_S
                while self.deadline.clock() < gap_end:
                    self.call(x, "step", frames=CHUNK)
                continue
            break
        if not r.get("ok"):
            if not (arrive_in_event is not False and r.get("error") == "timeout"):
                raise pc.BridgeError(r)
            # (the bridge's wait is over, not the warp: a warp measures 13.5-15.5 s per game, WARP_PAIR_MEASURED_S,
            # more than ARRIVE_IN_EVENT_WAIT; so the arrival is waited for on STATE, stepped, inside the deadline;
            # review-c123-8: in the scene, not loading AND the event running, never a timeout with no event)
            ran_wait = 0
            while True:
                st = self.state(x)
                if ((st.get("scene") or "").lower() == scene.lower() and not st.get("loading")
                        and st.get("event") is True):
                    break
                if ran_wait >= BOOT_FRAMES:      # (the frame ceiling of a load, as the boot proof's; the deadline first)
                    raise pc.BridgeError(dict(r, message="%s; then %d frames: %s"
                                              % (r.get("message"), ran_wait, self.why_not(x, st))))
                self.call(x, "step", frames=CHUNK)     # (game x only: the other is stepped to x's frames below)
                ran_wait += CHUNK
            self.notes.append("%s: warp %s answered timeout in the arrival event; in the scene, not loading: "
                              "arrived (event %s)" % (x, scene, st.get("event")))
        ran = max(0, (self.c[x].last_frame or before) - before)
        for y in self.c:
            if y != x and ran:
                self.step_other(y, ran)
        return self.state(x)

    def step_other(self, y, frames):
        left = int(frames)
        while left > 0:
            n = min(pc.MAX_FRAMES, left)
            self.call(y, "step", frames=n)
            left -= n

    def act(self, x, line):
        return self.call(x, "act", lines=line)

    def give(self, x, item):
        return self.call(x, "give", item=item)

    def flag(self, x, name, value=1):
        return self.call(x, "flag", name=name, value=value)

    def flag_read(self, x, name):
        return self.call(x, "flag_read", name=name)

    # -- looks ------------------------------------------------------------------------------------------------
    def check_shot(self, x, name, tolerance=0.90):
        """A bridge shot of game x (the game's framebuffer, no overlay), compared to refs/<name>-<x>.png by SSIM.
        No reference yet: the shot is kept as <out>/new/<name>-<x>.png, the test goes on, and its result is NO-REF (an
        owner-approved reference is copied into refs/ by hand; never made a pass by the test itself)."""
        import imagecheck   # (Pillow + scikit-image: only tests with a look check need them)
        r = self.call(x, "shot", name="%s-%s" % (name, x))
        path = r.get("path")
        ref = os.path.join(self.refs_dir or os.path.join(HERE, "refs"), "%s-%s.png" % (name, x))
        res = imagecheck.compare(path, ref, tolerance, keep_dir=self.out_dir)
        self.shots.append(dict(res, game=x, name=name))
        if res["why"].startswith("NO-REF"):
            return res        # (the test goes on; bt_plugin labels it NO-REF at its end, never PASS)
        assert res["ok"], "shot %s-%s: %s" % (name, x, res["why"])
        return res

    def unproven(self, reason):
        """Ends the test as NO-REF (never PASS, never FAIL): the row cannot be proven by this run (no measured value
        yet, a probe build, ...). `reason` goes into the summary's why."""
        raise NoRef("NO-REF: %s" % reason)

    def no_ref(self):
        return ["%s-%s" % (s["name"], s["game"]) for s in self.shots if s["why"].startswith("NO-REF")]


def read_session(session_dir):
    """The games of a live.py session (session.json): ports, folders, logs."""
    with open(os.path.join(session_dir, "session.json"), encoding="utf-8") as f:
        s = json.load(f)
    out = {}
    for x, g in s["games"].items():
        if not g.get("playtest_port"):
            raise SetupFail("session %s game %s has no play-test port (live.py start without --playtest)" % (session_dir, x))
        out[x] = {"port": g["playtest_port"], "folder": g["folder"], "log": g["log"]}
    return out
