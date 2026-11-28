"""Selftests of ingame/test_native_body.py (native puppets N1) on the fake games (ingame/fakes/native_body.py) and of
its pack maker ingame/packs/native.py (with a stand-in build); run by tests/test_ingame.py's load_tests."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_ingame import pytest_run, summary   # noqa: E402

INGAME = os.path.join(os.path.dirname(HERE), "ingame")
NAME = "test_native_body"
# (the fake games' pack: a stand-in maker, as test_ingame_kokiri_band.py; the real maker needs a romfs and a build)
MAKER = "\n".join([
    "import os",
    "def make(out, ctx):",
    "    p = os.path.join(out, 'coop_puppet.json')",
    "    open(p, 'w').write('{\"body\": true}')",
    "    return {'coop_puppet.json': p}",
    ""])


class NativeBodyFake(unittest.TestCase):
    """Deselected without --pack native; with it NO-REF on the fake (look shots, no references); each bug is caught:
    no body -> FAIL, no body line -> SETUP-FAIL, a body left at the old spot -> FAIL, drift -> FAIL. Without the pack
    the fake does nothing (B #149: scoped to the boot that has the pack in)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.packs, self.tpl = os.path.join(self.tmp, "packs"), os.path.join(self.tmp, "tpl")
        os.makedirs(self.packs)
        os.makedirs(self.tpl)
        with open(os.path.join(self.packs, "native.py"), "w", encoding="utf-8") as f:
            f.write(MAKER)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, bug=None, pack=True):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs"), "BT_PACKS_DIR": self.packs,
               "BT_FAKE_PACK_DIR": self.tpl}
        if bug:
            env["BT_FAKE_NATIVE_BUG"] = bug
        p = pytest_run(["test_native_body.py", "--fake", "--out", out, "--no-rerun", "-q", "-p", "no:cacheprovider"]
                       + (["--pack", "native"] if pack else []), env=env)
        return p, summary(out)

    def result(self, p, s):
        r = [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith(NAME)]
        self.assertEqual(len(r), 1, (p.stdout[-2000:], s))
        return r[0]

    def test_fake_no_ref(self):
        p, s = self.run_fake("ok")
        res, why = self.result(p, s)
        self.assertEqual(res, "NO-REF", why)
        self.assertIn("the N1 thresholds are not measured yet", why)
        self.assertNotIn("misses", why)
        notes = json.dumps(s)
        for step in ("walk: moved 2.50 m, closest 0.50 m", "drop: A over B's feet ['1.17'", "summary: rest 240, moved 60",
                     "old spot: A got"):
            self.assertIn(step, notes)      # (every step ran, each on the fake's body)

    def test_without_pack_deselected(self):
        p, s = self.run_fake("nopack", pack=False)
        self.assertIn({"test": "test_native_body.py::test_native_body", "needs_pack": ["native"], "needs_env": []},
                      s["deselected"], (p.stdout[-2000:], s))
        self.assertFalse([t for t in s["tests"] if t["test"].endswith(NAME) and t["result"] in ("PASS", "FAIL", "NO-REF")],
                         s)

    def test_threshold_miss_is_no_ref_listed(self):
        p, s = self.run_fake("lowtop", bug="lowtop")
        res, why = self.result(p, s)
        self.assertEqual(res, "NO-REF", why)
        self.assertIn("misses: A never stood still 1.17 +- 0.35 m over B's feet", why)

    def test_no_body_fails(self):
        p, s = self.run_fake("nobody", bug="nobody")
        res, why = self.result(p, s)
        self.assertEqual(res, "FAIL", why)
        self.assertIn("A walked through B's standing copy", why)

    def test_no_body_line_is_setup_fail(self):
        p, s = self.run_fake("noline", bug="noline")
        res, why = self.result(p, s)
        self.assertEqual(res, "SETUP-FAIL", why)
        self.assertIn("has no body line", why)

    def test_body_left_behind_fails(self):
        p, s = self.run_fake("left", bug="left")
        res, why = self.result(p, s)
        self.assertEqual(res, "FAIL", why)
        self.assertIn("a body left behind there", why)

    def test_drift_fails(self):
        p, s = self.run_fake("drift", bug="drift")
        res, why = self.result(p, s)
        self.assertEqual(res, "FAIL", why)
        self.assertIn("the game had the copy 0.250 m", why)


