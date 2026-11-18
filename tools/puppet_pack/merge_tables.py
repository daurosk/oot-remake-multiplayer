"""One merged resource size table at each start (#48: a randomizer seed from OOTRR's website).

Longshot serves ONE ResourceSizeTable: the one of the mod indexed last (host/mods.c with our patch 0011: the Mods
tab's list `coop-mods.list`, its first row last; the mods it does not name and the puppet pack under the listed ones, by name without case, the last wins; the module: coop-mods.applied). A seed
folder made on OOTRR's page has its own table, made without the puppet pack's entries, so the puppets cannot load.
This writes ONE table where the game reads it: the winner's own entries, and from every other mod that is on, each
entry it has (the larger size where both have one). The winner's original is kept in
`<winner>/_coop-size-table/` (Longshot skips folders whose name starts with '_'), with `merged.txt`, the files the
merge was made from and the file it wrote (`<path under mods> <length> <FNV-1a 64>`), so the next start merges again
only when one of them changed. Last, `<winner>/coop-merged.txt` (beside its romfs) tells the co-op module the table
it serves holds the puppets' entries (coop_rando MergedByKit): `coop-merged 1` / `table <file name> <FNV-1a 64 of
that table>` / `pack <FNV-1a 64 of the puppet pack's table of that name>` / `rule merged|contains` (which rule let
the table through); it holds the puppets again when either file changed since. `--undo` puts every original back.

A winning table that already holds every puppet pack entry, each at least the pack's size (a mod maker merged the
pack's entries in by hand, e.g. a skins mod), needs no merge: it is left as it is and marked `rule contains`. That is
also the only way a winner under `contents/` other than the pack (a title's own mod folder, never rewritten here)
keeps the puppets on.

Our own seeds (`randomizer-*`) are left alone when they win: the module builds a seed beside every other table
already (coop_rando StageSizeTables). When the puppet pack wins (a mod the Mods list does not name that sorts under
`contents/` by name, e.g. "bestbuddy"), the other mods' entries go into the pack's table, our seeds' not: without
them a file that mod made larger has no size the game accepts and the game aborts at the start (player report 101).

The puppet pack's rows in RSDB tables (player report 144/145, merge_rows): before the size tables, each RSDB table the
pack ships (ActorInfo, GameActorInfo) is merged the same way into the copy of the mod that wins it, our seeds too, and
the size the merged file needs goes into the size table the game reads (fit).

    python -I merge_tables.py --mods <game>/mods [--romfs <game>/romfs] [--undo]

Prints what it did, one line each. Exit 0 also when there is nothing to do; 1 when a table could not be merged
(said why; the game starts anyway and the module says the puppets are off).
"""
import argparse
import os
import shutil
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from puppet_pack import byml, rstb, zs  # noqa: E402
else:
    from . import byml, rstb, zs

LIST_FILE = "coop-mods.list"
KEEP_DIR = "_coop-size-table"
STAMP = "merged.txt"
MARK = "coop-merged.txt"   # (the module's kMergedFile)
STAMP_HEAD = "coop-merged 1"
RESOURCE = ("romfs", "System", "Resource")
TAIL = ".rsizetable.zs"
CONTROL = "oot-remake-randomizer"
PACK_DIR = "contents/0400486024DEC000/coop-puppet"   # (the module's kPackFolder)
PACK = PACK_DIR.lower()   # (folded)
MAX_DEPTH = 4   # mods/contents/<title>/coop-puppet is at depth 3
RSDB = ("romfs", "RSDB")
RSDB_TAIL = ".rstbl.byml.zs"
ROWS_KEEP = "_coop-rows"
ROWS_HEAD = "coop-rows 1"
ROWS_MARK = "coop-rows.txt"   # (the module's kRowsFile)


class MergeError(Exception):
    pass


def fold(name):
    """Names as Hookshot compares them: ASCII letters without case, '\\' as '/'."""
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in name.replace("\\", "/"))


