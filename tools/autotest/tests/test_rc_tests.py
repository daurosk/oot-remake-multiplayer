"""rc_tests.py: one rc pass of the in-game bridge tests over both lanes (collect, plan in sessions, one summary)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

AUTOTEST = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, AUTOTEST)
import proven as pv  # noqa: E402
import rc_tests as rt  # noqa: E402

MOD = "tools/autotest/ingame/test_rows.py"
BASE = ["LICHII_COOP_PUPPET=1", "LICHII_COOP_TEST_TO_TITLE=1"]
HUD = ["COOP_TEST_PARTY_HUD=1", "B:COOP_TEST_PARTY_HUD_NO_PACK=1"]   # test_party_hud.py's pytestmark (party-hud-test-c126)
BOOT = {g: {"script": "tools/autotest/scenarios/live-kakariko-child.%s.txt" % g, "sha256": g * 64,
            "switches": ["LICHII_COOP_CHEATS=1"]} for g in ("A", "B")}   # bt_plugin boot_identity (949467249)
S1, S2 = "test_smoke.py::test_boot", "test_smoke.py::test_pause"
A, B, C = "test_rows.py::test_a", "test_rows.py::test_b", "test_rows.py::test_c"
H1, H2 = "test_party_hud.py::test_party_hud_after_boot", "test_party_hud.py::test_party_hud_title_and_load_again"

INI = "[pytest]\nmarkers =\n    smoke: rc smoke tier\n    needs_env: game switches\n"
FILES = {
    "pytest.ini": INI,
    "bt_plugin.py": "import os\nBASE_ENV = %r   # every boot\n\n\ndef boot_identity():\n    return %r\n" % (BASE, BOOT),
    "test_smoke.py": "import pytest\n\n@pytest.mark.smoke\ndef test_boot(): pass\n\n"
                     "@pytest.mark.smoke\ndef test_pause(): pass\n",
    "test_rows.py": "def test_a(): pass\n\ndef test_b(): pass\n\ndef test_c(): pass\n",
    "test_party_hud.py": "import pytest\n\npytestmark = pytest.mark.needs_env(%s)\n\n"
                         "def test_party_hud_after_boot(): pass\n\ndef test_party_hud_title_and_load_again(): pass\n"
                         % ", ".join(repr(e) for e in HUD),
}


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def items(*specs):
    """collect() rows: (test, smoke, env)."""
    return [{"test": t, "smoke": s, "env": list(e)} for t, s, e in specs]


MARKER_DUMP = r'''
import json, sys
import pytest
sys.path.insert(0, sys.argv[3])
import proven


class Dump:
    """Each collected test's own markers, read before bt_plugin deselects any: {base nodeid: [needs_env args in
    marker order (deduplicated), needs_boot name or "", smoke]}."""
    def __init__(self):
        self.out = {}

    @pytest.hookimpl(tryfirst=True)
    def pytest_collection_modifyitems(self, config, items):
        for it in items:
            env = []
            for m in it.iter_markers("needs_env"):
                env += [str(a).strip() for a in m.args if str(a).strip() not in env]
            b = it.get_closest_marker("needs_boot")
            self.out.setdefault(proven.base_nodeid(it.nodeid),
                                [env, str(b.args[0]) if b and b.args else "", it.get_closest_marker("smoke") is not None])


d = Dump()
code = pytest.main([sys.argv[1], "--collect-only", "-q", "-p", "no:cacheprovider"], plugins=[d])
with open(sys.argv[2], "w", encoding="utf-8") as f:
    json.dump({"code": int(code), "tests": d.out}, f)
'''


def marker_dump(folder, work):
    """{test: (needs_env list, needs_boot, smoke)} of the tests under folder, from a pytest collection of this
    selftest's own (an independent read of the markers, to check rc_tests.collect + groups_of against)."""
    import subprocess
    script, out, cwd = (os.path.join(work, n) for n in ("marker_dump.py", "marker_dump.json", "marker_dump"))
    write(script, MARKER_DUMP)
    os.makedirs(cwd, exist_ok=True)   # (bt_plugin's session end writes ingame-out/ into the current folder)
    p = subprocess.run([sys.executable, "-I", script, os.path.abspath(folder), out, AUTOTEST], cwd=cwd,
                       capture_output=True, text=True, timeout=300)
    if not os.path.isfile(out):
        raise AssertionError("marker dump: exit %s: %s" % (p.returncode, (p.stdout + p.stderr)[-600:]))
    with open(out, encoding="utf-8") as f:
        doc = json.load(f)
    if doc["code"] not in (0, 1) or not doc["tests"]:   # (bt_plugin: a collect-only session with no test run = 1)
        raise AssertionError("marker dump: pytest exit %s, %d test(s)" % (doc["code"], len(doc["tests"])))
    return {t: (v[0], v[1], v[2]) for t, v in doc["tests"].items()}


