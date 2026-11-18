"""Adds UI parts and text to the game's layouts and message files, from the player's own game at
pack time (issue #264, piece 3; docs/hookshot.md "Adding parts and text to the game's layouts").

Nothing from the game is in this file or in strings/: the pack step reads the player's own romfs,
edits the files in memory and writes them into the mod's romfs. title_menu.py (the title's rows)
is built on it.

  clone_part, clone_parts   a copy of a part of a layout (Layout2 .buil in UI/Layout2Archive)
  move_panes                panes of a layout moved (their translate), nothing else changed
  frame_offset              where a pane's parent sits in an ancestor's frame (a copy's new place)
  edit_entries              every copy of a file in an archive (some hold a layout four times)
  add_labels                new labels and texts in a message file (MSBT, Mals/<language>...)
  load_strings              our own words: one file per language in strings/, English fallback

The Layout2 resource (BFFH "Nintendo.Ui.ResModels.LayoutResData"), as far as read here:
  - absolute u64 offsets from the file's start; u32 file size at +0x08; BFSI section headers near
    the start (data offset, size, name at +0x18): Default (the records) and StringPool (the
    strings, at the file's end);
  - the root follows "UIL" at the Default section's start (+8). It holds dictionaries of
    {keys* (names), values* (records), ids*, u32 count, u32 stride=8}: panes at +0x18, parts at
    +0xb8;
  - a pane record is 0x44 bytes: +0x10 and +0x14 its scale (f32; 1.0 in most panes), +0x18 its
    rotation (f32, radians), +0x1c translate x and +0x20 translate y (f32, y up), +0x28 its
    parent's index (-1 for the root), +0x2c and +0x30 its size (f32), +0x34 the index past its
    subtree (panes are in pre-order), +0x38 its kind, +0x42 u16 its index in its kind's table (a
    part's: in the part dictionary);
  - a part record is 0x48 bytes: +0x00 the part layout's name, +0x08 and +0x18 two lists of
    overrides ({u64 offset, u32 count, u32 stride}; not copied here), +0x28 its parameters (a
    list of offsets of {name, f32 value, 4 zero bytes}), +0x38 the offset of its properties list
    (16 bytes; empty on the parts copied here), +0x40 u64 its pane's index;
  - ids are 64-bit identities of the panes, also the parts' ids: CityHash64 of the name (part_id);
    the game finds a pane by name through it;
  - the animations appear to name panes by index: MenuOverlay_00's four tabs, panes 8 to 11,
    show up in its animation strings as 0x40, 0x48, 0x50 and 0x58.

How a copy is added, so that nothing the layout already has moves:
  - the new pane goes at the end of the pane order and the new part at the end of the part order:
    no existing index moves;
  - its name, its records, the grown dictionaries and its own copies of the blocks its records
    point to (strings excepted) go after the string pool, which grows to the file's new end: no
    existing offset moves. The old dictionary arrays stay where they were, unused. No block is
    shared between two records, as in the game's own layouts;
  - the parent and the parent's ancestors whose subtree reaches the last pane are made to reach
    the copy too. When the parent's subtree ends before the last pane (MenuOverlay_00's tabs),
    the copy cannot sit inside it without moving indices: it is the parent's child by its parent
    index only. clone_parts refuses that unless out_of_order=True; whether the game draws such a
    copy under its parent is to be checked in game;
  - or the copy goes under another pane (Copy.parent), one whose subtree reaches the last pane, so
    the order holds: an ancestor of the source, for example. Its translate is then in that pane's
    frame: frame_offset gives the move that keeps it where the source's frame would put it.
"""
import math
import os
import re
import struct
from typing import NamedTuple, Optional

from . import msbt

PANE_SIZE = 0x44
PART_SIZE = 0x48
NO_PARENT = 0xFFFFFFFF
_PANES = 0x18                 # the root's pane dictionary
_PARTS = 0xb8                 # its part dictionary


class UiPackError(ValueError):
    pass


class Entry(NamedTuple):
    """A pane or a part of a layout: its name, its record's offset and its id."""
    name: str
    record: int
    id: int


