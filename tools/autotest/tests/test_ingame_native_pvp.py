"""The true PvP probe's selftest (ingame/test_native_pvp.py + ingame/fakes/native_pvp.py), run by test_ingame.py's
load_tests: NO-REF on the fake (look shots, no references) with every kind's strike and outcome in pvp-probe.json and
the guarded slash, FAIL when A's slash sends no strike, SETUP-FAIL when the pack has no _Pvp copies, and deselected
without the probe's switches. The `native` pack is a stand-in maker here (BT_PACKS_DIR): the real one is
ingame/packs/native.py, which needs a lane."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

NAME = "test_pvp_probe"
ENV = ["--env", "LICHII_COOP_PVP_NATIVE=1", "--env", "LICHII_COOP_DEBUG_OBJECTS=1", "--env", "LICHII_COOP_CHEATS=1"]
MAKER = '''import os


def make(out, ctx):
    p = os.path.join(out, "coop_puppet.json")
    with open(p, "w") as f:
        f.write('{"pvp": true}')
    return {"coop_puppet.json": p}
'''


class NativePvp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.packs = os.path.join(self.tmp, "packs")
        self.tpl = os.path.join(self.tmp, "tpl")    # the fake games' co-op pack (bt_plugin BT_FAKE_PACK_DIR)
        os.makedirs(self.packs)
        os.makedirs(self.tpl)
        with open(os.path.join(self.packs, "native.py"), "w", encoding="utf-8") as f:
            f.write(MAKER)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, bug=None, env_args=ENV):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs"), "BT_PACKS_DIR": self.packs,
               "BT_FAKE_PACK_DIR": self.tpl}
        if bug:
            env["BT_FAKE_PVP_BUG"] = bug
        p = T.pytest_run([".", "--fake", "--out", out, "--pack", "native"] + list(env_args) +
                         (["--no-rerun"] if bug else []) + ["-q", "-p", "no:cacheprovider", "-k", NAME], env=env)
        return p, T.summary(out), out

    def result(self, s):
        return [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith(NAME)]

    def test_fake_no_ref_with_every_kind(self):
        p, s, out = self.run_fake("ok")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "NO-REF", r)
        found = [os.path.join(d, n) for d, _, ns in os.walk(out) for n in ns if n == "pvp-probe.json"]
        self.assertEqual(len(found), 1, found)
        with open(found[0], encoding="utf-8") as f:
            facts = json.load(f)
        self.assertEqual([k["kind"] for k in facts], ["slash", "spin", "jump", "stick", "slash-at-guard"])
        for k in facts:
            self.assertIn("outcome", k, k)
            self.assertEqual(len(k["record"]), 38, k)
        self.assertEqual([k["outcome"] for k in facts], ["kept_off"] * 4 + ["guarded"])
        self.assertTrue(facts[-1]["b_guard_anim"], facts[-1])
        self.assertEqual([k["b_front_bit"] for k in facts], [0] * 4 + [1])
        self.assertEqual(len({k["seq"] for k in facts}), 5, facts)   # one strike per press

    def test_no_strike_fails(self):
        p, s, _ = self.run_fake("nostrike", bug="nostrike")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "FAIL", r)
        self.assertIn("sent no strike", r[0][1])

    def test_no_pvp_pack_is_setup_fail(self):
        p, s, _ = self.run_fake("nopack", bug="nopack")
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        self.assertEqual(r[0][0], "SETUP-FAIL", r)
        self.assertIn("no _Pvp copies", r[0][1])

    def test_without_switches_deselected(self):
        p, s, _ = self.run_fake("noenv", env_args=())
        self.assertEqual(self.result(s), [], (p.stdout, s))


if __name__ == "__main__":
    unittest.main()
