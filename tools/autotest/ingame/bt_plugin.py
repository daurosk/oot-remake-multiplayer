"""pytest plugin of the in-game test system (docs/ingame-tests.md): lanes, one game-pair boot per lane, results.

    python -I -m pytest tools/autotest/ingame --lanes 1 --out DIR            (one lane)
    python -I -m pytest tools/autotest/ingame -n 2 --lanes 1,2 --out DIR     (both lanes at once: xdist worker N -> lane N+1)
    ... --fake                                                               (no game: fake bridges, for the selftests)
    ... --repeat 10 --no-smoke-gate                                          (the pilot: everything 10 times, one boot)

Results (one summary per run, DIR/summary.json + summary.md, and pytest's --junitxml if asked):
  PASS        every assert held
  FAIL        an assert failed or the test ran past its budget ('timeout'); rerun once: FAIL again stays FAIL
  FLAKY       failed, then passed on the one rerun (never counted as a pass)
  SETUP-FAIL  the lane, the boot or the test's own set-up (Pair.setup(), < 30 s) did not hold; not rerun
  NO-REF      a look check has no stored reference yet (its shot is kept under DIR/new/); not a pass
Smoke gate: a smoke test (marker `smoke`) that FAILs or SETUP-FAILs stops the run there (the rc is broken), unless
--no-smoke-gate.
"""
import hashlib
import json
import re
import os
import subprocess
import sys
import time

import pytest
from _pytest.runner import runtestprotocol

HERE = os.path.dirname(os.path.abspath(__file__))
AUTOTEST = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, AUTOTEST)
import pair as P   # noqa: E402

RESULTS = ("PASS", "FAIL", "FLAKY", "SETUP-FAIL", "NO-REF", "SKIP", "NOT-RUN")
DEFAULT_BUDGET = 120


class GameCrash(AssertionError):
    """A game crashed while the lane booted or loaded (its module log has a CRASH line): every test of the lane is
    FAIL with that line, never SETUP-FAIL, because that window is what crash rows guard (review-c123-7 08:53)."""


NoRef = P.NoRef     # (the typed NO-REF path lives in pair.py, so row tests need only `pair`)


def _exc_kind(e):
    """A failure's kind from its exception TYPE (B #130 / kokiri-band-c124-4 09:41: never from traceback text, which a
    FAIL's source lines or message can contain): 'crash', 'noref', 'setup' or 'other'."""
    if isinstance(e, GameCrash):
        return "crash"
    if isinstance(e, NoRef):
        return "noref"
    if isinstance(e, P.SetupFail):
        return "setup"
    return "other"


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    rep.bt_kind = _exc_kind(call.excinfo.value) if call.excinfo is not None else None


def pytest_addoption(parser):
    g = parser.getgroup("ingame")
    g.addoption("--lanes", default="1", help="test lanes, comma separated; xdist worker N takes the Nth")
    g.addoption("--out", default=None, help="results folder (summary.json, summary.md, shots)")
    g.addoption("--fake", action="store_true", help="fake games (fakegame.py): no game, for the selftests")
    g.addoption("--repeat", type=int, default=1, help="run every test N times on the same boot (the pilot: 10)")
    g.addoption("--no-smoke-gate", action="store_true", help="go on after a smoke test that did not pass")
    g.addoption("--no-rerun", action="store_true", help="no FLAKY rerun of a failed test")
    g.addoption("--kit-ls", default=None, help="the lanes folder (default: lanes.py's kit root + ls)")
    g.addoption("--expect-commit", default=None, help="SETUP-FAIL unless the lane's lane-server.txt commit starts so")
    g.addoption("--session-minutes", type=float, default=240, help="live.py --max-minutes of each lane's session")
    g.addoption("--env", action="append", default=[], metavar="NAME=VALUE", help="a game switch for this boot, set at "
                "the game launch (live.py start --env; repeatable): NAME=VALUE for both games, A:NAME=VALUE / "
                "B:NAME=VALUE for one; a scenario's '#! switches' line. Kept in summary.json 'identity'")
    g.addoption("--deadline-min", type=float, default=None, metavar="N", help="the run's wall-clock limit in minutes "
                "(set it below the queue's Minutes): past N no new test starts and no rerun, the rest are NOT-RUN "
                "(never a pass), the games are closed and the lane let go (live.py stop), then summary.json is written")
    g.addoption("--boot", default=DEFAULT_BOOT, metavar="NAME", help="the lane's boot: scenarios/NAME.A.txt + NAME.B.txt "
                "(default %(default)s); tests run only with their own boot (@pytest.mark.needs_boot(NAME), else the default)")
    g.addoption("--lane-check-only", action="store_true", help="the setup proof of every --lanes lane (lane_check + "
                "--expect-commit + the tests collect; no game boot, no test run): one SETUP-OK/SETUP-FAIL line per "
                "lane, OUT/setup.json; exit 0 only if every lane is OK (pytest's own --setup-only is another thing)")


