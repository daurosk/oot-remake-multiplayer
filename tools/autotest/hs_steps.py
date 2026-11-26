"""Steps files (scenarios/<name>.steps.txt) on Hookshot: the same lines tools/autotest/run.ps1 plays on the emulator.

    python -I tools/autotest/hs_steps.py check <name>.steps.txt...              (parse, print the plan)
    python -I tools/autotest/hs_steps.py run --steps <file> --game-a DIR --game-b DIR --server EXE --out DIR
                                          [--port N] [--pipe NAME] [--env NAME=VALUE]... [--dry-run]

run.ps1 -Runtime hookshot calls `run` when the scenario is a steps file. The verbs (run.ps1's Run-Steps):
  start A [B]           boot the games named (again: a running one is ended first), with the save baseline put back
                        and a fresh longshot_log.txt (the old one kept as hookshot_log-<x>-<n>.txt); wait until each
                        logs "coop: welcomed as player"
  start A [B] idle      the same with no server for them (no LICHII_COOP_SERVER, co-op idle at the title, as a player
                        who has not hosted or joined yet: the Host page before hosting); waits for the renderer only
  load A [B]            title to the save in each (hookshot.py to_play); the `#! start` regex checked after it
  play A f [B f]        play scenarios/<f> in A (and B), both at once, and wait for both
  close B / kill B      B quits through its control bridge (it leaves the server) / B ends at once (a crash)
  freeze B / thaw B     B's process is suspended / goes on (Windows): a frozen game sends and retries nothing, so
                        after `server start` the other game gets back in (and hosts) first; every game is thawed
                        before it is ended
  server stop|start     the server goes away / comes back on the same port (server-out[-n].txt)
  sleep S               seconds
  password B TEXT       the join password game B sends from its next start (LICHII_COOP_PASSWORD; - for none);
                        without a password line a game sends the server's (OOT_COOP_TEST_PASSWORD)
  try B welcomed|refused|none   start B without waiting and see within 40 s: welcomed (its log), refused (a new
                        "refused a connection" in the server log) or none; then end B
  expect NAME A=1 B=0 [see=CHECK...]   wait up to --expect-wait s until each game named has that many puppets
                        (CoopPuppet_*) and as many fairies (Dm_Npc_Navi) alive, from the objects debug lines; then shot
                        NAME (s<step>-NAME-A/-B.png) and grade its see= checks (tools/autotest/proofs.txt, proof.py)
  see CHECK [CHECK...]  grade screenshot checks on the shots taken so far (after a `shot`)
  shot NAME             a screenshot of every running game
  lines NAME B max=N|min=N REGEX   game B's log has at most / at least N lines matching REGEX
  key A F1              a key press posted to game A's window (WM_KEYDOWN, WM_KEYUP: Longshot's wndproc, patch 0001,
                        hands it to hos_coop_key); proven landed by the module's line for it (KEYS_POSTED) within
                        10 s, else SETUP-FAIL and the run stops (Windows only)
  state A event|free|title|load [S]   game A's last "coop: free play: ..." line (the module's, at each change)
                        says that state within S s (default 60), else SETUP-FAIL and the run stops: a run that
                        relies on a state proves it before it acts on it
  flag A key64:0xKEY|bool:0xHASH 0|1 [S]   the expect-state gate: a new read of that flag in game A's log (the
                        module's watch line, FLAG_WATCH; the header must switch the watch on for A) says the value
                        within S s (default 30), else SETUP-FAIL: a scenario that relies on save state proves it
  wshot NAME            a WINDOW shot of every running game (s<step>-NAME-A/-B.png) through the helper in
                        OOT_WINDOW_SHOT: Longshot's overlays (its menu) included, which `shot` never has; no helper,
                        a covered or failed shot = SETUP-FAIL
Results go to counts.txt (PASS/FAIL/UNPROVEN lines as run.ps1 writes them); a step that cannot run is a FAIL there too.
Owner rule (2026-11-18), no PASS from log lines alone: puppets and fairies are visible, so an expect step whose
counts match is PASS only when its see= checks are all OK (FAIL when a shot shows the wrong result), else UNPROVEN.
Log-only steps by nature: `try` (a join's outcome: a connection result) and `lines` (counts log lines); a run made of
them alone still needs its group line's see= or logonly= (groups.py).
The server's join password comes from the environment variable OOT_COOP_TEST_PASSWORD (run.ps1 -ServerPassword).
Unset, `run` makes a random one for the run (ensure_server_password) and gives it to the server and to every game: every
session has a join password (coop_server does not start without one; a game without the server's is refused).
"""
import argparse
import os
import re
import secrets
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lanes  # noqa: E402  (brings hs.py: run_quiet and popen_quiet start a console child with no window)

HERE = os.path.dirname(os.path.abspath(__file__))
WINDOWS = os.name == "nt"
WANT_TRY = ("welcomed", "refused", "none")
JOINED = "coop: welcomed as player"
# hookshot.py TEST_GAME_DEFAULTS (the same pairs; tests/test_hs_steps.py checks): each game its own save, and no
# "The game crashed last time" page on the title to take the load's A presses.
TEST_GAME_DEFAULTS = (("LICHII_COOP_TEST_OWN_SAVE", "1"), ("LICHII_COOP_PROBLEM_ASK", "0"))
LIVE = re.compile(r"coop: objects: ([+-]) (CoopPuppet_\w+|Dm_Npc_Navi) (0x[0-9a-f]+)")
# Lines the module says once per game process, checked in every game's log after the steps (row 267: the list
# head was said at every object scan, 43% of a tester's 190 MB log). A game process starts with "coop: build".
ONCE_PER_PROCESS = (("objects list head (row 267)", re.compile(r"(?m)^.*coop: objects: list head ")),)
PROCESS_START = re.compile(r"(?m)^.*coop: build ")


# The keys `key` can post, each with its Windows virtual-key and scan code and the module line that proves it landed:
# F1, hookshot/src/coop_hookshot.cpp hos_coop_key (vk == 0x70), which logs "coop: F1: <its route>" for each press
# (f1-event-c31). Scan code 0x3B: F1's set-1 make code. Only keys hos_coop_key logs belong here (F6 is read by
# GetAsyncKeyState in the module, so a posted message never reaches it).
KEYS_POSTED = {"F1": (0x70, 0x3B, r"coop: F1: ")}
KEY_LAND_WAIT = 10.0
# `state`: the game's play state as the module logs each change of it (hookshot/src/coop_hookshot.cpp, the free play
# block of the frame: "coop: free play: yes|no (event)|no (title)|no (load, no Link)"), the last such line counted.
STATE_LINE = re.compile(r"(?m)^.*coop: free play: (.+?)\s*$")
STATES = {"free": "yes", "event": "no (event)", "title": "no (title)", "load": "no (load, no Link)"}
STATE_WAIT = 60.0


