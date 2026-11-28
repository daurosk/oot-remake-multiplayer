"""Selftests of the stale menu path check (tools/autotest/ingame/stale_paths.py, B #156): the repo's in-game tests and
fakes have none; each table row finds its planted line; a planted test file is SETUP-FAIL by name at its set-up (no
game input) and --lane-check-only prints it as SETUP-FAIL. Run by test_ingame.py's load_tests (CI) or alone."""
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

INGAME = T.INGAME
sys.path.insert(0, INGAME)
import stale_paths as S   # noqa: E402

PLANTED = {
    "state-coop-menu": '    return (pair.state(x).get("coop") or {}).get("page")\n',
    "plus-menu-multiplayer": '    tap(pair, both, "A", 120)                                 # System\'s Multiplayer button\n',
    "title-host-join-rows": 'ROWS = r"coop: title: the title\'s own rows Host, Join, Co-op Settings"\n',
}
CLEAN = [
    '    return ((pair.state(x).get("menu") or {}).get("coop") or {}).get("page")\n',   # (menu.coop: the right one)
    '    players = (pair.state(x).get("coop") or {}).get("players") or []\n',            # (state.coop.players is real)
    '    tap(pair, both, "PLUS", 240)                               # the game\'s own + pause\n',
]


class StaleTable(unittest.TestCase):
    def test_repo_has_none(self):
        files = [f for f in glob.glob(os.path.join(INGAME, "*.py")) + glob.glob(os.path.join(INGAME, "fakes", "*.py"))
                 if os.path.basename(f) != "stale_paths.py"]
        self.assertGreater(len(files), 50)
        hits = [S.line_of(h) for f in files for h in S.scan_file(f)]
        self.assertEqual(hits, [])

    def test_each_row_finds_its_line(self):
        self.assertEqual(sorted(PLANTED), sorted(r["name"] for r in S.STALE))
        for name, line in PLANTED.items():
            with self.subTest(name=name):
                hits = S.scan_text("x = 1\n" + line, "test_planted.py")
                self.assertEqual([(h["name"], h["line"]) for h in hits], [(name, 2)])
                self.assertIn("stale path %s at test_planted.py:2: " % name, S.line_of(hits[0]))

    def test_right_ways_are_not_hits(self):
        for line in CLEAN:
            with self.subTest(line=line):
                self.assertEqual(S.scan_text(line), [])

    def test_every_row_cites_a_source(self):
        for r in S.STALE:
            self.assertRegex(r["source"], r"^hookshot/src/\S+\.cpp:\d+")
            self.assertTrue(r["why"])


class StaleInARun(unittest.TestCase):
    """A planted test file in its own folder with the plugin (as ingame/conftest.py loads it)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.dir = os.path.join(self.tmp, "suite")
        os.makedirs(self.dir)
        shutil.copy(os.path.join(INGAME, "pytest.ini"), self.dir)
        with open(os.path.join(self.dir, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = [\"bt_plugin\"]\n" % INGAME)
        with open(os.path.join(self.dir, "test_planted.py"), "w", encoding="utf-8") as f:
            f.write("def test_planted(pair):\n    pair.act(\"A\", \"chat hi\")\n" + PLANTED["plus-menu-multiplayer"])

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_setup_fail_by_name(self):
        out = os.path.join(self.tmp, "out")
        p = T.pytest_run(["test_planted.py", "--fake", "--no-rerun", "--out", out, "-q"], cwd=self.dir)
        s = T.summary(out)
        self.assertEqual([t["result"] for t in s["tests"]], ["SETUP-FAIL"], p.stdout[-2000:])
        self.assertIn("stale path plus-menu-multiplayer at test_planted.py:3", s["tests"][0].get("why", ""))

    def test_lane_check_only_says_setup_fail(self):
        out = os.path.join(self.tmp, "chk")
        p = T.pytest_run(["test_planted.py", "--fake", "--lane-check-only", "--out", out, "-q"], cwd=self.dir)
        self.assertNotEqual(p.returncode, 0, p.stdout[-2000:])
        self.assertIn("SETUP-FAIL stale path plus-menu-multiplayer at test_planted.py:3", p.stdout)
        self.assertIn("RESULT: SETUP-FAIL", p.stdout)


if __name__ == "__main__":
    unittest.main()