def pytest_configure(config):
    config.addinivalue_line("markers", "smoke: the rc smoke tier (runs first; a miss stops the run)")
    config.addinivalue_line("markers", "budget(seconds): the test's own hard deadline (default %d)" % DEFAULT_BUDGET)
    config.addinivalue_line("markers", "row(text): the rc row the test proves (in the summary)")
    config.addinivalue_line("markers", "look: a user-facing look check (bridge shots on each game, compared to refs)")
    config.addinivalue_line("markers", "needs_boot(NAME): the test boots with scenarios/NAME.A/B.txt (run with --boot NAME); "
                            "without it a test needs the default boot")
    config.addinivalue_line("markers", "needs_env(*'NAME=VALUE'): game switches the test needs; a run whose "
                            "--env lacks them deselects it (named in the output and summary.json 'deselected')")
    config._bt_rows, config._bt_deselected = [], []
    if config.getoption("lane_check_only") and getattr(config.option, "numprocesses", None):
        raise pytest.UsageError("--lane-check-only runs in one process: leave out -n (it checks every --lanes lane)")
    boot_env(config)        # (a malformed --env stops the run before any boot)
    boot_paths(config.getoption("boot"))   # (an unknown --boot too)
    config._bt_started = time.time()


# -- repeat and order: smoke first, then rows; repeat r of everything before repeat r+1 ------------------------
@pytest.fixture(autouse=True)
def bt_rep(request):
    return getattr(request, "param", 0)


def pytest_generate_tests(metafunc):
    n = metafunc.config.getoption("repeat")
    if n > 1 and "bt_rep" in metafunc.fixturenames:
        metafunc.parametrize("bt_rep", range(n), ids=lambda i: "r%d" % (i + 1))


def _rep_of(item):
    cs = getattr(item, "callspec", None)
    return cs.params.get("bt_rep", 0) if cs else 0


def pytest_collection_modifyitems(config, items):
    order = {id(it): i for i, it in enumerate(items)}
    items.sort(key=lambda it: (_rep_of(it), 0 if it.get_closest_marker("smoke") else 1, order[id(it)]))
    _deselect_needs_env(config, items)
    if config.getoption("lane_check_only"):
        _lane_check_only(config, items)


def _deselect_line(d):
    if d.get("needs_boot"):
        return "deselected: needs_boot %s %s" % (d["needs_boot"], d["test"])
    return "deselected: needs_env %s %s" % (" ".join(d["needs_env"]), d["test"])


