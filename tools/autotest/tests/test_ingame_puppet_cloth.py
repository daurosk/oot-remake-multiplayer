"""Selftests of tools/autotest/ingame/test_puppet_cloth.py (cloth-puppet-313) on the fake games
(ingame/fakes/puppet_cloth.py); loaded by test_ingame.py's load_tests."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_ingame import pytest_run, results, summary   # noqa: E402

ENV = "LICHII_COOP_DEBUG_CLOTH=1"
TESTS = ("test_cloth_child", "test_cloth_adult")
CONTROL = "test_cloth_absent_control"
# (a stand-in for packs/no_cloth.py, which needs a romfs: the --no-cloth pack's manifest, as build.py writes it)
MAKER = "\n".join([
    "import json, os",
    "def make(out, ctx):",
    "    p = os.path.join(out, 'coop_puppet.json')",
    "    with open(p, 'w') as f:",
    "        json.dump({'cloth': False}, f)",
    "    return {'coop_puppet.json': p}",
    ""])


class PuppetCloth(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.refs = os.path.join(self.tmp, "refs")
        os.makedirs(self.refs)
        self.packs, self.tpl = os.path.join(self.tmp, "packs"), os.path.join(self.tmp, "tpl")
        os.makedirs(self.packs)
        os.makedirs(self.tpl)
        with open(os.path.join(self.tpl, "coop_puppet.json"), "w") as f:
            f.write('{"cloth": true}')
        with open(os.path.join(self.packs, "no_cloth.py"), "w", encoding="utf-8") as f:
            f.write(MAKER)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_(self, name, extra=("--env", ENV), env=None):
        out = os.path.join(self.tmp, name)
        p = pytest_run(["test_puppet_cloth.py", "--fake", "--out", out, "-q", "-p", "no:cacheprovider", "--no-rerun"]
                       + list(extra), env=dict({"BT_FAKE_REFS": self.refs, "BT_PACKS_DIR": self.packs, "BT_FAKE_PACK_DIR": self.tpl},
                                **(env or {})))
        return p, summary(out), out

    def test_passes_once_refs_exist(self):
        """Run 1: NO-REF (only the shots' refs missing); with the kept candidates as refs, run 2: PASS."""
        p, s, out = self.run_("c1")
        self.assertEqual(results(s), {t: "NO-REF" for t in TESTS}, p.stdout[-3000:])
        new = os.path.join(out, "lane1", "new")
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(self.refs, f))
        p, s, _ = self.run_("c2")
        self.assertEqual(results(s), {t: "PASS" for t in TESTS}, p.stdout[-3000:])

    def test_no_cloth_fails(self):
        """The bug (a copy's holder 0) FAILs both tests, naming row 313's cause."""
        p, s, _ = self.run_("c3", env={"BT_FAKE_CLOTH": "none"})
        self.assertEqual(results(s), {t: "FAIL" for t in TESTS}, p.stdout[-3000:])
        for t in s["tests"]:
            self.assertIn("holds NO cloth", t["why"])

    def test_needs_the_switch(self):
        """Without --env LICHII_COOP_DEBUG_CLOTH=1 all three are deselected, never SETUP-FAIL."""
        p, s, _ = self.run_("c4", extra=())
        self.assertEqual(results(s), {}, p.stdout[-3000:])
        self.assertEqual(len(s.get("deselected") or []), 3, s.get("deselected"))


    def test_own_link_without_holder_fails(self):
        """Our own Link's holder 0 too (the probe's chain finds no cloth at all): FAIL, never a copy verdict."""
        p, s, _ = self.run_("c5", env={"BT_FAKE_CLOTH": "own-none"})
        self.assertEqual(results(s), {t: "FAIL" for t in TESTS}, p.stdout[-3000:])
        for t in s["tests"]:
            self.assertIn("our own Link has no cloth holder", t["why"])

    def test_still_words_are_no_ref(self):
        """The copy's words never change running: NO-REF naming the swing (even with every shot's ref there)."""
        p, s, out = self.run_("c6", env={"BT_FAKE_CLOTH": "still"})
        new = os.path.join(out, "lane1", "new")
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(self.refs, f))
        p, s, _ = self.run_("c7", env={"BT_FAKE_CLOTH": "still"})
        self.assertEqual(results(s), {t: "NO-REF" for t in TESTS}, p.stdout[-3000:])
        for t in s["tests"]:
            self.assertIn("did not move more running", t["why"])

    def test_control_on_the_old_pack(self):
        """--pack no_cloth: the control PASSes (copy holder 0, our Link's set); without the pack it is deselected;
        on the fixed pack's cloth (BT_FAKE_CLOTH unset, the stand-in not taken) it is never run."""
        p, s, _ = self.run_("c8", extra=("--env", ENV, "--pack", "no_cloth", "-k", CONTROL))
        self.assertEqual(results(s), {CONTROL: "PASS"}, p.stdout[-3000:])
        notes = " ".join(n for t in s["tests"] for n in t.get("notes", []))
        self.assertIn("control: copy", notes)

    def test_no_cloth_reads_the_kit_flags(self):
        """packs/no_cloth.py takes the pack flags from kit-paths.ps1 Get-KitPackArgs (never a copy): the repo's file
        gives the kit's '--title-rows', '--pause-tab', and a changed function is followed."""
        import importlib.util
        here = os.path.dirname(os.path.abspath(__file__))
        spec = importlib.util.spec_from_file_location("no_cloth_pack", os.path.join(os.path.dirname(here), "ingame",
                                                                                     "packs", "no_cloth.py"))
        nc = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(nc)
        tools = os.path.dirname(os.path.dirname(here))
        self.assertEqual(nc.kit_flags(tools), ["--title-rows", "--pause-tab", "--puppet-body", "--puppet-liftable=auto"])
        fake = os.path.join(self.tmp, "tools")
        os.makedirs(os.path.dirname(os.path.join(fake, nc.KIT_PATHS)))
        with open(os.path.join(fake, nc.KIT_PATHS), "w", encoding="utf-8") as f:
            f.write("function Get-KitPackArgs([string]$Tool, [string]$From, [string]$Mod) {\n"
                    "    return @('-I', '-B', $Tool, '--romfs', $From, '--mod-dir', $Mod, '--title-rows', '--x-new')\n}\n")
        self.assertEqual(nc.kit_flags(fake), ["--title-rows", "--x-new"])

    def test_parse_takes_nan_words(self):
        """A '-nan(ind)' / 'inf' word (Windows CRT %g of game memory) parses as NaN, never raises."""
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ingame"))
        import test_puppet_cloth as tc
        c = tc.parse("[hos x] coop: puppet: cloth: P2 physics 0x1 set 0x2 holder 0x3 n 1 list 0x4 entries 2 e0 flags "
                     "0000000000000001: 1 -nan(ind) inf 2")
        self.assertEqual(len(c["words"]), 4)
        self.assertNotEqual(c["words"][1], c["words"][1])    # NaN
        self.assertEqual(tc.change([c, dict(c, words=[1.5, float("nan"), float("inf"), 2.0])]), 0.5)


if __name__ == "__main__":
    unittest.main()
