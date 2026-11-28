"""Selftests of the Phantom Ganon fall-down probe (ingame/test_probe_pg_falldown_287.py), no game: fake bridges
(fakes/phantom_ganon.py's probe mode: verb 139 overwritten by the AI's 40, sword swings, 'Fly' at the third).
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

BOOT = "live-phantom-ganon"
NAME = "test_probe_pg_falldown"
TEST = "test_probe_pg_falldown_287.py::" + NAME
ENV = ["--env", "BT_PROBE_PG_FALLDOWN=1"]


class ProbePgFalldown(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_probe(self, more, env=None):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        p = T.pytest_run(["test_probe_pg_falldown_287.py", "--fake", "--no-rerun", "--out", out, "-q", "--boot", BOOT]
                         + more, env=env)
        return p, T.summary(out), out

    def rec(self, out):
        found = [os.path.join(d, f) for d, _, fs in os.walk(out) for f in fs if f == "probe-pg-falldown.json"]
        self.assertEqual(len(found), 1, found)
        with open(found[0], encoding="utf-8") as f:
            return json.load(f)

    def test_unproven_names_what_was_seen(self):
        """As pganon-287-182cd53b: verb 139 never starts it; the sword reaches Fly (fake), still nothing -> NO-REF
        'UNPROVEN' listing the anims seen; c1..c3 each a window; his life never 0."""
        p, s, out = self.run_probe(ENV)
        self.assertEqual(T.results(s), {NAME: "NO-REF"}, p.stdout[-2000:])
        why = s["tests"][0]["why"]
        self.assertIn("UNPROVEN", why)
        self.assertIn("'Fly'", why)
        rec = self.rec(out)
        self.assertEqual(sorted(rec["windows"]), ["c1-verb139", "c2-sword", "c3-fly"])
        self.assertEqual(rec["owner"], "A")
        self.assertTrue(rec["swings"])
        self.assertTrue(all(x["life_after"] > 0 for x in rec["swings"]), rec["swings"])
        self.assertNotIn("error", rec)
        a = rec["windows"]["c1-verb139"]["games"]["A"]["lines"]
        self.assertTrue(any("verb 40 (changed 1) after 1 x ours asks slot 0 verb 139" in ln for ln in a), a)

    def test_started_in_fly_names_the_window_and_lines(self):
        p, s, out = self.run_probe(ENV, env={"BT_FAKE_PG_FLY_FALL": "1"})
        self.assertEqual(T.results(s), {NAME: "NO-REF"}, p.stdout[-2000:])
        why = s["tests"][0]["why"]
        self.assertIn("STARTED by c3-fly (owner A)", why)
        self.assertIn("attack starts (action verbs 139", why)
        self.assertEqual(self.rec(out)["windows"]["c2-sword"]["started"], [])

    def test_deselected_without_its_switch(self):
        p, s, _ = self.run_probe([])
        self.assertEqual(s["tests"], [], p.stdout[-2000:])
        self.assertIn(TEST, [d["test"] for d in s["deselected"]])


if __name__ == "__main__":
    unittest.main()