class Copy(NamedTuple):
    """One copy for clone_parts: the part copied, the copy's name, how far it moves (x, y in the
    layout's units, y up, added to the source's translate), new values for some of the part's
    parameters ({name: float}), the pane it goes under (None: the source's parent), and whether it is
    stored hidden (bit 0 of its kind word cleared, as on the game's own panes that start hidden: a
    co-op module shows it, and a game without that module looks as it always did)."""
    source: str
    name: str
    move: tuple = (0.0, 0.0)
    params: Optional[dict] = None
    parent: Optional[str] = None
    hidden: bool = False


def _q(d, o):
    return struct.unpack_from("<Q", d, o)[0]


def _u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def _cstr(d, o):
    return d[o:d.index(b"\0", o)].decode("ascii")


def _q8(xs):
    return b"".join(struct.pack("<Q", x) for x in xs)


def _f32(value, what):
    """value as an f32's 4 bytes; UiPackError when it is not a number that fits one."""
    try:
        if isinstance(value, bool) or not math.isfinite(value):
            raise TypeError
        return struct.pack("<f", value)
    except (TypeError, OverflowError, struct.error):
        raise UiPackError("%s is not a number for an f32 (%r)" % (what, value)) from None


def sections(d):
    """{name: (header offset, data offset, size)} of a layout's BFSI sections."""
    out = {}
    at = 0
    while True:
        at = d.find(b"BFSI", at)
        if at < 0 or at > 0x400:
            return out
        off, size = struct.unpack_from("<II", d, at + 4)
        name = d[at + 0x18:d.index(b"\0", at + 0x18)].decode("ascii")
        out[name] = (at, off, size)
        at += 4


def _root(d):
    sects = sections(d)
    if "Default" not in sects or "StringPool" not in sects:
        raise UiPackError("no Default or StringPool section")
    at = sects["Default"][1]
    if d[at:at + 3] != b"UIL":
        raise UiPackError("no UIL root at %#x" % at)
    return at + 8


def _dict(d, at):
    keys, values, ids = _q(d, at), _q(d, at + 8), _q(d, at + 16)
    count, stride = struct.unpack_from("<II", d, at + 24)
    if stride != 8:
        raise UiPackError("a dictionary of stride %d at %#x" % (stride, at))
    return keys, values, ids, count


def read_layout(layout):
    """The panes and parts of a Layout2 layout, in the file's order: ([Entry], [Entry])."""
    d = bytes(layout)
    if d[:4] != b"BFFH" or b"Nintendo.Ui.ResModels.LayoutResData" not in d[:0x60]:
        raise UiPackError("not a Layout2 layout")
    try:
        root = _root(d)

        def entries(at):
            keys, values, ids, n = _dict(d, root + at)
            return [Entry(_cstr(d, _q(d, keys + 8 * i)), _q(d, values + 8 * i), _q(d, ids + 8 * i))
                    for i in range(n)]

        return entries(_PANES), entries(_PARTS)
    except UiPackError:
        raise
    except (struct.error, IndexError, ValueError) as e:   # cut short; ValueError: no string end
        raise UiPackError("the layout is cut short or damaged (%s)" % e) from e


def part_id(name):
    """The id of a new pane and its part: CityHash64 of its name, as the game's own. The game finds a pane by name
    by hashing the name and looking for that id (main+0x43e300, the hash main+0xebf190, read from main 2026-11-18;
    every pane id of the game's layouts checked is CityHash64 of its name): a copy with another id is never found."""
    return city_hash64(name.encode("ascii"))


# ---------------------------------------------------------------------------------------- CityHash64
# CityHash64, version 1.1, for strings up to 64 bytes (pane names are short), from Google's CityHash
# (Copyright (c) 2011 Google, Inc.; MIT licence; https://github.com/google/cityhash, city.cc), written in
# Python here. THIRD_PARTY_NOTICES.md has its licence.

_M64 = (1 << 64) - 1
_K0, _K1, _K2 = 0xc3a5c85c97cb3127, 0xb492b66fbe98f273, 0x9ae16a3b2f90404f


def _city64(s, i):
    return struct.unpack_from("<Q", s, i)[0]


def _city32(s, i):
    return struct.unpack_from("<I", s, i)[0]


def _rot(v, r):
    return v if r == 0 else ((v >> r) | (v << (64 - r))) & _M64


def _len16(u, v, mul=0x9ddfea08eb382d69):
    a = ((u ^ v) * mul) & _M64
    a ^= a >> 47
    b = ((v ^ a) * mul) & _M64
    b ^= b >> 47
    return (b * mul) & _M64


