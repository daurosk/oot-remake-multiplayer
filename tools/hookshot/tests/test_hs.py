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
        os.makedirs(os.path.join(self.cache, "deadbeefdeadbeefdead.part", "game"))
        self.assertEqual(hs.entries(), [])
        k = hs.cache_store(self.game(), self.repo, say=self.say)
        gone = hs.cache_prune(3, self.repo, say=self.say)
        self.assertEqual(gone, ["deadbeefdeadbeefdead.part"])
        self.assertEqual([e[0] for e in hs.entries()], [k])

    def test_prune_touches_only_cache_entries(self):
        # Review must-fix 2: anything else in the cache root stays (and a plain file does not crash prune).
        k = hs.cache_store(self.game(), self.repo, say=self.say)
        write(os.path.join(self.cache, "notes.txt"), "mine")
        write(os.path.join(self.cache, "my-folder", "x.txt"), "mine")
        write(os.path.join(self.cache, "0123456789abcdef0123"), "a file with a key's name")
        os.makedirs(os.path.join(self.cache, "fedcba9876543210fedc.part"))
        gone = hs.cache_prune(1, self.repo, say=self.say)
        self.assertEqual(gone, ["fedcba9876543210fedc.part"])
        self.assertEqual(sorted(os.listdir(self.cache)), sorted([k, "notes.txt", "my-folder", "0123456789abcdef0123"]))

    def test_prune_keeps_the_newest_and_the_current(self):
        cur = hs.cache_store(self.game(), self.repo, say=self.say)
        old = []
        for i in range(4):
            d = os.path.join(self.cache, "%020x" % (0xa0 + i))
            write(os.path.join(d, "game", "hookshot.exe"), "x")
            write(os.path.join(d, "stamp.txt"), "s%d" % i)
            write(os.path.join(d, "used.txt"), "t")
            t = time.time() - 1000 + i * 10   # old3 newest of the old ones
            os.utime(os.path.join(d, "used.txt"), (t, t))
            old.append("%020x" % (0xa0 + i))
        t = time.time() - 5000   # the current one is the least recently used, and still kept
        os.utime(os.path.join(self.cache, cur, "used.txt"), (t, t))
        gone = hs.cache_prune(2, self.repo, say=self.say)
        self.assertEqual(sorted(gone), ["%020x" % (0xa0 + i) for i in range(3)])
        self.assertEqual(sorted(os.listdir(self.cache)), sorted([cur, "%020x" % 0xa3]))

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


