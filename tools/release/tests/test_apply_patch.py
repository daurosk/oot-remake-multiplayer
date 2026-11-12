"""Tests for tools/release/kit/scripts/apply_patch.py (the kit's git-less patch applier for Hookshot).

    python tools/release/tests/test_apply_patch.py -v
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "kit", "scripts", "apply_patch.py")

PATCH = """From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001
From: someone <someone@localhost>
Subject: [PATCH] co-op

---
 lift.py   | 2 ++
 new.c     | 2 ++
 2 files changed

diff --git a/nx2/lift.py b/nx2/lift.py
index 1111111..2222222 100644
--- a/nx2/lift.py
+++ b/nx2/lift.py
@@ -2,4 +2,6 @@ def main():
     a = 1
     b = 2
+    import coop_hooks
+    coop_hooks.install()
     c = 3
     d = 4
diff --git a/host/new.c b/host/new.c
new file mode 100644
index 0000000..3333333
--- /dev/null
+++ b/host/new.c
@@ -0,0 +1,2 @@
+int x;
+int y;
--
2.47.0
"""

LIFT = "def main():\n    a = 1\n    b = 2\n    c = 3\n    d = 4\n"


def run(root, *patches):
    return subprocess.run([sys.executable, "-I", "-B", TOOL, root, *patches], capture_output=True, text=True)


class ApplyPatchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, "src")
        os.makedirs(os.path.join(self.root, "nx2"))
        os.makedirs(os.path.join(self.root, "host"))
        self.patch = os.path.join(self.tmp.name, "0001.patch")
        with open(self.patch, "w", newline="\n") as f:
            f.write(PATCH)

    def tearDown(self):
        self.tmp.cleanup()

    def write_lift(self, text, eol="\n"):
        with open(os.path.join(self.root, "nx2", "lift.py"), "wb") as f:
            f.write(text.replace("\n", eol).encode())

    def read(self, *parts):
        with open(os.path.join(self.root, *parts), "rb") as f:
            return f.read().decode()

    def test_applies(self):
        self.write_lift(LIFT)
        r = run(self.root, self.patch)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual(self.read("nx2", "lift.py"),
                         "def main():\n    a = 1\n    b = 2\n    import coop_hooks\n    coop_hooks.install()\n    c = 3\n    d = 4\n")
        self.assertEqual(self.read("host", "new.c"), "int x;\nint y;\n")

    def test_keeps_crlf(self):
        self.write_lift(LIFT, "\r\n")
        self.assertEqual(run(self.root, self.patch).returncode, 0)
        self.assertIn("    import coop_hooks\r\n", self.read("nx2", "lift.py"))
        self.assertNotIn("\n\n", self.read("nx2", "lift.py").replace("\r\n", "|"))

    def test_second_run_refused_and_nothing_written(self):
        self.write_lift(LIFT)
        self.assertEqual(run(self.root, self.patch).returncode, 0)
        before = self.read("nx2", "lift.py")
        r = run(self.root, self.patch)
        self.assertEqual(r.returncode, 1)
        self.assertIn("does not match", r.stdout)
        self.assertEqual(self.read("nx2", "lift.py"), before)

    def test_changed_context_refused_and_nothing_written(self):
        self.write_lift(LIFT.replace("b = 2", "b = 7"))
        r = run(self.root, self.patch)
        self.assertEqual(r.returncode, 1)
        self.assertIn("nx2/lift.py", r.stdout)
        self.assertFalse(os.path.exists(os.path.join(self.root, "host", "new.c")))

    def test_path_outside_refused(self):
        self.write_lift(LIFT)
        bad = os.path.join(self.tmp.name, "bad.patch")
        with open(bad, "w", newline="\n") as f:
            f.write("diff --git a/../x b/../x\n--- /dev/null\n+++ b/../x\n@@ -0,0 +1 @@\n+x\n")
        r = run(self.root, bad)
        self.assertEqual(r.returncode, 1)
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "x")))


PATCH2 = """diff --git a/nx2/lift.py b/nx2/lift.py
--- a/nx2/lift.py
+++ b/nx2/lift.py
@@ -2,4 +2,5 @@ def main():
     a = 1
     b = 2
+    import coop_hooks2
     c = 3
     d = 4
