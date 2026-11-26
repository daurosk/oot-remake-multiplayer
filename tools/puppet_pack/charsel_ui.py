"""Co-op's own Characters tab, Link only (the owner, 2026-11-22: "our mod itself should natively include the character
tab even without downloading any of Snails mod").

The - menu's four-tab header (Characters | Key Items | Threads of Time | Map), the Characters page with Link, its picker
and the Customize screen are Snail's (OoT Character Select v0.2.1, MIT, used with his permission): his code, run for
Link alone by tools/puppet_pack/charsel/make_data.py, made the files; what this repository keeps of them is a delta
per file against the game's own file (charsel/data: copy-from-the-player's-file and insert operations, none of the
game's bytes), and the code patch (charsel/data/charsel-native.ips). At pack time the deltas are applied to the
player's own files, each checked by its source's and its result's SHA-1, through the pack run's shared view
(ui_pack.PackRun) before the title rows and the pause menu, so their tabs go onto the four-tab header.

With the Character Select mod in the mods (CHARSEL_MOD with a code patch), his files and his code patch are the only
ones: the pack makes none of this (build.py), and the kit leaves charsel-native.ips out of the game's build.

Delta format (CDLT1): magic, u32 source length, 20-byte source SHA-1, u32 result length, 20-byte result SHA-1, then
ops until 0x00: 0x01 <varint offset> <varint length> copies from the source, 0x02 <varint length> <bytes> inserts.
"""
import hashlib
import json
import os

from . import ui_pack

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "charsel", "data")
MANIFEST = os.path.join(DATA, "manifest.json")
CODE_PATCH = os.path.join(DATA, "charsel-native.ips")
BUILD_ID = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348"
CHARSEL_MOD = "OoT Character Select"
MAGIC = b"CDLT1\0"
# The data's own hash (data_hash(): every file of charsel/data), kept here so that a change of the data alone changes
# this file and with it the tool's hash (build.py tool_hash, *.py only): packs made with other data are made again
# (the kit's Test-KitPuppetPackStale, the Linux kit's pack stamp). test_charsel_ui checks it is the data's.
DATA_SHA256 = "f7012683ca4b0cc414434f849bea654eff14e81f80512184308235ffebd018d6"


def data_hash(data_dir=DATA):
    """sha256 over "<name>:<sha256 of the file>\\n" of each file in data_dir, names in byte order."""
    import hashlib
    names = sorted((n for n in os.listdir(data_dir) if os.path.isfile(os.path.join(data_dir, n))),
                   key=lambda n: n.encode("utf-8"))
    text = ""
    for n in names:
        with open(os.path.join(data_dir, n), "rb") as f:
            text += "%s:%s\n" % (n, hashlib.sha256(f.read()).hexdigest())
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


class CharselError(ValueError):
    pass


def _varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _read_varint(d, i):
    n = shift = 0
    while True:
        if i >= len(d):
            raise CharselError("delta ends inside a number")
        b = d[i]
        i += 1
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return n, i


def make_delta(src, dst, block=16):
    """A CDLT1 delta that makes `dst` from `src` (greedy: the first source place of each block of `block` bytes)."""
    index = {}
    for i in range(0, len(src) - block + 1, 4):
        index.setdefault(src[i:i + block], i)
    out = bytearray(MAGIC)
    out += len(src).to_bytes(4, "little") + hashlib.sha1(src).digest()
    out += len(dst).to_bytes(4, "little") + hashlib.sha1(dst).digest()
    lit = bytearray()
    i = 0
    while i < len(dst):
        j = index.get(dst[i:i + block])
        if j is None:
            lit.append(dst[i])
            i += 1
            continue
        n = block
        while i + n < len(dst) and j + n < len(src) and dst[i + n] == src[j + n]:
            n += 1
        if lit:
            out += b"\x02" + _varint(len(lit)) + lit
            lit = bytearray()
        out += b"\x01" + _varint(j) + _varint(n)
        i += n
    if lit:
        out += b"\x02" + _varint(len(lit)) + lit
    return bytes(out + b"\x00")


def apply_delta(src, delta):
    """The result of a CDLT1 delta on `src`; CharselError when `src` is not the file it was made from, or the result
    is not the file it made."""
    if delta[:len(MAGIC)] != MAGIC:
        raise CharselError("not a CDLT1 delta")
    i = len(MAGIC)
    slen = int.from_bytes(delta[i:i + 4], "little")
    ssha = delta[i + 4:i + 24]
    dlen = int.from_bytes(delta[i + 24:i + 28], "little")
    dsha = delta[i + 28:i + 48]
    i += 48
    if len(src) != slen or hashlib.sha1(src).digest() != ssha:
        raise CharselError("the game's file is not the one the delta was made from (another game version?)")
    out = bytearray()
    while True:
        if i >= len(delta):
            raise CharselError("delta has no end")
        op = delta[i]
        i += 1
        if op == 0:
            break
        if op == 1:
            off, i = _read_varint(delta, i)
            n, i = _read_varint(delta, i)
            if off + n > len(src):
                raise CharselError("delta copies past the source's end")
            out += src[off:off + n]
        elif op == 2:
            n, i = _read_varint(delta, i)
            out += delta[i:i + n]
            i += n
        else:
            raise CharselError("unknown delta op %d" % op)
    if len(out) != dlen or hashlib.sha1(out).digest() != dsha:
        raise CharselError("delta result differs from the file it was made for")
    return bytes(out)


