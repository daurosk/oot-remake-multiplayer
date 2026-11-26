"""Selftests of Pair.window_shot and ingame/windowshot.py (harness item 8, C #133 for port-sweep-c131-6's map arrow):
the WINDOW shot (Longshot's ImGui overlay included) through the shared offline/lib/screen-shot.ps1, compared to a ref
like check_shot; every way it can go wrong is SETUP-FAIL, never PASS. No game: the fake lane and a fake helper run.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
AUTOTEST = os.path.dirname(HERE)
sys.path.insert(0, INGAME)
sys.path.insert(0, AUTOTEST)
import pair as P          # noqa: E402
import windowshot         # noqa: E402
import hs_steps           # noqa: E402

WINDOW_TESTS = '''
def test_window(pair):
    pair.window_shot("A", "w")
    pair.window_shot("B", "w")
'''


def pytest_run(args, cwd, env=None):
    e = dict(os.environ)
    e.update(env or {})
    return subprocess.run([sys.executable, "-m", "pytest"] + args, cwd=cwd, capture_output=True, text=True,
                          timeout=600, env=e)


def rows(out):
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        return {r["test"].split("::")[-1]: r for r in json.load(f)["tests"]}


class FakeLaneWindowShot(unittest.TestCase):
    """The runner's labels: no ref = NO-REF (candidate kept), the kept candidate as ref = PASS, covered = SETUP-FAIL."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        d = os.path.join(self.tmp, "t")
        os.makedirs(d)
        with open(os.path.join(d, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(d, "test_w.py"), "w", encoding="utf-8") as f:
            f.write(WINDOW_TESTS)
        self.dir, self.refs = d, os.path.join(self.tmp, "refs")
        os.makedirs(self.refs)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_once(self, name, env=None):
        out = os.path.join(self.tmp, name)
        p = pytest_run([".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider"], self.dir,
                       dict({"BT_FAKE_REFS": self.refs}, **(env or {})))
        return out, rows(out)["test_window"], p

    def test_noref_then_pass_then_covered(self):
        out, r, p = self.run_once("o1")
        self.assertEqual(r["result"], "NO-REF", p.stdout[-2000:])
        for x in "AB":
            cand = os.path.join(out, "lane1", "new", "w-window-%s.png" % x)   # (Pair.out_dir = <out>/lane<N>)
            self.assertTrue(os.path.isfile(cand), cand)
            shutil.copyfile(cand, os.path.join(self.refs, "w-window-%s.png" % x))
        out, r, p = self.run_once("o2")
        self.assertEqual(r["result"], "PASS", p.stdout[-2000:])
        out, r, p = self.run_once("o3", {"BT_FAKE_WINDOW_COVERED": "B"})
        self.assertEqual(r["result"], "SETUP-FAIL", p.stdout[-2000:])
        self.assertIn("COVERED", r["why"])

    def test_window_shot_differs_from_bridge_shot(self):
        """The fake window has the 'overlay' the bridge shot lacks: a bridge-shot ref never passes a window check."""
        out, r, p = self.run_once("o1")
        from PIL import Image
        import fakegame
        g = fakegame.FakeLane(os.path.join(self.tmp, "fl"))
        try:
            bridge = os.path.join(self.tmp, "bridge.png")
            g.g["A"].picture(bridge)
            win = g.window_picture("A", os.path.join(self.tmp, "win.png"))
            with Image.open(bridge) as a, Image.open(win) as b:
                self.assertNotEqual(a.convert("RGB").tobytes(), b.convert("RGB").tobytes())
        finally:
            g.close()


class Shooter(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.session = os.path.join(self.tmp, "session")
        os.makedirs(self.session)
        with open(os.path.join(self.session, "pids.json"), "w", encoding="utf-8") as f:
            json.dump({"games": {"A": 4242, "B": None}, "server": 7}, f)
        self.helper = os.path.join(self.tmp, "O O T", "screen-shot.ps1")     # (a space in the path, like 'OOT REMAKE')
        os.makedirs(os.path.dirname(self.helper))
        open(self.helper, "w").close()
        self.png = os.path.join(self.tmp, "out", "window", "w-window-A.png")
        self.cmds = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fake_run(self, code, text, write=True, size=(400, 300)):
        def run(cmd, **kw):
            from PIL import Image
            self.cmds.append(cmd)
            if write and code == 0:     # (a window shot: a 'title bar' and 'desktop' border around the picture)
                im = Image.new("RGB", size, (0, 0, 255))
                f = size[0] // 400
                im.paste((200, 0, 0), (8 * f, 31 * f, 392 * f, 292 * f))
                im.save(self.png)
            return subprocess.CompletedProcess(cmd, code, text, "")
        return run

    @staticmethod
    def bounds(pid):                     # (window rect, client rect on screen: an 8 px frame, a 23 px title bar)
        return (100, 50, 500, 350), (108, 81, 492, 342)

    def shooter(self, session=None, helper=None, run=None, bounds=None):
        return windowshot.Shooter(session or self.session, helper or self.helper, run=run,
                                  bounds=bounds or self.bounds)

    def test_saved(self):
        s = self.shooter(run=self.fake_run(0, "saved " + self.png))
        self.assertEqual(s("A", self.png), self.png)
        from PIL import Image
        with Image.open(self.png) as im:          # (cut to the client area: only the picture is left)
            self.assertEqual(im.size, (384, 261))
            self.assertEqual(im.convert("RGB").getcolors(), [(384 * 261, (200, 0, 0))])
        cmd = self.cmds[0]
        self.assertEqual(cmd, hs_steps.window_shot_command(self.helper, 4242, self.png,
                                                           os.path.join(os.path.dirname(self.png), windowshot.LOG)))
        self.assertIn("Get-Process -Id 4242", cmd[-1])

    def test_every_fault_is_a_setup_fail(self):
        cases = [
            (self.shooter(helper=os.path.join(self.tmp, "none.ps1"), run=self.fake_run(0, "")), "A",
             "no helper file"),
            (self.shooter(run=self.fake_run(0, "")), "B", "no pid for game B"),
            (self.shooter(session=os.path.join(self.tmp, "nosession"), run=self.fake_run(0, "")), "A",
             "no pids.json"),
            (self.shooter(run=self.fake_run(3, "COVERED x: point (1,2) ...")), "A",
             "exit 3: COVERED"),
            (self.shooter(run=self.fake_run(4, "no window handle")), "A", "exit 4"),
            (self.shooter(run=self.fake_run(0, "saved", write=False)), "A",
             "not on disk"),
            (self.shooter(run=self.fake_run(0, "saved"), bounds=lambda pid: None), "A", "no visible window"),
            (self.shooter(run=self.fake_run(0, "saved"), bounds=lambda pid: ((0, 0, 400, 300), (0, 0, 500, 300))),
             "A", "does not fit"),
        ]
        for s, x, why in cases:
            with self.assertRaises(P.SetupFail) as e:
                s(x, self.png)
            self.assertIn(why, str(e.exception))

    def test_hang_is_a_setup_fail_of_the_pair_type(self):
        def hang(cmd, **kw):
            raise subprocess.TimeoutExpired(cmd, kw.get("timeout"))
        with self.assertRaises(P.SetupFail) as e:          # (not hs_steps.SetupFail: bt_plugin judges P.SetupFail)
            self.shooter(run=hang)("A", self.png)
        self.assertIn("did not end", str(e.exception))

    def test_crop_scales_a_dpi_virtualised_rect(self):
        """This process's rects at 100% while the png is 200% (powershell DPI-aware): the same pixels are cut."""
        s = self.shooter(run=self.fake_run(0, "saved", size=(800, 600)))
        s("A", self.png)
        from PIL import Image
        with Image.open(self.png) as im:
            self.assertEqual(im.size, (768, 522))
            self.assertEqual(len(im.convert("RGB").getcolors()), 1)

    def test_one_env_name_and_default_helper(self):
        self.assertEqual(windowshot.ENV, hs_steps.WINDOW_SHOT_ENV)
        self.assertTrue(windowshot.default_helper().replace("\\", "/").endswith(
            "/coord-multiplayer/offline/lib/screen-shot.ps1"))

    def test_pair_without_shooter_is_a_setup_fail(self):
        p = P.Pair({}, out_dir=self.tmp)
        with self.assertRaises(P.SetupFail):
            p.window_shot("A", "w")


if __name__ == "__main__":
    unittest.main()
