"""Selftests of typing-blink-0121's bridge test (ingame/test_typing_blink.py + ingame/fakes/chat_typing.py), no game:
fake bridges. Owner typing-blink-0121. Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results

TEST = "test_typing_line_steady"


class TypingBlink(unittest.TestCase):
    """NO-REF, then PASS once refs are adopted; FAIL when the banner is opened again while it shows, or the game shuts
    it and it is opened again (fakes/chat_typing.py BT_FAKE_TYPING); SETUP-FAIL on a module without the banner state."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_it(self, tag, env=None):
        out = os.path.join(self.tmp, tag)
        p = pytest_run(["test_typing_blink.py", "--fake", "--no-rerun", "--out", out, "-q", "-p", "no:cacheprovider"],
                       env=env)
        return p, summary(out)

    def refs(self):
        p, s = self.run_it("first")
        self.assertEqual(results(s), {TEST: "NO-REF"}, p.stdout[-2000:])
        new = os.path.join(self.tmp, "first", "lane1", "new")
        self.assertEqual(sorted(os.listdir(new)),
                         sorted("typing-banner-%d-%s.png" % (n, x) for n in (1, 2) for x in "AB"))
        refs = os.path.join(self.tmp, "refs")
        os.makedirs(refs)
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(refs, f))
        return refs

    def test_no_ref_then_pass_then_blinks_fail(self):
        refs = self.refs()
        p, s = self.run_it("ok", env={"BT_FAKE_REFS": refs})
        self.assertEqual(results(s), {TEST: "PASS"}, p.stdout[-2000:])
        for bug in ("reopen", "shut", "type"):
            p, s = self.run_it("bug-" + bug, env={"BT_FAKE_REFS": refs, "BT_FAKE_TYPING": bug})
            self.assertEqual(results(s), {TEST: "FAIL"}, p.stdout[-2000:])
            self.assertIn("the typed line blinked", p.stdout)

    def test_setup_fail_without_the_banner_state(self):
        p, s = self.run_it("nostate", env={"BT_FAKE_TYPING": "nostate"})
        self.assertEqual(results(s), {TEST: "SETUP-FAIL"}, p.stdout[-2000:])


if __name__ == "__main__":
    unittest.main()
