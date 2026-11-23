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
pack's entries in by hand, e.g. a skins mod), needs no merge: it is left as it is and marked `rule contains`.

A mod under `contents/<title id>/` (where emulators keep a title's mods, e.g. Classic Link Voices) is a mod like any
other here: merged when it wins, its original kept the same way. Until 0.11.12 such a winner was never rewritten, so
the game read its table without the sizes of the other mods' files (the merged actor tables among them) and aborted at
every boot in its RSDB loader (player report 259). The same holds for every file merged below: actor tables, menu
archives, the saved values list.

Our own seeds (`randomizer-*` with `coop-rando.txt`, built by the game) are left alone when they win: the module
builds a seed beside every other table already (coop_rando StageSizeTables). When the puppet pack wins (a mod the Mods list does not name that sorts under
`contents/` by name, e.g. "bestbuddy"), the other mods' entries go into the pack's table, our seeds' not: without
them a file that mod made larger has no size the game accepts and the game aborts at the start (player report 101).
A seed folder put in by hand (no `coop-rando.txt`: built with another OOTRR, or made on OOTRR's page) is a mod like
any other here: merged when it wins, its entries taken when the pack wins.

The puppet pack's rows in RSDB tables (player report 144/145, merge_rows): before the size tables, each RSDB table the
pack ships (ActorInfo, GameActorInfo) is merged the same way into the copy of the mod that wins it, our seeds too, and
the size the merged file needs goes into the size table the game reads (fit).

The pack's menu archives where another mod ships the same file and wins it (merge_members: the Character Select mod's
Mals and Common layout archives): the pack's changed members go into that mod's copy, the same way, after the rows.

Two mods that share such a file (player report 206, SHARED_NOTE: a randomizer seed and the Character Select mod):
merge_rows and merge_members take every mod under the winner, not only the pack, and merge_gamedata does the same for
the game's list of saved values (GameData/GameDataList).

The pack's menu files (build.py --title-rows / --pause-tab: larger copies of files the game reads at boot), last
(guard_menus): kept out of the pack's romfs, in `<pack>/_coop-menus`, while the table the game reads lacks their sizes;
else the language archive fails to load and the game crashes at boot.

    python -I merge_tables.py --mods <game>/mods [--romfs <game>/romfs] [--undo]

Prints what it did, one line each. Exit 0 also when there is nothing to do; 1 when a table could not be merged
(said why; the game starts anyway and the module says the puppets are off).
"""
import argparse
import os
import shutil
import struct
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from puppet_pack import byml, msbt, pause_menu, rstb, sarc, title_menu, tunic, tunic_extra, ui_pack, zs  # noqa: E402
else:
    from . import byml, msbt, pause_menu, rstb, sarc, title_menu, tunic, tunic_extra, ui_pack, zs

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
MENU_KEEP = "_coop-menus"     # the pack's menu files while the table the game reads cannot hold them (guard_menus)
# (coop_puppet.json's "menu_keys": the file lists kept aside together, ui_pack.MENU_KEYS when the pack was made; a pack
# without the field has the two old keys)
MEMBERS_KEEP = "_coop-archives"   # a mod's own copies of archives the pack's members were merged into (merge_members)
MEMBERS_HEAD = "coop-members 1"


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


DEFAULT_OFF = "default off"   # hos_coop_mods.h HOS_COOP_MODS_DEFAULT_OFF: a mod the list does not name is off too


def read_list_default(root):
    """The Mods tab's rows [(name, off)] from coop-mods.list and whether it has the "default off" line (Original game:
    a mod the list does not name is off, hos_coop_mods_rank); ([], False) with no list, or not version 1."""
    try:
        with open(os.path.join(root, LIST_FILE), "rb") as f:
            text = f.read().decode("utf-8", "replace")
    except OSError:
        return [], False
    lines = text.split("\n")
    if lines[0].rstrip("\r") != "coop-mods 1":
        return [], False
    rows, seen, default_off = [], set(), False
    for line in lines[1:]:
        line = line.rstrip("\r")
        if line == DEFAULT_OFF:
            default_off = True
            continue
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
    return rows, default_off


def read_list(root):
    """The Mods tab's rows [(name, off)] from coop-mods.list, or [] (no list, or not version 1)."""
    return read_list_default(root)[0]


def load_order(root):
    """The mods that are on, in the order Longshot indexes them (the last one's file wins). A mod the list does not
    name keeps Longshot's own place, or is off with the list's "default off" line."""
    rows, default_off = read_list_default(root)
    rank = {fold(n): (-1 if off else len(rows) - i) for i, (n, off) in enumerate(rows)}
    unnamed = -1 if default_off else 0
    mods = find_mods(root)
    on = [(rank.get(fold(n), unnamed), fold(n), n) for n in mods if rank.get(fold(n), unnamed) >= 0]
    return [(n, mods[n]) for _, _, n in sorted(on)]


def tables_of(path):
    d = os.path.join(path, *RESOURCE)
    try:
        return sorted(f for f in os.listdir(d) if f.endswith(TAIL) and len(f) > len(TAIL)
                      and os.path.isfile(os.path.join(d, f)))
    except OSError:
        return []


SEED_MARK = "coop-rando.txt"  # in a seed the game built (coop_rando kPresetFile): a hand-put seed has none


def is_ours(name, path):
    """The randomizer's control folder, or a seed the game built (beside every other table already)."""
    top = fold(name).split("/", 1)[0]
    if top == CONTROL:
        return True
    return top.startswith("randomizer-") and "/" not in fold(name) and os.path.isfile(os.path.join(path, SEED_MARK))


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


def row_id(row):
    """A row's key: __RowId, or Name in the tables that have no __RowId (UIScreen: the game's screens, one row each)."""
    return row["__RowId"] if "__RowId" in row else row["Name"]


def rows_of(data):
    rows = byml.loads(data)
    ok = isinstance(rows, list) and all(isinstance(r, dict) for r in rows)
    if not ok or not (all("__RowId" in r for r in rows) or all("__RowId" not in r and "Name" in r for r in rows)):
        raise MergeError("not a table of rows with __RowId (or, with none of them, all with Name)")
    return rows


def put_back_stale(root, keep_name, target_of, touched, say, label, mark=None):
    """Review of player report 206: a merged file stayed merged when the mod it was merged with was switched off,
    taken out or moved over the winner, since only --undo put the winner's own copy back. Before each merge, every
    kept copy (under any mod's `keep_name` folder, mods that are off too) that this start does not write again
    (`touched`: the kept copies of this start's winners) goes back over the file the merge wrote (target_of(mod path,
    path under the keep)), the same check as undo: a file put there since stays. The kept copy and its stamp go."""
    for name, path in sorted(find_mods(root).items()):
        keep = os.path.join(path, keep_name)
        if not os.path.isdir(keep):
            continue
        back, gone = [], 0
        for dirpath, _, fnames in os.walk(keep):
            for f in sorted(fnames):
                kept = os.path.join(dirpath, f)
                if f.endswith((".txt", ".tmp")) or os.path.normcase(os.path.abspath(kept)) in touched:
                    continue
                rel = os.path.relpath(kept, keep)
                stamp = read_text(kept + ".txt")
                out = next((l[4:] for l in (stamp or "").split("\n") if l.startswith("out ")), None)
                target = target_of(path, rel)
                if out and os.path.isfile(target) and out == file_line(root, target):
                    shutil.copyfile(kept, target + ".tmp")
                    os.replace(target + ".tmp", target)
                    back.append(rel.replace(os.sep, "/"))
                os.remove(kept)
                if os.path.isfile(kept + ".txt"):
                    os.remove(kept + ".txt")
                gone += 1
        for dirpath, dirnames, fnames in os.walk(keep, topdown=False):
            if not os.listdir(dirpath):
                os.rmdir(dirpath)
        if gone and mark and not os.path.isdir(keep) and os.path.isfile(os.path.join(path, mark)):
            os.remove(os.path.join(path, mark))  # (no merged table left in this mod: no mark for the module)
        if back:
            say(f"{label}: {name}'s own {', '.join(sorted(back))} back (what it was merged with is off, gone or "
                "above it now).")


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
    codec = Codec(romfs)
    failed, fit, marks = 0, {}, {}
    touched = set()
    for step in ("plan", "merge"):
        failed, fit, marks = 0, {}, {}
        tell = say if step == "merge" else (lambda line: None)
        if step == "merge":
            put_back_stale(root, ROWS_KEEP, lambda path, rel: os.path.join(path, *RSDB, rel), touched, say,
                           "Actor tables", ROWS_MARK)
        for fname in sorted({f for _, p in order for f in rsdb_tables_of(p)}):
            having = [(n, p) for n, p in order if fname in rsdb_tables_of(p)]
            if len(having) < 2:
                continue
            win_name, win_path = having[-1]
            pack = pack_path if any(p == pack_path for _, p in having[:-1]) else None
            # Player report 206: the rows of every other mod under the winner too (SHARED_NOTE).
            game = os.path.join(romfs, "RSDB", fname) if romfs else ""
            others = [(n, p) for n, p in having[:-1] if p != pack_path]
            if others and not os.path.isfile(game):
                tell(f"Actor tables: {win_name} and {', '.join(n for n, _ in others)} both ship RSDB/{fname}; the game "
                    f"reads {win_name}'s. Not merged without the game's own copy (--romfs): the others' rows are "
                    "missing.")
                others = []
            if pack is None and not others:
                continue
            kept = os.path.join(win_path, ROWS_KEEP, fname)
            if step == "plan":
                touched.add(os.path.normcase(os.path.abspath(kept)))
                continue
            try:
                line, need = merge_rows_one(root, codec, fname, win_name, win_path, pack, say, others, game)
            except (OSError, ValueError, KeyError, IndexError, MergeError) as e:
                if pack is not None:
                    tell(f"Actor tables: {win_name}'s RSDB/{fname} could not be merged with the puppet pack's rows ({e}): "
                        "the other players can't be shown with it on.")
                    failed += 1
                    marks.setdefault(win_path, [])
                else:
                    tell(f"Actor tables: {win_name}'s RSDB/{fname} could not be merged with the rows of "
                        f"{', '.join(n for n, _ in others)} ({e}): their rows are missing.")
                continue
            if line is not None:
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


