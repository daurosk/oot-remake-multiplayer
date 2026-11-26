"""Row 136's selftest (ingame/test_boss_away.py + ingame/fakes/boss_away.py; owner port-sweep-c131-5), run by
test_ingame.py's load_tests: NO-REF on the fake (look shots, no references) on its own boot, deselected on another boot
or without the row's switches, FAIL under each real bug, NO-REF (VOID) when the setup did not make her or the kill missed."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

BOOT = "live-deku-gohma"
ENV = ["--env", "LICHII_COOP_DEBUG_BOSS=1", "--env", "LICHII_COOP_DEBUG_OBJECTS=1"]


class BossAway(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, boot=BOOT, env_args=ENV, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_BOSS_AWAY_BUG"] = bug
        if boot:
            env["BT_FAKE_BOOT"] = boot
        p = T.pytest_run([".", "--fake", "--out", out] + list(env_args) + (["--boot", boot] if boot else []) +
                         (["--no-rerun"] if bug else []) +
                         ["-q", "-p", "no:cacheprovider", "-k", "test_boss_away"], env=env)
        return p, T.summary(out)

    def result(self, s):
        r = T.results(s).get("test_boss_away_dungeon")
        return [(r, [t["why"] for t in s["tests"] if t["test"].endswith("test_boss_away_dungeon")][0])] if r else []

    def test_fake_no_ref(self):
        p, s = self.run_fake("ok")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "NO-REF", r)
        self.assertIn("no stored reference", r[0][1])

    def test_deselected_other_boot(self):
        p, s = self.run_fake("other", boot=None)
        self.assertEqual(self.result(s), [], s)
        self.assertIn("live-deku-gohma", [d.get("needs_boot") for d in s.get("deselected", [])], s)

    def test_deselected_without_switches(self):
        p, s = self.run_fake("noenv", env_args=[])
        self.assertEqual(self.result(s), [], s)

    def test_bugs_fail(self):
        for bug in ("late", "alive"):
            with self.subTest(bug=bug):
                p, s = self.run_fake(bug, bug=bug)
                r = self.result(s)
                self.assertEqual(r[0][0], "FAIL", r)

    def test_void_is_no_ref(self):
        for bug, why in (("nobody", "no live Enemy_DungeonBoss_Gohma"), ("nodeath", "the kill did not land")):
            with self.subTest(bug=bug):
                p, s = self.run_fake(bug, bug=bug)
                r = self.result(s)
                self.assertEqual(r[0][0], "NO-REF", r)
                self.assertIn(why, r[0][1])


if __name__ == "__main__":
    unittest.main()
