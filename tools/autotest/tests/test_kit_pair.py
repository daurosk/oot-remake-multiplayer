"""Tests for the autotest's kit mode: tools/autotest/kit_pair.py (a kit unzipped twice, pointed at a lane's games;
the lane backed up and put back), hookshot.py's kit start (play.ps1 choose -Hookshot, the game's process id) and
groups.py --kit. Fake folders in a temp dir; no game and no PowerShell are started."""
import io
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import groups  # noqa: E402
import hookshot as hs  # noqa: E402
import kit_pair  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
PLAY = os.path.join(REPO, "tools", "release", "kit", "scripts", "play.ps1")


def write(path, text="x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


# The two lines of a pre-0.11.1 play.ps1 that the hook changes, with what is around them.
OLD_PLAY = ("    Set-KitOptionVariables\r\n" + kit_pair.HOOK_EDITS[0][0].replace("\n", "\r\n") +
            "        Remove-Item -Path \"env:$v\" -ErrorAction SilentlyContinue\r\n    }\r\n" +
            kit_pair.HOOK_EDITS[1][0].replace("\n", "\r\n") + "    Write-Host 'Game started.'\r\n")


class Hook(unittest.TestCase):
    def test_old_play_gets_the_hook_once(self):
        t = kit_pair.add_hook(OLD_PLAY)
        self.assertIn("OOT_COOP_TEST_KIT", t)
        self.assertIn("if ($testKit -and $v -in 'LICHII_COOP_SERVER', 'LICHII_COOP_PASSWORD') { continue }\r\n", t)
        self.assertIn("WriteAllText($testKit", t)
        self.assertNotIn("[void](Start-Process", t)
        self.assertNotIn("\n", t.replace("\r\n", ""))   # its CRLF kept
        self.assertEqual(kit_pair.add_hook(t), t)

    def test_unknown_play_is_refused(self):
        with self.assertRaises(kit_pair.Fail):
            kit_pair.add_hook("Write-Host 'some other start'\n")

    def test_the_repo_play_has_the_same_hook(self):
        # (kit_pair puts into an old kit what the kit's own play.ps1 does since 0.11.1)
        t = read(PLAY).replace("\r\n", "\n")
        for _, new in kit_pair.HOOK_EDITS:
            for line in new.splitlines():
                self.assertIn(line.strip(), t)


def without_no_activate(text):
    """The repo's play.ps1 as a kit of 0.11.1 has it: the hook, but the title-mode start is the plain Start-Process."""
    t = text.replace("\r\n", "\n").replace(kit_pair.start_function(), "")
    a = t.index("    if ($testKit) {   # (the autotest's kit mode")
    b = t.index("    }\n", t.index("    } else {\n", a)) + len("    }\n")
    return t[:a] + "    $game = Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -PassThru\n" + t[b:]


class NoActivate(unittest.TestCase):
    """kit_pair puts the no-activation test-mode start into EVERY kit, not only those without the hook."""

    def test_a_kit_with_the_hook_gets_the_start_and_equals_the_repo_play(self):
        repo = read(PLAY).replace("\r\n", "\n")
        old = without_no_activate(repo)
        self.assertNotIn("Start-KitTestGame", old)
        self.assertIn("OOT_COOP_TEST_KIT", old)      # (a released 0.11.1+ kit: the hook is there)
        self.assertEqual(kit_pair.add_hook(old), old)
        fn = kit_pair.start_function()
        patched = kit_pair.add_no_activate(old)
        self.assertEqual(patched.count(fn), 1)
        # (the same start as the kit's own since this change: only the function's place differs, here in the block)
        self.assertEqual(patched.replace(fn, ""), repo.replace(fn, ""))

    def test_once_and_crlf_kept(self):
        old = without_no_activate(read(PLAY)).replace("\n", "\r\n")
        t = kit_pair.add_no_activate(old)
        self.assertIn("Start-KitTestGame $exe", t)
        self.assertNotIn("\n", t.replace("\r\n", ""))
        self.assertEqual(kit_pair.add_no_activate(t), t)
        self.assertEqual(t.count("function Start-KitTestGame"), 1)

    def test_a_kit_without_the_hook_gets_both(self):
        t = kit_pair.add_no_activate(kit_pair.add_hook(OLD_PLAY))
        self.assertIn("OOT_COOP_TEST_KIT", t)
        self.assertIn("function Start-KitTestGame", t)
        self.assertIn("Get-Process -Id (Start-KitTestGame $exe", t)
        self.assertIn("} else {\r\n        $game = Start-Process", t)   # the player's start stays
        self.assertIn("WriteAllText($testKit", t)                         # the pid file is still written

    def test_unknown_start_is_refused(self):
        with self.assertRaises(kit_pair.Fail):
            kit_pair.add_no_activate("$testKit = 1\nWrite-Host 'some other start'\n")


class Pair(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.kit = os.path.join(self.tmp, "coop-kit")
        self.out = io.StringIO()
        self.say = lambda s: self.out.write(s + "\n")
        self.nup = os.path.join(self.tmp, "game.nup")
        write(self.nup)
        for x in "AB":
            g = os.path.join(self.kit, "ls", "lane1-" + x)
            write(os.path.join(g, "longshot.exe"), "exe")
            write(os.path.join(g, "hookshot_coop.dll"), "lane module " + x)
            write(os.path.join(g, "coop.ini"), "server = 127.0.0.1\n")
            write(os.path.join(g, "mods", "contents", "0400486024DEC000", "coop-puppet", "coop_puppet.json"), "lane")
        self.zip = os.path.join(self.tmp, "oot-coop-v9.zip")
        with zipfile.ZipFile(self.zip, "w") as z:
            z.writestr("oot-coop-v9/Start OoT Co-op.bat", "@echo off\r\n")
            z.writestr("oot-coop-v9/hookshot/hookshot_coop.dll", "kit module")
            z.writestr("oot-coop-v9/scripts/play.ps1", OLD_PLAY)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def setup_pair(self, **kw):
        return kit_pair.setup(1, self.zip, self.nup, say=self.say, kit=self.kit, busy=lambda f: [], **kw)

    def test_setup_points_each_kit_at_its_lane_game(self):
        p = self.setup_pair()
        for x in "AB":
            k = p["kit" + x]
            self.assertTrue(os.path.isfile(os.path.join(k, "Start OoT Co-op.bat")))
            self.assertEqual(read(os.path.join(k, "hookshot", "game-folder.txt")).strip(), p[x])
            self.assertEqual(read(os.path.join(k, "hookshot", "game-files.txt")).strip(), self.nup)
            self.assertTrue(hs.same_path(hs.kit_game_folder(k), p[x]))
            self.assertIn("OOT_COOP_TEST_KIT", read(os.path.join(k, "scripts", "play.ps1")))
            self.assertIn("Start-KitTestGame $exe", read(os.path.join(k, "scripts", "play.ps1")))   # no window activation
            self.assertEqual(read(os.path.join(p["backup"], x, "hookshot_coop.dll")), "lane module " + x)

    def test_restore_puts_back_what_the_kit_start_changed(self):
        p = self.setup_pair()
        a = p["A"]
        # what a kit's start does: its module, a new pack, coop.ini's server gone, a randomizer added
        write(os.path.join(a, "hookshot_coop.dll"), "kit module")
        write(os.path.join(a, "mods", "contents", "0400486024DEC000", "coop-puppet", "coop_puppet.json"), "kit")
        write(os.path.join(a, "coop.ini"), "# server = 127.0.0.1\n")
        write(os.path.join(a, "tools", "ootrr", "ootrr.exe"), "rr")
        kit_pair.restore(1, say=self.say, kit=self.kit, busy=lambda f: [])
        self.assertEqual(read(os.path.join(a, "hookshot_coop.dll")), "lane module A")
        self.assertEqual(read(os.path.join(a, "mods", "contents", "0400486024DEC000", "coop-puppet",
                                           "coop_puppet.json")), "lane")
        self.assertEqual(read(os.path.join(a, "coop.ini")), "server = 127.0.0.1\n")
        self.assertFalse(os.path.exists(os.path.join(a, "tools")))   # (not there before the setup)
        self.assertTrue(os.path.isfile(os.path.join(a, "longshot.exe")))
        self.assertFalse(os.path.exists(p["backup"]))

    def test_a_second_setup_waits_for_the_restore(self):
        self.setup_pair()
        with self.assertRaises(kit_pair.Fail):
            self.setup_pair()

    def test_never_while_a_game_runs(self):
        with self.assertRaises(kit_pair.Fail):
            kit_pair.setup(1, self.zip, self.nup, say=self.say, kit=self.kit, busy=lambda f: [f + r"\longshot.exe"])
        self.assertFalse(os.path.exists(kit_pair.paths(1, self.kit)["backup"]))

    def test_dry_run_changes_nothing(self):
        p = self.setup_pair(dry=True)
        self.assertFalse(os.path.exists(p["backup"]) or os.path.exists(p["kitA"]))


class KitStart(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_start_gives_the_game_pid_and_the_test_variables(self):
        seen = {}

        def fake(cmd, cwd, env, input, stdout, stderr, timeout):
            seen.update(cmd=cmd, env=env, input=input)
            stdout.write(b"Game started.\r\n")
            write(env["OOT_COOP_TEST_KIT"], "4321\n")
            return subprocess.CompletedProcess(cmd, 0)
        pid = hs.kit_start(r"C:\kits\kit-A", {"LICHII_COOP_SERVER": "127.0.0.1:27520"}, self.tmp, "A", run_cmd=fake)
        self.assertEqual(pid, 4321)
        self.assertEqual(seen["cmd"][-4:], ["-Mode", "choose", "-Hookshot", "-NoUpdate"])
        self.assertTrue(seen["cmd"][seen["cmd"].index("-File") + 1].endswith(os.path.join("scripts", "play.ps1")))
        self.assertEqual(seen["env"]["LICHII_COOP_SERVER"], "127.0.0.1:27520")
        self.assertEqual(seen["env"]["OOT_COOP_KIT"], r"C:\kits\kit-A")
        self.assertTrue(seen["input"].startswith(b"n\r\n"))   # a long build's question: no
        self.assertIn("Game started.", hs.read_text(os.path.join(self.tmp, "kit-start-A.txt")))

    def test_a_failed_start_says_why(self):
        def fake(cmd, cwd, env, input, stdout, stderr, timeout):
            stdout.write(b"Not now. Start \"Start OoT Co-op.bat\" again when you want to set it up.\r\n")
            return subprocess.CompletedProcess(cmd, 1)
        with self.assertRaises(RuntimeError) as e:
            hs.kit_start(r"C:\kits\kit-A", {}, self.tmp, "A", run_cmd=fake)
        self.assertIn("Not now", str(e.exception))
        self.assertIn("exit 1", str(e.exception))

    def test_an_old_kit_without_the_hook(self):
        def fake(cmd, cwd, env, input, stdout, stderr, timeout):
            return subprocess.CompletedProcess(cmd, 0)
        with self.assertRaises(RuntimeError) as e:
            hs.kit_start(r"C:\kits\kit-A", {}, self.tmp, "A", run_cmd=fake)
        self.assertIn("OOT_COOP_TEST_KIT", str(e.exception))

    @unittest.skipUnless(hs.WINDOWS, "kit mode runs on Windows (elsewhere a killed child stays a zombie until reaped)")
    def test_pid_process_follows_a_real_process(self):
        p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        try:
            w = hs.PidProcess(p.pid)
            self.assertIsNone(w.poll())
            w.kill()
            w.wait(10)
            self.assertIsNotNone(w.poll())
        finally:
            p.kill()
            p.wait()

    def test_fresh_log_ignores_an_older_run(self):
        log = os.path.join(self.tmp, "longshot_log.txt")
        write(log, "precompile old")
        old = time.time() - 600
        os.utime(log, (old, old))
        self.assertEqual(hs.fresh_log(self.tmp, time.time()), "")
        os.utime(log, None)
        self.assertEqual(hs.fresh_log(self.tmp, time.time()), "precompile old")

    def test_run_refuses_a_kit_for_another_game(self):
        kit = os.path.join(self.tmp, "kit-A")
        write(os.path.join(kit, "hookshot", "game-folder.txt"), os.path.join(self.tmp, "other") + "\n")
        a = type("A", (), dict(out=os.path.join(self.tmp, "run"), game_a=os.path.join(self.tmp, "lane1-A"),
                               game_b=os.path.join(self.tmp, "lane1-B"), kit_a=kit, kit_b=None, script_a=None,
                               script_b=None, load=None))()
        with self.assertRaises(SystemExit) as e:
            hs.run(a)
        self.assertIn("game-folder.txt", str(e.exception))


class Groups(unittest.TestCase):
    def test_kit_goes_to_run_ps1(self):
        kit = tempfile.mkdtemp()
        try:
            gs = groups.parse_groups("[g1]\nwalk\n")
            planned = groups.plan(gs, kit, os.path.join(kit, "runs"), "server.exe", lane=1, kits=["C:\\kA", "C:\\kB"])
            cmd = planned[0][2][0]
            self.assertEqual(cmd[cmd.index("-Kit") + 1], "C:\\kA,C:\\kB")
            self.assertIn("-Lane", cmd)
            with self.assertRaises(groups.Fail):
                groups.plan(gs, kit, os.path.join(kit, "runs"), "server.exe", kits=["C:\\kA", "C:\\kB"])
        finally:
            shutil.rmtree(kit, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