def group_faults(marks, groups):
    """What is wrong with rc_tests.groups_of's groups for these markers ({test: (env, boot, smoke)}): the default
    group first; each test in exactly the group of its own (needs_boot, proven.env_key(needs_env)); no empty group;
    smoke-tier tests (smoke, no env/boot) in none, every other test in one."""
    faults, placed = [], {}
    if not groups or groups[0]["name"] != "default" or groups[0]["boot"] or groups[0]["env"]:
        faults.append("the first group is not the default one")
    for g in groups:
        want = (g["boot"], pv.env_key(g["env"]))
        if g["name"] != "default" and not g["tests"]:
            faults.append("empty group %s" % g["name"])
        for t in g["tests"]:
            env, boot, smoke = marks[t]
            if t in placed:
                faults.append("%s in two groups (%s, %s)" % (t, placed[t], g["name"]))
            placed[t] = g["name"]
            if (boot, pv.env_key(env)) != want:
                faults.append("%s (needs_boot %r, needs_env %s) is in group %s" % (t, boot, env, g["name"]))
    for t, (env, boot, smoke) in marks.items():
        if smoke and not env and not boot:
            if t in placed:
                faults.append("smoke-tier %s is in group %s" % (t, placed[t]))
        elif t not in placed:
            faults.append("%s is in no group" % t)
    return faults


BOSS = "live-twinrova-room"
K1, K2 = "test_boss.py::test_twinrova_shooter", "test_boss.py::test_twinrova_hud"
# bt_plugin 86711283c's own collection step: a test this run's --boot/--env cannot run is deselected, also on
# --collect-only (the plan's collector must see it anyway).
DESELECT = ("import pytest\n\n\ndef pytest_collection_modifyitems(config, items):\n"
            "    keep = [it for it in items if not it.get_closest_marker('needs_boot')\n"
            "            and not it.get_closest_marker('needs_env')]\n"
            "    drop = [it for it in items if it not in keep]\n"
            "    if drop:\n        config.hook.pytest_deselected(items=drop)\n        items[:] = keep\n")
BOSS_FILE = ("import pytest\n\npytestmark = pytest.mark.needs_boot(%r)\n\n\ndef test_twinrova_shooter(): pass\n\n\n"
             "@pytest.mark.needs_env('COOP_TEST_PARTY_HUD=1')\ndef test_twinrova_hud(): pass\n" % BOSS)

