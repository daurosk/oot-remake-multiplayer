"""Selftests of the in-game test system (tools/autotest/ingame, docs/ingame-tests.md), no game: fake bridges.

Needs pytest, pytest-xdist, pytest-timeout, Pillow and scikit-image (the tools venv and CI's python-tools job have them).
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, INGAME)
import pair as P          # noqa: E402
import imagecheck         # noqa: E402


def pytest_run(args, cwd=INGAME, env=None):
    e = dict(os.environ)
    e.update(env or {})
    p = subprocess.run([sys.executable, "-m", "pytest"] + args, cwd=cwd, capture_output=True, text=True, timeout=600,
                       env=e)
    return p


def suite_ids(marks=None):
    """The real suite's nodeids (its own --collect-only), e.g. the look tests: suite_ids("look")."""
    out = tempfile.mkdtemp()
    args = [".", "--fake", "--collect-only", "-q", "-p", "no:cacheprovider", "--out", out] + (["-m", marks] if marks else [])
    p = pytest_run(args)
    shutil.rmtree(out, ignore_errors=True)
    return [ln.strip() for ln in p.stdout.splitlines() if "::" in ln and not ln.startswith("deselected")]


def suite_size(marks=None):
    """How many tests the real suite runs (its own collection, as a run with no --env deselects): the counts below
    follow every row added to ingame/, so a new row port does not break these selftests (row-53-ocarina-5)."""
    out = tempfile.mkdtemp()
    args = [".", "--fake", "--collect-only", "-q", "-p", "no:cacheprovider", "--out", out] + (["-m", marks] if marks else [])
    p = pytest_run(args)
    shutil.rmtree(out, ignore_errors=True)
    return sum(1 for ln in p.stdout.splitlines() if "::" in ln and not ln.startswith("deselected"))


MISSING_REF = "NO-REF: no stored reference"    # (bt_plugin's pair fixture: the text of a missing reference's NoRef)


def summary(out):
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        return json.load(f)


def results(s):
    return {r["test"].split("::")[-1]: r["result"] for r in s["tests"]}