"""

HOST = "int keep;\n"


def refresh(keep, root, *patches):
    return subprocess.run([sys.executable, "-I", "-B", TOOL, "--refresh", keep, root, *patches],
                          capture_output=True, text=True)


class RefreshTest(unittest.TestCase):
    """--refresh (hs-build-speed): a new patch set over an earlier one, with only the changed files written."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, "src")
        self.keep = os.path.join(self.tmp.name, "keep")
        os.makedirs(os.path.join(self.root, "nx2"))
        os.makedirs(os.path.join(self.root, "host"))
        with open(os.path.join(self.root, "nx2", "lift.py"), "w", newline="\n") as f:
            f.write(LIFT)
        with open(os.path.join(self.root, "host", "other.c"), "w", newline="\n") as f:
            f.write(HOST)
        self.p1 = os.path.join(self.tmp.name, "0001.patch")
        self.p2 = os.path.join(self.tmp.name, "0002.patch")
        with open(self.p1, "w", newline="\n") as f:
            f.write(PATCH)
        with open(self.p2, "w", newline="\n") as f:
            f.write(PATCH2)

    def tearDown(self):
        self.tmp.cleanup()

    def read(self, *parts):
        with open(os.path.join(self.root, *parts), "rb") as f:
            return f.read().decode()

    def mtime(self, *parts):
        return os.stat(os.path.join(self.root, *parts)).st_mtime_ns

    def age(self):
        # every file a known old time: a rewrite shows as a new one
        for d, _, fs in os.walk(self.root):
            for n in fs:
                os.utime(os.path.join(d, n), ns=(10**18, 10**18))

    def test_first_run_as_plain_then_again_writes_nothing(self):
        r = refresh(self.keep, self.root, self.p1)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("import coop_hooks\n", self.read("nx2", "lift.py"))
        self.assertEqual(self.read("host", "new.c"), "int x;\nint y;\n")
        self.age()
        r = refresh(self.keep, self.root, self.p1)   # the same set again: applied to the originals, nothing changes
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("0 file(s) written", r.stdout)
        for f in (("nx2", "lift.py"), ("host", "new.c"), ("host", "other.c")):
            self.assertEqual(self.mtime(*f), 10**18, f)

    def test_a_changed_set_is_applied_to_the_originals(self):
        self.assertEqual(refresh(self.keep, self.root, self.p1).returncode, 0)
        self.age()
        r = refresh(self.keep, self.root, self.p2)   # 0001 gone, 0002 in: lift.py from its original, new.c removed
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual(self.read("nx2", "lift.py"),
                         "def main():\n    a = 1\n    b = 2\n    import coop_hooks2\n    c = 3\n    d = 4\n")
        self.assertFalse(os.path.exists(os.path.join(self.root, "host", "new.c")))
        self.assertEqual(self.mtime("host", "other.c"), 10**18)   # never patched: untouched
        r = refresh(self.keep, self.root)   # no patch at all: the source as it came
        self.assertEqual(r.returncode, 2)   # (the usage: at least one patch)
        self.assertEqual(refresh(self.keep, self.root, self.p1, self.p2).returncode, 1)   # both touch the same lines
        self.assertEqual(self.read("nx2", "lift.py"),
                         "def main():\n    a = 1\n    b = 2\n    import coop_hooks2\n    c = 3\n    d = 4\n")

    def test_a_dropped_patch_gives_the_original_back(self):
        self.assertEqual(refresh(self.keep, self.root, self.p2).returncode, 0)
        self.assertEqual(refresh(self.keep, self.root, self.p1).returncode, 0)
        # lift.py from its original (not 0002's) with 0001's lines, and 0001's new file
        self.assertEqual(self.read("nx2", "lift.py"),
                         "def main():\n    a = 1\n    b = 2\n    import coop_hooks\n    coop_hooks.install()\n"
                         "    c = 3\n    d = 4\n")
        self.assertTrue(os.path.exists(os.path.join(self.root, "host", "new.c")))

    def test_bad_patch_writes_nothing(self):
        self.assertEqual(refresh(self.keep, self.root, self.p1).returncode, 0)
        before = self.read("nx2", "lift.py")
        bad = os.path.join(self.tmp.name, "bad.patch")
        with open(bad, "w", newline="\n") as f:
            f.write(PATCH2.replace("     b = 2", "     b = 9"))
        r = refresh(self.keep, self.root, bad)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(self.read("nx2", "lift.py"), before)
        self.assertTrue(os.path.exists(os.path.join(self.root, "host", "new.c")))

    def test_a_run_stopped_half_way_applies_cleanly_next_time(self):
        # (review should-fix) the list is written before the files: a stop after the first file leaves it naming that
        # file with its original, so the next run applies to the original again instead of to a patched file.
        import importlib.util
        spec = importlib.util.spec_from_file_location("apply_patch", TOOL)
        ap = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ap)
        with open(self.p1, encoding="utf-8") as f:
            text1 = f.read()
        real = ap.write_if_changed
        written = []

        def stop_after_one(target, data):
            if os.path.join(self.root, "") in target and written:
                raise KeyboardInterrupt("the window was closed")
            r = real(target, data)
            if os.path.join(self.root, "") in target:
                written.append(target)
            return r

        ap.write_if_changed = stop_after_one
        with self.assertRaises(KeyboardInterrupt):
            ap.refresh(self.root, self.keep, [("0001", text1)])
        self.assertEqual(len(written), 1)   # one file of the source changed, then the stop
        ap.write_if_changed = real
        r = refresh(self.keep, self.root, self.p1)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("import coop_hooks\n", self.read("nx2", "lift.py"))
        self.assertEqual(self.read("nx2", "lift.py").count("import coop_hooks"), 1)
        self.assertEqual(self.read("host", "new.c"), "int x;\nint y;\n")

if __name__ == "__main__":
    unittest.main()
