"""Tests for tools/hookshot/hs.py's shared game files: romfs hard-linked by copy_tree (one copy of the ~26 GB for every
pin, cache entry and test pair), and a new pin's game files taken from an older pin (Longshot's --romfs-from).
Made-up folders in a temp folder; no build, no game.
    python tools/hookshot/tests/test_hs_share.py -v"""
import io
import os
import shutil
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import hs  # noqa: E402

COMMIT = "c544828ff2d545829505cd894bb7968c7c2c5fcb"


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.cache = os.path.join(self.tmp, "cache")
        os.environ["OOT_COOP_HS_CACHE"] = self.cache
        hs.LINKS_REFUSED.clear()   # (one run's memory: each test is a run of its own)
        self.out = io.StringIO()
        self.say = lambda *a: self.out.write(" ".join(str(x) for x in a) + "\n")

    def tearDown(self):
        os.environ.pop("OOT_COOP_HS_CACHE", None)
        shutil.rmtree(self.tmp, ignore_errors=True)


class ShareTest(Base):
    """copy_tree: the top-level romfs is hard-linked file by file; everything else is still copied."""

    def game(self):
        g = os.path.join(self.tmp, "src")
        write(os.path.join(g, "longshot.exe"), "exe")
        write(os.path.join(g, "romfs", "Scene", "a.zs"), "scene a")
        write(os.path.join(g, "romfs", "b.bin"), "b")
        write(os.path.join(g, "data", "romfs", "c.txt"), "not the game files: a deeper romfs is copied")
        return g

    def test_romfs_is_linked_the_rest_copied(self):
        src, dst = self.game(), os.path.join(self.tmp, "dst")
        hs.copy_tree(src, dst, say=self.say)
        for rel in (("romfs", "Scene", "a.zs"), ("romfs", "b.bin")):
            self.assertTrue(os.path.samefile(os.path.join(src, *rel), os.path.join(dst, *rel)), rel)
            self.assertGreaterEqual(os.stat(os.path.join(dst, *rel)).st_nlink, 2)
        for rel in (("longshot.exe",), ("data", "romfs", "c.txt")):
            self.assertFalse(os.path.samefile(os.path.join(src, *rel), os.path.join(dst, *rel)), rel)
            self.assertEqual(read(os.path.join(dst, *rel)), read(os.path.join(src, *rel)))

    def test_again_changes_nothing_and_an_old_full_copy_becomes_a_link(self):
        src, dst = self.game(), os.path.join(self.tmp, "dst")
        hs.copy_tree(src, dst, say=self.say)
        self.assertEqual(hs.copy_tree(src, dst, say=self.say), (0, 0))
        # (a folder made before: its own copy of a game file, the same size and time) is linked now, freeing its space
        old = os.path.join(dst, "romfs", "b.bin")
        os.unlink(old)
        shutil.copy2(os.path.join(src, "romfs", "b.bin"), old)
        self.assertFalse(os.path.samefile(old, os.path.join(src, "romfs", "b.bin")))
        copied, _ = hs.copy_tree(src, dst, say=self.say)
        self.assertEqual(copied, 1)
        self.assertTrue(os.path.samefile(old, os.path.join(src, "romfs", "b.bin")))

    def test_a_changed_game_file_replaces_the_link_and_never_writes_through_it(self):
        src, dst = self.game(), os.path.join(self.tmp, "dst")
        other = os.path.join(self.tmp, "other")
        hs.copy_tree(src, dst, say=self.say)
        hs.copy_tree(dst, other, say=self.say)   # (a third folder sharing the same file)
        new = os.path.join(self.tmp, "new.zs")
        write(new, "scene a, changed")
        os.unlink(os.path.join(src, "romfs", "Scene", "a.zs"))
        shutil.copy2(new, os.path.join(src, "romfs", "Scene", "a.zs"))
        hs.copy_tree(src, dst, say=self.say)
        self.assertEqual(read(os.path.join(dst, "romfs", "Scene", "a.zs")), "scene a, changed")
        self.assertEqual(read(os.path.join(other, "romfs", "Scene", "a.zs")), "scene a")   # the other folder kept its own

    def test_where_links_are_refused_the_copies_are_not_made_again(self):
        # (a cache on another drive, a FAT disk: every link refused) the first run copies, the second copies nothing
        src, dst = self.game(), os.path.join(self.tmp, "dst")
        real = os.link
        try:
            def refuse(*_a, **_k):
                raise OSError("no hard links here")
            os.link = refuse
            first, _ = hs.copy_tree(src, dst, say=self.say)
            hs.LINKS_REFUSED.clear()   # (a new run: it finds out again, with one file at most)
            second, _ = hs.copy_tree(src, dst, say=self.say)
            third, _ = hs.copy_tree(src, dst, say=self.say)
        finally:
            os.link = real
        self.assertEqual(first, 4)   # (the exe, two game files, data/romfs/c.txt)
        self.assertLessEqual(second, 1)
        self.assertEqual(third, 0)
        self.assertEqual(read(os.path.join(dst, "romfs", "Scene", "a.zs")), "scene a")

    def test_can_link(self):
        class St:
            def __init__(self, dev, nlink=1):
                self.st_dev, self.st_nlink = dev, nlink
        self.assertTrue(hs.can_link(St(1), St(1)))
        self.assertFalse(hs.can_link(St(1), St(2)))   # (another drive: a copy stays a copy)
        self.assertFalse(hs.can_link(St(1, hs.NTFS_MAX_LINKS), St(1)))   # (NTFS: no more names for that file)
        hs.LINKS_REFUSED.add((1, 1))
        self.assertFalse(hs.can_link(St(1), St(1)))   # (refused once in this run)

    def test_a_link_that_cannot_be_made_is_a_copy(self):
        src = os.path.join(self.tmp, "s.bin")
        write(src, "x")
        d = os.path.join(self.tmp, "d.bin")
        real = os.link
        try:
            def refuse(*_a, **_k):
                raise OSError("another drive")
            os.link = refuse
            self.assertFalse(hs.link_or_copy(src, d))
        finally:
            os.link = real
        self.assertEqual(read(d), "x")
        self.assertFalse(os.path.samefile(src, d))
        self.assertTrue(hs.link_or_copy(src, d))   # (an old d is unlinked first, then linked)
        self.assertTrue(os.path.samefile(src, d))


