"""Row 308's probe selftest (ingame/test_bomb_pocket_308.py + ingame/fakes/bomb_pocket_308.py; owner carry-sync-304),
run by test_ingame.py's load_tests: the set-up (bombs, B's bomb out and down, A at it) holds on the fake; A's pocket of
B's bomb is NO-REF (shots, no references; never PASS); A still holding it, the bomb gone with no count, or put down
after 'press B' is NO-REF (UNKNOWN 2/3, not a FAIL); the thrower's count changed or the bomb left on B's game after the
pocket is FAIL; deselected without the row's switches."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

ROW = ["--env", "LICHII_COOP_DEBUG_OBJECTS=1", "--env", "LICHII_COOP_BOMB_SHARE=1"]
ALL = ("test_bomb_pocket",)


class BombPocket308(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, mode, row=True):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs"), "BT_FAKE_BOMB_308": mode}
        p = T.pytest_run(["test_bomb_pocket_308.py", "--fake", "--out", out, "--no-rerun"] + (ROW if row else []) + [
            "-q", "-p", "no:cacheprovider"], env=env)
        return p, T.summary(out)

    def why(self, s, test):
        return [t["why"] for t in s["tests"] if t["test"].endswith(test)][0]

    def test_pocket_is_no_ref(self):
        p, s = self.run_fake("ok", "ok")
        self.assertEqual(T.results(s), {"test_bomb_pocket": "NO-REF"}, p.stdout[-2000:])
        self.assertIn("no stored reference", self.why(s, "test_bomb_pocket"))

    def test_still_held_is_no_ref(self):
        p, s = self.run_fake("nopocket", "nopocket")
        self.assertEqual(T.results(s), {"test_bomb_pocket": "NO-REF"}, p.stdout[-2000:])
        self.assertIn("A still has the bomb after 'press B'", self.why(s, "test_bomb_pocket"))

    def test_gone_no_count_is_no_ref(self):
        p, s = self.run_fake("boom", "boom")
        self.assertEqual(T.results(s), {"test_bomb_pocket": "NO-REF"}, p.stdout[-2000:])
        self.assertIn("the bomb is gone with A's count 20 -> 20", self.why(s, "test_bomb_pocket"))

    def test_put_down_is_no_ref(self):
        p, s = self.run_fake("down", "down")
        self.assertEqual(T.results(s), {"test_bomb_pocket": "NO-REF"}, p.stdout[-2000:])
        self.assertIn("left A's hands but not into A's pocket", self.why(s, "test_bomb_pocket"))

    def test_left_on_other_game_fails(self):
        p, s = self.run_fake("stays", "stays")
        self.assertEqual(T.results(s), {"test_bomb_pocket": "FAIL"}, p.stdout[-2000:])
        self.assertIn("a bomb is still in the world after A put it away", self.why(s, "test_bomb_pocket"))

    def test_thrower_count_changed_fails(self):
        p, s = self.run_fake("both", "both")
        self.assertEqual(T.results(s), {"test_bomb_pocket": "FAIL"}, p.stdout[-2000:])
        self.assertIn("B's count changed by A's pocket", self.why(s, "test_bomb_pocket"))

    def test_without_the_switch_deselected(self):
        p, s = self.run_fake("none", "ok", row=False)
        self.assertEqual(T.results(s), {}, p.stdout[-1500:])
        names = [d["test"] for d in s.get("deselected", [])]
        for t in ALL:
            self.assertTrue(any(n.endswith(t) for n in names), names)


if __name__ == "__main__":
    unittest.main()
