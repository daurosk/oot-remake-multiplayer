"""Checks a play-test setup before a run and says what is wrong, one line each (docs/playtest.md, "Kit doctor").

    python -I tools/autotest/kit_doctor.py --lane 1 [--session DIR] [--mcp .mcp.json] [--running]
    python -I tools/autotest/kit_doctor.py --game-a FOLDER --game-b FOLDER
    python -I tools/autotest/kit_doctor.py --write-mcp .mcp.json --lane 1 --session DIR --log-dir DIR

Each line is `OK`, `WARN` or `FAIL`, a name and the sentence. Exit code 1 when a line says FAIL. It reads files and tries
the bridge ports; it starts nothing and changes nothing (`--write-mcp` writes the one file you name, never over an
existing one without `--force`). Standard library, and `lanes.py` / `hs.py` of the repository for the lane folders.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import lanes  # noqa: E402
import playtest_client as pc  # noqa: E402

hs = lanes.hs
TEMPLATE = os.path.join(HERE, "mcp.json.template")
KIT_FILES = ("mcp_playtest.py", "playtest_client.py", "playtest_helpers.py", "live.py", "kit_tools.py", "lanes.py")
DLL = "hookshot_coop.dll"
SERVERS = ("oot-p1", "oot-p2", "oot-p3", "oot-p4", "oot-pair", "oot-playtest")   # oot-pair: mcp_playtest --pair
DEFAULT_LOG_DIR = "playtest-logs"   # (mcp_playtest.py --log-dir default)


class Report:
    """The lines of one run: (level, name, text)."""

    def __init__(self):
        self.lines = []

    def add(self, level, name, text):
        self.lines.append((level, name, text))

    def ok(self, name, text):
        self.add("OK", name, text)

    def warn(self, name, text):
        self.add("WARN", name, text)

    def fail(self, name, text):
        self.add("FAIL", name, text)

    def failed(self):
        return any(x[0] == "FAIL" for x in self.lines)

    def text(self):
        return "\n".join("%-4s %s: %s" % x for x in self.lines)


def listening(port, host="127.0.0.1", connect=socket.create_connection):
    """True when something accepts a connection on the port."""
    try:
        connect((host, port), timeout=0.3).close()
        return True
    except OSError:
        return False


def check_python(r, version=None):
    v = tuple(version or sys.version_info[:3])
    if v < (3, 10):
        r.fail("python", "%s is too old: the kit needs Python 3.10 or newer (the kit's own python is in hookshot/tools/python)"
               % ".".join(map(str, v)))
    else:
        r.ok("python", ".".join(map(str, v)))


def python_version(cmd):
    """The (major, minor, micro) of the python a .mcp.json starts, or None when it cannot be asked."""
    try:
        out = hs.run_quiet([cmd, "-I", "-c", "import sys; print(*sys.version_info[:3])"], capture_output=True, text=True,
                           timeout=15).stdout.split()
        return tuple(int(x) for x in out[:3]) if len(out) >= 3 else None
    except (OSError, ValueError, subprocess.SubprocessError):
        return None


def check_repo(r, root=HERE):
    missing = [f for f in KIT_FILES if not os.path.isfile(os.path.join(root, f))]
    if missing:
        r.fail("kit files", "missing from tools/autotest: %s (is this the whole repository?)" % ", ".join(missing))
    else:
        r.ok("kit files", "tools/autotest has the play-test kit")


def check_game(r, tag, folder, dll_hint=""):
    """One game copy: the folder, its exe, the co-op module beside it, the shader warm-up."""
    name = "game %s" % tag
    if not folder or not os.path.isdir(folder):
        r.fail(name, "no folder %s (lanes.py setup builds a lane's pair from two game folders)" % folder)
        return None
    exe = hs.find_exe(folder)
    if not exe:
        r.fail(name, "%s has no longshot.exe (in bin/ or the folder itself)" % folder)
        return None
    if not os.path.isfile(os.path.join(os.path.dirname(exe), DLL)):
        r.fail(name, "no %s next to the exe: the co-op module is not installed%s" % (DLL, dll_hint))
    else:
        r.ok(name, "exe and %s found" % DLL)
    try:
        warmed = os.path.isfile(os.path.join(hs.warm_dir(folder), "longshot_pipelines.bin.built"))
    except (OSError, AttributeError):
        warmed = True   # (an older hs.py: nothing to check)
    if not warmed:
        r.warn(name + " shaders", "not prepared yet: the first start takes a long while")
    return exe


def check_copies(r, a, b):
    """The two games must be two folders: one folder twice means one save and one log for both players."""
    if not (a and b and os.path.isdir(a) and os.path.isdir(b)):
        return
    if os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b)) or lanes.same_file(a, b):
        r.fail("game copies", "game A and game B are the same folder: two games cannot share one (save, log, control file)")
    else:
        r.ok("game copies", "A and B are two folders")


def check_ports(r, players, lane, running, is_listening=listening):
    """Bridge ports. Before a run they must be free; while a run is on, each must answer."""
    for p in players:
        port = pc.port_for(p, lane)
        up = is_listening(port)
        name = "port %s" % p
        if running:
            if up:
                r.ok(name, "%d answers" % port)
            else:
                r.fail(name, "%d: no game listens (it did not start, was not started with --playtest, or its bridge is off: "
                       "LICHII_COOP_PLAYTEST_PORT)" % port)
        elif up:
            r.warn(name, "%d is taken: a game or another program is on it; close it, or use another lane" % port)
        else:
            r.ok(name, "%d is free" % port)
    if lane and not running:
        for port in lanes.lane(lane)["ports"]:
            if is_listening(port):
                r.warn("port server", "%d (lane %d's server) is taken" % (port, lane))
                break
        else:
            r.ok("port server", "lane %d's server ports are free" % lane)


def check_session(r, d, alive=None):
    """A session folder from live.py start: it exists, has not ended, and its games are alive."""
    import live
    alive = alive or hs.pid_alive
    if not d or not os.path.isdir(d):
        r.fail("session", "no folder %s (live.py start --session DIR makes it)" % d)
        return
    try:
        live.load(d)
    except (OSError, ValueError):
        r.fail("session", "%s has no readable session.json (it was not made by live.py start)" % d)
        return
    why = live.ended(d)
    if why:
        r.fail("session", "ended (%s): start a new one" % why)
        return
    pids = live.game_pids(d)[0]
    dead = sorted(x for x, p in pids.items() if not alive(p))
    if not pids:
        r.warn("session", "running, but pids.json names no game yet (still booting?)")
    elif dead:
        r.fail("session", "game %s is gone (its process %s ended): stop the session and start again"
               % (", ".join(dead), "has" if len(dead) == 1 else "have"))
    else:
        r.ok("session", "running, games %s alive" % ", ".join(sorted(pids)))


# --- .mcp.json ---------------------------------------------------------------------------------------------------

def _opt(args, flag):
    """The value after a flag in an args list (None when absent)."""
    for i, a in enumerate(args[:-1]):
        if a == flag:
            return args[i + 1]
    return None


def check_mcp(r, path, lane=None, which=shutil.which, probe=None):
    """A project .mcp.json: each oot server points at files that exist, the lane that is meant, and one log folder.
    probe(command) gives the (major, minor, micro) of the python the file starts (None: not asked)."""
    if not os.path.isfile(path):
        r.fail("mcp", "no %s (python -I tools/autotest/kit_doctor.py --write-mcp %s --lane N makes one)"
               % (path, os.path.basename(path)))
        return
    try:
        with open(path, encoding="utf-8") as f:
            servers = (json.load(f) or {}).get("mcpServers") or {}
    except (OSError, ValueError) as e:
        r.fail("mcp", "%s is not readable JSON (%s)" % (path, e))
        return
    base = os.path.dirname(os.path.abspath(path))
    oot = {k: v for k, v in servers.items() if k in SERVERS and isinstance(v, dict)}
    if not oot:
        r.fail("mcp", "%s has no oot-p1 / oot-p2 server (nor an oot-pair)" % path)
        return
    logs = set()
    for name in sorted(oot):
        s = oot[name]
        args = [str(x) for x in s.get("args") or []]
        cmd = str(s.get("command") or "")
        bad = []
        if not cmd or not (which(cmd) or os.path.isfile(cmd)):
            bad.append("command %r is not found" % cmd)
        script = next((a for a in args if a.replace("\\", "/").endswith("mcp_playtest.py")), None)
        if not script:
            bad.append("args do not name mcp_playtest.py")
        elif not os.path.isfile(script if os.path.isabs(script) else os.path.join(base, script)):
            bad.append("%s does not exist (relative paths are read from %s)" % (script, base))
        player = _opt(args, "--player")
        if name == "oot-pair":
            if "--pair" not in args:
                bad.append("args do not have --pair")
            if player or _opt(args, "--port"):
                bad.append("--pair takes no --player or --port (both games' ports come from --lane)")
        elif name != "oot-playtest" and player != name[4:]:
            bad.append("--player is %s, not %s" % (player, name[4:]))
        got = _opt(args, "--lane")
        lane_ok = got is None or got.lstrip("-").isdigit()
        if name != "oot-playtest" and not lane_ok:
            bad.append("--lane is %r, not a lane number" % got)
        elif name != "oot-playtest" and lane is not None and str(got) != str(lane):
            bad.append("--lane is %s, not %s" % (got, lane))
        if probe and not bad and (which(cmd) or os.path.isfile(cmd)):
            v = probe(cmd)
            if v is None:
                bad.append("command %r could not be asked for its version" % cmd)
            elif v < (3, 10):
                bad.append("command %r is Python %s: the kit needs 3.10 or newer" % (cmd, ".".join(map(str, v))))
        session = _opt(args, "--session")
        if session and not os.path.isdir(session if os.path.isabs(session) else os.path.join(base, session)):
            bad.append("--session %s is not a folder yet" % session)
        if name != "oot-playtest":
            logs.add(_opt(args, "--log-dir") or DEFAULT_LOG_DIR)   # (a missing --log-dir is the default folder)
        if bad:
            r.fail("mcp " + name, "; ".join(bad))
        else:
            lane_n = int(got) if got else None
            if name == "oot-pair":
                r.ok("mcp " + name, "ok, bridge ports p1 %d, p2 %d" % (pc.port_for("p1", lane_n), pc.port_for("p2", lane_n)))
            else:
                port = pc.port_for(name[4:], lane_n) if name != "oot-playtest" else None
                r.ok("mcp " + name, "ok" + (", bridge port %d" % port if port else ""))
    both = sorted(k for k in ("oot-p1", "oot-p2") if k in oot)
    if "oot-pair" in oot and both:
        r.fail("mcp oot-pair", "registered with %s: a bridge takes one client at a time, keep oot-pair or the per-game "
               "servers, not both" % " and ".join(both))
    if len(logs) > 1:
        r.fail("mcp log-dir", "the servers have different --log-dir (%s): desync_check reads one folder"
               % ", ".join(sorted(logs)))


def render_mcp(template_path=TEMPLATE, lane=None, session=None, log_dir=None):
    """The template with {LANE}, {SESSION} and {LOG_DIR} filled in; an option whose value is not given is dropped."""
    with open(template_path, encoding="utf-8") as f:
        data = json.load(f)
    values = {"{LANE}": lane, "{SESSION}": session, "{LOG_DIR}": log_dir}
    if not session:   # the session server needs a session folder (live.py start)
        data["mcpServers"].pop("oot-playtest", None)
    for srv in data["mcpServers"].values():
        out, args = [], srv["args"]
        i = 0
        while i < len(args):
            nxt = args[i + 1] if i + 1 < len(args) else None
            if nxt in values:
                if values[nxt] not in (None, ""):
                    out += [args[i], str(values[nxt])]
                i += 2
                continue
            out.append(args[i])
            i += 1
        srv["args"] = out
    return data


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--lane", type=int, choices=list(lanes.LANES), help="a test lane (tools/autotest/lanes.py)")
    p.add_argument("--game-a", help="game A's folder (without --lane)")
    p.add_argument("--game-b", help="game B's folder (without --lane)")
    p.add_argument("--players", type=int, default=2, choices=(1, 2, 3, 4), help="bridge ports to look at (default 2)")
    p.add_argument("--session", help="a live.py session folder")
    p.add_argument("--mcp", metavar="FILE", help="a .mcp.json to check (default: ./.mcp.json when there is one)")
    p.add_argument("--log-dir", help="--write-mcp: the run's log folder")
    p.add_argument("--running", action="store_true", help="games should be running now: their ports must answer")
    p.add_argument("--write-mcp", metavar="FILE", help="write a .mcp.json from the template and stop")
    p.add_argument("--force", action="store_true", help="--write-mcp: replace an existing file")
    p.add_argument("--json", action="store_true", help="the lines as JSON")
    a = p.parse_args(argv)

    if a.write_mcp:
        if os.path.exists(a.write_mcp) and not a.force:
            print("FAIL mcp: %s exists (--force replaces it)" % a.write_mcp)
            return 1
        data = render_mcp(TEMPLATE, a.lane, a.session, a.log_dir)
        with open(a.write_mcp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, indent=2)
            f.write("\n")
        print("OK   mcp: wrote %s (%s%s)" % (a.write_mcp, ", ".join(data["mcpServers"]),
                                            "" if a.lane else "; no --lane: ports 5001, 5002"))
        return 0

    r = Report()
    check_python(r)
    check_repo(r)
    ga, gb = a.game_a, a.game_b
    if a.lane:
        L = lanes.lane(a.lane)
        ga, gb = ga or L["a"], gb or L["b"]
    hint = " (lanes.py install --lane %d --dll <path to hookshot_coop.dll>)" % a.lane if a.lane else ""
    if ga or gb:
        check_game(r, "A", ga, hint)
        check_game(r, "B", gb, hint)
        check_copies(r, ga, gb)
    else:
        r.warn("games", "no --lane and no --game-a/--game-b: the game folders were not looked at")
    check_ports(r, pc.PLAYERS[:a.players], a.lane, a.running)
    if a.session:
        check_session(r, a.session)
    mcp = a.mcp or (".mcp.json" if os.path.isfile(".mcp.json") else None)
    if mcp:
        check_mcp(r, mcp, a.lane, probe=python_version)
    else:
        r.warn("mcp", "no .mcp.json looked at (--mcp FILE, or --write-mcp FILE makes one)")
    if a.json:
        print(json.dumps([{"level": x[0], "name": x[1], "text": x[2]} for x in r.lines], indent=1))
    else:
        print(r.text())
    return 1 if r.failed() else 0


if __name__ == "__main__":
    sys.exit(main())