def find_mods(root):
    """Every mod folder (a folder holding romfs or exefs) under root: {name with '/': path}."""
    out = {}

    def walk(path, rel, depth):
        try:
            entries = sorted(os.scandir(path), key=lambda e: e.name)
        except OSError:
            return
        for e in entries:
            if not e.is_dir() or e.name[:1] in "-_":
                continue
            name = f"{rel}/{e.name}" if rel else e.name
            if os.path.isdir(os.path.join(e.path, "romfs")) or os.path.isdir(os.path.join(e.path, "exefs")):
                out[name] = e.path
            elif depth < MAX_DEPTH:
                walk(e.path, name, depth + 1)

    walk(root, "", 1)
    return out


def read_list(root):
    """The Mods tab's rows [(name, off)] from coop-mods.list, or [] (no list, or not version 1)."""
    try:
        with open(os.path.join(root, LIST_FILE), "rb") as f:
            text = f.read().decode("utf-8", "replace")
    except OSError:
        return []
    lines = text.split("\n")
    if lines[0].rstrip("\r") != "coop-mods 1":
        return []
    rows, seen = [], set()
    for line in lines[1:]:
        line = line.rstrip("\r")
        if line.startswith("on ") and len(line) > 3:
            name, off = line[3:], False
        elif line.startswith("off ") and len(line) > 4:
            name, off = line[4:], True
        else:
            continue
        if fold(name) in seen:
            continue
        seen.add(fold(name))
        rows.append((name, off))
    return rows


def load_order(root):
    """The mods that are on, in the order Longshot indexes them (the last one's file wins)."""
    rows = read_list(root)
    rank = {fold(n): (-1 if off else len(rows) - i) for i, (n, off) in enumerate(rows)}
    mods = find_mods(root)
    on = [(rank.get(fold(n), 0), fold(n), n) for n in mods if rank.get(fold(n), 0) >= 0]
    return [(n, mods[n]) for _, _, n in sorted(on)]


def tables_of(path):
    d = os.path.join(path, *RESOURCE)
    try:
        return sorted(f for f in os.listdir(d) if f.endswith(TAIL) and len(f) > len(TAIL)
                      and os.path.isfile(os.path.join(d, f)))
    except OSError:
        return []


def is_ours(name):
    top = fold(name).split("/", 1)[0]
    return top.startswith("randomizer-") or top == CONTROL


def fnv64_bytes(data):
    """FNV-1a 64 of bytes, 16 lower-case hex: coop_rando's Fnv64Hex."""
    h = 0xcbf29ce484222325
    for c in data:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def fnv64(path):
    """(FNV-1a 64 hex of the file's bytes, its length)."""
    with open(path, "rb") as f:
        data = f.read()
    return fnv64_bytes(data), len(data)


def file_line(root, path):
    """`<path under root with '/'> <length> <FNV-1a 64 hex>` (coop_rando TablesStamp's line)."""
    h, n = fnv64(path)
    rel = os.path.relpath(path, root).replace(os.sep, "/")
    return f"{rel} {n} {h}"


def missing_pack_entries(table, pack):
    """How many of the pack's entries `table` lacks or holds smaller than the pack (0: it holds them all).

    A name row of the pack may sit in the table as a crc row (no crc clash there), and the other way round."""
    missing = 0
    for c, size in pack.by_crc.items():
        if table.by_crc.get(c, -1) < size:
            missing += 1
    for n, size in pack.by_name.items():
        if max(table.by_name.get(n, -1), table.by_crc.get(rstb.Rstb.crc(n), -1)) < size:
            missing += 1
    return missing


def write_mark(root, win_path, fname, rule="merged"):
    """The module's mark (coop_rando MergedByKit), written last. None without a puppet pack table of that name."""
    pack = os.path.join(root, *PACK_DIR.split("/"), *RESOURCE, fname)
    mark = os.path.join(win_path, MARK)
    if not os.path.isfile(pack):
        if os.path.isfile(mark):
            os.remove(mark)
        return None
    table = fnv64(os.path.join(win_path, *RESOURCE, fname))[0]
    text = f"coop-merged 1\ntable {fname} {table}\npack {fnv64(pack)[0]}\nrule {rule}\n"
    write_atomic(mark, text.encode("utf-8"))
    return mark