def rows_stamp(root, table_path, pack_table, out_line=None, more=()):
    ins = ([pack_table] if pack_table else []) + list(more)
    return f"{ROWS_HEAD}\nout {out_line or file_line(root, table_path)}\n" + "".join(
        f"in {file_line(root, t)}\n" for t in ins)


def stamp_holds(old, head):
    """A stamp written for exactly these files: the head, then nothing but its fit line."""
    return old is not None and old.startswith(head) and (old == head or old[len(head):].startswith("fit "))


def same(a, b):
    """Two table entries hold the same (as the bytes they are written as)."""
    return byml.dumps([a]) == byml.dumps([b])


# Player report 206 (0.11.8: a randomizer seed and the Character Select mod, both on). Longshot serves each file from
# ONE mod, the one indexed last. Two mods that each ship their own copy of a table of entries (an RSDB table's rows,
# GameDataList's saved values, the members of a message or layout archive) therefore lose the lower mod's entries
# while its other files and its code patch still load: the seed's item actors and its changed defaults, or the
# characters' saved picks and the players' own rows. The kit merged such files only for the puppet pack. Now every
# mod under the winner counts: an entry it changed or added, told from the game's own copy, goes into the winner's
# copy where the winner left that entry as the game has it. An entry both changed stays the winner's and is named.
SHARED_NOTE = "player report 206"


def changed_entries(theirs, games, mine, skip=()):
    """theirs / games / mine: {key: entry} of a lower mod's file, the game's own and the winner's own (SHARED_NOTE).
    Returns ({key: entry} to take, [keys both changed differently: the winner's stay])."""
    take, both = {}, []
    for k, e in theirs.items():
        if k in skip or (k in games and same(e, games[k])):
            continue
        if k not in mine or (k in games and same(mine[k], games[k])):
            take[k] = e
        elif not same(mine[k], e):
            both.append(k)
    return take, both


def size_needed(codec, key, new_length, copies):
    """The size the game needs for a merged file: each mod's size table entry for the copy it sized (copies: [(mod
    path, that copy's length)]), plus what the merged one is longer than it. 0 when no table names the file."""
    need = 0
    for mod_path, length in copies:
        for t in tables_of(mod_path):
            try:
                entry = load_table(codec, mod_path, t).get(key)
            except (OSError, ValueError, MergeError):
                entry = None
            if entry is not None:
                need = max(need, entry + max(0, new_length - length))
    return need


def merge_rows_one(root, codec, fname, win_name, win_path, pack_path, say, others=(), game=""):
    """Merges one table: the pack's rows (pack_path, or None) and the changed rows of the other mods under the winner
    (others [(name, path)] in load order, told from the game's own copy `game`). Returns (the mark's line, or None
    without the pack; the size the written file needs or 0)."""
    table_path = os.path.join(win_path, *RSDB, fname)
    pack_table = os.path.join(pack_path, *RSDB, fname) if pack_path else None
    more = [os.path.join(p, *RSDB, fname) for _, p in others]
    whose = " and ".join((["the puppet pack's"] if pack_path else []) + [f"{n}'s" for n, _ in others])
    keep = os.path.join(win_path, ROWS_KEEP)
    orig_path = os.path.join(keep, fname)
    stamp_path = orig_path + ".txt"
    old = read_text(stamp_path)
    head = rows_stamp(root, table_path, pack_table, more=more)
    if stamp_holds(old, head):
        need = next((int(l.split()[2]) for l in old.split("\n") if l.startswith("fit ") and len(l.split()) == 3), 0)
        say(f"Actor tables: {win_name}'s RSDB/{fname} has {whose} rows already (merged at an earlier start).")
        return (f"table {fname} {fnv64(table_path)[0]} {fnv64(pack_table)[0]}" if pack_path else None), need
    ours = old is not None and old.split("\n")[1:2] == ["out " + file_line(root, table_path)] \
        and os.path.isfile(orig_path)
    if not ours:  # (the mod's own table: a new seed put there, or the first start with it)
        os.makedirs(keep, exist_ok=True)
        shutil.copyfile(table_path, orig_path + ".tmp")
        os.replace(orig_path + ".tmp", orig_path)
    with open(orig_path, "rb") as f:
        orig_raw = f.read()
    pack_raw = pack_data = b""
    if pack_table:
        with open(pack_table, "rb") as f:
            pack_raw = f.read()
        pack_data = codec.unpack(pack_raw)
    orig_data = codec.unpack(orig_raw)
    rows = rows_of(orig_data)
    have = {row_id(r) for r in rows}
    pack_rows = rows_of(pack_data) if pack_table else []
    added = [r for r in pack_rows if row_id(r) not in have]
    taken, both, lengths = {}, [], []
    games = {}
    if os.path.isfile(game):
        with open(game, "rb") as f:
            games = {row_id(r): r for r in rows_of(codec.unpack(f.read()))}
    # The pack's changed rows of the game's own (UIScreen's Title_00 with Common added to its archives): taken where
    # the winner's row is still the game's own, as another mod's changed row is.
    mine = {row_id(r): r for r in rows}
    if games:
        take, b = changed_entries({row_id(r): r for r in pack_rows if row_id(r) in games}, games, mine)
        both += b
        taken.update(take)
    # The other mods' rows (SHARED_NOTE): the mod nearer the winner first, so its row is taken where two changed one.
    if others:
        for (_, p), t in reversed(list(zip(others, more))):
            with open(t, "rb") as f:
                their_data = codec.unpack(f.read())
            lengths.append((p, len(their_data)))
            take, b = changed_entries({row_id(r): r for r in rows_of(their_data)}, games, mine,
                                      skip={row_id(r) for r in added})
            both += [k for k in b if k not in both]
            for k, r in take.items():
                taken.setdefault(k, r)
    if added or taken:
        by_id = {row_id(r): r for r in rows + added}
        by_id.update(taken)
        # (sorted by the key's bytes, as the game's own tables are: __RowId, or Name in UIScreen)
        new_data = byml.dumps([by_id[k] for k in sorted(by_id, key=lambda s: s.encode("utf-8"))])
        data = codec.pack(new_data, orig_raw)
    else:
        new_data, data = orig_data, orig_raw
    # The size the game needs for it: as the pack sized its own copy (build.py: the game's entry plus the bytes it
    # grew, plus a margin), plus the bytes this one is longer than the pack's; and as the mod sized its own.
    key = size_key(fname)
    need = size_needed(codec, key, len(new_data),
                       ([(pack_path, len(pack_data))] if pack_path else []) + [(win_path, len(orig_data))] + lengths)
    rel = os.path.relpath(table_path, root).replace(os.sep, "/")
    stamp = rows_stamp(root, table_path, pack_table, f"{rel} {len(data)} {fnv64_bytes(data)}", more)
    if need:
        stamp += f"fit {key} {need}\n"
    # The stamp before the table, as merge_one: stopped in between, the original is never a merged table.
    write_atomic(stamp_path, stamp.encode("utf-8"))
    write_atomic(table_path, data)
    say(f"Actor tables: the game reads {win_name}'s RSDB/{fname}; merged in {whose} rows ({len(added) + len(taken)} "
        f"added). Its own copy is kept in {win_name}/{ROWS_KEEP} (merge_tables.py --undo puts it back)."
        + (f" {win_name} changed {', '.join(sorted(both))} too: its own kept." if both else ""))
    return (f"table {fname} {fnv64_bytes(data)} {fnv64_bytes(pack_raw)}" if pack_path else None), need


def _members(data):
    """{name: [bytes, ...]} of a SARC (a name more than once kept in order), and its entries in order."""
    entries = sarc.read_entries(data)
    out = {}
    for n, d in entries:
        out.setdefault(n, []).append(bytes(d))
    return out, entries