class Suite(unittest.TestCase):
    """The real suite (smoke + pilot rows) against two fake games."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_no_refs_then_refs(self):
        out1, refs = os.path.join(self.tmp, "o1"), os.path.join(self.tmp, "refs")
        os.makedirs(refs)
        p = pytest_run([".", "--fake", "--out", out1, "-q"], env={"BT_FAKE_REFS": refs})
        s = summary(out1)
        total, looks = suite_size(), suite_size("look")
        # (vest-link-2 11:27) a NO-REF is either a missing stored reference (bt_plugin's 'NO-REF: no stored reference')
        # or a test that cannot prove its row yet (pair.unproven): only the first kind goes away once refs exist
        missing = [r["test"] for r in s["tests"] if r["result"] == "NO-REF" and MISSING_REF in r["why"]]
        unproven = [r["test"] for r in s["tests"] if r["result"] == "NO-REF" and MISSING_REF not in r["why"]]
        self.assertLessEqual(len(missing), looks, p.stdout[-2000:])
        # (review-c123-9 on 423c5e3d9) exact: EVERY look test is missing its ref or unproven on run 1, so a look test
        # that PASSes with no stored reference is still caught (the screenshot rule's guard)
        look_ids = suite_ids("look")
        self.assertEqual(len(look_ids), looks)
        self.assertLessEqual(set(look_ids), set(missing) | set(unproven), (look_ids, missing, unproven))
        self.assertEqual(s["counts"]["NO-REF"], len(missing) + len(unproven), p.stdout[-2000:])
        self.assertEqual(s["counts"]["PASS"], total - len(missing) - len(unproven), p.stdout[-2000:])
        self.assertEqual(s["verdict"], "NOT PASS")
        self.assertNotEqual(p.returncode, 0)
        new = os.path.join(out1, "lane1", "new")
        for f in os.listdir(new):                  # (a person approves the candidates: here, all of them)
            shutil.copyfile(os.path.join(new, f), os.path.join(refs, f))
        out2 = os.path.join(self.tmp, "o2")
        p = pytest_run([".", "--fake", "--out", out2, "-q"], env={"BT_FAKE_REFS": refs})
        s = summary(out2)
        self.assertFalse([r["test"] for r in s["tests"] if MISSING_REF in r["why"]], p.stdout[-2000:])
        still = sorted(r["test"] for r in s["tests"] if r["result"] == "NO-REF")
        self.assertEqual(still, sorted(unproven), p.stdout[-2000:])      # (only the unproven ones stay NO-REF)
        self.assertEqual(s["counts"]["PASS"], total - len(unproven), p.stdout[-2000:])
        self.assertEqual(s["verdict"], "PASS" if not unproven else "NOT PASS")
        self.assertEqual(p.returncode == 0, not unproven)
        # (derived, not a fixed total, so a row port's look test never edits this: crawl-337-9) every look test kept
        # shots, each test that shot did so on BOTH games, a few per test (not hundreds), all matching their refs
        shots = [sh for r in s["tests"] for sh in r["shots"]]
        by_test = {r["test"]: r["shots"] for r in s["tests"] if r["shots"]}
        self.assertGreaterEqual(len(by_test), looks, by_test.keys())
        for t, sh in by_test.items():
            self.assertEqual({x["game"] for x in sh}, {"A", "B"}, t)
            self.assertLessEqual(len(sh), 8, t)
        self.assertTrue(all(sh["ok"] and sh["score"] >= sh["tolerance"] for sh in shots))

    def test_default_suite_has_no_setup_fail(self):
        """The default run (no --env) of the real suite: 0 SETUP-FAIL (a needs_env test is deselected, B #129)."""
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--out", out, "-q", "-m", "not look"])
        s = summary(out)
        self.assertEqual(s["counts"]["SETUP-FAIL"], 0, p.stdout[-2000:])
        self.assertTrue(s["tests"])
        for d in s["deselected"]:
            self.assertTrue(d["needs_env"] or d.get("needs_boot") or d.get("needs_pack") or d.get("needs_solo"), d)   # (a boss row's boot;
            # a --pack test: port-sweep-c131-6 11:46)

    def test_repeat_keeps_one_boot_and_orders_by_round(self):
        out = os.path.join(self.tmp, "o")
        pytest_run([".", "--fake", "--out", out, "-q", "--repeat", "2", "--no-smoke-gate"])
        s = summary(out)
        names = [r["test"] for r in s["tests"]]
        n = suite_size()
        self.assertEqual(len(names), 2 * n)
        self.assertTrue(all("[r1]" in m for m in names[:n]) and all("[r2]" in m for m in names[n:]), names)
        self.assertIn("test_boot", names[0])     # smoke first in each round
        self.assertIn("test_boot", names[n])

    def test_two_lanes_at_once(self):
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--out", out, "-q", "-n", "2", "--lanes", "1,2", "-m", "not look"])
        s = summary(out)
        self.assertEqual(s["counts"]["PASS"], len(s["tests"]), p.stdout[-2000:])
        self.assertEqual(len(s["tests"]), suite_size("not look"))
        self.assertTrue(os.path.isdir(os.path.join(out, "lane1")) and os.path.isdir(os.path.join(out, "lane2")))


LABEL_TESTS = '''
import os, pytest, pair as P

def _count(name):
    f = os.path.join(os.environ["BT_COUNT_DIR"], name)
    n = int(open(f).read()) if os.path.exists(f) else 0
    open(f, "w").write(str(n + 1))
    return n

def test_ok(pair):
    pair.step_both(5)

def test_flaky(pair):
    assert _count("flaky") > 0, "fails the first time only"

def test_always_fails(pair):
    _count("always")
    assert False, "never holds"

def test_setup_fails(pair):
    with pair.setup():
        raise RuntimeError("no such scene")

@pytest.mark.budget(1)
def test_times_out(pair):
    pair.until("never", lambda: False, max_frames=10 ** 9)

@pytest.mark.smoke
def test_smoke_ok(pair):
    pass

def test_fails_then_skips(pair):
    if _count("fskip") > 0:
        pytest.skip("the rerun skips (a test that changed the boot)")
    assert False, "the first run's own reason"

def test_unproven(pair):
    pair.unproven("no measured value yet (selftest)")

def test_fail_naming_noref(pair):
    assert False, "NO-REF SetupFail GameCrash AssertionError: words in a FAIL's own message"
'''


class Labels(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        d = os.path.join(self.tmp, "t")
        os.makedirs(d)
        with open(os.path.join(d, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(d, "test_labels.py"), "w", encoding="utf-8") as f:
            f.write(LABEL_TESTS)
        self.dir, self.out = d, os.path.join(self.tmp, "o")
        self.env = {"BT_COUNT_DIR": self.tmp}

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_every_label(self):
        p = pytest_run([".", "--fake", "--out", self.out, "-q", "-p", "no:cacheprovider"], cwd=self.dir, env=self.env)
        r = results(summary(self.out))
        self.assertEqual(r, {"test_smoke_ok": "PASS", "test_ok": "PASS", "test_flaky": "FLAKY",
                             "test_always_fails": "FAIL", "test_setup_fails": "SETUP-FAIL",
                             "test_times_out": "FAIL", "test_fail_naming_noref": "FAIL", "test_unproven": "NO-REF",
                             "test_fails_then_skips": "FAIL"},
                         p.stdout[-3000:])
        rows = {x["test"].split("::")[-1]: x for x in summary(self.out)["tests"]}
        self.assertIn("timeout", rows["test_times_out"]["why"])
        self.assertIn("set-up: RuntimeError: no such scene", rows["test_setup_fails"]["why"])
        self.assertIn("the first run's own reason", rows["test_fails_then_skips"]["why"])   # (review-rows-c130 11:10)
        self.assertTrue(rows["test_fails_then_skips"]["rerun"].startswith("SKIP"), rows["test_fails_then_skips"])
        self.assertEqual(rows["test_fails_then_skips"]["rerun"],                    # (the skip's message, not a tuple)
                         "SKIP: Skipped: the rerun skips (a test that changed the boot)")
        self.assertIn("fails the first time only", rows["test_flaky"]["why"])                # (FLAKY: the first failure)
        self.assertTrue(rows["test_flaky"]["rerun"].startswith("PASS"), rows["test_flaky"])
        self.assertEqual(open(os.path.join(self.tmp, "always")).read(), "2")   # rerun once, no more
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(summary(self.out)["tests"][0]["test"].split("::")[-1], "test_smoke_ok")   # smoke first

    def test_smoke_gate_stops_the_run(self):
        with open(os.path.join(self.dir, "test_labels.py"), "a", encoding="utf-8") as f:
            f.write("\n@pytest.mark.smoke\ndef test_smoke_bad(pair):\n    assert False\n")
        pytest_run([".", "--fake", "--out", self.out, "-q"], cwd=self.dir, env=self.env)
        r = results(summary(self.out))
        self.assertEqual(r, {"test_smoke_ok": "PASS", "test_smoke_bad": "FAIL"})


class LaneCheckOnly(unittest.TestCase):
    """--lane-check-only: the setup proof without a boot (no test runs, no summary.json)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_fake_lane_ok(self):
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--lane-check-only", "--out", out, "-q"])
        self.assertEqual(p.returncode, 0, p.stdout[-2000:])
        self.assertIn("RESULT: SETUP-ONLY OK", p.stdout)
        with open(os.path.join(out, "setup.json"), encoding="utf-8") as f:
            s = json.load(f)
        self.assertTrue(s["ok"])
        self.assertEqual(s["collected"], suite_size())
        self.assertFalse(os.path.exists(os.path.join(out, "summary.json")))

    def test_real_lane_needs_expect_commit(self):
        out, ls = os.path.join(self.tmp, "o"), os.path.join(self.tmp, "ls")   # (an empty lanes folder)
        os.makedirs(ls)
        p = pytest_run([".", "--lane-check-only", "--kit-ls", ls, "--out", out, "-q"])
        self.assertEqual(p.returncode, 1, p.stdout[-2000:])
        self.assertIn("RESULT: SETUP-FAIL", p.stdout)
        self.assertIn("lane-server.txt", p.stdout)       # lane_check first: the lane is not installed


ENV_TESTS = """
import pytest

@pytest.mark.needs_env("LICHII_COOP_X=1")
def test_needs_x_both(pair):
    pass

@pytest.mark.needs_env("A:LICHII_COOP_Y=2")
def test_needs_y_on_a(pair):
    pass
"""


class BootEnv(unittest.TestCase):
    """--env: game switches per boot (both games, or A:/B: one), needs_env, kept in summary.json identity."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        d = os.path.join(self.tmp, "t")
        os.makedirs(d)
        with open(os.path.join(d, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(d, "test_env.py"), "w", encoding="utf-8") as f:
            f.write(ENV_TESTS)
        self.dir, self.out = d, os.path.join(self.tmp, "o")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_boot_crash_reason(self):
        import bt_plugin as B
        ses = os.path.join(self.tmp, "ses")
        os.makedirs(ses)
        la, lb = os.path.join(ses, "game-out-A.txt"), os.path.join(ses, "game-out-B.txt")
        with open(la, "w", encoding="utf-8") as f:
            f.write("coop: start\nCRASH: exception 0xc0000005 at hookshot_coop.dll+0x50594\nmore\n")
        with open(lb, "w", encoding="utf-8") as f:
            f.write("coop: start\n")
        with open(os.path.join(ses, "session.json"), "w", encoding="utf-8") as f:
            json.dump({"games": {"A": {"log": la}, "B": {"log": lb}}}, f)
        self.assertEqual(B._crash_why(ses), "; A: CRASH: exception 0xc0000005 at hookshot_coop.dll+0x50594")
        self.assertEqual(B._crash_why(os.path.join(self.tmp, "none")), "")

    def test_boot_crash_report_file(self):
        """A game that crashed with no module line: its new bug-reports/pending/crash-*.txt names the crash; an older
        one (before the session started) does not (pganon-cct-287-3 10:23)."""
        import time as _t
        import bt_plugin as B
        ses, game = os.path.join(self.tmp, "ses2"), os.path.join(self.tmp, "gameA")
        pend = os.path.join(game, "bug-reports", "pending")
        os.makedirs(ses)
        os.makedirs(pend)
        old = os.path.join(pend, "crash-20261126T000000Z.txt")
        with open(old, "w", encoding="utf-8") as f:
            f.write("old")
        os.utime(old, (_t.time() - 3600, _t.time() - 3600))
        log = os.path.join(ses, "game-out-A.txt")
        open(log, "w").close()
        with open(os.path.join(ses, "session.json"), "w", encoding="utf-8") as f:
            json.dump({"started": _t.time() - 60, "games": {"A": {"log": log, "folder": game}}}, f)
        self.assertEqual(B._crash_why(ses), "")
        with open(os.path.join(pend, "crash-20261126T095000Z.txt"), "w", encoding="utf-8") as f:
            f.write("new")
        self.assertIn("A: crash report ", B._crash_why(ses))
        self.assertIn("crash-20261126T095000Z.txt", B._crash_why(ses))

    def test_deselected_under_xdist(self):
        out = os.path.join(self.tmp, "ox")
        p = pytest_run([".", "--fake", "--out", out, "-q", "-n", "2", "--lanes", "1,2", "--env", "LICHII_COOP_X=1"],
                       cwd=self.dir)
        s = summary(out)
        self.assertEqual(results(s), {"test_needs_x_both": "PASS"}, p.stdout[-2000:])
        self.assertEqual(s["deselected"], [{"test": "test_env.py::test_needs_y_on_a", "needs_env": ["A:LICHII_COOP_Y=2"]}])
        self.assertEqual(p.stdout.count("deselected: needs_env A:LICHII_COOP_Y=2 test_env.py::test_needs_y_on_a"), 1,
                         p.stdout[-2000:])

    def test_boot_crash_is_fail_not_setup_fail(self):
        out = os.path.join(self.tmp, "oc")
        boots = os.path.join(self.tmp, "boots.txt")
        p = pytest_run([".", "--fake", "--out", out, "-q", "--env", "LICHII_COOP_X=1", "--env", "A:LICHII_COOP_Y=2"],
                       cwd=self.dir, env={"BT_FAKE_BOOT_CRASH": "CRASH: exception 0xc0000005", "BT_FAKE_BOOT_LOG": boots})
        with open(boots, encoding="utf-8") as f:     # (a FAIL from the lane's boot is never rerun: one boot only)
            self.assertEqual(f.read().count("boot"), 1, p.stdout[-1500:])
        s = summary(out)
        self.assertEqual(results(s), {"test_needs_x_both": "FAIL", "test_needs_y_on_a": "FAIL"}, p.stdout[-2000:])
        self.assertTrue(all("CRASH: exception 0xc0000005" in r["why"] for r in s["tests"]), s["tests"])
        self.assertNotEqual(p.returncode, 0)

    def test_env_has(self):
        import bt_plugin as B
        self.assertTrue(B.env_has(["X=1"], "X=1"))
        self.assertTrue(B.env_has(["A:X=1", "B:X=1"], "X=1"))
        self.assertFalse(B.env_has(["A:X=1"], "X=1"))
        self.assertTrue(B.env_has(["X=1"], "A:X=1"))
        self.assertFalse(B.env_has(["X=1", "A:X=0"], "A:X=1"))     # the last setting on a game wins
        self.assertFalse(B.env_has(["X=2"], "X=1"))

    def test_missing_switch_is_setup_fail_and_given_is_pass(self):
        p = pytest_run([".", "--fake", "--no-rerun", "--out", self.out, "-q", "--env", "LICHII_COOP_X=1"], cwd=self.dir)
        s = summary(self.out)       # (B #129: lacking a switch = deselected, named, never SETUP-FAIL and never hidden)
        self.assertEqual(results(s), {"test_needs_x_both": "PASS"}, p.stdout[-2000:])
        self.assertEqual(s["deselected"], [{"test": "test_env.py::test_needs_y_on_a", "needs_env": ["A:LICHII_COOP_Y=2"]}])
        self.assertIn("deselected: needs_env A:LICHII_COOP_Y=2 test_env.py::test_needs_y_on_a", p.stdout)
        out0 = os.path.join(self.tmp, "o0")
        pytest_run([".", "--fake", "--out", out0, "-q"], cwd=self.dir)
        s = summary(out0)
        self.assertEqual(s["tests"], [])
        self.assertEqual(len(s["deselected"]), 2)
        self.assertEqual(s["verdict"], "NOT PASS")      # (nothing ran: never a pass)
        out2 = os.path.join(self.tmp, "o2")
        p = pytest_run([".", "--fake", "--out", out2, "-q", "--env", "LICHII_COOP_X=1", "--env", "A:LICHII_COOP_Y=2"],
                       cwd=self.dir)
        s = summary(out2)
        self.assertEqual(s["counts"]["PASS"], 2, p.stdout[-2000:])
        self.assertEqual(s["identity"]["env"][-2:], ["LICHII_COOP_X=1", "A:LICHII_COOP_Y=2"])

    def test_bad_env_and_n_with_lane_check_only_are_refused(self):
        p = pytest_run([".", "--fake", "--out", self.out, "-q", "--env", "C:X=1"], cwd=self.dir)
        self.assertEqual(p.returncode, 4, p.stdout[-1000:] + p.stderr[-1000:])     # pytest's usage error
        p = pytest_run([".", "--fake", "--out", self.out, "-q", "--lane-check-only", "-n", "2", "--lanes", "1,2"],
                       cwd=self.dir)
        self.assertEqual(p.returncode, 4, p.stdout[-1000:] + p.stderr[-1000:])
        self.assertIn("leave out -n", p.stdout + p.stderr)


DEADLINE_TESTS = """
import time, pytest

@pytest.mark.smoke
def test_first(pair):
    time.sleep(2.0)          # (wall clock on purpose: it carries the run past --deadline-min)

def test_second(pair):
    pass

def test_third(pair):
    pass
"""


class DeadlineMin(unittest.TestCase):
    """--deadline-min: past it no test starts, the rest are NOT-RUN (never PASS), the lane is closed, summary written."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        d = os.path.join(self.tmp, "t")
        os.makedirs(d)
        with open(os.path.join(d, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(d, "test_deadline.py"), "w", encoding="utf-8") as f:
            f.write(DEADLINE_TESTS)
        self.dir, self.out = d, os.path.join(self.tmp, "o")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_rest_not_run_lane_closed_summary_written(self):
        p = pytest_run([".", "--fake", "--out", self.out, "-q", "--deadline-min", "0.02"], cwd=self.dir)
        s = summary(self.out)
        self.assertEqual(results(s), {"test_first": "PASS", "test_second": "NOT-RUN", "test_third": "NOT-RUN"},
                         p.stdout[-2000:])
        self.assertEqual(s["counts"]["NOT-RUN"], 2)
        self.assertEqual(s["verdict"], "NOT PASS")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("--deadline-min 0.02", s["stopped"])
        self.assertTrue(s["lane_closed"])          # (the lane's fixture ran its teardown before the summary)
        self.assertIn("NOT-RUN", [r for r in s["tests"] if r["test"].endswith("test_third")][0]["why"])

    def test_no_deadline_runs_all(self):
        p = pytest_run([".", "--fake", "--out", self.out, "-q"], cwd=self.dir)
        s = summary(self.out)
        self.assertEqual(s["counts"]["PASS"], 3, p.stdout[-2000:])
        self.assertIsNone(s["stopped"])


STOP_CONFTEST = """import json, os, subprocess, sys, time
sys.path.insert(0, %r)
import bt_plugin as B
pytest_plugins = ['bt_plugin']
LOG, MODE = os.environ['BT_STOP_LOG'], os.environ['BT_START_MODE']

def fake_live(args, timeout):
    with open(LOG, 'a') as f:
        f.write(args[0] + ' ')
    if args[0] != 'start':
        return {'ok': True, 'ended': 'stop asked'}
    sess = args[args.index('--session') + 1]
    if MODE not in ('refused', 'stale'):          # (live.py start wrote its session: games may be up)
        os.makedirs(sess, exist_ok=True)
        with open(os.path.join(sess, 'session.json'), 'w') as f:
            json.dump({'started': time.time(), 'games': {}}, f)
    if MODE == 'timeout':
        raise subprocess.TimeoutExpired('live.py start', timeout)
    if MODE == 'misread':                         # (C #132 11:05: games booted, the answer not read)
        return {'ok': False, 'error': 'live.py start: exit 0, no JSON answer: ... renderer ready'}
    if MODE == 'played':
        return {'ok': False, 'boot': 'FAILED (see steps.txt): the session is closed'}
    if MODE in ('notready', 'nomodule'):          # (started; the boot proof finds B elsewhere, or B has no module)
        return {'ok': True}
    return {'ok': False, 'lock': 'not taken', 'error': 'refused before the session'}

B._live = fake_live
B._real_lane_info = lambda config, lane: {'server': 'srv', 'commit': 'abc'}

class _Hello:
    def hello(self):
        return {'control': True}

class _NotReadyPair:                              # (test-bridge-pilot-fa63e90f r1: B in Graveyard_Child in an event)
    def __init__(self, *a, **k):
        self.c, self.notes = {'A': _Hello(), 'B': _Hello()}, []
    def begin(self, seconds):
        pass
    def module_started(self):
        if MODE == 'nomodule':                   # (B #137: party-hud-bridge-85e3b255, B's module never started)
            raise B.P.SetupFail('B: the module never started (no listening line)')
        return True
    def boot_ready(self, scene):
        raise B.P.SetupFail('boot: not in the boot scene ' + str(scene) + ': B: scene=Graveyard_Child event=True')
    def close(self):
        pass

if MODE in ('notready', 'nomodule'):
    B.P.read_session = lambda session: {}
    B.P.Pair = _NotReadyPair
"""


class BootStopsSession(unittest.TestCase):
    """C #133 (party-hud-bridge-fa63e90f 11:05: a SETUP-FAIL boot left both lane-2 games running): every way out of
    lane_pair's boot runs live.py stop on the session its start wrote; none when the start wrote no session, or the
    session.json there is an earlier run's (its pids may be another program's now)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.dir, self.out = os.path.join(self.tmp, "t"), os.path.join(self.tmp, "o")
        os.makedirs(self.dir)
        with open(os.path.join(self.dir, "conftest.py"), "w", encoding="utf-8") as f:
            f.write(STOP_CONFTEST % INGAME)
        with open(os.path.join(self.dir, "test_boot.py"), "w", encoding="utf-8") as f:
            f.write("def test_one(pair):\n    pass\n\ndef test_two(pair):\n    pass\n")
        self.log = os.path.join(self.tmp, "calls.txt")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_mode(self, mode):
        p = pytest_run([".", "--out", self.out, "-q", "-p", "no:cacheprovider", "--expect-commit", "abc"],
                       cwd=self.dir, env={"BT_STOP_LOG": self.log, "BT_START_MODE": mode})
        calls = open(self.log).read().split() if os.path.exists(self.log) else []
        return p, calls, summary(self.out)

    def test_every_failed_start_that_wrote_a_session_is_stopped(self):
        for mode in ("misread", "played", "timeout"):
            shutil.rmtree(self.out, ignore_errors=True)
            if os.path.exists(self.log):
                os.remove(self.log)
            p, calls, s = self.run_mode(mode)
            self.assertEqual(calls, ["start", "stop"], (mode, p.stdout[-2000:]))   # one boot, one stop
            self.assertNotEqual(s["verdict"], "PASS", mode)
            self.assertEqual(s["lane_closed"], {"ok": True, "ended": "stop asked"}, mode)

    def test_a_boot_not_in_play_is_stopped_and_setup_fail_boot(self):
        """review-c123-9 12:10: the boot proof (Pair.boot_ready) runs inside lane_pair's try, so its SetupFail ends
        with the session stopped like a failed start."""
        p, calls, s = self.run_mode("notready")
        self.assertEqual(calls, ["start", "stop"], p.stdout[-2000:])
        self.assertEqual(set(results(s).values()), {"SETUP-FAIL"}, p.stdout[-2000:])
        self.assertTrue(all("lane1 boot: not in the boot scene" in r["why"] for r in s["tests"]), s["tests"])
        self.assertEqual(s["lane_closed"], {"ok": True, "ended": "stop asked"})

    def test_a_game_with_no_module_is_stopped_and_setup_fail_boot(self):
        """B #137 14:57: the module proof runs before any bridge call, inside lane_pair's try: its SetupFail ends with
        the session stopped, every test SETUP-FAIL 'lane1 boot: B: the module never started'."""
        p, calls, s = self.run_mode("nomodule")
        self.assertEqual(calls, ["start", "stop"], p.stdout[-2000:])
        self.assertEqual(set(results(s).values()), {"SETUP-FAIL"}, p.stdout[-2000:])
        self.assertTrue(all("lane1 boot: B: the module never started" in r["why"] for r in s["tests"]), s["tests"])

    def test_no_session_written_no_stop(self):
        p, calls, s = self.run_mode("refused")
        self.assertEqual(calls, ["start"], p.stdout[-2000:])
        self.assertEqual(set(results(s).values()), {"SETUP-FAIL"})

    def test_an_earlier_runs_session_is_not_stopped(self):
        sess = os.path.join(self.out, "lane1", "session")
        os.makedirs(sess)
        with open(os.path.join(sess, "session.json"), "w") as f:
            json.dump({"started": 1000.0, "games": {}}, f)
        p, calls, s = self.run_mode("stale")
        self.assertEqual(calls, ["start"], p.stdout[-2000:])


class HookOrder(unittest.TestCase):
    """bt_plugin's runtest protocol must answer before pytest's own (both are firstresult): without tryfirst the FLAKY
    rerun, NOT-RUN and --deadline-min logic could be skipped silently (review-c123-6, 646484a63)."""

    def test_runtest_protocol_is_tryfirst(self):
        import bt_plugin as B
        opts = getattr(B.pytest_runtest_protocol, "pytest_impl", None) or {}
        self.assertTrue(opts.get("tryfirst"), "bt_plugin.pytest_runtest_protocol lost @pytest.hookimpl(tryfirst=True)")
        self.assertFalse(hasattr(B._past_deadline, "pytest_impl"), "the hookimpl decorator sits on a helper")


AUTOTEST = os.path.dirname(HERE)
START_DRIVER = 'import sys\nsys.path.insert(0, sys.argv[1])\nimport hookshot, live\ndef fake_main(argv):\n    if "--check-args" in argv:\n        return 0\n    print("00:00:01 A: started pid 1, bridge coop-x-A")          # (hookshot.py run\'s step log: Log(echo=print))\n    print("00:02:13 A: renderer ready (133 s waited)")\n    print("00:03:00 scenario played", end=" ")                   # (no newline before the JSON: the 11:05 shape)\n    return 0\nhookshot.main = fake_main\nlive.spawn_watchdog = lambda d: None\nlive.wait_lock = lambda d, proc, timeout: "none"\nsys.exit(live.main(sys.argv[2:]))\n'


class BootCommand(unittest.TestCase):
    """The lane's boot command is proven before any run (pilot test-bridge-pilot-967cac67 VOID 11-26: live.py start had
    no --boot-a, hookshot.py run refused it, and the refused start kept lane1.lock with no games)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        for x in ("a", "b"):
            os.makedirs(os.path.join(self.tmp, x))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def live(self, *args):
        p = subprocess.run([sys.executable, "-I", os.path.join(AUTOTEST, "live.py"), "start"] + list(args),
                           capture_output=True, text=True, timeout=120)
        return json.loads(p.stdout)

    def games(self):
        return ["--game-a", os.path.join(self.tmp, "a"), "--game-b", os.path.join(self.tmp, "b")]

    def test_check_args(self):
        hs = [sys.executable, "-I", os.path.join(AUTOTEST, "hookshot.py"), "run", "--out", self.tmp] + self.games()
        p = subprocess.run(hs + ["--check-args"], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
        self.assertIn("--script-a", p.stderr)
        bad = os.path.join(self.tmp, "bad.txt")
        with open(bad, "w", encoding="utf-8") as f:
            f.write("frobnicate 3\n")
        p = subprocess.run(hs + ["--script-a", bad, "--check-args"], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
        import bt_plugin as B
        p = subprocess.run(hs + ["--script-a", B.BOOT_A, "--script-b", B.BOOT_B, "--check-args"], capture_output=True,
                           text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_result_line(self):
        import bt_plugin as B
        c = {k: 0 for k in B.RESULTS}
        row = lambda t, r, why="": {"test": t, "result": r, "why": why}
        self.assertEqual(B.result_line({"tests": [row("t1", "PASS")], "counts": dict(c, PASS=1), "minutes": 1.5}),
                         "RESULT: PASS (1 tests, 1.5 min)")
        s = {"tests": [row("t1", "SETUP-FAIL", "lane1 boot: no"), row("t2", "SETUP-FAIL", "lane1 boot: no")],
             "counts": dict(c, **{"SETUP-FAIL": 2}), "minutes": 0}
        self.assertEqual(B.result_line(s), "RESULT: SETUP-FAIL lane1 boot: no")
        s = {"tests": [row("t1", "PASS"), row("t2", "FLAKY", "x")], "counts": dict(c, PASS=1, FLAKY=1), "minutes": 0}
        self.assertEqual(B.result_line(s), "RESULT: NOT PASS (PASS 1, FLAKY 1); first: t2 FLAKY x")

    def test_crash_line_needs_crash_exception(self):
        import bt_plugin as B
        ses = os.path.join(self.tmp, "ses")
        os.makedirs(ses)
        la = os.path.join(ses, "game-out-A.txt")
        with open(la, "w", encoding="utf-8") as f:
            f.write("coop: CRASH handler installed\n")      # (a mention, not a crash: a plain boot failure stays SETUP-FAIL)
        with open(os.path.join(ses, "session.json"), "w", encoding="utf-8") as f:
            json.dump({"games": {"A": {"log": la}}}, f)
        self.assertEqual(B._crash_why(ses), "")

    def test_start_stdout_is_one_json_answer(self):
        """C #132 11:05 (party-hud-bridge-fa63e90f VOID): live.py start ran hookshot.main in-process, whose step log
        echoes to stdout, so stdout was boot lines + the JSON and _live read ok false. The REAL live.py start path
        (only the runner, watchdog and lock stubbed): its stdout is now ONLY the JSON answer, and _live reads it."""
        import bt_plugin as B
        drv = os.path.join(self.tmp, "drv.py")
        with open(drv, "w", encoding="utf-8") as f:
            f.write(START_DRIVER)
        ses = os.path.join(self.tmp, "s")
        p = subprocess.run([sys.executable, "-I", drv, AUTOTEST, "start", "--session", ses] + self.games() +
                           ["--boot-a", B.BOOT_A, "--boot-b", B.BOOT_B, "--no-load"],
                           capture_output=True, text=True, timeout=120)
        out = json.loads(p.stdout)                   # (the whole stdout: one JSON object)
        self.assertTrue(out["ok"], (out, p.stderr[-800:]))
        self.assertIn("renderer ready", p.stderr)    # (the boot lines went to stderr)

    def test_last_json_reads_the_old_mixed_shape(self):
        import bt_plugin as B
        mixed = 'A: started\nA: renderer ready (133 s)\nscenario played { "ok": true,\n "session": "s", "lane": 1}\n'
        self.assertEqual(B.last_json(mixed), {"ok": True, "session": "s", "lane": 1})
        self.assertEqual(B.last_json('{"ok": true}\nlog line after it'), None)    # (must end the output)
        self.assertEqual(B.last_json('{"a": {"b": 1}}'), {"a": {"b": 1}})          # (an inner '{' is not the answer)
        self.assertIsNone(B.last_json("no json at all"))

    def test_refused_start_leaves_no_watchdog_or_lock(self):
        ses = os.path.join(self.tmp, "s")
        out = self.live("--session", ses, "--playtest", *self.games())
        self.assertFalse(out["ok"])
        self.assertEqual(out["lock"], "not taken")
        self.assertIn("--script-a", out["error"])
        self.assertFalse(os.path.exists(os.path.join(ses, "watchdog.json")))
        self.assertFalse(os.path.exists(os.path.join(ses, "session.json")))

    def test_boot_identity(self):
        """The boot scripts and their switches are part of summary.json identity (the proof key sees the boot)."""
        import bt_plugin as B
        idn = B.boot_identity()
        self.assertEqual(idn["A"]["script"], "tools/autotest/scenarios/live-kakariko-child.A.txt")
        self.assertIn("LICHII_COOP_CHEATS=1", idn["A"]["switches"])
        self.assertEqual(len(idn["B"]["sha256"]), 64)
        out = os.path.join(self.tmp, "o")
        pytest_run([".", "--fake", "--out", out, "-q", "-m", "not look", "-k", "test_boot"])
        self.assertEqual(summary(out)["identity"]["boot"], dict(idn, name="live-kakariko-child"))

    def test_needs_boot(self):
        """--boot NAME: only tests of that boot run (needs_boot(NAME); unmarked = the default boot); the rest are
        deselected and named. An unknown boot is a usage error before any boot."""
        d = os.path.join(self.tmp, "nb")
        os.makedirs(d)
        with open(os.path.join(d, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(d, "test_nb.py"), "w", encoding="utf-8") as f:
            f.write("import pytest\n\ndef test_plain(pair):\n    pass\n\n"
                    "@pytest.mark.needs_boot('live-twinrova-room')\ndef test_boss(pair):\n    pass\n")
        out = os.path.join(self.tmp, "o1")
        p = pytest_run([".", "--fake", "--out", out, "-q"], cwd=d)
        s = summary(out)
        self.assertEqual(results(s), {"test_plain": "PASS"}, p.stdout[-2000:])
        self.assertEqual(s["deselected"], [{"test": "test_nb.py::test_boss", "needs_boot": "live-twinrova-room",
                                            "needs_env": []}])
        self.assertIn("deselected: needs_boot live-twinrova-room test_nb.py::test_boss", p.stdout)
        out = os.path.join(self.tmp, "o2")
        p = pytest_run([".", "--fake", "--out", out, "-q", "--boot", "live-twinrova-room"], cwd=d)
        s = summary(out)
        self.assertEqual(results(s), {"test_boss": "PASS"}, p.stdout[-2000:])
        self.assertEqual(s["identity"]["boot"]["name"], "live-twinrova-room")
        self.assertTrue(s["identity"]["boot"]["A"]["script"].endswith("live-twinrova-room.A.txt"))
        p = pytest_run([".", "--fake", "--out", out, "-q", "--boot", "no-such-boot"], cwd=d)
        self.assertEqual(p.returncode, 4, p.stdout[-1000:] + p.stderr[-1000:])

    def test_every_live_boot_parses(self):
        """Every --boot NAME a test can ask (scenarios/live-*.A.txt with its .B.txt) through live.py start --dry-run:
        hookshot.py run's own parser + a parse of both scripts (pganon-cct-287-3: a broken boot is caught here)."""
        import glob
        import bt_plugin as B
        names = sorted(os.path.basename(f)[:-len(".A.txt")] for f in glob.glob(os.path.join(AUTOTEST, "scenarios",
                                                                                              "live-*.A.txt")))
        names = [n for n in names if os.path.isfile(os.path.join(AUTOTEST, "scenarios", n + ".B.txt"))]
        self.assertIn(B.DEFAULT_BOOT, names)
        for name in names:
            class Cfg:
                def getoption(self, opt, name=name):
                    return {"env": [], "session_minutes": 60, "boot": name}[opt]
            args = B.boot_args(Cfg(), 1, os.path.join(self.tmp, "coop_server.exe"), os.path.join(self.tmp, "s"))
            i = args.index("--lane")
            args[i:i + 2] = self.games()
            with self.subTest(boot=name):
                out = self.live(*(args + ["--dry-run"]))
                self.assertTrue(out["ok"], out)

    def test_real_boot_args_parse(self):
        """bt_plugin.boot_args (what lane_pair boots with) through live.py start --dry-run: hookshot.py run's own
        parser + a parse of both boot scripts. The lane pair is swapped for two folders (CI has no lanes)."""
        import bt_plugin as B

        class Cfg:
            def getoption(self, name):
                return {"env": ["LICHII_COOP_PARTY_HUD_PROBE=1"], "session_minutes": 175, "boot": B.DEFAULT_BOOT}[name]
        args = B.boot_args(Cfg(), 1, os.path.join(self.tmp, "coop_server.exe"), os.path.join(self.tmp, "s"))
        i = args.index("--lane")
        args[i:i + 2] = self.games()
        out = self.live(*(args + ["--dry-run"]))
        self.assertTrue(out["ok"], out)
        for want in ("--script-a", B.BOOT_A, "--script-b", B.BOOT_B, "--no-load", "LICHII_COOP_PARTY_HUD_PROBE=1"):
            self.assertIn(want, out["runner"])

    def test_nocs_switch_boots_b_from_lane_b_nocs(self):
        """--env B:COOP_TEST_NO_CHARACTER_SELECT=1 (no_cs.py's switch): game B boots from lane<N>-B-nocs (--game-b,
        which live.py resolve takes over the lane's B); without it, no --game-b."""
        import bt_plugin as B
        for env, nocs in (([], False), ([B.NOCS_ENV], True), (["COOP_TEST_NO_CHARACTER_SELECT=1"], True),
                          (["A:COOP_TEST_NO_CHARACTER_SELECT=1"], False)):
            class Cfg:
                def getoption(self, name, env=env):
                    return {"env": env, "session_minutes": 60, "boot": B.DEFAULT_BOOT, "kit_ls": self_kit}[name]
            self_kit = os.path.join(self.tmp, "coop-kit", "ls")
            args = B.boot_args(Cfg(), 2, "srv", "s")
            with self.subTest(env=env):
                self.assertEqual(B.nocs_wanted(Cfg()), nocs)
                if nocs:
                    self.assertEqual(args[args.index("--game-b") + 1], os.path.join(self_kit, "lane2-B-nocs"))
                else:
                    self.assertNotIn("--game-b", args)


HOOK_TESTS = 'import pytest\n\ndef test_hooks(pair):\n    pair.act("A", "frob 3")\n    pair.expect_log("A", r"fake hook: frob 3 on A", max_frames=30)\n    st = pair.state("B")\n    assert any(a.get("name") == "HookActor_B" for a in st["world"]["actors"]), st["world"]\n    assert st.get("hooked") == "shared-1"\n    r = pair.call("A", "ping_hook")\n    assert r.get("pong") is True\n    pair.press("A", ["ZR"], frames=2)\n    pair.expect_log("A", r"fake hook: pressed ZR", max_frames=30)\n\n    import playtest_client as pc\n    with pytest.raises(pc.BridgeError):           # (a plain warp: the boss intro event makes it a timeout)\n        pair.warp_both("BossStage_Test")\n    st = pair.warp_both("BossStage_Test", arrive_in_event=True)\n    assert st["A"]["scene"] == st["B"]["scene"] == "BossStage_Test"\n    assert any("answered timeout in the arrival event" in n for n in pair.notes), pair.notes\n    pair.act("A", "place 0 0 0")\n    pair.call("A", "move", yaw=90, strength=1.0, frames=30)\n    assert pair.state("A")["pos"][0] == 1.0, pair.state("A")["pos"]      # (move_step: the wall at x 1.0)\n    with pytest.raises(pc.BridgeError):           # (a timeout with NO event is not an arrival: review-c123-8)\n        pair.warp_both("NoEventStage_Test", arrive_in_event=True)\n    a_scene, a_frame = pair.state("A")["scene"], pair.state("A")["frame"]\n    st = pair.warp_one("B", "KakarikoVillage_Child")      # (one game; the other stepped the same frames)\n    assert st["scene"] == "KakarikoVillage_Child" and pair.state("A")["scene"] == a_scene\n    assert pair.state("A")["frame"] > a_frame              # (A ran the frames B\'s warp took)\n'
HOOK_MODULE = 'def setup(game):\n    game.hook_seen = []\n\ndef lane(fl):\n    fl.g["A"].shared = fl.g["B"].shared = {"n": 1}\n\ndef command(game, m):\n    if m.get("cmd") == "ping_hook":\n        return {"ok": True, "pong": True}\n    if m.get("cmd") == "warp" and m.get("scene") in ("BossStage_Test", "NoEventStage_Test"):\n        game.run(40)\n        game.scene = m["scene"]                 # (there, but the bridge says timeout: an intro event, or not)\n        return {"ok": False, "error": "timeout", "message": "still in an event"}\n    return None\n\ndef move_step(game, pos):\n    if game.scene == "BossStage_Test" and pos[0] > 1.0:\n        return [1.0, pos[1], pos[2]]\n    return None\n\ndef after(game, m, r):\n    if m.get("cmd") == "state":\n        r["hooked"] = "shared-%d" % game.shared["n"]\n\ndef press(game, buttons):\n    if "ZR" in buttons:\n        game.say("fake hook: pressed ZR")\n        return True\n    return False\n\ndef act(game, words):\n    if words[0] == "frob":\n        game.say("fake hook: frob %s on %s" % (words[1], game.tag))\n        return True\n    return False\n\ndef actors(game):\n    return [{"id": 99, "name": "HookActor_%s" % game.tag, "kind": "enemy", "pos": [0.0, 0.0, 0.0]}]\n\ndef state(game, st):\n    if game.scene == "BossStage_Test":\n        st["event"] = True                      # (its intro event runs)\n'


PACK_MAKER = """import os

def make(out, ctx):
    mode = os.environ.get("BT_PACK_MODE", "ok")
    if mode == "boom":
        raise RuntimeError("no such texture")
    for n, t in (("map-arrow.png", "new arrow"), ("heart-0.png", "new heart")):
        with open(os.path.join(out, n), "w") as f:
            f.write(t)
    ctx.say("selftest pack made in %s (lane %d)" % (out, ctx.lane))
    rel = "../escape.png" if mode == "badrel" else "coop_hud/map-arrow.png"
    return {rel: os.path.join(out, "map-arrow.png"), "coop_hud/heart-0.png": os.path.join(out, "heart-0.png")}
"""

PACK_TESTS = """import os, pytest

def _hud(pair, x):
    return os.path.join(pair.games[x]["folder"], "mods", "contents", "0400486024DEC000", "coop-puppet", "coop_hud")

@pytest.mark.needs_pack("sel")
def test_pack_is_in(pair):
    for x in ("A", "B"):
        assert open(os.path.join(_hud(pair, x), "map-arrow.png")).read() == "new arrow"
        assert open(os.path.join(_hud(pair, x), "heart-0.png")).read() == "new heart"
        assert open(os.path.join(_hud(pair, x), "font-regular.otf")).read() == "font"   # (untouched)

def test_plain(pair):
    pass
"""


SAVE_TESTS = '''import os
import pytest
import save_flags as SF

def flag(x):
    p = os.path.join(os.environ["BT_T_OUT"], "lane1", "fake", "lane1-" + x, "autotest-save", "slot_00", "progress.sav")
    return SF.get(SF.read_file(p), 0, 0x6b093b51)

@pytest.mark.needs_save("boss")
def test_save_is_in(pair):
    assert flag("A") == 0 and flag("B") == 0

def test_plain(pair):
    pass
'''
SAVE_PROFILE = 'FLAGS = ["bool:0x6b093b51=0", "enum:0x7d776b61=0xb5df43cb"]  # (Gohma_DefeatBoss, quest step)\n'


class SaveProfile(unittest.TestCase):
    """--save NAME (testsave.py): FLAGS set in BOTH games' autotest-save before the boot, the baseline back after; a
    profile left by a run that died is undone at the next boot; needs_save deselects."""

    def setUp(self):
        sys.path.insert(0, HERE)
        from test_save_flags import make_save
        self.tmp = tempfile.mkdtemp()
        self.dir, self.out = os.path.join(self.tmp, "t"), os.path.join(self.tmp, "o")
        self.saves, self.tpl = os.path.join(self.tmp, "saves"), os.path.join(self.tmp, "tpl")
        self.base = bytes(make_save({0: [(0x6b093b51, 1)], 6: [(0x7d776b61, 0x11111111)]}))
        for d in (self.dir, self.saves, os.path.join(self.tpl, "slot_00"), os.path.join(self.tpl, "slot_01")):
            os.makedirs(d)
        for s in ("slot_00", "slot_01"):
            with open(os.path.join(self.tpl, s, "progress.sav"), "wb") as f:
                f.write(self.base)
        with open(os.path.join(self.dir, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n"
                    % (INGAME, os.path.dirname(INGAME)))
        with open(os.path.join(self.dir, "test_save.py"), "w", encoding="utf-8") as f:
            f.write(SAVE_TESTS)
        with open(os.path.join(self.saves, "boss.py"), "w", encoding="utf-8") as f:
            f.write(SAVE_PROFILE)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def game(self, x):
        return os.path.join(self.out, "lane1", "fake", "lane1-%s" % x)

    def run_(self, extra=()):
        return pytest_run([".", "--fake", "--out", self.out, "-q", "-p", "no:cacheprovider"] + list(extra),
                          cwd=self.dir, env={"BT_SAVES_DIR": self.saves, "BT_FAKE_SAVE_DIR": self.tpl,
                                             "BT_T_OUT": self.out})

    def assert_as_before(self):
        import testsave
        for x in ("A", "B"):
            for s in ("slot_00", "slot_01"):
                with open(os.path.join(self.game(x), "autotest-save", s, "progress.sav"), "rb") as f:
                    self.assertEqual(f.read(), self.base, (x, s))
            self.assertFalse(os.path.exists(os.path.join(self.game(x), testsave.MANIFEST)), x)
            self.assertFalse(os.path.exists(os.path.join(self.game(x), testsave.KEPT)), x)

    def test_save_in_for_the_run_and_out_after(self):
        p = self.run_(["--save", "boss"])
        s = summary(self.out)
        self.assertEqual(results(s), {"test_save_is_in": "PASS", "test_plain": "PASS"}, p.stdout[-3000:] + p.stderr[-2000:])
        self.assertEqual(s["identity"]["save"]["name"], "boss")
        self.assertEqual(sorted(s["identity"]["save"]["sha256"]), ["lane1-A", "lane1-B"])
        self.assertEqual(len(s["identity"]["save"]["sha256"]["lane1-A"]), 2)
        self.assert_as_before()

    def test_without_save_the_test_is_deselected(self):
        p = self.run_()
        s = summary(self.out)
        self.assertEqual(results(s), {"test_plain": "PASS"}, p.stdout[-2000:])
        self.assertEqual([d.get("needs_save") for d in s["deselected"]], ["boss"])
        self.assertIn("deselected: needs_save boss", p.stdout)

    def test_a_flag_not_in_the_save_or_no_profile_is_setup_fail_lane_unchanged(self):
        with open(os.path.join(self.saves, "bad.py"), "w", encoding="utf-8") as f:
            f.write('FLAGS = ["bool:0x6b093b51=0", "bool:0x12345678=1"]\n')
        for name, why in (("bad", "not in this save"), ("none", "no ")):
            p = self.run_(["--save", name])
            s = summary(self.out)
            self.assertEqual(set(results(s).values()), {"SETUP-FAIL"}, p.stdout[-2000:])
            self.assertIn(why, " ".join(r["why"] for r in s["tests"]))
            self.assert_as_before()

    def test_a_dead_runs_profile_is_undone_at_the_next_boot(self):
        import testsave
        g = self.game("A")
        shutil.copytree(self.tpl, os.path.join(g, "autotest-save"))
        testsave.put_in([g], "boss", testsave.load("boss", self.saves), say=lambda m: None)
        self.assertNotEqual(open(os.path.join(g, "autotest-save", "slot_00", "progress.sav"), "rb").read(), self.base)
        self.run_()                                         # (no --save: still undone first)
        self.assert_as_before()
        testsave.undo(g)                                    # (again: nothing left, no error)


class PackFiles(unittest.TestCase):
    """--pack NAME (testpack.py): a maker's files go into BOTH games' co-op pack before the boot and come out after;
    a replaced file is put back; a pack left by a run that died is undone at the next boot; needs_pack deselects."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.dir, self.out = os.path.join(self.tmp, "t"), os.path.join(self.tmp, "o")
        self.packs, self.tpl = os.path.join(self.tmp, "packs"), os.path.join(self.tmp, "tpl")
        for d in (self.dir, self.packs, os.path.join(self.tpl, "coop_hud")):
            os.makedirs(d)
        with open(os.path.join(self.dir, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(self.dir, "test_pack.py"), "w", encoding="utf-8") as f:
            f.write(PACK_TESTS)
        with open(os.path.join(self.packs, "sel.py"), "w", encoding="utf-8") as f:
            f.write(PACK_MAKER)
        for n, t in (("heart-0.png", "old heart"), ("font-regular.otf", "font")):
            with open(os.path.join(self.tpl, "coop_hud", n), "w") as f:
                f.write(t)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def hud(self, x):
        return os.path.join(self.out, "lane1", "fake", "lane1-%s" % x, P.__dict__.get("PACK_REL", "") or
                            os.path.join("mods", "contents", "0400486024DEC000", "coop-puppet"), "coop_hud")

    def run_(self, extra=(), mode="ok"):
        return pytest_run([".", "--fake", "--out", self.out, "-q", "-p", "no:cacheprovider"] + list(extra),
                          cwd=self.dir, env={"BT_PACKS_DIR": self.packs, "BT_FAKE_PACK_DIR": self.tpl,
                                             "BT_PACK_MODE": mode})

    def assert_as_before(self):
        for x in ("A", "B"):
            g = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(self.hud(x))))))
            self.assertEqual(open(os.path.join(self.hud(x), "heart-0.png")).read(), "old heart", x)
            self.assertFalse(os.path.exists(os.path.join(self.hud(x), "map-arrow.png")), x)
            self.assertFalse(os.path.exists(os.path.join(g, "coop-test-pack.json")), x)
            self.assertFalse(os.path.exists(os.path.join(g, "coop-test-pack-kept")), x)

    def test_undo_removes_only_the_folders_it_made(self):
        """review-c123-9 11:36 nit b: the new folders put_in made go again once empty; a folder that was there
        before stays, even when empty."""
        import testpack
        g = os.path.join(self.tmp, "g")
        pack = os.path.join(g, testpack.PACK_REL)
        os.makedirs(os.path.join(pack, "was_empty"))
        src = os.path.join(self.tmp, "f.bin")
        with open(src, "w") as f:
            f.write("x")
        testpack.put_in([g], {"p": {"new/sub/a.bin": src, "was_empty/b.bin": src}}, say=lambda m: None)
        self.assertTrue(os.path.isfile(os.path.join(pack, "new", "sub", "a.bin")))
        testpack.undo(g, say=lambda m: None)
        self.assertFalse(os.path.exists(os.path.join(pack, "new")))
        self.assertTrue(os.path.isdir(os.path.join(pack, "was_empty")))

    def test_pack_rel_is_hs_pack_dir(self):
        import testpack
        sys.path.insert(0, os.path.join(os.path.dirname(HERE), "..", "hookshot"))
        import hs
        self.assertEqual(testpack.PACK_REL, hs.PACK_DIR)

    def test_pack_in_for_the_run_and_out_after(self):
        p = self.run_(["--pack", "sel"])
        s = summary(self.out)
        self.assertEqual(results(s), {"test_pack_is_in": "PASS", "test_plain": "PASS"}, p.stdout[-3000:] + p.stderr[-2000:])
        import hashlib
        self.assertEqual(s["identity"]["pack"], {"sel": {
            "coop_hud/heart-0.png": hashlib.sha256(b"new heart").hexdigest(),
            "coop_hud/map-arrow.png": hashlib.sha256(b"new arrow").hexdigest()}})
        self.assertIsNone(s["pack_out_errors"])
        self.assert_as_before()

    def test_without_pack_the_test_is_deselected(self):
        p = self.run_()
        s = summary(self.out)
        self.assertEqual(results(s), {"test_plain": "PASS"})
        self.assertEqual([d["needs_pack"] for d in s["deselected"]], [["sel"]])
        self.assertIn("deselected: needs_pack sel", p.stdout)
        self.assertEqual(s["identity"]["pack"], {})

    def test_a_pack_left_by_a_dead_run_is_undone_at_the_next_boot(self):
        import testpack
        g = {x: os.path.join(self.out, "lane1", "fake", "lane1-%s" % x) for x in ("A", "B")}
        made = {"sel": {"coop_hud/map-arrow.png": os.path.join(self.tmp, "arrow"),
                        "coop_hud/heart-0.png": os.path.join(self.tmp, "heart")}}
        for f, t in (("arrow", "new arrow"), ("heart", "new heart")):
            with open(os.path.join(self.tmp, f), "w") as fh:
                fh.write(t)
        for x in g:
            shutil.copytree(self.tpl, os.path.join(g[x], testpack.PACK_REL))
        testpack.put_in(list(g.values()), made, say=lambda m: None)      # (a run that died here: never taken out)
        self.assertEqual(open(os.path.join(self.hud("A"), "heart-0.png")).read(), "new heart")
        p = self.run_()                                                  # (the next run, no --pack)
        self.assertEqual(results(summary(self.out)), {"test_plain": "PASS"}, p.stdout[-2000:])
        self.assert_as_before()

    def test_a_failing_maker_is_setup_fail_and_leaves_the_lane_as_it_was(self):
        for mode, word in (("boom", "make failed: RuntimeError: no such texture"), ("badrel", "not a path under")):
            shutil.rmtree(self.out, ignore_errors=True)
            p = self.run_(["--pack", "sel"], mode=mode)
            s = summary(self.out)
            self.assertEqual(set(results(s).values()), {"SETUP-FAIL"}, (mode, p.stdout[-2000:]))
            self.assertIn(word, s["tests"][0]["why"], mode)
            self.assert_as_before()

    def test_unknown_pack_is_setup_fail(self):
        p = self.run_(["--pack", "nope"])
        s = summary(self.out)
        self.assertEqual(set(results(s).values()), {"SETUP-FAIL"}, p.stdout[-2000:])
        self.assertIn("--pack nope: no ", s["tests"][0]["why"])

    def test_pack_check_catches_a_changed_file(self):
        import testpack
        g = os.path.join(self.tmp, "g")
        shutil.copytree(self.tpl, os.path.join(g, testpack.PACK_REL))
        src = os.path.join(self.tmp, "src.png")
        with open(src, "w") as f:
            f.write("x")
        real_copy = shutil.copyfile
        try:
            shutil.copyfile = lambda a, b: open(b, "w").write("not x")   # (a copy that went wrong)
            with self.assertRaises(testpack.PackError):
                testpack.put_in([g], {"sel": {"coop_hud/x.png": src}}, say=lambda m: None)
        finally:
            shutil.copyfile = real_copy
        self.assertTrue(testpack.undo(g, say=lambda m: None))
        self.assertFalse(os.path.exists(os.path.join(g, testpack.PACK_REL, "coop_hud", "x.png")))


class FakeHooks(unittest.TestCase):
    """A row port's fake reactions live in ingame/fakes/<name>.py (fakegame.py hooks), never in fakegame.py itself."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        d, f = os.path.join(self.tmp, "t"), os.path.join(self.tmp, "fakes")
        os.makedirs(d)
        os.makedirs(f)
        with open(os.path.join(d, "conftest.py"), "w", encoding="utf-8") as fh:
            fh.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(d, "test_hooks.py"), "w", encoding="utf-8") as fh:
            fh.write(HOOK_TESTS)
        with open(os.path.join(f, "myrow.py"), "w", encoding="utf-8") as fh:
            fh.write(HOOK_MODULE)
        with open(os.path.join(f, "_helper.py"), "w", encoding="utf-8") as fh:
            fh.write("raise RuntimeError('a _ module is not a hook module')\n")
        self.dir, self.fakes = d, f

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_every_hook(self):
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider"], cwd=self.dir,
                       env={"BT_FAKE_FAKES_DIR": self.fakes})
        self.assertEqual(results(summary(out)), {"test_hooks": "PASS"}, p.stdout[-2000:])

    def test_without_the_module_the_test_fails(self):
        os.remove(os.path.join(self.fakes, "myrow.py"))
        out = os.path.join(self.tmp, "o")
        pytest_run([".", "--fake", "--no-rerun", "--out", out, "-q", "-p", "no:cacheprovider"], cwd=self.dir,
                   env={"BT_FAKE_FAKES_DIR": self.fakes})
        self.assertNotEqual(results(summary(out))["test_hooks"], "PASS")

    def test_repo_fakes_folder_loads(self):
        sys.path.insert(0, INGAME)
        import fakegame
        for m in fakegame.load_fakes():          # (every port's module imports without error)
            self.assertTrue(m.__name__.startswith("fakes_"))


class ReplyWait(unittest.TestCase):
    """boss-respawn-bridge-e3e6e253 CAUSE 2: a call sent with the 0.2 s left of a spent deadline timed the socket out,
    the client closed it, the game ran free mid-arrival and every later recovery SETUP-FAILed ('teleport still in
    progress'). Now: never sent short (TestTimeout naming it, nothing sent), a long step gets its frames' wait, a lost
    reply re-holds the game at once, and a warp met by Longshot's 8 s teleport gap waits it out and asks once more."""

    class Clock:
        t = 0.0

        def __call__(self):
            return self.t

    class Scripted:
        """A client: warps answer from `warps` in turn; state says KokiriForest_Child in play; step advances the clock."""

        def __init__(self, clock, warps=()):
            self.clock, self.warps, self.calls, self.last_frame = clock, list(warps), [], 100

        def request(self, cmd, reply_timeout=None, check=True, **args):
            self.calls.append((cmd, reply_timeout, args))
            if cmd == "step":
                self.clock.t += 0.5
                self.last_frame += int(args.get("frames", 1))
                return {"ok": True, "frame": self.last_frame}
            if cmd == "warp":
                r = self.warps.pop(0)
                if check and not r.get("ok"):
                    raise P.pc.BridgeError(r)
                return r
            return {"ok": True, "frame": self.last_frame, "scene": "KokiriForest_Child", "loading": False,
                    "event": False, "not_ready": None}

    def make(self, budget, warps=()):
        clock = self.Clock()
        pair = P.Pair.__new__(P.Pair)
        pair.c = {"A": self.Scripted(clock, warps)}
        pair.log, pair.shots, pair.notes = {}, [], []
        pair.deadline = P.Deadline(budget, clock=clock)
        return pair, clock

    def test_a_call_is_never_sent_short(self):
        pair, clock = self.make(1.0)
        with self.assertRaisesRegex(P.TestTimeout, r"state not sent \(1\.0 s left, its reply needs 2\.0 s; the game "
                                                   r"stays held\)"):
            pair.state("A")
        self.assertEqual(pair.c["A"].calls, [])            # (nothing went out: the connection and the hold are kept)

    def test_a_long_step_gets_its_frames_wait(self):
        pair, clock = self.make(300)
        pair.call("A", "step", frames=1200)
        wait = pair.c["A"].calls[0][1]
        self.assertGreaterEqual(wait, P.REPLY_MIN_S + 1200 / P.CLIENT_FPS)   # 62 s (was capped at 60: a step of 1200
        self.assertLessEqual(wait, 300)                                        # frames at 20 fps timed out)

    def test_a_step_its_deadline_cannot_carry_is_not_sent(self):
        pair, clock = self.make(30)
        with self.assertRaisesRegex(P.TestTimeout, r"step not sent"):
            pair.call("A", "step", frames=1200)
        self.assertEqual(pair.c["A"].calls, [])

    def test_a_warp_is_never_sent_short_of_its_own_timeout(self):
        pair, clock = self.make(2.5, warps=[{"ok": True}])
        with self.assertRaisesRegex(P.TestTimeout, r"warp not sent"):
            pair.warp_one("A", "KakarikoVillage_Child")
        self.assertEqual(pair.c["A"].calls, [])

    def test_a_teleport_gap_is_waited_out_and_asked_once_more(self):
        busy = {"ok": False, "error": "failed", "message": "Longshot's teleport: another teleport is still in progress"}
        pair, clock = self.make(120, warps=[busy, {"ok": True, "arrived": True}])
        pair.warp_one("A", "KakarikoVillage_Child")
        self.assertEqual([c for c, _, _ in pair.c["A"].calls].count("warp"), 2)
        self.assertGreaterEqual(clock.t, P.Pair.TELEPORT_GAP_S)
        self.assertTrue(any("the gap waited out" in n for n in pair.notes), pair.notes)

    def test_a_second_teleport_gap_raises(self):
        busy = {"ok": False, "error": "failed", "message": "another teleport is still in progress"}
        pair, clock = self.make(120, warps=[busy, busy])
        with self.assertRaisesRegex(P.pc.BridgeError, r"still in progress"):
            pair.warp_one("A", "KakarikoVillage_Child")

    def test_a_lost_reply_holds_the_game_again(self):
        """A real fake lane: the reply of one state is lost (socket timeout), the client closes, the fake game runs
        free; Pair re-holds it at once (a reconnect holds), noted, and the error goes on."""
        import socket
        import fakegame
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        lane = fakegame.FakeLane(tmp)
        self.addCleanup(lane.close)
        pair = P.Pair(lane.games(), out_dir=tmp)
        self.addCleanup(pair.close)
        pair.begin(60)
        pair.state("A")
        self.assertTrue(lane.g["A"].paused)
        client = pair.c["A"]
        real = client._send
        def lost(cmd, wait, args):
            if cmd == "state":
                raise socket.timeout("timed out")
            return real(cmd, wait, args)
        client._send = lost
        with self.assertRaises(OSError):
            pair.state("A")
        client._send = real
        self.assertTrue(any("A: held again after state: TimeoutError" in n for n in pair.notes), pair.notes)
        self.assertTrue(client.held)
        self.assertTrue(lane.g["A"].paused)


class OcarinaReady(unittest.TestCase):
    """Pair.need_ocarina (songs-frogs-733c4d8f SETUP-FAIL, 11-26): a D-pad press before the module's ocarina re-equip
    after a load takes nothing out; every ocarina test waits for that line first."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def lane(self, late):
        import fakegame
        from unittest import mock
        with mock.patch.dict(os.environ, {"BT_FAKE_OCARINA_LATE": str(late)}):
            lane = fakegame.FakeLane(self.tmp)
        self.addCleanup(lane.close)
        pair = P.Pair(lane.games(), out_dir=self.tmp)
        self.addCleanup(pair.close)
        pair.begin(60)
        return pair

    def test_a_press_before_the_re_equip_takes_nothing_out(self):
        pair = self.lane(200)
        self.assertFalse(pair.ocarina_ready("B"))
        pair.press("B", ["LEFT"], frames=12)
        pair.step_both(40)
        self.assertIsNone(pair.log["B"].find(r"ocarina probe: our Link plays the ocarina", pos=0))  # (the 733c4d8f fail)

    def test_need_ocarina_steps_to_the_re_equip(self):
        pair = self.lane(200)
        pair.need_ocarina("B")
        self.assertTrue(pair.ocarina_ready("B"))
        pair.press("B", ["LEFT"], frames=12)
        pair.step_both(40)
        self.assertTrue(pair.log["B"].find(r"ocarina probe: our Link plays the ocarina", pos=0))

    def test_a_warp_needs_a_new_re_equip(self):
        pair = self.lane(200)
        pair.need_ocarina("B")
        pair.warp_one("B", "ZoraRiver_Child")
        self.assertFalse(pair.ocarina_ready("B"))     # (the equip line before the load no longer counts)
        pair.need_ocarina("B")
        self.assertTrue(pair.ocarina_ready("B"))

    def test_no_re_equip_names_it(self):
        pair = self.lane(5000)
        with self.assertRaisesRegex(AssertionError, r"B: the ocarina back on the D-pad after the load.*not after 600"):
            pair.need_ocarina("B")

    def test_the_ocarina_tests_pass_on_a_late_re_equip(self):
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--no-rerun", "--out", out, "-q", "-p", "no:cacheprovider",
                        "-k", "ocarina or frog or epona_seen"], env={"BT_FAKE_OCARINA_LATE": "200"})
        r = results(summary(out))
        self.assertTrue(r, p.stdout[-2000:])
        for name, res in r.items():
            self.assertIn(res, ("PASS", "NO-REF", "SKIP"), (name, res, p.stdout[-2000:]))


class SetupLimit(unittest.TestCase):
    """Pair.setup's 30 s limit holds for the calls inside it (review-c123-6 point 2): a hung bridge in a set-up is a
    SETUP-FAIL at the limit, not a FAIL at the test's budget. A fake clock: no real waiting."""

    class Clock:
        t = 0.0

        def __call__(self):
            return self.t

    class HungClient:
        def __init__(self, clock):
            self.clock, self.waits, self.last_frame = clock, [], 0

        def request(self, cmd, reply_timeout=None, **args):
            self.waits.append(reply_timeout)
            self.clock.t += reply_timeout       # waits its whole reply time, then no answer
            raise TimeoutError("no reply to %s in %.1f s" % (cmd, reply_timeout))

        def close(self):
            pass

    def make(self, budget):
        clock = self.Clock()
        pair = P.Pair.__new__(P.Pair)
        pair.c = {"A": self.HungClient(clock)}
        pair.log, pair.shots, pair.notes = {}, [], []
        pair.deadline = P.Deadline(budget, clock=clock)
        return pair, clock

    def test_hung_call_stops_at_the_limit(self):
        pair, clock = self.make(240)
        with self.assertRaises(P.SetupFail) as cm:
            with pair.setup():
                pair.state("A")
        self.assertLessEqual(pair.c["A"].waits[0], P.SETUP_LIMIT_S)
        # (+ the re-hold after the lost reply, at most REPLY_MIN_S: no game runs free; on a hung bridge it fails too)
        self.assertLessEqual(clock.t, P.SETUP_LIMIT_S + P.REPLY_MIN_S)
        self.assertEqual(pair.c["A"].waits[1:], [P.REPLY_MIN_S])
        self.assertTrue(any("NOT held again" in n for n in pair.notes), pair.notes)
        self.assertIn("TimeoutError", str(cm.exception))
        self.assertEqual(pair.deadline.left(), 240 - clock.t)      # the test's own deadline is back after it

    def test_many_slow_calls_stop_at_the_limit(self):
        pair, clock = self.make(240)
        def slow(cmd, reply_timeout=None, **a):
            clock.t += 7.0
            return {"ok": True}
        pair.c["A"].request = slow
        with self.assertRaises(P.SetupFail) as cm:
            with pair.setup():
                while True:
                    pair.state("A")
        self.assertIn("timeout after 30 s", str(cm.exception))
        self.assertLessEqual(clock.t, P.SETUP_LIMIT_S + 7.0)

    def test_hung_warp_stops_at_the_limit(self):
        """warp_both's own bridge call (not Pair.call) keeps its reply wait inside the deadline (review-c123-8)."""
        pair, clock = self.make(240)
        with self.assertRaises(P.SetupFail):
            with pair.setup():
                pair.warp_both("PhantomGanonStage", arrive_in_event=True)
        self.assertLessEqual(clock.t, P.SETUP_LIMIT_S + P.REPLY_MIN_S)      # (+ the re-hold, as above)
        self.assertTrue(all(w <= P.SETUP_LIMIT_S for w in pair.c["A"].waits), pair.c["A"].waits)

    def test_short_test_budget_wins(self):
        pair, clock = self.make(10)
        with self.assertRaises(P.SetupFail):
            with pair.setup():
                pair.state("A")
        self.assertLessEqual(pair.c["A"].waits[0], 10)


class RecoverOnState(unittest.TestCase):
    """The lane's recovery (Pair.recover) waits on game state, bounded per warp it needs: a warp takes the game's wall
    time (party-hud-bridge-6947aa3d: warp_both 26.9-31.0 s, 3 recoveries SETUP-FAIL at the old fixed 30 s). Fake
    clock and a fake bridge whose warp costs WARP_S of wall time: no real waiting."""

    class Clock:
        t = 0.0

        def __call__(self):
            return self.t

    class SlowWarp:
        """One game: in `scene`, in play. warp costs `warp_s` s of the clock and `warp_frames` frames; `hang` = the
        warp never answers (waits its whole reply time). load_frames > 0: the warp answers 'timeout' (an
        arrive_in_event warp) with the game still loading for that many frames, then in the scene in an event."""

        def __init__(self, clock, scene, warp_s, warp_frames=150, hang=False, load_frames=0):
            self.clock, self.scene, self.warp_s, self.hang = clock, scene, warp_s, hang
            self.warp_frames, self.load_frames = warp_frames, load_frames
            self.frame, self.last_frame, self.loading_until, self.event = 0, 0, 0, False
            self.waits = []

        def st(self):
            return {"scene": self.scene, "loading": self.frame < self.loading_until, "event": self.event,
                    "not_ready": None}

        def request(self, cmd, reply_timeout=None, check=True, **a):
            self.waits.append(reply_timeout)
            if cmd == "warp":
                if self.hang:
                    self.clock.t += reply_timeout
                    raise TimeoutError("timed out")
                self.clock.t += min(self.warp_s, a["timeout"])
                self.frame += self.warp_frames
                self.last_frame = self.frame
                self.scene = a["scene"]
                if self.load_frames:
                    self.loading_until, self.event = self.frame + self.load_frames, True
                    return {"ok": False, "error": "timeout", "message": "held", "frame": self.frame}
                return {"ok": True, "arrived": True, "frame": self.frame, "state": self.st()}
            if cmd == "step":
                self.frame += a["frames"]
                self.clock.t += 0.01 * a["frames"]
            self.last_frame = self.frame
            return dict(self.st(), ok=True, frame=self.frame)

        def close(self):
            pass

    BOOT = "KakarikoVillage_Child"
    WARP_S = 15.5        # one game's warp: the worst measured warp_both (31.0 s, test_warp r3) over two games

    def make(self, budget, **games):
        clock = self.Clock()
        pair = P.Pair.__new__(P.Pair)
        pair.c = {x: self.SlowWarp(clock, **kw) for x, kw in games.items()}
        pair.log, pair.shots, pair.notes = {}, [], []
        pair.deadline = P.Deadline(budget, clock=clock)
        return pair, clock

    def test_two_measured_warps_recover(self):
        """Both games in the save's scene (after a title test): two 15.5 s warps = 31 s, over the old 30 s limit."""
        import bt_plugin
        pair, clock = self.make(bt_plugin.RECOVER_S + 5, A=dict(scene="Graveyard_Child", warp_s=self.WARP_S),
                                B=dict(scene="Graveyard_Child", warp_s=self.WARP_S))
        st = pair.recover(self.BOOT)
        self.assertTrue(all(s["scene"] == self.BOOT for s in st.values()), st)
        self.assertGreater(clock.t, P.SETUP_LIMIT_S)         # (the run that failed 3/3: more than 30 s, and it passes)
        self.assertTrue(any("2 warps" in n for n in pair.notes), pair.notes)
        self.assertEqual(pair.c["A"].frame, pair.c["B"].frame)   # (the other game is stepped to the same frames)

    def test_twice_the_measured_warp_still_recovers(self):
        """The margin: one game's warp as long as the worst two-game warp measured (31 s each)."""
        import bt_plugin
        pair, clock = self.make(bt_plugin.RECOVER_S + 5, A=dict(scene="Graveyard_Child", warp_s=31.0),
                                B=dict(scene="Graveyard_Child", warp_s=31.0))
        pair.recover(self.BOOT)
        self.assertLessEqual(clock.t, bt_plugin.RECOVER_S)

    def test_a_hung_warp_is_setup_fail_inside_the_bound(self):
        import bt_plugin
        pair, clock = self.make(bt_plugin.RECOVER_S + 5, A=dict(scene="Graveyard_Child", warp_s=0, hang=True),
                                B=dict(scene=self.BOOT, warp_s=0))
        with self.assertRaisesRegex(P.SetupFail, r"^boot: TimeoutError"):
            pair.recover(self.BOOT)
        self.assertLessEqual(clock.t, P.RECOVER_BASE_S + P.RECOVER_WARP_S)   # (one game away: one warp's bound)
        self.assertTrue(all(w <= P.RECOVER_BASE_S + P.RECOVER_WARP_S for w in pair.c["A"].waits), pair.c["A"].waits)
        self.assertEqual(pair.deadline.seconds, bt_plugin.RECOVER_S + 5)    # (the outer deadline is back)

    def test_nothing_away_is_bounded_like_a_set_up(self):
        pair, clock = self.make(240, A=dict(scene=self.BOOT, warp_s=0, hang=True), B=dict(scene=self.BOOT, warp_s=0))
        pair.recover(self.BOOT)                               # (no warp asked: the hung warp is never called)
        self.assertTrue(any("bound 30 s, 0 warps" in n for n in pair.notes), pair.notes)

    def test_recover_s_covers_both_warps(self):
        import bt_plugin
        self.assertGreaterEqual(bt_plugin.RECOVER_S, P.RECOVER_BASE_S + 2 * P.WARP_PAIR_MEASURED_S)
        self.assertLessEqual(bt_plugin.RECOVER_S, bt_plugin.BOOT_READY_S)  # (never more than the boot proof's own)

    def test_arrive_in_event_still_loading_is_stepped_to_the_arrival(self):
        """The bridge's ARRIVE_IN_EVENT_WAIT is over while the game still loads (a warp is longer than 12 s under
        load): stepped until in the scene, not loading, in the event; was one read, raised at once."""
        pair, clock = self.make(120, A=dict(scene=self.BOOT, warp_s=12, load_frames=200),
                                B=dict(scene=self.BOOT, warp_s=12, load_frames=200))
        st = pair.warp_both("PhantomGanonStage", arrive_in_event=True)
        self.assertTrue(all(s["scene"] == "PhantomGanonStage" and s["event"] and not s["loading"]
                            for s in st.values()), st)
        self.assertEqual(pair.c["A"].frame, pair.c["B"].frame)

    def test_arrive_in_event_never_arriving_raises(self):
        pair, clock = self.make(600, A=dict(scene=self.BOOT, warp_s=12, load_frames=10 ** 6),
                                B=dict(scene=self.BOOT, warp_s=12))
        with self.assertRaisesRegex(P.pc.BridgeError, r"^timeout: held; then \d+ frames: A: scene=PhantomGanonStage"):
            pair.warp_one("A", "PhantomGanonStage", arrive_in_event=True)


class LaneCheck(unittest.TestCase):
    def make(self, root, x, dll=b"module", patches="patches=abc", server=None, **over):
        d = os.path.join(root, "lane1-%s" % x)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "hookshot_coop.dll"), "wb") as f:
            f.write(dll)
        with open(os.path.join(d, "coop-built-from.txt"), "w", encoding="utf-8") as f:
            f.write("commit=1\n%s\n" % patches)
        fields = {"server": server, "dll_sha256": hashlib.sha256(dll).hexdigest().upper(), "commit": "ae97cc772",
                  "patches_line_sha256": hashlib.sha256(patches.encode()).hexdigest().upper()}
        fields.update(over)
        with open(os.path.join(d, "lane-server.txt"), "w", encoding="utf-8") as f:
            f.write("".join("%s=%s\n" % kv for kv in fields.items()))

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.server = os.path.join(self.tmp, "coop_server.exe")
        open(self.server, "w").close()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_good_lane(self):
        for x in "AB":
            self.make(self.tmp, x, server=self.server)
        self.assertEqual(P.lane_check(self.tmp, 1)["commit"], "ae97cc772")

    def test_module_changed_after_install(self):
        for x in "AB":
            self.make(self.tmp, x, server=self.server)
        with open(os.path.join(self.tmp, "lane1-B", "hookshot_coop.dll"), "wb") as f:
            f.write(b"other")
        with self.assertRaisesRegex(P.SetupFail, "lane1-B hookshot_coop.dll .* is not lane-server.txt's"):
            P.lane_check(self.tmp, 1)

    def test_game_rebuilt_after_install(self):
        self.make(self.tmp, "A", server=self.server)
        self.make(self.tmp, "B", server=self.server, patches="patches=new",
                  patches_line_sha256=hashlib.sha256(b"patches=abc").hexdigest().upper())
        with self.assertRaisesRegex(P.SetupFail, "game patches line"):
            P.lane_check(self.tmp, 1)

    def test_no_server(self):
        for x in "AB":
            self.make(self.tmp, x, server=os.path.join(self.tmp, "gone.exe"))
        with self.assertRaisesRegex(P.SetupFail, "no server"):
            P.lane_check(self.tmp, 1)

    def test_not_installed(self):
        with self.assertRaisesRegex(P.SetupFail, "not installed by lane-game-for"):
            P.lane_check(self.tmp, 1)

    # lane<N>-B-nocs (lanes.py nocs): game B without Character Select
    def nocs_lane(self, exefs="exefs=contents/0400486024DEC000/coop-puppet/exefs/E0.ips:aa;list=none", **over):
        for x in "AB":
            self.make(self.tmp, x, server=self.server)
        self.make(self.tmp, "B-nocs", server=self.server, nocs_of="lane1-B", **over)
        with open(os.path.join(self.tmp, "lane1-B-nocs", "coop-built-from.txt"), "a", encoding="utf-8") as f:
            f.write(exefs + "\n")

    def test_nocs_good(self):
        self.nocs_lane()
        info = P.lane_check(self.tmp, 1, nocs=True)
        self.assertEqual(os.path.basename(info["game_b"]), "lane1-B-nocs")
        self.assertIn("coop-puppet/exefs", info["game_b_exefs"])
        self.assertEqual(os.path.basename(P.lane_check(self.tmp, 1)["game_b"]), "lane1-B")   # (off by default)

    def test_nocs_missing_or_not_made_from_this_lane(self):
        for x in "AB":
            self.make(self.tmp, x, server=self.server)
        with self.assertRaisesRegex(P.SetupFail, "lane1-B-nocs.*not installed"):
            P.lane_check(self.tmp, 1, nocs=True)
        self.make(self.tmp, "B-nocs", server=self.server, nocs_of="lane2-B")
        with self.assertRaisesRegex(P.SetupFail, "nocs_of=lane2-B"):
            P.lane_check(self.tmp, 1, nocs=True)

    def test_nocs_other_module_commit_or_cs_code(self):
        self.nocs_lane(commit="0000000")
        with self.assertRaisesRegex(P.SetupFail, "commit .* is not lane1-B's"):
            P.lane_check(self.tmp, 1, nocs=True)
        shutil.rmtree(os.path.join(self.tmp, "lane1-B-nocs"))
        self.nocs_lane(exefs="exefs=OoT Character Select/exefs/E0.ips:bb;list=none")
        with self.assertRaisesRegex(P.SetupFail, "built with Character Select's code"):
            P.lane_check(self.tmp, 1, nocs=True)
        shutil.rmtree(os.path.join(self.tmp, "lane1-B-nocs"))
        self.nocs_lane()
        with open(os.path.join(self.tmp, "lane1-B-nocs", "exefs_patches.txt"), "w", encoding="utf-8") as f:
            f.write("# list\nOoT Character Select\tE0.ips\t27e24366\tcode\t\n")
        with self.assertRaisesRegex(P.SetupFail, "exefs_patches.txt still lists"):
            P.lane_check(self.tmp, 1, nocs=True)