def stamp_text(root, out_path, inputs, out_line=None, fit=None):
    lines = [STAMP_HEAD, "out " + (out_line or file_line(root, out_path))]
    lines += sorted("in " + file_line(root, p) for p in inputs)
    lines += [f"fit {k} {n}" for k, n in sorted((fit or {}).items())]
    return "\n".join(lines) + "\n"


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read()
    except OSError:
        return None


class Codec:
    """Decompresses and compresses tables; the game's dictionaries only when a frame names one."""

    def __init__(self, romfs):
        self.romfs = romfs
        self._dicts = None

    def dicts(self):
        if self._dicts is None:
            if not self.romfs or not os.path.isfile(os.path.join(self.romfs, "Pack", "ZsDic.pack.zs")):
                raise MergeError("a size table needs the game's dictionaries (romfs/Pack/ZsDic.pack.zs), not found")
            self._dicts = zs.Dicts.from_romfs(self.romfs)
        return self._dicts

    def unpack(self, raw):
        """The file's bytes, decompressed when it is a zstd frame."""
        if raw[:4] != zs.ZSTD_MAGIC:
            return raw
        if zs.frame_dict_id(raw) == 0:
            z = zs._zstd()
            return z.ZstdDecompressor().decompressobj().decompress(raw)
        return self.dicts().decompress(raw)

    def load(self, raw):
        return rstb.Rstb.loads(self.unpack(raw))

    def save(self, table, like_raw):
        return self.pack(table.dumps(), like_raw)

    def pack(self, data, like_raw):
        """data compressed as like_raw is: the same dictionary, or none, or not at all."""
        if like_raw[:4] != zs.ZSTD_MAGIC:
            return data
        did = zs.frame_dict_id(like_raw)
        if did == 0:
            z = zs._zstd()
            return z.ZstdCompressor(level=16, write_content_size=True, write_checksum=False).compress(data)
        d = self.dicts()
        name = next((n for n, x in d.by_name.items() if x.dict_id() == did), None)
        if name is None:
            raise MergeError(f"no dictionary with id {did}")
        return d.compress(data, name)


def merge(base, others):
    """base with every entry of others (the larger size where both have one). (added, raised)"""
    added = raised = 0
    for t in others:
        for rows, theirs in ((base.by_crc, t.by_crc), (base.by_name, t.by_name)):
            for k, size in theirs.items():
                have = rows.get(k)
                if have is None:
                    rows[k] = size
                    added += 1
                elif size > have:
                    rows[k] = size
                    raised += 1
    return added, raised


def write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def rsdb_tables_of(path):
    d = os.path.join(path, *RSDB)
    try:
        return sorted(f for f in os.listdir(d) if f.endswith(RSDB_TAIL) and len(f) > len(RSDB_TAIL)
                      and os.path.isfile(os.path.join(d, f)))
    except OSError:
        return []


def size_key(fname):
    """The size table's path of an RSDB table file: RSDB/<name> without .zs."""
    return "RSDB/" + fname[:-3]


def rows_of(data):
    rows = byml.loads(data)
    if not isinstance(rows, list) or not all(isinstance(r, dict) and "__RowId" in r for r in rows):
        raise MergeError("not a table of rows with __RowId")
    return rows