# `flag` (f1-event-c124, the shared expect-state gate): a scenario that relies on save state (a chest still closed,
# an event not played yet) proves it before it acts, instead of trusting the lane's save (run f1-event-aebd16089
# VOID: the slingshot chest was already open there). The value is the module's own read of the game's flag store:
# lichii/hook/coop_flags.cpp FlagDebug::Frame logs "coop: flags debug: get_key64(0x...) = 0|1|none" (switch
# LICHII_COOP_DEBUG_KEY64=0xKEY) or "get_bool(0x........) = ..." (LICHII_COOP_DEBUG_BOOL=0xHASH) at each change and
# every 5 s. One key and one bool per game: the header's switch for that game must name it (flag_watch_problems).
FLAG_WATCH = {"key64": ("LICHII_COOP_DEBUG_KEY64", "get_key64"), "bool": ("LICHII_COOP_DEBUG_BOOL", "get_bool")}
FLAG_WAIT = 30.0
WINDOW_SHOT_ENV = "OOT_WINDOW_SHOT"   # `wshot`: the ONE window-shot helper (coord-multiplayer offline/lib/screen-shot.ps1)


def _ps_quote(text):
    return "'" + str(text).replace("'", "''") + "'"


def window_shot_command(helper, pid, path, log):
    """powershell dot-sources the shared helper and calls its Save-WindowShot (raise + 9-point own-pid cover check).
    Exit 0 saved; 3 covered (saved as <name>-COVERED.png); 4 no window / failed; 2 no such process."""
    script = ("$ErrorActionPreference = 'Stop'; . %s; $p = Get-Process -Id %d -ErrorAction SilentlyContinue; "
              "if (-not $p) { 'no process %d'; exit 2 }; $s = Save-WindowShot -Process $p -Path %s -Log %s; "
              "if ($null -eq $s) { 'no window handle, or under 100 px'; exit 4 }; "
              "if ($s.Failed) { 'FAILED ' + $s.Path + ': ' + $s.Why; exit 4 }; "
              "if ($s.Covered) { 'COVERED ' + $s.Path + ': ' + $s.Why; exit 3 }; 'saved ' + $s.Path; exit 0"
              % (_ps_quote(helper), pid, pid, _ps_quote(path), _ps_quote(log)))
    return ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script]


WINDOW_SHOT_WAIT = 90


def run_window_shot(cmd, run=subprocess.run, wait=WINDOW_SHOT_WAIT):
    """(exit code, output on one line). A helper that hangs past `wait` s (subprocess.run kills it) or cannot start is
    a SETUP-FAIL with its reason, never an exception out of the steps run (review-c124 on 8614cb1cc)."""
    try:
        r = run(cmd, capture_output=True, text=True, timeout=wait)
    except subprocess.TimeoutExpired:
        raise SetupFail("the window-shot helper did not end within %d s (ended)" % wait)
    except OSError as e:
        raise SetupFail("the window-shot helper did not start: %s" % e)
    return r.returncode, ((r.stdout or "") + (r.stderr or "")).strip().replace("\n", " | ")


def flag_watch(name):
    """'key64:0xKEY' / 'bool:0xHASH' -> (kind, number), else None (a bool hash fits 32 bits, neither is 0)."""
    m = re.match(r"^(key64|bool):0[xX]([0-9a-fA-F]{1,16})$", name or "")
    if not m:
        return None
    value = int(m.group(2), 16)
    if value == 0 or (m.group(1) == "bool" and value > 0xFFFFFFFF):
        return None
    return m.group(1), value


def flag_reads(text, kind, number):
    """The values ('0', '1', 'none') the module logged for that watched flag, in log order."""
    fn = FLAG_WATCH[kind][1]
    out = []
    for m in re.finditer(r"coop: flags debug: %s\((0x[0-9a-fA-F]+)\) = (\w+)" % fn, text or ""):
        if int(m.group(1), 16) == number:
            out.append(m.group(2))
    return out


def flag_watch_problems(steps, head):
    """Each `flag` step whose game's switches (the `#! switches` header, 'NAME=V' or 'A:NAME=V') do not watch that
    flag: the module would never log it, so the gate could only time out. Checked by `check` and before a run."""
    given = {}
    for sw in head.get("switches", []):
        who, _, kv = sw.rpartition(":") if re.match(r"^[AB]:", sw) else ("", "", sw)
        k, _, v = kv.partition("=")
        given.setdefault(who, {})[k.strip()] = v.strip()
    bad = []
    for s in steps:
        if s.verb != "flag":
            continue
        x, name = s.args[0], s.args[1]
        kind, number = flag_watch(name)
        env = FLAG_WATCH[kind][0]
        v = given.get(x, {}).get(env, given.get("", {}).get(env))
        try:
            ok = v is not None and int(v, 16) == number
        except ValueError:
            ok = False
        if not ok:
            bad.append("line %d: flag %s %s needs '#! switches %s:%s=%s' (the module logs only the watched one)"
                       % (s.n, x, name, x, env, name.split(":", 1)[1]))
    return bad


def last_state(text):
    """The last play state the module logged ("yes", "no (event)", ...), or None."""
    found = STATE_LINE.findall(text or "")
    return found[-1] if found else None


class SetupFail(Exception):
    """A setup check failed: the run stops (SETUP-FAIL in counts.txt), nothing after it is graded."""


class StepsError(Exception):
    pass


class Step:
    def __init__(self, n, verb, args, line, pattern=None, idle=False):
        self.n, self.verb, self.args, self.line, self.pattern = n, verb, args, line, pattern
        self.idle = idle   # start ... idle: no server for these games

    def __repr__(self):
        return "Step(%d, %r, %r)" % (self.n, self.verb, self.args)


def ensure_server_password(environ):
    """The run's join password in environ["OOT_COOP_TEST_PASSWORD"]: kept when set (run.ps1 -ServerPassword, a
    wrapper's own), else a random one made here, so the server (--password-env) and the games (Runner's default
    password, LICHII_COOP_PASSWORD) always get the same one. A wrapper that called `run` without it started a server
    with no password, which coop_server refuses (rejoin-host-273's harness gaps). True when one was made."""
    if environ.get("OOT_COOP_TEST_PASSWORD"):
        return False
    environ["OOT_COOP_TEST_PASSWORD"] = "autotest-" + secrets.token_hex(8)
    return True


def _game(n, x):
    if x not in ("A", "B"):
        raise StepsError("line %d: %r is not a game (A or B)" % (n, x))
    return x