def _deselect_needs_env(config, items):
    """A test this run cannot run is DESELECTED (B #129, 2026-11-26), with one 'deselected: ...' line each and a
    summary.json 'deselected' list: never hidden, and never a SETUP-FAIL of the default run (the rc summary, rc_tests.py
    summary --plan, makes it NOT-RUN if no session ran it):
    - needs_env: a switch this run's --env lacks;
    - needs_boot: a test boots with ONE boot (@pytest.mark.needs_boot(NAME), else DEFAULT_BOOT); a run with another
      --boot leaves it out (a boss boot is not the Kakariko boot the smoke tier and the rows assume)."""
    have, boot = boot_env(config), config.getoption("boot")
    keep, drop = [], []
    for it in items:
        m = it.get_closest_marker("needs_boot")
        want = m.args[0] if m else DEFAULT_BOOT
        lack = [e for mk in it.iter_markers("needs_env") for e in mk.args if not env_has(have, e)]
        if want != boot:
            config._bt_deselected.append({"test": it.nodeid, "needs_boot": want, "needs_env": lack})
        elif lack:
            config._bt_deselected.append({"test": it.nodeid, "needs_env": lack})
        else:
            keep.append(it)
            continue
        drop.append(it)
    if not drop:
        return
    tr = config.pluginmanager.get_plugin("terminalreporter")
    for d in config._bt_deselected[-len(drop):]:
        line = _deselect_line(d)
        tr.write_line(line) if tr else print(line)
    config.hook.pytest_deselected(items=drop)
    items[:] = keep


# The lane's boot (live.py start): the documented live.py start boot, scenarios/live-kakariko-child.A/B.txt (its own
# header: "boot for live.py start": child save, warp to Kakariko by the well, through the title; '#! noload' + its
# switches line). --no-load: the boot scripts take the title themselves (live.py start --no-load help; the same form
# as offline/desync-275-run.ps1:41-42). hookshot.py run needs --script-a (its own check: '--script-a (or --only B)').
DEFAULT_BOOT = "live-kakariko-child"
BOOT_A = os.path.join(AUTOTEST, "scenarios", DEFAULT_BOOT + ".A.txt")
BOOT_B = os.path.join(AUTOTEST, "scenarios", DEFAULT_BOOT + ".B.txt")


def boot_paths(name):
    """--boot NAME: scenarios/NAME.A.txt and NAME.B.txt (both must exist; e.g. live-twinrova-room for a boss stage,
    where the bridge's warp answers not_in_event in free play: docs/playtest.md warp)."""
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", name or ""):
        raise pytest.UsageError("--boot %r: a scenario name (scenarios/NAME.A.txt + NAME.B.txt)" % name)
    a, b = (os.path.join(AUTOTEST, "scenarios", "%s.%s.txt" % (name, x)) for x in ("A", "B"))
    for f in (a, b):
        if not os.path.isfile(f):
            raise pytest.UsageError("--boot %s: no %s" % (name, f))
    return a, b