def merge_rows(root, romfs, say=print):
    """Player report 144/145: the puppet pack adds rows to RSDB tables (ActorInfo, GameActorInfo: CoopPuppet_*,
    CoopShot_*, ...). Longshot serves each RSDB file from the mod indexed last, like the size table, so a mod that
    ships its own copy of such a table (an OOTRR seed made with settings that change actors: ActorInfo,
    GameActorInfo, PouchActorInfo, built from the game's table) hides the pack's rows, and the game refuses every
    puppet create. For each RSDB table the pack ships, the copy of the mod that wins (our seeds too: OOTRR builds
    them from the game's tables, not the pack's) gets every row of the pack's copy that it lacks, by __RowId (its
    own rows stay: the seed's changes win). Its own copy is kept in `<mod>/_coop-rows/` with `<table>.txt`
    (`coop-rows 1` / `out <the file written>` / `in <the pack's copy>` / `fit <size path> <size>`), so the next start
    merges again only when one of them changed, from the kept copy. `<mod>/coop-rows.txt` tells the module (coop_rando
    RowsMergedByKit) the table it serves holds the pack's rows: `coop-rows 1`, then per table `table <file name>
    <FNV-1a 64 of that table> <FNV-1a 64 of the pack's copy>`.

    Returns (tables not merged, {size path: the size the merged file needs}): merge_mods raises those entries in the
    size table the game serves, as the pack did for its own copy."""
    order = load_order(root)
    pack_path = next((p for n, p in order if fold(n) == PACK), None)
    if pack_path is None:
        return 0, {}   # (no puppet pack on: nothing of ours to carry)
    codec = Codec(romfs)
    failed, fit, marks = 0, {}, {}
    for fname in rsdb_tables_of(pack_path):
        having = [(n, p) for n, p in order if fname in rsdb_tables_of(p)]
        win_name, win_path = having[-1]
        if fold(win_name) == PACK:
            continue
        if fold(win_name).startswith("contents/"):  # (a title's own mod: never rewritten here)
            say(f"Actor tables: the game reads {win_name}'s RSDB/{fname}, without the puppet pack's rows: the other "
                "players can't be shown with it on. Turn it off.")
            failed += 1
            continue
        try:
            line, need = merge_rows_one(root, codec, fname, win_name, win_path, pack_path, say)
        except (OSError, ValueError, KeyError, IndexError, MergeError) as e:
            say(f"Actor tables: {win_name}'s RSDB/{fname} could not be merged with the puppet pack's rows ({e}): the "
                "other players can't be shown with it on.")
            failed += 1
            marks.setdefault(win_path, [])
            continue
        marks.setdefault(win_path, []).append(line)
        if need:
            fit[size_key(fname)] = max(fit.get(size_key(fname), 0), need)
    for win_path, lines in marks.items():
        mark = os.path.join(win_path, ROWS_MARK)
        if lines:
            write_atomic(mark, ("\n".join([ROWS_HEAD] + lines) + "\n").encode("utf-8"))
        elif os.path.isfile(mark):
            os.remove(mark)  # (an old mark must not keep the puppets on)
    return failed, fit


def rows_stamp(root, table_path, pack_table, out_line=None):
    return f"{ROWS_HEAD}\nout {out_line or file_line(root, table_path)}\nin {file_line(root, pack_table)}\n"


