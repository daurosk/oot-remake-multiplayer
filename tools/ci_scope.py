"""Build's scope job: does this run need the C++ jobs (windows, linux, linux-longshot) or only python-tools?

B #131 2026-11-26: ~10 full Builds queued, most for branches that touch only tools/autotest python, each running the
whole C++ matrix. A manual run on a work branch whose diff (vs its merge-base with --base) touches only the paths in
SCOPED runs python-tools only; everything else runs the full matrix. main, integrate-*, release-*, tags and pull
requests always run full (they are the proofs a release rests on).

SCOPED is the one table: folders whose files no C++ job, kit script or kit test reads (checked 2026-11-26 against
build.yml's windows/linux/linux-longshot steps, tools/release/*.ps1|*.sh and CMakeLists.txt). tools/hookshot,
tools/puppet_pack, tools/release, tools/nucleus, tools/ci-runner, tools/coop_test and tools/playtest are NOT in it:
the kit or a C++/windows job uses them. Adding a folder here needs that same check.

BRANCH TIER (OWNER 2026-11-28 00:16 UTC via B #160): a manual run on a work branch is the branch tier (build.yml: scope,
python-tools ubuntu+windows 3.12, windows, linux (clang), and linux-longshot unless EVERY changed file is one that job
never reads: LONGSHOT_NEVER_READ). asan, tsan, gcc and Python 3.10 run only on integrate-*/main/release-*/tag Builds.
`longshot=true|false` says whether linux-longshot runs (any doubt = true).

BASE (same go): --base auto (the default) picks the origin/integrate-* whose merge-base with HEAD is the newest commit
(committer time), so a branch off integrate-0121 is not diffed against a stale integrate-0120 (that sent every tests-only
branch to the full Build). A named --base (the workflow's `base` input) overrides it.

Prints `full=true|false` and `longshot=true|false` (for $GITHUB_OUTPUT) on stdout and one `scope: ...` line on stderr and in $GITHUB_STEP_SUMMARY.
Any doubt (unknown event, no merge-base, git error, empty diff) = full.
"""
import argparse
import fnmatch
import os
import re
import subprocess
import sys

SCOPED = (
    "docs/**",
    "**.md",
    "tools/autotest/**",
    "tools/game_scan/**",
    "tools/netsim/**",
    "tools/discord-bot/**",
    "tools/rando/**",
    "tools/tests/**",
    "tools/ci_scope.py",
    "tools/check_content.py",
    "tools/address_index.py",
    "tools/check_actions.py",
    "tools/check_actions_test.py",
)

ALWAYS_FULL_REF = re.compile(r"^refs/(heads/(main|integrate-.*|release-.*)|tags/.*)$")
# linux-longshot is skipped in the branch tier only when EVERY changed file is one it never reads (review-mod-153-2
# 2026-11-28 00:23: it builds CMakeLists.txt, cmake/, coop/, server/, hookshot/, reads tools/release (make_kit_linux.sh,
# linux_longshot_kit_test.sh, kit-paths.ps1, tests/fixtures), tools/puppet_pack, tools/hookshot and build.yml itself).
# The SCOPED python-only paths, plus: the Windows-only test scripts directly in tools/release/tests (*.ps1, none named by
# make_kit_linux.sh, linux_longshot_kit_test.sh, host_compile_check.sh or kit_pack_check.py; fixtures/ excluded), and the
# content guard workflow. Adding a path here needs that same check; any doubt = linux-longshot runs.
LONGSHOT_NEVER_READ_FILES = (".github/workflows/content-guard.yml",)
LONGSHOT_NEVER_READ_DIR_EXT = (("tools/release/tests", ".ps1"),)
BASE_NAME = re.compile(r"^[A-Za-z0-9._/-]+$")


def scoped_path(path):
    """True when path matches a SCOPED pattern ('**' = any depth, '**.md' = any .md file)."""
    for pat in SCOPED:
        if pat == "**.md":
            if path.lower().endswith(".md"):
                return True
        elif pat.endswith("/**"):
            if path.startswith(pat[:-2]):
                return True
        elif fnmatch.fnmatchcase(path, pat):
            return True
    return False


