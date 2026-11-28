"""Selftests of test_look.py::test_kokiri_hood_band (kokiri-band-c124's port) on the fake games
(ingame/fakes/kokiri_band.py); run by tests/test_ingame.py's load_tests."""
import os
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_ingame import pytest_run, results, summary   # noqa: E402


class KokiriBand(unittest.TestCase):
    """Deselected without its switches; with them it runs to its shots and puts both games back to child, so the
    lane's next test still sets up. Its band check holds on the fake shots but ends NO-REF (no stored reference, and
    the thresholds are not measured: never PASS); the pack before the fix and overlapping boxes are SETUP-FAIL; the
    row's bug (the puppet's band blue) is FAIL."""
    ENV = ["--env", "LICHII_COOP_COLOR=blue", "--env", "LICHII_COOP_LOOK=cap_trim:yellow",
           "--env", "LICHII_COOP_DEBUG_ANIM=1", "--env", "LICHII_COOP_CHEATS=1"]
    TESTS = ["test_look.py::test_kokiri_hood_band", "test_rows.py::test_puppets_spawn"]
    # (the fake games' pack: a stand-in maker per pack name, BT_PACKS_DIR / BT_FAKE_PACK_DIR as PackFiles in
    # test_ingame.py; the real makers need a romfs: packs/look_cap.py, packs/look_cap_old.py)
    MAKER = "\n".join([
        "import os",
        "def make(out, ctx):",
        "    os.makedirs(out, exist_ok=True)",
        "    p = os.path.join(out, 'cap.bin')",
        "    open(p, 'wb').write(b'COOPCAP1 %s')",
        "    ctx.say('cap.bin: fake')",
        "    return {'coop_look/cap.bin': p}",
        ""])

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.packs, self.tpl = os.path.join(self.tmp, "packs"), os.path.join(self.tmp, "tpl")
        os.makedirs(self.packs)
        os.makedirs(os.path.join(self.tpl, "coop_look"))
        for name in ("look_cap", "look_cap_old"):
            with open(os.path.join(self.packs, name + ".py"), "w", encoding="utf-8") as f:
                f.write(self.MAKER % name)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_it(self, name, env_args, env=None, pack="look_cap"):
        out = os.path.join(self.tmp, name)
        e = {"BT_PACKS_DIR": self.packs, "BT_FAKE_PACK_DIR": self.tpl}
        e.update(env or {})
        p = pytest_run(self.TESTS + ["--fake", "--no-rerun", "--out", out, "-q", "-s", "-p", "no:cacheprovider"]
                       + (["--pack", pack] if pack else []) + env_args, env=e)
        return summary(out), p

    def test_runs_with_its_switches_and_leaves_child(self):
        s, p = self.run_it("on", self.ENV)
        self.assertEqual(results(s), {"test_kokiri_hood_band": "NO-REF", "test_puppets_spawn": "PASS"},
                         p.stdout[-2000:])
        self.assertIn("the band thresholds are not measured yet", p.stdout)
        self.assertEqual(len(re.findall(r"(?m)^bandcheck \d", p.stdout)), 7, p.stdout[-2000:])   # (one line per distance)
        self.assertEqual(len(re.findall(r"(?m)^sight kokiri-band-\S+: B sees A", p.stdout)), 7, p.stdout[-3000:])   # (far + 6 nearer)
        s, p = self.run_it("off", [])
        self.assertEqual([d["test"] for d in s["deselected"]], ["test_look.py::test_kokiri_hood_band"], p.stdout[-2000:])
        s, p = self.run_it("nopack", self.ENV, pack=None)   # (the switches but not the fix's cap.bin: not run)
        self.assertEqual([d["test"] for d in s["deselected"]], ["test_look.py::test_kokiri_hood_band"], p.stdout[-2000:])

    def test_old_cap_bin_is_setup_fail(self):
        s, p = self.run_it("old", self.ENV, env={"BT_FAKE_CAP_BLOCKS": "43228"})
        self.assertEqual(results(s)["test_kokiri_hood_band"], "SETUP-FAIL", p.stdout[-2000:])
        self.assertIn("not the fix's cap.bin", p.stdout)

    def test_probe_on_the_old_pack_is_no_ref(self):
        """KOKIRI_BAND_PROBE_PACK=43600 (packs/look_cap_old.py's count): the old pack is taken, every bandcheck line printed, never PASS or FAIL."""
        s, p = self.run_it("probe", self.ENV, env={"BT_FAKE_CAP_BLOCKS": "43600", "BT_FAKE_BAND": "blue",
                                                   "KOKIRI_BAND_PROBE_PACK": "43600"}, pack="look_cap_old")
        self.assertEqual(results(s)["test_kokiri_hood_band"], "NO-REF", p.stdout[-2000:])
        self.assertIn("probe run on the 43600 pack", p.stdout)
        self.assertEqual(len(re.findall(r"(?m)^bandcheck \d", p.stdout)), 7, p.stdout[-2000:])

    def test_blue_band_on_the_puppet_fails(self):
        """The row's bug (the puppet's hood band in the tunic's blue at a distance) is a FAIL, not a pass or NO-REF."""
        s, p = self.run_it("blue", self.ENV, env={"BT_FAKE_BAND": "blue"})
        self.assertEqual(results(s)["test_kokiri_hood_band"], "FAIL", p.stdout[-2000:])
        self.assertIn("the puppet's hood band is not the trim colour", p.stdout)
        self.assertEqual(results(s)["test_puppets_spawn"], "PASS", p.stdout[-2000:])   # (back to child after a FAIL)

    def test_puppet_on_the_line_of_sight_is_setup_fail(self):
        """A's puppet drawn over B's own Link (or a background box over the other Link): SETUP-FAIL, never judged."""
        s, p = self.run_it("line", self.ENV, env={"BT_FAKE_KOKIRI_ON_LINE": "1"})
        self.assertEqual(results(s)["test_kokiri_hood_band"], "SETUP-FAIL", p.stdout[-2000:])
        self.assertIn("boxes overlap", p.stdout)   # (sight.prove's check, before any band check)
        self.assertEqual(results(s)["test_puppets_spawn"], "PASS", p.stdout[-2000:])

    def test_puppet_out_of_sight_is_setup_fail(self):
        """A's puppet not drawn on B's shot (behind a wall: run kokiri-look-182cd53b, 2026-11-28): sight.prove finds it
        on no heading = SETUP-FAIL with each try's numbers, never a FAIL of the band; the lane's next test still sets up."""
        s, p = self.run_it("hidden", self.ENV, env={"BT_FAKE_KOKIRI_HIDDEN": "1"})
        self.assertEqual(results(s)["test_kokiri_hood_band"], "SETUP-FAIL", p.stdout[-2000:])
        self.assertIn("in sight at 14.0 m on no heading", p.stdout)
        for turn in (0, 90, 180, 270):   # (every heading tried, each with its own reason)
            self.assertIn("turn %d: sight kokiri-band-turn%03d" % (turn, turn), p.stdout)
        self.assertIn("NOT in sight on B's shot", p.stdout)
        self.assertNotIn("bandcheck 14.0", p.stdout)
        self.assertEqual(results(s)["test_puppets_spawn"], "PASS", p.stdout[-2000:])


if __name__ == "__main__":
    unittest.main()