SUITE = items((S1, True, []), (S2, True, []), (A, False, []), (B, False, []), (C, False, []),
              (H1, False, HUD), (H2, False, HUD))


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.repo = os.path.join(self.tmp, "repo")
        self.ingame = os.path.join(self.repo, "tools", "autotest", "ingame")
        for name, text in FILES.items():
            write(os.path.join(self.ingame, name), text)
        self.n = 0

    def summary(self, root, rows, key="k1", env=None, boot=BOOT, **extra):
        """A session's summary.json as bt_plugin writes it (identity: key + env + boot; per-row test_hash)."""
        self.n += 1
        d = os.path.join(self.tmp, root, "run%d" % self.n)
        ident = {"env": list(BASE) if env is None else env}
        if boot is not None:
            ident["boot"] = boot
        if key:
            ident["key"] = key
        doc = dict({"identity": ident, "tests": rows, "verdict": "x", "minutes": 1}, **extra)
        write(os.path.join(d, "summary.json"), json.dumps(doc))
        return d

    def row(self, test, result="PASS", seconds=60, hashed=True):
        r = {"test": test, "result": result, "seconds": seconds}
        if hashed:
            r["test_hash"] = pv.test_hash(pv.base_nodeid(test), self.repo)
        return r

    def plan(self, its=SUITE, lanes=(1, 2), **kw):
        return rt.plan(its, list(lanes), repo=self.repo, **kw)

    def session(self, doc, sid):
        return [s for s in doc["sessions"] if s["id"] == sid][0]

    def env_session(self, doc):
        return [s for s in doc["sessions"] if s["group"] == "env1"][0]


class Hash(Base):
    def test_base_nodeid(self):
        for n, want in (("a.py::t[r3]", "a.py::t"), ("a.py::t[x-r2]", "a.py::t[x]"), ("a.py::t", "a.py::t")):
            self.assertEqual(pv.base_nodeid(n), want)

    def test_test_hash_follows_module_harness_and_refs(self):
        base = pv.test_hash(A, self.repo)
        self.assertTrue(base)
        self.assertEqual(pv.test_hash(B, self.repo), base)                       # same file: same hash
        self.assertEqual(pv.test_hash(MOD + "::test_a", self.repo), base)        # the repo-relative form
        self.assertEqual(pv.test_hash("gone.py::t", self.repo), "")
        for rel in (MOD, "tools/autotest/ingame/pair.py", "tools/autotest/ingame/refs/x-A.png"):
            write(os.path.join(self.repo, *rel.split("/")), "changed %s\n" % rel)
            now = pv.test_hash(A, self.repo)
            self.assertNotEqual(base, now, rel)
            base = now


