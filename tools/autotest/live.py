"""A live two-game play-test session: start the server and both games once, then act and look one step at a time.

The two-game runner (hookshot.py run) plays a whole script blind. This keeps the games open after their boot script
(`run --keep-open`) and lets a tester (a person, or Claude through tools/autotest/mcp_playtest.py) take one step at a
time and see the result before the next: the step's own script lines (hookshot.py's test script format, `;` between
lines), then what each game says now as JSON (the control bridge's `status` snapshot: map, Link's place, facing, life,
event, loading, prompt) and the new co-op log lines since the last look. A checkpoint writes each game's shared flags
(the module's `dump TAG` test command) and a screenshot of each game beside the session;
tools/autotest/desync_check.py compares them.

The games run in real time between steps (the bridge has no frame step): a step's lines are what the game does, and
a long think between steps is time the game goes on (Link stands still, enemies do not). Standard library only.

A session on a test lane (`--lane N`, tools/autotest/lanes.py) uses that lane's game pair, port and control pipes and
holds its lock (laneN.lock) for the whole session, not only the boot. Every session has a watchdog (a process of its
own, `live.py watchdog`, started by `start`): it holds the lane lock under its own pid, and at the session's end (`stop`,
the maximum duration, a tester idle too long, a game that ended, or the watchdog itself stopped by a signal) it quits
both games, kills what did not quit and the server, and lets the lock go. A watchdog killed outright cannot clean up,
but its lock goes stale with it (the lock names its pid). Each game's save is restored to its baseline before the
boot, as the lane runs do (`--no-restore` keeps whatever the copies hold).

    python -I tools/autotest/live.py start --session DIR [--server coop_server.exe] (--lane N [--only A|B] |
                                           --game-a A --game-b B | --game-a A | --game-b B)
                                           [--boot-a boot.A.txt --boot-b boot.B.txt] [--env K=V ...]
                                           [--max-minutes 30] [--idle-minutes 10] [--lock-wait S] [--no-restore]
    python -I tools/autotest/live.py do --session DIR A "goto 10.0 20.0 0.5 300; press A"
    python -I tools/autotest/live.py state --session DIR [A|B] [--since ID]
    python -I tools/autotest/live.py replay [FILE ...] [--session DIR] [--dry-run]   (run a call log again)
    python -I tools/autotest/live.py shot --session DIR [A|B]
    python -I tools/autotest/live.py checkpoint --session DIR TAG
    python -I tools/autotest/live.py stop --session DIR
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hookshot  # noqa: E402  (the same folder)
import kit_tools  # noqa: E402
import lanes  # noqa: E402  (the same folder; it brings tools/hookshot/hs.py)

TAGS = ("A", "B")
PIPE = "coop-live"   # without a lane
PORT = 27593         # without a lane
# Log lines a tester never needs (renderer, GPU, controller and frame-time chatter).
NOISE = re.compile(r"\] (vk:|nvn:|hid:|vi:|vcores:|barrier:|\[debug\])")
MAX_LINES = 80
MAX_MINUTES = 30     # a session's whole length (the lane lock goes stale after 4 h anyway; a slot is shorter)
BACKUP = lanes.LIVE_BACKUP   # --dll: the lane's own module beside it while a session runs (lanes.py swap_in)
IDLE_MINUTES = 10    # no tester command for this long after the boot: the tester is gone (hand-off, crash, pause)
WATCH_EVERY = 5.0    # s between the watchdog's looks
SERVER_PASSWORD = "autotest-pass"   # the session's join password: given to the boot, kept in session.json (runner)
QUIT_WAIT = 15.0     # s a game gets to quit before it is killed


def session_file(d):
    return os.path.join(d, "session.json")


def watchdog_file(d):
    return os.path.join(d, "watchdog.json")


def stop_file(d):
    return os.path.join(d, "stop-request")


def active_file(d):
    return os.path.join(d, "active")


def read_json(path, default=None):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
    for i in range(20):   # (Windows: a reader holding the old file a moment refuses the replace)
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if i == 19:
                raise
            time.sleep(0.05)


def load(d):
    with open(session_file(d), encoding="utf-8") as f:
        return json.load(f)


def save(d, s):
    write_json(session_file(d), s)


def lock_owner(d):
    return "live.py session %s" % d


def touch(d):
    """A tester command was given now (the watchdog's idle clock)."""
    with open(active_file(d), "a", encoding="utf-8"):
        pass
    os.utime(active_file(d), None)


def activity(d, s):
    try:
        return os.path.getmtime(active_file(d))
    except OSError:
        return s.get("started", 0)


def game_pids(d):
    """({tag: pid}, server pid) from the runner's pids.json (hookshot.py run --keep-open)."""
    p = read_json(os.path.join(d, "pids.json"), {}) or {}
    return {x: int(v) for x, v in (p.get("games") or {}).items() if v}, (int(p["server"]) if p.get("server") else None)


def ended(d):
    """Why the session ended ('' while it runs)."""
    return (read_json(watchdog_file(d), {}) or {}).get("ended") or ""


def open_session(d):
    """The session for a tester command: refused once it ended (the games are closed and the lane is free)."""
    s = load(d)
    why = ended(d)
    if why:
        raise SystemExit("the session ended (%s): start a new one" % why)
    touch(d)
    return s


def resolve(a):
    """({tag: game folder}, port, pipe): the lane's (tools/autotest/lanes.py) unless given. One game alone: only
    --game-a or only --game-b, or --only A / B (a lane's one game); the other game is not started or touched."""
    only = getattr(a, "only", None)
    if a.lane:
        L = lanes.lane(a.lane)
        games, port, pipe = {"A": a.game_a or L["a"], "B": a.game_b or L["b"]}, a.port or L["port"], L["pipe"]
    else:
        if not (a.game_a or a.game_b):
            raise SystemExit("--game-a and/or --game-b (or --lane N)")
        games, port, pipe = {x: g for x, g in (("A", a.game_a), ("B", a.game_b)) if g}, a.port or PORT, PIPE
    if only:
        if only not in games:
            raise SystemExit("--only %s: no game %s given" % (only, only))
        games = {only: games[only]}
    return games, port, pipe


def session_tags(s):
    """The games the session started, A before B (one alone, or both)."""
    return [x for x in TAGS if x in (s.get("games") or {})]


def runner_argv(a, games, port, pipe, d):
    """hookshot.py run's arguments for the boot. No --lane: the watchdog holds the lane lock for the whole session (the
    runner would let it go when the boot ends, with the games still open). No --server: the games use their coop.ini
    (a game alone needs none)."""
    argv = ["run"]
    for x in TAGS:
        if x in games:
            argv += ["--game-%s" % x.lower(), games[x]]
    if a.server:
        argv += ["--server", a.server]
    argv += ["--port", str(port), "--out", d, "--name", "live", "--pipe", pipe, "--keep-open", "--every", "0",
             "--password", SERVER_PASSWORD, "--scenario-timeout", str(a.boot_timeout)]
    if a.no_restore:
        argv.append("--no-restore")
    if a.boot_a and "A" in games:
        argv += ["--script-a", a.boot_a]
    if a.boot_b and "B" in games:
        argv += ["--script-b", a.boot_b]
    if a.no_load:
        argv.append("--no-load")
    elif a.load:
        argv += ["--load", a.load]
    if getattr(a, "playtest", False):
        argv += ["--playtest-ports", str(playtest_base(a.lane))]
    for kv in a.env:
        argv += ["--env", kv]
    if getattr(a, "display", None):
        argv += ["--display", a.display]
    return argv


def playtest_base(lane):
    """The play-test bridge's port base: game A (p1) listens on base + 1, B (p2) on base + 2
    (tools/autotest/playtest_client.py port_for: 5000 + N, or 5000 + 10*L + N on lane L)."""
    return 5000 + 10 * int(lane or 0)


def spawn_watchdog(d):
    # It must outlive the shell that started it, and it must not make a window flash. DETACHED_PROCESS has no console, so
    # every console program the watchdog (or lanes.running_in, tasklist, git ...) starts gets a NEW visible console
    # window: a chain of flashing windows through a run. CREATE_NO_WINDOW gives it a hidden console its children share;
    # CREATE_NEW_PROCESS_GROUP keeps the shell's Ctrl+C away from it, and with its own console the shell's closing does
    # not end it.
    kw = ({"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP | lanes.hs.CREATE_NO_WINDOW}
          if os.name == "nt" else {"start_new_session": True})
    with open(os.path.join(d, "watchdog.txt"), "ab") as log:
        return subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), "watchdog", "--session", d],
                                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, close_fds=True, **kw)


