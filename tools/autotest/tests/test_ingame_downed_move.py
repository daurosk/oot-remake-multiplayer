"""Selftests of ingame/test_downed_move.py (0.12.1 downed-move) against the fake games (ingame/fakes/downed_move.py
on top of fakes/dbno_crawl.py): the tests PASS only when the downed Link moves under the stick with the room answer
(child) or the lying ground move (adult) and the child's view comes in to the crawl camera; the v0.12.0 bug FAILs,
a game without patch 0046 is a SETUP-FAIL. Also the eye maths on a known matrix. Run by tests/test_ingame.py's
load_tests in CI."""
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
CHILD, ADULT = "test_downed_child_moves", "test_downed_adult_moves"


def run(out, k, refs=None, bug=None):
    env = dict(os.environ)
    env.pop("BT_FAKE_DOWNED_BUG", None)
    env.pop("BT_FAKE_DBNO_BUG", None)
    if bug:
        env["BT_FAKE_DOWNED_BUG"] = bug
    if refs:
        env["BT_FAKE_REFS"] = refs
    p = subprocess.run([sys.executable, "-m", "pytest", "test_downed_move.py", "--fake", "--out", out, "-q", "-p",
                        "no:cacheprovider", "--no-smoke-gate", "-k", k], cwd=INGAME, capture_output=True, text=True,
                       timeout=600, env=env)
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        s = json.load(f)
    return p, {r["test"].split("::")[-1]: r for r in s["tests"]}


class Eye(unittest.TestCase):
    def test_eye_of_a_known_camera(self):
        sys.path.insert(0, INGAME)
        sys.path.insert(0, os.path.join(INGAME, "fakes"))
        import test_downed_move as t
        import downed_move as f
        for at, yaw in (([1.0, 2.0, 3.0], 0.0), ([-5.5, 0.3, 12.0], 137.0)):
            e = t.eye({"view": {"clip": f.clip(at, yaw)}})
            self.assertTrue(all(math.isclose(a, b, abs_tol=1e-9) for a, b in zip(e, at)), (e, at))
        self.assertIsNone(t.eye({"view": None}))
        self.assertIsNone(t.eye({"view": {"clip": [0.0] * 16}}))


class DownedMove(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """References from a first (NO-REF) run, approved here wholesale, so the runs below judge state, not looks."""
        cls.tmp = tempfile.mkdtemp()
        cls.refs = os.path.join(cls.tmp, "refs")
        os.makedirs(cls.refs)
        out = os.path.join(cls.tmp, "first")
        p, res = run(out, "downed", refs=cls.refs)
        cls.first, cls.first_out = res, p.stdout[-3000:]
        new = os.path.join(out, "lane1", "new")
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(cls.refs, f))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def out(self, name):
        return os.path.join(self.tmp, name)

    def test_first_run_is_no_ref_not_fail(self):
        self.assertEqual({t: r["result"] for t, r in self.first.items()}, {CHILD: "NO-REF", ADULT: "NO-REF"},
                         self.first_out)
        for r in self.first.values():
            self.assertEqual({sh["game"] for sh in r["shots"]}, {"A", "B"})
            self.assertLessEqual(len(r["shots"]), 8)

    def test_passes_with_refs(self):
        p, res = run(self.out("pass"), "downed", refs=self.refs)
        self.assertEqual({r["result"] for r in res.values()}, {"PASS"}, p.stdout[-3000:])

    def test_the_v0120_bug_fails_the_child(self):
        p, res = run(self.out("still"), CHILD, refs=self.refs, bug="still")
        self.assertEqual(res[CHILD]["result"], "FAIL", p.stdout[-3000:])

    def test_the_v0120_bug_fails_the_adult(self):
        p, res = run(self.out("still-adult"), ADULT, refs=self.refs, bug="still")
        self.assertEqual(res[ADULT]["result"], "FAIL", p.stdout[-3000:])

    def test_a_flicker_fails_both(self):
        p, res = run(self.out("flicker"), "downed", refs=self.refs, bug="flicker")
        self.assertEqual({t: r["result"] for t, r in res.items()}, {CHILD: "FAIL", ADULT: "FAIL"}, p.stdout[-3000:])
        self.assertIn("flickers", json.dumps(res))

    def test_no_play_skipped_fails_the_adult(self):
        # patch 0049's root fix: no skip line = the game's Wait still replaces the pose (the 89133f1ee flicker)
        p, res = run(self.out("noskip"), ADULT, refs=self.refs, bug="noskip")
        self.assertEqual(res[ADULT]["result"], "FAIL", p.stdout[-3000:])
        self.assertIn("skipped", json.dumps(res[ADULT]))

    def test_no_crawl_camera_fails_the_child(self):
        p, res = run(self.out("farcam"), CHILD, refs=self.refs, bug="farcam")
        self.assertEqual(res[CHILD]["result"], "FAIL", p.stdout[-3000:])
        self.assertIn("crawl camera", json.dumps(res[CHILD]))

    def test_no_patch_is_setup_fail(self):
        p, res = run(self.out("nowrap"), "downed", refs=self.refs, bug="nowrap")
        self.assertEqual({r["result"] for r in res.values()}, {"SETUP-FAIL"}, p.stdout[-3000:])


if __name__ == "__main__":
    unittest.main()
