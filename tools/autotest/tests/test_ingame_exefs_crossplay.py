"""Selftests of the ExeFS cross-play test (ingame/test_exefs_crossplay.py), no game and no emulator.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402  (puts ingame/ on sys.path)
import test_exefs_crossplay as X   # noqa: E402
from pair import SetupFail   # noqa: E402

# lines as the real programs write them (server/session.cpp:553, switch/proto/main.cpp:288/190/232,
# switch/net/coop_join.cpp:245/253/261)
SERVER = ["listening on port 27520",
          "player 1 joined: A (client: hookshot 0.12.1)",
          "player 2 joined: B (client: hookshot 0.12.1)",
          "player 3 joined: emulator (client: exefs-proto subsdk1 (Fritchii))"]
MODULE = ["[ 1.0] Debug.Emulated <Info> coop-proto: hello from subsdk1 (ExeFS prototype)",
          "[ 1.1] Debug.Emulated <Info> coop-proto: hook 0: frame hook in at main+0x1234",
          "[ 2.0] Debug.Emulated <Info> coop-proto: game value: scene Field/F_Kakariko at frame 40",
          "[30.0] Debug.Emulated <Info> coop-join: WELCOME: I am player 3, tick rate 30",
          "[30.0] Debug.Emulated <Info> coop-join: in the session: player 1 'A'",
          "[30.0] Debug.Emulated <Info> coop-join: in the session: player 2 'B'"]


class Helpers(unittest.TestCase):
    def test_server_nicks(self):
        self.assertEqual(X.server_nicks(SERVER[:3]), {1: "A", 2: "B"})
        self.assertEqual(sorted(set(X.server_nicks(SERVER).values())), ["A", "B", "emulator"])

    def test_judge_all_pass(self):
        checks = X.judge(MODULE, SERVER, ["A", "B"])
        self.assertTrue(all(ok for _, ok in checks), checks)
        self.assertTrue(X.join_done(MODULE, ["A", "B"]))

    def test_judge_refused_and_missing(self):
        ml = MODULE[:3] + ["coop-join: REFUSED reason 6 (plain)"]
        bad = [n for n, ok in X.judge(ml, SERVER[:3], ["A", "B"]) if not ok]
        self.assertIn("module got WELCOME", bad)
        self.assertIn("module never refused/failed", bad)
        self.assertIn("server: emulator joined (client: exefs-proto ...)", bad)
        self.assertTrue(X.join_done(ml, ["A", "B"]))     # (a refusal ends the wait)

    def test_join_not_done_until_both_seen(self):
        self.assertFalse(X.join_done(MODULE[:5], ["A", "B"]))

    def test_module_lines_newest_log(self):
        d = tempfile.mkdtemp()
        try:
            old, new = os.path.join(d, "old.txt"), os.path.join(d, "fritchii_log.txt")
            with open(old, "w") as f:
                f.write("coop-join: REFUSED reason 1 (plain)\n")
            with open(new, "w") as f:
                f.write("\n".join(["noise"] + MODULE) + "\n")
            os.utime(old, (1, 1))
            self.assertEqual(len(X.module_lines(d)), len(MODULE))
            self.assertEqual(X.module_lines(os.path.join(d, "none")), [])
        finally:
            shutil.rmtree(d, ignore_errors=True)


class PrepareUser(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.src = os.path.join(self.tmp, "kit", "emulator")
        for d in X.USER_DIRS:
            os.makedirs(os.path.join(self.src, "user", d))
        user = os.path.join(self.src, "user")
        with open(os.path.join(self.src, "user", "config", "qt-config.ini"), "w") as f:
            f.write("nand_directory=%s\\nand\nsdmc=%s/sdmc\nesc=%s\n"
                    % (user, user.replace("\\", "/"), user.replace("\\", "\\\\")))
        with open(os.path.join(self.src, "user", "keys", "prod.keys"), "w") as f:
            f.write("k")
        self.emu = os.path.join(self.tmp, "Fritchii")
        os.makedirs(os.path.join(self.emu, "user", "log"))
        with open(os.path.join(self.emu, "user", "log", "fritchii_log.txt"), "w") as f:
            f.write("old")
        os.makedirs(os.path.join(self.emu, "user", "load", X.TITLE, "other-mod"))
        self.nso = os.path.join(self.tmp, "subsdk1")
        with open(self.nso, "wb") as f:
            f.write(b"NSO0module")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_copies_rewrites_installs(self):
        installed, log_dir, notes = X.prepare_user(self.emu, self.src, self.nso)
        user = os.path.join(self.emu, "user")
        self.assertTrue(os.path.isfile(os.path.join(user, "keys", "prod.keys")))
        with open(os.path.join(user, "config", "qt-config.ini")) as f:
            text = f.read()
        self.assertNotIn(os.path.abspath(self.src).lower(), text.lower())
        self.assertNotIn(os.path.abspath(self.src).replace("\\", "/").lower(), text.lower())
        self.assertIn(os.path.abspath(user), text)
        self.assertEqual(os.listdir(os.path.join(user, "load", X.TITLE)), ["coop-proto"])   # (only our mod)
        with open(installed, "rb") as f:
            self.assertEqual(f.read(), b"NSO0module")
        self.assertEqual(os.listdir(log_dir), [])
        self.assertTrue(any("kit paths" in n for n in notes))

    def test_kit_path_left_is_setup_fail(self):
        os.makedirs(os.path.join(self.emu, "user", "config"))   # (already there: not copied, not rewritten from user\)
        with open(os.path.join(self.emu, "user", "config", "x.ini"), "w") as f:
            f.write("dir=%s\\other\n" % self.src)
        with self.assertRaises(SetupFail):
            X.prepare_user(self.emu, self.src, self.nso)


class BuildModule(unittest.TestCase):
    def setUp(self):
        self.out = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.out, ignore_errors=True)

    def fake(self, proto, nso=True, code=0):
        def run(argv, **kw):
            self.argv = argv
            mod = argv[argv.index("--out") + 1]
            os.makedirs(mod, exist_ok=True)
            with open(os.path.join(mod, "coop_join_config.h"), "w") as f:
                f.write("#define COOP_PROTOCOL_VERSION %d\n" % proto)
            if nso:
                with open(os.path.join(mod, "subsdk1"), "wb") as f:
                    f.write(b"x")
            return subprocess.CompletedProcess(argv, code)
        return run

    def test_ok(self):
        nso = X.build_module(self.out, "53d40ab3", 27520, "autotest-pass", "18", run=self.fake(18))
        self.assertTrue(nso.endswith("subsdk1"))
        self.assertEqual(self.argv[self.argv.index("--join") + 1], "127.0.0.1:27520")
        self.assertEqual(self.argv[self.argv.index("--protocol-from") + 1], "53d40ab3")
        self.assertEqual(self.argv[self.argv.index("--name") + 1], X.NICK)

    def test_wrong_protocol_no_nso_bad_exit(self):
        for run in (self.fake(17), self.fake(18, nso=False), self.fake(18, code=1)):
            with self.assertRaises(SetupFail):
                X.build_module(self.out, "53d40ab3", 27520, "pw", "18", run=run)


class Collects(unittest.TestCase):
    def test_deselected_without_opt_in(self):
        out = tempfile.mkdtemp()
        p = T.pytest_run(["-p", "no:cacheprovider", "-q", "--fake", "--out", out, "test_exefs_crossplay.py"])
        shutil.rmtree(out, ignore_errors=True)
        self.assertIn("needs_env %s" % X.OPT_IN, p.stdout + p.stderr)

    def test_fake_lane_is_setup_fail_never_pass(self):
        out = tempfile.mkdtemp()
        try:
            p = T.pytest_run(["-p", "no:cacheprovider", "-q", "--fake", "--no-rerun", "--out", out,
                              "--env", X.OPT_IN, "test_exefs_crossplay.py"],
                             env={"OOT_TEST_FRITCHII": out, "OOT_TEST_NUP": out, "OOT_TEST_FRITCHII_FROM": out})
            self.assertEqual(T.results(T.summary(out)),
                             {"test_exefs_emulator_joins_longshot_session": "SETUP-FAIL"}, p.stdout[-2000:])
        finally:
            shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