def parse_steps(text):
    """The steps of a steps file, checked as run.ps1 would run them (a `#` starts a comment, `#!` a header)."""
    steps = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        w = line.split()
        verb, rest = w[0], w[1:]
        if verb in ("start", "launch", "load"):
            idle = verb == "start" and rest[-1:] == ["idle"]
            if idle:
                rest = rest[:-1]
            if not rest:
                raise StepsError("line %d: %s A [B]%s" % (n, verb, " [idle]" if verb == "start" else ""))
            steps.append(Step(n, verb, [_game(n, x) for x in rest], line, idle=idle))
        elif verb == "play":
            if not rest or len(rest) % 2:
                raise StepsError("line %d: play A file [B file]" % n)
            pairs = [(_game(n, rest[i]), rest[i + 1]) for i in range(0, len(rest), 2)]
            steps.append(Step(n, verb, pairs, line))
        elif verb in ("close", "kill", "freeze", "thaw"):
            if len(rest) != 1:
                raise StepsError("line %d: %s B" % (n, verb))
            steps.append(Step(n, verb, [_game(n, rest[0])], line))
        elif verb == "server":
            if rest not in (["stop"], ["start"]):
                raise StepsError("line %d: server stop|start" % n)
            steps.append(Step(n, verb, rest, line))
        elif verb == "sleep":
            try:
                secs = float(rest[0]) if len(rest) == 1 else None
            except ValueError:
                secs = None
            if secs is None or secs < 0:
                raise StepsError("line %d: sleep SECONDS" % n)
            steps.append(Step(n, verb, [secs], line))
        elif verb == "password":
            if len(rest) != 2:
                raise StepsError("line %d: password B TEXT (- for none)" % n)
            steps.append(Step(n, verb, [_game(n, rest[0]), "" if rest[1] == "-" else rest[1]], line))
        elif verb == "try":
            if len(rest) != 2 or rest[1] not in WANT_TRY:
                raise StepsError("line %d: try B welcomed|refused|none" % n)
            steps.append(Step(n, verb, [_game(n, rest[0]), rest[1]], line))
        elif verb == "expect":
            if len(rest) < 2:
                raise StepsError("line %d: expect NAME A=1 [B=0] [see=CHECK...]" % n)
            want, see = {}, []
            for kv in rest[1:]:
                m = re.match(r"^([AB])=(\d+)$", kv)
                if m:
                    want[m.group(1)] = int(m.group(2))
                elif re.match(r"^see=[A-Za-z0-9_.-]+$", kv):
                    see.append(kv[4:])
                else:
                    raise StepsError("line %d: expect wants A=N / B=N / see=CHECK, not %r" % (n, kv))
            if not want:
                raise StepsError("line %d: expect NAME A=1 [B=0] [see=CHECK...]" % n)
            steps.append(Step(n, verb, [rest[0], want, see], line))
        elif verb == "see":
            if not rest or not all(re.match(r"^[A-Za-z0-9_.-]+$", c) for c in rest):
                raise StepsError("line %d: see CHECK [CHECK...]" % n)
            steps.append(Step(n, verb, list(rest), line))
        elif verb == "shot":
            if len(rest) != 1:
                raise StepsError("line %d: shot NAME" % n)
            steps.append(Step(n, verb, rest, line))
        elif verb == "state":
            try:
                secs = float(rest[2]) if len(rest) == 3 else STATE_WAIT
            except ValueError:
                secs = None
            if len(rest) not in (2, 3) or rest[1] not in STATES or secs is None or secs <= 0:
                raise StepsError("line %d: state A %s [SECONDS]" % (n, "|".join(sorted(STATES))))
            steps.append(Step(n, verb, [_game(n, rest[0]), rest[1], secs], line))
        elif verb == "key":
            if len(rest) != 2 or rest[1] not in KEYS_POSTED:
                raise StepsError("line %d: key A %s" % (n, "|".join(sorted(KEYS_POSTED))))
            steps.append(Step(n, verb, [_game(n, rest[0]), rest[1]], line))
        elif verb == "flag":
            try:
                secs = float(rest[3]) if len(rest) == 4 else FLAG_WAIT
            except ValueError:
                secs = None
            watch = flag_watch(rest[1]) if len(rest) in (3, 4) else None
            if watch is None or rest[2] not in ("0", "1") or secs is None or secs <= 0:
                raise StepsError("line %d: flag A key64:0xKEY|bool:0xHASH 0|1 [SECONDS]" % n)
            steps.append(Step(n, verb, [_game(n, rest[0]), rest[1], rest[2], secs], line))
        elif verb == "wshot":
            if len(rest) != 1 or not re.match(r"^[A-Za-z0-9_.-]+$", rest[0]):
                raise StepsError("line %d: wshot NAME" % n)
            steps.append(Step(n, verb, rest, line))
        elif verb == "lines":
            m = re.match(r"^lines\s+(\S+)\s+([AB])\s+(max|min)=(\d+)\s+(.+)$", line)
            if not m:
                raise StepsError("line %d: lines NAME B max=N|min=N REGEX" % n)
            steps.append(Step(n, verb, [m.group(1), m.group(2), m.group(3), int(m.group(4))], line, m.group(5)))
        else:
            raise StepsError("line %d: unknown %r" % (n, verb))
    return steps


def parse_header(text):
    """The `#!` lines: switches (list), start (regex or None), timeout (int or None), server (more coop_server
    arguments, a list; run.ps1's `#! server`)."""
    head = {"switches": [], "start": None, "timeout": None, "server": []}
    for raw in text.splitlines():
        m = re.match(r"^#!\s*(\w+)\s*(.*?)\s*$", raw)
        if not m:
            continue
        k, v = m.group(1), m.group(2)
        if k == "switches":
            head["switches"] += [s.strip() for s in v.split(",") if s.strip()]
        elif k == "start":
            head["start"] = v
        elif k == "timeout" and v.isdigit():
            head["timeout"] = int(v)
        elif k == "server":
            head["server"] += v.split()
    return head


def count_lines(text, pattern):
    """Lines of text matching pattern (run.ps1: "(?m)^.*PATTERN"). Python's re first; a .NET-only pattern (a
    variable-width lookbehind) through PowerShell's [regex] on Windows. None: the pattern cannot be counted here."""
    try:
        return len(re.findall("(?m)^.*" + pattern, text))
    except re.error:
        pass
    if not WINDOWS:
        return None
    script = ("$t = [Console]::In.ReadToEnd(); "
              "[regex]::Matches($t, '(?m)^.*' + $env:HS_STEPS_PATTERN).Count")
    try:
        r = lanes.hs.run_quiet(["powershell", "-NoProfile", "-Command", script], input=text, capture_output=True,
                           text=True, encoding="utf-8", env=dict(os.environ, HS_STEPS_PATTERN=pattern), timeout=120)
        return int(r.stdout.strip()) if r.returncode == 0 and r.stdout.strip().isdigit() else None
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def once_floods(text):
    """(name, lines, processes) of each once-per-process line said more often than the log has game processes."""
    processes = max(1, len(PROCESS_START.findall(text)))
    out = []
    for name, pattern in ONCE_PER_PROCESS:
        got = len(pattern.findall(text))
        if got > processes:
            out.append((name, got, processes))
    return out


def live_actors(text):
    """The puppets and fairies alive in a game's log: name list (as run.ps1's Live-Actors)."""
    live = {}
    for m in LIVE.finditer(text):
        key = m.group(3) + " " + m.group(2)
        if m.group(1) == "+":
            live[key] = m.group(2)
        else:
            live.pop(key, None)
    return list(live.values())


def puppet_count(text):
    names = live_actors(text)
    p = [x for x in names if x.startswith("CoopPuppet_")]
    f = [x for x in names if x == "Dm_Npc_Navi"]
    kinds = ", ".join("%d %s" % (p.count(k), k) for k in sorted(set(p)))
    return len(p), len(f), "%d puppet (%s), %d fairy" % (len(p), kinds, len(f))