class Collect(Base):
    def test_collect_gives_base_ids_smoke_and_needs_env(self):
        got = rt.collect(self.ingame, self.tmp)
        self.assertEqual({x["test"]: (x["smoke"], x["env"]) for x in got},
                         {S1: (True, []), S2: (True, []), A: (False, []), B: (False, []), C: (False, []),
                          H1: (False, HUD), H2: (False, HUD)})

    def test_collect_takes_a_not_pass_session_end_but_not_a_broken_file(self):
        write(os.path.join(self.ingame, "conftest.py"),
              "def pytest_sessionfinish(session, exitstatus):\n    session.exitstatus = 1   # bt_plugin: no test ran\n")
        self.assertEqual(len(rt.collect(self.ingame, self.tmp)), 7)
        self.assertFalse(os.path.exists(os.path.join(self.repo, "ingame-out")))
        write(os.path.join(self.ingame, "test_broken.py"), "def test_x(:\n")
        with self.assertRaises(rt.Fail):
            rt.collect(self.ingame, self.tmp)

    def test_env_group_holds_only_the_party_hud_tests_and_default_none(self):
        groups = rt.groups_of(rt.collect(self.ingame, self.tmp))
        self.assertEqual([(g["name"], g["env"], g["tests"]) for g in groups],
                         [("default", [], [A, B, C]), ("env1", HUD, [H1, H2])])

    def test_real_suite_groups_follow_each_tests_own_markers(self):
        """B #129 selftest on the repo's own tools/autotest/ingame (B #139: was pinned to test_party_hud.py, which
        broke on every new needs_env test). The expected group of EVERY test comes from its own needs_env /
        needs_boot / smoke markers, read by a pytest collection of this test's own (marker_dump below, not
        rc_tests' collector): each test is in exactly the group of its (needs_boot, proven.env_key(needs_env)), the
        default group holds no needs_env or needs_boot test, a group never holds a test of another key, and no test
        is lost. A non-env test landing in an env group (or the reverse) fails here."""
        real = os.path.join(AUTOTEST, "ingame")
        if not os.path.isfile(os.path.join(real, "test_party_hud.py")):
            self.skipTest("no tools/autotest/ingame/test_party_hud.py on this branch yet")
        marks = marker_dump(real, self.tmp)
        got = rt.collect(real, self.tmp)
        self.assertEqual({x["test"] for x in got}, set(marks))                       # the same tests
        for x in got:                                                                # rc_tests reads the markers
            self.assertEqual((x["env"], x["boot"], x["smoke"]), marks[x["test"]], x["test"])
        self.assertTrue(any(m[0] for m in marks.values()), "no needs_env test in the real suite")
        self.assertTrue(all(marks[t][0] for t in (x["test"] for x in got) if t.startswith("test_party_hud.py::")))
        self.assertEqual(group_faults(marks, rt.groups_of(got)), [])

    def test_group_faults_catches_a_test_in_the_wrong_group(self):
        marks = {A: ([], "", False), S1: ([], "", True), H1: (HUD, "", False), K1: ([], BOSS, False)}
        good = [{"name": "default", "boot": "", "env": [], "tests": [A]},
                {"name": "env1", "boot": "", "env": HUD, "tests": [H1]},
                {"name": BOSS, "boot": BOSS, "env": [], "tests": [K1]}]
        self.assertEqual(group_faults(marks, good), [])
        bad = [dict(g, tests=list(g["tests"])) for g in good]
        bad[1]["tests"].append(A)                                   # a non-env test in the env group
        self.assertTrue(any("is in group env1" in f for f in group_faults(marks, bad)))
        bad = [dict(g, tests=list(g["tests"])) for g in good]
        bad[0]["tests"].append(H1); bad[1]["tests"] = []            # an env test in the default group
        faults = group_faults(marks, bad)
        self.assertTrue(any("is in group default" in f for f in faults) and any("empty group env1" in f for f in faults))
        bad = [dict(g, tests=list(g["tests"])) for g in good]
        bad[2]["tests"] = []                                        # a needs_boot test lost
        self.assertTrue(any("%s is in no group" % K1 in f for f in group_faults(marks, bad)))
        bad = [dict(g, tests=list(g["tests"])) for g in good]
        bad[0]["tests"].append(S1)                                  # a smoke test in a group
        self.assertTrue(any("smoke" in f for f in group_faults(marks, bad)))