class WarmTest(Fixture):
    """The shader warm-up: which files, the copy, the restore of a pair that held another build, and the start
    (a fake game process and bridge: no Hookshot)."""

    WARM = ("hookshot_pipelines.bin", "hookshot_pipelines.bin.built", "hookshot_prepared.bin",
            "hookshot_programs.bin", "hookshot_programs.idx", "mkw_vk_pipelines.bin", "mkw_vk_pipelines_play_1.bin")

    def warmed(self, name="warm"):
        g = self.game(name)
        for n in self.WARM:
            write(os.path.join(g, n), "warm " + n)
        write(os.path.join(g, "hookshot_pipelines.bin.built.warming"), "1\n")   # not a warm file
        return g

    def test_warm_files_are_the_preparations_caches_only(self):
        g = self.warmed()
        self.assertEqual(hs.warm_files(g), sorted(self.WARM))
        for n in ("hookshot_shaders.bin", "hookshot_pipeline_states_shipped.bin", "hookshot_log.txt", "image.bin"):
            self.assertFalse(hs.WARM_FILES.match(n), n)
        for n in self.WARM:
            self.assertTrue(hs.RUNTIME_CACHE.match(n), n)   # never overwritten in a pair of the same build

    def test_spread_overwrites_and_drops_another_starts_play_files(self):
        src = self.warmed()
        dst = self.game("pair")
        write(os.path.join(dst, "hookshot_prepared.bin"), "cold")
        write(os.path.join(dst, "mkw_vk_pipelines_play_4.bin"), "a play file of another start")
        self.assertEqual(hs.warm_spread(src, [dst, src], say=self.say), len(self.WARM))   # src itself is skipped
        with open(os.path.join(dst, "hookshot_prepared.bin")) as f:
            self.assertEqual(f.read(), "warm hookshot_prepared.bin")
        self.assertFalse(os.path.exists(os.path.join(dst, "mkw_vk_pipelines_play_4.bin")))
        self.assertEqual(hs.warm_files(dst), sorted(self.WARM))

    def test_spread_refuses_a_folder_whose_preparation_did_not_finish(self):
        src = self.warmed()
        os.unlink(os.path.join(src, "hookshot_pipelines.bin.built"))
        with self.assertRaises(hs.Fail):
            hs.warm_spread(src, [self.game("pair")], say=self.say)

    def test_restore_replaces_the_runtime_caches_of_another_build(self):
        k = hs.cache_store(self.warmed("new"), self.repo, say=self.say)
        same = self.game("same")
        write(os.path.join(same, "hookshot_prepared.bin"), "this build's own, newer")
        hs.cache_restore(same, self.repo, say=self.say)
        with open(os.path.join(same, "hookshot_prepared.bin")) as f:
            self.assertEqual(f.read(), "this build's own, newer")   # same build: kept, as before
        old = self.game("old", stamp="release=v2.1.0\ncommit=x\npatches=old")
        write(os.path.join(old, "hookshot_prepared.bin"), "another translator's")
        hs.cache_restore(old, self.repo, say=self.say)
        with open(os.path.join(old, "hookshot_prepared.bin")) as f:
            self.assertEqual(f.read(), "warm hookshot_prepared.bin")
        self.assertIn("held another build", self.out.getvalue())
        with open(os.path.join(old, "coop.ini")) as f:
            self.assertEqual(f.read(), "server = me")   # per-player files still kept
        self.assertTrue(os.path.isdir(os.path.join(self.cache, k)))

    def test_warmup_starts_the_pair_then_fills_the_cache_and_the_others(self):
        hs.cache_store(self.game("built"), self.repo, say=self.say)
        pair, other = self.game("A1"), self.game("B1")
        started = []

        def start(game, out, timeout, say=print):
            started.append((game, timeout))
            for n in self.WARM:
                write(os.path.join(game, n), "warm " + n)
            return True

        self.assertTrue(hs.warmup(pair, [other], 99, repo=self.repo, say=self.say, start=start))
        self.assertEqual(started, [(pair, 99)])
        k = hs.key_of(hs.stamp(self.repo))
        for d in (other, os.path.join(self.cache, k, "game")):
            self.assertEqual(hs.warm_files(d), sorted(self.WARM), d)

    def test_warmup_refuses_a_pair_of_another_build_and_dry_run_starts_nothing(self):
        old = self.game("old", stamp="release=v2.1.0\ncommit=x\npatches=old")
        with self.assertRaises(hs.Fail):
            hs.warmup(old, repo=self.repo, say=self.say, start=lambda *a, **k: self.fail("started"))
        self.assertTrue(hs.warmup(self.game("A1"), dry=True, repo=self.repo, say=self.say,
                                  start=lambda *a, **k: self.fail("started")))

    def test_start_waits_for_the_line_then_quits_through_the_bridge(self):
        g = self.game("A1")
        out = os.path.join(self.tmp, "out")
        asked = []
        test = self

        class Proc:
            pid, returncode, polls = 7, None, 0

            def __init__(self, args, stdout=None, **kw):
                test.assertTrue(kw["env"]["HOS_CONTROL"])   # the bridge the quit goes through
                self.out = stdout

            def poll(self):
                self.polls += 1
                if self.polls == 2:   # the game's output, written while it runs
                    with open(os.path.join(out, "warmup-out.txt"), "a") as f:
                        f.write("[hos host] vk: start-up: the shaders took 1081.5 s\n")
                return self.returncode

            def wait(self, t=None):
                self.returncode = 0
                return 0

            def kill(self):
                asked.append("kill")

        class Bridge:
            def request(self, cmd, **k):
                asked.append(cmd)

            def close(self):
                pass

        ok = hs.warm_start(g, out, 600, say=self.say, popen=Proc, connect=lambda s, timeout: Bridge(),
                           clock=iter(range(0, 10000, 5)).__next__, sleep=lambda s: None)
        self.assertTrue(ok)
        self.assertEqual(asked, ["quit"])
        self.assertIn("the shaders took 1081.5 s", self.out.getvalue())

    def test_start_that_ends_early_is_a_failure(self):
        g = self.game("A1")

        class Proc:
            pid, returncode = 8, 3

            def __init__(self, *a, **k):
                pass

            def poll(self):
                return self.returncode

        self.assertFalse(hs.warm_start(g, os.path.join(self.tmp, "out"), 600, say=self.say, popen=Proc,
                                       connect=lambda s, timeout: None, sleep=lambda s: None))
        self.assertIn("ended before its shaders were prepared", self.out.getvalue())