class NativeBodyProbeFake(unittest.TestCase):
    """ingame/test_native_body_probe.py on the fake games (fakes/native_body.py): NO-REF with both walks measured; the
    guard stops A where the two touch, the copy too; npcthrough: neither stops him (A goes past both)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.packs, self.tpl = os.path.join(self.tmp, "packs"), os.path.join(self.tmp, "tpl")
        os.makedirs(self.packs)
        os.makedirs(self.tpl)
        with open(os.path.join(self.packs, "native.py"), "w", encoding="utf-8") as f:
            f.write(MAKER)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_probe(self, name, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs"), "BT_PACKS_DIR": self.packs,
               "BT_FAKE_PACK_DIR": self.tpl}
        if bug:
            env["BT_FAKE_NATIVE_BUG"] = bug
        p = pytest_run(["test_native_body_probe.py", "--fake", "--out", out, "--no-rerun", "-q", "-p",
                        "no:cacheprovider", "--pack", "native"], env=env)
        s = summary(out)
        r = [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith("test_native_body_probe")]
        self.assertEqual(len(r), 1, (p.stdout[-2000:], s))
        return r[0]

    def test_probe_measures_both(self):
        res, why = self.run_probe("ok")
        self.assertEqual(res, "NO-REF", why)
        self.assertRegex(why, r"npc Npc_Hylia_Soldier_003: A moved [0-9.]+ m, closest 0\.8[0-9] m to its place, "
                              r"0\.8[0-9] m to its body \(25/25 samples listed\), body moved 0\.00 m, A stopped short")
        self.assertRegex(why, r"copy P2: A moved [0-9.]+ m, closest 0\.5[0-9] m .* A stopped short")
        self.assertIn("summary: rest 240, moved 60, calls 60, waited 0, gave way 0, drift 0.000 m", why)

    def test_probe_npc_through(self):
        res, why = self.run_probe("through", bug="npcthrough")
        self.assertEqual(res, "NO-REF", why)
        self.assertRegex(why, r"npc Npc_Hylia_Soldier_003: .* A went past")
        self.assertRegex(why, r"copy P2: .* A went past")

    def test_probe_no_body_line_is_setup_fail(self):
        res, why = self.run_probe("noline", bug="noline")
        self.assertEqual(res, "SETUP-FAIL", why)
        self.assertIn("has no body line", why)

def _load_maker():
    import importlib.util
    spec = importlib.util.spec_from_file_location("native_pack", os.path.join(INGAME, "packs", "native.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(data)


class NativePack(unittest.TestCase):
    """packs/native.py: the switches, the build command (the lane's pack command + the switch), the junction to the
    lane's Character Select, and only the files that differ from lane A's pack; every wrong pack a make failure."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.m = _load_maker()
        tools = os.path.dirname(os.path.dirname(HERE))
        for p in (tools, os.path.dirname(HERE), INGAME):
            if p not in sys.path:
                sys.path.insert(0, p)
        import testpack
        self.game = os.path.join(self.tmp, "lane1-A")
        self.pack = os.path.join(self.game, testpack.PACK_REL)
        self.lane_doc = {"body": False, "characters": {"mod": "OoT Character Select"}, "title_rows": True,
                         "pause_menu": ["a"], "charsel_native": [], "actors": [{"actor": "P", "body": None}]}
        _write(os.path.join(self.pack, "coop_puppet.json"), json.dumps(self.lane_doc))
        _write(os.path.join(self.pack, "romfs", "same.bin"), "same")
        _write(os.path.join(self.pack, "romfs", "table.bin"), "old")
        _write(os.path.join(self.pack, "_coop-archives", "kept.bin"), "kit")
        _write(os.path.join(self.game, "romfs", "game.bin"), "game")
        _write(os.path.join(self.game, "mods", "OoT Character Select", "characters.json"), "{}")
        _write(os.path.join(self.game, "mods", "coop-mods.list"), "coop-mods 1\n")
        self.out = os.path.join(self.tmp, "out")
        os.makedirs(self.out)
        self.said, self.cmds = [], []
        self.ctx = types.SimpleNamespace(tools=tools, romfs=os.path.join(self.game, "romfs"), game_nup="", lane=None,
                                         say=self.said.append)
        self.real_run = subprocess.run
        self.made = {"body": True}
        self.extra = {}

    def tearDown(self):
        subprocess.run = self.real_run
        os.environ.pop(self.m.ENV, None)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fake_run(self, cmd, **kw):
        if cmd[:1] == ["cmd"]:                     # (mklink /J: the real one)
            return self.real_run(cmd, **kw)
        self.cmds.append(cmd)
        built = cmd[cmd.index("--mod-dir") + 1]
        mods = os.path.dirname(os.path.dirname(os.path.dirname(built)))
        self.saw_cs = os.path.isfile(os.path.join(mods, "OoT Character Select", "characters.json"))
        doc = dict(self.lane_doc, **self.made)
        doc["actors"] = [{"actor": "P", "body": {"x": 1}}] if doc.get("body") else doc["actors"]
        _write(os.path.join(built, "coop_puppet.json"), json.dumps(doc))
        _write(os.path.join(built, "romfs", "same.bin"), "same")
        _write(os.path.join(built, "romfs", "table.bin"), "new")
        _write(os.path.join(built, "romfs", "body.bin"), "body")
        for rel, data in self.extra.items():
            _write(os.path.join(built, *rel.split("/")), data)
        return types.SimpleNamespace(returncode=0, stdout="done", stderr="")

    def make(self):
        subprocess.run = self.fake_run
        return self.m.make(self.out, self.ctx)

    def test_switches(self):
        os.environ.pop(self.m.ENV, None)
        self.assertEqual(self.m.switches(), ["body"])          # (the default)
        self.assertEqual(self.m.switches("pvp, body"), ["body", "pvp"])
        with self.assertRaises(RuntimeError):
            self.m.switches("bodies")
        with self.assertRaises(RuntimeError):
            self.m.switches(" , ")

    def test_only_changed_files_and_the_lane_command(self):
        files = self.make()
        self.assertEqual(sorted(files), ["coop_puppet.json", "romfs/body.bin", "romfs/table.bin"])
        cmd = self.cmds[0]
        import lanes
        self.assertEqual(cmd[cmd.index("--mod-dir") + 2:], list(lanes.PACK_TOOL_ARGS) + ["--puppet-body"])
        self.assertEqual(cmd[cmd.index("--romfs") + 1], self.ctx.romfs)
        self.assertTrue(self.saw_cs, "the build did not see lane A's Character Select")
        self.assertFalse(os.path.lexists(os.path.join(self.out, "mods", "OoT Character Select")), "junction left")
        self.assertTrue(os.path.isfile(os.path.join(self.game, "mods", "OoT Character Select", "characters.json")),
                        "the junction's removal touched lane A's Character Select")

    def test_switch_that_did_not_take(self):
        self.made = {"body": False}
        with self.assertRaisesRegex(RuntimeError, "body=False, not true"):
            self.make()

    def test_pack_not_built_as_the_lanes(self):
        self.made = {"body": True, "characters": None}
        with self.assertRaisesRegex(RuntimeError, "characters .* is not the lane pack's"):
            self.make()

    def test_lane_only_romfs_file(self):
        _write(os.path.join(self.pack, "romfs", "stale.bin"), "x")
        with self.assertRaisesRegex(RuntimeError, "romfs files this build does not make .*romfs/stale.bin"):
            self.make()

    def test_start_tunic_files_are_not_stale(self):
        """The kit's start puts own-tunic files into the pack's romfs (self_tunic.put: FIXED + self_files); those
        lane-only files are not a stale pack (dry make on lane1-A: 9 of them), any other lane-only romfs file is."""
        from puppet_pack import self_tunic
        _write(os.path.join(self.pack, "romfs", *self_tunic.FIXED[-1].split("/")), "start")
        self.lane_doc["self_files"] = {"blue": {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/blue/x"}}
        _write(os.path.join(self.pack, "coop_puppet.json"), json.dumps(self.lane_doc))
        _write(os.path.join(self.pack, "romfs", "Tex", "Angin_Tile_S_Alb.bntx.zs"), "start")
        files = self.make()
        self.assertNotIn("romfs/Tex/Angin_Tile_S_Alb.bntx.zs", files)
        _write(os.path.join(self.pack, "romfs", "Tex", "Other.bntx.zs"), "x")
        shutil.rmtree(self.out)
        os.makedirs(self.out)                      # (a fresh out, as testpack.make gives each maker)
        with self.assertRaisesRegex(RuntimeError, r"1 romfs files this build does not make \(romfs/Tex/Other.bntx.zs\)"):
            self.make()

    def test_build_failure(self):
        def bad(cmd, **kw):
            if cmd[:1] == ["cmd"]:
                return self.real_run(cmd, **kw)
            return types.SimpleNamespace(returncode=1, stdout="", stderr="error: dump")
        subprocess.run = bad
        with self.assertRaisesRegex(RuntimeError, "pack build failed .*error: dump"):
            self.m.make(self.out, self.ctx)
        self.assertFalse(os.path.lexists(os.path.join(self.out, "mods", "OoT Character Select")), "junction left")

    def test_env_lift_pvp(self):
        os.environ[self.m.ENV] = "lift,pvp"
        self.made = {"liftable": True, "pvp": True}
        self.make()
        cmd = self.cmds[0]
        self.assertEqual(cmd[-2:], ["--puppet-liftable", "--puppet-pvp"])


if __name__ == "__main__":
    unittest.main()
