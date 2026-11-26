"""Rows 138(4)/137's selftest (ingame/test_boss_reset.py + ingame/fakes/boss_reset.py + saves/gohma-alive.py and
testsave.prove; owner port-g3-save), run by test_ingame.py's load_tests: NO-REF on the fake (look shots, no references)
with its boot + --save gohma-alive, deselected without that save or on another boot, SETUP-FAIL when the games do not
run the profile's save, FAIL under each real bug, NO-REF for row 137 when B took no container."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402
from test_save_flags import make_save   # noqa: E402

BOOT, SAVE, NAME = "live-deku-gohma-reset", "gohma-alive", "test_boss_reset_host_loads_before_her"
ENV = ["--env", "LICHII_COOP_DEBUG_BOSS=1", "--env", "LICHII_COOP_DEBUG_OBJECTS=1"]
# the lane baseline: Gohma BEATEN in the file (the profile must change every one of its five flags)
BASE = {0: [(0x6b093b51, 1), (0x3419ccb3, 1), (0x00c0a2f6, 1), (0x5882768c, 1)], 6: [(0x7d776b61, 0x11111111)]}


class BossReset(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.tpl = os.path.join(self.tmp, "tpl")
        os.makedirs(os.path.join(self.tpl, "slot_00"))
        with open(os.path.join(self.tpl, "slot_00", "progress.sav"), "wb") as f:
            f.write(bytes(make_save(BASE)))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, boot=BOOT, save=SAVE, env_args=ENV, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs"), "BT_FAKE_SAVE_DIR": self.tpl}
        if bug:
            env["BT_FAKE_BOSS_RESET_BUG"] = bug
        if boot:
            env["BT_FAKE_BOOT"] = boot
        p = T.pytest_run([".", "--fake", "--out", out] + list(env_args) + (["--boot", boot] if boot else []) +
                         (["--save", save] if save else []) + (["--no-rerun"] if bug else []) +
                         ["-q", "-p", "no:cacheprovider", "-k", "test_boss_reset"], env=env)
        return p, T.summary(out)

    def result(self, s):
        r = T.results(s).get(NAME)
        return [(r, [t["why"] for t in s["tests"] if t["test"].endswith(NAME)][0])] if r else []

    def test_fake_no_ref(self):
        p, s = self.run_fake("ok")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "NO-REF", r)
        self.assertIn("no stored reference", r[0][1])

    def test_deselected_without_the_save_or_boot(self):
        for name, kw, key in (("nosaveopt", {"save": None}, "needs_save"), ("otherboot", {"boot": None}, "needs_boot")):
            with self.subTest(name=name):
                p, s = self.run_fake(name, **kw)
                self.assertEqual(self.result(s), [], s)
                self.assertIn(NAME, " ".join(d["test"] for d in s.get("deselected", []) if d.get(key)), s)

    def test_save_not_loaded_is_setup_fail(self):
        p, s = self.run_fake("nosave", bug="nosave")
        r = self.result(s)
        self.assertEqual(r[0][0], "SETUP-FAIL", r)
        self.assertIn("--save gohma-alive", r[0][1])
        self.assertIn("flag 0x6b093b51", r[0][1])

    def test_bugs_fail(self):
        for bug, why in (("early", "while still in her area"), ("noput", "is alive in the host's save"),
                         ("warp", "still 1 back in her room"), ("nolower", "max life")):
            with self.subTest(bug=bug):
                p, s = self.run_fake(bug, bug=bug)
                r = self.result(s)
                self.assertEqual(r[0][0], "FAIL", r)
                self.assertIn(why, r[0][1])

    def test_no_container_is_no_ref_for_137(self):
        p, s = self.run_fake("nocontainer", bug="nocontainer")
        r = self.result(s)
        self.assertEqual(r[0][0], "NO-REF", r)
        self.assertIn("row 137 not proven", r[0][1])


if __name__ == "__main__":
    unittest.main()