class Boots(Base):
    """bt_plugin 86711283c: @pytest.mark.needs_boot(NAME) runs only under --boot NAME (review-c123-7 09:3x)."""

    def setUp(self):
        super().setUp()
        write(os.path.join(self.ingame, "conftest.py"), DESELECT)
        write(os.path.join(self.ingame, "test_boss.py"), BOSS_FILE)
        self.ingame_ini = os.path.join(self.ingame, "pytest.ini")
        write(self.ingame_ini, INI + "    needs_boot: boot scenario\n")

    def test_collect_sees_the_tests_bt_plugin_deselects(self):
        got = {x["test"]: (x["env"], x["boot"]) for x in rt.collect(self.ingame, self.tmp)}
        self.assertEqual(got[K1], ([], BOSS))
        self.assertEqual(got[K2], (["COOP_TEST_PARTY_HUD=1"], BOSS))
        self.assertEqual(got[H1], (HUD, ""))
        self.assertEqual(got[A], ([], ""))

    def test_groups_by_boot_and_env_and_sessions_get_boot(self):
        its = rt.collect(self.ingame, self.tmp)
        groups = rt.groups_of(its)
        self.assertEqual([(g["name"], g["boot"], g["env"], g["tests"]) for g in groups],
                         [("default", "", [], [A, B, C]), (BOSS, BOSS, [], [K1]),
                          ("%s-env2" % BOSS, BOSS, ["COOP_TEST_PARTY_HUD=1"], [K2]), ("env3", "", HUD, [H1, H2])])
        doc = self.plan(its=its)
        for s in doc["sessions"]:
            cmd = rt.command(s, "p.json", "runs")
            boots = [cmd[i + 1] for i, x in enumerate(cmd) if x == "--boot"]
            if s["group"] in ("default", "env3"):
                self.assertEqual(boots, [], s["id"])                          # the default boot: no --boot
                self.assertFalse({K1, K2} & set(s["tests"]), s["id"])
            else:
                self.assertEqual(boots, [BOSS], s["id"])
                self.assertFalse({S1, S2} & set(s["tests"]), s["id"])         # smoke only on the default boot
        played = sorted(sum((s["tests"] for s in doc["sessions"]), []))
        self.assertEqual(played, sorted([S1, S2] * 2 + [A, B, C, H1, H2, K1, K2]))

    def test_a_boss_test_is_proven_only_by_a_pass_on_its_own_boot(self):
        runs = os.path.join(self.tmp, "runs")
        its = rt.collect(self.ingame, self.tmp)
        boss = {g: dict(BOOT[g], script="tools/autotest/scenarios/%s.%s.txt" % (BOSS, g), sha256=g.lower() * 64)
                for g in ("A", "B")}
        boots = {"": BOOT, BOSS: boss}
        self.summary("runs", [self.row(K1)])                                  # a PASS on the default boot
        self.assertEqual(self.plan(its=its, proven_roots=[runs], keys=["k1"], boot=boots)["skipped"], [])
        self.summary("runs", [self.row(K1)], boot=dict(boss, name=BOSS))      # bt_plugin adds the boot's name
        self.assertEqual([x["test"] for x in self.plan(its=its, proven_roots=[runs], keys=["k1"], boot=boots)["skipped"]],
                         [K1])
        with self.assertRaises(rt.Fail):                                       # a boot bt_plugin does not know
            self.plan(its=its, proven_roots=[runs], keys=["k1"], boot={"": BOOT})