def _swap(v):
    return int.from_bytes(v.to_bytes(8, "little"), "big")


def city_hash64(s):
    """CityHash64 (v1.1) of bytes, up to 64 of them."""
    n = len(s)
    if n <= 16:
        if n >= 8:
            mul = (_K2 + n * 2) & _M64
            a = (_city64(s, 0) + _K2) & _M64
            b = _city64(s, n - 8)
            return _len16((_rot(b, 37) * mul + a) & _M64, ((_rot(a, 25) + b) * mul) & _M64, mul)
        if n >= 4:
            return _len16((n + (_city32(s, 0) << 3)) & _M64, _city32(s, n - 4), (_K2 + n * 2) & _M64)
        if n > 0:
            y = (s[0] + (s[n >> 1] << 8)) & 0xFFFFFFFF
            z = (n + (s[n - 1] << 2)) & 0xFFFFFFFF
            v = (y * _K2 ^ z * _K0) & _M64
            return ((v ^ (v >> 47)) * _K2) & _M64
        return _K2
    mul = (_K2 + n * 2) & _M64
    if n <= 32:
        a = (_city64(s, 0) * _K1) & _M64
        b = _city64(s, 8)
        c = (_city64(s, n - 8) * mul) & _M64
        d = (_city64(s, n - 16) * _K2) & _M64
        return _len16((_rot((a + b) & _M64, 43) + _rot(c, 30) + d) & _M64,
                      (a + _rot((b + _K2) & _M64, 18) + c) & _M64, mul)
    if n > 64:
        raise UiPackError("a name of %d bytes: names up to 64 only" % n)
    a = (_city64(s, 0) * _K2) & _M64
    b, c, d = _city64(s, 8), _city64(s, n - 24), _city64(s, n - 32)
    e = (_city64(s, 16) * _K2) & _M64
    f = (_city64(s, 24) * 9) & _M64
    g = _city64(s, n - 8)
    h = (_city64(s, n - 16) * mul) & _M64
    u = (_rot((a + g) & _M64, 43) + (_rot(b, 30) + c) * 9) & _M64
    v = (((a + g) ^ d) + f + 1) & _M64
    w = (_swap(((u + v) * mul) & _M64) + h) & _M64
    x = (_rot((e + f) & _M64, 42) + c) & _M64
    y = ((_swap(((v + w) * mul) & _M64) + g) * mul) & _M64
    z = (e + f + c) & _M64
    a = (_swap(((x + z) * mul + y) & _M64) + b) & _M64
    t = ((z + a) * mul + d + h) & _M64
    b = ((t ^ (t >> 47)) * mul) & _M64
    return (b + x) & _M64


def clone_part(layout, source, name, move=(0.0, 0.0), params=None, new_id=part_id, out_of_order=False):
    """The layout (a .buil's bytes) with a copy of its part `source` named `name`, `move` (x, y)
    away, under the same parent; see clone_parts."""
    return clone_parts(layout, [Copy(source, name, move, params)], new_id, out_of_order)


def clone_parts(layout, copies, new_id=part_id, out_of_order=False):
    """The layout (a .buil's bytes) with a copy of a part for each Copy (source, name, move,
    params), in that order, each a new last pane and last part. A copy is its source's pane and
    part records with: the new name and id (new_id(name)), its position moved, its own parameters
    list (with `params` set) and properties list, and the indices of its place. Raises UiPackError
    when any copy cannot be made."""
    d = bytes(layout)
    panes, parts = read_layout(d)
    copies = [c if isinstance(c, Copy) else Copy(*c) for c in copies]
    if not copies:
        return d
    try:
        return _clone(d, panes, parts, copies, new_id, out_of_order)
    except UiPackError:
        raise
    except (struct.error, IndexError, ValueError) as e:   # a record or list past the file's end
        raise UiPackError("the layout is cut short or damaged (%s)" % e) from e