class ImageCompare(unittest.TestCase):
    def setUp(self):
        from PIL import Image, ImageDraw
        self.tmp = tempfile.mkdtemp()
        self.paths = {}
        for name, box in (("a", (40, 40, 120, 100)), ("a2", (41, 40, 121, 100)), ("b", (200, 20, 300, 160))):
            im = Image.new("RGB", (640, 360), (30, 80, 30))
            ImageDraw.Draw(im).rectangle(box, fill=(220, 30, 30))
            self.paths[name] = os.path.join(self.tmp, name + ".png")
            im.save(self.paths[name])

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_same_close_and_different(self):
        p = self.paths
        self.assertTrue(imagecheck.compare(p["a"], p["a"], 0.99)["ok"])
        self.assertTrue(imagecheck.compare(p["a2"], p["a"], 0.90)["ok"])      # a pixel of animation
        r = imagecheck.compare(p["b"], p["a"], 0.90)
        self.assertFalse(r["ok"])
        self.assertIn("<", r["why"])

    def test_colour_fault_fails(self):
        """review-rows-c130 10:38 / B #131: a recoloured thing at the same grey value (green tunic (0,91,0) -> blue
        (0,60,160), both grey 53.4 by ITU-R 601) passes a grey SSIM but must FAIL the colour compare at the rows'
        0.80; the same shot passes; a hue-shifted copy of the whole picture fails."""
        from PIL import Image, ImageDraw
        def pic(name, link, bg=(30, 80, 30)):
            im = Image.new("RGB", (640, 360), bg)
            d = ImageDraw.Draw(im)
            d.rectangle((100, 60, 180, 220), fill=link)              # "Link's tunic"
            d.rectangle((400, 40, 600, 80), fill=(220, 30, 30))      # "hearts"
            path = os.path.join(self.tmp, name + ".png")
            im.save(path)
            return path
        green, blue = pic("green", (0, 91, 0)), pic("blue", (0, 60, 160))
        grey, _ = imagecheck.ssim(blue, green, colour=False)
        self.assertGreaterEqual(grey, 0.80)          # (why grey alone was blind: the stored ref would pass)
        r = imagecheck.compare(blue, green, 0.80)
        self.assertFalse(r["ok"], r)
        self.assertIn("colour SSIM", r["why"])
        self.assertTrue(imagecheck.compare(green, green, 0.99)["ok"])
        from PIL import Image as _I
        with _I.open(green) as im:
            r_, g_, b_ = im.split()
            _I.merge("RGB", (b_, r_, g_)).save(os.path.join(self.tmp, "hue.png"))   # (channels rotated: a hue shift)
        self.assertFalse(imagecheck.compare(os.path.join(self.tmp, "hue.png"), green, 0.80)["ok"])

    def test_no_reference_is_never_ok(self):
        keep = os.path.join(self.tmp, "keep")
        r = imagecheck.compare(self.paths["a"], os.path.join(self.tmp, "refs", "x-A.png"), 0.5, keep_dir=keep)
        self.assertFalse(r["ok"])
        self.assertTrue(r["why"].startswith("NO-REF"))
        self.assertTrue(os.path.exists(os.path.join(keep, "new", "x-A.png")))


