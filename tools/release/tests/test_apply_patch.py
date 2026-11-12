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

    def test_file_added_then_removed_by_a_later_patch(self):
        self.write_lift(LIFT)
        later = os.path.join(self.tmp.name, "0002.patch")
        with open(later, "w", newline="\n") as f:
            f.write("diff --git a/host/new.c b/host/new.c\ndeleted file mode 100644\n--- a/host/new.c\n+++ /dev/null\n"
                    "@@ -1,2 +0,0 @@\n-int x;\n-int y;\n")
        r = run(self.root, self.patch, later)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertFalse(os.path.exists(os.path.join(self.root, "host", "new.c")))
        self.assertIn("coop_hooks.install()", self.read("nx2", "lift.py"))


if __name__ == "__main__":
    unittest.main()
