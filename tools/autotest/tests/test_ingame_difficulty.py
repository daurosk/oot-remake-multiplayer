"""Selftests of difficulty #114's bridge tests (tools/autotest/ingame/test_difficulty.py) on the fake games
(ingame/fakes/difficulty.py), and of the '#! server' boot header they need (hookshot.py header_server_args).
Run by test_ingame.py's load_tests (CI) or alone."""
import glob
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import test_ingame as T   # noqa: E402
import hookshot as hs     # noqa: E402

BOOT = ["--boot", "live-lobby-hard"]
LOBBY, HOST = "test_lobby_difficulty_from_first_player", "test_host_page_difficulty_kept"


def results(s):
    return {t["test"].split("::")[-1].split("[")[0]: t["result"] for t in s["tests"]}


class DifficultyBridgeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_it(self, tag, mode, before=None, left=None):
        out = os.path.join(self.tmp, tag)
        env = {"BT_FAKE_DIFFICULTY": mode, "BT_FAKE_REFS": os.path.join(self.tmp, "refs")}
        if before is not None:
            env["BT_FAKE_DIFFICULTY_BEFORE"] = before
        if left is not None:
            env["BT_FAKE_DIFFICULTY_LEFT"] = left
        p = T.pytest_run(["test_difficulty.py", "--fake", "--no-rerun", "--out", out, "-q"] + BOOT, env=env)
        return p, T.summary(out), out

    def kept_files(self, out):
        return glob.glob(os.path.join(out, "**", "coop-difficulty.ini"), recursive=True)

    def test_kept_file_before_is_setup_fail_and_left_as_it_was(self):
        """A coop-difficulty.ini left by an earlier run (lane1-A/lane2-A 2026-11-26: a kept Custom hp=300, read once at
        game start, the Host page starts there): each set-up is SETUP-FAIL naming the file, before any input, and the
        file is as it was (B #148 / review-rows-c130-6 02:22)."""
        p, s, out = self.run_it("kept", "hard", before="preset = custom\nhp = 300\n")
        self.assertEqual(results(s), {LOBBY: "SETUP-FAIL", HOST: "SETUP-FAIL"}, p.stdout[-2000:])
        self.assertIn("holds a kept difficulty from an earlier run", p.stdout)
        files = self.kept_files(out)
        self.assertEqual(len(files), 1, files)
        with open(files[0], encoding="utf-8") as f:
            self.assertEqual(f.read(), "preset = custom\nhp = 300\n")

    def test_lobby_hard_and_kept_steps_then_no_file_after(self):
        """Hard from the first player and the Host page's kept steps: the state/log judge passes (NO-REF: no refs
        yet), and the file the Host page wrote is gone after (the lane had none before)."""
        p, s, out = self.run_it("ok", "hard")
        self.assertEqual(results(s), {LOBBY: "NO-REF", HOST: "NO-REF"}, p.stdout[-2000:])
        self.assertEqual(self.kept_files(out), [])

    def test_no_kept_file_before_means_none_after(self):
        p, s, out = self.run_it("none", "hard")
        self.assertEqual(results(s)[HOST], "NO-REF", p.stdout[-2000:])
        self.assertEqual(self.kept_files(out), [])

    def test_fails_when_the_lobby_difficulty_is_lost(self):
        """The bug the old row looked for: the server made Hard, the games say Automatic."""
        p, s, _ = self.run_it("auto", "auto")
        self.assertEqual(results(s)[LOBBY], "FAIL", p.stdout[-2000:])
        self.assertIn("coop: difficulty: Hard", p.stdout)

    def test_fails_when_the_idle_host_page_refuses(self):
        p, s, out = self.run_it("refused", "refused")
        self.assertEqual(results(s)[HOST], "FAIL", p.stdout[-2000:])
        self.assertEqual(self.kept_files(out), [])

    def test_out_of_the_session_is_setup_fail(self):
        """review-rows-c136 15:09: a test run after the Host page test (A left the session for good), or a --repeat
        r2, finds A in play but with no connected players: each set-up says SETUP-FAIL with the counts, never a FAIL
        half-way, and nothing of the test runs (A's kept file untouched)."""
        p, s, out = self.run_it("left", "hard", left="A")
        self.assertEqual(results(s), {LOBBY: "SETUP-FAIL", HOST: "SETUP-FAIL"}, p.stdout[-2000:])
        self.assertIn("connected players A=0 B=2", p.stdout)
        self.assertEqual(self.kept_files(out), [])

    def test_deselected_on_the_default_boot(self):
        out = os.path.join(self.tmp, "default")
        T.pytest_run(["test_difficulty.py", "--fake", "--no-rerun", "--out", out, "-q"], env={"BT_FAKE_DIFFICULTY": "hard"})
        self.assertEqual(T.summary(out)["tests"], [])


class ServerHeader(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def script(self, text):
        p = os.path.join(self.tmp, "s.A.txt")
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
        return p

    def test_reads_the_header(self):
        p = self.script("# x\n#! switches A=1\n#! server --difficulty hard\n#!server --pvp on\nwait 1\n")
        self.assertEqual(hs.header_server_args(p), ["--difficulty", "hard", "--pvp", "on"])
        self.assertEqual(hs.header_server_args(None), [])
        self.assertEqual(hs.header_server_args(self.script("wait 1\n")), [])

    def test_the_boot_gives_hard(self):
        boot = os.path.join(os.path.dirname(HERE), "scenarios", "live-lobby-hard.A.txt")
        self.assertEqual(hs.header_server_args(boot), ["--difficulty", "hard"])
        self.assertEqual(hs.header_switches(boot), [("LICHII_COOP_CHEATS", "1")])

    def test_refuses_the_runners_own_and_odd_words(self):
        for bad in ("#! server --port 1\n", "#! server --password-env X\n", "#! server --bind=0.0.0.0\n",
                    "#! server --name a;b\n", "#! server $(x)\n"):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    hs.header_server_args(self.script(bad))


if __name__ == "__main__":
    unittest.main()