def _clone(d, panes, parts, copies, new_id, out_of_order):
    root = _root(d)
    pool_hdr, pool_off, pool_size = sections(d)["StringPool"]
    if pool_off + pool_size != len(d):
        raise UiPackError("the string pool is not the file's end")
    index = {p.name: i for i, p in enumerate(panes)}
    part_of = {_q(d, p.record + 0x40): p for p in parts}
    parent = [_u32(d, p.record + 0x28) for p in panes]
    end = [_u32(d, p.record + 0x34) for p in panes]
    old_end = list(end)
    taken = {p.name for p in panes} | {p.name for p in parts}
    params_of = []
    for c in copies:
        if c.source not in index or index[c.source] not in part_of:
            raise UiPackError("%s is not a part of the layout" % c.source)
        si = index[c.source]
        if end[si] != si + 1:
            raise UiPackError("%s has panes under it; only a part without child panes is copied" % c.source)
        if parent[si] == NO_PARENT:
            raise UiPackError("%s has no parent" % c.source)
        if c.parent is not None and c.parent not in index:
            raise UiPackError("the layout has no pane %s for %s to go under" % (c.parent, c.name))
        if not re.fullmatch(r"[\x21-\x7e]{1,255}", c.name):
            raise UiPackError("%r is not a name for a pane (printable ASCII, no spaces)" % c.name)
        if c.name in taken:
            raise UiPackError("the layout has a %s already" % c.name)
        taken.add(c.name)
        try:
            for v in c.move:
                _f32(v, "the move of %s" % c.name)
            ok = len(c.move) == 2
        except TypeError:
            ok = False
        if not ok:
            raise UiPackError("the move of %s is not (x, y)" % c.name)
        rec = part_of[si].record
        if any(d[rec + 0x08:rec + 0x28]):
            raise UiPackError("%s overrides panes of its part (+0x08, +0x18), which is not copied here" % c.source)
        props = _q(d, rec + 0x38)
        if props and any(d[props:props + 16]):
            raise UiPackError("%s has a properties list, which is not copied here" % c.source)
        plist, pcount, pstride = _q(d, rec + 0x28), _u32(d, rec + 0x30), _u32(d, rec + 0x34)
        if pcount and pstride != 8:
            raise UiPackError("the parameters of %s have a stride of %d" % (c.source, pstride))
        entries = [_q(d, plist + 8 * j) for j in range(pcount)]
        names = [_cstr(d, _q(d, e)) for e in entries]
        for p, v in (c.params or {}).items():
            if p not in names:
                raise UiPackError("%s has no parameter %s" % (c.source, p))
            _f32(v, "parameter %s of %s" % (p, c.name))
        params_of.append(list(zip(names, entries)))
    ids = [new_id(c.name) for c in copies]
    known = {p.id for p in panes} | {p.id for p in parts}
    for c, i in zip(copies, ids):
        if i in known:
            raise UiPackError("the id of %s is taken" % c.name)
        known.add(i)

    out = bytearray(d)

    def put(b):
        while len(out) % 8:
            out.append(0)
        at = len(out)
        out.extend(b)
        return at

    # the new names, after the pool's strings
    name_at = []
    for c in copies:
        name_at.append(len(out))
        out.extend(c.name.encode("ascii") + b"\0")
    # the new pane records, each the last pane and its own subtree
    n0, p0 = len(panes), len(parts)
    if p0 + len(copies) > 0xFFFF:
        raise UiPackError("too many parts")
    pane_recs = []
    for i, c in enumerate(copies):
        k = n0 + i
        si = index[c.source]
        r = bytearray(d[panes[si].record:panes[si].record + PANE_SIZE])
        for at, by in ((0x1c, c.move[0]), (0x20, c.move[1])):
            if by:
                r[at:at + 4] = _f32(struct.unpack_from("<f", r, at)[0] + by, "the position of %s" % c.name)
        struct.pack_into("<I", r, 0x34, k + 1)
        struct.pack_into("<H", r, 0x42, p0 + i)
        if c.hidden:
            struct.pack_into("<I", r, 0x38, struct.unpack_from("<I", r, 0x38)[0] & ~1)   # (bit 0: shown at start)
        up = parent[si] if c.parent is None else index[c.parent]
        if end[up] != k and not out_of_order:
            raise UiPackError("the subtree of %s (where %s goes) ends before the last pane: the copy would be "
                              "its child by its parent index only (out_of_order=True), or put it under a pane "
                              "that reaches the last pane (Copy.parent)" % (panes[up].name, c.name))
        struct.pack_into("<I", r, 0x28, up)
        mine = up
        while up != NO_PARENT:
            if end[up] == k:
                end[up] = k + 1   # the subtrees that reached the last pane reach the copy
            up = parent[up]
        parent.append(mine)
        end.append(k + 1)
        pane_recs.append(put(bytes(r)))
    for i, p in enumerate(panes):
        if end[i] != old_end[i]:
            struct.pack_into("<I", out, p.record + 0x34, end[i])
    # the pane dictionary, grown
    keys, values, idarr, n = _dict(d, root + _PANES)
    keys = put(d[keys:keys + 8 * n] + _q8(name_at))
    values = put(d[values:values + 8 * n] + _q8(pane_recs))
    idarr = put(d[idarr:idarr + 8 * n] + _q8(ids))
    struct.pack_into("<QQQI", out, root + _PANES, keys, values, idarr, n + len(copies))
    # the part records, each with its own parameters and properties lists, and the part dictionary
    part_recs = []
    for i, (c, plist) in enumerate(zip(copies, params_of)):
        src = part_of[index[c.source]].record
        p = bytearray(d[src:src + PART_SIZE])
        if plist:
            own = []
            for pname, e in plist:
                entry = bytearray(d[e:e + 16])
                if c.params and pname in c.params:
                    entry[8:12] = _f32(c.params[pname], "parameter %s of %s" % (pname, c.name))
                own.append(put(bytes(entry)))
            struct.pack_into("<Q", p, 0x28, put(_q8(own)))
        props = _q(d, src + 0x38)
        if props:
            struct.pack_into("<Q", p, 0x38, put(d[props:props + 16]))
        struct.pack_into("<Q", p, 0x40, n0 + i)
        part_recs.append(put(bytes(p)))
    keys, values, idarr, n = _dict(d, root + _PARTS)
    keys = put(d[keys:keys + 8 * n] + _q8(name_at))
    values = put(d[values:values + 8 * n] + _q8(part_recs))
    idarr = put(d[idarr:idarr + 8 * n] + _q8(ids))
    struct.pack_into("<QQQI", out, root + _PARTS, keys, values, idarr, n + len(copies))
    while len(out) % 8:
        out.append(0)
    # the pool grows to the file's new end; the header's size too
    struct.pack_into("<I", out, pool_hdr + 8, len(out) - pool_off)
    struct.pack_into("<I", out, 0x08, len(out))
    return bytes(out)


