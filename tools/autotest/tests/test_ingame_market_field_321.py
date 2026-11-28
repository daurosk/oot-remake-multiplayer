"""Row 321's selftest (ingame/test_market_field_321.py + ingame/fakes/market_field_321.py; owner crash-sweep-0120-5), run
by test_ingame.py's load_tests: the pair test PASSes on the fake, FAILs when the field's arrival writes the crash line
(BT_FAKE_MARKET_FIELD=crash); the solo test PASSes on the solo fake."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

PAIR = "test_market_field_guest_crosses"
SOLO = "test_market_field_solo"


class MarketField(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, key, bug=None, solo=False, switch=True):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_MARKET_FIELD"] = bug
        p = T.pytest_run([".", "--fake"] + (["--env", "BT_ROW321=1"] if switch else []) + ["--out", out] + (["--solo"] if solo else []) +
                         (["--no-rerun"] if bug else []) + ["-q", "-p", "no:cacheprovider", "-k", key], env=env)
        s = T.summary(out)
        return p, [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith(key)], s

    def test_pair_passes(self):
        p, r, s = self.run_fake("ok", PAIR)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "PASS", r)

    def test_crash_fails(self):
        p, r, s = self.run_fake("crash", PAIR, bug="crash")
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "FAIL", r)
        self.assertIn("CRASH: exception", r[0][1])

    def test_without_switch_deselected(self):
        out = os.path.join(self.tmp, "noenv")
        p = T.pytest_run([".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider", "-k", PAIR],
                         env={"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")})
        self.assertIn("needs_env BT_ROW321=1", p.stdout + p.stderr)

    def test_solo_passes(self):
        p, r, s = self.run_fake("solo", SOLO, solo=True, switch=False)   # (the solo test carries no switch)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "PASS", r)


if __name__ == "__main__":
    unittest.main()
