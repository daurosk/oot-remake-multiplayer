"""Selftests of tools/autotest/ingame/fmtcheck.py and its collection gate in bt_plugin (no game).

probe0121-c53046a6 (2026-11-27 22:41 UTC): a note() with 5 placeholders and 4 values ended a lane run in a TypeError
after its --lane-check-only said SETUP-ONLY OK. The gate makes that a SETUP-FAIL source at collection.
"""
import glob
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
import fmtcheck  # noqa: E402

# (the line from test_gate_probe_0121.py c53046a6f:108, as it ran)
PROBE_BUG = '''s = "B +%d after Complete: Get_Epona %s, %s %r, lines %s" % (
    (i + 1) * 30, pair.flag_read("B", GET_EPONA).get("value"), pair.flag_read("B", HORSE).get("value"),
    quest_lines(pair, "B", pos))
'''

BAD_TEST = '''def test_bad():
    x = "%s and %s" % ("one",)
'''


class Count(unittest.TestCase):
    def test_counts(self):
        self.assertEqual(fmtcheck.count_values("a %s b %r c %d"), 3)
        self.assertEqual(fmtcheck.count_values("100%% done %s"), 1)
        self.assertEqual(fmtcheck.count_values("%.1f m %-5s %05.2f"), 3)
        self.assertEqual(fmtcheck.count_values("%*d"), 2)
        self.assertEqual(fmtcheck.count_values("%*.*f"), 3)
        self.assertEqual(fmtcheck.count_values("no specs"), 0)
        self.assertIsNone(fmtcheck.count_values("%(name)s"))

    def test_probe_bug_found(self):
        p = fmtcheck.problems(PROBE_BUG)
        self.assertEqual(len(p), 1, p)
        self.assertIn("takes 5 value(s), given 4", p[0][1])

    def test_ok_forms_pass(self):
        src = '''a = "%s %s" % (1, 2)
b = "%s" % x
c = "%s %s" % x
d = "%s %s" % (*xs,)
e = "%(a)s" % {"a": 1}
f = "%d%%" % 5
g = "%s" % [1, 2]
'''
        self.assertEqual(fmtcheck.problems(src), [])

    def test_single_literal_counted(self):
        self.assertEqual(len(fmtcheck.problems('a = "%s %s" % "x"\n')), 1)
        self.assertEqual(len(fmtcheck.problems('a = "%s" % (1, 2)\n')), 1)

    def test_repo_tests_clean(self):
        files = glob.glob(os.path.join(INGAME, "*.py")) + glob.glob(os.path.join(os.path.dirname(INGAME), "*.py"))
        self.assertEqual(fmtcheck.check_files(files), [])


class Gate(unittest.TestCase):
    """bt_plugin collection: --lane-check-only -> SETUP-FAIL source + setup.json ok false; a run -> usage error."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        with open(os.path.join(self.tmp, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(self.tmp, "test_fmt_bad.py"), "w", encoding="utf-8") as f:
            f.write(BAD_TEST)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_pytest(self, *args):
        return subprocess.run([sys.executable, "-m", "pytest", "test_fmt_bad.py", "--fake", "-q", "-p",
                               "no:cacheprovider", "--out", os.path.join(self.tmp, "o")] + list(args),
                              cwd=self.tmp, capture_output=True, text=True, timeout=300)

    def test_lane_check_only_fails(self):
        p = self.run_pytest("--lane-check-only")
        self.assertNotEqual(p.returncode, 0, p.stdout[-2000:])
        self.assertIn("SETUP-FAIL source: ", p.stdout)
        self.assertIn("test_fmt_bad.py:2: format string takes 2 value(s), given 1", p.stdout)
        self.assertIn("RESULT: SETUP-FAIL", p.stdout)
        with open(os.path.join(self.tmp, "o", "setup.json"), encoding="utf-8") as f:
            s = json.load(f)
        self.assertFalse(s["ok"])
        self.assertEqual(len(s["source"]), 1)

    def test_run_fails_at_collection(self):
        p = self.run_pytest()
        self.assertNotEqual(p.returncode, 0, p.stdout[-2000:])
        self.assertIn("SETUP-FAIL source: ", p.stdout + p.stderr)
        self.assertNotIn("1 passed", p.stdout)


if __name__ == "__main__":
    unittest.main()
