"""Tests for tools/autotest/groups.py: the group file, each group's own pair/port/pipe, the parallel runner (with a fake
run.ps1) and the merged PASS/FAIL table from saved run folders. No game is started."""
import io
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import types
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import groups as g  # noqa: E402

GROUPS = """
# a few groups on Hookshot
[g1]
walk
teleport-same  switches=LICHII_COOP_CHEATS=1,A:LICHII_COOP_COLOR=red  expect="A:coop: connected"  label="#178 door"

[g2]
chest  expect="B:got item \\d+"  expect=puppet  timeout=900
"""


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


class ParseTest(unittest.TestCase):
    def test_parse(self):
        gs = g.parse_groups(GROUPS)
        self.assertEqual([n for n, _ in gs], ["g1", "g2"])
        walk, door = gs[0][1]
        self.assertEqual(walk, {"scenario": "walk", "switches": "", "expects": [], "timeout": None, "label": ""})
        self.assertEqual(door["switches"], "LICHII_COOP_CHEATS=1,A:LICHII_COOP_COLOR=red")
        self.assertEqual(door["expects"], [("A", "coop: connected")])
        self.assertEqual(door["label"], "#178 door")
        chest = gs[1][1][0]
        self.assertEqual(chest["expects"], [("B", r"got item \d+"), ("", "puppet")])
        self.assertEqual(chest["timeout"], 900)

    def test_errors(self):
        for text in ("walk\n", "[g1]\n", "[g1]\nwalk\n[g1]\nchest\n", "[g1]\nwalk nope\n", "[g1]\nwalk color=red\n",
                     "[g1]\nwalk expect=(\n", "[g1]\nwalk timeout=ten\n", "[g1]\n../x\n", "", "[g1]\nwalk label=\"x\n"):
            with self.assertRaises(g.Fail, msg=text):
                g.parse_groups(text)


class PlanTest(unittest.TestCase):
    def test_each_group_has_its_own_pair_ports_pipe_and_folder(self):
        planned = g.plan(g.parse_groups(GROUPS + "[g3]\nwalk\n"), r"K:\kit", r"K:\kit\shots-hookshot\groups-1", "S.exe")
        s1, s2, s3 = (p[0] for p in planned)
        self.assertEqual((s1["a"], s1["b"]), (os.path.join(r"K:\kit", "hookshot-A1"), os.path.join(r"K:\kit", "hookshot-B1")))
        self.assertEqual((s1["port"], s2["port"], s3["port"]), (27560, 27570, 27580))
        self.assertEqual((s1["pipe"], s2["pipe"]), ("coop-auto-g1", "coop-auto-g2"))
        self.assertNotEqual(s1["shots"], s2["shots"])
        cmd = planned[0][2][1]
        self.assertEqual(cmd[cmd.index("-Scenario") + 1], "teleport-same")
        self.assertEqual(cmd[cmd.index("-Port") + 1], "27560")
        self.assertEqual(cmd[cmd.index("-Pipe") + 1], "coop-auto-g1")
        self.assertEqual(cmd[cmd.index("-Runtime") + 1], "hookshot")
        self.assertEqual(cmd[cmd.index("-Switches") + 1], "LICHII_COOP_CHEATS=1,A:LICHII_COOP_COLOR=red")
        self.assertEqual(cmd[cmd.index("-Server") + 1], "S.exe")
        self.assertNotIn("-Switches", planned[0][2][0])
        self.assertEqual(planned[1][2][0][planned[1][2][0].index("-Timeout") + 1], "900")

    def test_overlap_is_refused(self):
        s1 = g.group_setup(1, "g1", "K", "R")
        s2 = dict(g.group_setup(2, "g2", "K", "R"), port=27562)   # inside g1's relay ports
        with self.assertRaises(g.Fail):
            g.check_distinct([s1, s2])
        s3 = dict(g.group_setup(2, "g2", "K", "R"), pipe="coop-auto-g1")
        with self.assertRaises(g.Fail):
            g.check_distinct([s1, s3])

    def test_dry_run_prints_every_command_and_starts_nothing(self):
        planned = g.plan(g.parse_groups(GROUPS), "K", "R", "S.exe")
        out = []
        called = []
        self.assertEqual(g.run_groups(planned, dry=True, say=out.append, runner=called.append), [])
        self.assertEqual(called, [])
        self.assertEqual(sum(1 for line in out if "run.ps1" in line), 3)
        self.assertTrue(out[0].startswith("[g1] pair"))


class RunTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_groups_run_in_parallel_and_runs_in_order(self):
        planned = g.plan(g.parse_groups(GROUPS), self.tmp, os.path.join(self.tmp, "R"), "S.exe")
        active, peak, order, lock = [0], [0], [], threading.Lock()

        def runner(cmd):
            name = cmd[cmd.index("-Scenario") + 1]
            shots = cmd[cmd.index("-Shots") + 1]
            with lock:
                active[0] += 1
                peak[0] = max(peak[0], active[0])
                order.append(name)
            time.sleep(0.2)
            folder = os.path.join(shots, name, "20261112-120000")
            write(os.path.join(folder, "steps.txt"), "scenario played\n")
            with lock:
                active[0] -= 1
            return types.SimpleNamespace(returncode=0 if name != "chest" else 1, stdout="hookshot: ...\n%s\n" % folder,
                                         stderr="")

        res = g.run_groups(planned, say=lambda *_: None, runner=runner)
        self.assertEqual(peak[0], 2)
        self.assertLess(order.index("walk"), order.index("teleport-same"))
        self.assertEqual(sorted(r[1]["scenario"] for r in res), ["chest", "teleport-same", "walk"])
        chest = next(r for r in res if r[1]["scenario"] == "chest")
        with open(os.path.join(chest[2], "run.json"), encoding="utf-8") as f:
            meta = json.load(f)
        self.assertEqual((meta["group"], meta["exit"], meta["port"]), ("g2", 1, 27570))

    def test_a_run_without_folder_leaves_a_note(self):
        planned = g.plan(g.parse_groups("[g1]\nwalk\n"), self.tmp, os.path.join(self.tmp, "R"), "S.exe")
        res = g.run_groups(planned, say=lambda *_: None,
                           runner=lambda cmd: types.SimpleNamespace(returncode=1, stdout="no server\n", stderr="x"))
        self.assertIsNone(res[0][2])
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, "R", "g1", "walk-no-folder.txt")))

    def test_prepare_copies_the_base_pairs_files_once(self):
        planned = g.plan(g.parse_groups("[g1]\nwalk\n[g2]\nwalk\n"), self.tmp, os.path.join(self.tmp, "R"), "S")
        write(os.path.join(self.tmp, "hookshot-A", "autotest-save", "slot_00", "progress.sav"), "a")
        write(os.path.join(self.tmp, "hookshot-B", "coop-options.ini"), "b")
        restored = []
        ok = g.prepare(planned, self.tmp, say=lambda *_: None, restore=lambda f, dry: restored.append(f) or True)
        self.assertTrue(ok)
        self.assertEqual(len(restored), 4)
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, "hookshot-A2", "autotest-save", "slot_00",
                                                    "progress.sav")))
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, "hookshot-B1", "coop-options.ini")))
        write(os.path.join(self.tmp, "hookshot-A1", "autotest-save", "mine.sav"), "kept")
        write(os.path.join(self.tmp, "hookshot-A", "autotest-save", "new.sav"), "new")
        self.assertTrue(g.prepare(planned, self.tmp, say=lambda *_: None, restore=None))
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, "hookshot-A1", "autotest-save", "mine.sav")))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "hookshot-A1", "autotest-save", "new.sav")))
        self.assertFalse(g.prepare(planned, self.tmp, dry=True, say=lambda *_: None, restore=lambda f, d: False))


