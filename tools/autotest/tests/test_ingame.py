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


def suite_size(marks=None):
    """How many tests the real suite runs (its own collection, as a run with no --env deselects): the counts below
    follow every row added to ingame/, so a new row port does not break these selftests (row-53-ocarina-5)."""
    out = tempfile.mkdtemp()
    args = [".", "--fake", "--collect-only", "-q", "-p", "no:cacheprovider", "--out", out] + (["-m", marks] if marks else [])
    p = pytest_run(args)
    shutil.rmtree(out, ignore_errors=True)
    return sum(1 for ln in p.stdout.splitlines() if "::" in ln and not ln.startswith("deselected"))


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
        self.assertEqual(s["counts"]["NO-REF"], looks, p.stdout[-2000:])
        self.assertEqual(s["counts"]["PASS"], total - looks, p.stdout[-2000:])
        self.assertEqual(s["verdict"], "NOT PASS")
        self.assertNotEqual(p.returncode, 0)
        new = os.path.join(out1, "lane1", "new")
        for f in os.listdir(new):                  # (a person approves the candidates: here, all of them)
            shutil.copyfile(os.path.join(new, f), os.path.join(refs, f))
        out2 = os.path.join(self.tmp, "o2")
        p = pytest_run([".", "--fake", "--out", out2, "-q"], env={"BT_FAKE_REFS": refs})
        s = summary(out2)
        self.assertEqual(s["counts"]["PASS"], total, p.stdout[-2000:])
        self.assertEqual(s["verdict"], "PASS")
        self.assertEqual(p.returncode, 0)
        shots = [sh for r in s["tests"] for sh in r["shots"]]
        self.assertEqual(len(shots), 10)            # pause 2 + masks 4 + HUD 4: a few per rc, not hundreds
        self.assertTrue(all(sh["ok"] and sh["score"] >= sh["tolerance"] for sh in shots))

    def test_default_suite_has_no_setup_fail(self):
        """The default run (no --env) of the real suite: 0 SETUP-FAIL (a needs_env test is deselected, B #129)."""
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--out", out, "-q", "-m", "not look"])
        s = summary(out)
        self.assertEqual(s["counts"]["SETUP-FAIL"], 0, p.stdout[-2000:])
        self.assertTrue(s["tests"])
        for d in s["deselected"]:
            self.assertTrue(d["needs_env"] or d.get("needs_boot"), d)     # (pganon-cct-287-3: a boss row's boot)

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
                             "test_times_out": "FAIL", "test_fail_naming_noref": "FAIL", "test_unproven": "NO-REF"},
                         p.stdout[-3000:])
        rows = {x["test"].split("::")[-1]: x for x in summary(self.out)["tests"]}
        self.assertIn("timeout", rows["test_times_out"]["why"])
        self.assertIn("set-up: RuntimeError: no such scene", rows["test_setup_fails"]["why"])
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
        p = pytest_run([".", "--fake", "--out", out, "-q", "--env", "LICHII_COOP_X=1", "--env", "A:LICHII_COOP_Y=2"],
                       cwd=self.dir, env={"BT_FAKE_BOOT_CRASH": "CRASH: exception 0xc0000005"})
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


class HookOrder(unittest.TestCase):
    """bt_plugin's runtest protocol must answer before pytest's own (both are firstresult): without tryfirst the FLAKY
    rerun, NOT-RUN and --deadline-min logic could be skipped silently (review-c123-6, 646484a63)."""

    def test_runtest_protocol_is_tryfirst(self):
        import bt_plugin as B
        opts = getattr(B.pytest_runtest_protocol, "pytest_impl", None) or {}
        self.assertTrue(opts.get("tryfirst"), "bt_plugin.pytest_runtest_protocol lost @pytest.hookimpl(tryfirst=True)")
        self.assertFalse(hasattr(B._past_deadline, "pytest_impl"), "the hookimpl decorator sits on a helper")


AUTOTEST = os.path.dirname(HERE)


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


HOOK_TESTS = 'import pytest\n\ndef test_hooks(pair):\n    pair.act("A", "frob 3")\n    pair.expect_log("A", r"fake hook: frob 3 on A", max_frames=30)\n    st = pair.state("B")\n    assert any(a.get("name") == "HookActor_B" for a in st["world"]["actors"]), st["world"]\n    assert st.get("hooked") == "shared-1"\n    r = pair.call("A", "ping_hook")\n    assert r.get("pong") is True\n    pair.press("A", ["ZR"], frames=2)\n    pair.expect_log("A", r"fake hook: pressed ZR", max_frames=30)\n\n    import playtest_client as pc\n    with pytest.raises(pc.BridgeError):           # (a plain warp: the boss intro event makes it a timeout)\n        pair.warp_both("BossStage_Test")\n    st = pair.warp_both("BossStage_Test", arrive_in_event=True)\n    assert st["A"]["scene"] == st["B"]["scene"] == "BossStage_Test"\n    assert any("answered timeout in the arrival event" in n for n in pair.notes), pair.notes\n    pair.act("A", "place 0 0 0")\n    pair.call("A", "move", yaw=90, strength=1.0, frames=30)\n    assert pair.state("A")["pos"][0] == 1.0, pair.state("A")["pos"]      # (move_step: the wall at x 1.0)\n    with pytest.raises(pc.BridgeError):           # (a timeout with NO event is not an arrival: review-c123-8)\n        pair.warp_both("NoEventStage_Test", arrive_in_event=True)\n'
HOOK_MODULE = 'def setup(game):\n    game.hook_seen = []\n\ndef lane(fl):\n    fl.g["A"].shared = fl.g["B"].shared = {"n": 1}\n\ndef command(game, m):\n    if m.get("cmd") == "ping_hook":\n        return {"ok": True, "pong": True}\n    if m.get("cmd") == "warp" and m.get("scene") in ("BossStage_Test", "NoEventStage_Test"):\n        game.run(40)\n        game.scene = m["scene"]                 # (there, but the bridge says timeout: an intro event, or not)\n        return {"ok": False, "error": "timeout", "message": "still in an event"}\n    return None\n\ndef move_step(game, pos):\n    if game.scene == "BossStage_Test" and pos[0] > 1.0:\n        return [1.0, pos[1], pos[2]]\n    return None\n\ndef after(game, m, r):\n    if m.get("cmd") == "state":\n        r["hooked"] = "shared-%d" % game.shared["n"]\n\ndef press(game, buttons):\n    if "ZR" in buttons:\n        game.say("fake hook: pressed ZR")\n        return True\n    return False\n\ndef act(game, words):\n    if words[0] == "frob":\n        game.say("fake hook: frob %s on %s" % (words[1], game.tag))\n        return True\n    return False\n\ndef actors(game):\n    return [{"id": 99, "name": "HookActor_%s" % game.tag, "kind": "enemy", "pos": [0.0, 0.0, 0.0]}]\n\ndef state(game, st):\n    if game.scene == "BossStage_Test":\n        st["event"] = True                      # (its intro event runs)\n'


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
        self.assertLessEqual(clock.t, P.SETUP_LIMIT_S)
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
        self.assertLessEqual(clock.t, P.SETUP_LIMIT_S)
        self.assertTrue(all(w <= P.SETUP_LIMIT_S for w in pair.c["A"].waits), pair.c["A"].waits)

    def test_short_test_budget_wins(self):
        pair, clock = self.make(10)
        with self.assertRaises(P.SetupFail):
            with pair.setup():
                pair.state("A")
        self.assertLessEqual(pair.c["A"].waits[0], 10)


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


if __name__ == "__main__":
    unittest.main()
