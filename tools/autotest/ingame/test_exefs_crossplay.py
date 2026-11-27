"""v1.0 P0 proof: an emulator running the ExeFS module (switch/proto, built by tools/switch_mod/build.py) joins the
lane's live coop_server session next to its two Longshot games, and each side sees the other.
This replaces tools/switch_mod/run-crossplay.ps1 (v1-exefs-crossplay 6fe89f2bd/47b523918; never run: parked twice):
| Old check (run-crossplay.ps1) | Here |
|---|---|
| lane-server.txt A == B, dll hashes, server present, protocol at the lane commit | bt_plugin lane_pair (P.lane_check) + --expect-commit; build.py --protocol-from <lane commit> and the built coop_join_config.h COOP_PROTOCOL_VERSION read back (== lane protocol) |
| its own hookshot.py run of lane 1 (two games standing still, own server, own password) | the lane's live.py session (default boot, --server = the lane's coop_server); the module is built for that session's port + password (session.json runner, live.py:303) |
| Fritchii only after the server logged both Longshot players joined (the module is never player 1 / the host; coop_join does not retry) | set-up: the session's server-out.txt has two 'player N joined: NICK (client: ...)' lines (server/session.cpp:553) before Fritchii starts |
| Fritchii user folder: keys/config/nand\\system from the kit's emulator, kit paths rewritten, only our mod in load\\<title> | prepare_user(): the same (copy only when missing; three spellings of the kit path rewritten; any left = SETUP-FAIL); load\\<title> is ours alone during the test and removed after |
| module: hello from subsdk1, hook 0 frame hook in, game value: scene, WELCOME, sees A and B, no REFUSED/FAIL | judge(): the same lines in Fritchii's newest user\\log\\*.txt (svcOutputDebugString -> LOG_INFO Debug_Emulated), switch/proto/main.cpp:288/232, switch/net/coop_join.cpp:245/253/261/156/232 |
| server: 'joined: emulator (client: exefs-proto' | the same in server-out.txt (session.cpp:553 client_label = switch/proto/main.cpp:250 build_id) |
| each Longshot game: "coop: player N 'emulator' joined" | Pair.expect_log on A and B (lichii/hook/coop_hook.cpp:2858, built into hookshot: hookshot/CMakeLists.txt:19) |
| fritchii.png window shot | NOT here: the module draws nothing (no puppet, no UI; it sends HELLO + pings only), so nothing a player sees changes; the test is judged by log lines (worker rules TEST SYSTEM: shots only for user-facing things) |
Opt-in only (it starts a third emulator beside the lane's games): --env OOT_TEST_EXEFS_XPLAY=1, e.g.
  pytest tools/autotest/ingame/test_exefs_crossplay.py --lanes 1 --env OOT_TEST_EXEFS_XPLAY=1 --expect-commit <sha> ...
Paths (defaults = run-crossplay.ps1:49-51; override by environment): OOT_TEST_FRITCHII (folder with fritchii.exe),
OOT_TEST_NUP (the game), OOT_TEST_FRITCHII_FROM (an emulator folder whose user\\ has keys, config, nand\\system).
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time

import pytest

from pair import SetupFail

AUTOTEST = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(AUTOTEST))
ROOT = os.path.dirname(REPO)                    # the OOT REMAKE folder (worktrees sit in it: worker rules)
OPT_IN = "OOT_TEST_EXEFS_XPLAY=1"
TITLE = "0400486024DEC000"                      # the game's title id (Fritchii load\\<title>; run-crossplay.ps1:55)
NICK = "emulator"                               # build.py --name (tools/switch_mod/build.py:128)
BUILD_PY = os.path.join(REPO, "tools", "switch_mod", "build.py")
PLACEMENT = os.path.join(AUTOTEST, "placement.py")
USER_DIRS = ("keys", "config", os.path.join("nand", "system"))   # run-crossplay.ps1:136 (wt-exefs-spike run-fritchii.ps1)

# the module's lines (Fritchii log, LOG_INFO Debug_Emulated: fritchii-src svc_debug_string.cpp)
MODULE_LINE = re.compile(r"coop-proto: |coop-join: ")
HELLO = r"coop-proto: hello from subsdk1"                     # switch/proto/main.cpp:288
FRAME_HOOK = r"hook 0: frame hook in"                         # switch/proto/main.cpp:190
SCENE = r"game value: scene "                                 # switch/proto/main.cpp:232
WELCOME = r"coop-join: WELCOME: I am player"                  # switch/net/coop_join.cpp:245
SEES = r"coop-join: (in the session: |PLAYER_JOINED )player \d+ '%s'"   # coop_join.cpp:253 / :261
REFUSED = r"coop-join: (REFUSED|FAIL)"                        # coop_join.cpp:232 / :156
DISCONNECTED = r"coop-join: disconnected"                     # coop_join.cpp:407
# the server's (server/session.cpp:553) and each Longshot game's (lichii/hook/coop_hook.cpp:2858)
SRV_JOINED = re.compile(r"player (\d+) joined: (\S+) \(client: ")
SRV_EMU = r"player \d+ joined: %s \(client: exefs-proto" % NICK    # build_id: switch/proto/main.cpp:250
GAME_SAW = r"coop: player \d+ '%s' joined" % NICK

BUILD_S = 300           # build.py (clang + ld.lld, a handful of files)
JOIN_S = 360            # Fritchii boot + the module's 900 frames to its join (run-crossplay.ps1:187: 6 minutes)
PINGS_S = 60            # all three in, pings run a minute (run-crossplay.ps1:197)
CLOSE_S = 30


def paths():
    emu = os.environ.get("OOT_TEST_FRITCHII") or os.path.join(ROOT, "exefs-spike", "fritchii-v6.5", "Fritchii-v6.5")
    game = os.environ.get("OOT_TEST_NUP") or os.path.join(
        ROOT, "The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup")
    src = os.environ.get("OOT_TEST_FRITCHII_FROM") or os.path.join(ROOT, "coop-kit", "auto-A", "emulator")
    return emu, game, src


def check_paths(emu, game, src):
    """SETUP-FAIL naming the first missing input (seconds, starts nothing)."""
    need = [os.path.join(emu, "fritchii.exe"), game, BUILD_PY, PLACEMENT]
    need += [os.path.join(src, "user", d) for d in USER_DIRS]
    for p in need:
        if not os.path.exists(p):
            raise SetupFail("exefs-crossplay: missing %s" % p)


def server_nicks(lines):
    """{player number: nickname} of every 'player N joined: NICK (client: ...)' line."""
    got = {}
    for ln in lines:
        m = SRV_JOINED.search(ln)
        if m:
            got[int(m.group(1))] = m.group(2)
    return got


def read_lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def newest_log(log_dir):
    try:
        fs = [os.path.join(log_dir, f) for f in os.listdir(log_dir) if f.lower().endswith(".txt")]
    except OSError:
        return None
    fs = [f for f in fs if os.path.isfile(f)]
    return max(fs, key=os.path.getmtime) if fs else None


def module_lines(log_dir):
    f = newest_log(log_dir)
    return [ln.strip() for ln in read_lines(f) if MODULE_LINE.search(ln)] if f else []


def has(lines, pattern):
    rx = re.compile(pattern)
    return any(rx.search(ln) for ln in lines)


def join_done(ml, nicks):
    """The module's wait is over: refused/failed/disconnected, or WELCOME and every Longshot nick seen."""
    if has(ml, REFUSED) or has(ml, DISCONNECTED):
        return True
    return has(ml, WELCOME) and all(has(ml, SEES % re.escape(n)) for n in nicks)


