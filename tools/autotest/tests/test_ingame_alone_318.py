"""Selftests of ingame/test_alone_318.py (rows 318 + 334, dbno-alone-318's file) against the fake games
(ingame/fakes/alone_318.py): the rows PASS only when a player with no teammate nearby goes to its own entrance (no
downed wait, no watching a far player); each known way of failing (BT_FAKE_ALONE_BUG) FAILs its row; a FAIL leaves the
lane as the next rows expect it. Run by tests/test_ingame.py's load_tests in CI."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
LETHAL = "test_lethal_hit_with_nobody_nearby_goes_to_the_entrance"
BLED = "test_bled_out_after_the_teammate_left_goes_to_the_entrance"
WATCHED = "test_watched_teammate_leaves_and_the_spectator_goes_to_the_entrance"


def run(out, k, refs=None, bug=None):
    env = dict(os.environ)
    env.pop("BT_FAKE_ALONE_BUG", None)
    if bug:
        env["BT_FAKE_ALONE_BUG"] = bug
    if refs:
        env["BT_FAKE_REFS"] = refs
    p = subprocess.run([sys.executable, "-m", "pytest", ".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider",
                        "--no-smoke-gate", "-k", k], cwd=INGAME, capture_output=True, text=True, timeout=600, env=env)
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        s = json.load(f)
    return p, {r["test"].split("::")[-1]: r for r in s["tests"]}


class Alone318(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """References from a first (NO-REF) run, approved here wholesale, so the runs below judge state, not looks."""
        cls.tmp = tempfile.mkdtemp()
        cls.refs = os.path.join(cls.tmp, "refs")
        os.makedirs(cls.refs)
        out = os.path.join(cls.tmp, "first")
        p, res = run(out, "alone_318", refs=cls.refs)
        cls.first, cls.first_out = res, p.stdout[-2000:]
        new = os.path.join(out, "lane1", "new")
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(cls.refs, f))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def out(self, name):
        return os.path.join(self.tmp, name)

    def test_first_run_is_no_ref_not_fail(self):
        self.assertEqual({t: r["result"] for t, r in self.first.items()},
                         {LETHAL: "NO-REF", BLED: "NO-REF", WATCHED: "NO-REF"}, self.first_out)
        for r in self.first.values():
            self.assertEqual({sh["game"] for sh in r["shots"]}, {"A", "B"})
            self.assertLessEqual(len(r["shots"]), 8)

    def test_passes_with_refs(self):
        p, res = run(self.out("pass"), "alone_318", refs=self.refs)
        self.assertEqual({r["result"] for r in res.values()}, {"PASS"}, p.stdout[-2000:])
        self.assertEqual(len(res), 3, p.stdout[-2000:])

    def test_downed_with_nobody_nearby_fails_row_334(self):
        p, res = run(self.out("bug-down"), LETHAL, refs=self.refs, bug="down")
        self.assertEqual(res[LETHAL]["result"], "FAIL", p.stdout[-2000:])

    def test_watching_a_far_player_fails_row_318(self):
        for t in (BLED, WATCHED):
            with self.subTest(test=t):
                p, res = run(self.out("bug-watch-" + t[-12:]), t, refs=self.refs, bug="watch")
                self.assertEqual(res[t]["result"], "FAIL", p.stdout[-2000:])

    def test_a_fail_leaves_the_lane_for_the_next_rows(self):
        """After the row FAILs (B came back beside A, in A's scene), the rows after it still set up (the restore: Down
        but not out on at 45 s, both at the Kakariko boot's spot)."""
        p, res = run(self.out("after"), "%s or test_puppets_spawn or test_teleport_to_player" % WATCHED,
                     refs=self.refs, bug="watch")
        self.assertEqual(res[WATCHED]["result"], "FAIL", p.stdout[-2000:])
        self.assertEqual(res["test_puppets_spawn"]["result"], "PASS", p.stdout[-2000:])
        self.assertEqual(res["test_teleport_to_player"]["result"], "PASS", p.stdout[-2000:])


if __name__ == "__main__":
    unittest.main()
