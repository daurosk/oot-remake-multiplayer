"""Selftests of ingame/test_lift_317.py (row 317 probes P1 + P2) against the fake games (ingame/fakes/lift_317.py on
top of fakes/dbno_crawl.py), run by test_ingame.py's load_tests: both probes end NO-REF (never PASS) with their values
in the notes; P1 is SETUP-FAIL without Ruto, FAIL when B refuses the lift, deselected without its switch. The `native`
pack is a stand-in maker here (BT_PACKS_DIR), as tests/test_ingame_native_pvp.py; the real one needs a lane."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

P1, P2 = "test_probe_lift_prompt_317", "test_probe_revive_button_317"
ENV = ["--env", "LICHII_COOP_LIFT_PLAYER=1"]
MAKER = '''import os


def make(out, ctx):
    p = os.path.join(out, "coop_puppet.json")
    with open(p, "w") as f:
        f.write('{"body": true, "liftable": true}')
    return {"coop_puppet.json": p}
'''


class Lift317(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.packs = os.path.join(self.tmp, "packs")
        self.tpl = os.path.join(self.tmp, "tpl")
        os.makedirs(self.packs)
        os.makedirs(self.tpl)
        with open(os.path.join(self.packs, "native.py"), "w", encoding="utf-8") as f:
            f.write(MAKER)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, k, bug=None, env_args=ENV):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs"), "BT_PACKS_DIR": self.packs,
               "BT_FAKE_PACK_DIR": self.tpl, "BT_FAKE_LIFT317_BUG": bug or ""}
        p = T.pytest_run(["test_lift_317.py", "--fake", "--out", out, "--pack", "native"] + list(env_args) +
                         (["--no-rerun"] if bug else []) + ["-q", "-p", "no:cacheprovider", "--no-smoke-gate",
                                                            "-k", k], env=env)
        return p, T.summary(out)

    def one(self, p, s, test):
        r = [t for t in s["tests"] if t["test"].endswith(test)]
        self.assertEqual(len(r), 1, (p.stdout[-3000:], s))
        return r[0]

    def notes(self, r):
        return " | ".join(r.get("notes", []))

    def test_p1_no_ref_with_values(self):
        p, s = self.run_fake("ok", P1)
        r = self.one(p, s, P1)
        self.assertEqual(r["result"], "NO-REF", (r, p.stdout[-3000:]))
        n = self.notes(r)
        for want in ("copy_near='Lift'", "ruto_near='Lift'", "copy_carry='Throw'", "same_near='same'",
                     "same_carry='same'", "A_answered='yes'", "B_input='off'", "B_carried_pose='Eaten'",
                     "B_input_end='back'"):
            self.assertIn(want, n)
        self.assertEqual({sh["game"] for sh in r["shots"]}, {"A", "B"})
        self.assertLessEqual(len(r["shots"]), 8)

    def test_p1_no_copy_prompt_is_noted(self):
        p, s = self.run_fake("noprompt", P1, bug="noprompt")
        r = self.one(p, s, P1)
        self.assertEqual(r["result"], "NO-REF", r)
        self.assertIn("same_near='DIFFERENT'", self.notes(r))

    def test_p1_refused_fails(self):
        p, s = self.run_fake("refuse", P1, bug="refuse")
        r = self.one(p, s, P1)
        self.assertEqual(r["result"], "FAIL", r)
        self.assertIn("refused", r["why"])

    def test_p1_no_ruto_is_setup_fail(self):
        p, s = self.run_fake("noruto", P1, bug="noruto")
        r = self.one(p, s, P1)
        self.assertEqual(r["result"], "SETUP-FAIL", r)
        self.assertIn("Npc_Zora_Ruto_Child_001", r["why"])

    def test_p1_without_switch_deselected(self):
        p, s = self.run_fake("noenv", P1, env_args=())
        self.assertEqual([t for t in s["tests"] if t["test"].endswith(P1)], [], (p.stdout[-2000:], s))

    def test_p2_no_ref_lists_inert(self):
        p, s = self.run_fake("p2", P2, env_args=())
        r = self.one(p, s, P2)
        self.assertEqual(r["result"], "NO-REF", (r, p.stdout[-3000:]))
        self.assertIn("P2 inert beside a downed copy: B, X, L, R, DDOWN, DUP", self.notes(r))
        self.assertEqual({sh["game"] for sh in r["shots"]}, {"A", "B"})
        self.assertEqual(len(r["shots"]), 4)


if __name__ == "__main__":
    unittest.main()