def judge(ml, srv, nicks):
    """[(check, ok)] over the module's lines `ml`, the server's lines `srv`, the Longshot nicknames."""
    checks = [("module loaded by rtld", has(ml, HELLO)),
              ("frame hook in", has(ml, FRAME_HOOK)),
              ("game value read (scene)", has(ml, SCENE)),
              ("module got WELCOME", has(ml, WELCOME))]
    checks += [("module sees Longshot player '%s'" % n, has(ml, SEES % re.escape(n))) for n in nicks]
    checks += [("module never refused/failed", not has(ml, REFUSED)),
               ("server: %s joined (client: exefs-proto ...)" % NICK, has(srv, SRV_EMU))]
    return checks


def _forms(path):
    p = os.path.abspath(path).rstrip("\\/")
    return [p, p.replace("\\", "/"), p.replace("\\", "\\\\")]


def prepare_user(emu, src, nso):
    """Fritchii's own portable user folder: keys, config, nand\\system copied from `src`\\user when missing; the kit's
    paths in config\\*.ini rewritten to this folder (SETUP-FAIL if any spelling of the kit folder is left); load\\<title>
    holds only our module; old logs removed. Returns (installed module path, log folder, [notes])."""
    user = os.path.join(emu, "user")
    notes = []
    for d in USER_DIRS:
        dst = os.path.join(user, d)
        if not os.path.exists(dst):
            shutil.copytree(os.path.join(src, "user", d), dst)
            notes.append("copied %s" % d)
    cfg = os.path.join(user, "config")
    inis = [os.path.join(cfg, f) for f in os.listdir(cfg) if f.lower().endswith(".ini")]
    pairs = list(zip(_forms(os.path.join(src, "user")), _forms(user)))
    for ini in inis:
        with open(ini, encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
        new = text
        for old, mine in pairs:
            new = re.sub(re.escape(old), mine.replace("\\", "\\\\"), new, flags=re.IGNORECASE)
        if new != text:
            with open(ini, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(new)
            notes.append("kit paths -> own user folder in %s" % os.path.basename(ini))
    for ini in inis:
        with open(ini, encoding="utf-8", errors="surrogateescape") as f:
            text = f.read().lower()
        for form in _forms(src):
            if form.lower() in text:
                raise SetupFail("exefs-crossplay: %s still names the kit folder %s" % (ini, form))
    load = os.path.join(user, "load", TITLE)
    if os.path.exists(load):
        shutil.rmtree(load)
    installed = os.path.join(load, "coop-proto", "exefs", "subsdk1")
    os.makedirs(os.path.dirname(installed))
    shutil.copyfile(nso, installed)
    with open(nso, "rb") as a, open(installed, "rb") as b:
        if a.read() != b.read():
            raise SetupFail("exefs-crossplay: installed %s differs from the build" % installed)
    log_dir = os.path.join(user, "log")
    if os.path.isdir(log_dir):
        for f in os.listdir(log_dir):
            p = os.path.join(log_dir, f)
            if os.path.isfile(p):
                os.remove(p)
    return installed, log_dir, notes


def build_module(out, commit, port, password, protocol, run=subprocess.run):
    """build.py for the lane's protocol, joining the session's server; returns the subsdk1 path."""
    mod = os.path.join(out, "exefs-module")
    shutil.rmtree(mod, ignore_errors=True)   # (a module left by an earlier build must never pass for this one)
    with open(os.path.join(out, "exefs-build.log"), "w", encoding="utf-8") as log:
        r = run([sys.executable, "-I", BUILD_PY, "--out", mod, "--protocol-from", commit,
                 "--join", "127.0.0.1:%d" % port, "--password", password, "--name", NICK],
                stdout=log, stderr=subprocess.STDOUT, timeout=BUILD_S)
    if r.returncode != 0:
        raise SetupFail("exefs-crossplay: build.py exit %d (%s)" % (r.returncode, log.name))
    cfg = "\n".join(read_lines(os.path.join(mod, "coop_join_config.h")))
    m = re.search(r"COOP_PROTOCOL_VERSION (\d+)", cfg)
    if not m or int(m.group(1)) != int(protocol):
        raise SetupFail("exefs-crossplay: module built for protocol %s, the lane is %s"
                        % (m.group(1) if m else "?", protocol))
    nso = os.path.join(mod, "subsdk1")
    if not os.path.isfile(nso):
        raise SetupFail("exefs-crossplay: build.py wrote no %s" % nso)
    return nso


def emulators_running():
    """Names of running fritchii/lichii processes (a second one would share the user folder or the port's game)."""
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    names = [ln.split(",")[0].strip('"').lower() for ln in out.splitlines() if ln]
    return sorted({n for n in names if n in ("fritchii.exe", "lichii.exe")})


def close_emulator(proc):
    """WM_CLOSE first (taskkill without /F), then /F after CLOSE_S; returns how it ended."""
    if proc.poll() is not None:
        return "exited by itself (exit %s)" % proc.returncode
    subprocess.run(["taskkill", "/PID", str(proc.pid)], capture_output=True, timeout=30)
    try:
        proc.wait(CLOSE_S)
        return "closed"
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True, timeout=30)
        proc.wait(30)
        return "did not close in %d s: stopped" % CLOSE_S


def session_runner(pair):
    with open(os.path.join(pair.session, "session.json"), encoding="utf-8") as f:
        return json.load(f).get("runner") or {}


def lane_protocol(folder):
    """protocol= of game A's lane-server.txt (lane-prep / lane-game-for write it; P.lane_check proved A == B)."""
    for ln in read_lines(os.path.join(folder, "lane-server.txt")):
        if ln.startswith("protocol="):
            return ln.split("=", 1)[1].strip()
    return None


@pytest.mark.row("v1.0 P0: an ExeFS-module emulator (Fritchii) joins the Longshot session and both sides see it")
@pytest.mark.needs_env(OPT_IN)
@pytest.mark.budget(1200)   # build <= 300 s + Fritchii boot/join <= 360 s + pings 60 s + close <= 60 s + margin
def test_exefs_emulator_joins_longshot_session(pair, request):
    import bt_plugin
    out = pair.out_dir
    with pair.setup():
        pair.need_boot_place()       # (a fresh stepped in-play sample of both games in the boot's scene; any age)
        emu, game, src = paths()
        check_paths(emu, game, src)
        if not (pair.session and getattr(pair, "info", None)):
            raise SetupFail("exefs-crossplay: no live.py session (Pair.session): the module joins the session's server")
        r = session_runner(pair)
        if not r.get("server") or not r.get("port") or not r.get("password"):
            raise SetupFail("exefs-crossplay: the session started no coop_server (runner server/port/password %s/%s/%s)"
                            % (r.get("server"), r.get("port"), bool(r.get("password"))))
        commit = pair.info.get("commit")
        protocol = lane_protocol(bt_plugin.game_a_folder(request.config, pair.lane))
        if not commit or not protocol:
            raise SetupFail("exefs-crossplay: lane commit/protocol unknown (%s/%s)" % (commit, protocol))
        srv = pair.server_log("server-out.txt")
        nicks = sorted(set(server_nicks(srv.since(0)).values()))
        if len(nicks) < 2:
            raise SetupFail("exefs-crossplay: server-out.txt has %d Longshot player(s) joined, not 2: the module must "
                            "never be the session's first player" % len(nicks))
        running = emulators_running()
        if running:
            raise SetupFail("exefs-crossplay: %s already running (would share the user folder)" % ", ".join(running))
        pair.notes.append("session port %s, lane commit %s protocol %s, Longshot players %s"
                          % (r["port"], commit[:9], protocol, nicks))

    nso = build_module(out, commit, int(r["port"]), r["password"], protocol)
    installed, log_dir, notes = prepare_user(emu, src, nso)
    pair.notes += ["fritchii: %s" % n for n in notes]
    proc = None
    ml = []
    saw = {}
    try:
        with open(os.path.join(out, "fritchii-out.txt"), "w", encoding="utf-8") as fo:
            proc = subprocess.Popen([os.path.join(emu, "fritchii.exe"), "-g", game], cwd=emu, stdout=fo,
                                    stderr=subprocess.STDOUT)
        pair.notes.append("fritchii pid %d, module %s" % (proc.pid, installed))
        pl = subprocess.run([sys.executable, "-I", PLACEMENT, "place", "--pid", str(proc.pid), "--wait", "60"],
                            capture_output=True, text=True, timeout=120)
        pair.notes.append("place: %s" % (pl.stdout or pl.stderr).strip()[-200:])
        # the Longshot games keep running frames (bridge steps) while the module boots and joins
        t_end = time.monotonic() + JOIN_S
        while time.monotonic() < t_end and proc.poll() is None:
            ml = module_lines(log_dir)
            if join_done(ml, nicks):
                break
            pair.step_both(30)
        t_end = time.monotonic() + PINGS_S
        while time.monotonic() < t_end and proc.poll() is None and not has(ml, REFUSED):
            pair.step_both(30)
        for x in ("A", "B"):
            saw[x] = pair.log[x].find(GAME_SAW, 0) is not None   # (from the session's start: the join came early)
    finally:
        if proc is not None:
            pair.notes.append("fritchii: %s" % close_emulator(proc))
        f = newest_log(log_dir)
        if f:
            shutil.copyfile(f, os.path.join(out, "fritchii-" + os.path.basename(f)))
        shutil.rmtree(os.path.join(emu, "user", "load", TITLE), ignore_errors=True)   # (our module out again)
    ml = module_lines(log_dir) or ml
    with open(os.path.join(out, "exefs-module-lines.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(ml) + "\n")
    srv_lines = pair.server_log("server-out.txt").since(0)
    checks = judge(ml, srv_lines, nicks)
    checks += [("Longshot game %s saw the emulator join" % x, saw.get(x, False)) for x in ("A", "B")]
    for name, ok in checks:
        pair.notes.append("%s %s" % ("PASS" if ok else "FAIL", name))
    if has(ml, DISCONNECTED):
        pair.notes.append("note: the module logged a disconnect: %s" % next(ln for ln in ml if re.search(DISCONNECTED, ln)))
    bad = [name for name, ok in checks if not ok]
    assert not bad, "exefs-crossplay: %s (module lines: %s)" % (" | ".join(bad), out)