def decide(event, ref, files):
    """(full, reason). Only a workflow_dispatch on a work branch can be python-only."""
    if event != "workflow_dispatch":
        return True, "full (event %s)" % (event or "?")
    if ALWAYS_FULL_REF.match(ref or ""):
        return True, "full (%s always runs full)" % ref
    if files is None:
        return True, "full (no diff: merge-base not found)"
    if not files:
        return True, "full (empty diff)"
    outside = [f for f in files if not scoped_path(f)]
    if outside:
        return True, "full (files outside the python-only paths: %s)" % ", ".join(outside[:10])
    return False, "python-only (files: %s)" % ", ".join(files[:20]) + (" +%d more" % (len(files) - 20) if len(files) > 20 else "")


def longshot_never_read(path):
    """True when linux-longshot never reads path (see LONGSHOT_NEVER_READ_*)."""
    if scoped_path(path) or path in LONGSHOT_NEVER_READ_FILES:
        return True
    d, _, name = path.rpartition("/")
    return any(d == folder and name.lower().endswith(ext) for folder, ext in LONGSHOT_NEVER_READ_DIR_EXT)


def longshot_needed(files):
    """True when linux-longshot must run in the branch tier: unknown/empty diff, or any file it may read."""
    if not files:
        return True
    return not all(longshot_never_read(f) for f in files)


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True, text=True, check=True).stdout


def changed_files(base):
    """Files changed between HEAD and its merge-base with origin/<base>, or None when that is not known.
    --no-renames: a file moved out of a C++ path lists its old path too (review-c123-8 10:47), so the move runs full."""
    if not BASE_NAME.match(base or ""):
        return None
    try:
        git("fetch", "--no-tags", "--filter=blob:none", "origin", "+refs/heads/%s:refs/remotes/origin/%s" % (base, base))
        mb = git("merge-base", "HEAD", "origin/" + base).strip()
        return [l for l in git("diff", "--name-only", "--no-renames", mb, "HEAD").splitlines() if l]
    except (subprocess.CalledProcessError, OSError):
        return None


def auto_base():
    """The integrate-* branch whose merge-base with HEAD is the newest commit (committer time), or None."""
    try:
        heads = [l.split("refs/heads/", 1)[1] for l in git("ls-remote", "--heads", "origin", "integrate-*").splitlines()
                 if "refs/heads/" in l]
    except (subprocess.CalledProcessError, OSError):
        return None
    best = None
    for b in heads:
        if not BASE_NAME.match(b):
            continue
        try:
            git("fetch", "--no-tags", "--filter=blob:none", "origin", "+refs/heads/%s:refs/remotes/origin/%s" % (b, b))
            mb = git("merge-base", "HEAD", "origin/" + b).strip()
            t = int(git("show", "-s", "--format=%ct", mb).strip())
        except (subprocess.CalledProcessError, OSError, ValueError):
            continue
        if best is None or t > best[0] or (t == best[0] and b > best[1]):
            best = (t, b)
    return best[1] if best else None


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--event", default=os.environ.get("GITHUB_EVENT_NAME", ""))
    p.add_argument("--ref", default=os.environ.get("GITHUB_REF", ""))
    p.add_argument("--base", default="auto", help="integrate-* branch to diff against; auto = newest merge-base")
    a = p.parse_args(argv)
    branch = a.event == "workflow_dispatch" and not ALWAYS_FULL_REF.match(a.ref)
    base = a.base
    if branch and base in ("", "auto"):
        base = auto_base() or "?"
    files = changed_files(base) if branch else []
    full, reason = decide(a.event, a.ref, files)
    longshot = (not branch) or longshot_needed(files)
    line = "scope: %s (base %s%s); linux-longshot %s" % (reason, base, " auto" if a.base in ("", "auto") and branch else "",
                                                       "runs" if longshot else "skipped (every file is one it never reads)")
    print("full=%s" % ("true" if full else "false"))
    print("longshot=%s" % ("true" if longshot else "false"))
    print(line, file=sys.stderr)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(line + ("\n\nC++ jobs (windows, linux, linux-longshot) SKIPPED: python-only diff.\n" if not full else "\n"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
