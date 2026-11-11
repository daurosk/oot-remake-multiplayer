"""Fail if the repository contains game content or large binaries, or names a release.

Checks every file tracked by git (or every file under the repository when git
is not available). Exit code 0 means clean, 1 means something must be removed.

Release names (Caleb, 2026-11-11: "nothing should reference old releases in the
repo"): the newest build is on the releases page, and history notes give the
date or the issue instead of the release. Text files are searched for "test"
followed by a number as a whole word, and for the tag form (v, 0.1.0, dash, test).
Example tags in tests use another word ("beta").
"""

import os
import re
import subprocess
import sys

BLOCKED_EXTENSIONS = {
    ".nsp", ".xci", ".nup", ".nca", ".nso", ".nro", ".keys",
    ".sav", ".zs", ".pack", ".sarc", ".bfres", ".bntx", ".bwav",
    ".bfevfl", ".ainb", ".bgyml", ".byml", ".webm",
}
BLOCKED_NAMES = {"prod.keys", "title.keys", "main.npdm"}
BLOCKED_DIRS = {"romfs", "exefs", "nand", "keys"}
MAX_BYTES = 2 * 1024 * 1024

RELEASE_NAME = re.compile(r"\btest[0-9]+(?:\.[0-9]+)?\b|\bv?0\.1\.0-test", re.IGNORECASE)
# Files that may name a release because they cannot change ("path": "why").
RELEASE_NAME_ALLOWED = {}


def tracked_files(root):
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            cwd=root, check=True, capture_output=True,
        ).stdout
        return [p for p in out.decode("utf-8").split("\0") if p]
    except (OSError, subprocess.CalledProcessError):
        files = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d != ".git"]
            for name in filenames:
                files.append(os.path.relpath(os.path.join(dirpath, name), root))
        return files


def problem(root, path):
    parts = path.replace("\\", "/").split("/")
    name = parts[-1].lower()
    if any(part.lower() in BLOCKED_DIRS for part in parts[:-1]):
        return "is inside a game content folder"
    if name in BLOCKED_NAMES:
        return "is a key or game file"
    if os.path.splitext(name)[1] in BLOCKED_EXTENSIONS:
        return "has a game file extension"
    full = os.path.join(root, path)
    if os.path.isfile(full) and os.path.getsize(full) > MAX_BYTES:
        return "is larger than 2 MB"
    return None


def release_names(root, path):
    """The lines of a text file that name a release, as "line N: text"."""
    if path.replace("\\", "/") in RELEASE_NAME_ALLOWED:
        return []
    full = os.path.join(root, path)
    if not os.path.isfile(full) or os.path.getsize(full) > MAX_BYTES:
        return []
    with open(full, "rb") as f:
        data = f.read()
    if b"\0" in data:
        return []  # a binary file
    found = []
    for number, line in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
        if RELEASE_NAME.search(line):
            found.append(f"line {number}: {line.strip()[:120]}")
    return found


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    bad = []
    named = []
    for path in tracked_files(root):
        reason = problem(root, path)
        if reason:
            bad.append(f"{path}: {reason}")
        for line in release_names(root, path):
            named.append(f"{path}: {line}")
    if bad:
        print("Content guard failed. Remove these files:")
        for line in bad:
            print("  " + line)
    if named:
        print("Content guard failed. These lines name a release; say it without one")
        print("(the newest build is on the releases page; history gives the date or the issue):")
        for line in named:
            print("  " + line)
    if bad or named:
        return 1
    print("Content guard passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
