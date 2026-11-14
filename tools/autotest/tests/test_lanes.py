"""Tests for tools/autotest/lanes.py (two test lanes on one PC), groups.py --lane and hs.py's lane-safe copies and
locks. Fake game folders in a temp dir; no game is started."""
import io
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import lanes  # noqa: E402
import groups  # noqa: E402
hs = lanes.hs


def write(path, text="x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def nlink(path):
    return os.stat(path).st_nlink


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.kit = os.path.join(self.tmp, "coop-kit")
        self.out = io.StringIO()
        self.say = lambda *a: self.out.write(" ".join(str(x) for x in a) + "\n")
        self.idle = lambda folder: []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def game(self, name):
        g = os.path.join(self.tmp, name)
        for f in ("longshot.exe", "SDL3.dll", "image.bin", "longshot_pipeline_states_shipped.bin",
                  "hookshot_coop.dll", "longshot_programs.bin", "mkw_vk_pipelines.bin", "longshot_pipelines.bin.built",
                  "coop-options.ini", "longshot_log.txt", "longshot_pipelines.bin.built.warming"):
            write(os.path.join(g, f), name + ":" + f)
        write(os.path.join(g, "romfs", "Pack", "a.pack"), "pack")
        write(os.path.join(g, "user", "save", "u", "slot_00", "game.sav"), "save")
        write(os.path.join(g, "autotest-save", "account.sav"), "base")
        return g


class LaneTable(Base):
    def test_two_lanes_share_nothing_and_avoid_the_owner_and_groups_ports(self):
        ls = [lanes.lane(n, self.kit) for n in lanes.LANES]
        lanes.check_distinct(ls)
        for L in ls:
            self.assertNotIn(lanes.OWNER_PORT, L["ports"])
            self.assertTrue(all(p < 27560 for p in L["ports"]))   # groups.py starts at 27560
        self.assertEqual(ls[0]["a"], os.path.join(self.kit, "ls", "lane1-A"))
        self.assertEqual((ls[0]["port"], ls[1]["port"]), (27520, 27530))
        self.assertNotEqual(ls[0]["pipe"], ls[1]["pipe"])

    def test_clash_is_refused(self):
        L = lanes.lane(1, self.kit)
        with self.assertRaises(lanes.Fail):
            lanes.check_distinct([L, dict(L, lane=2, a="x", b="y", shots="z", pipe="p")])   # same ports
        with self.assertRaises(lanes.Fail):
            lanes.check_distinct([dict(L, ports=[27510])])
        with self.assertRaises(lanes.Fail):
            lanes.lane(3, self.kit)

    def test_run_ps1_and_hookshot_py_take_the_lane(self):
        with open(os.path.join(HERE, "..", "run.ps1"), encoding="utf-8") as f:
            ps = f.read()
        self.assertIn("lanes.py\" info --lane $Lane --json", ps)
        self.assertIn("lock acquire --lane $Lane", ps)
        with open(os.path.join(HERE, "..", "hookshot.py"), encoding="utf-8") as f:
            self.assertIn('"--lane"', f.read())


class Fill(Base):
    def test_read_only_files_are_hard_links_everything_else_copies(self):
        src = self.game("A1")
        L = lanes.setup(1, src, self.game("B1"), say=self.say, kit=self.kit, busy=self.idle)
        a = L["a"]
        for f in ("longshot.exe", "SDL3.dll", "image.bin", "longshot_pipeline_states_shipped.bin",
                  os.path.join("romfs", "Pack", "a.pack")):
            self.assertTrue(os.path.samefile(os.path.join(src, f), os.path.join(a, f)), f)
        for f in ("hookshot_coop.dll", "longshot_programs.bin", "mkw_vk_pipelines.bin", "longshot_pipelines.bin.built",
                  "coop-options.ini", os.path.join("user", "save", "u", "slot_00", "game.sav"),
                  os.path.join("autotest-save", "account.sav")):
            self.assertEqual(nlink(os.path.join(a, f)), 1, f)
            self.assertEqual(read(os.path.join(a, f)), read(os.path.join(src, f)))
        # per run: never taken over (a stale marker made a game prepare every shader at each start)
        self.assertFalse(os.path.exists(os.path.join(a, "longshot_log.txt")))
        self.assertFalse(os.path.exists(os.path.join(a, "longshot_pipelines.bin.built.warming")))
        self.assertEqual(lanes.problems(1, self.kit, busy=self.idle), [])

    def test_refill_never_writes_into_a_shared_inode(self):
        src = self.game("A1")
        lanes.setup(1, src, self.game("B1"), say=self.say, kit=self.kit, busy=self.idle)
        dst = os.path.join(lanes.lane(1, self.kit)["a"], "coop-options.ini")
        os.unlink(dst)
        os.link(os.path.join(src, "coop-options.ini"), dst)   # someone hard-linked a written file
        self.assertTrue(any("hard-linked but the game writes it" in p for p in lanes.problems(1, self.kit, busy=self.idle)))
        write(os.path.join(src, "coop-options.ini"), "changed in the source")
        time.sleep(0.01)
        lanes.fill(src, os.path.dirname(dst), say=self.say)
        self.assertEqual(nlink(dst), 1)
        self.assertEqual(read(os.path.join(src, "coop-options.ini")), "changed in the source")

    def test_check_finds_a_stale_marker_and_a_missing_game(self):
        lanes.setup(1, self.game("A1"), self.game("B1"), say=self.say, kit=self.kit, busy=self.idle)
        write(os.path.join(lanes.lane(1, self.kit)["b"], "longshot_pipelines.bin.built.warming"), "1")
        ps = lanes.problems(1, self.kit, busy=self.idle)
        self.assertEqual(len(ps), 1)
        self.assertIn("stale shader-preparation marker", ps[0])
        self.assertIn("no game", lanes.problems(2, self.kit, busy=self.idle)[0])

    def test_setup_refuses_a_running_lane(self):
        with self.assertRaises(lanes.Fail):
            lanes.setup(1, self.game("A1"), self.game("B1"), say=self.say, kit=self.kit, busy=lambda f: ["game.exe"])

    def test_install_replaces_only_the_module_and_never_through_a_link(self):
        src = self.game("A1")
        L = lanes.setup(2, src, self.game("B1"), say=self.say, kit=self.kit, busy=self.idle)
        dll = os.path.join(self.tmp, "new", "hookshot_coop.dll")
        write(dll, "new module")
        exe_before = os.path.getmtime(os.path.join(L["a"], "longshot.exe"))
        lanes.install(2, dll, say=self.say, kit=self.kit, busy=self.idle)
        for g in (L["a"], L["b"]):
            self.assertEqual(read(os.path.join(g, "hookshot_coop.dll")), "new module")
        self.assertEqual(read(os.path.join(src, "hookshot_coop.dll")), "A1:hookshot_coop.dll")
        self.assertEqual(os.path.getmtime(os.path.join(L["a"], "longshot.exe")), exe_before)
        with self.assertRaises(lanes.Fail):
            lanes.install(2, dll, say=self.say, kit=self.kit, busy=lambda f: ["running"])


class Locks(Base):
    def setUp(self):
        super().setUp()
        self.root = os.path.join(self.tmp, "root")
        os.makedirs(self.root)

    def test_two_lanes_run_at_once_a_game_build_stops_both(self):
        q = lambda *a: None
        self.assertTrue(lanes.lock_acquire(1, "run one", root=self.root, say=q))
        self.assertTrue(lanes.lock_acquire(2, "run two", root=self.root, say=q))
        self.assertFalse(lanes.lock_acquire(1, "run three", root=self.root, say=q))   # lane 1 is busy
        hs.lock_release(self.root, "lane1", say=q)
        hs.lock_release(self.root, "lane2", say=q)
        self.assertTrue(hs.lock_acquire(self.root, "game", "a game build", say=q))
        self.assertFalse(lanes.lock_acquire(1, "run", root=self.root, say=q))
        self.assertFalse(lanes.lock_acquire(2, "run", root=self.root, say=q))
        hs.lock_release(self.root, "game", say=q)
        self.assertTrue(lanes.lock_acquire(2, "run", root=self.root, say=q))

    def test_a_build_starting_between_take_and_check_wins(self):
        """The lane takes its lock, then a game build takes build-game.lock before the lane looks: the lane lets its lock
        go again, so the build (which looks for lane locks after taking its own) and the run never both go."""
        q = lambda *a: None

        def build_slips_in(root):
            hs.lock_acquire(root, "game", "a game build", say=q)
            return lanes.blockers(root)
        self.assertFalse(lanes.lock_acquire(1, "run", root=self.root, say=q, check=build_slips_in))
        self.assertFalse(os.path.isdir(os.path.join(self.root, "lane1.lock")))
        self.assertTrue(os.path.isdir(os.path.join(self.root, "build-game.lock")))
        # and the other order: the lane's lock is there when the build looks
        hs.lock_release(self.root, "game", say=q)
        self.assertTrue(lanes.lock_acquire(2, "run", root=self.root, say=q))
        self.assertFalse(hs.wait_free(os.path.join(self.root, "lane2.lock"), 0, say=q))

    def test_module_build_lock_blocks_no_lane_but_a_hand_made_one_does(self):
        q = lambda *a: None
        self.assertTrue(hs.lock_acquire(self.root, "hs", "compile-all", say=q))   # hs.py's JSON owner
        self.assertEqual(lanes.blockers(self.root), [])
        hs.lock_release(self.root, "hs", say=q)
        os.makedirs(os.path.join(self.root, "build-hs.lock"))
        write(os.path.join(self.root, "build-hs.lock", "owner.txt"), "someone 23:20 UTC game run")
        self.assertEqual(len(lanes.blockers(self.root)), 1)
        self.assertFalse(lanes.lock_acquire(1, "run", root=self.root, say=q))

    def test_build_waits_for_running_lanes(self):
        q = lambda *a: None
        hs.lock_acquire(self.root, "lane1", "a run", say=q)
        t = [0.0]
        ok = hs.wait_free(os.path.join(self.root, "lane1.lock"), 30, say=q, sleep=lambda s: t.__setitem__(0, t[0] + s),
                          clock=lambda: t[0])
        self.assertFalse(ok)
        hs.lock_release(self.root, "lane1", say=q)
        self.assertTrue(hs.wait_free(os.path.join(self.root, "lane1.lock"), 30, say=q))


class CopyTreeLinks(Base):
    def test_cache_copy_unlinks_a_hard_link_before_writing(self):
        src, dst, other = (os.path.join(self.tmp, n) for n in ("cache", "lane", "other"))
        write(os.path.join(other, "SDL3.dll"), "old")
        os.makedirs(dst)
        os.link(os.path.join(other, "SDL3.dll"), os.path.join(dst, "SDL3.dll"))
        write(os.path.join(src, "SDL3.dll"), "new build")
        hs.copy_tree(src, dst, say=self.say)
        self.assertEqual(read(os.path.join(dst, "SDL3.dll")), "new build")
        self.assertEqual(read(os.path.join(other, "SDL3.dll")), "old")


class GroupsInALane(Base):
    def test_every_group_on_the_lane_one_after_another(self):
        gs = groups.parse_groups("[g1]\nwalk\n[g2]\nchest\n")
        planned = groups.plan(gs, self.kit, os.path.join(self.tmp, "runs"), "server.exe", lane=2)
        L = lanes.lane(2, self.kit)
        for s, runs, cmds in planned:
            self.assertEqual((s["a"], s["port"], s["pipe"]), (L["a"], L["port"], L["pipe"]))
            self.assertEqual(cmds[0][-2:], ["-Lane", "2"])
        order = []
        groups.run_groups(planned, say=lambda *a: None,
                          runner=lambda cmd: order.append(cmd[cmd.index("-Scenario") + 1]) or
                          type("P", (), {"returncode": 0, "stdout": "", "stderr": ""})())
        self.assertEqual(order, ["walk", "chest"])

    def test_without_a_lane_groups_keep_their_own_pairs(self):
        gs = groups.parse_groups("[g1]\nwalk\n[g2]\nchest\n")
        planned = groups.plan(gs, self.kit, os.path.join(self.tmp, "runs"), "server.exe")
        self.assertNotEqual(planned[0][0]["port"], planned[1][0]["port"])
        self.assertNotIn("-Lane", planned[0][2][0])


class Cli(Base):
    def test_info_json(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(lanes.main(["info", "--lane", "1", "--json"]), 0)
        self.assertEqual(json.loads(out.getvalue())["port"], 27520)


if __name__ == "__main__":
    unittest.main()