def move_panes(layout, moves):
    """The layout (a .buil's bytes) with each pane named in `moves` ({name: (x, y)}) moved that far:
    its translate (+0x1c, +0x20) and nothing else. Raises UiPackError for a name the layout has not
    or a move that is not (x, y)."""
    d = bytearray(layout)
    panes, _ = read_layout(bytes(d))
    by_name = {p.name: p for p in panes}
    for name, move in moves.items():
        if name not in by_name:
            raise UiPackError("the layout has no pane %s to move" % name)
        try:
            ok = len(move) == 2
        except TypeError:
            ok = False
        if not ok:
            raise UiPackError("the move of %s is not (x, y)" % name)
        rec = by_name[name].record
        for at, by in ((0x1c, move[0]), (0x20, move[1])):
            _f32(by, "the move of %s" % name)
            if by:
                d[rec + at:rec + at + 4] = _f32(struct.unpack_from("<f", d, rec + at)[0] + by, "the position of %s" % name)
    return bytes(d)


def frame_offset(layout, pane, ancestor):
    """(x, y): where the frame of `pane`'s parent sits in the frame of `ancestor`, one of its
    ancestors: the translates of the panes between them (the parent included, the ancestor not)
    added up. So a copy of `pane` that goes under `ancestor` (Copy.parent) and moves by this much
    sits where `pane` sits. Raises UiPackError when `ancestor` is not an ancestor of `pane`, or when a
    pane between them is scaled or turned (its frame is not a plain move)."""
    d = bytes(layout)
    panes, _ = read_layout(d)
    index = {p.name: i for i, p in enumerate(panes)}
    for name in (pane, ancestor):
        if name not in index:
            raise UiPackError("the layout has no pane %s" % name)
    x = y = 0.0
    at = _u32(d, panes[index[pane]].record + 0x28)
    while at != index[ancestor]:
        if at == NO_PARENT or at >= len(panes):
            raise UiPackError("%s is not an ancestor of %s" % (ancestor, pane))
        rec = panes[at].record
        sx, sy, turn, tx, ty = struct.unpack_from("<5f", d, rec + 0x10)
        if (sx, sy, turn) != (1.0, 1.0, 0.0):
            raise UiPackError("%s, between %s and %s, is scaled or turned" % (panes[at].name, pane, ancestor))
        x, y = x + tx, y + ty
        at = _u32(d, rec + 0x28)
    return (x, y)