class MergeTest(unittest.TestCase):
    """Saved run folders as hookshot.py and run.ps1 leave them."""

    def setUp(self):
        self.root = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def run_folder(self, group, name, meta=None, files=None):
        f = os.path.join(self.root, group, name, "20261112-120000")
        write(os.path.join(f, "steps.txt"), "A: started\nscenario played\n")
        if meta is not None:
            write(os.path.join(f, "run.json"), json.dumps(dict({"group": group, "scenario": name}, **meta)))
        for k, v in (files or {}).items():
            write(os.path.join(f, k), v)
        return f

    def test_table(self):
        log_a = "[1.0] coop: module loaded\n[2.0] coop: connected to 127.0.0.1:27560\n"
        log_b = "[3.0] puppet: P1 spawned\n[9.0] got item 42 from the chest\n"
        self.run_folder("g1", "walk", {"expects": [], "exit": 0})
        self.run_folder("g1", "teleport-same", {"expects": [["A", "coop: connected"]], "exit": 0, "label": "#178"},
                        {"hookshot_log-A.txt": log_a})
        self.run_folder("g2", "chest", {"expects": [["B", r"got item \d+"], ["", "puppet"]], "exit": 0},
                        {"hookshot_log-A.txt": log_a, "hookshot_log-B.txt": log_b})
        table = g.merge([self.root], rel_to=self.root)
        lines = table.splitlines()
        self.assertEqual(lines[0], "| Run | Result | Evidence |")
        self.assertIn("| g1 teleport-same (#178) | PASS | g1/teleport-same/20261112-120000: A: '[2.0] coop: connected to "
                      "127.0.0.1:27560' |", lines)
        self.assertTrue(any(line.startswith("| g1 walk | PLAYED |") for line in lines))
        self.assertTrue(any(line.startswith("| g2 chest | PASS |") and "got item 42" in line for line in lines))
        self.assertEqual(lines[-1], "3 run(s): 2 PASS, 1 PLAYED -> PASS")

    def test_failures(self):
        self.run_folder("g1", "failed", {"exit": 1}, {"FAILED.txt": "play: A timeout at line 12\n"})
        self.run_folder("g1", "counts", {"exit": 0}, {"counts.txt": "PASS a\nFAIL puppet not seen\nFAIL b\n"})
        self.run_folder("g2", "exit", {"exit": 3})
        self.run_folder("g2", "missing", {"exit": 0, "expects": [["B", "coop: connected"]]},
                        {"hookshot_log-A.txt": "coop: connected\n"})
        f = self.run_folder("g2", "unplayed", {"exit": 0})
        write(os.path.join(f, "steps.txt"), "A: started\n")
        rows = {line.split(" | ")[0][2:]: line for line in g.merge([self.root]).splitlines() if line.startswith("| g")}
        self.assertIn("FAILED.txt: play: A timeout at line 12", rows["g1 failed"])
        self.assertIn("counts.txt: 2 FAIL, first: FAIL puppet not seen", rows["g1 counts"])
        self.assertIn("run.ps1 exit 3", rows["g2 exit"])
        self.assertIn("missing B:'coop: connected'", rows["g2 missing"])
        self.assertIn("no 'scenario played'", rows["g2 unplayed"])
        for row in rows.values():
            self.assertIn("| FAIL |", row)
        self.assertTrue(g.merge([self.root]).endswith("5 run(s): 5 FAIL -> FAIL"))

    def test_folders_without_run_json_and_empty(self):
        self.run_folder("g1", "walk")   # a plain run.ps1 -Runtime hookshot folder
        self.assertIn("| walk | PLAYED |", g.merge([self.root]))
        empty = tempfile.mkdtemp()
        try:
            self.assertTrue(g.merge([empty]).endswith("0 run(s): none -> FAIL"))
        finally:
            shutil.rmtree(empty)

    def test_cli_merge_writes_the_table(self):
        self.run_folder("g1", "walk", {"exit": 0})
        out = os.path.join(self.root, "results.md")
        with redirect_stdout(io.StringIO()):
            self.assertEqual(g.main(["merge", self.root, "--out", out]), 0)
        with open(out, encoding="utf-8") as f:
            self.assertIn("1 run(s): 1 PLAYED -> PASS", f.read())


if __name__ == "__main__":
    unittest.main()
