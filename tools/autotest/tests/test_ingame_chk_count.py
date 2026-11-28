"""Selftest of --lane-check-only's test count (B #158: gate-hilt 23:09 'collected 49', pause-press 23:27 'collected 6 not
2'): with -k or -m, the chk line's 'tests collected N' and setup.json's "collected" are the tests the run gets (after
pytest's own deselection), the same as --collect-only with the same switches. Run by test_ingame.py's load_tests (CI)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_ingame as T   # noqa: E402


def collect_ids(extra):
    out = tempfile.mkdtemp()
    p = T.pytest_run([".", "--fake", "--collect-only", "-q", "--out", out] + extra)
    shutil.rmtree(out, ignore_errors=True)
    return [ln.strip() for ln in p.stdout.splitlines() if "::" in ln and not ln.startswith("deselected")]


class LaneCheckCount(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def chk(self, tag, extra):
        out = os.path.join(self.tmp, tag)
        p = T.pytest_run([".", "--fake", "--lane-check-only", "--out", out, "-q"] + extra)
        self.assertEqual(p.returncode, 0, p.stdout[-2000:])
        with open(os.path.join(out, "setup.json"), encoding="utf-8") as f:
            return p, json.load(f)

    def test_k_and_m_counts_are_the_runs(self):
        everything = len(collect_ids([]))
        for n, extra in enumerate((["-k", "test_difficulty or test_menu_pages"], ["-m", "smoke"],
                                   ["test_smoke.py", "-k", "pause"])):
            with self.subTest(extra=extra):
                want = len(collect_ids(extra))
                self.assertGreater(want, 0)
                self.assertLess(want, everything)
                p, s = self.chk("k%d" % n, extra)
                self.assertEqual(s["collected"], want, p.stdout[-2000:])
                self.assertIn("tests collected %d " % want, p.stdout)
                if extra == ["-m", "smoke"]:
                    self.assertEqual(s["smoke"], want)


if __name__ == "__main__":
    unittest.main()