def wait_lock(d, proc, timeout, clock=time.monotonic, sleep=time.sleep):
    """The watchdog's word on the lock: 'held', 'none' (no lane), or why it could not start."""
    end = clock() + timeout
    while clock() < end:
        w = read_json(watchdog_file(d), {}) or {}
        if w.get("ended"):
            return w["ended"]
        if w.get("lock") in ("held", "none"):
            return w["lock"]
        if proc.poll() is not None:
            return "the watchdog ended (exit %s, see watchdog.txt)" % proc.returncode
        sleep(0.2)
    return "the watchdog did not answer in %d s" % timeout


def ends_text(max_minutes, idle_minutes):
    """What the answer to `start` says about when the session ends on its own (0 = no limit)."""
    parts = []
    if max_minutes:
        parts.append("after %d min" % max_minutes)
    if idle_minutes:
        parts.append("%d min without a command" % idle_minutes)
    return (", or ".join(parts) if parts else "never by itself") + " (live.py stop, or a game that ended)"


def start(a):
    if getattr(a, "dll", None) and not a.lane:
        raise SystemExit("--dll needs --lane (it swaps a lane's own module for the session)")
    d = os.path.abspath(a.session)
    os.makedirs(d, exist_ok=True)
    w = read_json(watchdog_file(d), {}) or {}
    if w.get("pid") and not w.get("ended") and lanes.hs.pid_alive(int(w["pid"])):
        raise SystemExit("a session is still open in %s: stop it first" % d)
    games, port, pipe = resolve(a)
    if getattr(a, "dll", None) and len(games) == 1:
        raise SystemExit("--dll swaps the module of both games of the lane: not with one game alone")
    for f in (watchdog_file(d), stop_file(d), os.path.join(d, "pids.json")):
        try:
            os.remove(f)
        except OSError:
            pass
    s = {"started": time.time(), "dir": d, "lane": a.lane, "port": port, "booted": False,
         "max_s": int(a.max_minutes * 60), "idle_s": int(a.idle_minutes * 60), "lock_wait": a.lock_wait,
         "games": {}, "offsets": {}, "planned": {},
         "runner": {"game_a": games.get("A") and os.path.abspath(games["A"]),
                    "game_b": games.get("B") and os.path.abspath(games["B"]), "port": port, "pipe": pipe,
                    "server": a.server and os.path.abspath(a.server), "env": list(a.env),
                    "display": getattr(a, "display", None), "boot_timeout": a.boot_timeout, "password": SERVER_PASSWORD,
                    "playtest": playtest_base(a.lane) if getattr(a, "playtest", False) else 0}}
    if a.lane and len(games) == 1:
        s["game_lock"] = next(iter(games))   # one game of the lane: its own lock, the other game stays free
    for x, g in games.items():
        i = TAGS.index(x)   # (B alone keeps B's pipe and port: another worker may drive A)
        s["games"][x] = {"folder": os.path.abspath(g), "pipe": "%s-%s" % (pipe, x),
                         "commands": os.path.join(d, "commands-%s.txt" % x),
                         "log": os.path.join(d, "game-out-%s.txt" % x)}
        if getattr(a, "playtest", False):
            s["games"][x]["playtest_port"] = playtest_base(a.lane) + i + 1
    save(d, s)
    touch(d)
    lock = wait_lock(d, spawn_watchdog(d), a.lock_wait + 30)
    if lock not in ("held", "none"):
        return {"ok": False, "session": d, "lane": a.lane, "error": lock}
    if getattr(a, "dll", None):
        try:
            install_dll(s, a.dll)
            save(d, s)
        except (lanes.Fail, OSError) as e:
            save(d, s)
            out = {"ok": False, "session": d, "lane": a.lane, "error": "--dll: %s" % e}
            out["stop"] = stop(argparse.Namespace(session=d))
            return out
    code = hookshot.main(runner_argv(a, games, port, pipe, d))
    s = load(d)
    for x in session_tags(s):
        log = s["games"][x]["log"]
        s["offsets"][x] = os.path.getsize(log) if os.path.exists(log) else 0
    s["booted"] = True
    save(d, s)
    touch(d)
    out = {"ok": code == 0, "session": d, "lane": a.lane, "lock": lock, "port": port,
           "boot": "played" if code == 0 else "FAILED (see steps.txt): the session is closed",
           "ends": ends_text(a.max_minutes, a.idle_minutes)}
    if getattr(a, "playtest", False):
        out["playtest"] = {"p%d" % (TAGS.index(x) + 1): s["games"][x]["playtest_port"] for x in session_tags(s)}
    if code != 0:
        out["stop"] = stop(argparse.Namespace(session=d))
    return out


