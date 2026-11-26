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
READY_FRAMES = 300    # a set-up's step-until-in-play ceiling (10 s of game: a stale sample, an event's end; inside 30 s)
FPS = 30              # Ocarina of Time's calc rate on Hookshot: frames -> seconds in messages only, never for waiting
# A warp's wall time is the game's (a load), not ours: measured, not guessed. warp_both (A then B, Longshot's teleport)
# took 26.9 / 28.8 / 31.0 s on a full lane 2 (party-hud-bridge-6947aa3d summary.md test_warp r1-r3), i.e. one game's
# warp 13.5-15.5 s. So a set-up that warps cannot be proven in SETUP_LIMIT_S (the cause of that run's 3 SETUP-FAILs).
WARP_PAIR_MEASURED_S = 31.0
RECOVER_WARP_S = WARP_PAIR_MEASURED_S   # one game's warp in a recovery: the worst TWO-game warp = 2x one game's (margin)
RECOVER_BASE_S = SETUP_LIMIT_S          # a recovery's other part: out of a load, an event's end, in play (stepped)


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


def lane_check(kit_ls, lane):
    """The lane's setup proof, the Python twin of offline/lib/lane-server.ps1 + lane-pair.ps1 (one source of the rule:
    both read lane-server.txt as offline/lane-game-for.ps1 writes it). Returns {server, commit, dll_sha256}; raises
    SetupFail with the first thing wrong."""
    got = {}
    for x in ("A", "B"):
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
        got[x] = {"server": srv, "commit": commit, "dll_sha256": want, "patches": want_p}
    a, b = got["A"], got["B"]
    if a["dll_sha256"] != b["dll_sha256"]:
        raise SetupFail("lane%d A/B module differ: %s vs %s" % (lane, a["dll_sha256"][:16], b["dll_sha256"][:16]))
    if a["patches"] != b["patches"]:
        raise SetupFail("lane%d A and B games come from different patches builds" % lane)
    if a["server"] != b["server"]:
        raise SetupFail("lane%d A and B name different servers (%s / %s)" % (lane, a["server"], b["server"]))
    if not os.path.exists(a["server"]):
        raise SetupFail("no server %s (lane%d lane-server.txt)" % (a["server"], lane))
    return a


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
    def _wait(self):
        """A reply's wait: what is left of the deadline (at most 60 s, at least 0.2 s so a call can still answer)."""
        return max(0.2, min(60.0, self.deadline.left()))

    def call(self, x, cmd, **args):
        self.deadline.check(cmd)
        return self.c[x].request(cmd, reply_timeout=self._wait(), **args)

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

    def until_in_play(self, what, games=("A", "B"), max_frames=600):
        """Steps both games until every one of `games` is in play (a fresh Link sample, a scene, no load, no event).
        A stale sample ('Link sample older than the frame'), a load or an event is a game not ready YET: it is stepped,
        never judged from one read (pilot test-bridge-pilot-fa63e90f: test_boot / test_pause read once, failed on a
        stale sample). After max_frames: AssertionError naming each game's state."""
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
                    self.warp_one(x, scene)
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
        for x in ("A", "B"):
            self.warp_one(x, scene, entrance, arrive_in_event)
        return {x: self.state(x) for x in self.c}

    def warp_one(self, x, scene, entrance=None, arrive_in_event=False):
        """Game x alone to `scene`, by warp_both's rules (entrance, arrive_in_event, waits inside the deadline); the
        other game is stepped to the same frame count after it (row-286-wake-3). Returns x's state."""
        extra = {} if entrance in (None, "") else {"entrance": entrance}
        before = self.c[x].last_frame or 0
        wait = self.ARRIVE_IN_EVENT_WAIT if arrive_in_event else 60
        t = max(1, int(min(wait, self.deadline.left() - 2)))   # (the bridge answers by t, a little under the deadline)
        self.deadline.check("warp")
        r = self.c[x].request("warp", check=False, reply_timeout=max(0.2, min(t + 15, self.deadline.left())),
                              scene=scene, timeout=t, **extra)        # (the reply wait stays inside the deadline)
        if not r.get("ok"):
            if not (arrive_in_event and r.get("error") == "timeout"):
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