class Plan(Base):
    def test_sessions_smoke_per_lane_env_group_its_own_boot(self):
        doc = self.plan()
        self.assertEqual(len(doc["sessions"]), 3)
        for n in (1, 2):
            d = self.session(doc, "lane%d-default" % n)
            self.assertEqual(d["tests"][:2], [S1, S2])                       # smoke on every lane
            self.assertEqual(d["env"], [])                                  # default boot: no extra switch
            self.assertFalse(set(d["tests"]) & {H1, H2})
        env = self.env_session(doc)
        self.assertEqual((env["tests"], env["env"]), ([H1, H2], HUD))        # together, own boot, its switches
        self.assertEqual(sorted(sum((s["tests"] for s in doc["sessions"]), [])),
                         sorted([S1, S2] * 2 + [A, B, C, H1, H2]))

    def test_balance_by_measured_seconds_env_group_as_one_unit(self):
        h = self.summary("hist", [self.row(A, seconds=900), self.row(B, seconds=100), self.row(C, seconds=100),
                                  self.row(H1, seconds=200), self.row(H2, seconds=200), self.row(S1, seconds=10),
                                  self.row(S2, seconds=10), self.row(C, "SETUP-FAIL", seconds=1)])
        doc = self.plan(roots=[os.path.dirname(h)])
        lane1 = [s for s in doc["sessions"] if s["lane"] == 1]
        lane2 = [s for s in doc["sessions"] if s["lane"] == 2]
        self.assertEqual([s["tests"] for s in lane1], [[S1, S2, A]])          # A (900) alone
        self.assertEqual([s["group"] for s in lane2], ["default", "env1"])    # env1 (boot 120 + 400) + B + C
        self.assertEqual(doc["lane_seconds"], {"1": 1040.0, "2": 860.0})
        self.assertEqual(doc["estimates"][C]["seconds"], 100.0)              # the SETUP-FAIL row is not a timing

    def test_commands_and_plan_files(self):
        out = os.path.join(self.tmp, "plan")
        doc = self.plan(env=["LICHII_COOP_X=1"])
        rt.write_plan(doc, out, os.path.join(self.tmp, "runs"))
        env = self.env_session(doc)
        self.assertEqual(env["env"], HUD + ["LICHII_COOP_X=1"])
        cmd = env["command"]
        self.assertEqual([cmd[i + 1] for i, x in enumerate(cmd) if x == "--env"], HUD + ["LICHII_COOP_X=1"])
        self.assertEqual(cmd[cmd.index("--out") + 1], os.path.join(self.tmp, "runs", env["id"]))
        with open(env["plan_file"], encoding="utf-8") as f:
            sp = json.load(f)
        self.assertEqual(sp["lanes"], {str(env["lane"]): [H1, H2]})
        d = self.session(doc, "lane1-default")["command"]
        self.assertEqual([d[i + 1] for i, x in enumerate(d) if x == "--env"], ["LICHII_COOP_X=1"])
        plain = rt.command(self.session(self.plan(), "lane1-default"), "p.json", "runs")
        self.assertNotIn("--env", plain)                                    # the default group: no extra switch

    def test_pass_on_every_key_same_boot_env_is_skipped(self):
        runs = os.path.join(self.tmp, "runs")
        d1 = self.summary("runs", [self.row(A), self.row(S1)], key="k1")
        self.summary("runs", [self.row(A + "[r2]")], key="k2")
        self.summary("runs", [self.row(H1)], key="k1", env=BASE + HUD)
        self.summary("runs", [self.row(H1)], key="k2", env=BASE + ["A:COOP_TEST_PARTY_HUD=1",
                                                                   "B:COOP_TEST_PARTY_HUD=1",
                                                                   "B:COOP_TEST_PARTY_HUD_NO_PACK=1"])
        self.summary("runs", [self.row(H2)], key="k1")                     # a boot WITHOUT the HUD switches
        self.summary("runs", [self.row(H2)], key="k2")
        doc = self.plan(proven_roots=[runs], keys=["k1", "k2"])
        self.assertEqual(sorted(s["test"] for s in doc["skipped"]), sorted([A, H1]))
        self.assertIn(d1, {x["folder"] for s in doc["skipped"] for x in s["proof"]})
        played = sum((s["tests"] for s in doc["sessions"]), [])
        self.assertNotIn(A, played)
        self.assertIn(H2, played)                                            # its PASS was on another boot
        self.assertIn(S1, played)                                            # smoke is never skipped

    def test_other_boot_switches_prove_nothing(self):
        runs = os.path.join(self.tmp, "runs")
        self.summary("runs", [self.row(A)], env=BASE + ["LICHII_COOP_PARTY_HUD_PROBE=1"])   # the HUD-off pilot
        self.summary("runs", [self.row(B)], env=BASE + ["B:LICHII_COOP_CHEATS=1"])
        self.summary("runs", [self.row(C)], env="not a list")
        self.assertEqual(self.plan(proven_roots=[runs], keys=["k1"])["skipped"], [])
        doc = self.plan(proven_roots=[runs], keys=["k1"], env=["LICHII_COOP_PARTY_HUD_PROBE=1"])
        self.assertEqual([x["test"] for x in doc["skipped"]], [A])

    def test_flaky_fail_setup_fail_noref_notrun_other_key_old_hash_never_prove(self):
        runs = os.path.join(self.tmp, "runs")
        for bad in ("FLAKY", "FAIL", "SETUP-FAIL", "NO-REF", "NOT-RUN"):
            shutil.rmtree(runs, True)
            self.summary("runs", [self.row(A), self.row(A, bad)])
            self.assertEqual(self.plan(proven_roots=[runs], keys=["k1"])["skipped"], [], bad)
        shutil.rmtree(runs, True)
        self.summary("runs", [self.row(A)], key="other")
        self.summary("runs", [self.row(A)], key=None)
        self.summary("runs", [self.row(A, hashed=False)])
        self.assertEqual(self.plan(proven_roots=[runs], keys=["k1"])["skipped"], [])
        shutil.rmtree(runs, True)
        self.summary("runs", [self.row(A)])
        self.assertEqual(self.plan(proven_roots=[runs], keys=["k1", "k2"])["skipped"], [])   # every key
        write(os.path.join(self.repo, *MOD.split("/")), "def test_a(): assert 1\n")           # module changed
        self.assertEqual(self.plan(proven_roots=[runs], keys=["k1"])["skipped"], [])

    def test_another_boot_or_none_proves_nothing(self):
        runs = os.path.join(self.tmp, "runs")
        other = json.loads(json.dumps(BOOT))
        other["A"]["switches"] = []                                          # a boot without cheats
        script = json.loads(json.dumps(BOOT))
        script["B"]["sha256"] = "f" * 64                                     # the boot script's text changed
        path = json.loads(json.dumps(BOOT))
        path["A"]["script"] = "tools/autotest/scenarios/live-title.A.txt"   # another boot script
        self.summary("runs", [self.row(A)], boot=other)
        self.summary("runs", [self.row(B)], boot=script)
        self.summary("runs", [self.row(C)], boot=path)
        self.summary("runs", [self.row(H1)], env=BASE + HUD, boot=None)     # a summary from before identity.boot
        self.summary("runs", [self.row(H2)], env=BASE + HUD, boot={"A": BOOT["A"]})
        self.assertEqual(self.plan(proven_roots=[runs], keys=["k1"])["skipped"], [])
        self.summary("runs", [self.row(A, "FAIL")], boot=other)              # a FAIL on another boot is not counted
        self.summary("runs", [self.row(A)])                                  # either: only this boot's rows prove
        self.assertEqual([x["test"] for x in self.plan(proven_roots=[runs], keys=["k1"])["skipped"]], [A])

    def test_boot_is_asked_of_bt_plugin(self):
        self.assertEqual(pv.ingame_boot_identity(self.repo), BOOT)
        self.assertEqual(pv.boot_key(BOOT), pv.boot_key({g: dict(BOOT[g], script=BOOT[g]["script"].replace("/", "\\"))
                                                          for g in BOOT}))
        write(os.path.join(self.ingame, "bt_plugin.py"), "BASE_ENV = %r\n" % BASE)   # no boot_identity
        with self.assertRaises(pv.Fail):
            self.plan(proven_roots=[self.tmp], keys=["k1"])

    def test_base_env_is_read_from_bt_plugin(self):
        self.assertEqual(pv.ingame_base_env(self.repo), BASE)
        os.remove(os.path.join(self.ingame, "bt_plugin.py"))
        with self.assertRaises(pv.Fail):
            self.plan(proven_roots=[self.tmp], keys=["k1"])

    def test_refusals(self):
        with self.assertRaises(rt.Fail):
            self.plan(proven_roots=[self.tmp], keys=[])
        with self.assertRaises(rt.Fail):
            self.plan(lanes=())
        with self.assertRaises(rt.Fail):
            self.plan(its=[])