def bridge(s, x):
    return hookshot.Bridge.connect(s["games"][x]["pipe"], timeout=15)


def new_lines(d, s, x, match=None):
    """The game's co-op log lines since the last look (noise left out), at most MAX_LINES (the newest).
    With `match` (a text), only the lines holding it, the OLDEST MAX_LINES of them; the cursor given moves past the lines
    looked at only when all their matches were returned (more than MAX_LINES: it stops after the last one returned, so
    no match is lost). The caller keeps this cursor apart from the plain one: the lines a match left out are still there
    for a plain look."""
    path = s["games"][x]["log"]
    if match:
        return _new_matching(path, s, x, match)
    try:
        with open(path, "rb") as f:
            f.seek(s["offsets"].get(x, 0))
            data = f.read()
            s["offsets"][x] = f.tell()
    except OSError:
        return []
    lines = [ln for ln in data.decode("utf-8", "replace").splitlines() if "coop:" in ln and not NOISE.search(ln)]
    lines = [re.sub(r"^\[hos [^\]]*\] ", "", ln) for ln in lines]
    if len(lines) > MAX_LINES:
        lines = ["(%d older lines left out)" % (len(lines) - MAX_LINES)] + lines[-MAX_LINES:]
    return lines


def _new_matching(path, s, x, match):
    try:
        with open(path, "rb") as f:
            start = s["offsets"].get(x, 0)
            f.seek(start)
            data = f.read()
    except OSError:
        return []
    out, end, pos, capped = [], start, 0, False
    for raw in data.splitlines(keepends=True):
        if not raw.endswith(b"\n"):
            break   # a half-written last line: left for the next look
        pos += len(raw)
        ln = raw.decode("utf-8", "replace").rstrip("\r\n")
        if "coop:" in ln and not NOISE.search(ln) and match in ln:
            out.append(re.sub(r"^\[hos [^\]]*\] ", "", ln))
            if len(out) >= MAX_LINES:
                end, capped = start + pos, True
                break
    if not capped:
        end = start + pos   # every complete line was looked at, and every match is in `out`
    s["offsets"][x] = end
    return out


def snapshot(b):
    r = b.request("status", reply_timeout=10.0)
    g = r.get("game") or {}
    out = {"frame": r.get("frame"), "state": r.get("state"), "map": r.get("map")}
    for k in ("pos", "facing", "camera", "life", "max_life", "rupees", "age", "event", "loading", "prompt"):
        if k in g:
            out[k] = g[k]
    return out


