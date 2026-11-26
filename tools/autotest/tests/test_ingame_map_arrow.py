"""G2a's selftest (ingame/test_map_arrow.py + ingame/fakes/map_arrow.py; owner port-sweep-c131-6), run by
test_ingame.py's load_tests: NO-REF on the fake with --pack hud (state checks pass; no window shot helper yet, so never
PASS), deselected without --pack hud, FAIL under each real bug, SETUP-FAIL when the pack has no arrow (an empty pack is the harness's own SETUP-FAIL)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

# A stand-in for packs/hud.py (the real one needs the base game's romfs): the two arrow pictures.
MAKER = """import os

def make(out, ctx):
    names = ("map-arrow.png", "map-arrow-fill.png")
    if os.environ.get("BT_FAKE_MAP_ARROW_BUG") == "nopack":
        names = ("map-arrow-fill.png",)     # (an older hud.py: no rim picture, the module loads no arrows)
    files = {}
    for n in names:
        with open(os.path.join(out, n), "w") as f:
            f.write(n)
        files["coop_hud/" + n] = os.path.join(out, n)
    return files
"""


class MapArrow(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.packs, self.tpl = os.path.join(self.tmp, "packs"), os.path.join(self.tmp, "tpl")
        for d in (self.packs, os.path.join(self.tpl, "coop_hud")):
            os.makedirs(d)
        with open(os.path.join(self.packs, "hud.py"), "w", encoding="utf-8") as f:
            f.write(MAKER)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, pack=True, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs"), "BT_PACKS_DIR": self.packs,
               "BT_FAKE_PACK_DIR": self.tpl}
        if bug:
            env["BT_FAKE_MAP_ARROW_BUG"] = bug
        p = T.pytest_run([".", "--fake", "--out", out] + (["--pack", "hud"] if pack else []) +
                         (["--no-rerun"] if bug else []) + ["-q", "-p", "no:cacheprovider", "-k", "test_map_arrow"],
                         env=env)
        return p, T.summary(out)

    def result(self, s):
        r = [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith("test_map_arrow_turns")]
        return r

    def test_fake_no_ref_state_only(self):
        p, s = self.run_fake("ok")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "NO-REF", r)
        self.assertIn("state only", r[0][1])

    def test_deselected_without_pack(self):
        p, s = self.run_fake("nopack-arg", pack=False)
        self.assertEqual(self.result(s), [], s)

    def test_bugs_fail(self):
        for bug in ("still", "noarrow", "nopage"):
            with self.subTest(bug=bug):
                p, s = self.run_fake(bug, bug=bug)
                r = self.result(s)
                self.assertEqual(len(r), 1, (p.stdout, s))
                self.assertEqual(r[0][0], "FAIL", r)

    def test_no_arrow_in_pack_is_setup_fail(self):
        p, s = self.run_fake("nofiles", bug="nopack")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "SETUP-FAIL", r)
        self.assertIn("map arrows", r[0][1])


if __name__ == "__main__":
    unittest.main()