def boot_identity(name=DEFAULT_BOOT):
    """What the boot itself brings, for the proof key (review-c123-7): each boot script's path (repo-relative), sha256
    and its '#! switches' header (hookshot.header_switches: the A script's go to both games, an --env of the same name
    wins; live-kakariko-child.A.txt turns LICHII_COOP_CHEATS=1 on). A different boot, or one without those switches,
    is a different identity."""
    import hookshot
    out = {}
    for x, path in zip(("A", "B"), boot_paths(name)):
        with open(path, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        out[x] = {"script": os.path.relpath(path, os.path.dirname(os.path.dirname(AUTOTEST))).replace(os.sep, "/"),
                  "sha256": sha, "switches": ["%s=%s" % kv for kv in hookshot.header_switches(path)]}
    return out


def boot_args(config, lane, server, session):
    """live.py start's arguments for this lane's boot (after 'start'): the ONE list lane_pair boots with and
    --lane-check-only proves (live.py start --dry-run = hookshot.py run's own parser + script parse)."""
    args = ["--session", session, "--lane", str(lane), "--playtest", "--server", server,
            "--boot-a", boot_paths(config.getoption("boot"))[0], "--boot-b", boot_paths(config.getoption("boot"))[1],
            "--no-load"]
    for e in boot_env(config):
        args += ["--env", e]
    return args + ["--max-minutes", str(config.getoption("session_minutes")), "--idle-minutes", "15"]


def _real_lane_info(config, lane):
    """A real lane's setup proof before any boot: lane-server.txt twin (P.lane_check) + the module commit."""
    import lanes as L
    kit_ls = config.getoption("kit_ls") or os.path.join(L.kit_root(), "ls")
    info = P.lane_check(kit_ls, lane)
    want = config.getoption("expect_commit")
    if not want:
        raise P.SetupFail("--expect-commit is required on a real lane (worker-rules SETUP PROOF: the right module sha)")
    if not info["commit"].startswith(want):
        raise P.SetupFail("lane%d runs commit %s, not %s" % (lane, info["commit"], want))
    return info


def _lane_check_only(config, items):
    lanes = [int(x) for x in str(config.getoption("lanes")).split(",") if x.strip()]
    res = {"collected": len(items), "smoke": sum(1 for it in items if it.get_closest_marker("smoke")),
           "look": sum(1 for it in items if it.get_closest_marker("look")), "lanes": {}}
    for lane in lanes:
        try:
            if config.getoption("fake"):
                info = {"fake": True}
            else:
                info = _real_lane_info(config, lane)
                session = os.path.join(_out_dir(config, lane), "session")
                dry = _live(["start"] + boot_args(config, lane, info["server"], session) + ["--dry-run"], timeout=120)
                if not dry.get("ok"):
                    raise P.SetupFail("lane%d boot command: %s" % (lane, dry.get("error")))
                info = dict(info, boot="live.py start --dry-run OK")
            res["lanes"][str(lane)] = dict(info, ok=True)
        except P.SetupFail as e:
            res["lanes"][str(lane)] = {"ok": False, "why": str(e)}
    res["ok"] = bool(lanes) and bool(items) and all(v["ok"] for v in res["lanes"].values())
    out = config.getoption("out") or os.path.join(os.getcwd(), "ingame-out")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "setup.json"), "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    tr = config.pluginmanager.get_plugin("terminalreporter")
    say = tr.write_line if tr else print
    for lane, v in res["lanes"].items():
        say("lane%s %s %s" % (lane, "SETUP-OK" if v["ok"] else "SETUP-FAIL",
                              ("commit %s dll %s" % (v.get("commit"), (v.get("dll_sha256") or "")[:16])) if v["ok"]
                              and not v.get("fake") else v.get("why", "fake")))
    say("tests collected %d (smoke %d, look %d); RESULT: %s" % (res["collected"], res["smoke"], res["look"],
                                                               "SETUP-ONLY OK" if res["ok"] else "SETUP-FAIL"))
    config._bt_setup = res
    config.hook.pytest_deselected(items=list(items))
    items[:] = []


# -- the lane: one boot for every test this process runs -------------------------------------------------------
def _lane_for(config):
    lanes = [int(x) for x in str(config.getoption("lanes")).split(",") if x.strip()]
    wid = getattr(config, "workerinput", {}).get("workerid", "gw0")
    n = int(wid[2:]) if wid.startswith("gw") else 0
    if n >= len(lanes):
        raise P.SetupFail("xdist worker %s has no lane (--lanes %s): give one lane per worker" % (wid, config.getoption("lanes")))
    return lanes[n]


def _out_dir(config, lane):
    out = config.getoption("out") or os.path.join(os.getcwd(), "ingame-out")
    d = os.path.join(out, "lane%d" % lane)
    os.makedirs(d, exist_ok=True)
    return d


def last_json(text):
    """The last JSON object in `text` (live.py's answer comes last; a live.py without 3383fb731+'s stdout fix also
    printed the runner's boot lines before it on stdout, sometimes on the same line as the '{': C #132 11:05,
    party-hud-bridge-fa63e90f 'scenario played { "ok": true ...'). The object must run to the end of the text
    (trailing white space only). None if there is none."""
    dec = json.JSONDecoder()
    i = text.rfind("{")
    while i >= 0:
        try:
            v, end = dec.raw_decode(text, i)
        except ValueError:
            v, end = None, -1
        if isinstance(v, dict) and not text[end:].strip():
            return v
        i = text.rfind("{", 0, i)
    return None