def menu_of(port, client=None):
    """What co-op's menus show in a game started with the play-test bridge (docs/playtest.md "menu"): {"menu": ...}, or
    {"menu_error": why}. A bridge shot leaves out Longshot's overlays (the co-op pages), so this is how they are read.
    The game is not held for it (one `state` on a connection of its own)."""
    import playtest_client
    c = client or playtest_client.Client(port, timeout=5.0, hold=False)
    try:
        g = playtest_client.game_of(c.request("state"))
        return {"menu": g["menu"]} if "menu" in g else {"menu_error": "the bridge's state has no menu (an older module?)"}
    except Exception as e:  # (a game without the bridge, or busy: said, the look still comes back)
        return {"menu_error": "%s: %s" % (e.__class__.__name__, e)}
    finally:
        c.close()


def look(d, s, x):
    b = bridge(s, x)
    try:
        st = snapshot(b)
    finally:
        b.close()
    port = s["games"][x].get("playtest_port")
    if port:
        st.update(menu_of(port))
    st["log"] = new_lines(d, s, x)
    return st


def state(a):
    d = os.path.abspath(a.session)
    s = open_session(d)
    if a.player and a.player not in session_tags(s):
        return {"error": "game %s is not in this session (it has %s)" % (a.player, " and ".join(session_tags(s)))}
    out = {x: look(d, s, x) for x in ([a.player] if a.player else session_tags(s))}
    save(d, s)
    # every look gets a `state_id`; with `since` (an earlier look's id) only what changed comes back (the new log lines
    # are new since the last look already, so they are always whole)
    return kit_tools.StateHistory(os.path.join(d, "state-history.json")).reply(out, getattr(a, "since", None))


def do(a):
    d = os.path.abspath(a.session)
    s = open_session(d)
    x = a.player
    text = "\n".join(part.strip() for part in a.script.split(";") if part.strip()) + "\n"
    try:
        steps = hookshot.parse(text)
    except hookshot.ScriptError as e:   # (a warp to a scene the module would refuse: nothing is sent)
        return {"error": "ScriptError: %s" % e}
    if x not in session_tags(s):
        return {"error": "game %s is not in this session (it has %s)" % (x, " and ".join(session_tags(s)))}
    b = bridge(s, x)
    err = None
    try:
        p = hookshot.Player(b, hookshot.Log(os.path.join(d, "player-%s.txt" % x)),
                            connect=lambda: bridge(s, x), commands=s["games"][x]["commands"])
        p.play(steps)
    except Exception as e:  # (a step the game refused: said, and the look still comes back)
        err = "%s: %s" % (e.__class__.__name__, e)
    finally:
        b.close()
    touch(d)   # (a long step is not idle time)
    out = {x: look(d, s, x)}
    if err:
        out["error"] = err
    save(d, s)
    return out


def take_shot(s, x, path):
    """A screenshot of game x at path: the path, or why there is none."""
    b = bridge(s, x)
    try:
        b.request("shot", reply_timeout=30.0, path=path, timeout=20)
        return path
    except Exception as e:
        return "no shot: %s" % e
    finally:
        b.close()


def shot(a):
    d = os.path.abspath(a.session)
    s = open_session(d)
    os.makedirs(os.path.join(d, "shots"), exist_ok=True)
    return {x: take_shot(s, x, os.path.join(d, "shots", "%s-%d.png" % (x, int(time.time() * 1000))))
            for x in ([a.player] if a.player else session_tags(s))}


