"""Fail if the repository contains game content or large binaries, or names a release.

Checks every file tracked by git (or every file under the repository when git
is not available). Exit code 0 means clean, 1 means something must be removed.

Release names (Alex, 2026-11-11: "nothing should reference old releases in the
repo"): the newest build is on the releases page, and history notes give the
date or the issue instead of the release. Text files are searched for "test"
followed by a number as a whole word, and for the tag form (v, 0.1.0, dash, test).
Example tags in tests use another word ("beta").

Test names: a doctest name with a ';' (TEST_CASE, SUBCASE, SCENARIO, TEST_CASE_TEMPLATE, or the
second argument of TEST_CASE_FIXTURE) splits the name for ctest, which then runs nothing for it;
tests/CMakeLists.txt refuses it at configure time. The guard refuses it at commit time, before any
configure (three branches in one day only found out from a failed build).

Private names: the owner's real name must never appear in the repository (in
any case, also inside a path or a longer word); the owner is "Alex". The guard
keeps only a hash of the name, so the name itself is not in this file either.
"""

import hashlib
import os
import posixpath
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
# sha256 of private names (lowercase) that must never appear anywhere.
PRIVATE_NAME_HASHES = {
    "813a0c3c8e05421428b2e4cc7513a7d29e3e02a2567375c178f77780cf3a56ed",
}
PRIVATE_NAME_LENGTHS = {5}
LETTERS = re.compile(r"[a-z]+")

# A doctest name as written in a test source: the macro, then the name's string literal.
TEST_SOURCE_EXTENSIONS = (".cpp", ".hpp", ".h")
_STRING = r'"((?:[^"' + "\\\\" + r']|' + "\\\\" + r'.)*)"'  # a C++ string literal ("\\" escapes)
TEST_NAME = re.compile(
    r'\b(?:TEST_CASE|SUBCASE|SCENARIO|TEST_CASE_TEMPLATE)\s*\(\s*' + _STRING
    + r'|\bTEST_CASE_FIXTURE\s*\(\s*[\w:<>]+\s*,\s*' + _STRING)

# The shared play-test library (tools/playtest): numbers and names we wrote down, in text only.
PLAYTEST_DIR = "tools/playtest/"
PLAYTEST_EXTENSIONS = (".md", ".json", ".jsonl")
PLAYTEST_MAX_BYTES = 256 * 1024
PLAYTEST_LOCAL = re.compile(
    r"\b[A-Za-z]:[\\/]"                                        # a drive path
    r"|(?:^|[\\/\s\"'])(?:Users|home)[\\/][A-Za-z0-9._-]+"     # a user folder
    r"|(?<![\d.])(?!127\.0\.0\.1(?![\d.]))\d{1,3}(?:\.\d{1,3}){3}(?![\d.])"   # an IPv4 address
)

# What a play-test run writes (shots, call logs, session files, the .mcp.json with local paths): never committed,
# wherever it sits. Shots are pictures of the game.
PLAYTEST_OUTPUT_FILES = ("calls.jsonl", "state-history.json", "session.json", ".mcp.json")
PLAYTEST_OUTPUT_DIRS = ("playtest-logs", "sessions")

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


def text_lines(root, path):
    full = os.path.join(root, path)
    if not os.path.isfile(full) or os.path.getsize(full) > MAX_BYTES:
        return []
    with open(full, "rb") as f:
        data = f.read()
    if b"\0" in data:
        return []
    return data.decode("utf-8", "replace").splitlines()


def has_private_name(text):
    """True if any run of letters in text contains a private name."""
    for run in LETTERS.findall(text.lower()):
        for size in PRIVATE_NAME_LENGTHS:
            for start in range(len(run) - size + 1):
                part = run[start:start + size].encode("ascii")
                if hashlib.sha256(part).hexdigest() in PRIVATE_NAME_HASHES:
                    return True
    return False


def private_names(root, path):
    """The lines (and the path itself) that contain a private name."""
    found = []
    if has_private_name(path):
        found.append("the file path")
    for number, line in enumerate(text_lines(root, path), 1):
        if has_private_name(line):
            found.append(f"line {number}")
    return found


