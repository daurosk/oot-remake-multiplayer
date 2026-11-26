"""Patch 0008 on Longshot 3.1's staged saves (review-01111-b, 2026-11-22): the account save switched between folders
(the player's own user\\save\\<uid>, a guest's <uid>-coop, the multiplayer saves' <uid>-mp) through the module's
hos_coop_fs_save_remount, run through Longshot's own nn::fs HLEs without a game.

Every folder must stage in its own work copy (save-work\\<folder's name>) and publish only into itself at Commit: going
back to the own folder after a session as a guest writes the player's own save, never the co-op folder (the bug: the
co-op work copy, still bound to <uid>-coop, took the own folder's saves). Built with Longshot's harness
(tests/test_fs_save_commit.py of the patched source: its stand-ins and its Fs driver) and Longshot's llvm-mingw, as the
CI windows job's host_compile_check.ps1 lays them out.

    python -I -B coop_save_remount_test.py <patched Longshot source> <llvm-mingw folder>
"""
import ctypes
import importlib.util
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def harness(src):
    spec = importlib.util.spec_from_file_location("ls_fs_save_commit", os.path.join(src, "tests", "test_fs_save_commit.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Save:
    def __init__(self, h, lib, root):
        self.f = h.Fs(lib, root)
        self.root, self.uid = root, h.UID
        self.remount_fn = lib.hos_coop_fs_save_remount
        self.remount_fn.argtypes, self.remount_fn.restype = [ctypes.c_char_p], ctypes.c_int
        self.root_fn = lib.hos_coop_fs_save_root
        self.root_fn.argtypes, self.root_fn.restype = [ctypes.c_char_p, ctypes.c_size_t], ctypes.c_int

    def folder(self, suffix):
        return self.root / "save" / (self.uid + suffix)

    def work(self, name):
        return self.root / "save-work" / name

    def remount(self, path):
        return self.remount_fn(os.fsencode(str(path)))

    def mounted_root(self):
        buf = ctypes.create_string_buffer(1024)
        assert self.root_fn(buf, len(buf)) == 1, "hos_coop_fs_save_root: no save root\n" + self.f.text()
        return Path(os.fsdecode(buf.value))

    def save(self, data, rel="progress.sav"):
        self.f.call("create", self.f.path(rel), 0)   # (already there: refused, and the file is overwritten below)
        h = self.f.open(rel)
        assert self.f.call("resize", h, len(data)) == 0
        assert self.f.write(h, data) == 0
        self.f.close(h)
        assert self.f.commit() == 0, "Commit failed\n" + self.f.text()


def read(path):
    return path.read_bytes() if path.exists() else None


def worker(src, libpath, root):
    h = harness(src)
    s = Save(h, ctypes.CDLL(libpath), Path(root))
    own, coop, mp = s.folder(""), s.folder("-coop"), s.folder("-mp")

    def expect(want, step):
        for path, data in want.items():
            got = read(path / "progress.sav")
            assert got == data, "%s: %s holds %r, want %r\n%s" % (step, path, got, data, s.f.text())

    s.f.mount()
    s.save(b"own-1")
    expect({own: b"own-1"}, "own first")
    assert s.mounted_root() == own

    # (asking the folder twice keeps no lock: Longshot 3.1's hos_fs_save_root holds save_lock until hos_fs_save_end, and
    #  the remount below would wait for ever)
    assert s.mounted_root() == own

    # a guest: <uid>-coop, in its own work copy
    assert s.remount(coop) == 1, "remount to -coop\n" + s.f.text()
    assert s.mounted_root() == coop
    s.save(b"guest-1")
    expect({own: b"own-1", coop: b"guest-1"}, "guest")
    assert read(s.work(s.uid + "-coop") / "progress.sav") == b"guest-1"

    # back to the own folder (the review's case): Longshot's own work copy save-work\<uid>, published into own only
    assert s.remount(own) == 1, "remount back to own\n" + s.f.text()
    assert s.mounted_root() == own, "back on own: the save root is %s" % s.mounted_root()
    s.save(b"own-2")
    expect({own: b"own-2", coop: b"guest-1"}, "own after a session as a guest")
    assert read(s.work(s.uid) / "progress.sav") == b"own-2"
    assert read(s.work(s.uid + "-coop") / "progress.sav") == b"guest-1"

    # a guest again: the co-op folder as it was, then its own new save
    assert s.remount(coop) == 1
    assert s.mounted_root() == coop
    s.save(b"guest-2")
    expect({own: b"own-2", coop: b"guest-2"}, "guest again")

    # the multiplayer saves (<uid>-mp): a third folder, a third work copy
    assert s.remount(mp) == 1
    assert s.mounted_root() == mp
    s.save(b"mp-1")
    expect({own: b"own-2", coop: b"guest-2", mp: b"mp-1"}, "multiplayer saves")
    assert read(s.work(s.uid + "-mp") / "progress.sav") == b"mp-1"

    # the own folder named another way (a trailing separator; on Windows another case) is still the own folder
    named = str(own) + "\\"
    if sys.platform == "win32":
        named = named.upper()
    assert s.remount(named) == 1
    s.save(b"own-3")
    expect({own: b"own-3", coop: b"guest-2", mp: b"mp-1"}, "own named another way")

    # the game mounts its save again (a title reload, Try Again): the folder switched to stays
    assert s.remount(coop) == 1
    s.f.unmount()
    s.f.mount()
    assert s.mounted_root() == coop, "the game's own mount after a switch went to %s" % s.mounted_root()
    s.save(b"guest-3")
    expect({own: b"own-3", coop: b"guest-3", mp: b"mp-1"}, "the game's own mount on the co-op folder")
    s.f.unmount()
    # one work copy per folder, and no other
    works = sorted(p.name for p in (s.root / "save-work").iterdir() if p.is_dir())
    assert works == sorted([s.uid, s.uid + "-coop", s.uid + "-mp"]), "work copies: %s" % works
    print("PASS: patch 0008 on staged saves: own, guest, own, guest, multiplayer, own again, the game's own mount; "
          "each folder written only into itself")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--worker":
        worker(*sys.argv[2:5])
        return
    src, mingw = (os.path.abspath(a) for a in sys.argv[1:3])
    h = harness(src)
    cc = os.path.join(mingw, "bin", "x86_64-w64-mingw32-clang.exe")
    with tempfile.TemporaryDirectory(prefix="coop-save-remount-") as temporary:
        tmp = Path(temporary)
        (tmp / "a64.h").write_text(h.A64)
        # (+ the one nn_fs.c call Longshot 3.1's harness stand-ins leave out: a Windows DLL links every name, where
        #  Linux's .so left it unresolved; oot_damaged_files.c's stand-in for an incomplete dump, never one here.
        #  Longshot 3.2's harness has it itself, and a second definition does not compile)
        extra = "" if "hos_stand_in_damaged" in h.STUBS else (
            "\nint hos_stand_in_damaged(const char *guest, char *path, size_t cap) "
            "{ (void)guest; (void)path; (void)cap; return 0; }\n")
        (tmp / "stubs.c").write_text(h.STUBS + extra)
        libpath = tmp / "fs.dll"
        # (Longshot's own Windows build of the harness: tests/test_fs_save_commit.py main; string.h first, as the game's
        #  build has it before hos.h, whose inline helpers use memset and memcpy)
        subprocess.run([cc, "-std=gnu11", "-shared", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
                        "-I", str(tmp), "-I", os.path.join(src, "host"), "-include", "string.h",
                        "-include", os.path.join(src, "gpu", "vk", "compat", "longshot_env.h"),
                        os.path.join(src, "host", "nn_fs.c"), str(tmp / "stubs.c"),
                        "-Wl,--export-all-symbols", "-o", str(libpath)], check=True)
        # (a process of its own: the save's owner handle is held to process exit, as in the game)
        subprocess.run([sys.executable, "-I", "-B", os.path.abspath(__file__), "--worker", src, str(libpath),
                        str(tmp / "portable")], check=True, timeout=300)   # (a save lock kept: a hang, failed here)


if __name__ == "__main__":
    main()