class Runner:
    """Plays parsed steps through a backend (HookshotGames, or a fake in the tests)."""

    def __init__(self, games, out, say=print, expect_wait=60.0, try_wait=40.0, clock=time.monotonic,
                 sleep=time.sleep, default_password=None, kit_root=None, checks=None):
        self.g, self.out, self.say = games, out, say
        # A game with no `password` line sends the server's own (run.ps1's Start-Game does the same): every
        # session may have one. `password X -` is none.
        self.default_password = (os.environ.get("OOT_COOP_TEST_PASSWORD", "") if default_password is None
                                 else default_password)
        self.expect_wait, self.try_wait = expect_wait, try_wait
        self.clock, self.sleep = clock, sleep
        self.passwords = {}
        self.fails = 0
        self.unproven = 0
        self.kit_root, self.checks = kit_root, checks   # proof.py's (None: the coop-kit above the repo, proofs.txt)

    def password(self, x):
        return self.passwords[x] if x in self.passwords else self.default_password

    def result(self, ok, text):
        """ok: True (PASS), False (FAIL) or a state: "PASS", "FAIL", "UNPROVEN"."""
        state = ok if isinstance(ok, str) else ("PASS" if ok else "FAIL")
        line = "%s %s" % (state, text)
        self.say(line)
        with open(os.path.join(self.out, "counts.txt"), "a", encoding="utf-8") as f:
            f.write(line + "\n")
        if state == "FAIL":
            self.fails += 1
        elif state == "UNPROVEN":
            self.unproven += 1

    def prove(self, names):
        """(state, text) of screenshot checks on this run's shots so far (proof.check_all)."""
        sys.path.insert(0, HERE)
        import proof
        kit = self.kit_root if self.kit_root is not None else proof.find_kit_root()
        return proof.check_all(self.out, names, kit, self.checks)

    def run(self, steps):
        for s in steps:
            self.say("step %d: %s" % (s.n, s.line))
            try:
                getattr(self, "do_" + s.verb)(s)
            except SetupFail as e:
                self.result(False, "SETUP-FAIL step %d (%s): %s" % (s.n, s.line, e))
                break
            except Exception as e:  # a step that cannot run: a FAIL line, the test goes on as run.ps1's does
                self.result(False, "step %d (%s): %s" % (s.n, s.line, e))
            ended = self.g.ended_by_itself()
            for x in ended:
                self.result(False, "game %s ended by itself during step %d" % (x, s.n))
        self.check_floods()
        return self.fails

    def check_floods(self):
        """A FAIL line for each once-per-process line a game's log says more often (silent when none)."""
        for x in ("A", "B"):
            try:
                text = self.g.log(x)
            except Exception:  # a game this backend does not have
                continue
            for name, got, processes in once_floods(text or ""):
                self.result(False, "%s: %s said %d times in %d game process(es), want at most once each" % (
                    x, name, got, processes))

    def do_start(self, s):
        for x in s.args:
            self.g.start(x, self.password(x), wait=False, idle=s.idle)
        for x in s.args:
            (self.g.wait_ready if s.idle else self.g.wait_joined)(x)

    def do_launch(self, s):
        # like start, but only until each game draws: for a game that starts before the server (no welcome yet)
        for x in s.args:
            self.g.start(x, self.password(x), wait=False)
        for x in s.args:
            self.g.wait_ready(x)

    def do_load(self, s):
        for x, ok, why in self.g.load(s.args):
            if not ok:
                self.result(False, "load %s: %s" % (x, why))

    def do_play(self, s):
        self.g.play(dict(s.args), "s%d-%s" % (s.n, os.path.splitext(s.args[0][1])[0]))

    def do_close(self, s):
        self.g.close(s.args[0])

    def do_kill(self, s):
        self.g.kill(s.args[0])

    def do_freeze(self, s):
        self.g.freeze(s.args[0])

    def do_thaw(self, s):
        self.g.thaw(s.args[0])

    def do_server(self, s):
        (self.g.server_stop if s.args[0] == "stop" else self.g.server_start)()

    def do_sleep(self, s):
        self.sleep(s.args[0])

    def do_password(self, s):
        self.passwords[s.args[0]] = s.args[1]

    def do_try(self, s):
        x, want = s.args
        before = self.g.server_log().count("refused a connection")
        self.g.start(x, self.password(x), wait=False)
        got, t0 = "none", self.clock()
        while self.clock() - t0 < self.try_wait:
            if JOINED in self.g.log(x):
                got = "welcomed"
                break
            if self.g.server_log().count("refused a connection") > before:
                got = "refused"
                break
            self.sleep(0.5)
        self.sleep(3)
        self.g.shot("s%d-try-%s-%s" % (s.n, x, want))
        self.result(got == want, "try %s (want %s, got %s, password set: %s)" % (x, want, got,
                                                                                  bool(self.password(x))))
        self.g.kill(x, keep_log="try%d" % s.n)

    def do_expect(self, s):
        name, want = s.args[:2]
        t0 = self.clock()
        while True:
            ok, texts = True, []
            for x, n in want.items():
                p, f, text = puppet_count(self.g.log(x)) if self.g.alive(x) else (0, 0, "0 puppet (), 0 fairy")
                texts.append("%s: %s" % (x, text))
                if p != n or f != n:
                    ok = False
            if ok or self.clock() - t0 >= self.expect_wait:
                break
            self.sleep(0.5)
        text = "%s (want %s, after %d s): %s" % (name, " ".join("%s=%d" % kv for kv in want.items()),
                                                 int(self.clock() - t0), "; ".join(texts))
        self.g.shot("s%d-%s" % (s.n, name))
        if not ok:
            self.result(False, text)
            return
        # The counts come from log lines; the puppets are visible: the shot proves them (owner rule).
        see = s.args[2] if len(s.args) > 2 else []
        if not see:
            self.result("UNPROVEN", text + " [counts from the log only: no see= check on shot s%d-%s]" % (s.n, name))
            return
        state, proof_text = self.prove(see)
        self.result(state, text + (" [seen: %s]" if state == "PASS" else " [screenshot check: %s]") % proof_text)

    def do_see(self, s):
        state, text = self.prove(s.args)
        self.result(state, "see %s%s" % (" ".join(s.args), " [seen: %s]" % text if state == "PASS"
                                          else " [screenshot check: %s]" % text))

    def do_shot(self, s):
        self.g.shot("s%d-%s" % (s.n, s.args[0]))

    def do_state(self, s):
        x, name, secs = s.args
        want = STATES[name]
        deadline = self.clock() + secs
        while True:
            got = last_state(self.g.log(x))
            if got == want:
                self.say("  %s: play state %s ('coop: free play: %s')" % (x, name, got))
                return
            if self.clock() >= deadline:
                raise SetupFail("%s is not in %s after %d s: its last 'coop: free play:' line says %s" % (
                    x, name, secs, repr(got) if got else "nothing"))
            self.sleep(0.5)

    def do_flag(self, s):
        """The expect-state gate: a NEW read of the watched flag (logged after this step began, so never a value
        from before the scenario set it) says the value, within S s; else SETUP-FAIL."""
        x, name, want, secs = s.args
        kind, number = flag_watch(name)
        before = len(flag_reads(self.g.log(x), kind, number))
        deadline = self.clock() + secs
        while True:
            reads = flag_reads(self.g.log(x), kind, number)
            new = reads[before:]
            if new and new[-1] == want:
                self.say("  %s: %s = %s (the module's read, %d new)" % (x, name, want, len(new)))
                return
            if self.clock() >= deadline:
                raise SetupFail("%s's %s is not %s after %d s: the module's last read says %s%s" % (
                    x, name, want, secs, repr(reads[-1]) if reads else "nothing",
                    "" if new else " (no read since this step began: is the watch switch set?)"))
            self.sleep(0.5)

    def do_wshot(self, s):
        self.g.wshot("s%d-%s" % (s.n, s.args[0]))

    def do_key(self, s):
        x, key = s.args
        vk, scan, proof = KEYS_POSTED[key]
        before = count_lines(self.g.log(x), proof) or 0
        self.g.key(x, vk, scan)
        deadline = self.clock() + KEY_LAND_WAIT
        while True:
            got = count_lines(self.g.log(x), proof) or 0
            if got > before:
                self.say("  %s landed in %s ('%s' lines %d -> %d)" % (key, x, proof, before, got))
                return
            if self.clock() >= deadline:
                raise SetupFail("%s posted to %s's window but no new '%s' line within %d s" % (
                    key, x, proof, KEY_LAND_WAIT))
            self.sleep(0.5)

    def do_lines(self, s):
        name, x, kind, bound = s.args
        got = count_lines(self.g.log(x), s.pattern)
        if got is None:
            self.result(False, "%s (%s: the pattern '%s' cannot be counted here)" % (name, x, s.pattern))
            return
        ok = got >= bound if kind == "min" else got <= bound
        self.result(ok, "%s (%s: %d lines match '%s', %s %d)" % (name, x, got, s.pattern,
                                                                 "at least" if kind == "min" else "at most", bound))