class BuildSpeedTest(Fixture):
    """hs-build-speed: the pin's shared folders, Hookshot's own jobs, warm shader files seeded (dry runs, no build)."""

    def kit(self, new=True):
        k = os.path.join(self.tmp, "kit")
        script = ("param([string]$Game = '', [string]$Out = '', [int]$Jobs = 0, [string]$Tools = '', [string]$Work = '')\n"
                  if new else "param([string]$Game = '', [string]$Out = '', [int]$Jobs = 0)\n")
        write(os.path.join(k, "scripts", "hookshot-build.ps1"), script)
        return k

    def test_pin_of_and_its_folder(self):
        st = hs.stamp(self.repo)
        self.assertEqual(hs.pin_of(st), "v2.1.0-%s" % COMMIT[:12])
        self.assertEqual(hs.pin_of(st.replace("patches=", "patches=other,")), hs.pin_of(st))   # patches: same folder
        self.assertEqual(hs.pin_dir(st), os.path.join(self.cache, "pins", hs.pin_of(st)))
        self.assertEqual(hs.pin_of("release=v2.2.0-rc.1\ncommit=4aee23659b9436dab2cd\n"), "v2.2.0-rc.1-4aee23659b94")
        with self.assertRaises(hs.Fail):
            hs.pin_of("patches=x")
        self.assertFalse(hs.CACHE_NAME.match("pins"))   # cache_prune leaves the shared folders alone

    def test_script_args(self):
        self.assertEqual(hs.build_script_args(os.path.join(self.kit(), "scripts", "hookshot-build.ps1")), {"Tools", "Work"})
        self.assertEqual(hs.build_script_args(os.path.join(self.kit(False), "scripts", "hookshot-build.ps1")), set())
        self.assertEqual(hs.build_script_args(os.path.join(self.tmp, "none.ps1")), set())

    def test_a_miss_builds_in_the_pins_folders_with_hookshots_jobs(self):
        kit = self.kit()
        hs.build([os.path.join(self.tmp, "A1")], os.path.join(self.tmp, "b"), kit=kit, game="g.nup", repo=self.repo,
                 dry=True, say=self.say)
        out = self.out.getvalue()
        shared = hs.pin_dir(hs.stamp(self.repo))
        line = [ln for ln in out.splitlines() if ln.startswith("$ ") and "hookshot-build.ps1" in ln][0]
        self.assertIn('-Tools "%s"' % os.path.join(shared, "tools") if " " in shared else os.path.join(shared, "tools"), line)
        self.assertIn(os.path.join(shared, "work"), line)
        self.assertIn(os.path.join(shared, "game"), line)   # the pin's one game folder: the lift key stays the same
        self.assertIn("would store %s as" % os.path.join(shared, "game"), out)
        self.assertNotIn(".build", line)
        self.assertNotIn("-Jobs", line)   # Hookshot's own choice: one compiler per CPU thread
        self.assertIn("--parallel %d" % (os.cpu_count() or 8), out)   # the module: every thread too
        self.assertIn("game folder", out)

    def test_an_old_kit_builds_as_before_and_jobs_are_passed_when_asked(self):
        hs.build([os.path.join(self.tmp, "A1")], os.path.join(self.tmp, "b"), kit=self.kit(False), jobs=12,
                 repo=self.repo, dry=True, say=self.say)
        out = self.out.getvalue()
        line = [ln for ln in out.splitlines() if ln.startswith("$ ") and "hookshot-build.ps1" in ln][0]
        self.assertNotIn("-Work", line)
        self.assertIn("-Jobs 12", line)
        self.assertIn("a full build in the kit's own folders", out)
        self.assertIn(".build", line)   # an old kit: a new folder, removed after the store

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

    def test_owner_txt_never_holds_the_pc_name(self):
        self.assertTrue(self.take("hs", "integrator"))
        host = self.owner("build-hs.lock")["host"]
        self.assertEqual(host, hs.host_id())
        self.assertNotIn(hs.socket.gethostname(), host)

    def test_a_shell_lock_has_no_pid_and_goes_stale_by_age_only(self):
        # `hs.py lock acquire` ends at once: its own pid must not make the lock stale (review must-fix 1).
        self.assertTrue(hs.lock_acquire(self.root, "emu", "integrator", 0, 4.0, lambda pid: False, self.sleep,
                                        self.clock, self.say, pid=0))
        self.assertIsNone(self.owner("build-emu.lock")["pid"])
        self.assertFalse(self.take("emu", "other", alive=lambda pid: False))
        self.assertEqual(self.owner("build-emu.lock")["owner"], "integrator")
        self.t[0] += 4 * 3600 + 1
        self.assertTrue(self.take("emu", "other", alive=lambda pid: False))

    def test_cli_acquire_twice(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(hs.main(["lock", "acquire", "--name", "hs", "--owner", "a", "--root", self.root]), 0)
            self.assertEqual(hs.main(["lock", "acquire", "--name", "hs", "--owner", "b", "--root", self.root]), 1)
            self.assertEqual(hs.main(["lock", "release", "--name", "hs", "--owner", "b", "--root", self.root]), 1)
            self.assertEqual(hs.main(["lock", "release", "--name", "hs", "--owner", "a", "--root", self.root]), 0)
        self.assertNotIn("stale", out.getvalue())

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
            # A configure that failed: the cache is there, build.ninja is not -> configure again.
            write(os.path.join(b, "CMakeCache.txt"), "")
            steps = hs.unit_steps(b, 8, repo="R", target="hookshot_coop")
            self.assertEqual(len(steps), 2)
            self.assertEqual(steps[0][:2], ["cmake", "-S"])
            write(os.path.join(b, "build.ninja"), "")
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
