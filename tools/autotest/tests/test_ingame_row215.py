"""Selftests of row 215's finish test (ingame/test_row215.py + ingame/fakes/row215.py), no game: fake bridges.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results

TEST = "test_twinrova_finish_stays_with_its_owner"


class Row215(unittest.TestCase):
    """PASS when the server keeps Twinrova with her owner in her finish; FAIL when it hands her over (the bug);
    NO-REF when no ask was made or no merge came (never PASS); deselected without the switches or on another boot."""
    ENV = ["--env", "LICHII_COOP_DEBUG_BOSS=1", "--env", "LICHII_COOP_DEBUG_OBJECTS=1", "--env", "LICHII_COOP_CHEATS=1"]
    BOOT = ["--boot", "live-twinrova-room"]

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_row(self, tag, mode=None, more=None):
        out = os.path.join(self.tmp, tag)
        env = {"BT_FAKE_ROW215": mode or "keep"}
        args = self.ENV + self.BOOT if more is None else more
        p = pytest_run(["test_row215.py", "--fake", "--no-rerun", "--out", out, "-q"] + list(args), env=env)
        return p, summary(out)

    def test_pass_when_kept(self):
        p, s = self.run_row("ok", "keep")
        self.assertEqual(results(s), {TEST: "PASS"}, p.stdout[-3000:])

    def test_fail_when_handed_over(self):
        p, s = self.run_row("handover", "handover")
        self.assertEqual(results(s), {TEST: "FAIL"}, p.stdout[-3000:])
        self.assertIn("handed over in its finish", p.stdout)

    def test_no_ref_when_no_ask(self):
        p, s = self.run_row("noask", "noask")
        self.assertEqual(results(s), {TEST: "NO-REF"}, p.stdout[-3000:])

    def test_no_ref_when_no_merge(self):
        p, s = self.run_row("nomerge", "nomerge")
        self.assertEqual(results(s), {TEST: "NO-REF"}, p.stdout[-3000:])

    def test_deselected_without_the_switches(self):
        p, s = self.run_row("none-env", more=self.BOOT)
        self.assertEqual(s["tests"], [], p.stdout[-2000:])

    def test_deselected_on_another_boot(self):
        p, s = self.run_row("kakariko", more=self.ENV)
        self.assertEqual(s["tests"], [], p.stdout[-2000:])


if __name__ == "__main__":
    unittest.main()
