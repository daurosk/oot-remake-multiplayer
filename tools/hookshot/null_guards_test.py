"""Patch 0023 (coop-null-guards): its own tests (tests/test_coop_null_guards.py in the patch) on Longshot's source with
our patches applied. The linux-longshot job runs it on the kit's data/builder/source after build.sh --renderer-only;
that source ships no tests/, so the test file is taken out of hookshot/patches/0023-*.patch and run from a temporary
folder with COOP_LONGSHOT_SOURCE pointing at the source (nx2/coop_null_guards.py and a64_lift.py's Lifter.unless are
the source's own, as the game build uses them).
    python -I -B null_guards_test.py <patched source> [--cc gcc]
Fails when the source has no nx2/coop_null_guards.py, when any test fails, or when the compiled-C test was skipped
(no compiler): the guard's C must run once. Exit status 0 when all hold.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PATCHES = REPO / "hookshot" / "patches"
TEST = "tests/test_coop_null_guards.py"


def new_file(text, path):
    """The text of a file the patch creates (its + lines)"""
    part = text[text.index("diff --git a/%s b/%s" % (path, path)):]
    end = part.find("\ndiff --git", 1)
    part = part if end < 0 else part[:end]
    body = part[part.index("\n@@ ") + 1:]
    lines = body.split("\n")[1:]
    return "\n".join(line[1:] for line in lines if line.startswith("+")) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--cc", default="")
    a = ap.parse_args()
    src = Path(a.source).resolve()
    if not (src / "nx2" / "coop_null_guards.py").is_file():
        print("FAIL: %s has no nx2/coop_null_guards.py (patch 0023 not applied)" % src)
        return 1
    names = sorted(n for n in os.listdir(PATCHES) if n.startswith("0023-"))
    if len(names) != 1:
        print("FAIL: expected one hookshot/patches/0023-*.patch, found %s" % names)
        return 1
    text = (PATCHES / names[0]).read_text(encoding="utf-8")
    env = dict(os.environ, COOP_LONGSHOT_SOURCE=str(src))
    if a.cc:
        env["CC"] = a.cc
    with tempfile.TemporaryDirectory() as tmp:
        test = Path(tmp) / "test_coop_null_guards.py"
        test.write_text(new_file(text, TEST), encoding="utf-8")
        r = subprocess.run([sys.executable, "-I", "-B", str(test), "-v"], env=env, capture_output=True, text=True)
    out = r.stdout + r.stderr
    print(out)
    if r.returncode != 0:
        print("FAIL: patch 0023's tests failed (exit %d)" % r.returncode)
        return 1
    if "skipped" in out:
        print("FAIL: a test was skipped (the compiled-C test needs a compiler: --cc)")
        return 1
    print("ok: patch 0023's tests on %s" % src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