def _live(args, timeout):
    """tools/autotest/live.py as its own process: its answer is the JSON object its stdout ends with."""
    cmd = [sys.executable, "-I", os.path.join(AUTOTEST, "live.py")] + args
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    v = last_json(p.stdout or "")
    if v is not None:
        return v
    return {"ok": False, "error": "live.py %s: exit %s, no JSON answer: %s" % (
        args[0], p.returncode, " ".join((p.stderr or p.stdout or "").split())[-600:])}


CRASH_LINE = "CRASH: exception"   # the crash line a game's module log gets (review-c123-7: not any 'CRASH' mention)


def _crash_why(session):
    """'; A: CRASH: ...' for each game of the session that crashed: the first 'CRASH: exception' line of its module
    log (session.json's games.X.log), or else a crash report the game wrote since the session started
    (<its folder>/bug-reports/pending/crash-*.txt newer than session.json's 'started'; pganon-cct-287-3 10:23: a game
    can crash with no module line). A failed boot with either is a GameCrash (FAIL), not a SETUP-FAIL."""
    try:
        with open(os.path.join(session, "session.json"), encoding="utf-8") as f:
            s = json.load(f)
    except (OSError, ValueError):
        return ""
    games, since = s.get("games") or {}, float(s.get("started") or 0)
    out = []
    for x in sorted(games):
        g = games[x] or {}
        try:
            with open(g.get("log"), encoding="utf-8", errors="replace") as f:
                line = next((ln.strip() for ln in f if CRASH_LINE in ln), None)
        except (OSError, TypeError):
            line = None
        if not line and g.get("folder"):
            pend = os.path.join(g["folder"], "bug-reports", "pending")
            try:
                new = sorted(n for n in os.listdir(pend) if re.fullmatch(r"crash-[^/]*\.txt", n)
                             and os.path.getmtime(os.path.join(pend, n)) >= since)
            except OSError:
                new = []
            if new:
                line = "crash report %s" % os.path.join(pend, new[0])
        if line:
            out.append("%s: %s" % (x, line[:300]))
    return ("; " + "; ".join(out)) if out else ""


@pytest.fixture(scope="session")
def lane_pair(request):
    config = request.config
    lane = _lane_for(config)
    out = _out_dir(config, lane)
    refs = os.path.join(HERE, "refs")
    if config.getoption("fake"):
        if os.environ.get("BT_FAKE_BOOT_CRASH"):        # (selftest: a game crashed in the boot's save load)
            if os.environ.get("BT_FAKE_BOOT_LOG"):      # (selftest: one line per boot tried)
                with open(os.environ["BT_FAKE_BOOT_LOG"], "a", encoding="utf-8") as f:
                    f.write("boot\n")
            raise GameCrash("lane%d boot: game A ended; A: %s" % (lane, os.environ["BT_FAKE_BOOT_CRASH"]))
        import fakegame
        lanefake = fakegame.FakeLane(out, env=boot_env(config),     # (fakes/*.py hooks see the boot's switches)
                                     fakes_dir=os.environ.get("BT_FAKE_FAKES_DIR") or fakegame.FAKES)
        pair = P.Pair(lanefake.games(), out_dir=out, refs_dir=os.environ.get("BT_FAKE_REFS", refs))
        pair.lane, pair.info = lane, {"fake": True}
        yield pair
        pair.close()
        lanefake.close()
        config._bt_lane_closed = True
        return
    t0 = time.monotonic()
    info = _real_lane_info(config, lane)                    # (SetupFail: every test of this lane is SETUP-FAIL)
    session = os.path.join(out, "session")
    started = _live(["start"] + boot_args(config, lane, info["server"], session), timeout=1200)
    if not started.get("ok"):
        crash = _crash_why(session)
        why = "lane%d boot: %s%s" % (lane, started.get("error") or started.get("boot"), crash)
        raise GameCrash(why) if crash else P.SetupFail(why)
    try:
        pair = P.Pair(P.read_session(session), out_dir=out, refs_dir=refs)
        pair.lane, pair.info, pair.session = lane, dict(info, boot_s=round(time.monotonic() - t0, 1)), session
        for x in ("A", "B"):
            h = pair.c[x].hello()
            if not h.get("control"):
                raise P.SetupFail("lane%d %s: the bridge has no Longshot control (HOS_CONTROL)" % (lane, x))
        yield pair
        pair.close()
    finally:
        stopped = _live(["stop", "--session", session], timeout=180)
        config._bt_lane_closed = stopped


