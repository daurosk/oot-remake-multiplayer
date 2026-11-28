"""Patch 0023 (coop-null-guards): its tests (tools/hookshot/null_guards_cases_test.py) on Longshot's source with our patches
applied. The linux-longshot job runs it on the kit's data/builder/source after build.sh --renderer-only. The tests are
ours, not in the patch: the Linux AppImage ships no tests/, so a patch hunk there would stop the Linux kit. They import
the source's own nx2/coop_null_guards.py, a64_lift.py, nx2/fastfp.py, nx2/chunks.py and nx2/lift.py's chain, as the
game build uses them.
    python -I -B null_guards_test.py <patched source> [--cc gcc]
Fails when the source has no nx2/coop_null_guards.py, when any test fails, or when a test was skipped (the compiled-C
test needs a compiler): the guard's C must run once. Exit status 0 when all hold.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

CASES = Path(__file__).resolve().parent / "null_guards_cases_test.py"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--cc", default="")
    a = ap.parse_args()
    src = Path(a.source).resolve()
    if not (src / "nx2" / "coop_null_guards.py").is_file():
        print("FAIL: %s has no nx2/coop_null_guards.py (patch 0023 not applied)" % src)
        return 1
    env = dict(os.environ, COOP_LONGSHOT_SOURCE=str(src))
    if a.cc:
        env["CC"] = a.cc
    r = subprocess.run([sys.executable, "-I", "-B", str(CASES), "-v"], env=env, capture_output=True, text=True)
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