def merge_rows_one(root, codec, fname, win_name, win_path, pack_path, say):
    """Merges one table. Returns (the mark's line, the size the written file needs or 0)."""
    table_path = os.path.join(win_path, *RSDB, fname)
    pack_table = os.path.join(pack_path, *RSDB, fname)
    keep = os.path.join(win_path, ROWS_KEEP)
    orig_path = os.path.join(keep, fname)
    stamp_path = orig_path + ".txt"
    old = read_text(stamp_path)
    head = rows_stamp(root, table_path, pack_table)
    if old is not None and old.startswith(head):
        need = next((int(l.split()[2]) for l in old.split("\n") if l.startswith("fit ") and len(l.split()) == 3), 0)
        say(f"Actor tables: {win_name}'s RSDB/{fname} has the puppet pack's rows already (merged at an earlier start).")
        return f"table {fname} {fnv64(table_path)[0]} {fnv64(pack_table)[0]}", need
    ours = old is not None and old.split("\n")[1:2] == ["out " + file_line(root, table_path)] \
        and os.path.isfile(orig_path)
    if not ours:  # (the mod's own table: a new seed put there, or the first start with it)
        os.makedirs(keep, exist_ok=True)
        shutil.copyfile(table_path, orig_path + ".tmp")
        os.replace(orig_path + ".tmp", orig_path)
    with open(orig_path, "rb") as f:
        orig_raw = f.read()
    with open(pack_table, "rb") as f:
        pack_raw = f.read()
    orig_data, pack_data = codec.unpack(orig_raw), codec.unpack(pack_raw)
    rows = rows_of(orig_data)
    have = {r["__RowId"] for r in rows}
    added = [r for r in rows_of(pack_data) if r["__RowId"] not in have]
    if added:
        by_id = {r["__RowId"]: r for r in rows + added}
        new_data = byml.dumps([by_id[k] for k in sorted(by_id, key=lambda s: s.encode("utf-8"))])
        data = codec.pack(new_data, orig_raw)
    else:
        new_data, data = orig_data, orig_raw
    # The size the game needs for it: as the pack sized its own copy (build.py: the game's entry plus the bytes it
    # grew, plus a margin), plus the bytes this one is longer than the pack's; and as the mod sized its own.
    need = 0
    key = size_key(fname)
    for mod_path, length in ((pack_path, len(pack_data)), (win_path, len(orig_data))):
        for t in tables_of(mod_path):
            try:
                entry = load_table(codec, mod_path, t).get(key)
            except (OSError, ValueError, MergeError):
                entry = None
            if entry is not None:
                need = max(need, entry + max(0, len(new_data) - length))
    rel = os.path.relpath(table_path, root).replace(os.sep, "/")
    stamp = rows_stamp(root, table_path, pack_table, f"{rel} {len(data)} {fnv64_bytes(data)}")
    if need:
        stamp += f"fit {key} {need}\n"
    # The stamp before the table, as merge_one: stopped in between, the original is never a merged table.
    write_atomic(stamp_path, stamp.encode("utf-8"))
    write_atomic(table_path, data)
    say(f"Actor tables: the game reads {win_name}'s RSDB/{fname}; merged in the puppet pack's rows ({len(added)} "
        f"added). Its own copy is kept in {win_name}/{ROWS_KEEP} (merge_tables.py --undo puts it back).")
    return f"table {fname} {fnv64_bytes(data)} {fnv64_bytes(pack_raw)}", need


def fit_ok(table, fit):
    return all((table.get(k) or 0) >= n for k, n in fit.items())


def apply_fit(table, fit):
    """Raises the entries in fit (never lowers one). How many it raised."""
    raised = 0
    for k, n in sorted(fit.items()):
        if (table.get(k) or 0) < n:
            if table.get(k) is None:
                table.add_new(k, n)
            else:
                table.set(k, n)
            raised += 1
    return raised


def fit_in_place(codec, win_name, win_path, fname, fit, say):
    """The size table the game reads, left as it is otherwise (one of our seeds, or the pack's alone), with the merged
    RSDB tables' entries raised. Raises only, so a second start finds nothing to do."""
    path = os.path.join(win_path, *RESOURCE, fname)
    with open(path, "rb") as f:
        raw = f.read()
    table = codec.load(raw)
    if apply_fit(table, fit):
        write_atomic(path, codec.save(table, raw))
        shown = "the puppet pack" if fold(win_name) == PACK else win_name
        say(f"Size tables: the game reads {shown}'s; raised the entries of the merged actor tables "
            f"({', '.join(sorted(fit))}).")


