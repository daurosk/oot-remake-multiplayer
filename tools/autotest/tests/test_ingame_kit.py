"""Selftests of the short gate's kit smoke on the bridge (tools/autotest/ingame/test_kit.py, --kit-ls, lane_record.py):
no game."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, INGAME)
import pair as P          # noqa: E402
import bt_plugin as B     # noqa: E402


def make_pair(kit_ls, lane=1, patches="patches=0001 0002"):
    server = os.path.join(kit_ls, "coop_server.exe")
    with open(server, "wb") as f:
        f.write(b"server")
    for x in ("A", "B"):
        d = os.path.join(kit_ls, "lane%d-%s" % (lane, x))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "hookshot_coop.dll"), "wb") as f:
            f.write(b"module")
        with open(os.path.join(d, "coop-built-from.txt"), "w", encoding="utf-8") as f:
            f.write("pin=v3.2.0\n%s\n" % patches)
    return server


SHA = "abcdef0123456789abcdef0123456789abcdef01"


def make_kit(root, module=b"module", commit=SHA, extra=""):
    """A kit folder as tools/release/make_kit_hookshot.ps1 lays it out: hookshot/hookshot_coop.dll + VERSION.txt
    ('co-op repo: <sha> (<branch>)<extra>', 'hookshot_coop.dll: <sha256 first 16> (sha256, first 16)')."""
    import hashlib
    kit = os.path.join(root, "kit")
    os.makedirs(os.path.join(kit, "hookshot"), exist_ok=True)
    with open(os.path.join(kit, "hookshot", "hookshot_coop.dll"), "wb") as f:
        f.write(module)
    d16 = hashlib.sha256(module).hexdigest().upper()[:16]
    with open(os.path.join(kit, "VERSION.txt"), "w", encoding="utf-8-sig") as f:
        f.write("kit: t\nkind: hookshot\nco-op repo: %s (testkit-0120)%s\nhookshot pin: x\n"
                "hookshot_coop.dll: %s (sha256, first 16)\n" % (commit, extra, d16))
    return kit


class LaneRecord(unittest.TestCase):
    def setUp(self):
        self.ls = tempfile.mkdtemp()
        self.server = make_pair(self.ls)
        self.kit = make_kit(self.ls)

    def cli(self, *extra):
        return subprocess.run([sys.executable, "-I", os.path.join(INGAME, "lane_record.py"), self.ls, "--lane", "1",
                               "--server", self.server] + list(extra), capture_output=True, text=True, timeout=60)

    def test_written_record_passes_lane_check(self):
        for x in ("A", "B"):
            P.write_lane_server(os.path.join(self.ls, "lane1-%s" % x), self.kit, self.server, "16", "t")
        info = P.lane_check(self.ls, 1)
        self.assertEqual(info["commit"], SHA)   # the kit's own VERSION.txt commit, no caller argument
        self.assertEqual(info["server"], os.path.abspath(self.server))

    def test_no_patches_line_is_setup_fail(self):
        os.remove(os.path.join(self.ls, "lane1-A", "coop-built-from.txt"))
        with self.assertRaises(P.SetupFail):
            P.write_lane_server(os.path.join(self.ls, "lane1-A"), self.kit, self.server)

    def test_pair_module_not_the_kits_is_setup_fail(self):
        # (review-c123-11 15:25: commit= was the typed --commit checked against itself; now it is tied to the module)
        self.kit = make_kit(self.ls, module=b"another build")
        with self.assertRaises(P.SetupFail) as c:
            P.write_lane_server(os.path.join(self.ls, "lane1-A"), self.kit, self.server)
        self.assertIn("is not the kit's module", str(c.exception))
        self.assertFalse(os.path.exists(os.path.join(self.ls, "lane1-A", "lane-server.txt")))

    def test_kit_record_faults_are_setup_fail(self):
        cases = [("no VERSION.txt", lambda: os.remove(os.path.join(self.kit, "VERSION.txt"))),
                 ("dirty build", lambda: make_kit(self.ls, extra=" with uncommitted changes")),
                 ("short commit", lambda: make_kit(self.ls, commit="abcdef01")),
                 ("no kit module", lambda: os.remove(os.path.join(self.kit, "hookshot", "hookshot_coop.dll")))]
        for name, spoil in cases:
            with self.subTest(name):
                self.kit = make_kit(self.ls)
                spoil()
                with self.assertRaises(P.SetupFail):
                    P.kit_build(self.kit)

    def test_version_dll_line_must_match_kit_module(self):
        with open(os.path.join(self.kit, "hookshot", "hookshot_coop.dll"), "wb") as f:
            f.write(b"module swapped after packaging")
        with self.assertRaises(P.SetupFail) as c:
            P.kit_build(self.kit)
        self.assertIn("hookshot_coop.dll line", str(c.exception))

    def test_record_names_its_kit(self):
        txt = P.write_lane_server(os.path.join(self.ls, "lane1-A"), self.kit, self.server)
        self.assertIn("commit=%s\n" % SHA, txt)
        self.assertIn("kit=%s\n" % os.path.abspath(self.kit), txt)

    def test_cli_stdout_is_one_json_object(self):
        # (C #132: a tool read as JSON prints only the JSON; one REAL run through the same reader as the gate's)
        p = self.cli("--kit", self.kit, "--protocol", "16")
        self.assertEqual(p.returncode, 0, p.stderr)
        v = json.loads(p.stdout)
        self.assertEqual((v["ok"], v["commit"], v["kit"]), (True, SHA, os.path.abspath(self.kit)))

    def test_cli_setup_fail_exit_2(self):
        os.remove(os.path.join(self.ls, "lane1-B", "hookshot_coop.dll"))
        p = self.cli("--kit", self.kit)
        self.assertEqual(p.returncode, 2)
        self.assertFalse(json.loads(p.stdout)["ok"])

    def test_cli_wrong_module_exit_2(self):
        self.kit = make_kit(self.ls, module=b"another build")
        p = self.cli("--kit", self.kit)
        self.assertEqual(p.returncode, 2, p.stderr)
        v = json.loads(p.stdout)
        self.assertFalse(v["ok"])
        self.assertIn("is not the kit's module", v["error"])

    def test_cli_commit_is_not_an_argument(self):
        p = self.cli("--kit", self.kit, "--commit", SHA)   # the old typed --commit is refused, never used
        self.assertEqual(p.returncode, 2)
        self.assertEqual(p.stdout.strip(), "")
        p = self.cli()                                     # no --kit: argparse exit 2, nothing on stdout
        self.assertEqual(p.returncode, 2)
        self.assertEqual(p.stdout.strip(), "")


class KitBoot(unittest.TestCase):
    def cfg(self, kit_ls):
        class Cfg:
            def getoption(self, name):
                return {"env": [], "session_minutes": 60, "boot": B.DEFAULT_BOOT, "kit_ls": kit_ls}[name]
        return Cfg()

    def test_kit_ls_boots_its_own_folders(self):
        args = B.boot_args(self.cfg(r"C:\kit ls"), 1, "srv.exe", "sess")
        self.assertEqual(args[args.index("--game-a") + 1], os.path.join(r"C:\kit ls", "lane1-A"))
        self.assertEqual(args[args.index("--game-b") + 1], os.path.join(r"C:\kit ls", "lane1-B"))
        self.assertIn("--lane", args)   # (the lane's ports, pipe and lock stay the lane's)

    def test_no_kit_ls_boots_the_lane(self):
        args = B.boot_args(self.cfg(None), 1, "srv.exe", "sess")
        self.assertNotIn("--game-a", args)
        self.assertNotIn("--game-b", args)


class KitTestsCollect(unittest.TestCase):
    def test_deselected_without_kit_ls(self):
        out = tempfile.mkdtemp()
        p = subprocess.run([sys.executable, "-m", "pytest", "-p", "no:cacheprovider", "-q", "--fake", "--out", out,
                            "-k", "test_kit", "."], cwd=INGAME, capture_output=True, text=True, timeout=300)
        self.assertIn("deselected: needs_kit (no --kit-ls)", p.stdout + p.stderr)
        self.assertIn("test_kit_overlay_game_look", p.stdout + p.stderr)


if __name__ == "__main__":
    unittest.main()
