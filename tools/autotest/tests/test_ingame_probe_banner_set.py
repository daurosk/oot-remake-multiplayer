"""Selftests of ingame/test_probe_banner_set.py (revive-flicker-0121 probe 1) against the fake games
(ingame/fakes/dbno_banner.py on fakes/dbno_crawl.py): the probe always ends NO-REF (never PASS); with
BT_FAKE_BANNER_BUG=setblank (B's line set once a second, each set blanking the next SETBLANK_FRAMES shots) it counts
exactly those blank frames after each set and no dark frame without a set; with no bug it counts none."""
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_ingame_revive_banner as rbt   # noqa: E402  (run: the fake suite on one -k)

INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, os.path.join(INGAME, "fakes"))
import dbno_banner   # noqa: E402

TEST = "test_probe_banner_set"


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
        p, res = rbt.run(out, TEST, bug="setblank")
        self.assertEqual(res[TEST]["result"], "NO-REF", p.stdout[-2000:])
        f = frames(out)
        self.assertEqual(len(f["samples"]), 180)
        self.assertGreaterEqual(len(f["after_set"]), 2, f["after_set"])
        self.assertTrue(all(a["blank_after"] == dbno_banner.SETBLANK_FRAMES for a in f["after_set"][:-1]), f["after_set"])
        self.assertEqual(f["stray_dark"], [])
        self.assertTrue(os.path.exists(os.path.join(out, "lane1", "revive-banner", "probe-banner-set", "f000-B.png")))

    def test_no_blank_without_the_bug(self):
        out = os.path.join(self.tmp, "plain")
        p, res = rbt.run(out, TEST)
        self.assertEqual(res[TEST]["result"], "NO-REF", p.stdout[-2000:])
        f = frames(out)
        self.assertTrue(all(a["blank_after"] == 0 for a in f["after_set"]), f["after_set"])
        self.assertEqual(f["stray_dark"], [])


if __name__ == "__main__":
    unittest.main()
