"""Selftests of ingame/test_revive_banner.py (revive-flicker-0121) against the fake games (ingame/fakes/dbno_banner.py
on fakes/dbno_crawl.py): the test PASSes only when B's banner line is in every frame shot while A holds A; the 0.12.0
blink (BT_FAKE_BANNER_BUG=blink: the line gone in every other frame) FAILs it; a FAIL leaves the lane as the next rows
expect it. Run by tests/test_ingame.py's load_tests in CI."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
TEST = "test_revive_banner_steady"


def run(out, k, refs=None, bug=None):
    env = dict(os.environ)
    env.pop("BT_FAKE_BANNER_BUG", None)
    if bug:
        env["BT_FAKE_BANNER_BUG"] = bug
    if refs:
        env["BT_FAKE_REFS"] = refs
    p = subprocess.run([sys.executable, "-m", "pytest", ".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider",
                        "--no-smoke-gate", "-k", k], cwd=INGAME, capture_output=True, text=True, timeout=600, env=env)
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        s = json.load(f)
    return p, {r["test"].split("::")[-1]: r for r in s["tests"]}


class ReviveBanner(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.refs = os.path.join(cls.tmp, "refs")
        os.makedirs(cls.refs)
        out = os.path.join(cls.tmp, "first")
        p, res = run(out, TEST, refs=cls.refs)
        cls.first = res
        new = os.path.join(out, "lane1", "new")
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(cls.refs, f))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def out(self, name):
        return os.path.join(self.tmp, name)

    def test_first_run_is_no_ref_not_fail(self):
        self.assertEqual({t: r["result"] for t, r in self.first.items()}, {TEST: "NO-REF"})
        shots = self.first[TEST]["shots"]
        self.assertEqual({sh["game"] for sh in shots}, {"A", "B"})
        self.assertLessEqual(len(shots), 8)

    def test_passes_with_refs_and_keeps_every_frame(self):
        out = self.out("pass")
        p, res = run(out, TEST, refs=self.refs)
        self.assertEqual(res[TEST]["result"], "PASS", p.stdout[-2000:])
        with open(os.path.join(out, "lane1", "revive-banner", "frames.json"), encoding="utf-8") as f:
            frames = json.load(f)
        self.assertGreaterEqual(len(frames["samples"]), 8)
        self.assertTrue(all(s["sets"] == 1 for s in frames["samples"]), frames["samples"])
        self.assertTrue(os.path.exists(os.path.join(out, "lane1", "revive-banner", "f00-B.png")))

    def test_the_blink_fails(self):
        p, res = run(self.out("blink"), TEST, refs=self.refs, bug="blink")
        self.assertEqual(res[TEST]["result"], "FAIL", p.stdout[-2000:])
        self.assertIn("banner blinked", res[TEST]["why"])

    def test_a_fail_leaves_the_lane_for_the_next_rows(self):
        p, res = run(self.out("after"), "%s or test_puppets_spawn or test_teleport_to_player" % TEST, refs=self.refs,
                     bug="blink")
        self.assertEqual(res[TEST]["result"], "FAIL", p.stdout[-2000:])
        self.assertEqual(res["test_puppets_spawn"]["result"], "PASS", p.stdout[-2000:])
        self.assertEqual(res["test_teleport_to_player"]["result"], "PASS", p.stdout[-2000:])


if __name__ == "__main__":
    unittest.main()
