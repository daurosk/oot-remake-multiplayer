"""Selftests of C3-1's F1 tests (ingame/test_f1.py + ingame/fakes/f1_event.py) and C6-2's first-night test
(ingame/test_first_night.py + ingame/fakes/first_night.py), no game: fake bridges. Owner port-g7.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results

PAIR_F1 = ("test_f1_title", "test_f1_event", "test_f1_free_play")
SOLO_F1 = ("test_f1_solo_title", "test_f1_solo_event")
NIGHT = "test_first_night_clear_stays_in_its_game"
NIGHT_ENV = ["--env", "LICHII_COOP_DEBUG_TIME=1", "--env", "LICHII_COOP_DEBUG_BOOL=0x8592aa82"]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_it(self, files, tag, env=None, more=()):
        out = os.path.join(self.tmp, tag)
        p = pytest_run(list(files) + ["--fake", "--no-rerun", "--out", out, "-q", "-p", "no:cacheprovider"] +
                       list(more), env=env)
        return p, summary(out)

    def adopt(self, tag):
        """The first run's NO-REF candidates as references (what a person does by hand)."""
        refs = os.path.join(self.tmp, "refs")
        os.makedirs(refs, exist_ok=True)
        new = os.path.join(self.tmp, tag, "lane1", "new")
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(refs, f))
        return refs


class F1(Base):
    """PASS on the fake once refs are adopted (NO-REF before); FAIL where F1 is Longshot's own (the rc-01113 bug) or
    takes Longshot's route in free play (fakes/f1_event.py BT_FAKE_F1); the solo tests only in a --solo run."""

    def refs(self):
        p, s = self.run_it(["test_f1.py"], "first")
        self.assertEqual(results(s), {t: "NO-REF" for t in PAIR_F1}, p.stdout[-2000:])
        names = sorted(os.listdir(os.path.join(self.tmp, "first", "lane1", "new")))
        want = sorted("%s-%s.png" % (n, x) for n in ("f1-event-window", "f1-free", "f1-title-window") for x in "AB")
        self.assertEqual(names, want)
        return self.adopt("first")

    def test_no_ref_then_pass(self):
        refs = self.refs()
        p, s = self.run_it(["test_f1.py"], "ok", env={"BT_FAKE_REFS": refs})
        self.assertEqual(results(s), {t: "PASS" for t in PAIR_F1}, p.stdout[-2000:])
        self.assertEqual(sorted(d["test"].split("::")[-1] for d in s["deselected"]), sorted(SOLO_F1))
        windows = [sh for r in s["tests"] for sh in r["shots"] if sh.get("window")]
        self.assertEqual(len(windows), 4)          # the title and the event, each game: Longshot's menu is in the window only

    def test_fail_when_f1_is_longshots_own(self):
        refs = self.refs()
        for where, test in (("title", "test_f1_title"), ("event", "test_f1_event"), ("free", "test_f1_free_play"),
                            ("freetabs", "test_f1_free_play")):
            p, s = self.run_it(["test_f1.py"], "bug-" + where, env={"BT_FAKE_REFS": refs, "BT_FAKE_F1": where})
            r = results(s)
            self.assertEqual(r[test], "FAIL", p.stdout[-2000:])
            self.assertEqual({t for t, v in r.items() if v == "FAIL"}, {test}, p.stdout[-2000:])

    def test_free_play_not_misled_by_an_earlier_tests_tab_line(self):
        """review-rows-c136 2: after the title and event tests the last tab line still says 2 (Longshot's menu did not
        tick in free play): the free-play test judges only its own lines, so it still PASSes."""
        refs = self.refs()
        p, s = self.run_it(["test_f1.py"], "stale", env={"BT_FAKE_REFS": refs, "BT_FAKE_F1": "stale"})
        self.assertEqual(results(s), {t: "PASS" for t in PAIR_F1}, p.stdout[-2000:])

    def test_solo(self):
        p, s = self.run_it(["test_f1.py"], "solo1", more=["--solo"])
        self.assertEqual(results(s), {t: "NO-REF" for t in SOLO_F1}, p.stdout[-2000:])
        refs = self.adopt("solo1")
        p, s = self.run_it(["test_f1.py"], "solo2", env={"BT_FAKE_REFS": refs}, more=["--solo"])
        self.assertEqual(results(s), {t: "PASS" for t in SOLO_F1}, p.stdout[-2000:])
        p, s = self.run_it(["test_f1.py"], "solo3", env={"BT_FAKE_REFS": refs, "BT_FAKE_F1": "event"}, more=["--solo"])
        self.assertEqual(results(s)["test_f1_solo_event"], "FAIL", p.stdout[-2000:])


class FirstNight(Base):
    """PASS once refs are adopted; FAIL when B's clear reaches A; SETUP-FAIL on a Longshot without the fix;
    deselected without the switches (fakes/first_night.py BT_FAKE_FIRST_NIGHT)."""

    def test_no_ref_then_pass_then_leak_fails(self):
        p, s = self.run_it(["test_first_night.py"], "first", more=NIGHT_ENV)
        self.assertEqual(results(s), {NIGHT: "NO-REF"}, p.stdout[-2000:])
        refs = self.adopt("first")
        p, s = self.run_it(["test_first_night.py"], "ok", env={"BT_FAKE_REFS": refs}, more=NIGHT_ENV)
        self.assertEqual(results(s), {NIGHT: "PASS"}, p.stdout[-2000:])
        p, s = self.run_it(["test_first_night.py"], "leak", env={"BT_FAKE_REFS": refs, "BT_FAKE_FIRST_NIGHT": "leak"},
                           more=NIGHT_ENV)
        self.assertEqual(results(s), {NIGHT: "FAIL"}, p.stdout[-2000:])
        self.assertIn("(was 1)", p.stdout)

    def test_setup_fail_without_the_fix(self):
        p, s = self.run_it(["test_first_night.py"], "nofix", env={"BT_FAKE_FIRST_NIGHT": "nofix"}, more=NIGHT_ENV)
        self.assertEqual(results(s), {NIGHT: "SETUP-FAIL"}, p.stdout[-2000:])

    def test_deselected_without_the_switches(self):
        p, s = self.run_it(["test_first_night.py"], "noenv")
        self.assertEqual(s["tests"], [], p.stdout[-2000:])
        self.assertEqual([d["test"] for d in s["deselected"]], ["test_first_night.py::" + NIGHT])


if __name__ == "__main__":
    unittest.main()