def checkpoint(a):
    d = os.path.abspath(a.session)
    if not re.fullmatch(r"[A-Za-z0-9_-]+", a.tag):
        raise SystemExit("a tag is letters, digits, - and _")
    s = open_session(d)
    cps = os.path.join(d, "checkpoints")
    os.makedirs(cps, exist_ok=True)
    out = {"tag": a.tag}
    for x in session_tags(s):
        dump = s["games"][x]["commands"] + ".dump-%s.json" % a.tag
        try:
            os.remove(dump)
        except OSError:
            pass
    for x in session_tags(s):
        b = bridge(s, x)
        try:
            p = hookshot.Player(b, hookshot.Log(os.path.join(d, "player-%s.txt" % x)),
                                commands=s["games"][x]["commands"])
            p.play(hookshot.parse("dump %s\n" % a.tag))
            out[x] = {"status": snapshot(b)}
        finally:
            b.close()
    for x in session_tags(s):   # (the picture proves what the flags say: owner rule, no PASS from data alone)
        got = take_shot(s, x, os.path.join(cps, "%s-%s.png" % (a.tag, x)))
        out[x]["shot" if not got.startswith("no shot") else "shot_error"] = got
    for x in session_tags(s):
        dump = s["games"][x]["commands"] + ".dump-%s.json" % a.tag
        deadline = time.monotonic() + 10
        while not os.path.exists(dump) and time.monotonic() < deadline:
            time.sleep(0.1)
        try:
            with open(dump, encoding="utf-8") as f:
                out[x]["flags"] = json.load(f).get("flags", {})
        except (OSError, ValueError) as e:
            out[x]["flags"] = None
            out[x]["error"] = "no dump: %s" % e
    with open(os.path.join(cps, "%s.json" % a.tag), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    save(d, s)
    return {"tag": a.tag, **{x: {"map": out[x]["status"].get("map"), "flags": len(out[x]["flags"] or {}),
                                 "shot": out[x].get("shot") or out[x].get("shot_error")} for x in session_tags(s)}}


# --- the watchdog ------------------------------------------------------------------------------------------------

def quit_game(s, x):
    b = bridge(s, x)
    try:
        b.request("quit", reply_timeout=10.0, check=False)
    finally:
        b.close()


def kill(pid):
    try:
        os.kill(pid, signal.SIGTERM)   # (Windows: TerminateProcess)
    except OSError:
        pass


def release_lock(n, owner, game=None):
    return lanes.hs.lock_release(lanes.hs.main_root(lanes.REPO), lanes.lock_name(n, game), owner, say=lambda *_: None)


# --- a game or the server closed and started again inside one session (quit, server, relaunch) ----------------------
# A planned exit is a step of the test, not the session's end: `planned` in session.json names what is down on purpose,
# and the watchdog's "game X ended" leaves those out. `relaunch` starts the game again through the runner (hookshot.py
# run --only X, into this session's folder: its pid is added to pids.json) with the session's own switches and ports.

def write_pids(d, games=None, server=False):
    """pids.json with these games' pids replaced (a pid of None: down) and, unless False, the server's."""
    p = read_json(os.path.join(d, "pids.json"), {}) or {}
    g = dict(p.get("games") or {})
    for x, pid in (games or {}).items():
        g[x] = pid
    p["games"] = g
    if server is not False:
        p["server"] = server
    write_json(os.path.join(d, "pids.json"), p)


def plan(d, what, why):
    """Marks `what` (A, B or server) as down on purpose (why) or, with why None, as back."""
    s = load(d)
    planned = s.setdefault("planned", {})
    if why:
        planned[what] = why
    else:
        planned.pop(what, None)
    save(d, s)
    return s


def quit_one(a, alive=None, quit=None, killer=None, wait=QUIT_WAIT, clock=time.monotonic, sleep=time.sleep):
    """Closes one game in the middle of a session (asked to quit, or `--kill`ed as a crash would end it)."""
    alive = alive or lanes.hs.pid_alive
    quit, killer = quit or quit_game, killer or kill
    d = os.path.abspath(a.session)
    open_session(d)
    if a.player not in session_tags(load(d)):
        return {"error": "game %s is not in this session" % a.player}
    s = plan(d, a.player, "killed" if a.kill else "quit")
    pid = game_pids(d)[0].get(a.player)
    if not pid or not alive(pid):
        write_pids(d, {a.player: None})
        return {"player": a.player, "pid": pid, "how": "was not running"}
    how = "killed"
    if a.kill:
        killer(pid)
    else:
        try:
            quit(s, a.player)
            how = "quit asked"
        except Exception as e:
            how = "quit not reached (%s)" % e
    end = clock() + wait
    while alive(pid) and clock() < end:
        sleep(0.5)
    if alive(pid):
        killer(pid)
        how += ", killed after %d s" % wait
    write_pids(d, {a.player: None})
    touch(d)
    return {"player": a.player, "pid": pid, "how": how}


def server_cmd(a, alive=None, killer=None, popen=None, sleep=time.sleep):
    """Stops the session's server, or starts it again (same exe, port and password as the boot's)."""
    alive = alive or lanes.hs.pid_alive
    killer = killer or kill
    d = os.path.abspath(a.session)
    s = open_session(d)
    r = s.get("runner")
    if not r:
        raise SystemExit("this session was started by an older live.py: no server settings kept")
    pid = game_pids(d)[1]
    if a.action == "stop":
        plan(d, "server", "stopped")
        if pid and alive(pid):
            killer(pid)
        write_pids(d, server=None)
        return {"server": "stopped", "pid": pid}
    if not r.get("server"):
        return {"error": "this session started no server (the games use their coop.ini)"}
    if pid and alive(pid):
        return {"error": "the server still runs (pid %d): live.py server stop first" % pid}
    n = len([f for f in os.listdir(d) if f.startswith("server-out")])
    with open(os.path.join(d, "server-out-%d.txt" % n), "w") as so:
        env = dict(os.environ, OOT_COOP_TEST_PASSWORD=r.get("password", SERVER_PASSWORD))   # (in the environment, never on the command line)
        proc = (popen or lanes.hs.popen_quiet)(lanes.hs.server_bind([r["server"], "--port", str(r["port"]), "--password-env",
                                                "OOT_COOP_TEST_PASSWORD"]), stdout=so, stderr=subprocess.STDOUT, env=env)
    write_pids(d, server=proc.pid)
    plan(d, "server", None)
    sleep(1)
    return {"server": "started", "pid": proc.pid, "port": r["port"]}


def relaunch_argv(r, d, x, boot=None, no_load=False, restore=False):
    """hookshot.py run's arguments that start game x again into session folder d (r: the session's `runner`)."""
    argv = ["run", "--only", x]
    for y in TAGS:
        if r.get("game_" + y.lower()):
            argv += ["--game-" + y.lower(), r["game_" + y.lower()]]
    argv += ["--port", str(r["port"]),
            "--out", d, "--name", "live-%s" % x.lower(), "--pipe", r["pipe"], "--keep-open", "--every", "0",
            "--password", r.get("password", SERVER_PASSWORD),
            "--scenario-timeout", str(r["boot_timeout"])]
    if not restore:
        argv.append("--no-restore")   # (the point of a rejoin is the save the game has now)
    if boot:
        argv += ["--script-%s" % x.lower(), boot]
    if no_load:
        argv.append("--no-load")
    if r.get("playtest"):
        argv += ["--playtest-ports", str(r["playtest"])]
    for kv in r.get("env") or []:
        argv += ["--env", kv]
    if r.get("display"):
        argv += ["--display", r["display"]]
    return argv


def relaunch(a, alive=None, runner=None):
    """Starts a game that `quit` closed (or that crashed) again, joining the session's server."""
    alive = alive or lanes.hs.pid_alive
    d = os.path.abspath(a.session)
    s = open_session(d)
    r = s.get("runner")
    if not r:
        raise SystemExit("this session was started by an older live.py: no runner settings kept")
    x = a.player
    if x not in session_tags(s):
        return {"error": "game %s is not in this session (it has %s)" % (x, " and ".join(session_tags(s)))}
    pid = game_pids(d)[0].get(x)
    if pid and alive(pid):
        return {"error": "game %s still runs (pid %d): live.py quit first" % (x, pid)}
    plan(d, x, "relaunch")
    log = s["games"][x]["log"]
    if os.path.exists(log):   # the runner writes game-out-X.txt from the start: the old one is kept beside it
        n = 1
        while os.path.exists("%s.%d" % (log, n)):
            n += 1
        os.replace(log, "%s.%d" % (log, n))
    code = (runner or hookshot.main)(relaunch_argv(r, d, x, a.boot, a.no_load, a.restore))
    s = load(d)
    s["offsets"][x] = 0
    save(d, s)
    if code == 0:
        plan(d, x, None)
    touch(d)
    return {"ok": code == 0, "player": x, "pid": game_pids(d)[0].get(x),
            "boot": "played" if code == 0 else "FAILED (see steps.txt): the game stays down"}


def wait_log(session, x, pattern, mn=1, mx=None, timeout=60.0, clock=time.monotonic, sleep=time.sleep):
    """Waits until game x's log (this run of it: game-out-X.txt) has at least `mn` lines matching `pattern`, at most
    `timeout` s; `mx` (0: none) fails as soon as more match. {ok, count, lines (the last 5 matches)}."""
    d = os.path.abspath(session)
    s = open_session(d)
    rx = re.compile(pattern)
    end = clock() + float(timeout)
    while True:
        hits = [ln for ln in hookshot.read_text(s["games"][x]["log"]).splitlines() if rx.search(ln)]
        if mx is not None and len(hits) > mx:
            return {"ok": False, "count": len(hits), "lines": hits[-5:], "why": "more than %d match" % mx}
        if len(hits) >= mn and (mx is None or clock() >= end or mn > 0):
            return {"ok": True, "count": len(hits), "lines": hits[-5:]}
        if clock() >= end:
            return {"ok": False, "count": len(hits), "lines": hits[-5:],
                    "why": "%d of %d after %g s" % (len(hits), mn, float(timeout))}
        touch(d)
        sleep(1.0)


def install_dll(s, dll, kit=None, swap=None):
    """--dll: `dll` in the lane for this session only (lanes.py swap_in: the lane's own kept beside it, also when the
    lane had none); close() puts the lane's own back (restore_dlls). lanes.py owns the lane's module state."""
    s["dll"] = {"installed": os.path.abspath(dll)}   # (written first: a half-done swap is still put back at close)
    return (swap or (lambda n, path: lanes.swap_in(n, path, kit=kit)))(s["lane"], dll)


def restore_dlls(s, kit=None, swap_back=None):
    """The lane's own module back after a --dll session (what close() does): one word per game, joined."""
    if not s.get("dll"):
        return None
    return ", ".join((swap_back or (lambda n: lanes.swap_back(n, kit=kit)))(s["lane"]))


def close(d, s, alive=None, quit=quit_game, killer=kill, release=release_lock, wait=QUIT_WAIT,
          clock=time.monotonic, sleep=time.sleep):
    """Ends a session: both games asked to quit, what is still running after `wait` s killed, the server killed, the
    lane lock let go. What happened to each, as a dict."""
    alive = alive or lanes.hs.pid_alive
    games, server = game_pids(d)
    out = {}
    for x in session_tags(s):
        try:
            quit(s, x)
            out[x] = "quit asked"
        except Exception as e:
            out[x] = "not reached: %s" % e
    end = clock() + wait
    while clock() < end and any(alive(p) for p in games.values()):
        sleep(0.5)
    for x, p in games.items():
        if alive(p):
            killer(p)
            out[x] = out.get(x, "") + ", killed"
    if server and alive(server):
        killer(server)
        out["server"] = "killed"
    if s.get("dll"):
        try:
            out["dll"] = restore_dlls(s)
        except OSError as e:
            out["dll"] = "NOT restored: %s" % e
    if s.get("lane"):
        g = s.get("game_lock")
        out["lock"] = "%s.lock %s" % (lanes.lock_name(s["lane"], g),
                                      "released" if release(s["lane"], lock_owner(d), g) else "held by another")
    return out


def watch(d, clock=time.time, sleep=time.sleep, alive=None, every=WATCH_EVERY):
    """Waits for the session's end; returns why it ended."""
    alive = alive or lanes.hs.pid_alive
    s = load(d)
    while True:
        try:
            s = load(d)
        except (OSError, ValueError):
            pass   # (being replaced this moment: the last one read)
        now = clock()
        if os.path.exists(stop_file(d)):
            return "stop asked"
        if s["max_s"] and now - s["started"] >= s["max_s"]:   # (0: no limit)
            return "maximum duration (%d min) reached" % (s["max_s"] // 60)
        if s.get("booted"):
            if s.get("idle_s") and now - activity(d, s) >= s["idle_s"]:
                return "no tester command for %d min" % (s["idle_s"] // 60)
            planned = s.get("planned") or {}   # (a game closed on purpose: live.py quit / relaunch)
            gone = [x for x, p in sorted(game_pids(d)[0].items()) if p and not planned.get(x) and not alive(p)]
            if gone:
                return "game %s ended" % " and ".join(gone)
        sleep(every)


def run_watchdog(d, acquire=None, watcher=watch, closer=close, pid=None):
    """The watchdog's whole life: the lane lock taken under its pid, the wait, the clean-up (also on an error or a
    signal). Writes watchdog.json: pid, lock ('held', 'none', 'refused'), ended (why), closed (what close did)."""
    pid = os.getpid() if pid is None else pid
    s = load(d)
    w = {"pid": pid, "lock": None, "ended": None}
    if s.get("lane"):
        acquire = acquire or (lambda n, owner, wait, game=None: lanes.lock_acquire(n, owner, wait=wait, pid=pid,
                                                                                    game=game))
        if not acquire(s["lane"], lock_owner(d), s.get("lock_wait", 0), s.get("game_lock")):
            w.update(lock="refused", ended="lane %d is not free (lanes.py lock status)" % s["lane"])
            write_json(watchdog_file(d), w)
            return 1
        w["lock"] = "held"
    else:
        w["lock"] = "none"
    write_json(watchdog_file(d), w)
    why = "the watchdog failed"
    try:
        why = watcher(d)
    except SystemExit as e:   # (a signal: see watchdog())
        why = "watchdog stopped (%s)" % e
    except Exception as e:
        why = "watchdog error: %s: %s" % (e.__class__.__name__, e)
    finally:
        w["closed"] = closer(d, load(d))
        w["ended"] = why
        write_json(watchdog_file(d), w)
    return 0


def watchdog(a):
    def bye(signum, _frame):
        raise SystemExit("signal %d" % signum)
    for name in ("SIGTERM", "SIGBREAK", "SIGINT"):
        if hasattr(signal, name):
            try:
                signal.signal(getattr(signal, name), bye)
            except (ValueError, OSError):
                pass
    return {"watchdog": run_watchdog(os.path.abspath(a.session))}


def stop(a, wait=60.0, clock=time.monotonic, sleep=time.sleep, alive=None):
    """Asks the watchdog to end the session and waits for it; without a live watchdog, closes the session here."""
    alive = alive or lanes.hs.pid_alive
    d = os.path.abspath(a.session)
    with open(stop_file(d), "w", encoding="utf-8"):
        pass
    w = read_json(watchdog_file(d), {}) or {}
    end = clock() + wait
    while not w.get("ended") and w.get("pid") and alive(int(w["pid"])) and clock() < end:
        sleep(0.5)
        w = read_json(watchdog_file(d), {}) or {}
    if not w.get("ended"):
        w["closed"] = close(d, load(d), alive=alive)
        w["ended"] = "stop asked (closed without the watchdog)"
        write_json(watchdog_file(d), w)
    return {"ended": w["ended"], "closed": w.get("closed")}


CALLS_FILE = "calls.jsonl"
# the CLI commands that are kit calls, under the MCP tool each one is (docs/playtest.md, "Kit helpers")
LOGGED = {"do": ("act", ("player", "script")), "state": ("state", ("player", "since")), "shot": ("shot", ("player",)),
          "checkpoint": ("checkpoint", ("tag",))}


def log_call(a, ok, error, began):
    """Writes a CLI command to the session's call log (server `live`), so `live.py replay` can run it again."""
    if a.cmd not in LOGGED or not os.path.isdir(a.session):
        return
    tool, keys = LOGGED[a.cmd]
    args = {k: getattr(a, k) for k in keys if getattr(a, k, None) is not None}
    kit_tools.CallLog(os.path.join(a.session, CALLS_FILE), "live").write(tool, args, ok, error, t=began)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("start")
    st.add_argument("--session", required=True)
    st.add_argument("--lane", type=int, choices=list(lanes.LANES),
                    help="a test lane: its pair, port and pipes unless given; its lock held for the whole session")
    st.add_argument("--game-a", help="(without --lane: this one, --game-b, or both)")
    st.add_argument("--game-b", help="(without --lane: this one, --game-a, or both)")
    st.add_argument("--only", choices=TAGS, help="start only this game (of the lane, or of the two given)")
    st.add_argument("--server", help="coop_server to start (none: the games use their coop.ini)")
    st.add_argument("--port", type=int, help="default %d (a lane: its own)" % PORT)
    st.add_argument("--boot-a")
    st.add_argument("--boot-b")
    st.add_argument("--load", help="a load script (A until play); default: the boot scripts load")
    st.add_argument("--no-load", action="store_true", help="the boot scripts take the title themselves")
    st.add_argument("--no-restore", action="store_true", help="keep the copies' saves (default: their baseline)")
    st.add_argument("--env", action="append", default=[], help="K=V, or A:K=V / B:K=V for one game")
    st.add_argument("--boot-timeout", type=int, default=900)
    st.add_argument("--display", help="the monitor the game windows go to, by device name (default $%s, else %s): "
                                      "tools/autotest/placement.py" % (hookshot.placement.ENV,
                                                                       hookshot.placement.DEFAULT_DISPLAY))
    st.add_argument("--max-minutes", type=float, default=MAX_MINUTES, help="the session's whole length; 0: no limit")
    st.add_argument("--idle-minutes", type=float, default=IDLE_MINUTES, help="0: no idle end")
    st.add_argument("--lock-wait", type=float, default=0, help="s to wait for a busy lane (default: refuse at once)")
    st.add_argument("--dll", help="(with --lane) a hookshot_coop.dll for this session only: the lane's own is put aside "
                    "and put back when the session ends")
    st.add_argument("--playtest", action="store_true",
                    help="turn each game's play-test bridge on (A = p1, B = p2: 5001/5002, lane L 5000+10*L+1/+2)")
    qt = sub.add_parser("quit", help="close one game in the middle of the session (a planned exit, not its end)")
    qt.add_argument("--session", required=True)
    qt.add_argument("player", choices=TAGS)
    qt.add_argument("--kill", action="store_true", help="end it at once, as a crash would (default: ask it to quit)")
    sv = sub.add_parser("server", help="stop the session's server, or start it again")
    sv.add_argument("--session", required=True)
    sv.add_argument("action", choices=["stop", "start"])
    rl = sub.add_parser("relaunch", help="start a game that quit (or crashed) again; it joins the session's server")
    rl.add_argument("--session", required=True)
    rl.add_argument("player", choices=TAGS)
    rl.add_argument("--boot", help="a boot script for it (default: none, the load presses A until play)")
    rl.add_argument("--no-load", action="store_true", help="the boot script takes the title itself")
    rl.add_argument("--restore", action="store_true", help="put its save baseline back first (default: keep its save)")
    for name in ("do", "state", "shot", "checkpoint", "stop", "watchdog"):
        q = sub.add_parser(name)
        q.add_argument("--session", required=True)
        if name == "do":
            q.add_argument("player", choices=TAGS)
            q.add_argument("script", help="test script lines, ';' between them")
        elif name in ("state", "shot"):
            q.add_argument("player", nargs="?", choices=TAGS)
            if name == "state":
                q.add_argument("--since", type=int, metavar="ID",
                               help="the state_id of an earlier look: only what changed since it (default: all)")
        elif name == "checkpoint":
            q.add_argument("tag")
    rp = sub.add_parser("replay", help="run the calls of a call log again (docs/playtest.md, \"Kit helpers\")")
    rp.add_argument("files", nargs="*", metavar="FILE", help="call logs (default: calls.jsonl of --session)")
    rp.add_argument("--session", help="the session the games are in (needed for oot-playtest and live calls)")
    rp.add_argument("--lane", type=int, help="a test lane: the bridge ports of the player servers")
    rp.add_argument("--log-dir", default="playtest-logs", help="where checkpoint logs and shots go (as the servers' --log-dir)")
    rp.add_argument("--include-reads", action="store_true", help="also repeat the calls that only look (get_state ...)")
    rp.add_argument("--include-failed", action="store_true", help="also repeat the calls that failed the first time")
    rp.add_argument("--keep-going", action="store_true", help="go on after a call that fails (default: stop there)")
    rp.add_argument("--dry-run", action="store_true", help="list the calls, run nothing")
    rp.add_argument("--delay", type=float, default=0.0, help="s to wait between calls")
    a = p.parse_args(argv)
    if a.cmd == "replay":
        files = a.files or ([os.path.join(a.session, CALLS_FILE)] if a.session else [])
        if not files:
            p.error("replay: give a call log FILE, or --session")
        out = kit_tools.replay(files, a.session, a.lane, a.log_dir, a.include_reads, a.include_failed, a.keep_going,
                               a.dry_run, a.delay, say=lambda s: print(s, file=sys.stderr))
        print(json.dumps(out, indent=1))
        return 1 if out["failed"] else 0
    began = time.time()
    try:
        out = {"start": start, "do": do, "state": state, "shot": shot, "checkpoint": checkpoint, "stop": stop,
               "watchdog": watchdog, "quit": quit_one, "server": server_cmd, "relaunch": relaunch}[a.cmd](a)
    except (Exception, SystemExit) as e:
        log_call(a, False, "%s: %s" % (e.__class__.__name__, e), began)
        raise
    log_call(a, "error" not in out, out.get("error") if isinstance(out, dict) else None, began)
    print(json.dumps(out, indent=1))
    return 1 if a.cmd == "do" and "error" in out else 0


if __name__ == "__main__":
    sys.exit(main())
