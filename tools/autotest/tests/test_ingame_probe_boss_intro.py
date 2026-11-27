"""Selftests of the boss-intro probe (ingame/test_probe_boss_intro.py), no game: fake bridges (fakes/boss_awake.py gives
TwinrovaStage's arrival event, 300 frames). Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ingame"))

TEST = "test_probe_boss_intro.py::test_probe_twinrova_intro_frames"
ENV = ["--env", "LICHII_COOP_DEBUG_BOSS=1", "--env", "LICHII_COOP_DEBUG_OBJECTS=1", "--env", "LICHII_COOP_CHEATS=1"]


class ProbeBossIntro(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_probe(self, more):
        out = os.path.join(self.tmp, "o")
        p = T.pytest_run(["test_probe_boss_intro.py", "--fake", "--no-rerun", "--out", out, "-q"] + more)
        return p, T.summary(out), out

    def test_no_ref_with_each_games_end_frame(self):
        p, s, out = self.run_probe(ENV + ["--env", "BT_PROBE_BOSS_INTRO=1"])
        self.assertEqual(T.results(s), {"test_probe_twinrova_intro_frames": "NO-REF"}, p.stdout[-2000:])
        why = s["tests"][0]["why"]
        self.assertRegex(why, r"A: event over after [0-9]+ stepped frames .*B: event over after [0-9]+ stepped frames")
        found = [os.path.join(d, f) for d, _, fs in os.walk(out) for f in fs if f == "probe-boss-intro.json"]
        self.assertEqual(len(found), 1, found)
        with open(found[0], encoding="utf-8") as f:
            rec = json.load(f)
        for x in ("A", "B"):
            g = rec["games"][x]
            self.assertEqual(g["after_own_warp"]["scene"], "TwinrovaStage")
            self.assertTrue(g["samples"][0]["event"], g["samples"][:2])
            self.assertFalse(g["ended"]["event"])
            self.assertLessEqual(g["ended"]["stepped"], 300)
        self.assertNotIn("error", rec)

    def test_ended_waits_for_a_fresh_sample(self):
        """review-rows-c130-8 S3: 'ended' is the boot proof's own in play (not_ready None too), not event False."""
        p, s, out = self.run_probe(ENV + ["--env", "BT_PROBE_BOSS_INTRO=1"])
        found = [os.path.join(d, f) for d, _, fs in os.walk(out) for f in fs if f == "probe-boss-intro.json"]
        with open(found[0], encoding="utf-8") as f:
            rec = json.load(f)
        for x in ("A", "B"):
            self.assertIsNone(rec["games"][x]["ended"]["not_ready"])
        import test_probe_boss_intro as probe

        class Fake:
            def in_play(self, st):
                return not st.get("event") and st.get("not_ready") is None
        self.assertFalse(probe._ready(Fake(), "A", {"event": False, "not_ready": "stale"}))
        self.assertTrue(probe._ready(Fake(), "A", {"event": False, "not_ready": None}))
        fake = Fake()
        fake.ready = lambda x, st: False             # (pause-press-0121's Pair.ready, e.g. the play UI down)
        self.assertFalse(probe._ready(fake, "A", {"event": False, "not_ready": None}))

    def test_deselected_without_its_switch(self):
        p, s, _ = self.run_probe(ENV)
        self.assertEqual(s["tests"], [], p.stdout[-2000:])
        self.assertIn(TEST, [d["test"] for d in s["deselected"]])


if __name__ == "__main__":
    unittest.main()
