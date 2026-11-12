"""Tests for tools/hookshot/hs.py: the cache key, the cache, the split locks, compile-all's plan. No build, no game."""
import io
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import hs  # noqa: E402

COMMIT = "c544828ff2d545829505cd894bb7968c7c2c5fcb"


def write(path, text, mode="w"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode, **({} if "b" in mode else {"encoding": "utf-8", "newline": ""})) as f:
        f.write(text)


class Fixture(unittest.TestCase):
    """A fake repo (PIN, TOOLS.txt, one patch) and a fake cache root."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.repo = os.path.join(self.tmp, "repo")
        write(os.path.join(self.repo, "hookshot", "PIN"), "repo=https://github.com/x/Hookshot\ncommit=%s\n" % COMMIT)
        write(os.path.join(self.repo, "tools", "release", "kit", "hookshot", "TOOLS.txt"),
              "# comment=ignored\nrelease=v2.1.0\nsetup_sha256=21a0\nsource_commit=%s\n" % COMMIT)
        write(os.path.join(self.repo, "hookshot", "patches", "0001-a.patch"), "patch one\n")
        self.cache = os.path.join(self.tmp, "cache")
        os.environ["OOT_COOP_HS_CACHE"] = self.cache
        self.out = io.StringIO()
        self.say = lambda *a: self.out.write(" ".join(str(x) for x in a) + "\n")

    def tearDown(self):
        os.environ.pop("OOT_COOP_HS_CACHE", None)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def game(self, name="built", stamp=None):
        g = os.path.join(self.tmp, name)
        write(os.path.join(g, "hookshot.exe"), "exe")
        write(os.path.join(g, "data", "image.bin"), "img")
        write(os.path.join(g, "hookshot_coop.dll"), "old dll")
        write(os.path.join(g, "coop.ini"), "server = me")
        write(os.path.join(g, "user", "save.bin"), "save")
        write(os.path.join(g, "hookshot_shaders.bin"), "shaders of the builder")
        write(os.path.join(g, "coop-built-from.txt"), stamp if stamp is not None else hs.stamp(self.repo))
        return g


class KeyTest(Fixture):
    def test_stamp_matches_the_kit_script(self):
        st = hs.stamp(self.repo)
        sha = hs.sha256_file(os.path.join(self.repo, "hookshot", "patches", "0001-a.patch"))
        self.assertEqual(st, "release=v2.1.0\ncommit=%s\npatches=%s" % (COMMIT, sha))

    def test_key_reads_keys_with_digits(self):
        # The earlier attempt's regex [a-z_]+ missed setup_sha256.
        keys = hs.read_keys(os.path.join(self.repo, "tools", "release", "kit", "hookshot", "TOOLS.txt"))
        self.assertEqual(keys["setup_sha256"], "21a0")
        self.assertNotIn("comment", keys)

    def test_key_changes_with_each_input(self):
        k0 = hs.key_of(hs.stamp(self.repo))
        self.assertRegex(k0, r"^[0-9a-f]{20}$")
        write(os.path.join(self.repo, "hookshot", "patches", "0001-a.patch"), "patch one changed\n")
        k1 = hs.key_of(hs.stamp(self.repo))
        write(os.path.join(self.repo, "hookshot", "patches", "0002-b.patch"), "two\n")
        k2 = hs.key_of(hs.stamp(self.repo))
        write(os.path.join(self.repo, "tools", "release", "kit", "hookshot", "TOOLS.txt"), "release=v2.2.0\n")
        k3 = hs.key_of(hs.stamp(self.repo))
        write(os.path.join(self.repo, "hookshot", "PIN"), "commit=%s\n" % ("0" * 40))
        k4 = hs.key_of(hs.stamp(self.repo))
        self.assertEqual(len({k0, k1, k2, k3, k4}), 5)

    def test_key_ignores_line_endings_and_files_that_are_not_patches(self):
        st = hs.stamp(self.repo)
        write(os.path.join(self.repo, "hookshot", "patches", "README.md"), "notes")
        self.assertEqual(hs.stamp(self.repo), st)
        self.assertEqual(hs.key_of(st), hs.key_of(st.replace("\n", "\r\n") + "\r\n"))

    def test_missing_pin_fails(self):
        write(os.path.join(self.repo, "hookshot", "PIN"), "repo=x\n")
        with self.assertRaises(hs.Fail):
            hs.stamp(self.repo)


class CacheTest(Fixture):
    def test_store_then_restore_keeps_per_player_files(self):
        k = hs.cache_store(self.game(), self.repo, say=self.say)
        cached = os.path.join(self.cache, k, "game")
        self.assertTrue(os.path.isfile(os.path.join(cached, "hookshot.exe")))
        self.assertTrue(os.path.isfile(os.path.join(cached, "data", "image.bin")))
        for per in ("hookshot_coop.dll", "coop.ini", "user"):
            self.assertFalse(os.path.exists(os.path.join(cached, per)), per)
        pair = os.path.join(self.tmp, "hookshot-A")
        write(os.path.join(pair, "coop.ini"), "server = pair")
        write(os.path.join(pair, "autotest-save", "s.bin"), "baseline")
        write(os.path.join(pair, "hookshot_shaders.bin"), "pair's own shaders")
        self.assertTrue(hs.cache_restore(pair, self.repo, say=self.say))
        self.assertTrue(os.path.isfile(os.path.join(pair, "hookshot.exe")))
        with open(os.path.join(pair, "coop.ini")) as f:
            self.assertEqual(f.read(), "server = pair")
        with open(os.path.join(pair, "hookshot_shaders.bin")) as f:
            self.assertEqual(f.read(), "pair's own shaders")
        self.assertTrue(os.path.isfile(os.path.join(pair, "autotest-save", "s.bin")))

    def test_restore_copies_only_changed_files(self):
        hs.cache_store(self.game(), self.repo, say=self.say)
        pair = os.path.join(self.tmp, "pair")
        hs.cache_restore(pair, self.repo, say=self.say)
        self.out.truncate(0)
        hs.cache_restore(pair, self.repo, say=self.say)
        self.assertIn("0 file(s) copied", self.out.getvalue())

    def test_store_refuses_a_game_of_another_stamp(self):
        with self.assertRaises(hs.Fail):
            hs.cache_store(self.game(stamp="release=v2.1.0\ncommit=x\npatches=old"), self.repo, say=self.say)
        self.assertEqual(hs.entries(), [])
        k = hs.cache_store(self.game("other", stamp="old"), self.repo, force=True, say=self.say)
        self.assertEqual([e[0] for e in hs.entries()], [k])

    def test_miss_and_dry_run_change_nothing(self):
        pair = os.path.join(self.tmp, "pair")
        self.assertFalse(hs.cache_restore(pair, self.repo, say=self.say))
        hs.cache_store(self.game(), self.repo, dry=True, say=self.say)
        self.assertFalse(os.path.exists(self.cache))

    def test_half_stored_entry_is_ignored_and_pruned(self):
        os.makedirs(os.path.join(self.cache, "deadbeef.part", "game"))
        self.assertEqual(hs.entries(), [])
        k = hs.cache_store(self.game(), self.repo, say=self.say)
        gone = hs.cache_prune(3, self.repo, say=self.say)
        self.assertEqual(gone, ["deadbeef.part"])
        self.assertEqual([e[0] for e in hs.entries()], [k])

    def test_prune_keeps_the_newest_and_the_current(self):
        cur = hs.cache_store(self.game(), self.repo, say=self.say)
        old = []
        for i in range(4):
            d = os.path.join(self.cache, "old%d" % i)
            write(os.path.join(d, "game", "hookshot.exe"), "x")
            write(os.path.join(d, "stamp.txt"), "s%d" % i)
            write(os.path.join(d, "used.txt"), "t")
            t = time.time() - 1000 + i * 10   # old3 newest of the old ones
            os.utime(os.path.join(d, "used.txt"), (t, t))
            old.append("old%d" % i)
        t = time.time() - 5000   # the current one is the least recently used, and still kept
        os.utime(os.path.join(self.cache, cur, "used.txt"), (t, t))
        gone = hs.cache_prune(2, self.repo, say=self.say)
        self.assertEqual(sorted(gone), ["old0", "old1", "old2"])
        self.assertEqual(sorted(os.listdir(self.cache)), sorted([cur, "old3"]))

    @unittest.skipUnless(hs.WINDOWS, "junctions are Windows")
    def test_links_are_kept_as_links(self):
        g = self.game()
        target = os.path.join(self.tmp, "shared-romfs")
        write(os.path.join(target, "big.bin"), "x" * 1000)
        hs.make_link(os.path.join(g, "romfs"), target)
        k = hs.cache_store(g, self.repo, say=self.say)
        self.assertTrue(hs.is_link(os.path.join(self.cache, k, "game", "romfs")))
        pair = os.path.join(self.tmp, "pair")
        hs.cache_restore(pair, self.repo, say=self.say)
        self.assertTrue(hs.is_link(os.path.join(pair, "romfs")))
        self.assertTrue(os.path.isfile(os.path.join(pair, "romfs", "big.bin")))
        hs.cache_prune(0, os.path.join(self.tmp, "no-repo"), say=self.say)   # no current key: removes all
        self.assertTrue(os.path.isfile(os.path.join(target, "big.bin")), "prune went into the junction")


class LockTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.t = [1_000_000.0]
        self.clock = lambda: self.t[0]
        self.slept = []

        def sleep(s):
            self.slept.append(s)
            self.t[0] += s
        self.sleep = sleep
        self.msgs = []
        self.say = self.msgs.append

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def take(self, which, owner, wait=0, alive=lambda pid: True):
        return hs.lock_acquire(self.root, which, owner, wait, 4.0, alive, self.sleep, self.clock, self.say)

    def owner(self, name):
        with open(os.path.join(self.root, name, "owner.txt")) as f:
            return json.load(f)

    def test_emu_and_hs_are_independent(self):
        self.assertTrue(self.take("emu", "integrator"))
        self.assertTrue(self.take("hs", "integrator"))
        self.assertEqual(self.owner("build-emu.lock")["owner"], "integrator")
        self.assertFalse(self.take("hs", "other"))
        self.assertFalse(hs.lock_release(self.root, "hs", "other", say=self.say))
        self.assertTrue(os.path.isdir(os.path.join(self.root, "build-hs.lock")))
        self.assertTrue(hs.lock_release(self.root, "hs", "integrator", say=self.say))
        self.assertTrue(self.take("hs", "other"))

    def test_old_build_lock_blocks_both(self):
        os.mkdir(os.path.join(self.root, "build.lock"))
        os.utime(os.path.join(self.root, "build.lock"), (self.t[0], self.t[0]))
        self.assertFalse(self.take("emu", "x"))
        self.assertFalse(self.take("hs", "x"))
        self.assertFalse(os.path.exists(os.path.join(self.root, "build-hs.lock")))

    def test_stale_old_lock_does_not_block_and_is_left(self):
        p = os.path.join(self.root, "build.lock")
        os.mkdir(p)
        os.utime(p, (self.t[0] - 5 * 3600,) * 2)
        self.assertTrue(self.take("hs", "x"))
        self.assertTrue(os.path.isdir(p))

    def test_stale_by_age_is_broken(self):
        self.assertTrue(self.take("hs", "old"))
        self.t[0] += 4 * 3600 + 1
        self.assertTrue(self.take("hs", "new"))
        self.assertEqual(self.owner("build-hs.lock")["owner"], "new")

    def test_stale_by_dead_pid_is_broken(self):
        self.assertTrue(self.take("emu", "crashed"))
        self.assertTrue(self.take("emu", "next", alive=lambda pid: False))
        self.assertEqual(self.owner("build-emu.lock")["owner"], "next")

    def test_wait_then_give_up(self):
        self.assertTrue(self.take("hs", "a"))
        self.assertFalse(self.take("hs", "b", wait=25))
        self.assertGreaterEqual(sum(self.slept), 25)
        self.assertIn("held by a", self.msgs[-1])

    def test_status(self):
        self.take("emu", "me")
        st = {name: (held, info.get("owner"), why) for name, held, info, why in
              hs.lock_status(self.root, alive=lambda pid: True)}
        self.assertEqual(st["build-emu.lock"][:2], (True, "me"))
        self.assertEqual(st["build-hs.lock"][0], False)
        self.assertEqual(st["build.lock"][0], False)


class CompileTest(unittest.TestCase):
    def test_hook_objects_from_ninja_targets(self):
        text = ("src/yuzu/CMakeFiles/yuzu.dir/main.cpp.obj: CXX_COMPILER__yuzu_Release\n"
                "src/core/CMakeFiles/core.dir/__/__/lichii/hook/coop_hook.cpp.obj: CXX_COMPILER__core_Release\n"
                "src/core/CMakeFiles/core.dir/__/__/lichii/hook/coop_puppet.cpp.obj: CXX_COMPILER__core_Release\n"
                "src/core/CMakeFiles/core.dir/__/__/lichii/hook/coop_hook.cpp.obj: CXX_COMPILER__core_Release\n"
                "lichii/hook/readme.md: phony\n")
        self.assertEqual(hs.hook_objects(text), [
            "src/core/CMakeFiles/core.dir/__/__/lichii/hook/coop_hook.cpp.obj",
            "src/core/CMakeFiles/core.dir/__/__/lichii/hook/coop_puppet.cpp.obj"])

    def test_unit_steps_configure_once(self):
        with tempfile.TemporaryDirectory() as b:
            steps = hs.unit_steps(b, 8, repo="R")
            self.assertEqual(steps[0][:6], ["cmake", "-S", "R", "-B", b, "-G"])
            self.assertIn("-DCOOP_BUILD_TESTS=ON", steps[0])
            write(os.path.join(b, "CMakeCache.txt"), "")
            steps = hs.unit_steps(b, 8, repo="R", target="hookshot_coop")
            self.assertEqual(steps, [["cmake", "--build", b, "--parallel", "8", "--target", "hookshot_coop"]])

    def test_dry_run_prints_the_plan_and_one_ok_line(self):
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as root, redirect_stdout(out):
            ok = hs.compile_all(os.path.join(root, "build"), emu_build="E", dry=True, root=root)
            self.assertEqual(os.listdir(root), [])   # no lock taken, nothing built
        self.assertTrue(ok)
        lines = out.getvalue().splitlines()
        self.assertTrue(lines[-1].startswith("compile-all: OK"))
        self.assertTrue(any(line.startswith("$ cmake --build") for line in lines))
        self.assertTrue(any("ninja -C E" in line for line in lines))


if __name__ == "__main__":
    unittest.main()
