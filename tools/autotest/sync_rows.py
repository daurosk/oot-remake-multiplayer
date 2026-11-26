"""Reads docs/sync-coverage.md and says what the page claims for an actor name (used by desync_check.py).

The page is the one table of what is shared between the games, so the comparison reads it instead of keeping a second
list that could drift. Rules (plan v4 K to P, v5 R to V, v6 W, X):

- A table is read when its header has a `Path` column, a `Live` column and a column whose header starts with `Actors`
  (`Actors (romfs)` in the Objects table). The first column is the row's name and is never read for globs.
- In the Actors cell only the backticked tokens count, and a token only when it is made of letters, digits, `_`, `*`,
  `?` and `[ ]`: `...TBoxAppear` (an ellipsis) is not a name, and `Obj_Pot_*` written with a count after it gives the glob.
  A row with no accepted token has no globs and is never matched.
- Label of a row (the weakest wins): Path starting with `none`, or Live `-`: "not shared"; Live `?`: "not proven"; Live
  starting with `reload`: "reload"; Live starting with `live`: "live". Any other Live value is "not proven" and is listed
  by `unknown_live` so the doc test fails and the page and this parser stay in step.
- A name that several rows' globs match goes to the best glob: a glob with no wildcard beats one with a wildcard, then
  the one with the most literal characters. Rows tied on the best glob that give different labels are all named and the
  weaker label is used (order: not shared, not proven, reload, live); tied rows that agree: the first row.
- A name that nothing matches has no row: the caller prints "no row in sync-coverage: not compared".

Standard library only.
"""
import fnmatch
import os
import re

DOC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "sync-coverage.md")
ORDER = ("not shared", "not proven", "reload", "live")        # weakest first
TOKEN = re.compile(r"^[A-Za-z0-9_*?\[\]]+$")
PREFIXES = ("Obj_", "Enemy_", "Npc_", "Item_", "Player_", "Dm_")
NO_ROW = "no row in sync-coverage: not compared"
SAYS = {
    "live": "live: a difference is a desync",
    "reload": "shared, shown on reload: not a desync",
    "not proven": "live not proven: check it twice, then report what you saw",
    "not shared": "not shared: not compared",
}


def weakest(labels):
    return min(labels, key=ORDER.index)


def live_label(cell):
    """(label, known) for a Live cell."""
    c = cell.strip().lower()
    if c.startswith("live"):
        return "live", True
    if c.startswith("reload"):
        return "reload", True
    if c == "?":
        return "not proven", True
    if c == "-":
        return "not shared", True
    return "not proven", False


def _cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def _globs(cell):
    return [t for t in re.findall(r"`([^`\n]*)`", cell) if TOKEN.match(t)]


def load(path=None):
    """(rows, tables): each row is {name, line, globs, path, live, label, known_live}; tables is how many tables were
    read. Rows come in file order. A table row with another number of cells than its header is listed in
    `bad_rows` of the first row's table: see `problems`."""
    rows, tables, bad = [], 0, []
    with open(path or DOC, encoding="utf-8") as f:
        lines = f.read().split("\n")
    i = 0
    while i < len(lines):
        if not (lines[i].startswith("|") and i + 1 < len(lines) and re.match(r"^\|[\s:|-]+\|?\s*$", lines[i + 1])):
            i += 1
            continue
        head = _cells(lines[i])
        p = head.index("Path") if "Path" in head else -1
        lv = head.index("Live") if "Live" in head else -1
        ac = next((n for n, h in enumerate(head) if h.startswith("Actors")), -1)
        i += 2
        ok = p >= 0 and lv >= 0 and ac >= 0
        tables += ok
        while i < len(lines) and lines[i].startswith("|"):
            if ok:
                cells = _cells(lines[i])
                if len(cells) != len(head):
                    bad.append(i + 1)
                else:
                    label, known = live_label(cells[lv])
                    if cells[p].lower().startswith("none"):
                        label = weakest([label, "not shared"])
                    rows.append({"name": cells[0], "line": i + 1, "globs": _globs(cells[ac]), "path": cells[p],
                                 "live": cells[lv], "label": label, "known_live": known})
            i += 1
    load.bad_rows = bad
    return rows, tables


def live_short(row):
    return re.sub(r"[`*]", "", row["live"]).strip()[:30]


def glob_key(g):
    """Higher is better: no wildcard first, then the most literal characters."""
    return (0 if re.search(r"[*?\[]", g) else 1, len(re.sub(r"[*?\[\]]", "", g)))


def match(rows, name):
    """None (no row), or {label, rows, glob, tied, text}: `rows` are the row(s) that speak for the name, `tied` is true
    when several rows with different labels share the best glob (then `rows` has all of them, label the weakest)."""
    hits = [(glob_key(g), g, r) for r in rows for g in r["globs"] if fnmatch.fnmatchcase(name, g)]
    if not hits:
        return None
    top = max(h[0] for h in hits)
    best = [(g, r) for k, g, r in hits if k == top]
    seen = []
    for g, r in best:
        if r not in seen:
            seen.append(r)
    tied = len({r["label"] for r in seen}) > 1
    label = weakest([r["label"] for r in seen]) if tied else seen[0]["label"]
    used = seen if tied else seen[:1]
    desc = " / ".join("%s (%s)" % (r["name"].split(" (")[0], live_short(r)) for r in used)   # (a row name without its note)
    return {"label": label, "rows": used, "glob": best[0][0], "tied": tied, "text": "%s: %s" % (desc, SAYS[label])}


def ties(rows):
    """{glob: [row names]} for every glob that stands on two or more rows whose labels differ: what `match` resolves by
    the weaker label. The doc test lists the known ones and fails on a new one."""
    by = {}
    for r in rows:
        for g in r["globs"]:
            by.setdefault(g, []).append(r)
    return {g: [r["name"].split(" (")[0] for r in rs] for g, rs in by.items()
            if len(rs) > 1 and len({r["label"] for r in rs}) > 1}


def problems(rows):
    """Things in the page this parser cannot read: unknown Live values, rows with another number of cells."""
    out = ["line %d: unknown Live value %r" % (r["line"], r["live"]) for r in rows if not r["known_live"]]
    out += ["line %d: a table row with another number of cells than its header" % n
            for n in getattr(load, "bad_rows", [])]
    return out


def unprefixed(rows):
    """Accepted tokens with none of the usual actor prefixes (a fragment of a note, not a name): the doc test prints
    them as a warning."""
    return sorted({(r["line"], g) for r in rows for g in r["globs"] if not g.startswith(PREFIXES) and g != "*"})
