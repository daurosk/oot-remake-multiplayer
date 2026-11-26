r"""Reads the game's message files (MSBT, Nintendo's LMS "MsgStdBn") and prints their texts.

A study tool (issue 264): it shows which label holds which text, and which control tags a text
uses, before co-op code looks a text up by its label (as title_menu.py does in Title_00.msbt).
The message files are in one archive per language, romfs/Mals/<language>.Product.110.sarc.zs
(zstd with the game's dictionaries from Pack/ZsDic.pack.zs). Run it on your own game files.
Nothing from the game is in this tool, and nothing it prints goes into the repository.

Usage:
    python -I tools/puppet_pack/msbt.py <file.msbt | <language>.Product.110.sarc.zs>
        [--romfs <romfs>] [--file LayoutMsg/Title_00.msbt] [--label NAME] [--list]

  Prints one line per label, "label<TAB>text", in the order the labels are in the file. Texts
  that no label names follow as "#<index><TAB>text". A summary goes to stderr.
  --list prints the .msbt files of an archive. Otherwise an archive needs --file.
  --romfs is the folder with Pack/ZsDic.pack.zs, needed for a .zs file. By default it is the
  folder above Mals, when the file is in one.

The format, as read here (numbers are in the file's byte order):
  header (0x20 bytes): "MsgStdBn", byte-order mark FF FE (little-endian) or FE FF (big-endian),
    u16 0, u8 encoding (0 UTF-8, 1 UTF-16, 2 UTF-32), u8 version, u16 section count, u16 0,
    u32 file size, padding.
  Then each section: 4-byte magic, u32 data size, 8 bytes padding, the data, padded to 16.
  LBL1: u32 bucket count, then per bucket u32 label count and u32 offset of its first label.
    A label is u8 length, the name, u32 text index. Offsets are from the section's data start.
    A label sits in bucket (hash % bucket count), hash = hash * 0x492 + byte over the name.
  TXT2: u32 text count, then u32 offset of each text (from the section's data start), then the
    texts. Each text ends with a 0 character.
  ATR1: u32 count, u32 entry size, then the entries. Kept as raw bytes here.
  Other sections (NLI1, TSY1, ...) are kept as raw data. Only their names are reported.

Inside a text, character 0x0E opens a control tag: u16 group, u16 type, u16 size of the
parameters in bytes, then the parameters. Character 0x0F closes a tag: u16 group, u16 type.
The tag fields are u16 in every encoding. render() writes a text on one line:
  {G.T}        a tag of group G and type T (decimal), without parameters
  {G.T:hex}    a tag with parameters: their bytes in hex, as stored
  {/G.T}       a closing tag
  \\ \{ \n \t \r            a backslash, a brace, a newline, a tab, a carriage return
  \xNN \uNNNN \UNNNNNNNN   any other character that does not print (control characters,
                           private use characters, spaces other than the plain space)
"""
import argparse
import os
import struct
import sys
from dataclasses import dataclass, field
from typing import NamedTuple, Optional

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import sarc, zs  # noqa: E402

MAGIC = b"MsgStdBn"
HEADER_SIZE = 0x20
ENCODINGS = {0: "utf-8", 1: "utf-16", 2: "utf-32"}
_UNIT = {"utf-8": 1, "utf-16": 2, "utf-32": 4}
_UNIT_FORMAT = {1: "B", 2: "H", 4: "I"}
TAG_OPEN = 0x0E
TAG_CLOSE = 0x0F


class MsbtError(ValueError):
    pass


class Tag(NamedTuple):
    """A control tag (character 0x0E): its group, its type and its parameters' bytes."""
    group: int
    type: int
    params: bytes = b""


class TagEnd(NamedTuple):
    """A closing tag (character 0x0F)."""
    group: int
    type: int


class Label(NamedTuple):
    name: str
    index: int


@dataclass
class Msbt:
    """One message file. A text is a list of items: str (characters), Tag and TagEnd."""
    big_endian: bool
    encoding: str                       # "utf-8", "utf-16" or "utf-32"
    version: int
    sections: list                      # [(magic, data)] in file order
    buckets: int = 0                    # LBL1's bucket count
    labels: list = field(default_factory=list)   # [Label] in file order
    texts: list = field(default_factory=list)    # [[item]] by text index
    attributes: Optional[list] = None   # ATR1's entries (bytes) by text index; None without ATR1

    def section_names(self):
        return [magic for magic, _ in self.sections]

    def text(self, label):
        """The text of a label. KeyError when the file has no such label."""
        for lab in self.labels:
            if lab.name == label:
                return self.texts[lab.index]
        raise KeyError(label)

    def rows(self):
        """(label, text) in the file's label order, then (#index, text) for unnamed texts."""
        named = set()
        for lab in self.labels:
            named.add(lab.index)
            yield lab.name, self.texts[lab.index]
        for i, text in enumerate(self.texts):
            if i not in named:
                yield "#%d" % i, text