def bad_test_names(text):
    """The doctest names in a source's text that have a ';' (ctest would run nothing for them)."""
    return [a or b for a, b in TEST_NAME.findall(text) if ";" in (a or b)]


def test_name_problems(root, path):
    """'line N: "name"' for each test name with a ';' in a C++ source under tests/."""
    if not path.replace("\\", "/").startswith("tests/") or not path.endswith(TEST_SOURCE_EXTENSIONS):
        return []
    found = []
    for number, line in enumerate(text_lines(root, path), 1):
        for name in bad_test_names(line):
            found.append(f'line {number}: "{name}"')
    return found


INGAME_DIR, INGAME_SELFTEST = "tools/autotest/ingame/", "tools/autotest/tests/test_ingame.py"


def unlisted_python_tests(paths, workflow_text, runner_text=""):
    """Python test files (tools/**/test_*.py and *_test.py) that no workflow names by path and no runner script
    (tools/hookshot/host_compile_check.*) names by file: CI runs them by name, so a new one would never run."""
    out = []
    for p in paths:
        q = p.replace("\\", "/")
        if not re.match(r"^tools/.*(/test_[^/]*|_test)\.py$", q) or "/" not in q:
            continue
        name = q.rsplit("/", 1)[1]
        # the in-game suite (tools/autotest/ingame/test_*.py) is played whole on two fake bridges by the selftest
        # tools/autotest/tests/test_ingame.py (docs/ingame-tests.md): a workflow naming that one runs every row file
        if q.startswith(INGAME_DIR) and INGAME_SELFTEST in workflow_text:
            continue
        # a row port's own selftests (tools/autotest/tests/test_ingame_<row>.py) run inside test_ingame.py (load_tests)
        if re.match(r"^tools/autotest/tests/test_ingame_[^/]+\.py$", q) and INGAME_SELFTEST in workflow_text:
            continue
        if q in workflow_text or re.search(r"(?<![A-Za-z0-9_.-])" + re.escape(name) + r"(?![A-Za-z0-9_-])", runner_text):
            continue
        out.append(p)
    return out


