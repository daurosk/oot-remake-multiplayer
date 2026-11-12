"""Applies a git patch (git format-patch or git diff output) to a folder, without git.

    python -I apply_patch.py <source folder> <file.patch> [<file.patch> ...]
    python -I apply_patch.py --refresh <keep folder> <source folder> <file.patch> [...]

Build-Hookshot-Coop.ps1 runs it with Hookshot's own Python on the Hookshot source it
downloaded (a source archive, not a git checkout). Strict: every hunk's context and
removed lines must match the file exactly, at the line the hunk names; a file the patch
adds must not exist yet. Nothing is written unless every patch applies (all files are
patched in memory first). Line ends are kept as the file has them.

With --refresh, the source folder may hold an earlier set of patches: the keep folder has the
original of every file a patch changed (and the list of the files a patch added), so the new set
is applied to the originals; a file the new set no longer changes gets its original back, one it
no longer adds goes. A file whose content comes out the same is not written (its time stays, so
Hookshot's build sees it unchanged). hs-build-speed: a patch change no longer unpacks the whole
source again.

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


def read_disk(root, name):
    """(lines, eol, final newline) of the file, None when there is none."""
    target = os.path.join(root, *name.split("/"))
    if not os.path.isfile(target):
        return None
    with open(target, "rb") as f:
        return split(f.read().decode("utf-8"))


def split(raw):
    eol = "\r\n" if "\r\n" in raw else "\n"
    final_newline = raw.endswith(eol) or raw == ""
    content = raw.split(eol)
    if final_newline:
        content.pop()
    return content, eol, final_newline


def joined(value):
    lines, eol, final_newline = value
    return (eol.join(lines) + (eol if final_newline and lines else "")).encode("utf-8")


def apply_file(root, entry, staged, base=None):
    old, new = entry["old"], entry["new"]
    name = new or old
    target = os.path.join(root, *name.split("/"))
    base = base or (lambda n: read_disk(root, n))
    if old is None:
        if (name not in staged and base(name) is not None) or staged.get(name) is not None:
            raise PatchError("%s: the patch adds it, and it is already there" % name)
        content, eol, final_newline = [], "\n", True
    elif name in staged:
        if staged[name] is None:
            raise PatchError("%s: removed by an earlier patch" % name)
        content, eol, final_newline = staged[name]
    else:
        have = base(name)
        if have is None:
            raise PatchError("%s: not found" % name)
        content, eol, final_newline = have
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


MANIFEST = "manifest.txt"   # in the keep folder: "M <path>" (its original is in orig/<path>) or "A <path>" (a patch added it)


def read_manifest(keep):
    out = {}
    try:
        with open(os.path.join(keep, MANIFEST), encoding="utf-8") as f:
            for line in f:
                kind, _, name = line.rstrip("\n").partition(" ")
                if kind in ("M", "A") and name:
                    path_of(name)   # (the same path check as a patch's)
                    out[name] = kind
    except OSError:
        pass
    return out


def write_if_changed(target, data):
    """True when written: a file whose bytes are already these keeps its time."""
    try:
        with open(target, "rb") as f:
            if f.read() == data:
                return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
    with open(target, "wb") as f:
        f.write(data)
    return True


def refresh(root, keep, patch_texts):
    """Applies the patches to the originals of an earlier set (see the module's doc). Returns files written."""
    old = read_manifest(keep)
    orig = os.path.join(keep, "orig")

    def base(name):
        kind = old.get(name)
        if kind == "A":
            return None
        if kind == "M":
            return read_disk(orig, name)
        return read_disk(root, name)

    staged = {}
    for patch, text in patch_texts:
        entries = parse(text)
        if not entries:
            raise PatchError("%s holds no changes" % patch)
        for entry in entries:
            apply_file(root, entry, staged, base)
    new = {}
    for name in staged:
        if name in old:
            new[name] = old[name]
        else:
            have = read_disk(root, name)
            new[name] = "A" if have is None else "M"
            if have is not None:   # the source's own file: kept before it changes
                write_if_changed(os.path.join(orig, *name.split("/")), joined(have))
    written = 0
    for name, value in staged.items():
        target = os.path.join(root, *name.split("/"))
        if value is None:
            if os.path.exists(target):
                os.remove(target)
                written += 1
        elif write_if_changed(target, joined(value)):
            written += 1
    for name, kind in old.items():   # no longer patched: the original back, or an added file gone
        if name in staged:
            continue
        target = os.path.join(root, *name.split("/"))
        if kind == "A":
            if os.path.exists(target):
                os.remove(target)
                written += 1
        else:
            have = read_disk(orig, name)
            if have is not None and write_if_changed(target, joined(have)):
                written += 1
            os.remove(os.path.join(orig, *name.split("/")))
    os.makedirs(keep, exist_ok=True)
    with open(os.path.join(keep, MANIFEST), "w", encoding="utf-8", newline="\n") as f:
        for name in sorted(new):
            f.write("%s %s\n" % (new[name], name))
    return written


def main(argv):
    if len(argv) >= 2 and argv[1] == "--refresh":
        if len(argv) < 5:
            print(__doc__.strip())
            return 2
        keep, root = argv[2], argv[3]
        if not os.path.isdir(root):
            print("apply_patch: %s is not a folder" % root)
            return 1
        try:
            texts = []
            for patch in argv[4:]:
                with open(patch, "rb") as f:
                    texts.append((patch, f.read().decode("utf-8").replace("\r\n", "\n")))
            n = refresh(root, keep, texts)
        except (PatchError, UnicodeDecodeError, OSError) as e:
            print("apply_patch: %s" % e)
            return 1
        print("apply_patch: %d file(s) written (refresh)" % n)
        return 0
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
            os.remove(target)
            continue
        os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
        with open(target, "wb") as f:
            f.write(joined(value))
    print("apply_patch: %d file(s) changed" % len(staged))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