class PackRun:
    """One pack run's view of the game's files, shared by every UI builder (title_menu.py, pause_menu.py): a file a
    builder made in this run is what the next one reads, so two builders that edit one archive (each language's
    messages) keep each other's work; and a file's size-table entry grows once, against the game's original: its
    size before the run, plus how much the file grew from the original, plus 0x1000.

    read(rel) and glob(pattern) are a builder's reader. A builder checks its growth (check) before it writes
    anything, then records its files and growth (commit). growth: [(rel, plain length read, plain length made,
    required)]; required: the table must have the entry (an archive), else it is grown when there."""

    def __init__(self, romfs, table):
        self.romfs = romfs
        self.table = table
        self.made = {}    # rel -> the compressed file a builder made in this run
        self._base = {}   # table key -> (its size before the run, the original's plain length)

    def read(self, rel):
        if rel in self.made:
            return self.made[rel]
        with open(os.path.join(self.romfs, *rel.split("/")), "rb") as f:
            return f.read()

    def glob(self, pattern):
        import glob as _glob
        n = len(self.romfs.rstrip("/\\")) + 1
        return sorted(p[n:].replace("\\", "/") for p in _glob.glob(os.path.join(self.romfs, *pattern.split("/"))))

    @staticmethod
    def key(rel):
        return rel[:-3] if rel.endswith(".zs") else rel

    def check(self, growth):
        for rel, _old, _new, required in growth:
            if required and self.table.get(self.key(rel)) is None:
                raise UiPackError("the size table has no %s" % self.key(rel))

    def commit(self, files, growth):
        for rel, _old, _new, required in growth:
            key = self.key(rel)
            size = self.table.get(key)
            if size is None and key not in self._base:
                continue
            base, original = self._base.setdefault(key, (size, _old))
            self.table.set(key, base + max(0, _new - original) + 0x1000)
        self.made.update(files)


def edit_entries(entries, name, edit):
    """An archive's entries ([(name, bytes)], from sarc.read_entries) with every copy of `name`
    replaced by edit(copy), and {old bytes: new bytes}. The engine's archives hold some files
    more than once (Title's and Common's layouts four times each); equal copies are edited once."""
    done = {}
    out = []
    for n, data in entries:
        if n == name:
            if data not in done:
                done[data] = edit(data)
            data = done[data]
        out.append((n, data))
    return out, done


# ---------------------------------------------------------------------------------------- MSBT
# The format is in msbt.py's notes. Here the sections are kept as raw bytes, so a file is written
# back exactly as it was read, with only the new labels and texts added.

_CODECS = {0: "utf-8", 1: "utf-16-le", 2: "utf-32-le"}
# Letters, digits, _ and -: a text pane's own label is its path through the parts, joined by -
# (PlusMenu_00.msbt's Pa_System_00-Pa_Button_00-T_Btn_00), and the layout fills the pane from it.
_LABEL = re.compile(r"[A-Za-z0-9_-]{1,255}")
_GROWN = {"LBL1", "TXT2", "ATR1", "NLI1"}   # the sections written back right with more texts


def label_hash(label, buckets):
    h = 0
    for c in label.encode("ascii"):
        h = (h * 0x492 + c) & 0xFFFFFFFF
    return h % buckets


def read_msbt(d):
    """(header bytes, [(magic, data)], {label: index}, [text bytes]) of a little-endian MSBT."""
    if d[:8] != b"MsgStdBn" or d[8:10] != b"\xff\xfe":
        raise UiPackError("not a little-endian MSBT")
    n = struct.unpack_from("<H", d, 0x0e)[0]
    at, sects = 0x20, []
    for _ in range(n):
        magic = d[at:at + 4]
        size = struct.unpack_from("<I", d, at + 4)[0]
        sects.append((magic, d[at + 16:at + 16 + size]))
        at = (at + 16 + size + 15) & ~15
    labels, texts = {}, []
    for magic, data in sects:
        if magic == b"LBL1":
            nb = struct.unpack_from("<I", data, 0)[0]
            for b in range(nb):
                cnt, off = struct.unpack_from("<II", data, 4 + 8 * b)
                for _ in range(cnt):
                    ln = data[off]
                    labels[data[off + 1:off + 1 + ln].decode("ascii")] = struct.unpack_from("<I", data, off + 1 + ln)[0]
                    off += 5 + ln
        elif magic == b"TXT2":
            nt = struct.unpack_from("<I", data, 0)[0]
            offs = list(struct.unpack_from("<%dI" % nt, data, 4)) + [len(data)]
            texts = [data[offs[i]:offs[i + 1]] for i in range(nt)]
    return d[:0x20], sects, labels, texts


