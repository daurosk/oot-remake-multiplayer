"""Selftests of row 286's wake test (ingame/test_boss_wake.py + ingame/fakes/boss_awake.py), no game: fake bridges.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results

TESTS = ("test_twinrova_wake_together", "test_twinrova_wake_late")
LOOKS = 2 * (1 + 4) + 2 * (2 + 4)   # paired looks: together awake + 4 fight; late arrive + awake + 4 fight


class BossWake(unittest.TestCase):
    """Row 286: PASS on the fake pair with the switches (after refs are adopted); FAIL when a late copy, or every copy,
    does not take the owner's fight (fakes/boss_awake.py BT_FAKE_BOSS_WAKE); deselected without the switches."""
    ENV = ["--env", "LICHII_COOP_DEBUG_BOSS=1", "--env", "LICHII_COOP_DEBUG_OBJECTS=1", "--env", "LICHII_COOP_CHEATS=1"]
    BOOT = ["--boot", "live-twinrova-room"]

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_wake(self, tag, env=None, more=()):
        out = os.path.join(self.tmp, tag)
        p = pytest_run(["test_boss_wake.py", "--fake", "--no-rerun", "--out", out, "-q"] + list(more), env=env)
        return p, summary(out)

    def test_no_ref_then_pass_with_refs(self):
        refs = os.path.join(self.tmp, "refs")
        p, s = self.run_wake("first", env={"BT_FAKE_REFS": refs}, more=self.ENV + self.BOOT)
        self.assertEqual(results(s), {t: "NO-REF" for t in TESTS}, p.stdout[-2000:])
        new = os.path.join(self.tmp, "first", "lane1", "new")
        self.assertEqual(len(os.listdir(new)), LOOKS)
        os.makedirs(refs)
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(refs, f))
        p, s = self.run_wake("ok", env={"BT_FAKE_REFS": refs}, more=self.ENV + self.BOOT)
        self.assertEqual(results(s), {t: "PASS" for t in TESTS}, p.stdout[-2000:])
        shots = [sh for r in s["tests"] for sh in r["shots"]]
        self.assertEqual(len(shots), LOOKS)
        self.assertTrue(all(sh["ok"] for sh in shots))

    def test_fail_when_a_late_copy_does_not_wake(self):
        """The reported bug: together passes the log/state judge (NO-REF for its looks), late FAILs."""
        p, s = self.run_wake("late", env={"BT_FAKE_BOSS_WAKE": "late"}, more=self.ENV + self.BOOT)
        r = results(s)
        self.assertEqual(r["test_twinrova_wake_late"], "FAIL", p.stdout[-2000:])
        self.assertNotEqual(r["test_twinrova_wake_together"], "FAIL", p.stdout[-2000:])
        self.assertIn("the owner's verbs came", p.stdout)

    def test_a_long_intro_is_waited_out(self):
        """A stepped real intro is thousands of frames (pair.py BOOT_EVENT_FRAMES, row215-309d7180): with a 2700-frame
        intro (fight lines only after it) both tests wait it out by state and pass the log/state judge (NO-REF: no
        refs here). The old fixed 900-frame wait judged inside the intro: 'the owner's verbs came' not seen -> FAIL."""
        p, s = self.run_wake("long", env={"BT_FAKE_BOSS_INTRO": "2700"}, more=self.ENV + self.BOOT)
        self.assertEqual(results(s), {t: "NO-REF" for t in TESTS}, p.stdout[-3000:])
        notes = " ".join(n for r in s["tests"] for n in r.get("notes", []))
        self.assertIn("together: A+B in play", notes)
        self.assertIn("late A: A in play", notes)

    def test_late_case_is_late_with_a_long_intro(self):
        """With a 2700-frame intro B must still come in AFTER A's intro (the late path): the late bug FAILs late.
        (The old 900-frame gap sent B in during A's intro: it was the together path, not the late one.)"""
        p, s = self.run_wake("long-late", env={"BT_FAKE_BOSS_INTRO": "2700", "BT_FAKE_BOSS_WAKE": "late"},
                             more=self.ENV + self.BOOT)
        r = results(s)
        self.assertEqual(r["test_twinrova_wake_late"], "FAIL", p.stdout[-3000:])
        self.assertNotEqual(r["test_twinrova_wake_together"], "FAIL", p.stdout[-3000:])

    def test_fail_when_no_copy_wakes(self):
        p, s = self.run_wake("copy", env={"BT_FAKE_BOSS_WAKE": "copy"}, more=self.ENV + self.BOOT)
        self.assertEqual(set(results(s).values()), {"FAIL"}, p.stdout[-2000:])

    def test_fail_when_a_game_names_the_server_owner(self):
        p, s = self.run_wake("server", env={"BT_FAKE_BOSS_WAKE": "server"}, more=self.ENV + self.BOOT)
        self.assertEqual(set(results(s).values()), {"FAIL"}, p.stdout[-2000:])
        self.assertIn("says the server owns it", p.stdout)

    def test_deselected_on_another_boot(self):
        p, s = self.run_wake("kakariko", more=self.ENV)
        self.assertEqual(s["tests"], [], p.stdout[-2000:])
        self.assertEqual(sorted(d["test"] for d in s["deselected"]), sorted("test_boss_wake.py::" + t for t in TESTS))

    def test_deselected_without_the_switches(self):
        p, s = self.run_wake("none-env", more=self.BOOT)
        self.assertEqual(s["tests"], [], p.stdout[-2000:])
        self.assertEqual(sorted(d["test"] for d in s["deselected"]), sorted("test_boss_wake.py::" + t for t in TESTS))


if __name__ == "__main__":
    unittest.main()
