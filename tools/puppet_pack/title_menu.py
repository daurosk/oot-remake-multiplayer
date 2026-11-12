"""The title screen's co-op rows (docs/hookshot.md "The title menu"): Host, Join and Co-op
Settings as three more buttons on the game's own title list, under Continue, New Game and
Options, in the game's own look. Made from the player's own game files, as the puppets are.

Writes into the puppet mod's romfs:
  UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs   Title_00.buil with three more
      title button parts (Pa_Button_04..06, the game's PaCommonBtnTitle_00, under A_BtnAlign_00
      as the game's four)
  Mals/<language>.Product.110.sarc.zs   LayoutMsg/Title_00.msbt with the rows' texts (labels
      0100..0102), for every language the game has
and grows those files' entries in the resource size table.

The game's own title code makes a button for every Pa_Button_NN it finds and focuses them with
the pad; the co-op module (hookshot/src/coop_title.cpp) sets the three rows' texts from these
labels, and opens Host, Join or Co-op Settings when one is pressed.

The Layout2 resource (BFFH "Nintendo.Ui.ResModels.LayoutResData"), as far as read here:
  - absolute u64 offsets from the file's start; strings in the StringPool section;
  - the root (after "UIL" at the Default section's start) holds dictionaries of
    {keys* (names), values* (records), ids*, u32 count, u32 stride=8}: panes at +0x18, parts at +0xb8;
  - a pane record is 0x44 bytes: ... +0x20 translate y, +0x28 parent pane, +0x34 the index past
    its subtree (panes are in pre-order), +0x38 its kind, +0x42 u16 its index in its kind's
    table (parts: the part's number);
  - a part record is 0x58 bytes: the part layout's name, ..., +0x38 its properties list, +0x40
    its pane's index;
  - ids are 64-bit identities of the panes, also used as the parts' ids.
New panes go at the end of the pane order (the last pane is the alignment's last button), so
no existing index moves; the grown arrays and the new records go after the string pool, which
grows to hold them.
"""
import glob
import hashlib
import os
import struct

TITLE_ARCHIVE = "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs"
TITLE_LAYOUT = "Title_00.buil"
MSG_FILE = "LayoutMsg/Title_00.msbt"
MALS_GLOB = "Mals/*.Product.*.sarc.zs"
BUTTON = "Pa_Button_%02d"
FIRST_NEW = 4                 # Pa_Button_04: after the game's 00..03
ROWS = (("0100", "Host"), ("0101", "Join"), ("0102", "Co-op Settings"))
ROW_STEP_Y = -110.0           # the game's buttons are 110 apart

_ROOT = 0x100                 # the UIL root (Title_00: Default section 0xf8 + 8)
_PANES = 0x18                 # the root's pane dictionary
_PARTS = 0xb8                 # its part dictionary
PANE_SIZE = 0x44
PART_SIZE = 0x58


class TitleMenuError(Exception):
    pass


def _q(d, o):
    return struct.unpack_from("<Q", d, o)[0]


def _cstr(d, o):
    e = d.index(b"\0", o)
    return d[o:e].decode("ascii")


def _sections(d):
    """{name: (header offset, data offset, size)} of the BFSI sections."""
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


def _dict(d, at):
    keys, values, ids = _q(d, at), _q(d, at + 8), _q(d, at + 16)
    count, stride = struct.unpack_from("<II", d, at + 24)
    if stride != 8:
        raise TitleMenuError("a dictionary of stride %d at %#x" % (stride, at))
    return keys, values, ids, count


def read_layout(d):
    """The panes and parts of a Layout2 layout: ([(name, record offset, id)], [(name, record, id)])."""
    if d[:4] != b"BFFH" or b"Nintendo.Ui.ResModels.LayoutResData" not in d[:0x60]:
        raise TitleMenuError("not a Layout2 layout")
    if d[_ROOT - 8:_ROOT - 5] != b"UIL":
        raise TitleMenuError("no UIL root at %#x" % (_ROOT - 8))

    def entries(at):
        keys, values, ids, n = _dict(d, _ROOT + at)
        return [(_cstr(d, _q(d, keys + 8 * i)), _q(d, values + 8 * i), _q(d, ids + 8 * i)) for i in range(n)]

    return entries(_PANES), entries(_PARTS)


def _new_id(name):
    return int.from_bytes(hashlib.sha256(b"oot-coop title row " + name.encode()).digest()[:8], "little")