def merge_mods(root, romfs, say=print, fit=None):
    """Merges the winning table of each table name. Returns the number of tables it could not merge.
    `fit` (merge_rows): RSDB entries the table the game reads must hold at least."""
    fit = fit or {}
    order = [(n, p, tables_of(p)) for n, p in load_order(root)]
    codec = Codec(romfs)
    failed = 0
    for fname in sorted({f for _, _, fs_ in order for f in fs_}):
        having = [(n, p) for n, p, fs_ in order if fname in fs_]
        win_name, win_path = having[-1]
        if fold(win_name) == PACK:
            # The puppet pack wins (another mod sorts under it by name, e.g. "bestbuddy" < "contents/", and the Mods
            # list does not name it): that mod's entries go into the pack's table, else a file it made larger has no
            # size the game accepts and the game aborts at the start (player report 101). Our seeds are left out:
            # each is built beside every other table, and the pack's table changing makes them be built again.
            others = [(n, p) for n, p in having[:-1] if not is_ours(n)]
            if not others:  # (the pack's table alone, or with our seeds only)
                if fit:
                    try:
                        fit_in_place(codec, win_name, win_path, fname, fit, say)
                    except (OSError, ValueError, MergeError) as e:
                        say(f"Size tables: the puppet pack's could not take the merged actor tables' sizes ({e}).")
                        failed += 1
                continue
            pack_in, title_mod = [], False
        else:
            title_mod = fold(win_name).startswith("contents/")
            if len(having) < 2 or is_ours(win_name):  # (one table, or one of our seeds: left as it is but the fit)
                if fit and not title_mod:
                    try:
                        fit_in_place(codec, win_name, win_path, fname, fit, say)
                    except (OSError, ValueError, MergeError) as e:
                        say(f"Size tables: {win_name}'s could not take the merged actor tables' sizes ({e}): the "
                            "other players can't be shown with it on.")
                        failed += 1
                continue
            others = having[:-1]
            pack_in = [p for n, p in others if fold(n) == PACK]
            if title_mod and not pack_in:
                continue  # (a title's own mod, nothing of the puppets' under it: left alone as ever)
        try:
            merged_before = os.path.isfile(os.path.join(win_path, KEEP_DIR, STAMP))
            if pack_in and (title_mod or (len(others) == 1 and not merged_before)):
                own = load_table(codec, win_path, fname)
                missing = missing_pack_entries(own, load_table(codec, pack_in[0], fname))
                if not fit_ok(own, fit):
                    missing += 1  # (the merged actor tables need larger entries: merged, never 'contains')
                if missing == 0:
                    write_mark(root, win_path, fname, "contains")
                    say(f"Size tables: the game reads {win_name}'s, which holds every puppet pack entry already "
                        "(left as it is): the other players can be shown.")
                    continue
                if title_mod:  # (never rewritten: a title's own mod)
                    remove_mark(win_path)
                    say(f"Size tables: the game reads {win_name}'s, which lacks {missing} of the puppet pack's "
                        "entries (or holds them smaller): the other players can't be shown with it on. Merge the "
                        "pack's entries into it, or turn it off.")
                    failed += 1
                    continue
            failed += merge_one(root, codec, fname, win_name, win_path, others, say, fit)
        except (OSError, ValueError, MergeError) as e:
            remove_mark(win_path)  # (an old mark must not keep the puppets on)
            say(f"Size tables: {win_name} could not be merged ({e}): the other players can't be shown with it on.")
            failed += 1
    return failed


def load_table(codec, mod_path, fname):
    with open(os.path.join(mod_path, *RESOURCE, fname), "rb") as f:
        return codec.load(f.read())


def remove_mark(win_path):
    try:
        os.remove(os.path.join(win_path, MARK))
    except OSError:
        pass