BASE_ENV = ["LICHII_COOP_PUPPET=1", "LICHII_COOP_TEST_TO_TITLE=1"]   # every boot (lane_pair's live.py start)


ENV_RX = re.compile(r"^(?:[AB]:)?[A-Za-z_][A-Za-z0-9_]*=[^\r\n]*$")


def boot_env(config):
    """The boot's game switches: BASE_ENV + --env, each NAME=VALUE or A:/B:NAME=VALUE (live.py start --env's form)."""
    out = list(BASE_ENV)
    for e in config.getoption("env") or []:
        if not ENV_RX.match(e.strip()):
            raise pytest.UsageError("--env %r is not NAME=VALUE, A:NAME=VALUE or B:NAME=VALUE" % e)
        out.append(e.strip())
    return out


def env_has(have, want):
    """Does the boot's env give `want`? 'X=1' needs X=1 on both games ('X=1', or both 'A:X=1' and 'B:X=1');
    'A:X=1' needs it on A ('A:X=1' or 'X=1'). The last setting of a name on a game wins, as in live.py's argv."""
    def per_game(g):
        d = {}
        for e in have:
            tag, _, kv = e.partition(":") if re.match(r"^[AB]:", e) else ("", "", e)
            if tag in ("", g):
                k, _, v = kv.partition("=")
                d[k] = v
        return d
    tag, _, kv = want.partition(":") if re.match(r"^[AB]:", want) else ("", "", want)
    k, _, v = kv.partition("=")
    return all(per_game(g).get(k) == v for g in ([tag] if tag else ["A", "B"]))


@pytest.fixture
def pair(request, lane_pair):
    have = boot_env(request.config)
    for m in request.node.iter_markers("needs_env"):
        lack = [e for e in m.args if not env_has(have, e)]
        if lack:
            raise P.SetupFail("this boot lacks the switch(es) %s (run it with --env %s)" % (", ".join(lack), lack[0]))
    m = request.node.get_closest_marker("budget")
    lane_pair.begin(m.args[0] if m else DEFAULT_BUDGET)
    if getattr(lane_pair, "session", None):
        import live
        live.touch(lane_pair.session)          # (the session's watchdog counts it as a tester's command)
    yield lane_pair
    request.node.user_properties.append(("shots", list(lane_pair.shots)))
    request.node.user_properties.append(("notes", list(lane_pair.notes)))
    missing = lane_pair.no_ref()
    if missing:
        raise NoRef("NO-REF: no stored reference for %s (candidates under new/)" % ", ".join(missing))


# -- results ---------------------------------------------------------------------------------------------------
def _kind(reports):
    """One run's result from its setup / call / teardown reports."""
    kind = lambda r: getattr(r, "bt_kind", None)      # (pytest_runtest_makereport: the exception's type)
    for r in reports:
        if r.when == "setup" and r.failed:
            return "FAIL" if kind(r) == "crash" else "SETUP-FAIL"
        if r.when == "setup" and r.skipped:
            return "SKIP"
    for r in reports:
        if r.when == "call" and r.failed:
            return {"setup": "SETUP-FAIL", "noref": "NO-REF"}.get(kind(r), "FAIL")
        if r.when == "call" and r.skipped:
            return "SKIP"
    for r in reports:
        if r.when == "teardown" and r.failed and kind(r) == "noref":
            return "NO-REF"
    if any(r.failed for r in reports):
        return "FAIL"
    return "PASS"


def _past_deadline(config):
    n = config.getoption("deadline_min")
    return n is not None and time.time() - config._bt_started >= n * 60