def write_msbt(header, sects, labels, texts):
    """An MSBT from read_msbt's parts: LBL1 made again from `labels` (each in its hash bucket),
    TXT2 from `texts`, ATR1 grown with empty entries, other sections as they were."""
    out_sections = []
    for magic, data in sects:
        if magic == b"LBL1":
            nb = struct.unpack_from("<I", data, 0)[0]
            buckets = [[] for _ in range(nb)]
            for name, idx in labels.items():
                buckets[label_hash(name, nb)].append((name, idx))
            body, table = b"", b""
            first = 4 + 8 * nb
            for b in buckets:
                table += struct.pack("<II", len(b), first + len(body))
                for name, idx in b:
                    body += bytes([len(name)]) + name.encode("ascii") + struct.pack("<I", idx)
            data = struct.pack("<I", nb) + table + body
        elif magic == b"TXT2":
            head = 4 + 4 * len(texts)
            offs, at = [], head
            for t in texts:
                offs.append(at)
                at += len(t)
            data = struct.pack("<I", len(texts)) + struct.pack("<%dI" % len(texts), *offs) + b"".join(texts)
        elif magic == b"ATR1":
            n, size = struct.unpack_from("<II", data, 0)
            if size:
                data = struct.pack("<II", len(texts), size) + data[8:8 + n * size] + b"\0" * (size * (len(texts) - n))
        out_sections.append((magic, data))
    body = b""
    for magic, data in out_sections:
        sec = magic + struct.pack("<I", len(data)) + b"\0" * 8 + data
        while len(sec) % 16:
            sec += b"\xab"
        body += sec
    head = bytearray(header)
    struct.pack_into("<H", head, 0x0e, len(out_sections))
    struct.pack_into("<I", head, 0x12, 0x20 + len(body))
    return bytes(head) + body


def _check_text(label, text):
    if not isinstance(text, str) or not text:
        raise UiPackError("the text of %s is empty or not a string" % label)
    for ch in text:
        if (ord(ch) < 0x20 and ch != "\n") or ch == "\x7f":
            raise UiPackError("the text of %s has the control character %#x (only \\n is taken)" % (label, ord(ch)))


def add_labels(data, labels, replace=False):
    """A message file (MSBT bytes) with more labels and texts ({label: text}, our own words from
    load_strings). The file's own labels keep their indices and texts; new texts go after them,
    in the file's encoding. A label the file has already is refused, or with replace=True its
    text is replaced in place. The file is read with msbt.read first, so a damaged one is refused
    before anything is made."""
    try:
        m = msbt.read(data)
    except msbt.MsbtError as e:
        raise UiPackError("the message file cannot be read (%s)" % e) from e
    if m.big_endian:
        raise UiPackError("the message file is big-endian; only little-endian ones are written here")
    names = m.section_names()
    if "LBL1" not in names or "TXT2" not in names:
        raise UiPackError("the message file has no labels or texts")
    for s in names:
        if s not in _GROWN:
            raise UiPackError("the message file has a %s section, which is not grown here" % s)
    try:
        header, sects, have, texts = read_msbt(data)
    except UnicodeDecodeError as e:
        raise UiPackError("a label of the message file is not ASCII (%s)" % e) from e
    codec = _CODECS[header[0x0c]]
    have = dict(have)
    texts = list(texts)
    for label, text in labels.items():
        if not isinstance(label, str) or not _LABEL.fullmatch(label):
            raise UiPackError("%r is not a label (letters, digits, _ and -)" % (label,))
        _check_text(label, text)
        enc = (text + "\0").encode(codec)
        if label in have:
            if not replace:
                raise UiPackError("the message file has the label %s already" % label)
            texts[have[label]] = enc
        else:
            have[label] = len(texts)
            texts.append(enc)
    return write_msbt(header, sects, have, texts)