def read(data):
    """The Msbt of a file's bytes. Raises MsbtError when the file cannot be read."""
    d = bytes(data)
    if len(d) < HEADER_SIZE or d[:8] != MAGIC:
        raise MsbtError("not an MSBT (no MsgStdBn header)")
    if d[8:10] == b"\xff\xfe":
        e = "<"
    elif d[8:10] == b"\xfe\xff":
        e = ">"
    else:
        raise MsbtError("unknown byte-order mark %s" % d[8:10].hex())
    if d[0x0C] not in ENCODINGS:
        raise MsbtError("unknown encoding %d" % d[0x0C])
    m = Msbt(big_endian=e == ">", encoding=ENCODINGS[d[0x0C]], version=d[0x0D], sections=[])
    count, = struct.unpack_from(e + "H", d, 0x0E)
    at = HEADER_SIZE
    for i in range(count):
        if at + 16 > len(d):
            raise MsbtError("section %d starts past the file's end" % i)
        magic = d[at:at + 4].decode("ascii", "backslashreplace")
        size, = struct.unpack_from(e + "I", d, at + 4)
        if at + 16 + size > len(d):
            raise MsbtError("section %s runs past the file's end" % magic)
        m.sections.append((magic, d[at + 16:at + 16 + size]))
        at = (at + 16 + size + 15) & ~15
    for magic, sec in m.sections:
        try:
            if magic == "LBL1":
                m.buckets, m.labels = _labels(sec, e)
            elif magic == "TXT2":
                m.texts = _texts(sec, e, m.encoding)
            elif magic == "ATR1":
                m.attributes = _attributes(sec, e)
        except (struct.error, IndexError) as ex:
            raise MsbtError("section %s is cut short (%s)" % (magic, ex)) from ex
    for lab in m.labels:
        if lab.index >= len(m.texts):
            raise MsbtError("label %s names text %d of %d" % (lab.name, lab.index, len(m.texts)))
    return m


def _labels(sec, e):
    """(bucket count, [Label] in file order) of an LBL1 section."""
    buckets, = struct.unpack_from(e + "I", sec, 0)
    found = []
    for b in range(buckets):
        count, off = struct.unpack_from(e + "II", sec, 4 + 8 * b)
        for _ in range(count):
            n = sec[off]
            name = sec[off + 1:off + 1 + n].decode("ascii", "backslashreplace")
            index, = struct.unpack_from(e + "I", sec, off + 1 + n)
            found.append((off, Label(name, index)))
            off += 5 + n
    found.sort(key=lambda f: f[0])   # buckets are usually in file order already
    return buckets, [lab for _, lab in found]


def _texts(sec, e, encoding):
    """The texts of a TXT2 section, each a list of items."""
    count, = struct.unpack_from(e + "I", sec, 0)
    offsets = struct.unpack_from(e + "%dI" % count, sec, 4)
    out = []
    for i, off in enumerate(offsets):
        if off > len(sec):
            raise MsbtError("text %d starts past its section's end" % i)
        out.append(_items(sec, off, e, encoding))
    return out


def _items(buf, at, e, encoding):
    """The items of one text, from `at` to its 0 character (or the buffer's end)."""
    unit = _UNIT[encoding]
    char = e + _UNIT_FORMAT[unit]
    codec = encoding if unit == 1 else encoding + ("-be" if e == ">" else "-le")
    errors = "surrogateescape" if unit == 1 else "surrogatepass"
    items, run, end = [], bytearray(), len(buf)

    def flush():
        if run:
            try:
                items.append(run.decode(codec, errors))
            except UnicodeDecodeError as ex:
                raise MsbtError("a text does not decode as %s (%s)" % (encoding, ex)) from ex
            run.clear()

    while at + unit <= end:
        c, = struct.unpack_from(char, buf, at)
        if c == 0:
            break
        if c == TAG_OPEN:
            p = at + unit + 6
            if p > end:
                raise MsbtError("a tag at %#x is cut short" % at)
            group, kind, size = struct.unpack_from(e + "HHH", buf, at + unit)
            if p + size > end:
                raise MsbtError("the parameters of tag %d.%d at %#x are cut short" % (group, kind, at))
            flush()
            items.append(Tag(group, kind, bytes(buf[p:p + size])))
            at = p + size
        elif c == TAG_CLOSE:
            if at + unit + 4 > end:
                raise MsbtError("a closing tag at %#x is cut short" % at)
            group, kind = struct.unpack_from(e + "HH", buf, at + unit)
            flush()
            items.append(TagEnd(group, kind))
            at += unit + 4
        else:
            run += buf[at:at + unit]
            at += unit
    flush()
    return items


