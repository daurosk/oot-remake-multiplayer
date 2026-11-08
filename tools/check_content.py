"""Fail if the repository contains game content or large binaries.

Checks every file tracked by git (or every file under the repository when git
is not available). Exit code 0 means clean, 1 means something must be removed.
"""

import os
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


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    bad = []
    for path in tracked_files(root):
        reason = problem(root, path)
        if reason:
            bad.append(f"{path}: {reason}")
    if bad:
        print("Content guard failed. Remove these files:")
        for line in bad:
            print("  " + line)
        return 1
    print("Content guard passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