# ---------------------------------------------------------------------------------------- strings
# Our own words, one file per language in strings/ (UTF-8):
#
#   # a comment line
#   [LayoutMsg/MenuOverlay_00.msbt]       a message file of the game's language archives
#   CoopMenu = Multiplayer                 label = text, the label in the game's own style
#
# Spaces around the label and the text are dropped; a text may hold "=" and "#"; "\n" is a new
# line and "\\" a backslash. en.txt holds every label. A translation is named for its language
# (fr.txt) or for one of the game's language codes (USfr.txt, CNzh.txt) and may leave labels out:
# those keep their English text.

STRINGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "strings")
FALLBACK = "en"
_SECTION = re.compile(r"\[([^\[\]\s]+\.msbt)\]")


def _unescape(text, where):
    out, i = [], 0
    while i < len(text):
        ch = text[i]
        if ch == "\\":
            nxt = text[i + 1:i + 2]
            if nxt == "n":
                ch = "\n"
            elif nxt == "\\":
                ch = "\\"
            else:
                raise UiPackError("%s: \\%s is not \\n or \\\\" % (where, nxt))
            i += 1
        out.append(ch)
        i += 1
    return "".join(out)


def parse_strings(text, source="strings"):
    """{message file: {label: text}} of one strings file's text. Raises UiPackError naming the
    line of anything it cannot read."""
    out, current, file = {}, None, None
    for number, line in enumerate(text.splitlines(), 1):
        where = "%s line %d" % (source, number)
        line = line.lstrip("\ufeff").strip() if number == 1 else line.strip()
        if not line or line.startswith("#"):
            continue
        section = _SECTION.fullmatch(line)
        if section:
            file = section.group(1)
            if file in out:
                raise UiPackError("%s: [%s] is in the file already" % (where, file))
            current = out[file] = {}
            continue
        if "=" not in line:
            raise UiPackError("%s: not \"label = text\" nor \"[file.msbt]\"" % where)
        if current is None:
            raise UiPackError("%s: a label before any [file.msbt] line" % where)
        label, _, value = line.partition("=")
        label = label.strip()
        if not _LABEL.fullmatch(label):
            raise UiPackError("%s: %r is not a label (letters, digits, _ and -)" % (where, label))
        if label in current:
            raise UiPackError("%s: %s is in [%s] already" % (where, label, file))
        value = _unescape(value.strip(), where)
        try:
            _check_text(label, value)
        except UiPackError as e:
            raise UiPackError("%s: %s" % (where, e)) from e
        current[label] = value
    return out


def language_files(language):
    """The strings files read for one of the game's languages, English first: "USfr" gives
    en, fr, USfr; "fr" gives en, fr."""
    if not re.fullmatch(r"[A-Za-z]{2,8}", language or ""):
        raise UiPackError("%r is not a language code" % (language,))
    names = [FALLBACK]
    if len(language) == 4 and language[:2].isupper():   # the game's own code: region, language
        names.append(language[2:].lower())
    names.append(language)
    return list(dict.fromkeys(names))


def language_of(rel):
    """The game's language code of a message archive: "Mals/USfr.Product.110.sarc.zs" gives USfr."""
    return rel.replace("\\", "/").rsplit("/", 1)[-1].split(".", 1)[0]


def _read_strings(folder, name):
    path = os.path.join(folder, name + ".txt")
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            return parse_strings(f.read(), "strings/%s.txt" % name)
    except UnicodeDecodeError as e:
        raise UiPackError("strings/%s.txt is not UTF-8 (%s)" % (name, e)) from e


def load_strings(language, folder=STRINGS):
    """Our words for one of the game's languages ("USen", "EUfr", "CNzh", or a plain "fr"):
    {message file: {label: text}}. en.txt is read first, then each further file of
    language_files over it, label by label. A label only a translation has is refused (a typo)."""
    base = _read_strings(folder, FALLBACK)
    if base is None:
        raise UiPackError("no %s.txt in the strings folder" % FALLBACK)
    out = {f: dict(rows) for f, rows in base.items()}
    for name in language_files(language)[1:]:
        more = _read_strings(folder, name)
        for f, rows in (more or {}).items():
            for label, text in rows.items():
                if label not in base.get(f, {}):
                    raise UiPackError("strings/%s.txt: [%s] %s is not in %s.txt" % (name, f, label, FALLBACK))
                out[f][label] = text
    return out