class Summary(Base):
    def planned(self):
        return {"sessions": [{"id": "lane1-default", "tests": [S1, A]}, {"id": "lane2-default", "tests": [S1, B]},
                             {"id": "lane2-env1", "tests": [H1, H2]}],
                "skipped": [{"test": C, "proof": [{"result": "PASS", "folder": "G"}]}]}

    def test_one_summary_from_every_session_and_the_plan(self):
        l1 = self.summary("o", [{"test": S1, "result": "PASS"}, {"test": A + "[r1]", "result": "PASS"}])
        l2 = self.summary("o", [{"test": S1, "result": "PASS"}, {"test": B, "result": "PASS"}])
        e2 = self.summary("o", [{"test": H1, "result": "PASS"}, {"test": H2, "result": "PASS"}], env=BASE + HUD)
        doc = rt.merge([("lane1-default", l1), ("lane2-default", l2), ("lane2-env1", e2)], self.planned())
        self.assertEqual((doc["verdict"], doc["counts"]["PASS"], doc["counts"]["SKIP"]), ("PASS", 6, 1))
        self.assertIn("| SKIP | %s |" % C, rt.summary_md(doc))

    def test_a_session_that_did_not_run_makes_its_tests_not_run(self):
        l1 = self.summary("o", [{"test": S1, "result": "PASS"}, {"test": A, "result": "PASS"}])
        l2 = self.summary("o", [{"test": S1, "result": "PASS"}])             # smoke gate / deselect bug: B missing
        doc = rt.merge([("lane1-default", l1), ("lane2-default", l2)], self.planned())   # env session never ran
        nr = sorted((r["test"], r["session"]) for r in doc["tests"] if r["result"] == "NOT-RUN")
        self.assertEqual(nr, sorted([(B, "lane2-default"), (H1, "lane2-env1"), (H2, "lane2-env1")]))
        self.assertEqual((doc["verdict"], doc["counts"]["NOT-RUN"]), ("NOT PASS", 3))
        self.assertIn("| NOT-RUN | %s | lane2-env1 |" % H1, rt.summary_md(doc))
        with self.assertRaises(rt.Fail):
            rt.merge([l1], self.planned())                                   # a plan needs (session, dir)
        with self.assertRaises(rt.Fail):
            rt.merge([("x", l1)], {"lanes": {}})                               # an old lanes-only plan

    def test_any_not_pass_or_a_missing_session_is_not_pass(self):
        l1 = self.summary("o", [{"test": "t::a", "result": "PASS"}])
        for bad in ("FAIL", "FLAKY", "SETUP-FAIL", "NO-REF", "NOT-RUN", "WEIRD"):
            l2 = self.summary("o", [{"test": "t::b", "result": bad}])
            self.assertEqual(rt.merge([l1, l2])["verdict"], "NOT PASS", bad)
        doc = rt.merge([l1, os.path.join(self.tmp, "nolane")])
        self.assertEqual((doc["verdict"], doc["counts"]["SETUP-FAIL"]), ("NOT PASS", 1))
        self.assertEqual(rt.merge([])["verdict"], "NOT PASS")

    def test_cli_plan_runs_and_summary(self):
        out, runs = os.path.join(self.tmp, "plan"), os.path.join(self.tmp, "runs")
        self.assertEqual(rt.main(["plan", "--ingame", self.ingame, "--out-dir", out, "--runs", runs]), 0)
        with open(os.path.join(out, "plan.json"), encoding="utf-8") as f:
            doc = json.load(f)
        self.assertEqual(len(doc["sessions"]), 3)
        for s in doc["sessions"]:
            write(os.path.join(runs, s["id"], "summary.json"),
                  json.dumps({"tests": [{"test": t, "result": "PASS"} for t in s["tests"]]}))
        rc = os.path.join(self.tmp, "rc")
        self.assertEqual(rt.main(["summary", "--plan", os.path.join(out, "plan.json"), "--runs", runs, "--out", rc]), 0)
        self.assertTrue(os.path.isfile(os.path.join(rc, "summary.md")))
        env = self.env_session(doc)
        shutil.rmtree(os.path.join(runs, env["id"]))
        self.assertEqual(rt.main(["summary", "--plan", os.path.join(out, "plan.json"), "--runs", runs,
                                  "--out", rc]), 1)                           # its tests SETUP-FAIL + NOT-RUN


if __name__ == "__main__":
    unittest.main()
