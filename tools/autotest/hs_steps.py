"""Steps files (scenarios/<name>.steps.txt) on Hookshot: the same lines tools/autotest/run.ps1 plays on the emulator.

    python -I tools/autotest/hs_steps.py check <name>.steps.txt...              (parse, print the plan)
    python -I tools/autotest/hs_steps.py run --steps <file> --game-a DIR --game-b DIR --server EXE --out DIR
                                          [--port N] [--pipe NAME] [--env NAME=VALUE]... [--dry-run]

run.ps1 -Runtime hookshot calls `run` when the scenario is a steps file. The verbs (run.ps1's Run-Steps):
  start A [B]           boot the games named (again: a running one is ended first), with the save baseline put back
                        and a fresh hookshot_log.txt (the old one kept as hookshot_log-<x>-<n>.txt); wait until each
                        logs "coop: welcomed as player"
  load A [B]            title to the save in each (hookshot.py to_play); the `#! start` regex checked after it
  play A f [B f]        play scenarios/<f> in A (and B), both at once, and wait for both
  close B / kill B      B quits through its control bridge (it leaves the server) / B ends at once (a crash)
  server stop|start     the server goes away / comes back on the same port (server-out[-n].txt)
  sleep S               seconds
  password B TEXT       the join password game B sends from its next start (LICHII_COOP_PASSWORD; - for none);
                        without a password line a game sends the server's (OOT_COOP_TEST_PASSWORD)
  try B welcomed|refused|none   start B without waiting and see within 40 s: welcomed (its log), refused (a new
                        "refused a connection" in the server log) or none; then end B
  expect NAME A=1 B=0   wait up to --expect-wait s until each game named has that many puppets (CoopPuppet_*) and
                        as many fairies (Dm_Npc_Navi) alive, from the objects debug lines; then shot NAME
  shot NAME             a screenshot of every running game
  lines NAME B max=N|min=N REGEX   game B's log has at most / at least N lines matching REGEX
Results go to counts.txt (PASS/FAIL lines as run.ps1 writes them); a step that cannot run is a FAIL there too.
The server's join password comes from the environment variable OOT_COOP_TEST_PASSWORD (run.ps1 -ServerPassword).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WINDOWS = os.name == "nt"
WANT_TRY = ("welcomed", "refused", "none")
JOINED = "coop: welcomed as player"
LIVE = re.compile(r"coop: objects: ([+-]) (CoopPuppet_\w+|Dm_Npc_Navi) (0x[0-9a-f]+)")


class StepsError(Exception):
    pass


class Step:
    def __init__(self, n, verb, args, line, pattern=None):
        self.n, self.verb, self.args, self.line, self.pattern = n, verb, args, line, pattern

    def __repr__(self):
        return "Step(%d, %r, %r)" % (self.n, self.verb, self.args)


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
        if verb in ("start", "load"):
            if not rest:
                raise StepsError("line %d: %s A [B]" % (n, verb))
            steps.append(Step(n, verb, [_game(n, x) for x in rest], line))
        elif verb == "play":
            if not rest or len(rest) % 2:
                raise StepsError("line %d: play A file [B file]" % n)
            pairs = [(_game(n, rest[i]), rest[i + 1]) for i in range(0, len(rest), 2)]
            steps.append(Step(n, verb, pairs, line))
        elif verb in ("close", "kill"):
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
                raise StepsError("line %d: expect NAME A=1 [B=0]" % n)
            want = {}
            for kv in rest[1:]:
                m = re.match(r"^([AB])=(\d+)$", kv)
                if not m:
                    raise StepsError("line %d: expect wants A=N / B=N, not %r" % (n, kv))
                want[m.group(1)] = int(m.group(2))
            steps.append(Step(n, verb, [rest[0], want], line))
        elif verb == "shot":
            if len(rest) != 1:
                raise StepsError("line %d: shot NAME" % n)
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
        r = subprocess.run(["powershell", "-NoProfile", "-Command", script], input=text, capture_output=True,
                           text=True, encoding="utf-8", env=dict(os.environ, HS_STEPS_PATTERN=pattern), timeout=120)
        return int(r.stdout.strip()) if r.returncode == 0 and r.stdout.strip().isdigit() else None
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


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
                 sleep=time.sleep, default_password=None):
        self.g, self.out, self.say = games, out, say
        # A game with no `password` line sends the server's own (run.ps1's Start-Game does the same): every
        # session may have one. `password X -` is none.
        self.default_password = (os.environ.get("OOT_COOP_TEST_PASSWORD", "") if default_password is None
                                 else default_password)
        self.expect_wait, self.try_wait = expect_wait, try_wait
        self.clock, self.sleep = clock, sleep
        self.passwords = {}
        self.fails = 0

    def password(self, x):
        return self.passwords[x] if x in self.passwords else self.default_password

    def result(self, ok, text):
        line = "%s %s" % ("PASS" if ok else "FAIL", text)
        self.say(line)
        with open(os.path.join(self.out, "counts.txt"), "a", encoding="utf-8") as f:
            f.write(line + "\n")
        if not ok:
            self.fails += 1

    def run(self, steps):
        for s in steps:
            self.say("step %d: %s" % (s.n, s.line))
            try:
                getattr(self, "do_" + s.verb)(s)
            except Exception as e:  # a step that cannot run: a FAIL line, the test goes on as run.ps1's does
                self.result(False, "step %d (%s): %s" % (s.n, s.line, e))
            ended = self.g.ended_by_itself()
            for x in ended:
                self.result(False, "game %s ended by itself during step %d" % (x, s.n))
        return self.fails

    def do_start(self, s):
        for x in s.args:
            self.g.start(x, self.password(x), wait=False)
        for x in s.args:
            self.g.wait_joined(x)

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
        name, want = s.args
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
        self.result(ok, "%s (want %s, after %d s): %s" % (name, " ".join("%s=%d" % kv for kv in want.items()),
                                                          int(self.clock() - t0), "; ".join(texts)))
        self.g.shot("s%d-%s" % (s.n, name))

    def do_shot(self, s):
        self.g.shot("s%d-%s" % (s.n, s.args[0]))

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

    def start(self, x, password, wait=True):
        self.say("  would start %s (save baseline back, fresh log%s)" % (x, ", join password set" if password else ""))

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

    def ended_by_itself(self):
        return []


class HookshotGames:
    """Two Hookshot game folders, the server and the control bridges (tools/autotest/hookshot.py's pieces)."""

    def __init__(self, hk, games, out, server, port, pipe, env, timeout, head, restore=True, say=print):
        self.hk, self.games, self.out, self.server_exe, self.port = hk, games, out, server, port
        self.pipe, self.env, self.timeout, self.head, self.restore, self.say = pipe, env, timeout, head, restore, say
        self.procs, self.bridges, self.runs, self.expected_exit = {}, {}, {"A": 0, "B": 0}, set()
        self.server, self.server_runs = None, 0
        self.commands = {x: os.path.join(out, "commands-%s.txt" % x) for x in games}

    def _log_path(self, x):
        return os.path.join(self.hk.exe_dir(self.games[x]), "hookshot_log.txt")

    def _run_log(self, x):
        """This run's log: the game's hookshot_log.txt (start() removes an older one), else game-out-<x>-<n>.txt.
        Hookshot writes hookshot_log.txt only when its stderr goes nowhere (started from its icon); here stderr is
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

    def _end(self, x, how):
        p = self.procs.get(x)
        if p is None:
            return
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

    def start(self, x, password, wait=True):
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
                   LICHII_COOP_SERVER="127.0.0.1:%d" % self.port, LICHII_COOP_NAME="Hook" + x)
        env.pop("LICHII_COOP_PASSWORD", None)
        if password:
            env["LICHII_COOP_PASSWORD"] = password
        for kv in self.head["switches"] + self.env:
            k, v = kv.split("=", 1)
            if k[:2] in ("A:", "B:"):
                if k[0] != x:
                    continue
                k = k[2:]
            env[k] = v
        exe = os.path.join(hk.exe_dir(g), "hookshot.exe" if WINDOWS else "hookshot")
        go = open(os.path.join(self.out, "game-out-%s-%d.txt" % (x, self.runs[x])), "wb")
        kw = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if WINDOWS else {"start_new_session": True}
        self.procs[x] = subprocess.Popen([exe], cwd=hk.exe_dir(g), env=env, stdin=subprocess.DEVNULL, stdout=go,
                                         stderr=subprocess.STDOUT, **kw)
        go.close()
        self.say("%s: started pid %d, bridge %s" % (x, self.procs[x].pid, sock))
        self.bridges[x] = (sock, hk.Bridge.connect(sock, timeout=120))
        if wait:
            self.wait_joined(x)

    def wait_joined(self, x):
        t0 = time.monotonic()
        while JOINED not in self.log(x):
            if not self.alive(x):
                raise RuntimeError("game %s closed while starting" % x)
            if time.monotonic() - t0 > self.timeout:
                raise TimeoutError("game %s did not reach '%s' in %d s" % (x, JOINED, self.timeout))
            time.sleep(0.5)
        # and drawing: a game whose shaders still compile reads no controller (a cold cache: minutes)
        waited = self.hk.wait_renderer(lambda: self.log(x), lambda: self.alive(x), self.timeout)
        self.say("%s: renderer ready (%.0f s waited)" % (x, waited))

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
            with open(os.path.join(HERE, "scenarios", f), encoding="utf-8") as fh:
                parsed[x] = hk.parse(fh.read())

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

    def shot(self, name):
        for y, (_, b) in list(self.bridges.items()):
            if not self.alive(y):
                continue
            path = os.path.join(self.out, "%s-%s.png" % (name, y))
            try:
                b.request("shot", reply_timeout=30.0, path=path, timeout=20)
            except Exception as e:
                self.say("%s shot %s: %s" % (y, os.path.basename(path), e))

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
        args = [self.server_exe, "--port", str(self.port)]
        if os.environ.get("OOT_COOP_TEST_PASSWORD"):
            args += ["--password-env", "OOT_COOP_TEST_PASSWORD"]
        args += self.head.get("server", [])   # the steps file's `#! server` arguments
        with open(os.path.join(self.out, "server-out%s.txt" % sfx), "w") as so:
            self.server = subprocess.Popen(args, stdout=so, stderr=subprocess.STDOUT)
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
    a = ap.parse_args(argv)
    if a.cmd == "check":
        bad = 0
        for path in a.files:
            with open(path, encoding="utf-8") as f:
                text = f.read()
            try:
                steps = parse_steps(text)
            except StepsError as e:
                print("%s: %s" % (path, e))
                bad += 1
                continue
            head = parse_header(text)
            plays = sorted({f for s in steps if s.verb == "play" for _, f in s.args})
            print("%s: %d steps (%s)%s%s" % (path, len(steps), " ".join(sorted({s.verb for s in steps})),
                                             ", plays " + " ".join(plays) if plays else "",
                                             ", switches " + ",".join(head["switches"]) if head["switches"] else ""))
        return 1 if bad else 0
    for kv in a.env:
        if "=" not in kv:
            ap.error("--env NAME=VALUE")
    with open(a.steps, encoding="utf-8") as f:
        text = f.read()
    try:
        steps = parse_steps(text)
    except StepsError as e:
        print("hs_steps.py: %s: %s" % (a.steps, e))
        return 1
    head = parse_header(text)
    if a.dry_run:
        print("steps %s: %d steps; games %s / %s; server %s port %d%s; pipes %s-A/-B; out %s" % (
            a.steps, len(steps), a.game_a, a.game_b, a.server, a.port,
            ", join password from OOT_COOP_TEST_PASSWORD" if os.environ.get("OOT_COOP_TEST_PASSWORD") else "",
            a.pipe, a.out))
        _dry(steps)
        return 0
    os.makedirs(a.out, exist_ok=True)
    sys.path.insert(0, HERE)
    import hookshot as hk  # noqa: E402 (tools/autotest/hookshot.py: the bridge, the player, the load)
    say = hk.Log(os.path.join(a.out, "steps.txt"), echo=print)
    games = HookshotGames(hk, {"A": os.path.abspath(a.game_a), "B": os.path.abspath(a.game_b)}, a.out, a.server,
                          a.port, a.pipe, a.env, head["timeout"] or a.timeout, head, restore=not a.no_restore, say=say)
    fails = 1
    try:
        games.server_start()
        fails = Runner(games, a.out, say=say, expect_wait=a.expect_wait).run(steps)
    finally:
        games.finish()
    say("steps done: %d FAIL" % fails)
    return 1 if fails else 0


def _dry(steps):
    """The plan without an out folder: what each step would do."""
    say = print
    games = DryGames(say)
    for s in steps:
        say("step %d: %s" % (s.n, s.line))
        if s.verb == "start":
            for x in s.args:
                games.start(x, "")
        elif s.verb == "load":
            games.load(s.args)
        elif s.verb == "play":
            games.play(dict(s.args), "")
        elif s.verb in ("close", "kill"):
            getattr(games, s.verb)(s.args[0])
        elif s.verb == "server":
            (games.server_stop if s.args[0] == "stop" else games.server_start)()
        elif s.verb == "try":
            say("  would start %s and check for '%s' within 40 s, then end it" % (s.args[0], s.args[1]))
        elif s.verb == "expect":
            say("  would wait until %s puppets and fairies, then shoot %s" % (
                " ".join("%s=%d" % kv for kv in s.args[1].items()), s.args[0]))
        elif s.verb == "lines":
            say("  would count %s's log lines matching '%s' (%s=%d)" % (s.args[1], s.pattern, s.args[2], s.args[3]))
        elif s.verb == "password":
            say("  %s's join password from its next start: %s" % (s.args[0], "set" if s.args[1] else "none"))
        else:
            say("  %s" % s.line)


if __name__ == "__main__":
    sys.exit(main())