def load_tests(loader, tests, pattern):
    """unittest (CI: python tools/autotest/tests/test_ingame.py) also runs every row port's own selftests,
    tests/test_ingame_<row>.py: a port adds its file and never edits this one (one owner per file)."""
    import glob
    import importlib.util
    for path in sorted(glob.glob(os.path.join(HERE, "test_ingame_*.py"))):
        name = os.path.splitext(os.path.basename(path))[0]
        spec = importlib.util.spec_from_file_location(name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        tests.addTests(loader.loadTestsFromModule(mod))
    return tests


class BootProof(unittest.TestCase):
    """NO REPEAT VOIDS after test-bridge-pilot-fa63e90f r1 (B left in Graveyard_Child in an event: every test after it
    failed) and party-hud-bridge r1 (test_boot read 'no Link sample has been taken yet' once): the lane's boot ends
    only with both games in play, no event, in the boot's scene (else SETUP-FAIL boot), and set-up checks step frames
    until ready instead of judging one read."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def lane(self, **env):
        import fakegame
        from unittest import mock
        with mock.patch.dict(os.environ, {k: str(v) for k, v in env.items()}):
            lane = fakegame.FakeLane(self.tmp)
        self.addCleanup(lane.close)
        pair = P.Pair(lane.games(), out_dir=self.tmp)
        self.addCleanup(pair.close)
        pair.begin(60)
        return pair

    def test_boot_scene_is_the_scripts_warp(self):
        import bt_plugin
        self.assertEqual(bt_plugin.boot_scene(), "KakarikoVillage_Child")
        self.assertEqual(bt_plugin.boot_scene("live-twinrova-room"), "TwinrovaStage")
        self.assertIsNone(bt_plugin.boot_scene("live-leave-solo"))

    def test_a_game_left_in_an_event_elsewhere_is_warped_to_the_boot_scene(self):
        pair = self.lane(BT_FAKE_BOOT_EVENT="B")
        self.assertTrue(pair.state("B")["event"])
        st = pair.boot_ready("KakarikoVillage_Child")
        for x in ("A", "B"):
            self.assertTrue(pair.in_play(st[x]), st[x])
            self.assertEqual(st[x]["scene"], "KakarikoVillage_Child")
        self.assertTrue(any("B in Graveyard_Child" in n for n in pair.notes), pair.notes)

    def test_a_game_that_stays_in_its_event_is_setup_fail_boot(self):
        pair = self.lane(BT_FAKE_BOOT_STUCK="B")
        with self.assertRaisesRegex(P.SetupFail, r"^boot: .*B"):
            pair.boot_ready("KakarikoVillage_Child", max_frames=100)

    INTRO_FAKE = (
        "def setup(game):\n    game.intro_until = 0\n"
        "def command(game, m):\n"
        "    if m.get('cmd') != 'warp' or m.get('scene') != 'BossStage':\n        return None\n"
        "    game.run(40)\n    game.scene, game.intro_until = m['scene'], game.frame + 300\n"
        "    return {'ok': False, 'error': 'timeout', 'message': 'not in BossStage; held (the intro event)'}\n"
        "def state(game, st):\n"
        "    if game.scene == 'BossStage' and game.frame < game.intro_until:\n        st['event'] = True\n")

    def intro_pair(self, fake):
        import fakegame
        d = os.path.join(self.tmp, "fakes")
        os.makedirs(d)
        with open(os.path.join(d, "intro.py"), "w", encoding="utf-8") as f:
            f.write(fake)
        lane = fakegame.FakeLane(self.tmp, fakes_dir=d)
        self.addCleanup(lane.close)
        pair = P.Pair(lane.games(), out_dir=self.tmp)
        self.addCleanup(pair.close)
        pair.begin(240)
        return pair

    def test_a_boot_scene_that_starts_an_event_on_arrival_is_waited_out(self):
        """A boss stage boot (live-twinrova-room, live-phantom-ganon): the bridge's warp answers 'timeout' with Link
        there, in the intro event (coop_playtest_core.cpp WarpArrived waits for no event). boot_ready takes that
        arrival on state and steps until the intro ends (was SETUP-FAIL 'boot: BridgeError: timeout': the four
        test_ingame_boss_wake selftests on integrate-0120 645a7dffd)."""
        pair = self.intro_pair(self.INTRO_FAKE)
        st = pair.boot_ready("BossStage")
        for x in ("A", "B"):
            self.assertTrue(pair.in_play(st[x]), st[x])
            self.assertEqual(st[x]["scene"], "BossStage")
        self.assertTrue(any("answered timeout in the arrival event" in n for n in pair.notes), pair.notes)

    def test_a_boot_warp_timeout_with_no_event_is_still_setup_fail_boot(self):
        """arrive_in_event=None is no pass for every timeout: in the scene with no event = SETUP-FAIL boot."""
        pair = self.intro_pair(self.INTRO_FAKE.replace("game.frame + 300", "0"))
        with self.assertRaisesRegex(P.SetupFail, r"^boot: BridgeError: timeout"):
            pair.boot_ready("BossStage", max_frames=100)

    def test_a_stale_sample_is_stepped_not_judged(self):
        pair = self.lane(BT_FAKE_STALE_FRAMES=50)
        self.assertFalse(pair.in_play(pair.state("A")))          # (the one read party-hud-bridge r1 failed on)
        pair.until_in_play("both in play", max_frames=P.READY_FRAMES)
        self.assertTrue(all(pair.in_play(pair.state(x)) for x in ("A", "B")))

    def test_not_ready_in_time_names_each_game(self):
        pair = self.lane(BT_FAKE_STALE_FRAMES=5000)
        with self.assertRaisesRegex(AssertionError, r"not after 300 frames.*A: scene=.*not_ready=.*B: scene="):
            pair.until_in_play("both in play", max_frames=P.READY_FRAMES)

    def test_suite_on_a_boot_left_in_an_event_passes(self):
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--out", out, "-q", "-m", "smoke and not look", "--no-smoke-gate"],
                       env={"BT_FAKE_BOOT_EVENT": "B", "BT_FAKE_STALE_FRAMES": "50"})
        s = summary(out)
        self.assertEqual(s["counts"]["PASS"], len(s["tests"]), p.stdout[-2000:])

    def test_suite_on_a_stuck_boot_is_setup_fail_boot_everywhere(self):
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--out", out, "-q", "-m", "smoke and not look", "--no-smoke-gate"],
                       env={"BT_FAKE_BOOT_STUCK": "B"})
        s = summary(out)
        self.assertTrue(s["tests"], p.stdout[-2000:])
        for r in s["tests"]:
            self.assertEqual(r["result"], "SETUP-FAIL", r)
            self.assertIn("lane1 boot:", r["why"], r)


class TitleAndRecovery(unittest.TestCase):
    """The title test (pilot test-bridge-pilot-fa63e90f: both games' way back 'given up' every round, then load_both
    pressed A on games in play for 3600 frames and every later test cascaded): it runs last in its round, FAILs at
    once on the given-up line, never presses A on a game in play, and the next test starts after the recovery."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_smoke(self, **env):
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--out", out, "-q", "-m", "smoke and not look", "--repeat", "2",
                        "--no-smoke-gate", "--no-rerun"], env=env)
        return summary(out), p, out

    def logs(self, out):
        text = ""
        for x in ("A", "B"):
            with open(os.path.join(out, "lane1", "fake", "lane1-%s" % x, "game-out-%s.txt" % x), encoding="utf-8") as f:
                text += f.read()
        return text

    def test_title_runs_last_in_each_round(self):
        s, p, _ = self.run_smoke()
        names = [r["test"] for r in s["tests"]]
        n = len(names) // 2
        self.assertIn("test_title_and_load_again", names[n - 1], names)
        self.assertIn("test_title_and_load_again", names[-1], names)
        self.assertEqual(s["counts"]["PASS"], len(names), p.stdout[-2000:])

    def test_given_up_fails_at_once_and_does_not_cascade(self):
        s, p, out = self.run_smoke(BT_FAKE_TO_TITLE_GIVES_UP="AB")
        rows = {r["test"]: r for r in s["tests"]}
        title = [r for t, r in rows.items() if "test_title_and_load_again" in t]
        self.assertEqual([r["result"] for r in title], ["FAIL", "FAIL"], p.stdout[-2000:])
        self.assertTrue(all("the way back to the title gave up" in r["why"] for r in title), title)
        self.assertTrue(all(r["seconds"] < 60 for r in title), title)
        others = [r for t, r in rows.items() if "test_title_and_load_again" not in t]
        self.assertTrue(others and all(r["result"] == "PASS" for r in others), others)   # (round 2 after the recovery)
        self.assertNotIn("fake: A pressed in play", self.logs(out))