def _attributes(sec, e):
    """ATR1's entries as raw bytes."""
    count, size = struct.unpack_from(e + "II", sec, 0)
    if 8 + count * size > len(sec):
        raise MsbtError("ATR1 holds %d entries of %d bytes, more than its size" % (count, size))
    return [bytes(sec[8 + i * size:8 + (i + 1) * size]) for i in range(count)]


_NAMED = {"\\": "\\\\", "{": "\\{", "\n": "\\n", "\t": "\\t", "\r": "\\r"}


def _char(c):
    if c in _NAMED:
        return _NAMED[c]
    if c.isprintable():
        return c
    o = ord(c)
    if o < 0x100:
        return "\\x%02x" % o
    return "\\u%04x" % o if o < 0x10000 else "\\U%08x" % o


def render(text):
    """A text (a list of items, or a str) as one readable line, in the form the module's
    notes give."""
    if isinstance(text, str):
        text = [text]
    out = []
    for item in text:
        if isinstance(item, Tag):
            if item.params:
                out.append("{%d.%d:%s}" % (item.group, item.type, item.params.hex()))
            else:
                out.append("{%d.%d}" % (item.group, item.type))
        elif isinstance(item, TagEnd):
            out.append("{/%d.%d}" % (item.group, item.type))
        else:
            out.append("".join(_char(c) for c in item))
    return "".join(out)


def tag_counts(m):
    """{(group, type): how many times} over every opening tag of the file's texts."""
    counts = {}
    for text in m.texts:
        for item in text:
            if isinstance(item, Tag):
                counts[(item.group, item.type)] = counts.get((item.group, item.type), 0) + 1
    return counts


# ---------------------------------------------------------------------------------------- CLI

def _romfs_of(path):
    """The romfs folder of a file in <romfs>/Mals, when it has the dictionaries; else None."""
    folder = os.path.dirname(os.path.abspath(path))
    if os.path.basename(folder).lower() != "mals":
        return None
    romfs = os.path.dirname(folder)
    return romfs if os.path.isfile(os.path.join(romfs, "Pack", "ZsDic.pack.zs")) else None


def _unpack(data, path, romfs):
    if data[:4] != zs.ZSTD_MAGIC:
        return data
    romfs = romfs or _romfs_of(path)
    if not romfs:
        raise MsbtError("a .zs file needs --romfs (the folder with Pack/ZsDic.pack.zs)")
    return zs.maybe_decompress(zs.Dicts.from_romfs(romfs), data)


def _stdout():
    # Game texts hold characters a Windows code page lacks: write UTF-8.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    return sys.stdout


def main(argv=None, out=None, err=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("path", help="a .msbt file, or a message archive (.sarc.zs, or a plain .sarc)")
    ap.add_argument("--romfs", help="the romfs folder, for the dictionaries in Pack/ZsDic.pack.zs "
                                    "(default: the folder above Mals, when the file is in one)")
    ap.add_argument("--file", help="the .msbt in the archive, as LayoutMsg/Title_00.msbt")
    ap.add_argument("--label", help="print only this label's text")
    ap.add_argument("--list", action="store_true", help="list the .msbt files in the archive")
    args = ap.parse_args(argv)
    out = _stdout() if out is None else out
    err = sys.stderr if err is None else err
    try:
        with open(args.path, "rb") as f:
            data = _unpack(f.read(), args.path, args.romfs)
        name = os.path.basename(args.path)
        if data[:4] == b"SARC":
            files = sarc.read(data)
            msbts = [n for n in files if n.endswith(".msbt")]
            if args.list:
                for n in msbts:
                    print(n, file=out)
                print("%s: %d .msbt files, %d files in all" % (name, len(msbts), len(files)), file=err)
                return 0
            if not args.file:
                raise MsbtError("%s is an archive: give --file (--list shows its files)" % name)
            if args.file not in files:
                raise MsbtError("%s has no %s (--list shows its files)" % (name, args.file))
            data, name = files[args.file], args.file
        elif args.list or args.file:
            raise MsbtError("%s is not an archive: --list and --file are for archives" % name)
        m = read(data)
        found = False
        for label, text in m.rows():
            if args.label is None or label == args.label:
                print("%s\t%s" % (label, render(text)), file=out)
                found = True
        if args.label is not None and not found:
            raise MsbtError("%s has no label %s" % (name, args.label))
        tags = " ".join("%d.%d x%d" % (g, t, n) for (g, t), n in sorted(tag_counts(m).items()))
        print("%s: %d labels, %d texts, %s %s-endian, sections %s%s"
              % (name, len(m.labels), len(m.texts), m.encoding, "big" if m.big_endian else "little",
                 " ".join(m.section_names()), ", tags " + tags if tags else ""), file=err)
        return 0
    except (ValueError, OSError) as ex:   # MsbtError, SarcError, a missing dictionary or file
        print("msbt: %s" % ex, file=err)
        return 1


if __name__ == "__main__":
    sys.exit(main())
