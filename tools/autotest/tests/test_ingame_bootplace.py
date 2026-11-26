"""Selftests of the default-boot place (B #136 14:38, row 285 test_levitate): a test that PASSES but leaves a game away
from the boot's scene is named in its own notes and the next test starts with the lane's recovery, so it never runs in
the wrong map; Pair.need_boot_place checks the scene as well as the age. No game: the fake lane.
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
sys.path.insert(0, INGAME)
sys.path.insert(0, os.path.dirname(HERE))
import pair as P          # noqa: E402

TESTS = '''
def test_1_leaves_without_put_back(pair):
    pair.warp_one("A", "Market_Child")          # (passes, and forgets to warp back)

def test_2_next_starts_at_the_boot_place(pair):
    with pair.setup():
        st = pair.need_boot_place("child")
    assert st["A"]["scene"] == "KakarikoVillage_Child", st["A"]["scene"]

def test_3_clean_test(pair):
    with pair.setup():
        pair.need_boot_place("child")
'''


class BootPlace(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        d = os.path.join(self.tmp, "t")
        os.makedirs(d)
        with open(os.path.join(d, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(d, "test_place.py"), "w", encoding="utf-8") as f:
            f.write(TESTS)
        self.dir, self.out = d, os.path.join(self.tmp, "o")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_a_passing_test_left_away_is_named_and_recovered(self):
        p = subprocess.run([sys.executable, "-m", "pytest", ".", "--fake", "--out", self.out, "-q", "-p",
                            "no:cacheprovider", "-p", "no:randomly"], cwd=self.dir, capture_output=True, text=True,
                           timeout=600)
        with open(os.path.join(self.out, "summary.json"), encoding="utf-8") as f:
            rows = {r["test"].split("::")[-1]: r for r in json.load(f)["tests"]}
        res = {k: v["result"] for k, v in rows.items()}
        self.assertEqual(res, {"test_1_leaves_without_put_back": "PASS", "test_2_next_starts_at_the_boot_place": "PASS",
                               "test_3_clean_test": "PASS"}, p.stdout[-3000:])
        left = " ".join(rows["test_1_leaves_without_put_back"]["notes"])
        self.assertIn("left the boot's place", left)
        self.assertIn("A: scene=Market_Child", left)
        self.assertNotIn("left the boot's place", " ".join(rows["test_2_next_starts_at_the_boot_place"]["notes"]))
        self.assertNotIn("left the boot's place", " ".join(rows["test_3_clean_test"]["notes"]))


class NeedBootPlace(unittest.TestCase):
    def setUp(self):
        import fakegame
        self.tmp = tempfile.mkdtemp()
        self.lane = fakegame.FakeLane(self.tmp)
        self.pair = P.Pair(self.lane.games())
        self.pair.begin(60)
        self.pair.boot_scene = "KakarikoVillage_Child"
        self.pair.boot_ready(self.pair.boot_scene)

    def tearDown(self):
        self.pair.close()
        self.lane.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_at_the_boot_place(self):
        self.assertEqual(self.pair.away_from_boot(), [])
        st = self.pair.need_boot_place("child")
        self.assertEqual({x: s["scene"] for x, s in st.items()}, {"A": "KakarikoVillage_Child", "B": "KakarikoVillage_Child"})

    def test_right_age_wrong_scene_is_a_setup_fail(self):
        """The old age-only check passed this: child Link, but in another map."""
        self.pair.warp_one("B", "Market_Child")
        self.assertEqual(self.pair.state("B")["age"], "child")
        with self.assertRaises(P.SetupFail) as e:
            self.pair.need_boot_place("child")
        self.assertIn("B is in Market_Child, not the boot's KakarikoVillage_Child", str(e.exception))
        self.assertEqual(len(self.pair.away_from_boot()), 1)

    def test_wrong_age_is_a_setup_fail(self):
        with self.assertRaises(P.SetupFail) as e:
            self.pair.need_boot_place("adult")
        self.assertIn("this test needs adult Link", str(e.exception))

    def test_no_boot_scene_checks_nothing_of_the_scene(self):
        self.pair.warp_one("B", "Market_Child")
        self.pair.boot_scene = None
        self.assertEqual(self.pair.away_from_boot(), [])
        self.pair.need_boot_place("child")

    def test_a_failed_read_counts_as_away(self):
        self.pair.c["A"].close()
        self.pair.c["A"].port = 1          # (nothing listens there: the read fails)
        away = self.pair.away_from_boot(seconds=3)
        self.assertTrue(any(w.startswith("A: no state") for w in away), away)


if __name__ == "__main__":
    unittest.main()