class CommitSampler(unittest.TestCase):
    """Each game's commit from the lane's boot start (C #134: target ~20 GB/game; pilot B ~30 GB from the boot)."""

    def test_samples_from_pids_json_and_summarises(self):
        import memsample
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        sess = os.path.join(tmp, "session")
        t = [0.0]
        m = memsample.Sampler(sess, tmp, every=30, read=lambda pid: {11: 19 * memsample.GB, 22: 30 * memsample.GB}[pid],
                              clock=lambda: t[0])
        self.assertIsNone(m.sample())                    # (no pids.json yet: the boot has not started the games)
        os.makedirs(sess)
        with open(os.path.join(sess, "pids.json"), "w") as f:
            json.dump({"games": {"A": 11, "B": 22}, "server": 5}, f)
        t[0] = 40.0
        self.assertEqual(m.sample()["B"], 30.0)
        t[0] = 70.0
        s = m.stop()                                     # (thread never started: one last sample)
        self.assertEqual(s["games"]["A"], {"first_gb": 19.0, "first_s": 40.0, "peak_gb": 19.0, "peak_s": 40.0,
                                           "last_gb": 19.0, "last_s": 70.0})
        self.assertEqual(s["games"]["B"]["peak_gb"], 30.0)
        with open(os.path.join(tmp, "commit.csv"), encoding="utf-8") as f:
            self.assertEqual(f.readline().strip(), "utc,s_after_boot_start,A_commit_gb,B_commit_gb")
        with open(os.path.join(tmp, "commit.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["samples"], 2)

    def test_own_process_reads_on_windows(self):
        import memsample
        b = memsample.private_bytes(os.getpid())
        if sys.platform == "win32":
            self.assertGreater(b, 1 << 20)
        else:
            self.assertIsNone(b)


if __name__ == "__main__":
    unittest.main()
