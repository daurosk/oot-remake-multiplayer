"""Selftests of ingame/test_probe_banner_set.py (revive-flicker-0121 probe 1) against the fake games
(ingame/fakes/dbno_banner.py on fakes/dbno_crawl.py): the probe always ends NO-REF (never PASS); with
BT_FAKE_BANNER_BUG=setblank (B's line set once a second, each set blanking the next SETBLANK_FRAMES shots) it counts
exactly those blank frames after each set and no dark frame without a set; with no bug it counts none. It runs only with
--env BT_PROBE_BANNER_SET=1: a run without it deselects it (the suite counts of test_ingame.Suite never see it)."""
import json
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, os.path.join(INGAME, "fakes"))
import dbno_banner   # noqa: E402

TEST = "test_probe_banner_set"


def run(out, bug=None, env_switch=True):
    """The fake suite on the probe alone (test_ingame_revive_banner.run, plus the probe's --env opt-in)."""
    env = dict(os.environ)
    env.pop("BT_FAKE_BANNER_BUG", None)
    if bug:
        env["BT_FAKE_BANNER_BUG"] = bug
    args = [sys.executable, "-m", "pytest", ".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider",
            "--no-smoke-gate", "-k", TEST]
    if env_switch:
        args += ["--env", "BT_PROBE_BANNER_SET=1"]
    p = subprocess.run(args, cwd=INGAME, capture_output=True, text=True, timeout=600, env=env)
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        s = json.load(f)
    return p, {r["test"].split("::")[-1]: r for r in s["tests"]}, s


def frames(out):
    with open(os.path.join(out, "lane1", "revive-banner", "probe-banner-set", "frames.json"), encoding="utf-8") as f:
        return json.load(f)


class ProbeBannerSet(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import tempfile
        cls.tmp = tempfile.mkdtemp()

    @classmethod
    def tearDownClass(cls):
        import shutil
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_set_blanks_are_counted(self):
        out = os.path.join(self.tmp, "setblank")
        p, res, _ = run(out, bug="setblank")
        self.assertEqual(res[TEST]["result"], "NO-REF", p.stdout[-2000:])
        f = frames(out)
        self.assertEqual(len(f["samples"]), 180)
        self.assertGreaterEqual(len(f["after_set"]), 2, f["after_set"])
        self.assertTrue(all(a["blank_after"] == dbno_banner.SETBLANK_FRAMES for a in f["after_set"][:-1]), f["after_set"])
        self.assertEqual(f["stray_dark"], [])
        self.assertTrue(os.path.exists(os.path.join(out, "lane1", "revive-banner", "probe-banner-set", "f000-B.png")))

    def test_no_blank_without_the_bug(self):
        out = os.path.join(self.tmp, "plain")
        p, res, _ = run(out)
        self.assertEqual(res[TEST]["result"], "NO-REF", p.stdout[-2000:])
        f = frames(out)
        self.assertTrue(all(a["blank_after"] == 0 for a in f["after_set"]), f["after_set"])
        self.assertEqual(f["stray_dark"], [])

    def test_deselected_without_its_switch(self):
        out = os.path.join(self.tmp, "noswitch")
        p = subprocess.run([sys.executable, "-m", "pytest", ".", "--fake", "--collect-only", "-q", "-p", "no:cacheprovider",
                            "--out", out, "-k", TEST], cwd=INGAME, capture_output=True, text=True, timeout=300)
        lines = p.stdout.splitlines()
        self.assertFalse([ln for ln in lines if "::" + TEST in ln and not ln.startswith("deselected")], p.stdout[-2000:])
        self.assertTrue([ln for ln in lines if ln.startswith("deselected") and "BT_PROBE_BANNER_SET=1" in ln and TEST in ln],
                        p.stdout[-2000:])


class Runs(unittest.TestCase):
    """runs() alone (review-tests-153-6): a dark run with no set before or in it is still flagged."""

    @staticmethod
    def samples(*words_sets):
        return [{"i": i, "frame": 100 + i, "words": w, "sets": n, "texts": ["t"] * n} for i, (w, n) in enumerate(words_sets)]

    def runs(self, samples, **kw):
        sys.path.insert(0, INGAME)
        import test_probe_banner_set as probe
        return probe.runs(samples, max(s["words"] for s in samples), **kw)

    def test_a_dark_run_with_no_set_is_flagged(self):
        after, stray = self.runs(self.samples((0.05, 0), (0.0, 0), (0.0, 0)))
        self.assertEqual(stray, [1, 2])
        self.assertEqual(after, [])

    def test_a_set_read_before_the_first_shot_covers_the_first_dark_run(self):
        self.assertEqual(self.runs(self.samples((0.0, 0), (0.0, 0), (0.05, 0)), set_before=True)[1], [])
        self.assertEqual(self.runs(self.samples((0.0, 0), (0.0, 0), (0.05, 0)))[1], [0, 1])

    def test_a_set_on_the_bright_frame_before_covers_its_dark_run(self):
        after, stray = self.runs(self.samples((0.05, 1), (0.0, 0), (0.0, 0), (0.05, 0), (0.0, 0)))
        self.assertEqual(stray, [4])
        self.assertEqual([(a["i"], a["blank_after"]) for a in after], [(0, 2)])


if __name__ == "__main__":
    unittest.main()