def _not_run(item, why):
    """A NOT-RUN row for an item that never started (its own report, so xdist's controller gets it too)."""
    from _pytest.reports import TestReport
    rep = TestReport(item.nodeid, item.location, {}, "skipped", (str(item.path), 0, why), "teardown",
                     user_properties=[("result", "NOT-RUN"), ("seconds", 0), ("why", why)])
    item.ihook.pytest_runtest_logreport(report=rep)


def _stop_at_deadline(item):
    """Past --deadline-min: this item is NOT-RUN (and so is each one after it, as pytest offers it: under xdist a worker
    sees only its own share). The first time, the session's fixtures are torn down at once (the lane's games: lane_pair
    -> live.py stop: each game asked to quit through its bridge, killed only if still up after the wait, the lock let
    go), so the games are closed before pytest_sessionfinish writes the summary."""
    config = item.config
    why = "NOT-RUN: --deadline-min %g passed before it started" % config.getoption("deadline_min")
    item.ihook.pytest_runtest_logstart(nodeid=item.nodeid, location=item.location)
    _not_run(item, why)
    item.ihook.pytest_runtest_logfinish(nodeid=item.nodeid, location=item.location)
    if not getattr(config, "_bt_stopped", None):
        config._bt_stopped = why
        item.session._setupstate.teardown_exact(None)
    return True


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_protocol(item, nextitem):
    if _past_deadline(item.config):
        return _stop_at_deadline(item)
    item.ihook.pytest_runtest_logstart(nodeid=item.nodeid, location=item.location)
    t0 = time.monotonic()
    reports = runtestprotocol(item, nextitem=nextitem, log=False)
    kind, first = _kind(reports), None
    # a FAIL reruns once, but never one from set-up (the lane's boot: a GameCrash): that would boot the lane again
    in_setup = any(r.when == "setup" and r.failed for r in reports)
    if kind == "FAIL" and not in_setup and not item.config.getoption("no_rerun") and not _past_deadline(item.config):
        first = reports
        reports = runtestprotocol(item, nextitem=nextitem, log=False)
        kind = "FLAKY" if _kind(reports) == "PASS" else "FAIL"
        if kind == "FLAKY":
            reports = first          # (the failure is what the summary keeps: a FLAKY is never shown as a pass)
    secs = round(time.monotonic() - t0, 1)
    why = ""
    for r in reports:
        if r.failed:
            crash = getattr(r.longrepr, "reprcrash", None)
            why = " ".join((crash.message if crash else str(r.longrepr)).split())[:400]
            break
    for r in reports:
        r.user_properties.append(("result", kind))
        r.user_properties.append(("seconds", secs))
        r.user_properties.append(("why", why))
        item.ihook.pytest_runtest_logreport(report=r)
    item.ihook.pytest_runtest_logfinish(nodeid=item.nodeid, location=item.location)
    if item.get_closest_marker("smoke") and kind in ("FAIL", "SETUP-FAIL") and not item.config.getoption("no_smoke_gate"):
        item.session.shouldstop = "smoke %s %s: the rc stops here" % (item.name, kind)
    return True


def pytest_runtest_logreport(report):
    if report.when != "teardown":
        return
    props = dict(report.user_properties)
    if "result" not in props:
        return
    cfg = _CONFIG.get("c")
    if cfg is not None:
        cfg._bt_rows.append({"test": report.nodeid, "result": props["result"], "seconds": props.get("seconds"),
                             "why": props.get("why", ""), "shots": props.get("shots", []),
                             "notes": props.get("notes", []),
                             "worker": getattr(report, "node", None) and getattr(report.node, "gateway", None)
                             and report.node.gateway.id})


_CONFIG = {}


