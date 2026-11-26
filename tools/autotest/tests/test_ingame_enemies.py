"""Rows 230-232's selftest (ingame/test_enemies.py + ingame/fakes/enemies.py; owner port-sweep-c131-4), run by
test_ingame.py's load_tests: each test NO-REF on the fake (look shots, no references) on its own boot, deselected on
another boot or without the rows' switches, FAIL (never PASS / NO-REF) under each bug, SKIP for Meg once she died."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

SCREAM = ["--env", "LICHII_COOP_DEBUG_OBJECTS=1"]
BOSS = ["--env", "LICHII_COOP_DEBUG_BOSS=1", "--env", "LICHII_COOP_DEBUG_OBJECTS=1"]
# each Dead Hand test on its own boot (232 moves his owner for good: review-rows-c130-2)
DH = {"live-deadhand-room": "test_deadhand_grab", "live-deadhand-room-b": "test_deadhand_grab_other_way"}


class Enemies(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, boot=None, row=SCREAM, bug=None, extra=()):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_ENEMY_BUG"] = bug
        if boot:
            env["BT_FAKE_BOOT"] = boot
        p = T.pytest_run([".", "--fake", "--out", out] + list(row) + (["--boot", boot] if boot else []) +
                         (["--no-rerun"] if bug else []) + list(extra) +
                         ["-q", "-p", "no:cacheprovider", "-k", "test_redead or test_meg or test_deadhand"], env=env)
        return p, T.summary(out)

    def why(self, s, test):
        return [t["why"] for t in s["tests"] if t["test"].endswith(test)][0]

    # -- row 230 ---------------------------------------------------------------------------------------------------
    def test_scream_ok_is_no_ref(self):
        p, s = self.run_fake("scream")
        self.assertEqual(T.results(s), {"test_redead_scream": "NO-REF"}, p.stdout[-2000:])
        self.assertIn("no stored reference", self.why(s, "test_redead_scream"))
        names = [d["test"] for d in s.get("deselected", [])]      # the boss rows need their own boots
        for t in ("test_meg_decoys",) + tuple(DH.values()):
            self.assertTrue(any(n.endswith(t) for n in names), names)

    def test_scream_without_the_switch_deselected(self):
        p, s = self.run_fake("none", row=())
        self.assertEqual(T.results(s), {}, p.stdout[-1500:])

    def test_scream_not_shared_fails(self):
        for bug, rx in (("noshare", "did not reach it"), ("nofreeze", "plays 'Wait', not 'Paralysis'")):
            p, s = self.run_fake(bug, bug=bug)
            self.assertEqual(T.results(s)["test_redead_scream"], "FAIL", p.stdout[-2000:])
            self.assertIn(rx, self.why(s, "test_redead_scream"))

    def test_no_scream_is_no_ref(self):
        p, s = self.run_fake("noscream", bug="noscream")
        self.assertEqual(T.results(s)["test_redead_scream"], "NO-REF", p.stdout[-2000:])
        self.assertIn("no shared scream in either round", self.why(s, "test_redead_scream"))

    # -- row 231 ---------------------------------------------------------------------------------------------------
    def test_meg_ok_is_no_ref(self):
        p, s = self.run_fake("meg", boot="live-meg-room", row=BOSS)
        self.assertEqual(T.results(s), {"test_meg_decoys": "NO-REF"}, p.stdout[-2000:])
        self.assertIn("no stored reference", self.why(s, "test_meg_decoys"))

    def test_meg_bugs_fail(self):
        for bug, rx in (("extra", "shows 6 Megs' decoys, the owner A 3"), ("loop", "minions: our copy"),
                        ("twice", "death begun 2 times")):
            p, s = self.run_fake("meg-" + bug, boot="live-meg-room", row=BOSS, bug=bug)
            self.assertEqual(T.results(s)["test_meg_decoys"], "FAIL", p.stdout[-2000:])
            self.assertIn(rx, self.why(s, "test_meg_decoys"))

    def test_meg_repeat_round_two_skips(self):
        p, s = self.run_fake("meg-rep", boot="live-meg-room", row=BOSS, extra=["--repeat", "2", "--no-smoke-gate"])
        self.assertEqual(T.results(s), {"test_meg_decoys[r1]": "NO-REF", "test_meg_decoys[r2]": "SKIP"},
                         p.stdout[-2000:])

    # -- row 232 ---------------------------------------------------------------------------------------------------
    def test_deadhand_ok_is_no_ref(self):
        for i, (boot, t) in enumerate(DH.items()):   # one test per boot: the other is deselected
            p, s = self.run_fake("dh%d" % i, boot=boot, row=BOSS)
            self.assertEqual(T.results(s), {t: "NO-REF"}, p.stdout[-2000:])
            self.assertIn("no stored reference", self.why(s, t))

    def test_deadhand_bugs_fail(self):
        for bug, rx in (("nohandover", "no hand-over to the held game"), ("apart", "at another hand")):
            for i, (boot, t) in enumerate(DH.items()):
                p, s = self.run_fake("dh%d-%s" % (i, bug), boot=boot, row=BOSS, bug=bug)
                self.assertEqual(T.results(s)[t], "FAIL", p.stdout[-2000:])
                self.assertIn(rx, self.why(s, t))

    def test_deadhand_already_owned_is_no_ref(self):
        """B owns him before any grab (a used boot): NO-REF in set-up, never the empty PASS (review-rows-c130-2)."""
        for i, (boot, t) in enumerate(DH.items()):
            p, s = self.run_fake("dh%d-bowns" % i, boot=boot, row=BOSS, bug="bowns")
            self.assertEqual(T.results(s)[t], "NO-REF", p.stdout[-2000:])
            self.assertIn("already owns Enemy_DeadHand before any grab", self.why(s, t))


if __name__ == "__main__":
    unittest.main()