def merge_members(root, romfs, say=print):
    """The pack's menu archives (build.py --title-rows / --pause-tab: each language's Mals archive, the Title and Common
    layout archives) where another mod ships the same file and wins it (the Character Select mod: Mals/USen, Mals/EUen,
    UI/Layout2Archive/Common, for its own page, picker and header). Longshot serves the winner's whole file, so the
    pack's members (its texts, MenuOverlay_00, PlusMenu_00) were lost. The winner's copy gets every member the pack
    changed from the game's own archive (romfs) where the winner left that member as the game has it; a member both
    changed stays the winner's and is named (a decision for the two mods: the pause header). Its own copy is kept in
    `<mod>/_coop-archives/<path>` with `<path>.txt` (`coop-members 1` / `out <the file written>` / `in <the pack's
    copy>` / `fit <size path> <size>`), so the next start merges again only when one of them changed, from the kept
    copy; --undo puts it back. Needs the game's romfs (to tell which member changed); without it nothing is merged.

    Returns (archives not merged, {size path: the size the merged file needs})."""
    order = load_order(root)
    pack_path = next((p for n, p in order if fold(n) == PACK), None)
    files = [rel for rel in menu_files(pack_path)
             if os.path.isfile(os.path.join(pack_path, "romfs", *rel.split("/")))] if pack_path else []
    # The archives two other mods share too (SHARED_NOTE: a seed's and the Character Select mod's messages).
    files += sorted({rel for _, p in order for rel in archives_of(p)} - set(files))
    codec = Codec(romfs)
    failed, fit = 0, {}
    touched = set()
    for step in ("plan", "merge"):
        failed, fit = 0, {}
        tell = say if step == "merge" else (lambda line: None)
        if step == "merge":
            put_back_stale(root, MEMBERS_KEEP, lambda path, rel: os.path.join(path, "romfs", rel), touched,
                           say, "Menus")
        for rel in files:
            real = [(n, p) for n, p in order if os.path.isfile(os.path.join(p, "romfs", *rel.split("/")))]
            if not real:
                continue
            win_name, win_path = real[-1]
            # Row 224: a mod that ships this language's messages only in English (Character Select: USen and EUen)
            # gives its English messages to every other language (stand_in), so its labels are not lost there.
            # (in load order, the winner last: a mod above it without this file is the nearest)
            having = [(n, p) for n, p in order if p != win_path and (
                (n, p) in real or (p != pack_path and stand_in(p, rel, romfs)))] + [(win_name, win_path)]
            if len(having) < 2:
                continue
            pack = pack_path if any(p == pack_path for _, p in having[:-1]) else None
            others = [(n, p) for n, p in having[:-1] if p != pack_path]
            names = ", ".join(n for n, _ in others)
            game = os.path.join(romfs, *rel.split("/")) if romfs else ""
            if not os.path.isfile(game):
                if pack is not None:
                    tell(f"Menus: {win_name} ships {rel} too and wins it; not merged without the game's own copy "
                        "(--romfs): the co-op menus' text and panes are missing with it on.")
                    failed += 1
                else:
                    tell(f"Menus: {win_name} and {names} both ship {rel}; the game reads {win_name}'s. Not merged without "
                        "the game's own copy (--romfs): the others' changes are missing.")
                continue
            kept = os.path.join(win_path, MEMBERS_KEEP, *rel.split("/"))
            if step == "plan":
                touched.add(os.path.normcase(os.path.abspath(kept)))
                continue
            try:
                need = merge_members_one(root, codec, rel, win_name, win_path, pack, game, say, others)
            except (OSError, ValueError, KeyError, IndexError, MergeError, sarc.SarcError) as e:
                if pack is not None:
                    tell(f"Menus: {win_name}'s {rel} could not be merged with the puppet pack's ({e}): the co-op menus' "
                        "text and panes are missing with it on.")
                    failed += 1
                else:
                    tell(f"Menus: {win_name}'s {rel} could not be merged with what {names} changed ({e}): their changes "
                        "are missing.")
                continue
            if need:
                k = rel[:-3] if rel.endswith(".zs") else rel
                fit[k] = max(fit.get(k, 0), need)
    return failed, fit


# 0.11.9 (the owner: co-op's customization in place of Character Select's, his screen kept): the module writes co-op's
# words into his Customize screen's texts (hookshot/src/coop_snail_custom.h), each through a placeholder message of its
# own in his screen's message file, as the pack's own Coop_Live_NN in LayoutMsg/PlusMenu_00 (the text box keeps a
# pointer to its text, so each text needs storage of its own): 13 (the title, six labels, six values), 64 dots each.
SNAIL_MSG = "LayoutMsg/CharacterCustomize_00.msbt"
SNAIL_SLOTS = 13
SNAIL_PLACEHOLDER = "." * 64


def add_snail_placeholders(entries):
    """(entries with the placeholders in his Customize screen's message file, whether one changed)."""
    labels = {"Coop_Custom_%02d" % i: SNAIL_PLACEHOLDER for i in range(SNAIL_SLOTS)}
    out, changed = [], False
    for n, d in entries:
        if n == SNAIL_MSG:
            d2 = ui_pack.add_labels(d, labels, replace=True)
            changed = changed or d2 != d
            d = d2
        out.append((n, d))
    return out, changed


def pack_edit(rel, name, theirs):
    """The pack's own change of member `name` of archive `rel` made again on the winner's copies (theirs: {name:
    [bytes]}), when both changed it: [bytes] or None (no such edit: the winner's is kept). 0.11.9, lane R1: Character
    Select and the pack both change MenuOverlay_00.buil (the - menu's strip); keeping his alone lost co-op's tabs. The
    pause menu's layouts get pause_menu's edits on his copy (his panes kept, ours added); a message file gets co-op's
    labels (strings/<language>) added to his. Raises ui_pack.UiPackError (or the parsers' errors) when it cannot."""
    if rel == pause_menu.COMMON_ARCHIVE:
        if name == pause_menu.OVERLAY_LAYOUT:
            pages = theirs.get(pause_menu.PAGES_LAYOUT) or []
            if not pages:
                raise ui_pack.UiPackError("no %s to place the - menu's page by" % pause_menu.PAGES_LAYOUT)
            place, size = pause_menu.shown_page(pages[0])
            edit = lambda d: pause_menu.add_minus_page(pause_menu.add_tab(d), place, size)
        elif name == pause_menu.PAGES_LAYOUT:
            edit = pause_menu.add_page
        elif name == pause_menu.SYSTEM_LAYOUT:
            edit = pause_menu._system_layout
        else:
            return None
        return [edit(d) for d in theirs[name]]
    # A randomizer seed (OOTRR 0.8: its Randomizer line in the title logo, Logo_USEU_0p, which Title_00.buil points
    # at) and the pack both change the title layout; the seed's alone lost co-op's title rows (compat-0119). The title
    # menu's rows go on the winner's copy, and its rows' texts on a Title_00.msbt the winner changed too.
    if rel == title_menu.TITLE_ARCHIVE:
        if name != title_menu.TITLE_LAYOUT:
            return None
        return [title_menu.add_menu(d) for d in theirs[name]]
    if rel.startswith("Mals/") and name == title_menu.MSG_FILE:
        return [title_menu.add_rows(d, title_menu.rows_for(rel)) for d in theirs[name]]
    if rel.startswith("Mals/") and name.endswith(".msbt"):
        ours = ui_pack.load_strings(ui_pack.language_of(rel)).get(name)
        if not ours:
            return None
        return [ui_pack.add_labels(d, ours, replace=True) for d in theirs[name]]
    return None


ARCHIVE_DIRS = (("Mals",), ("UI", "Layout2Archive"))
ARCHIVE_TAILS = (".sarc.zs", ".blarc.zs")


def archives_of(path):
    """A mod's message and layout archives (romfs paths with '/'): the files merge_members merges between mods."""
    out = []
    for parts in ARCHIVE_DIRS:
        d = os.path.join(path, "romfs", *parts)
        try:
            out += ["/".join(parts + (f,)) for f in sorted(os.listdir(d))
                    if f.endswith(ARCHIVE_TAILS) and os.path.isfile(os.path.join(d, f))]
        except OSError:
            pass
    return out


# Row 224 (Character Select's - menu tab read 'msgnotfound', and his page crashed): two ways a mod's messages were lost.
# (1) He ships Mals only for USen and EUen; in every other language the game reads another mod's archive (the pack's,
# a seed's) or its own, without his labels and message files. stand_in / english_stand_in: such a mod gives its English
# messages for the languages it has none of, as the game itself has a text in each language.
# (2) A message file two mods change was taken whole from the nearer one, the other's labels lost (and a member a mod
# under the winner gave was not looked at again for the pack's: the - menu's co-op tab labels). merge_msbt: per label.
ENGLISH = (("US", ("USen", "EUen")), ("", ("EUen", "USen")))   # (language prefix, English archives in that order)