class DryGames:
    """Prints what each step would do; starts nothing."""

    def __init__(self, say=print):
        self.say = say

    def start(self, x, password, wait=True, idle=False):
        self.say("  would start %s (save baseline back, fresh log%s)" % (
            x, ", no server: co-op idle" if idle else ", join password set" if password else ""))

    def wait_ready(self, x):
        self.say("  would wait for %s's renderer" % x)

    def wait_joined(self, x):
        self.say("  would wait for %s: '%s'" % (x, JOINED))

    def load(self, xs):
        self.say("  would load %s (title to the save)" % " ".join(xs))
        return [(x, True, "") for x in xs]

    def play(self, files, name):
        self.say("  would play %s" % ", ".join("%s %s" % kv for kv in files.items()))

    def close(self, x):
        self.say("  would close %s (quit through its bridge)" % x)

    def kill(self, x, keep_log=None):
        self.say("  would end %s at once" % x)

    def freeze(self, x):
        self.say("  would suspend %s's process" % x)

    def key(self, x, vk, scan):
        self.say("  would post key 0x%02x to %s's window" % (vk, x))

    def thaw(self, x):
        self.say("  would let %s's process go on" % x)

    def server_stop(self):
        self.say("  would stop the server")

    def server_start(self):
        self.say("  would start the server")

    def server_log(self):
        return ""

    def log(self, x):
        return ""

    def alive(self, x):
        return False

    def shot(self, name):
        self.say("  would shoot %s" % name)

    def wshot(self, name):
        self.say("  would window-shoot %s through %s (else SETUP-FAIL)" % (
            name, os.environ.get(WINDOW_SHOT_ENV) or "NO %s SET" % WINDOW_SHOT_ENV))

    def ended_by_itself(self):
        return []


