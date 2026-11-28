"""Selftests of the Phantom Ganon bow probe (ingame/test_probe_pg_bow_287.py), no game: fake bridges (fakes/
phantom_ganon.py's bow mode: ride-outs from three paintings, `give bow` fills 30 arrows, no camera view so no arrow
flies) and its pure helpers. Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ingame"))
import test_probe_pg_bow_287 as B   # noqa: E402

BOOT = "live-phantom-ganon"
NAME = "test_probe_pg_bow"
TEST = "test_probe_pg_bow_287.py::" + NAME
ENV = ["--env", "BT_PROBE_PG_BOW=1"]


class Helpers(unittest.TestCase):
    def test_painting_of(self):
        ps = [{"appear": [10.9, 82.2, -56.1]}, {"appear": [-12.0, 82.2, -50.0]}]
        self.assertEqual(B.painting_of(ps, [11.5, 82.0, -55.0]), 0)
        self.assertEqual(B.painting_of(ps, [-12.0, 90.0, -51.0]), 1)
        self.assertIsNone(B.painting_of(ps, [0.0, 82.2, -70.0]))

    def test_next_target_rotates_known_moments(self):
        ps = [{"appear": [1, 2, 3], "attack": None}, {"appear": [4, 5, 6], "attack": [7, 8, 9]}]
        self.assertEqual([B.next_target(ps, n) for n in range(4)],
                         [(0, "appear"), (1, "appear"), (1, "attack"), (0, "appear")])
        self.assertIsNone(B.next_target([], 0))

    def test_phase2_lines(self):
        pg = B.PG
        lines = ["coop: objects: anim %s 0x1 'Run' (ours)" % pg,
                 "coop: objects: anim %s 0x1 'Fly_Start' (ours)" % pg,
                 "coop: bosses: turns: %s 0x1 attack starts (action verbs 139 0 0 0 0, attributes 0x6)" % pg]
        self.assertEqual(len(B.phase2(lines)), 2)
        self.assertEqual(B.phase2(lines[:1]), [])


class ProbePgBow(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_probe(self, more, env=None):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        p = T.pytest_run(["test_probe_pg_bow_287.py", "--fake", "--no-rerun", "--out", out, "-q", "--boot", BOOT]
                         + more, env=env)
        return p, T.summary(out), out

    def rec(self, out):
        found = [os.path.join(d, f) for d, _, fs in os.walk(out) for f in fs if f == "probe-pg-bow.json"]
        self.assertEqual(len(found), 1, found)
        with open(found[0], encoding="utf-8") as f:
            return json.load(f)

    def test_flow_without_a_view(self):
        """Adult + bow set up, the three paintings and the attack place learnt from the log, every draw 'not aimed'
        (the fake has no camera view), case 2's life-only hits down to 1; NO-REF, never PASS."""
        p, s, out = self.run_probe(ENV)
        self.assertEqual(T.results(s), {NAME: "NO-REF"}, p.stdout[-2000:])
        why = s["tests"][0]["why"]
        self.assertIn("c1: no phase 2", why)
        self.assertIn("c2: life-only hits", why)
        rec = self.rec(out)
        self.assertNotIn("error", rec)
        c1 = rec["cases"]["c1-full-life"]
        self.assertEqual(len(c1["paintings"]), 3, c1["paintings"])
        self.assertTrue(all(p["attack"] == [0.9, 85.1, -63.0] for p in c1["paintings"]), c1["paintings"])
        self.assertEqual(c1["shots"], [])
        self.assertTrue(c1["notes"] and all("not aimed" in n for n in c1["notes"]), c1["notes"][:3])
        self.assertEqual(rec["cases"]["c2-life1-then-arrow"]["test_hits_lives"][-1], 1)

    def test_child_is_setup_fail(self):
        p, s, _ = self.run_probe(ENV, env={"BT_FAKE_PG_CHILD": "1"})
        self.assertEqual(T.results(s), {NAME: "SETUP-FAIL"}, p.stdout[-2000:])
        self.assertIn("not adult", s["tests"][0]["why"])

    def test_deselected_without_its_switch(self):
        p, s, _ = self.run_probe([])
        self.assertEqual(s["tests"], [], p.stdout[-2000:])
        self.assertIn(TEST, [d["test"] for d in s["deselected"]])


if __name__ == "__main__":
    unittest.main()