def add_buttons(d, count=len(ROWS)):
    """Title_00.buil with `count` more title buttons after the last one (Pa_Button_03): copies of it,
    ROW_STEP_Y further down each, under the same alignment pane."""
    d = bytes(d)
    panes, parts = read_layout(d)
    names = [p[0] for p in panes]
    last = BUTTON % (FIRST_NEW - 1)
    if last not in names or names[-1] != last:
        raise TitleMenuError("%s is not the layout's last pane" % last)
    if any(BUTTON % (FIRST_NEW + i) in names for i in range(count)):
        raise TitleMenuError("the layout has the rows already")
    li = names.index(last)
    last_rec = panes[li][1]
    parent = struct.unpack_from("<I", d, last_rec + 0x28)[0]
    old_n = len(panes)
    part_by_pane = {struct.unpack_from("<I", d, rec + 0x40)[0]: (n, rec, i) for n, rec, i in parts}
    if li not in part_by_pane:
        raise TitleMenuError("%s is not a part" % last)
    part_rec = part_by_pane[li][1]
    sects = _sections(d)
    if "StringPool" not in sects or "Default" not in sects:
        raise TitleMenuError("no Default or StringPool section")
    pool_hdr, pool_off, pool_size = sects["StringPool"]
    if pool_off + pool_size != len(d):
        raise TitleMenuError("the string pool is not the file's end")

    out = bytearray(d)

    def align(n=8):
        while len(out) % n:
            out.append(0)

    def put(b):
        align()
        at = len(out)
        out.extend(b)
        return at

    # the new names, after the pool's strings
    new_names = [BUTTON % (FIRST_NEW + i) for i in range(count)]
    name_at = []
    for n in new_names:
        name_at.append(len(out))
        out.extend(n.encode("ascii") + b"\0")
    # the new pane records: copies of the last button, further down, each its own subtree
    y0 = struct.unpack_from("<f", d, last_rec + 0x20)[0]
    part_no = struct.unpack_from("<H", d, last_rec + 0x42)[0]
    rec_at = []
    for i in range(count):
        r = bytearray(d[last_rec:last_rec + PANE_SIZE])
        struct.pack_into("<f", r, 0x20, y0 + ROW_STEP_Y * (i + 1))
        struct.pack_into("<I", r, 0x34, old_n + i + 1)
        struct.pack_into("<H", r, 0x42, part_no + i + 1)
        rec_at.append(put(bytes(r)))
    # every subtree that ended with the last button now ends after the new ones
    for _, rec, _ in panes:
        if rec != last_rec and struct.unpack_from("<I", out, rec + 0x34)[0] == old_n:
            struct.pack_into("<I", out, rec + 0x34, old_n + count)
    # the pane dictionary, grown
    ids = [_new_id(n) for n in new_names]
    q8 = lambda xs: b"".join(struct.pack("<Q", x) for x in xs)  # noqa: E731
    okeys, ovals, oids, _ = _dict(d, _ROOT + _PANES)
    keys = put(d[okeys:okeys + 8 * old_n] + q8(name_at))
    values = put(d[ovals:ovals + 8 * old_n] + q8(rec_at))
    idarr = put(d[oids:oids + 8 * old_n] + q8(ids))
    struct.pack_into("<QQQI", out, _ROOT + _PANES, keys, values, idarr, old_n + count)
    # the part records (the last button's, with its own empty properties list) and the part dictionary
    pkeys, pvals, pids, pn = _dict(d, _ROOT + _PARTS)
    props = _q(d, part_rec + 0x38)
    part_recs = []
    for i in range(count):
        p = bytearray(d[part_rec:part_rec + PART_SIZE])
        empty = put(bytes(d[props:props + 16]) if props else b"\0" * 16)
        struct.pack_into("<Q", p, 0x38, empty)
        struct.pack_into("<Q", p, 0x40, old_n + i)
        part_recs.append(put(bytes(p)))
    keys = put(d[pkeys:pkeys + 8 * pn] + q8(name_at))
    values = put(d[pvals:pvals + 8 * pn] + q8(part_recs))
    idarr = put(d[pids:pids + 8 * pn] + q8(ids))
    struct.pack_into("<QQQI", out, _ROOT + _PARTS, keys, values, idarr, pn + count)
    align()
    # the pool grows to the file's new end; the header's size too
    struct.pack_into("<I", out, pool_hdr + 8, len(out) - pool_off)
    struct.pack_into("<I", out, 0x08, len(out))
    return bytes(out)


# ---------------------------------------------------------------------------------------- MSBT

def _label_hash(label, buckets):
    h = 0
    for c in label.encode("ascii"):
        h = (h * 0x492 + c) & 0xFFFFFFFF
    return h % buckets


def read_msbt(d):
    """(header bytes, [(magic, data)], {label: index}, [text bytes]) of an MSBT."""
    if d[:8] != b"MsgStdBn" or d[8:10] != b"\xff\xfe":
        raise TitleMenuError("not a little-endian MSBT")
    n = struct.unpack_from("<H", d, 0x0e)[0]
    at, sections = 0x20, []
    for _ in range(n):
        magic = d[at:at + 4]
        size = struct.unpack_from("<I", d, at + 4)[0]
        sections.append((magic, d[at + 16:at + 16 + size]))
        at = (at + 16 + size + 15) & ~15
    labels, texts = {}, []
    for magic, data in sections:
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
    return d[:0x20], sections, labels, texts


