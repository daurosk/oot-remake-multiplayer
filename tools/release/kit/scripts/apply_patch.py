"""Applies a git patch (git format-patch or git diff output) to a folder, without git.

    python -I apply_patch.py <source folder> <file.patch> [<file.patch> ...]

Build-Hookshot-Coop.ps1 runs it with Hookshot's own Python on the Hookshot source it
downloaded (a source archive, not a git checkout). Strict: every hunk's context and
removed lines must match the file exactly, at the line the hunk names; a file the patch
adds must not exist yet. Nothing is written unless every patch applies (all files are
patched in memory first). Line ends are kept as the file has them.

Exit code 0: applied. 1: a patch does not apply (the message says which file and hunk).
"""

import os
import re
import sys

HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


class PatchError(Exception):
    pass


def path_of(field):
    field = field.split("\t")[0].strip()
    if field == "/dev/null":
        return None
    if field.startswith(("a/", "b/")):
        field = field[2:]
    parts = field.replace("\\", "/").split("/")
    if field.startswith("/") or ":" in field or ".." in parts:
        raise PatchError("a path outside the source folder: %s" % field)
    return field


def parse(text):
    """[{old, new, hunks}], a hunk = (old start, [(op, line)])"""
    files = []
    lines = text.split("\n")
    i = 0
    current = None
    while i < len(lines):
        line = lines[i]
        if line.startswith("diff --git "):
            current = {"old": None, "new": None, "hunks": []}
            files.append(current)
            i += 1
            continue
        if current is not None and line.startswith("--- ") and i + 1 < len(lines) and lines[i + 1].startswith("+++ "):
            current["old"] = path_of(line[4:])
            current["new"] = path_of(lines[i + 1][4:])
            i += 2
            continue
        m = HUNK.match(line)
        if current is not None and m:
            old_count = int(m.group(2)) if m.group(2) is not None else 1
            new_count = int(m.group(4)) if m.group(4) is not None else 1
            body = []
            i += 1
            seen_old = seen_new = 0
            while i < len(lines) and (seen_old < old_count or seen_new < new_count):
                l = lines[i]
                if l.startswith("\\"):   # "\ No newline at end of file"
                    i += 1
                    continue
                op, rest = (l[0], l[1:]) if l else (" ", "")
                if op not in " +-":
                    raise PatchError("the hunk at line %s of %s ends early" % (m.group(1), current["new"] or current["old"]))
                body.append((op, rest))
                if op != "+":
                    seen_old += 1
                if op != "-":
                    seen_new += 1
                i += 1
            if seen_old != old_count or seen_new != new_count:
                raise PatchError("the hunk at line %s of %s is cut short" % (m.group(1), current["new"] or current["old"]))
            while i < len(lines) and lines[i].startswith("\\"):
                i += 1
            current["hunks"].append((int(m.group(1)), body))
            continue
        i += 1
    return [f for f in files if f["old"] or f["new"]]


def apply_file(root, entry, staged):
    old, new = entry["old"], entry["new"]
    name = new or old
    target = os.path.join(root, *name.split("/"))
    if old is None:
        if os.path.exists(target) or staged.get(name) is not None:
            raise PatchError("%s: the patch adds it, and it is already there" % name)
        content, eol, final_newline = [], "\n", True
    elif name in staged:
        if staged[name] is None:
            raise PatchError("%s: removed by an earlier patch" % name)
        content, eol, final_newline = staged[name]
    else:
        if not os.path.isfile(target):
            raise PatchError("%s: not found" % name)
        with open(target, "rb") as f:
            raw = f.read().decode("utf-8")
        eol = "\r\n" if "\r\n" in raw else "\n"
        final_newline = raw.endswith(eol) or raw == ""
        content = raw.split(eol)
        if final_newline:
            content.pop()
    out = list(content)
    shift = 0
    for start, body in entry["hunks"]:
        want = [rest for op, rest in body if op != "+"]
        put = [rest for op, rest in body if op != "-"]
        at = 0 if old is None else max(start - 1, 0) + shift
        if not want and old is not None:
            at = start + shift   # (a hunk that only adds: "-N,0" means after line N)
        if out[at:at + len(want)] != want:
            raise PatchError("%s: the hunk at line %d does not match the file" % (name, start))
        out[at:at + len(want)] = put
        shift += len(put) - len(want)
    staged[name] = (out, eol, final_newline) if new is not None else None


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip())
        return 2
    root = argv[1]
    if not os.path.isdir(root):
        print("apply_patch: %s is not a folder" % root)
        return 1
    staged = {}
    try:
        for patch in argv[2:]:
            with open(patch, "rb") as f:
                text = f.read().decode("utf-8").replace("\r\n", "\n")
            entries = parse(text)
            if not entries:
                raise PatchError("%s holds no changes" % patch)
            for entry in entries:
                apply_file(root, entry, staged)
    except (PatchError, UnicodeDecodeError, OSError) as e:
        print("apply_patch: %s" % e)
        return 1
    for name, value in staged.items():
        target = os.path.join(root, *name.split("/"))
        if value is None:
            if os.path.exists(target):   # (a file an earlier patch of this run added was never written)
                os.remove(target)
            continue
        lines, eol, final_newline = value
        os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
        text = eol.join(lines) + (eol if final_newline and lines else "")
        with open(target, "wb") as f:
            f.write(text.encode("utf-8"))
    print("apply_patch: %d file(s) changed" % len(staged))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