def merge_one(root, codec, fname, win_name, win_path, others, say, fit=None):
    table_path = os.path.join(win_path, *RESOURCE, fname)
    keep = os.path.join(win_path, KEEP_DIR)
    orig_path = os.path.join(keep, fname)
    stamp_path = os.path.join(keep, STAMP)
    inputs = [os.path.join(p, *RESOURCE, fname) for _, p in others]
    old = read_text(stamp_path)
    if old is not None and old == stamp_text(root, table_path, inputs, fit=fit):
        write_mark(root, win_path, fname)
        say(f"Size tables: {win_name} has the other mods' entries already (merged at an earlier start).")
        return 0
    ours = old is not None and old.split("\n")[1:2] == ["out " + file_line(root, table_path)] \
        and os.path.isfile(orig_path)
    if not ours:  # (the mod's own table: a new seed put there, or the first start with it)
        os.makedirs(keep, exist_ok=True)
        shutil.copyfile(table_path, orig_path + ".tmp")
        os.replace(orig_path + ".tmp", orig_path)
    with open(orig_path, "rb") as f:
        orig_raw = f.read()
    table = codec.load(orig_raw)
    loaded = []
    for (n, _), p in zip(others, inputs):
        with open(p, "rb") as f:
            loaded.append(codec.load(f.read()))
    added, raised = merge(table, loaded)
    raised += apply_fit(table, fit or {})
    data = codec.save(table, orig_raw) if (added or raised) else orig_raw
    # The stamp names the table about to be written BEFORE it is written: stopped in between, the next start sees the
    # table is not that one (still the original) and keeps it as the original, never a merged table as the original.
    rel = os.path.relpath(table_path, root).replace(os.sep, "/")
    write_atomic(stamp_path, stamp_text(root, table_path, inputs,
                                        f"{rel} {len(data)} {fnv64_bytes(data)}", fit).encode("utf-8"))
    write_atomic(table_path, data)
    write_mark(root, win_path, fname)
    names = ", ".join("the puppet pack" if fold(n) == PACK else n for n, _ in others)
    shown = "the puppet pack" if fold(win_name) == PACK else win_name
    say(f"Size tables: the game reads {shown}'s; merged in the entries of {names} "
        f"({added} added, {raised} made larger). Its own table is kept in {win_name}/{KEEP_DIR}"
        " (merge_tables.py --undo puts it back).")
    return 0


def undo(root, say=print):
    """Puts each kept original back, but only over the table the merge wrote: a new seed put into the folder since
    keeps its own table (the kept one is the old seed's). The kept folder and the mark go either way."""
    n = 0
    for name, path in find_mods(root).items():
        rows_keep = os.path.join(path, ROWS_KEEP)
        if os.path.isdir(rows_keep):
            back = []
            for f in sorted(os.listdir(rows_keep)):
                stamp = read_text(os.path.join(rows_keep, f + ".txt")) if f.endswith(RSDB_TAIL) else None
                out = next((l[4:] for l in (stamp or "").split("\n") if l.startswith("out ")), None)
                table = os.path.join(path, *RSDB, f)
                if out and os.path.isfile(table) and out == file_line(root, table):
                    shutil.copyfile(os.path.join(rows_keep, f), table)
                    back.append(f)
            shutil.rmtree(rows_keep)
            if os.path.isfile(os.path.join(path, ROWS_MARK)):
                os.remove(os.path.join(path, ROWS_MARK))
            say(f"Actor tables: {name}'s own {', '.join(back)} back." if back else
                f"Actor tables: {name} has other tables than the merged ones now (a new seed?): left as they are.")
            n += 1
        keep = os.path.join(path, KEEP_DIR)
        stamp = read_text(os.path.join(keep, STAMP))
        if stamp is None:
            continue
        out = next((l[4:] for l in stamp.split("\n") if l.startswith("out ")), None)
        back = False
        for f in os.listdir(keep):
            table = os.path.join(path, *RESOURCE, f)
            if f.endswith(TAIL) and os.path.isfile(table) and out == file_line(root, table):
                shutil.copyfile(os.path.join(keep, f), table)
                back = True
        shutil.rmtree(keep)
        if os.path.isfile(os.path.join(path, MARK)):
            os.remove(os.path.join(path, MARK))
        say(f"Size tables: {name}'s own table is back." if back else
            f"Size tables: {name} has another table than the merged one now (a new seed?): left as it is.")
        n += 1
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--mods", required=True)
    ap.add_argument("--romfs", default="")
    ap.add_argument("--undo", action="store_true")
    a = ap.parse_args(argv)
    if not os.path.isdir(a.mods):
        return 0
    if a.undo:
        undo(a.mods)
        return 0
    try:
        rows_failed, fit = merge_rows(a.mods, a.romfs)
        return 1 if (merge_mods(a.mods, a.romfs, fit=fit) or rows_failed) else 0
    except SystemExit:  # (zs._zstd: no zstandard package)
        print("Size tables: not merged (this Python has no zstandard package): with a mod that has its own size "
              "table, the other players can't be shown.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