class HookshotGames:
    """Two Hookshot game folders, the server and the control bridges (tools/autotest/hookshot.py's pieces)."""

    def __init__(self, hk, games, out, server, port, pipe, env, timeout, head, restore=True, say=print):
        self.hk, self.games, self.out, self.server_exe, self.port = hk, games, out, server, port
        self.pipe, self.env, self.timeout, self.head, self.restore, self.say = pipe, env, timeout, head, restore, say
        self.procs, self.bridges, self.runs, self.expected_exit = {}, {}, {"A": 0, "B": 0}, set()
        self.frozen = set()  # games whose process is suspended (freeze)
        self.server, self.server_runs = None, 0
        self.server_delay = []   # hookshot.py test_delay_args: the TEST-ONLY delay (needs OOT_COOP_TEST=1)
        self.renderer = None     # (wait, stall, limit) for hookshot.wait_renderer; None: (timeout, its defaults)
        self.commands = {x: os.path.join(out, "commands-%s.txt" % x) for x in games}
        self.display = None      # the monitor for the game windows (placement.py); None: $OOT_TEST_DISPLAY, else DISPLAY2

    def _log_path(self, x):
        # Longshot rc.3 wrote longshot_log.txt beside the exe, rc.4 in logs\ there: the newest one, else rc.3's place.
        find = getattr(self.hk, "find_log", None)
        return (find and find(self.games[x])) or os.path.join(self.hk.exe_dir(self.games[x]), "longshot_log.txt")

    def _run_log(self, x):
        """This run's log: the game's longshot_log.txt (start() removes an older one), else game-out-<x>-<n>.txt.
        Hookshot writes longshot_log.txt only when its stderr goes nowhere (started from its icon); here stderr is
        captured, so its log is in game-out."""
        if os.path.isfile(self._log_path(x)) or not self.runs[x]:
            return self._log_path(x)
        return os.path.join(self.out, "game-out-%s-%d.txt" % (x, self.runs[x]))

    def log(self, x):
        try:
            with open(self._run_log(x), encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def alive(self, x):
        p = self.procs.get(x)
        return p is not None and p.poll() is None

    def _suspend(self, x, on):
        """NtSuspendProcess / NtResumeProcess on game x (Windows only; a game that is not running: nothing)."""
        p = self.procs.get(x)
        if p is None or p.poll() is not None:
            return False
        import ctypes
        k32, nt = ctypes.WinDLL("kernel32", use_last_error=True), ctypes.WinDLL("ntdll")
        k32.OpenProcess.restype = ctypes.c_void_p
        h = k32.OpenProcess(0x0800, False, p.pid)  # PROCESS_SUSPEND_RESUME
        if not h:
            raise OSError("cannot open game %s's process (error %d)" % (x, ctypes.get_last_error()))
        try:
            fn = nt.NtSuspendProcess if on else nt.NtResumeProcess
            fn.argtypes = [ctypes.c_void_p]
            status = fn(h) & 0xFFFFFFFF
        finally:
            k32.CloseHandle(ctypes.c_void_p(h))
        if status != 0:
            raise OSError("%s game %s: NTSTATUS %#x" % ("suspending" if on else "resuming", x, status))
        return True

    def freeze(self, x):
        if self._suspend(x, True):
            self.frozen.add(x)
            self.say("%s: process suspended" % x)

    def thaw(self, x):
        if x in self.frozen:
            self.frozen.discard(x)
            self._suspend(x, False)
            self.say("%s: process goes on" % x)

    def _end(self, x, how):
        p = self.procs.get(x)
        if p is None:
            return
        if x in self.frozen:
            try:
                self.thaw(x)  # a frozen game cannot quit through its bridge
            except OSError as e:
                self.say("%s" % e)
        self.expected_exit.add(x)
        if p.poll() is None:
            if how == "quit" and x in self.bridges:
                try:
                    self.bridges[x][1].request("quit", reply_timeout=10.0, check=False)
                except Exception:
                    pass
            if how == "kill":
                p.kill()
            try:
                p.wait(20)
            except subprocess.TimeoutExpired:
                self.say("game %s did not close in 20 s: ended" % x)
                p.kill()
                p.wait(15)
        if x in self.bridges:
            try:
                self.bridges[x][1].close()
            except Exception:
                pass
            del self.bridges[x]

    def start(self, x, password, wait=True, idle=False):
        hk, g = self.hk, self.games[x]
        self._end(x, "kill")
        self.expected_exit.discard(x)
        if self.restore:
            self.say("%s: save baseline %s" % (x, "restored" if hk.restore_save(g) else "none (autotest-save missing)"))
        lp = self._log_path(x)
        if os.path.isfile(lp):
            if self.runs[x]:
                shutil.copy2(lp, os.path.join(self.out, "hookshot_log-%s-%d.txt" % (x, self.runs[x])))
            os.remove(lp)
        self.runs[x] += 1
        open(self.commands[x], "w").close()
        sock = "%s-%s" % (self.pipe, x) if WINDOWS else os.path.join(self.out, "control-%s.sock" % x)
        env = dict(os.environ, HOS_CONTROL=sock, HOS_CONTROL_MEM="ro", LICHII_COOP_TEST_COMMANDS=self.commands[x],
                   LICHII_COOP_SERVER="127.0.0.1:%d" % self.port, LICHII_COOP_NAME=x)   # "A", "B": /teleport B
        env.pop("LICHII_COOP_PASSWORD", None)
        if idle:   # (co-op idle: no server from the run nor from the environment; it hosts or joins only when told)
            env.pop("LICHII_COOP_SERVER", None)
            password = ""
        # Its own save, no crash ask on the title (TEST_GAME_DEFAULTS, as hookshot.py's): a switch can undo it.
        for k, v in TEST_GAME_DEFAULTS:
            env.setdefault(k, v)
        hk.test_lane_precompile(env, getattr(self, "precompile", "test"))   # (game-commit-c141: hookshot.py's one rule)
        if password:
            env["LICHII_COOP_PASSWORD"] = password
        for kv in self.head["switches"] + self.env:
            k, v = kv.split("=", 1)
            if k[:2] in ("A:", "B:"):
                if k[0] != x:
                    continue
                k = k[2:]
            env[k] = v
        hk.pack_start(g, env, lambda line: self.say("%s: %s" % (x, line)))   # (as the kit's start: row 265)
        exe = os.path.join(hk.exe_dir(g), "longshot.exe" if WINDOWS else "longshot")
        go = open(os.path.join(self.out, "game-out-%s-%d.txt" % (x, self.runs[x])), "wb")
        flags = getattr(hk, "popen_flags", None)   # (no window activation, see hookshot.popen_flags)
        kw = flags() if flags else ({"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if WINDOWS
                                    else {"start_new_session": True})
        self.procs[x] = subprocess.Popen([exe], cwd=hk.exe_dir(g), env=env, stdin=subprocess.DEVNULL, stdout=go,
                                         stderr=subprocess.STDOUT, **kw)
        go.close()
        self.say("%s: started pid %d, bridge %s" % (x, self.procs[x].pid, sock))
        if getattr(hk, "place_games", None):   # (its window on the chosen monitor as soon as it is there)
            hk.place_games({x: self.procs[x].pid}, self.display, self.say, wait=30.0)
        self.bridges[x] = (sock, hk.Bridge.connect(sock, timeout=120))
        if wait:
            (self.wait_ready if idle else self.wait_joined)(x)

    def wait_ready(self, x):
        # Drawing first: a game whose shaders still compile reads no controller and joins only after (a cold driver
        # cache: minutes, its length not fixed), so this wait follows the compile's progress (hookshot.wait_renderer)
        # and the join's own timeout counts from there.
        wait, stall, limit = self.renderer or (self.timeout, self.hk.RENDERER_STALL, self.hk.RENDERER_LIMIT)
        waited = self.hk.wait_renderer(lambda: self.log(x), lambda: self.alive(x), wait, stall, limit)
        self.say("%s: renderer ready (%.0f s waited)" % (x, waited))
        if getattr(self.hk, "place_games", None):   # (the game can move its window when its renderer comes up)
            self.hk.place_games({x: self.procs[x].pid}, self.display, self.say)

    def wait_joined(self, x):
        self.wait_ready(x)
        t0 = time.monotonic()
        while JOINED not in self.log(x):
            if not self.alive(x):
                raise RuntimeError("game %s closed while starting" % x)
            if time.monotonic() - t0 > self.timeout:
                raise TimeoutError("game %s did not reach '%s' in %d s after its renderer started"
                                   % (x, JOINED, self.timeout))
            time.sleep(0.5)

    def load(self, xs):
        hk, res, errs = self.hk, [], {}

        def one(x):
            try:
                lb = hk.Bridge.connect(self.bridges[x][0])
                try:
                    lb.request("wait", reply_timeout=self.timeout + 30, event="frame", frame=1, timeout=self.timeout)
                    hk.to_play(lb, hk.Log(os.path.join(self.out, "player-%s.txt" % x)), self.timeout)
                finally:
                    lb.close()
            except Exception as e:
                errs[x] = e
        ts = [threading.Thread(target=one, args=(x,)) for x in xs]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        for x in xs:
            if x in errs:
                res.append((x, False, str(errs[x])))
                continue
            ok, why = True, ""
            if self.head["start"]:
                t0 = time.monotonic()
                while not re.search(self.head["start"], self.log(x)):
                    if time.monotonic() - t0 >= 15:
                        ok, why = False, "no '%s' in the log after the load" % self.head["start"]
                        break
                    time.sleep(0.5)
                with open(os.path.join(self.out, "start.txt"), "a", encoding="utf-8") as f:
                    f.write("%s %s: '%s'%s\n" % ("PASS" if ok else "FAIL", x, self.head["start"], "" if ok else
                                                 " not in the log"))
            res.append((x, ok, why))
        return res

    def play(self, files, name):
        hk, errs = self.hk, {}
        parsed = {}
        for x, f in files.items():
            parsed[x] = hk.parse(hk.read_script(os.path.join(HERE, "scenarios", f)))

        def one(x):
            try:
                pb = hk.Bridge.connect(self.bridges[x][0])
                try:
                    p = hk.Player(pb, hk.Log(os.path.join(self.out, "player-%s.txt" % x)), shot=self._shooter(x, name),
                                  connect=lambda: hk.Bridge.connect(self.bridges[x][0]), commands=self.commands[x])
                    p.play(parsed[x])
                    for t in p.warps:
                        t.join(p.warp_for)
                finally:
                    pb.close()
            except Exception as e:
                errs[x] = e
        ts = [threading.Thread(target=one, args=(x,), daemon=True) for x in parsed]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        if errs:
            raise RuntimeError("play: " + ", ".join("%s %s" % kv for kv in errs.items()))

    def _shooter(self, x, name):
        def shot(mark):
            self.shot("%s-%s-%s" % (name, x, mark) if mark else name)
        return shot

    def key(self, x, vk, scan):
        """A key's press and release posted to game x's visible windows (Windows; no focus needed: the window's
        thread runs Longshot's wndproc, which hands WM_KEYDOWN to hos_coop_key first)."""
        if not WINDOWS:
            raise SetupFail("key: Windows only")
        p = self.procs.get(x)
        if p is None or p.poll() is not None:
            raise SetupFail("key: game %s is not running" % x)
        import ctypes
        from ctypes import wintypes
        import bridge_proof
        hwnds = bridge_proof.windows_of(p.pid)
        if not hwnds:
            raise SetupFail("key: game %s (pid %d) has no visible window" % (x, p.pid))
        user32 = ctypes.windll.user32
        down = 1 | (scan << 16)
        up = down | (1 << 30) | (1 << 31)
        for h in hwnds:
            user32.PostMessageW(wintypes.HWND(h), 0x0100, vk, down)  # WM_KEYDOWN
            user32.PostMessageW(wintypes.HWND(h), 0x0101, vk, up)    # WM_KEYUP
        self.say("  %s: key 0x%02x posted to %d window(s)" % (x, vk, len(hwnds)))

    def shot(self, name):
        for y, (_, b) in list(self.bridges.items()):
            if not self.alive(y):
                continue
            path = os.path.join(self.out, "%s-%s.png" % (name, y))
            try:
                b.request("shot", reply_timeout=30.0, path=path, timeout=20)
            except Exception as e:
                self.say("%s shot %s: %s" % (y, os.path.basename(path), e))

    def wshot(self, name):
        """A WINDOW shot of every running game: the screen's copy of its window, Longshot's ImGui overlays (its menu,
        co-op's tabs) included, which a bridge `shot` never has (Longshot takes those before its overlays:
        vk_replay.cpp shot_ask_record; run f1-event-aebd16089's s30-f1-title). Through the helper named by
        OOT_WINDOW_SHOT (offline/lib/screen-shot.ps1, its Save-WindowShot: window_shot_command; worker rule, the
        game raised and its window checked for cover). No helper, a covered or a failed shot: SETUP-FAIL."""
        helper = os.environ.get(WINDOW_SHOT_ENV)
        if not helper or not os.path.isfile(helper):
            raise SetupFail("wshot: %s names no helper file (%r)" % (WINDOW_SHOT_ENV, helper))
        for y in sorted(self.procs):
            if not self.alive(y):
                continue
            path = os.path.join(self.out, "%s-%s.png" % (name, y))
            code, text = run_window_shot(window_shot_command(helper, self.procs[y].pid, path,
                                                             os.path.join(self.out, "window-shots.txt")))
            self.say("  %s window shot %s: exit %s %s" % (y, os.path.basename(path), code, text))
            if code != 0:
                raise SetupFail("wshot %s: %s's window shot not taken (exit %s: %s)" % (name, y, code,
                                                                                         text or "no output"))

    def close(self, x):
        self._end(x, "quit")

    def kill(self, x, keep_log=None):
        self._end(x, "kill")
        if keep_log and os.path.isfile(self._run_log(x)):
            shutil.copy2(self._run_log(x), os.path.join(self.out, "hookshot_log-%s-%s.txt" % (x, keep_log)))

    def server_start(self):
        if not self.server_exe:
            return
        self.server_runs += 1
        sfx = "-%d" % self.server_runs if self.server_runs > 1 else ""
        # (always set by main: ensure_server_password; the games send the same one)
        args = [self.server_exe, "--port", str(self.port), "--password-env", "OOT_COOP_TEST_PASSWORD"]
        args += self.head.get("server", [])   # the steps file's `#! server` arguments
        args = lanes.hs.server_bind(args)      # (127.0.0.1 unless they name their own --bind)
        env = None
        if self.server_delay:
            args += self.server_delay
            env = dict(os.environ, OOT_COOP_TEST="1")
        with open(os.path.join(self.out, "server-out%s.txt" % sfx), "w") as so:
            self.server = lanes.hs.popen_quiet(args, stdout=so, stderr=subprocess.STDOUT, env=env)
        time.sleep(1)

    def server_stop(self):
        if self.server and self.server.poll() is None:
            self.server.kill()
            self.server.wait(15)
        time.sleep(1)

    def server_log(self):
        text = ""
        for f in sorted(os.listdir(self.out)):
            if f.startswith("server-out"):
                with open(os.path.join(self.out, f), encoding="utf-8", errors="replace") as fh:
                    text += fh.read()
        return text

    def ended_by_itself(self):
        out = []
        for x, p in self.procs.items():
            if p.poll() is not None and x not in self.expected_exit:
                self.expected_exit.add(x)
                out.append(x)
        return out

    def finish(self):
        for x in list(self.procs):
            self._end(x, "quit")
        self.server_stop()
        for x in self.games:
            if os.path.isfile(self._run_log(x)):
                shutil.copy2(self._run_log(x), os.path.join(self.out, "hookshot_log-%s.txt" % x))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check", help="parse steps files and print their steps")
    c.add_argument("files", nargs="+")
    r = sub.add_parser("run")
    r.add_argument("--steps", required=True)
    r.add_argument("--game-a", required=True)
    r.add_argument("--game-b", required=True)
    r.add_argument("--server", help="coop_server.exe")
    r.add_argument("--port", type=int, default=27560)
    r.add_argument("--pipe", default="coop-auto")
    r.add_argument("--out", required=True)
    r.add_argument("--env", action="append", default=[], help="NAME=VALUE (or A:NAME=VALUE) for the games")
    r.add_argument("--timeout", type=int, default=900, help="seconds for a boot and a load")
    r.add_argument("--expect-wait", type=float, default=60.0)
    r.add_argument("--no-restore", action="store_true")
    r.add_argument("--dry-run", action="store_true")
    r.add_argument("--display", help="the monitor the game windows go to, by device name (default "
                                     "$OOT_TEST_DISPLAY, else DISPLAY2): tools/autotest/placement.py")
    r.add_argument("--test-delay-ms", type=int, default=0,
                   help="TEST ONLY: the server holds every packet, this round trip in ms (half each way)")
    r.add_argument("--test-jitter-ms", type=int, default=0, help="TEST ONLY: +- up to this many ms each way")
    r.add_argument("--renderer-wait", type=int, help="seconds a game may take to start its renderer with no compile "
                                                     "progress logged (default: --timeout)")
    r.add_argument("--renderer-stall", type=int, help="while the compile logs progress: seconds without a rise "
                                                      "before it fails (default: hookshot.py's)")
    r.add_argument("--renderer-limit", type=int, help="seconds in all for one renderer start (default: hookshot.py's)")
    import hookshot as hk_choices  # noqa: E402 (PRECOMPILE_CHOICES: hookshot.py's one list)
    r.add_argument("--precompile", choices=hk_choices.PRECOMPILE_CHOICES, default="test",
                   help="test (default): no shader precompile before the title (hookshot.py TEST_LANE_PRECOMPILE); "
                        "default: Longshot's own start, as players run it")
    a = ap.parse_args(argv)
    if a.cmd == "check":
        bad = 0
        for path in a.files:
            with open(path, encoding="utf-8-sig") as f:
                text = f.read()
            try:
                steps = parse_steps(text)
            except StepsError as e:
                print("%s: %s" % (path, e))
                bad += 1
                continue
            head = parse_header(text)
            problems = flag_watch_problems(steps, head)
            if problems:
                print("%s: %s" % (path, "; ".join(problems)))
                bad += 1
                continue
            plays = sorted({f for s in steps if s.verb == "play" for _, f in s.args})
            print("%s: %d steps (%s)%s%s" % (path, len(steps), " ".join(sorted({s.verb for s in steps})),
                                             ", plays " + " ".join(plays) if plays else "",
                                             ", switches " + ",".join(head["switches"]) if head["switches"] else ""))
        return 1 if bad else 0
    for kv in a.env:
        if "=" not in kv:
            ap.error("--env NAME=VALUE")
    with open(a.steps, encoding="utf-8-sig") as f:
        text = f.read()
    try:
        steps = parse_steps(text)
    except StepsError as e:
        print("hs_steps.py: %s: %s" % (a.steps, e))
        return 1
    head = parse_header(text)
    problems = flag_watch_problems(steps, head)
    if problems:
        print("hs_steps.py: %s: %s" % (a.steps, "; ".join(problems)))
        return 1
    if a.dry_run:   # (nothing set: a dry run starts nothing)
        print("steps %s: %d steps; games %s / %s; server %s port %d, %s; pipes %s-A/-B; out %s" % (
            a.steps, len(steps), a.game_a, a.game_b, a.server, a.port,
            "join password from OOT_COOP_TEST_PASSWORD" if os.environ.get("OOT_COOP_TEST_PASSWORD")
            else "a random join password would be made for the run (OOT_COOP_TEST_PASSWORD is unset)", a.pipe, a.out))
        _dry(steps)
        return 0
    made = ensure_server_password(os.environ)   # (before the server and the Runner read it)
    password_note = ("a random join password made for this run (OOT_COOP_TEST_PASSWORD was unset)" if made
                     else "join password from OOT_COOP_TEST_PASSWORD")
    os.makedirs(a.out, exist_ok=True)
    sys.path.insert(0, HERE)
    import hookshot as hk  # noqa: E402 (tools/autotest/hookshot.py: the bridge, the player, the load)
    say = hk.Log(os.path.join(a.out, "steps.txt"), echo=print)
    games = HookshotGames(hk, {"A": os.path.abspath(a.game_a), "B": os.path.abspath(a.game_b)}, a.out, a.server,
                          a.port, a.pipe, a.env, head["timeout"] or a.timeout, head, restore=not a.no_restore, say=say)
    games.display = a.display
    try:
        mon = hk.placement.check_display(a.display) if WINDOWS else None
    except hk.placement.PlacementError as e:   # (before any game window can open anywhere else)
        say("window placement: %s" % e)
        return 2
    if mon:
        if mon.get("note"):
            say("window placement: %s" % mon["note"])
        say("windows go to %s (work area %s)" % (mon["name"], mon["work"]))
    say("server and games: %s" % password_note)
    games.server_delay = hk.test_delay_args(a.test_delay_ms, a.test_jitter_ms)
    games.renderer = (a.renderer_wait or games.timeout, a.renderer_stall or hk.RENDERER_STALL,
                      a.renderer_limit or hk.RENDERER_LIMIT)
    games.precompile = a.precompile
    fails, runner = 1, None
    try:
        games.server_start()
        runner = Runner(games, a.out, say=say, expect_wait=a.expect_wait)
        fails = runner.run(steps)
    finally:
        games.finish()
    say("steps done: %d FAIL" % fails + (", %d UNPROVEN (no screenshot proof: not a pass)" % runner.unproven
                                         if runner and runner.unproven else ""))
    return 1 if fails else 0


def _dry(steps):
    """The plan without an out folder: what each step would do."""
    say = print
    games = DryGames(say)
    for s in steps:
        say("step %d: %s" % (s.n, s.line))
        if s.verb in ("start", "launch"):
            for x in s.args:
                games.start(x, "", idle=s.idle)
        elif s.verb == "load":
            games.load(s.args)
        elif s.verb == "play":
            games.play(dict(s.args), "")
        elif s.verb in ("close", "kill", "freeze", "thaw"):
            getattr(games, s.verb)(s.args[0])
        elif s.verb == "server":
            (games.server_stop if s.args[0] == "stop" else games.server_start)()
        elif s.verb == "try":
            say("  would start %s and check for '%s' within 40 s, then end it" % (s.args[0], s.args[1]))
        elif s.verb == "expect":
            say("  would wait until %s puppets and fairies, then shoot %s and grade %s" % (
                " ".join("%s=%d" % kv for kv in s.args[1].items()), s.args[0],
                " ".join(s.args[2]) if s.args[2] else "no see= check (UNPROVEN)"))
        elif s.verb == "see":
            say("  would grade %s on the shots so far" % " ".join(s.args))
        elif s.verb == "state":
            say("  would wait %d s for %s's 'coop: free play: %s' (else SETUP-FAIL)" % (
                s.args[2], s.args[0], STATES[s.args[1]]))
        elif s.verb == "key":
            games.key(s.args[0], *KEYS_POSTED[s.args[1]][:2])
            say("  then would wait %d s for '%s' in %s's log (else SETUP-FAIL)" % (
                KEY_LAND_WAIT, KEYS_POSTED[s.args[1]][2], s.args[0]))
        elif s.verb == "flag":
            say("  would wait %d s for a new read of %s's %s = %s (else SETUP-FAIL)" % (
                s.args[3], s.args[0], s.args[1], s.args[2]))
        elif s.verb == "wshot":
            games.wshot("s%d-%s" % (s.n, s.args[0]))
        elif s.verb == "lines":
            say("  would count %s's log lines matching '%s' (%s=%d)" % (s.args[1], s.pattern, s.args[2], s.args[3]))
        elif s.verb == "password":
            say("  %s's join password from its next start: %s" % (s.args[0], "set" if s.args[1] else "none"))
        else:
            say("  %s" % s.line)


if __name__ == "__main__":
    sys.exit(main())
