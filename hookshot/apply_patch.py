"""Applies our patches (git format-patch files) to a Hookshot source tree without git: the kit's build on a player's PC,
where the source comes from Hookshot's builder parts (hookshot/TOOLS) and git may not be installed.

    python -I apply_patch.py <source tree> <patch> [<patch> ...]

New files are written; changed files must have every hunk's old lines exactly where the hunk says (or anywhere, once),
else nothing is written and it exits 1. Running it again on a tree it already patched changes nothing (exit 0)."""
import os
import sys


def parse(text):
    """[(path, new_file, [(old_lines, new_lines), ...])] from a unified diff"""
    files, cur = [], None
    lines = text.replace("\r\n", "\n").split("\n")
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("diff --git "):
            cur = {"path": None, "new": False, "hunks": []}
            files.append(cur)
        elif cur is not None and ln.startswith("new file mode"):
            cur["new"] = True
        elif cur is not None and ln.startswith("+++ "):
            cur["path"] = ln[4:].split("\t")[0][2:] if ln[4:].startswith("b/") else ln[4:]
        elif cur is not None and ln.startswith("@@"):
            old, new = [], []
            i += 1
            while i < len(lines) and lines[i][:1] in (" ", "+", "-", "\\") and lines[i] != "-- ":
                t = lines[i]
                if t.startswith("\\"):
                    pass
                elif t[0] == " ":
                    old.append(t[1:]), new.append(t[1:])
                elif t[0] == "-":
                    old.append(t[1:])
                else:
                    new.append(t[1:])
                i += 1
            cur["hunks"].append((old, new))
            continue
        i += 1
    return [(f["path"], f["new"], f["hunks"]) for f in files if f["path"]]


def find(haystack, needle):
    for k in range(len(haystack) - len(needle) + 1):
        if haystack[k:k + len(needle)] == needle:
            return k
    return -1


def apply(root, patches):
    writes = {}
    for patch in patches:
        with open(patch, encoding="utf-8") as f:
            for path, new, hunks in parse(f.read()):
                full = os.path.join(root, *path.split("/"))
                if new:
                    body = "\n".join(hunks[0][1]) + "\n" if hunks else ""
                    if os.path.isfile(full):
                        with open(full, encoding="utf-8", newline="") as g:
                            if g.read().replace("\r\n", "\n") == body:
                                continue
                    writes[full] = body   # (a file only our patch has: an older one of ours is replaced)
                    continue
                if full in writes:
                    text = writes[full]
                else:
                    with open(full, encoding="utf-8", newline="") as g:
                        text = g.read()
                crlf = "\r\n" in text
                cur = text.replace("\r\n", "\n").split("\n")
                for old, newl in hunks:
                    if find(cur, newl) >= 0 and find(cur, old) < 0:
                        continue   # (patched before)
                    at = find(cur, old)
                    if at < 0:
                        raise SystemExit("apply_patch: %s: a change does not fit this source" % path)
                    cur = cur[:at] + newl + cur[at + len(old):]
                out = "\n".join(cur)
                writes[full] = out.replace("\n", "\r\n") if crlf else out
    for full, body in writes.items():
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8", newline="") as g:
            g.write(body)
        print("apply_patch: wrote %s" % full)
    return len(writes)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    apply(sys.argv[1], sys.argv[2:])