def listed_cpp_tests(root, paths):
    """The repository paths of the *_test.cpp files that a CMakeLists.txt or .cmake file names. Each name is resolved
    against the folder of the file that names it (tests/server/CMakeLists.txt lists bare names), a leading
    ${CMAKE_CURRENT_SOURCE_DIR}/ is dropped, and '#' comments are skipped: a name in a comment lists nothing."""
    listed = set()
    for p in paths:
        q = p.replace("\\", "/")
        if not (q.endswith("CMakeLists.txt") or q.endswith(".cmake")):
            continue
        try:
            with open(os.path.join(root, p), encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
        except OSError:
            continue
        here = q.rsplit("/", 1)[0] if "/" in q else ""
        for line in lines:
            code = line.split("#", 1)[0]
            for tok in re.findall(r"[A-Za-z0-9_./${}-]+_test\.cpp", code):
                tok = tok.replace("${CMAKE_CURRENT_SOURCE_DIR}/", "")
                listed.add(posixpath.normpath(posixpath.join(here, tok)))
    return listed


def unlisted_cpp_tests(paths, listed):
    """C++ test sources (*_test.cpp) that no CMakeLists.txt names by full path (listed_cpp_tests): tests/CMakeLists.txt
    lists them one by one (its GLOB_RECURSE only checks the test names), so a new one would never be built. Full
    paths, not file names: session_test.cpp exists in host/ and server/, and flags_test.cpp ends game_flags_test.cpp."""
    return [p for p in paths if p.replace("\\", "/").endswith("_test.cpp") and p.replace("\\", "/") not in listed]


def runner_text(root, paths):
    text = ""
    for p in paths:
        if re.match(r"^tools/hookshot/host_compile_check\.", p.replace("\\", "/")):
            try:
                with open(os.path.join(root, p), encoding="utf-8", errors="replace") as f:
                    text += f.read() + "\n"
            except OSError:
                pass
    return text


def workflow_text(root):
    text = ""
    wdir = os.path.join(root, ".github", "workflows")
    if os.path.isdir(wdir):
        for name in sorted(os.listdir(wdir)):
            if name.endswith((".yml", ".yaml")):
                with open(os.path.join(wdir, name), encoding="utf-8") as f:
                    text += f.read() + "\n"
    return text


def playtest_output_problems(path):
    """A file a play-test run writes (shot, call log, session file, .mcp.json), wherever it was put in the tree."""
    parts = path.replace("\\", "/").split("/")
    if parts[-1].lower() in PLAYTEST_OUTPUT_FILES:
        return ["is a play-test run's own file (keep it outside git; add only your tools/playtest file)"]
    if any(part in PLAYTEST_OUTPUT_DIRS for part in parts[:-1]):
        return ["is in a play-test run folder (keep it outside git; add only your tools/playtest file)"]
    if "shots" in parts[:-1] and parts[-1].lower().endswith((".png", ".jpg", ".jpeg", ".bmp")):
        return ["is a shot of the game (never commit it)"]
    return []


def playtest_problems(root, path):
    """What a file of the play-test library may not be or hold: not text we wrote, too big, or naming this PC."""
    norm = path.replace("\\", "/")
    found = playtest_output_problems(norm)
    if not norm.startswith(PLAYTEST_DIR):
        return found
    if not norm.lower().endswith(PLAYTEST_EXTENSIONS):
        found.append("only .md, .json and .jsonl files go in tools/playtest (no pictures, logs or game files)")
    full = os.path.join(root, path)
    if os.path.isfile(full) and os.path.getsize(full) > PLAYTEST_MAX_BYTES:
        found.append("is larger than 256 KB")
    for number, line in enumerate(text_lines(root, path), 1):
        if PLAYTEST_LOCAL.search(line):
            found.append("line %d holds a local path, a user folder or an IP address" % number)
    return found


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    bad = []
    named = []
    private = []
    semicolons = []
    playtest = []
    files = tracked_files(root)
    for path in files:
        reason = problem(root, path)
        if reason:
            bad.append(f"{path}: {reason}")
        for line in release_names(root, path):
            named.append(f"{path}: {line}")
        for where in private_names(root, path):
            private.append(f"{path}: {where}")
        for where in test_name_problems(root, path):
            semicolons.append(f"{path}: {where}")
        for where in playtest_problems(root, path):
            playtest.append(f"{path}: {where}")
    if bad:
        print("Content guard failed. Remove these files:")
        for line in bad:
            print("  " + line)
    if named:
        print("Content guard failed. These lines name a release; say it without one")
        print("(the newest build is on the releases page; history gives the date or the issue):")
        for line in named:
            print("  " + line)
    if private:
        print("Content guard failed. These name the owner by a private name; write Alex:")
        for line in private:
            print("  " + line)
    if semicolons:
        print("Content guard failed. These test names have a ';' (ctest would run nothing for them;")
        print("tests/CMakeLists.txt refuses them at configure). Use ',' or ':' instead:")
        for line in semicolons:
            print("  " + line)
    if playtest:
        print("Content guard failed. The play-test library (tools/playtest) holds only text about the game,")
        print("with no local path, user folder or IP address:")
        for line in playtest:
            print("  " + line)
    unlisted = unlisted_python_tests(files, workflow_text(root), runner_text(root, files)) if os.path.isdir(os.path.join(root, ".github", "workflows")) else []
    cpp_unlisted = unlisted_cpp_tests(files, listed_cpp_tests(root, files))
    if unlisted:
        print("Content guard failed. CI runs each Python test file by name (.github/workflows/build.yml, python-tools job);")
        print("these are named by no workflow, so they would never run. Add a step for each:")
        for line in unlisted:
            print("  " + line)
    if cpp_unlisted:
        print("Content guard failed. These C++ test sources are named in no CMakeLists.txt, so they are never built or run.")
        print("Add each to the test target (tests/CMakeLists.txt coop_tests):")
        for line in cpp_unlisted:
            print("  " + line)
    if bad or named or private or semicolons or playtest or unlisted or cpp_unlisted:
        return 1
    print("Content guard passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