def write_msbt(header, sections, labels, texts):
    out_sections = []
    for magic, data in sections:
        if magic == b"LBL1":
            nb = struct.unpack_from("<I", data, 0)[0]
            buckets = [[] for _ in range(nb)]
            for name, idx in labels.items():
                buckets[_label_hash(name, nb)].append((name, idx))
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


def add_rows(msbt, rows=ROWS):
    """Title_00.msbt with the rows' texts (label, text) added (UTF-8 or UTF-16 as the file is)."""
    header, sections, labels, texts = read_msbt(msbt)
    if not any(m == b"LBL1" for m, _ in sections) or not any(m == b"TXT2" for m, _ in sections):
        raise TitleMenuError("Title_00.msbt has no labels or texts")
    utf8 = header[0x0c] == 0
    labels = dict(labels)
    texts = list(texts)
    for label, text in rows:
        enc = (text + "\0").encode("utf-8" if utf8 else "utf-16-le")
        if label in labels:
            texts[labels[label]] = enc
        else:
            labels[label] = len(texts)
            texts.append(enc)
    return write_msbt(header, sections, labels, texts)


# ---------------------------------------------------------------------------------------- build

class _Dir:
    def __init__(self, root):
        self.root = root

    def read(self, rel):
        with open(os.path.join(self.root, *rel.split("/")), "rb") as f:
            return f.read()

    def glob(self, pattern):
        n = len(self.root.rstrip("/\\")) + 1
        return [p[n:].replace("\\", "/") for p in glob.glob(os.path.join(self.root, *pattern.split("/")))]


def build(romfs, out_romfs, dicts, table, write, recompress, log=print):
    """The title archive and every language's messages from the romfs folder, into out_romfs
    (write(out_romfs, rel, bytes)); their size table entries grown (`table`: rstb.Rstb). Returns
    the files written.

    All or nothing: every file and size is made in memory first; only when all of them were made
    (the layout and every language that has Title_00.msbt) is anything written or the table
    changed. A failure raises before that, so the pack never holds rows without their texts."""
    files, growth = make(_Dir(romfs), dicts, recompress)
    for rel, _, required in growth:
        if required and table.get(_key(rel)) is None:
            raise TitleMenuError("the size table has no %s" % _key(rel))
    for rel, by, required in growth:
        _grow(table, rel, by, required)
    for rel, data in files.items():
        write(out_romfs, rel, data)
    log("title menu: Host, Join and Co-op Settings on the title (%s, %d languages)" % (TITLE_LAYOUT, len(files) - 1))
    return list(files)


def make(reader, dicts, recompress):
    """({rel: compressed bytes}, [(rel, bytes grown, required)]) for the title archive and every
    language's messages; raises TitleMenuError (or the reader's error) when any one cannot be made."""
    from . import sarc
    files, growth = {}, []
    raw = reader.read(TITLE_ARCHIVE)
    entries = sarc.read_entries(dicts.decompress(raw))
    olds = [data for name, data in entries if name == TITLE_LAYOUT]
    if not olds:
        raise TitleMenuError("%s has no %s" % (TITLE_ARCHIVE, TITLE_LAYOUT))
    new = {}   # (every copy: the archive holds each layout more than once)
    entries = [(name, new.setdefault(data, add_buttons(data)) if name == TITLE_LAYOUT else data)
               for name, data in entries]
    archive = sarc.write(entries, data_align=16)   # (as the game's own: 16)
    growth.append((TITLE_ARCHIVE, len(archive) - len(dicts.decompress(raw)), True))
    growth.append((TITLE_LAYOUT, len(new[olds[0]]) - len(olds[0]), False))
    files[TITLE_ARCHIVE] = recompress(dicts, raw, archive)
    for rel in sorted(reader.glob(MALS_GLOB)):
        raw = reader.read(rel)
        plain = dicts.decompress(raw)
        msgs = sarc.read_entries(plain)
        if not any(name == MSG_FILE for name, _ in msgs):
            continue
        packed = sarc.write([(name, add_rows(data) if name == MSG_FILE else data) for name, data in msgs],
                            data_align=16)
        growth.append((rel, len(packed) - len(plain), True))
        files[rel] = recompress(dicts, raw, packed)
    if len(files) < 2:
        raise TitleMenuError("no language has %s" % MSG_FILE)
    return files, growth


def _key(rel):
    return rel[:-3] if rel.endswith(".zs") else rel


def _grow(table, rel, by, required=True):
    key = _key(rel)
    size = table.get(key)
    if size is None:
        if required:
            raise TitleMenuError("the size table has no %s" % key)
        return
    table.set(key, size + max(0, by) + 0x1000)