def stand_in(path, rel, romfs=None):
    """The English message archive ("Mals/USen..." or "Mals/EUen...") a mod gives for `rel`, a Mals archive of a language
    it ships none of; None when it ships `rel`, no English one, or the game has no English copy to tell its changes."""
    if not rel.startswith("Mals/") or not romfs or os.path.isfile(os.path.join(path, "romfs", *rel.split("/"))):
        return None
    lang = ui_pack.language_of(rel)
    tail = rel.split("/", 1)[1][len(lang):]
    for prefix, names in ENGLISH:
        if lang.startswith(prefix):
            for en in names:
                cand = "Mals/" + en + tail
                if (cand != rel and os.path.isfile(os.path.join(path, "romfs", *cand.split("/")))
                        and os.path.isfile(os.path.join(romfs, *cand.split("/")))):
                    return cand
            return None
    return None


def _msbt_attrs(sects):
    """(ATR1 entry size, [entry bytes]) of read_msbt's sections; (0, None) without ATR1 or with empty entries."""
    for magic, data in sects:
        if magic == b"ATR1":
            n, size = struct.unpack_from("<II", data, 0)
            if not size:
                return 0, None
            if len(data) != 8 + n * size:   # (attribute strings after the entries: not written here)
                raise ui_pack.UiPackError("the message file's attributes have strings, which are not merged here")
            return size, [data[8 + i * size:8 + (i + 1) * size] for i in range(n)]
    return 0, None


def merge_msbt(base, game, theirs, only_new=False):
    """A message file (MSBT bytes) `base` with another mod's labels: each label `theirs` adds or changes from the
    game's (`game`, or None when the game has no such file) is put in, unless `base` changed that label as well (then
    base's stays; named). only_new: only the labels the game does not have (english_stand_in). Every other label and
    text of base is kept as it is. Returns (bytes, [labels both changed]). Raises ui_pack.UiPackError when one of them
    is not a message file written here (then the caller keeps base whole)."""
    for d in (base, theirs):
        names = msbt.read(d).section_names()
        if "LBL1" not in names or "TXT2" not in names or any(s not in ui_pack._GROWN for s in names):
            raise ui_pack.UiPackError("a message file with sections not merged here")
    header, sects, have, texts = ui_pack.read_msbt(base)
    t_header, t_sects, t_have, t_texts = ui_pack.read_msbt(theirs)
    if game is not None:
        _, _, g_have, g_texts = ui_pack.read_msbt(game)
    else:
        g_have, g_texts = {}, []
    if header[0x0c] != t_header[0x0c]:
        raise ui_pack.UiPackError("the two message files are not in the same encoding")
    size, attrs = _msbt_attrs(sects)
    t_size, t_attrs = _msbt_attrs(t_sects)
    have, texts = dict(have), list(texts)
    attrs = list(attrs) if attrs is not None else None
    clash, changed = [], False
    for label, i in t_have.items():
        mine = t_texts[i]
        g = g_texts[g_have[label]] if label in g_have else None
        if mine == g or (only_new and g is not None):
            continue
        attr = t_attrs[i] if attrs is not None and t_attrs is not None and t_size == size else b"\0" * size
        if label in have:
            w = texts[have[label]]
            if w == mine:
                continue
            if w != g:
                clash.append(label)   # (both changed it: base's stays)
                continue
            texts[have[label]] = mine
            if attrs is not None:
                attrs[have[label]] = attr
            changed = True
        else:
            have[label] = len(texts)
            texts.append(mine)
            if attrs is not None:
                attrs.append(attr)
            changed = True
    if not changed:
        return base, clash
    if attrs is not None:
        sects = [(m, struct.pack("<II", len(attrs), size) + b"".join(attrs) if m == b"ATR1" else d) for m, d in sects]
    return ui_pack.write_msbt(header, sects, have, texts), clash


def english_stand_in(codec, mod_file, game_en_file, game_file):
    """The archive (bytes) a mod with English messages only (stand_in) gives for another language: the game's own
    archive of that language, with every message file the mod adds and every label it adds to one of the game's, in
    the mod's English words (the game's labels keep their words in that language)."""
    def members(path):
        with open(path, "rb") as f:
            return _members(codec.unpack(f.read()))
    mine, _ = members(mod_file)
    en, _ = members(game_en_file)
    games, entries = members(game_file)
    out, added = [], {}
    for name, datas in mine.items():
        if datas == en.get(name):
            continue
        if name not in en and name not in games:
            added[name] = datas
        elif (name.endswith(".msbt") and name in en and name in games
              and len(datas) == len(en[name]) == len(games[name]) == 1):
            try:
                added[name] = [merge_msbt(games[name][0], en[name][0], datas[0], only_new=True)[0]]
            except (ValueError, KeyError, IndexError, struct.error):
                pass   # (not a message file written here: the game's words in this language stay)
    for n, d in entries:
        out.append((n, added.pop(n)[0] if n in added else d))
    out.extend((n, x) for n in sorted(added) for x in added[n])
    return sarc.write(out, data_align=16)


