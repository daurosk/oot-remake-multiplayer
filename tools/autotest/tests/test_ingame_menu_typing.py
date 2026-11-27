"""Selftests of row 297 (ingame/test_menu_typing.py) on fake bridges (ingame/fakes/menu_typing.py), no game. Run by
test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py).
Fixed module: the keys come, B cancels: not FAIL (NO-REF: the shot has no reference yet). The 0.12.0 bug (no keys, a
letter typed not shown), typing that never starts, and typing that B does not end: FAIL.
"""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
AUTOTEST = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, AUTOTEST)
sys.path.insert(0, os.path.join(AUTOTEST, "ingame"))
import test_ingame as T   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results
TEST = "test_room_name_keys_on_the_minus_menu"


class MenuTyping(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_it(self, tag, env=None):
        out = os.path.join(self.tmp, tag)
        p = pytest_run(["test_menu_typing.py", "--fake", "--no-rerun", "--out", out, "-q", "-p", "no:cacheprovider"],
                       env=env)
        return p, summary(out)

    def test_keys_come(self):
        p, s = self.run_it("ok")
        self.assertIn(results(s).get(TEST), ("PASS", "NO-REF"), p.stdout[-3000:])

    def test_fail_without_keys(self):   # (the 0.12.0 bug)
        p, s = self.run_it("nokeys", env={"BT_FAKE_TYPING": "nokeys"})
        self.assertEqual(results(s), {TEST: "FAIL"}, p.stdout[-3000:])
        self.assertIn("no typing keys (row 297)", p.stdout)

    def test_fail_when_the_letter_does_not_show(self):   # (the bug's second half: "wasn't showing the text")
        p, s = self.run_it("notext", env={"BT_FAKE_TYPING": "notext"})
        self.assertEqual(results(s), {TEST: "FAIL"}, p.stdout[-3000:])
        self.assertIn("the letter typed shows", p.stdout)

    def test_fail_when_typing_never_starts(self):
        p, s = self.run_it("nostart", env={"BT_FAKE_TYPING": "nostart"})
        self.assertEqual(results(s), {TEST: "FAIL"}, p.stdout[-3000:])

    def test_fail_when_b_does_not_cancel(self):
        p, s = self.run_it("stuck", env={"BT_FAKE_TYPING": "stuck"})
        self.assertEqual(results(s), {TEST: "FAIL"}, p.stdout[-3000:])


if __name__ == "__main__":
    unittest.main()