@pytest.hookimpl(optionalhook=True)
def pytest_testnodedown(node, error):
    """xdist controller: a worker's needs_env deselections (each worker collects the whole suite, so the same test can
    come from several: kept once), its lines printed here, so summary.json 'deselected' is right under -n too."""
    config = node.config
    seen = {d["test"] for d in config._bt_deselected}
    tr = config.pluginmanager.get_plugin("terminalreporter")
    for d in (getattr(node, "workeroutput", None) or {}).get("bt_deselected") or []:
        if d["test"] in seen:
            continue
        seen.add(d["test"])
        config._bt_deselected.append(d)
        line = _deselect_line(d)
        tr.write_line(line) if tr else print(line)


def pytest_sessionstart(session):
    _CONFIG["c"] = session.config


def pytest_sessionfinish(session, exitstatus):
    config = session.config
    if config.option.collectonly:
        return     # (--collect-only: no summary, no ingame-out/, pytest's own exit status: test-speed-c127-4)
    if getattr(config, "_bt_setup", None) is not None:
        session.exitstatus = 0 if config._bt_setup["ok"] else 1      # (--lane-check-only: no summary.json)
        return
    if hasattr(config, "workerinput"):
        config.workeroutput["bt_deselected"] = list(config._bt_deselected)   # (merged by the controller, below)
        return     # (xdist: the controller writes the one summary)
    rows = config._bt_rows
    counts = {k: sum(1 for r in rows if r["result"] == k) for k in RESULTS}
    out = config.getoption("out") or os.path.join(os.getcwd(), "ingame-out")
    os.makedirs(out, exist_ok=True)
    summary = {"started": config._bt_started, "minutes": round((time.time() - config._bt_started) / 60, 2),
               "lanes": config.getoption("lanes"), "fake": bool(config.getoption("fake")),
               "repeat": config.getoption("repeat"), "counts": counts, "tests": rows,
               "identity": {"env": boot_env(config), "expect_commit": config.getoption("expect_commit"),
                            "boot": dict(boot_identity(config.getoption("boot")), name=config.getoption("boot"))},
               "deselected": config._bt_deselected,
               "stopped": getattr(config, "_bt_stopped", None),
               "lane_closed": getattr(config, "_bt_lane_closed", None),
               "verdict": "PASS" if rows and counts["PASS"] == len(rows) else "NOT PASS"}
    with open(os.path.join(out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    lines = ["# In-game tests: %s" % summary["verdict"], "",
             "%s; %.1f min; lanes %s%s" % (", ".join("%s %d" % (k, v) for k, v in counts.items() if v),
                                         summary["minutes"], summary["lanes"], " (fake games)" if summary["fake"] else ""),
             "", "| Result | Test | s | Why |", "|---|---|---|---|"]
    for r in rows:
        lines.append("| %s | %s | %s | %s |" % (r["result"], r["test"], r["seconds"], r["why"].replace("|", "/")))
    with open(os.path.join(out, "summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    if summary["verdict"] != "PASS" and session.exitstatus == 0:
        session.exitstatus = 1        # (NOT-RUN / SKIP rows are pytest skips: a NOT PASS run never exits 0)
    print(result_line(summary), flush=True)   # (stdout: the queue's log shows the reason, never NO-REASON)


def result_line(summary):
    """The run's one line: 'RESULT: PASS', 'RESULT: SETUP-FAIL <why>' when nothing got past set-up, else
    'RESULT: NOT PASS <counts>; first: <test> <why>'."""
    rows, c = summary["tests"], summary["counts"]
    if rows and c["PASS"] == len(rows):
        return "RESULT: PASS (%d tests, %.1f min)" % (len(rows), summary["minutes"])
    if not rows:
        return "RESULT: NOT PASS (no test ran%s)" % ("; deselected %d" % len(summary.get("deselected") or []))
    counts = ", ".join("%s %d" % (k, v) for k, v in c.items() if v)
    bad = next(r for r in rows if r["result"] != "PASS")
    if c["SETUP-FAIL"] == len(rows):
        return "RESULT: SETUP-FAIL %s" % bad["why"]
    return "RESULT: NOT PASS (%s); first: %s %s %s" % (counts, bad["test"], bad["result"], bad["why"][:300])