def merge_members_one(root, codec, rel, win_name, win_path, pack_path, game, say, others=()):
    """Merges one archive: the pack's members (pack_path, or None) and those of the other mods under the winner
    (others [(name, path)] in load order). Returns the size the written file needs, or 0."""
    path = os.path.join(win_path, "romfs", *rel.split("/"))
    pack_file = os.path.join(pack_path, "romfs", *rel.split("/")) if pack_path else None
    game_romfs = game
    for _ in rel.split("/"):
        game_romfs = os.path.dirname(game_romfs)
    more, english = [], {}   # (english: a mod's English archive standing in for this language -> the game's English)
    for _, p in others:
        t = os.path.join(p, "romfs", *rel.split("/"))
        if not os.path.isfile(t):
            en = stand_in(p, rel, game_romfs)   # (row 224; merge_members took the mod for it)
            if en is None:
                raise MergeError(f"{t} is gone")
            t = os.path.join(p, "romfs", *en.split("/"))
            english[t] = os.path.join(game_romfs, *en.split("/"))
        more.append(t)
    ins = "".join(f"in {file_line(root, t)}" + (f" for {ui_pack.language_of(rel)}" if t in english else "") + "\n"
                  for t in ([pack_file] if pack_file else []) + more)
    gave_english = [n for (n, _), t in zip(others, more) if t in english]
    whose = " and ".join((["the puppet pack's"] if pack_path else []) + [f"{n}'s" for n, _ in others])
    keep = os.path.join(win_path, MEMBERS_KEEP)
    orig_path = os.path.join(keep, *rel.split("/"))
    stamp_path = orig_path + ".txt"
    old = read_text(stamp_path)
    head = f"{MEMBERS_HEAD}\nout {file_line(root, path)}\n{ins}"
    if stamp_holds(old, head):
        say(f"Menus: {win_name}'s {rel} has {whose} members already (merged at an earlier start).")
        return next((int(l.split()[2]) for l in old.split("\n") if l.startswith("fit ") and len(l.split()) == 3), 0)
    ours = old is not None and old.split("\n")[1:2] == ["out " + file_line(root, path)] and os.path.isfile(orig_path)
    if not ours:  # (the mod's own archive: the first start with it, or a new copy of the mod)
        os.makedirs(os.path.dirname(orig_path), exist_ok=True)
        shutil.copyfile(path, orig_path + ".tmp")
        os.replace(orig_path + ".tmp", orig_path)
    with open(orig_path, "rb") as f:
        orig_raw = f.read()
    with open(game, "rb") as f:
        game_raw = f.read()
    win_data, game_data = codec.unpack(orig_raw), codec.unpack(game_raw)
    theirs, entries = _members(win_data)
    games, _ = _members(game_data)
    # The other mods, the one nearer the winner first (SHARED_NOTE), then the pack (under the mods the list names):
    # a member two of them changed is the nearer one's, as it was when that mod won the file.
    take, both, lengths, ontop, labels_kept, labelled = {}, [], [], [], [], []
    for mod_path, file in list(reversed([(p, t) for (_, p), t in zip(others, more)])) + (
            [(pack_path, pack_file)] if pack_file else []):
        if file in english:   # (its size table sized its English copy, not this one: no length of it)
            their_data = english_stand_in(codec, file, english[file], game)
        else:
            with open(file, "rb") as f:
                their_data = codec.unpack(f.read())
            lengths.append((mod_path, len(their_data)))
        for name, datas in _members(their_data)[0].items():
            if datas == games.get(name) or datas == theirs.get(name):
                continue   # (left as the game has it, or the winner has the same)
            base = take.get(name, theirs.get(name, games.get(name)))
            if (rel.startswith("Mals/") and name.endswith(".msbt") and base is not None and len(base) == 1
                    and len(datas) == 1 and len(games.get(name) or [b""]) == 1):
                # A message file: label by label (row 224), a label two changed stays the nearer one's (or the
                # winner's). Not a message file written here: the member as below.
                try:
                    made, clash = merge_msbt(base[0], (games.get(name) or [None])[0], datas[0])
                except (ValueError, KeyError, IndexError, struct.error):
                    made = None
                if made is not None:
                    if made != base[0]:
                        if theirs.get(name, games.get(name)) != games.get(name) or name in take:
                            labelled.append(name)   # (both kept: the labels of each)
                        take[name] = [made]
                    labels_kept.extend(f"{name} {label}" for label in clash)
                    continue
            if name in take:
                continue   # (a mod before gave it)
            if theirs.get(name, games.get(name)) == games.get(name):
                take[name] = datas   # (the winner left it as the game has it, or has none)
            elif name not in both:
                made = None
                if mod_path == pack_path:   # (the pack's change made again on the winner's: both kept)
                    try:
                        made = pack_edit(rel, name, {**games, **theirs})
                    except (ui_pack.UiPackError, OSError, ValueError, KeyError, IndexError, struct.error) as e:
                        say(f"Menus: co-op's {name} could not be made on {win_name}'s ({e}).")
                if made is not None:
                    take[name] = made
                    ontop.append(name)
                else:
                    both.append(name)    # (both changed it: the winner's stays)
    out, done = [], set()
    for n, d in entries:
        if n in take:
            if n not in done:
                out.extend((n, x) for x in take[n])
                done.add(n)
        else:
            out.append((n, d))
    out.extend((n, x) for n in sorted(take) if n not in done for x in take[n])
    out, placeholders = add_snail_placeholders(out)
    if take or placeholders:
        new_data = sarc.write(out, data_align=16)
        data = codec.pack(new_data, orig_raw)
    else:
        new_data, data = win_data, orig_raw
    # The size the game needs for it: each table's entry for the copy it sized, plus what this one is longer.
    key = rel[:-3] if rel.endswith(".zs") else rel
    need = size_needed(codec, key, len(new_data), lengths + [(win_path, len(win_data))])
    rel_out = os.path.relpath(path, root).replace(os.sep, "/")
    stamp = f"{MEMBERS_HEAD}\nout {rel_out} {len(data)} {fnv64_bytes(data)}\n{ins}"
    if need:
        stamp += f"fit {key} {need}\n"
    write_atomic(stamp_path, stamp.encode("utf-8"))   # (the stamp first, as merge_rows_one)
    write_atomic(path, data)
    say(f"Menus: the game reads {win_name}'s {rel}; merged in {whose} members ({len(take)}: "
        f"{', '.join(sorted(take)) or 'none'}). Its own copy is kept in {win_name}/{MEMBERS_KEEP}."
        + (f" Both changed {', '.join(sorted(ontop))}: co-op's part made on {win_name}'s (both kept)."
           if ontop else "")
        + (f" Both changed {', '.join(sorted(both))}: {win_name}'s kept (the co-op part of it is missing)."
           if both else "")
        + (f" More than one changed {', '.join(sorted(set(labelled)))}: the labels of each kept." if labelled else "")
        + (f" A label both changed, the nearer mod's kept: {', '.join(labels_kept)}." if labels_kept else "")
        + (f" {', '.join(gave_english)}: no archive in this language, its English messages taken."
           if gave_english else "")
        + (" Its Customize screen's messages got co-op's placeholders (co-op's customization in it)."
           if placeholders else ""))
    return need


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
            others = [(n, p) for n, p in having[:-1] if not is_ours(n, p)]
            if not others:  # (the pack's table alone, or with our seeds only)
                if fit:
                    try:
                        fit_in_place(codec, win_name, win_path, fname, fit, say)
                    except (OSError, ValueError, MergeError) as e:
                        say(f"Size tables: the puppet pack's could not take the merged actor tables' sizes ({e}).")
                        failed += 1
                continue
            pack_in = []
        else:
            if len(having) < 2 or is_ours(win_name, win_path):  # (one table, or our seed: left as it is but the fit)
                if fit:
                    try:
                        fit_in_place(codec, win_name, win_path, fname, fit, say)
                    except (OSError, ValueError, MergeError) as e:
                        say(f"Size tables: {win_name}'s could not take the merged actor tables' sizes ({e}): the "
                            "other players can't be shown with it on.")
                        failed += 1
                continue
            others = having[:-1]
            pack_in = [p for n, p in others if fold(n) == PACK]
        try:
            merged_before = os.path.isfile(os.path.join(win_path, KEEP_DIR, STAMP))
            if pack_in and len(others) == 1 and not merged_before:
                own = load_table(codec, win_path, fname)
                missing = missing_pack_entries(own, load_table(codec, pack_in[0], fname))
                if not fit_ok(own, fit):
                    missing += 1  # (the merged actor tables need larger entries: merged, never 'contains')
                if missing == 0:
                    write_mark(root, win_path, fname, "contains")
                    say(f"Size tables: the game reads {win_name}'s, which holds every puppet pack entry already "
                        "(left as it is): the other players can be shown.")
                    continue
            failed += merge_one(root, codec, fname, win_name, win_path, others, say, fit)
        except (OSError, ValueError, MergeError) as e:
            remove_mark(win_path)  # (an old mark must not keep the puppets on)
            say(f"Size tables: {win_name} could not be merged ({e}): the other players can't be shown with it on.")
            failed += 1
    return failed


def menu_files(pack_path):
    """The pack's files that replace files the game reads at boot (build.py --title-rows / --pause-tab: each language's
    Mals archive, the Title and Common layout archives), from its coop_puppet.json: [romfs path], [] when none."""
    import json
    try:
        with open(os.path.join(pack_path, "coop_puppet.json"), "rb") as f:
            m = json.loads(f.read().decode("utf-8"))
    except (OSError, ValueError):
        return []
    keys = m.get("menu_keys")
    if not isinstance(keys, list) or not all(isinstance(k, str) for k in keys):
        keys = list(ui_pack.OLD_MENU_KEYS)
    out = []
    for key in keys:
        for rel in m.get(key) or []:
            if isinstance(rel, str) and rel and rel not in out:
                out.append(rel)
    return out


def charsel_native_parked(pack_path):
    """Whether co-op's own Characters tab's files (coop_puppet.json "charsel_native", one of the menu lists) are kept
    aside in <pack>/_coop-menus (guard_menus, park_menus, the kit's Move-KitMenuFilesAside) and not in the pack's romfs.
    The tab's code patch (charsel-native.ips in the pack's exefs) must go with them: the game built with the tab's code
    over the game's own three-tab menu files said MsgNotFound on slot 0, lost the Map tab and left L dead. So the kit's
    code-patch rule (kit-paths.ps1 Sync-KitCharselNativePatch, build.sh charsel_native_sync) leaves the code out while
    this is true, and the start parks or puts back before that rule runs."""
    import json
    try:
        with open(os.path.join(pack_path, "coop_puppet.json"), "rb") as f:
            m = json.loads(f.read().decode("utf-8"))
    except (OSError, ValueError):
        return False
    rels = m.get("charsel_native") if isinstance(m, dict) else None
    for rel in rels if isinstance(rels, list) else []:
        if not isinstance(rel, str) or not rel:
            continue
        parts = rel.split("/")
        if (os.path.isfile(os.path.join(pack_path, MENU_KEEP, *parts))
                and not os.path.isfile(os.path.join(pack_path, "romfs", *parts))):
            return True
    return False


def _menu_short(codec, order, pack_path, files):
    """(the mod whose size table the game reads, how many of the menu files it holds smaller than the pack's own table
    does), or (None, 0) when the game reads the pack's own table or one that holds every menu file's size."""
    for fname in tables_of(pack_path):
        having = [(n, p) for n, p in order if fname in tables_of(p)]
        win_name, win_path = having[-1]
        if fold(win_name) == PACK:
            continue
        theirs, ours = load_table(codec, win_path, fname), load_table(codec, pack_path, fname)
        short = 0
        for rel in files:
            key = rel[:-3] if rel.endswith(".zs") else rel
            need = ours.get(key)
            if need is not None and (theirs.get(key) or -1) < need:
                short += 1
        if short:
            return win_name, short
    return None, 0


def guard_menus(root, romfs, say=print):
    """The menu files only where the game can load them. They are larger than the game's own (more labels, more panes),
    so the size table the game reads must hold the pack's sizes for them: the game loads a file larger than its entry
    as a failed resource, and the language archive failing at boot crashed the game (0xc0000005 reading guest 0x8 in
    sead::MainThread after 'prompts: switch icons': a copy of an older pack left in mods/contents as
    coop-puppet.old-b26 sorted after the pack, so its table won). When another mod's table wins and lacks them (a
    merge that failed), the menu files go to
    <pack>/_coop-menus (beside its romfs: the game never reads them there) and the game keeps its own menus (the module
    keeps the title rows and the pause tab off without their panes). They go back at the first start the table holds
    them again; a pack made since (its files in romfs again) drops the kept copies. Returns 1 when it could not move
    a file, else 0."""
    order = load_order(root)
    pack_path = next((p for n, p in order if fold(n) == PACK), None)
    if pack_path is None:
        return 0
    files = menu_files(pack_path)
    if not files:
        return 0
    keep = os.path.join(pack_path, MENU_KEEP)
    try:
        win_name, short = _menu_short(Codec(romfs), order, pack_path, files)
    except (OSError, ValueError, MergeError) as e:
        win_name, short = f"a mod's (it could not be read: {e})", len(files)
    try:
        if short:
            moved = 0
            for rel in files:
                src = os.path.join(pack_path, "romfs", *rel.split("/"))
                if os.path.isfile(src):
                    dst = os.path.join(keep, *rel.split("/"))
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    os.replace(src, dst)
                    moved += 1
            if moved or os.path.isdir(keep):
                say(f"Menus: the game reads {win_name}'s size table, which lacks the sizes of {short} of the puppet "
                    "pack's menu files: the title's co-op rows and the pause menu's Multiplayer tab are off this start "
                    "(the game's own menus; co-op still works from the Minus menu). Turn that mod off, or merge the "
                    "pack's entries into its table, and they come back.")
            return 0
        back = 0
        for rel in files:
            src = os.path.join(keep, *rel.split("/"))
            dst = os.path.join(pack_path, "romfs", *rel.split("/"))
            if not os.path.isfile(src):
                continue
            if os.path.isfile(dst):  # (a pack made since: its own file is the one)
                os.remove(src)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            os.replace(src, dst)
            back += 1
        if os.path.isdir(keep):
            shutil.rmtree(keep, ignore_errors=True)
        if back:
            say(f"Menus: the size table the game reads holds the puppet pack's menu files again: the title's co-op "
                f"rows and the pause menu's Multiplayer tab are back ({back} files).")
        return 0
    except OSError as e:
        say(f"Menus: the puppet pack's menu files could not be moved ({e}).")
        return 1


