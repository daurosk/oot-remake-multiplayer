"""Row 150's selftest (ingame/test_epona.py + ingame/fakes/epona.py; owner port-sweep-c131-3), run by test_ingame.py's
load_tests: both tests NO-REF on the fake (look shots, no references), deselected without the rows' switches, FAIL
(never PASS / NO-REF) under each bug, and NO-REF (never PASS) when the song brings nobody."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

ROW = ["--env", "LICHII_COOP_PUPPET=1", "--env", "LICHII_COOP_DEBUG_ANIM=1", "--env", "LICHII_COOP_OBJECTS=1"]
BOTH = ("test_epona_sync", "test_epona_seen")


class Epona(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, row=True, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_EPONA_BUG"] = bug
        # a bug run: no FLAKY rerun (the rerun of test_epona_sync on the same boot finds Epona won and SKIPs, and the
        # summary would keep that run's empty why; the rerun rule is bt_plugin's own, tested in test_ingame.py)
        p = T.pytest_run([".", "--fake", "--out", out] + (ROW if row else []) + (["--no-rerun"] if bug else []) + [
            "-q", "-p", "no:cacheprovider", "-k", "test_epona_"], env=env)
        return p, T.summary(out)

    def why(self, s, test):
        return [t["why"] for t in s["tests"] if t["test"].endswith(test)][0]

    def test_ok_is_no_ref(self):
        p, s = self.run_fake("ok")
        self.assertEqual(T.results(s), {t: "NO-REF" for t in BOTH}, p.stdout[-2000:])
        for t in BOTH:                    # NO-REF for the shots only (not a song, mount or get-off reason)
            self.assertIn("no stored reference", self.why(s, t))

    def test_without_the_switches_deselected(self):
        p, s = self.run_fake("none", row=False)
        self.assertEqual(T.results(s), {}, p.stdout[-1500:])
        names = [d["test"] for d in s.get("deselected", [])]
        for t in BOTH:
            self.assertTrue(any(n.endswith(t) for n in names), names)

    def test_second_copy_fails(self):
        p, s = self.run_fake("blink", bug="blink")
        r = T.results(s)
        for t in BOTH:
            self.assertEqual(r[t], "FAIL", p.stdout[-2000:])
            self.assertRegex(self.why(s, t), r"horse copy [2-9] times")

    def test_dropped_horse_fails(self):
        p, s = self.run_fake("gone", bug="gone")
        r = T.results(s)
        for t in BOTH:
            self.assertEqual(r[t], "FAIL", p.stdout[-2000:])

    def test_repeat_round_two_sync_skips(self):
        """review-rows-c130: Get_Epona stays Complete after round 1, so round 2's sync is SKIP (not SETUP-FAIL) and
        seen still runs."""
        out = os.path.join(self.tmp, "rep")
        p = T.pytest_run([".", "--fake", "--out", out] + ROW + ["--repeat", "2", "--no-smoke-gate", "-q", "-p",
                                                                "no:cacheprovider", "-k", "test_epona_"],
                         env={"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")})
        r = T.results(T.summary(out))
        self.assertEqual(r, {"test_epona_sync[r1]": "NO-REF", "test_epona_seen[r1]": "NO-REF",
                             "test_epona_sync[r2]": "SKIP", "test_epona_seen[r2]": "NO-REF"}, p.stdout[-2000:])

    def test_no_song_is_no_ref(self):
        p, s = self.run_fake("nosong", bug="nosong")
        r = T.results(s)
        self.assertEqual(r["test_epona_seen"], "NO-REF", p.stdout[-2000:])
        self.assertIn("song brought no free Epona", self.why(s, "test_epona_seen"))
        self.assertEqual(r["test_epona_sync"], "NO-REF")


if __name__ == "__main__":
    unittest.main()
