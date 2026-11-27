"""Row 295's selftest (ingame/test_map_back_295.py + ingame/fakes/map_back_295.py), no game. Run by test_ingame.py's
load_tests (CI: python tools/autotest/tests/test_ingame.py).
- On the fake: NO-REF (look shots, no references), never PASS without refs.
- BT_FAKE_MAP295=blank (the module before the fix: the Map page's panes never given back after L) and =late (given
  back only past the test's wait): FAIL, naming the Map page wait.
- The state member's name and fields are the module's (coop_menu_state.cpp MapPageJson) and docs/playtest.md's.
"""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
import test_ingame as T   # noqa: E402

NAME = "test_map_back_after_coop_page"


class MapBack295(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def one(self, name, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_MAP295"] = bug
        p = T.pytest_run([".", "--fake", "--out", out] + (["--no-rerun"] if bug else []) +
                         ["-q", "-p", "no:cacheprovider", "-k", "test_map_back_295"], env=env)
        s = T.summary(out)
        r = [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith(NAME)]
        self.assertEqual(len(r), 1, (p.stdout, s))
        return r[0]

    def test_fake_no_ref(self):
        r = self.one("ok")
        self.assertEqual(r[0], "NO-REF", r)

    def test_blank_map_page_fails(self):
        r = self.one("blank", "blank")
        self.assertEqual(r[0], "FAIL", r)
        self.assertIn("the Map page up in full", r[1])

    def test_late_map_page_fails(self):
        r = self.one("late", "late")
        self.assertEqual(r[0], "FAIL", r)
        self.assertIn("the Map page up in full", r[1])

    def test_state_member_matches_module_and_docs(self):
        def read(rel):
            with open(os.path.join(REPO, rel), encoding="utf-8") as f:
                return f.read()
        cpp = read("hookshot/src/coop_menu_state.cpp")
        for field in ("up", "panes", "shown"):
            self.assertIn('\\"%s\\":' % field, cpp, field)
        self.assertIn('\\"map_page\\":', cpp)
        self.assertIn("`map_page` (row 295)", read("docs/playtest.md"))
        test = read("tools/autotest/ingame/test_map_back_295.py")
        self.assertIn('get("map_page")', test)


if __name__ == "__main__":
    unittest.main()