def park_menus(root, say=print):
    """Row 235's other doors: a merge that could not finish (no zstandard package, an unreadable file) cannot say whether
    the size table the game reads holds the menu files' sizes, and a pack's larger menu files over a smaller entry
    crashed the game at the title. So, unchecked, they go to <pack>/_coop-menus as in guard_menus; the next run that can
    check puts them back. Returns the number of files moved (never raises)."""
    moved = 0
    try:
        order = load_order(root)
        pack_path = next((p for n, p in order if fold(n) == PACK), None)
        if pack_path is None:
            return 0
        # Only where another mod that is on could win a table the pack has (the last mod in the load order having that
        # table name is the one the game reads): when the pack's own table wins, its menus are safe, no check needed.
        if all(fold(having[-1][0]) == PACK
               for having in ([(n, p) for n, p in order if fname in tables_of(p)] for fname in tables_of(pack_path))
               if having):
            return 0
        keep = os.path.join(pack_path, MENU_KEEP)
        for rel in menu_files(pack_path):
            src = os.path.join(pack_path, "romfs", *rel.split("/"))
            if os.path.isfile(src):
                dst = os.path.join(keep, *rel.split("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.replace(src, dst)
                moved += 1
    except OSError:
        pass
    if moved:
        say(f"Menus: the title's co-op rows and the pause menu's Multiplayer tab are off this start ({moved} files "
            "kept aside: the size tables could not be checked); the next start that can merge puts them back.")
    return moved


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


# Row 202 (0.11.8: "customizing Link's colours does nothing" with Character Select on): your own Link wears the
# pack's copy of each Link model that names co-op's own tunic textures (the S copies the module finds and repaints:
# Set-KitSelfTunic, build.py's own model). Character Select ships its own Model/Player_Child and Player_Adult, which
# win that file, so your Link named the game's shared tunic textures and the module found none of yours. At every
# start each such copy of another mod gets the same names (tunic.model_for / tunic_extra.model_names, green, letter
# S): every mod that ships one, wherever it sits (the old top folder or under contents/), so whichever wins, your
# Link names yours. Its own copy is kept in <mod>/_coop-models/<path> with a stamp (`out <the file written>`);
# --undo puts it back. Only when the pack has your own tunic textures (the kit put them in).
MODELS_KEEP = "_coop-models"
MODELS_HEAD = "coop-models 1"
SELF_MODELS = {"Child": "Model/Player_Child.Player_Child.bfres.zs", "Adult": "Model/Player_Adult.Player_Adult.bfres.zs"}


def self_model(data, age):
    """A Link model with co-op's own tunic names (build.py's own copy for green)."""
    names = [n for n, ages in tunic_extra.EXTRA.items() if age in ages]
    out = tunic.model_for(data, "green", dark=True, letter=tunic.SELF_LETTER)
    return tunic_extra.model_names(out, "green", names, letter=tunic.SELF_LETTER)


def put_back_models(root, name, path, say=print):
    """A mod's own Link models back where merge_self_models wrote them (a file changed since, a new copy of the mod,
    keeps its own), and its kept folder gone. Returns the models put back; None when the mod keeps none."""
    models_keep = os.path.join(path, MODELS_KEEP)
    if not os.path.isdir(models_keep):
        return None
    back = []
    for rel in SELF_MODELS.values():
        kept = os.path.join(models_keep, *rel.split("/"))
        stamp = read_text(kept + ".txt")
        target = os.path.join(path, "romfs", *rel.split("/"))
        out = next((l[4:] for l in (stamp or "").split("\n") if l.startswith("out ")), None)
        if os.path.isfile(kept) and out and os.path.isfile(target) and out == file_line(root, target):
            shutil.copyfile(kept, target)
            back.append(rel)
    shutil.rmtree(models_keep)
    say(f"Your Link's colours: {name}'s own {', '.join(back)} back." if back else
        f"Your Link's colours: {name} has other Link models than the ones written: left as they are.")
    return back


def merge_self_models(root, romfs, say=print):
    """Every other mod's copy of a Link model gets co-op's own tunic names. Returns the copies not done. With the pack
    off, gone or without your own tunic textures those names would name textures no mod has, so every mod's own
    models go back (put_back_models, as --undo does) and nothing is named."""
    order = load_order(root)
    pack_path = next((p for n, p in order if fold(n) == PACK), None)
    own_tile = None if pack_path is None else os.path.join(
        pack_path, "romfs", "Tex", tunic.tile_name("green", tunic.SELF_LETTER) + ".bntx.zs")
    if own_tile is None or not os.path.isfile(own_tile):
        for name, path in find_mods(root).items():
            if fold(name) != PACK:
                put_back_models(root, name, path, say)
        if own_tile is not None:
            # Row 265 (0.11.12): the pack on, but its romfs has none of your own tunic textures, because the start that
            # puts them in (self_tunic.py, kit-paths.ps1 Set-KitSelfTunic) did not run before this merge (a lane's own
            # install). A mod's Link model then wins and names the game's shared tunic textures: belt and boots take
            # your colours but the tunic never does, in the field and in Customize's preview. Said at every start.
            for name, path in order:
                shipped = [rel for rel in SELF_MODELS.values()
                           if fold(name) != PACK and os.path.isfile(os.path.join(path, "romfs", *rel.split("/")))]
                if shipped:
                    say(f"Your Link's colours: {name} has its own {', '.join(shipped)} and the puppet pack has none "
                        "of your own tunic textures (put in by the kit's start before this merge: "
                        "tools/puppet_pack/self_tunic.py): your Link's tunic keeps that model's colours.")
        return 0
    codec = Codec(romfs)
    failed = 0
    for name, path in order:
        if fold(name) == PACK:
            continue
        for age, rel in SELF_MODELS.items():
            target = os.path.join(path, "romfs", *rel.split("/"))
            if not os.path.isfile(target):
                continue
            kept = os.path.join(path, MODELS_KEEP, *rel.split("/"))
            stamp_path = kept + ".txt"
            stamp = read_text(stamp_path)
            if stamp is not None and os.path.isfile(kept) and f"out {file_line(root, target)}" in stamp.split("\n"):
                continue   # (done at an earlier start, and still the file written)
            try:
                if stamp is None or not os.path.isfile(kept):   # (the mod's own: the first start with it)
                    os.makedirs(os.path.dirname(kept), exist_ok=True)
                    shutil.copyfile(target, kept + ".tmp")
                    os.replace(kept + ".tmp", kept)
                elif f"out {file_line(root, target)}" not in stamp.split("\n"):
                    shutil.copyfile(target, kept + ".tmp")   # (a new copy of the mod: its own again)
                    os.replace(kept + ".tmp", kept)
                with open(kept, "rb") as f:
                    raw = f.read()
                data = self_model(codec.unpack(raw), age)
                write_atomic(target, codec.pack(data, raw))
                write_atomic(stamp_path, f"{MODELS_HEAD}\nout {file_line(root, target)}\n".encode("utf-8"))
                say(f"Your Link's colours: {name}'s {rel} names co-op's own tunic textures now (its own copy kept in "
                    f"{name}/{MODELS_KEEP}).")
            except (OSError, ValueError, MergeError, tunic.TunicError) as e:
                failed += 1
                say(f"Your Link's colours: {name}'s {rel} could not be given co-op's own tunic names ({e}): with it "
                    "on, your Link's tunic colour does not change.")
    return failed


GAMEDATA = ("romfs", "GameData")
GAMEDATA_TAIL = ".byml.zs"
GAMEDATA_KEEP = "_coop-gamedata"   # a mod's own copy of a saved values list other mods' entries were merged into
GAMEDATA_HEAD = "coop-gamedata 1"


def gamedata_of(path):
    d = os.path.join(path, *GAMEDATA)
    try:
        return sorted(f for f in os.listdir(d) if f.endswith(GAMEDATA_TAIL) and os.path.isfile(os.path.join(d, f)))
    except OSError:
        return []


def saved_of(data):
    """A saved values list (GameDataList): (its root, {(type, hash): entry}). A type whose entries have no Hash is one
    entry as a whole, under (type, "")."""
    root = byml.loads(data)
    if not isinstance(root, dict) or not isinstance(root.get("Data"), dict):
        raise MergeError("not a list of saved values (no Data)")
    out = {}
    for kind, entries in root["Data"].items():
        if isinstance(entries, list) and entries and all(isinstance(e, dict) and "Hash" in e for e in entries):
            for e in entries:
                out[(kind, str(e["Hash"]))] = e
        else:
            out[(kind, "")] = entries
    return root, out


def merge_gamedata(root, romfs, say=print):
    """SHARED_NOTE, for the game's list of saved values (GameData/GameDataList): a seed changes the defaults of some
    values, the Character Select mod adds its own (the picked characters) and with them the save's layout (MetaData).
    The game reads one mod's list, so either the seed's defaults or the mod's values were gone. The winner's copy
    becomes: the list of the one mod that adds values (its layout with it), with every value another mod changed from
    the game's own, the winner's change first where two changed one. Two mods that both add values cannot be put
    together here (two layouts): said, and nothing is written. The winner's own copy is kept in
    `<mod>/_coop-gamedata/` with `<file>.txt` (`coop-gamedata 1` / `out` / `in` per other mod / `fit`), as merge_rows.

    Returns {size path: the size the merged file needs}."""
    order = load_order(root)
    codec = Codec(romfs)
    fit = {}
    touched = set()
    for step in ("plan", "merge"):
        fit = {}
        tell = say if step == "merge" else (lambda line: None)
        if step == "merge":
            put_back_stale(root, GAMEDATA_KEEP, lambda path, rel: os.path.join(path, *GAMEDATA, rel), touched,
                           say, "Saved values")
        for fname in sorted({f for _, p in order for f in gamedata_of(p)}):
            having = [(n, p) for n, p in order if fname in gamedata_of(p)]
            if len(having) < 2:
                continue
            (win_name, win_path), others = having[-1], having[:-1]
            names = ", ".join(n for n, _ in others)
            game = os.path.join(romfs, "GameData", fname) if romfs else ""
            if not os.path.isfile(game):
                tell(f"Saved values: {win_name} and {names} both ship GameData/{fname}; the game reads {win_name}'s. Not "
                    "merged without the game's own copy (--romfs): the others' values are missing.")
                continue
            kept = os.path.join(win_path, GAMEDATA_KEEP, fname)
            if step == "plan":
                touched.add(os.path.normcase(os.path.abspath(kept)))
                continue
            try:
                need = merge_gamedata_one(root, codec, fname, win_name, win_path, others, game, say)
            except (OSError, ValueError, KeyError, IndexError, TypeError, MergeError) as e:
                tell(f"Saved values: {win_name}'s GameData/{fname} could not be merged with what {names} changed ({e}): "
                    "their values are missing. Turn one of these mods off.")
                continue
            if need:
                key = "GameData/" + fname[:-3]
                fit[key] = max(fit.get(key, 0), need)
    return fit


def merge_gamedata_one(root, codec, fname, win_name, win_path, others, game, say):
    """Merges one list. Returns the size the written file needs, or 0."""
    path = os.path.join(win_path, *GAMEDATA, fname)
    more = [os.path.join(p, *GAMEDATA, fname) for _, p in others]
    ins = "".join(f"in {file_line(root, t)}\n" for t in more)
    whose = " and ".join(f"{n}'s" for n, _ in others)
    keep = os.path.join(win_path, GAMEDATA_KEEP)
    orig_path = os.path.join(keep, fname)
    stamp_path = orig_path + ".txt"
    old = read_text(stamp_path)
    head = f"{GAMEDATA_HEAD}\nout {file_line(root, path)}\n{ins}"
    if stamp_holds(old, head):
        say(f"Saved values: {win_name}'s GameData/{fname} has {whose} values already (merged at an earlier start).")
        return next((int(l.split()[2]) for l in old.split("\n") if l.startswith("fit ") and len(l.split()) == 3), 0)
    ours = old is not None and old.split("\n")[1:2] == ["out " + file_line(root, path)] and os.path.isfile(orig_path)
    if not ours:  # (the mod's own list: the first start with it, or a new copy of the mod)
        os.makedirs(keep, exist_ok=True)
        shutil.copyfile(path, orig_path + ".tmp")
        os.replace(orig_path + ".tmp", orig_path)
    with open(orig_path, "rb") as f:
        orig_raw = f.read()
    with open(game, "rb") as f:
        games = saved_of(codec.unpack(f.read()))[1]
    orig_data = codec.unpack(orig_raw)
    # The winner first, then the mod nearest to it: the first to change a value decides it.
    mods = [(win_name, win_path, orig_data)]
    for (n, p), t in reversed(list(zip(others, more))):
        with open(t, "rb") as f:
            mods.append((n, p, codec.unpack(f.read())))
    loaded = [(n, p, d) + saved_of(d) for n, p, d in mods]
    adders = [m for m in loaded if any(k not in games for k in m[4])]
    if len(adders) > 1:
        raise MergeError(f"{' and '.join(m[0] for m in adders)} each add saved values of their own, with a save "
                         "layout of their own")
    base = adders[0] if adders else loaded[0]
    decided, both = {}, []
    for n, _, _, _, saved in loaded:
        for k, e in saved.items():
            if k not in games or same(e, games[k]):
                continue
            if k not in decided:
                decided[k] = e
            elif not same(decided[k], e):
                if k[1] == "":   # (a kind without hashes is one block: two mods' blocks cannot be put together)
                    raise MergeError(f"two mods change the {k[0]} values, which cannot be told apart")
                if k not in both:
                    both.append(k)
    changed = base is not loaded[0]
    new_root, base_saved = base[3], base[4]
    for (kind, h), e in decided.items():
        if (kind, h) in base_saved and same(base_saved[(kind, h)], e):
            continue
        changed = True
        if h == "":
            new_root["Data"][kind] = e
        else:
            entries = new_root["Data"][kind]
            i = next((i for i, x in enumerate(entries) if str(x["Hash"]) == h), None)
            if i is None:   # (the mod that adds values took this one out of its list)
                raise MergeError(f"{base[0]}'s list has no {kind} value {h} that another mod changed")
            entries[i] = e
    if changed:
        new_data = byml.dumps(new_root, big=base[2][:2] == b"BY")   # (the game's list is big-endian: kept so)
        data = codec.pack(new_data, orig_raw)
    else:
        new_data, data = orig_data, orig_raw
    key = "GameData/" + fname[:-3]
    need = size_needed(codec, key, len(new_data), [(p, len(d)) for _, p, d, _, _ in loaded])
    rel = os.path.relpath(path, root).replace(os.sep, "/")
    stamp = f"{GAMEDATA_HEAD}\nout {rel} {len(data)} {fnv64_bytes(data)}\n{ins}"
    if need:
        stamp += f"fit {key} {need}\n"
    write_atomic(stamp_path, stamp.encode("utf-8"))   # (the stamp first, as merge_rows_one)
    write_atomic(path, data)
    say(f"Saved values: the game reads {win_name}'s GameData/{fname}; merged with {whose} ({len(decided)} changed "
        f"values kept{', the added ones and the save layout from ' + base[0] if adders else ''}). Its own copy is "
        f"kept in {win_name}/{GAMEDATA_KEEP}."
        + (f" {len(both)} values were changed by two mods: the one nearer the top of the list decides." if both else ""))
    return need


ONE_TIME_FILE = "coop-one-time-keys.txt"   # beside the mods (the module's kOneTimeFile, coop_pickups.h)
ONE_TIME_HEAD = "coop-one-time-keys 1"
KEY_TABLE = "Bool64bitKey"   # the game's one table of 64-bit-key flags, keyed by a placement's Hash
SAVED_ONCE = 24              # ResetTypeValue of a placement the save keeps done (chests, heart pieces)
RESETTING = (88, 280)        # ResetTypeValue of pots, crates, rocks (88) and enemies, flying pots (280)
BANC = "Banc"   # (under the game's romfs)
# The placed one-time pickups by actor name: CoopPickups::IsPickup's kinds (Gold Skulltulas and their tokens, heart
# containers and pieces, placed rupees and hearts, silver rupees) and the game's other placed items it saves once
# (a Deku Nut, Seed and Stick, the fire arrows).
PICKUP_PREFIXES = ("Obj_Rupee_", "Obj_RupeeToken_", "Obj_Heart", "Obj_DekuNut", "Obj_DekuSeed", "Obj_DekuStick",
                   "Obj_Arrow_")
PICKUP_NAMES = ("Enemy_Skulltula_Gold", "Obj_Token_Gold")


def _num(v):
    return v.v if isinstance(v, byml._Num) else v


def is_placed_pickup(name):
    return name in PICKUP_NAMES or name.startswith(PICKUP_PREFIXES) or (
        name.startswith("Obj_") and "Heart_Piece" in name)


def key_rows(data):
    """{Hash: (ResetTypeValue, ExtraByte)} of a saved values list's 64-bit-key table."""
    root = byml.loads(data)
    rows = root.get("Data", {}).get(KEY_TABLE) if isinstance(root, dict) else None
    if not isinstance(rows, list):
        raise MergeError(f"no {KEY_TABLE} table")
    out = {}
    for r in rows:
        if isinstance(r, dict) and "Hash" in r:
            out[int(_num(r["Hash"]))] = (_num(r.get("ResetTypeValue", -1)), _num(r.get("ExtraByte", 0)) or 0)
    return out


def saved_once(row):
    """A row the save keeps once done: type 24 and no ExtraByte (OOTRR pots.rs permanent)."""
    return row is not None and row[0] == SAVED_ONCE and row[1] == 0


def placed_pickups(codec, romfs):
    """{Hash: actor name} of every placed one-time pickup in the game's own Banc (a seed keeps the placement's Hash
    when it swaps the actor for another item, OOTRR slice/pickups.rs)."""
    out = {}

    def walk(v):
        if isinstance(v, dict):
            g, h = v.get("Gyaml"), v.get("Hash")
            if isinstance(g, str) and h is not None and is_placed_pickup(g):
                out[int(_num(h))] = g
            for x in v.values():
                walk(x)
        elif isinstance(v, list):
            for x in v:
                walk(x)

    for dirpath, _, fnames in os.walk(os.path.join(romfs, BANC)):
        for f in sorted(fnames):
            if ".bcett.byml" not in f:
                continue
            with open(os.path.join(dirpath, f), "rb") as fh:
                walk(byml.loads(codec.unpack(fh.read())))
    return out


def one_time_keys(root, romfs, say=print):
    """Row 219 (#84): the 64-bit keys of every placement the game, as it is served, gives once, written to
    `<mods>/coop-one-time-keys.txt` for the co-op module, which shares them from the session's start (the host's
    snapshot carries them to a player who joins or loads later, and a copy still loaded in another game goes):

    - `pickup <hex>`: a placed one-time pickup of the game's own Banc whose key the served list keeps saved once;
    - `holder <hex>`: a key the served list keeps saved once and the game's own list resets (88, 280): a randomizer
      seed's shuffled pot, crate, flying pot, beehive, rock or grass (OOTRR pot_slice/persist.rs), which gives its
      item once.

    The served list is the winner's GameData/<the game's list> (merge_gamedata ran first), else the game's own; its
    line `served <path under mods> <FNV-1a 64>` (or `served - -`) lets the module drop the holders when that list
    changed since (a seed built or removed without a kit start). The file is written again only when the `game` or
    `served` line changed. Returns 1 when it could not be written (said why), else 0."""
    out_path = os.path.join(root, ONE_TIME_FILE)
    gd = os.path.join(romfs, *GAMEDATA[1:]) if romfs else ""
    lists = sorted(f for f in os.listdir(gd) if f.startswith("GameDataList") and f.endswith(GAMEDATA_TAIL)) \
        if gd and os.path.isdir(gd) else []
    if len(lists) != 1 or not os.path.isdir(os.path.join(romfs, BANC)):
        if os.path.isfile(out_path):
            os.remove(out_path)   # (no game files: an old list must not stay)
        say("One-time pickups: not listed without the game's own GameData and Banc (--romfs): each is shared once "
            "seen in a game, as before.")
        return 0
    fname = lists[0]
    game_path = os.path.join(gd, fname)
    having = [p for _, p in load_order(root) if fname in gamedata_of(p)]
    served_path = os.path.join(having[-1], *GAMEDATA, fname) if having else game_path
    game_line = "game " + fnv64(game_path)[0]
    served_rel = os.path.relpath(served_path, root).replace(os.sep, "/") if having else "-"
    served_line = f"served {served_rel} {fnv64(served_path)[0] if having else '-'}"   # (the hash last: a path has spaces)
    old = (read_text(out_path) or "").split("\n")
    if old[:3] == [ONE_TIME_HEAD, game_line, served_line]:
        say("One-time pickups: listed already (the same game and saved values list as at an earlier start).")
        return 0
    try:
        codec = Codec(romfs)
        with open(game_path, "rb") as f:
            game_rows = key_rows(codec.unpack(f.read()))
        served_rows = game_rows
        if having:
            with open(served_path, "rb") as f:
                served_rows = key_rows(codec.unpack(f.read()))
        pickups = sorted(h for h in placed_pickups(codec, romfs) if saved_once(served_rows.get(h)))
        holders = sorted(h for h, r in served_rows.items()
                         if saved_once(r) and h in game_rows and game_rows[h][0] in RESETTING)
    except (OSError, ValueError, KeyError, TypeError, MergeError) as e:
        if os.path.isfile(out_path):
            os.remove(out_path)
        say(f"One-time pickups: not listed ({e}): each is shared once seen in a game, as before.")
        return 1
    text = "\n".join([ONE_TIME_HEAD, game_line, served_line] + [f"pickup {h:016x}" for h in pickups]
                     + [f"holder {h:016x}" for h in holders]) + "\n"
    write_atomic(out_path, text.encode("utf-8"))
    say(f"One-time pickups: {len(pickups)} placed pickups and {len(holders)} one-time holders"
        f"{' of ' + served_rel if having else ''} listed for co-op ({ONE_TIME_FILE}).")
    return 0


def undo(root, say=print):
    """Puts each kept original back, but only over the table the merge wrote: a new seed put into the folder since
    keeps its own table (the kept one is the old seed's). The kept folder and the mark go either way."""
    n = 0
    if os.path.isfile(os.path.join(root, ONE_TIME_FILE)):   # (one_time_keys: written again at the next start)
        os.remove(os.path.join(root, ONE_TIME_FILE))
    for name, path in find_mods(root).items():
        put_back_models(root, name, path, say)   # (merge_self_models: each Link model back where it was written)
        gamedata_keep = os.path.join(path, GAMEDATA_KEEP)
        if os.path.isdir(gamedata_keep):   # (merge_gamedata: each list back where the merge wrote it)
            back = []
            for f in sorted(os.listdir(gamedata_keep)):
                stamp = read_text(os.path.join(gamedata_keep, f + ".txt")) if f.endswith(GAMEDATA_TAIL) else None
                out = next((l[4:] for l in (stamp or "").split("\n") if l.startswith("out ")), None)
                target = os.path.join(path, *GAMEDATA, f)
                if out and os.path.isfile(target) and out == file_line(root, target):
                    shutil.copyfile(os.path.join(gamedata_keep, f), target)
                    back.append(f)
            shutil.rmtree(gamedata_keep)
            say(f"Saved values: {name}'s own {', '.join(back)} back." if back else
                f"Saved values: {name} has another list than the merged one now: left as it is.")
            n += 1
        members_keep = os.path.join(path, MEMBERS_KEEP)
        if os.path.isdir(members_keep):   # (merge_members: each archive back where the merge wrote it)
            back = []
            for dirpath, _, fnames in os.walk(members_keep):
                for f in fnames:
                    if f.endswith(".txt"):
                        continue
                    kept = os.path.join(dirpath, f)
                    rel = os.path.relpath(kept, members_keep).replace(os.sep, "/")
                    stamp = read_text(kept + ".txt")
                    out = next((l[4:] for l in (stamp or "").split("\n") if l.startswith("out ")), None)
                    target = os.path.join(path, "romfs", *rel.split("/"))
                    if out and os.path.isfile(target) and out == file_line(root, target):
                        shutil.copyfile(kept, target)
                        back.append(rel)
            shutil.rmtree(members_keep)
            say(f"Menus: {name}'s own {', '.join(sorted(back))} back." if back else
                f"Menus: {name} has other archives than the merged ones now: left as they are.")
            n += 1
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
        for k, n in merge_gamedata(a.mods, a.romfs).items():
            fit[k] = max(fit.get(k, 0), n)
        once_failed = one_time_keys(a.mods, a.romfs)   # (row 219: on the saved values list the game will read)
        members_failed, members_fit = merge_members(a.mods, a.romfs)
        for k, n in members_fit.items():
            fit[k] = max(fit.get(k, 0), n)
        models_failed = merge_self_models(a.mods, a.romfs)   # (row 202: your Link's colours with Character Select)
        failed = merge_mods(a.mods, a.romfs, fit=fit) + members_failed + models_failed
        # Last, on the tables as the game will read them: the pack's menu files only where they can load.
        menus_failed = guard_menus(a.mods, a.romfs)
        return 1 if (failed or rows_failed or menus_failed or once_failed) else 0
    except SystemExit:  # (zs._zstd: no zstandard package)
        print("Size tables: not merged (this Python has no zstandard package): with a mod that has its own size "
              "table, the other players can't be shown.")
        park_menus(a.mods)
        return 1
    except Exception as e:  # (a file the merge could not read: the menus must not be left unchecked)
        print(f"Size tables: not merged ({type(e).__name__}: {e}).")
        park_menus(a.mods)
        return 1


if __name__ == "__main__":
    sys.exit(main())