class RomfsFromTest(Base):
    """A new pin's game folder takes the game files of the newest older pin that unpacked its own."""

    def pin(self, name, own=True, age=0):
        g = os.path.join(self.cache, "pins", name, "game")
        os.makedirs(g, exist_ok=True)
        if own:
            write(os.path.join(g, "romfs", "a.zs"), "a")
            write(os.path.join(g, ".longshot_romfs"), "game.nup 123\n")
            t = time.time() - age
            os.utime(os.path.join(g, ".longshot_romfs"), (t, t))
        return g

    def test_the_newest_other_pin_with_its_own_game_files(self):
        old = self.pin("v3.0.0-rc.4-53f96d780588", age=200)
        newer = self.pin("v3.0.0-rc.5-e4d035e9e71a", age=100)
        self.pin("v3.0.0-rc.3-aaaaaaaaaaaa", own=False)   # (no stamp: not a source)
        out = os.path.join(self.cache, "pins", "v3.0.0-rc.6-181571bdaa66", "game")
        self.assertEqual(hs.romfs_source(out), newer)
        self.assertNotEqual(hs.romfs_source(out), old)

    def test_none_when_the_pin_has_its_own_or_there_is_no_other(self):
        out = os.path.join(self.cache, "pins", "v3.0.0-rc.6-181571bdaa66", "game")
        self.assertIsNone(hs.romfs_source(out))   # (no pins at all)
        self.pin("v3.0.0-rc.5-e4d035e9e71a")
        mine = self.pin("v3.0.0-rc.6-181571bdaa66")
        self.assertIsNone(hs.romfs_source(mine))   # (its own unpack comes first, as in Longshot)

    def kit(self):
        k = os.path.join(self.tmp, "kit")
        write(os.path.join(k, "scripts", "hookshot-build.ps1"),
              "param([string]$Game = '', [string]$Out = '', [int]$Jobs = 0, [string]$Tools = '', [string]$Work = '',\n"
              "      [string]$RomfsFrom = '')\n")
        return k

    def repo(self):
        r = os.path.join(self.tmp, "repo")
        write(os.path.join(r, "hookshot", "PIN"), "repo=https://example.invalid/Longshot\ncommit=%s\n" % COMMIT)
        write(os.path.join(r, "tools", "release", "kit", "hookshot", "TOOLS.txt"),
              "# comment=ignored\nrelease=v3.0.0-rc.6\nsetup_sha256=21a0\nsource_commit=%s\n" % COMMIT)
        write(os.path.join(r, "hookshot", "patches", "0001-a.patch"), "patch one\n")
        return r

    def build_line(self, game):
        hs.build([os.path.join(self.tmp, "A1")], os.path.join(self.tmp, "b"), kit=self.kit(), game=game,
                 repo=self.repo(), dry=True, say=self.say)
        return [ln for ln in self.out.getvalue().splitlines() if ln.startswith("$ ") and "hookshot-build.ps1" in ln][0]

    def test_a_miss_passes_the_older_pin_for_a_nup(self):
        self.assertEqual(hs.build_script_args(os.path.join(self.kit(), "scripts", "hookshot-build.ps1")),
                         {"Tools", "Work", "RomfsFrom"})
        older = self.pin("v3.0.0-rc.5-e4d035e9e71a")
        line = self.build_line("game.nup")
        self.assertIn("-RomfsFrom", line)
        self.assertIn(older if " " not in older else '"%s"' % older, line)
        self.assertIn("the new pin's game files come from", self.out.getvalue())

    def test_not_for_romfs_and_exefs_folders(self):
        self.pin("v3.0.0-rc.5-e4d035e9e71a")
        self.assertNotIn("-RomfsFrom", self.build_line(os.path.join(self.tmp, "romfs")))


if __name__ == "__main__":
    unittest.main()