def load_manifest(path=MANIFEST):
    with open(path, encoding="utf-8") as f:
        m = json.load(f)
    if m.get("format") != "coop-charsel 1":
        raise CharselError("charsel manifest: unknown format")
    return m


def _delta(name, data_dir):
    with open(os.path.join(data_dir, name), "rb") as f:
        return f.read()


def make(reader, dicts, recompress, manifest=None, data_dir=DATA, strings=ui_pack.STRINGS):
    """({rel: file bytes as the game reads them}, growth, [new size-table keys and sizes]) for every file of the
    manifest; raises CharselError (or the reader's error) when any one cannot be made.

    Manifest entries: {"rel", "zs": bool, "from": source rel (plain file) or null (new file), "delta"} or, for SARC
    archives, {"rel" or "glob", "zs", "align", "members": [{"name", "from": source member or null, "delta",
    "copies": how many entries a new member gets}], "labels": {member: {label: text}}}. recompress(dicts, raw, plain)
    as build.py's."""
    from . import sarc
    manifest = manifest or load_manifest()
    files, growth, new = {}, [], []
    for e in manifest["files"]:
        rels = reader.glob(e["glob"]) if "glob" in e else [e["rel"]]
        if not rels:
            raise CharselError("no game file for %s" % e.get("glob"))
        for rel in rels:
            if "members" not in e:
                if e["zs"] and e.get("from") != rel:
                    raise CharselError("%s: a compressed file is made only from the game's own copy" % rel)
                raw = reader.read(e["from"]) if e.get("from") else b""
                src = dicts.decompress(raw) if e["zs"] else raw
                out = apply_delta(src, _delta(e["delta"], data_dir))
                files[rel] = recompress(dicts, raw, out) if e["zs"] else out
                if e.get("from") == rel:
                    growth.append((rel, len(src), len(out), False))
                else:
                    new.append((ui_pack.PackRun.key(rel), len(out)))
                continue
            raw = reader.read(rel)
            plain = dicts.decompress(raw) if e["zs"] else raw
            entries = sarc.read_entries(plain)
            have = dict(entries)
            out = list(entries)
            for m in e["members"]:
                if m["from"] is not None and m["from"] not in have:
                    raise CharselError("%s has no %s" % (rel, m["from"]))
                data = apply_delta(have[m["from"]] if m["from"] is not None else b"", _delta(m["delta"], data_dir))
                if m["name"] in have:
                    out = [(n, data if n == m["name"] else d) for n, d in out]
                else:
                    out += [(m["name"], data)] * m.get("copies", 1)   # (layouts are stored four times, as the game's)
            for member, labels in e.get("labels", {}).items():
                out = [(n, ui_pack.add_labels(d, labels) if n == member else d) for n, d in out]
            if e.get("strings"):   # (our words over his English, in the archive's language: strings/*.txt)
                ours = ui_pack.load_strings(ui_pack.language_of(rel), strings)
                for member in e["strings"]:
                    if ours.get(member):
                        out = [(n, ui_pack.add_labels(d, ours[member], replace=True) if n == member else d)
                               for n, d in out]
            made = sarc.write(out, data_align=e.get("align", 16))
            files[rel] = recompress(dicts, raw, made) if e["zs"] else made
            growth.append((rel, len(plain), len(made), True))
    return files, growth, new


def build(romfs, out_romfs, dicts, table, write, recompress, log=print, run=None, manifest=None, data_dir=DATA):
    """The files into out_romfs (write(out_romfs, rel, bytes)), their size-table entries grown or added, and the code
    patch into the pack's exefs; all or nothing. Returns the files written."""
    run = run or ui_pack.PackRun(romfs, table)
    files, growth, new = make(run, dicts, recompress, manifest, data_dir)
    run.check(growth)
    run.commit(files, growth)
    for key, size in new:
        if table.get(key) is None:
            table.add_new(key, size + 0x1000)
    for rel, data in files.items():
        write(out_romfs, rel, data)
    log("characters tab: Link's page, picker and Customize (%d files)" % len(files))
    return list(files)


def sources(manifest=None):
    """The game's files the manifest's plain and compressed entries are made from (the archives by name and the
    language archives by glob are the pause menu's own: build.py nup_menu_files)."""
    manifest = manifest or load_manifest()
    out = []
    for e in manifest["files"]:
        for rel in (e.get("from"), e.get("rel")) if "members" not in e else (e.get("rel"),):
            if rel and rel not in out:
                out.append(rel)
    return out


def remove_code_patch(mod_dir):
    """The code patch out of <mod_dir>/exefs (the tab left out: no code that opens screens the game lacks)."""
    try:
        os.remove(os.path.join(mod_dir, "exefs", code_patch_name()))
    except OSError:
        pass


def code_patch_name():
    return BUILD_ID + ".ips"


def write_code_patch(mod_dir, src=CODE_PATCH):
    """charsel-native.ips as <mod_dir>/exefs/<build id>.ips (the bytes the kit puts there before the game's build)."""
    with open(src, "rb") as f:
        data = f.read()
    os.makedirs(os.path.join(mod_dir, "exefs"), exist_ok=True)
    with open(os.path.join(mod_dir, "exefs", code_patch_name()), "wb") as f:
        f.write(data)
