"""Reads and sets flags in a saved game (slot_NN/progress.sav), offline.

For the autotest kits' save baselines (<kit>/autotest-save/...), so a test can
start from a save that already holds what it needs, as a player's own save
would (issue 125: bombs given by the objects cheat after the save loaded were
deleted 0.1 s after every pull; a save that has them when it loads is the
next thing to try).

The save layout (all little-endian): magic 0x01020304, version, pool offset,
then from 0x20 a table of 8-byte records. A record whose first word is 0
opens a type (second word: the type id, 0 to 0x20 in order); any other first
word is a flag hash, and its second word is the value for the inline types
(Bool 0, Int 2, Enum 6, UInt 20). There is no checksum. Only values already
in the table are changed; nothing is added or moved.

  save_flags.py list AUTOTEST_SAVE        each user's slots and their bombs
  save_flags.py get SAVE int:0xdb1bcd4f [bool:0x2bb00768 ...]
  save_flags.py set SAVE int:0xdb1bcd4f=20 [bool:0x2bb00768=1 ...]
  save_flags.py bombs SAVE [SAVE ...]     what a real small bomb bag sets

SAVE may be a pattern (slot_0*/progress.sav); it is expanded here, as the
Windows shells do not. Only saves in a kit's baseline are touched: a path
must be <...>/autotest-save/<user>/<title>/slot_NN/progress.sav, and nothing
under %APPDATA% or %LOCALAPPDATA% (a player's own saves, the emulator's
NAND) is ever read or written. Before a change the file is copied to
progress.sav.bak-<time>, and the new bytes go to a temp file that then
replaces it. Close the game first: it writes its saves on every scene change.
"""
import argparse
import glob
import os
import re
import shutil
import struct
import sys
import time

MAGIC = 0x01020304
TABLE = 0x20
TYPES = {"bool": 0, "int": 2, "enum": 6, "uint": 20}
END = 0x20

# What a save of a player who got the small bomb bag in the game holds that a
# save without it does not (docs/items.md, "Bombs from the cheat"). The get
# order is set to the next number after the highest of GET_ORDERS.
BOMBS = [
    ("int", 0xdb1bcd4f, 20),  # PouchData.Item_Bomb.StockNum
    ("int", 0xb0a1bf51, 1),   # PouchData.Item_BombBag_Small.StockNum
    ("int", 0xe753508b, 20),  # capacity Bomb (struct 0xf80f8077)
    ("bool", 0x2bb00768, 1),  # IsGet.Item_Bomb
    ("bool", 0xa63f1e2d, 1),  # IsGet.Item_BombBag_Small
    ("bool", 0x6e5e7d9a, 1),  # 0x541947d0.Bomb
    ("bool", 0xfced7932, 1),  # 0x57f52eb8.Bomb
    ("bool", 0x4d345bbe, 1),  # 0x41f3e410.Item_BombBag_Small
    ("bool", 0xa795bc63, 1),  # 0x41f3e410.Item_Bomb
]
BOMB_GET_ORDER = 0xc7ecb4a1  # UInt, struct 0xf5ab6461, member Item_Bomb
GET_ORDERS = [
    0x117375f7, 0x22c20fa9, 0x3b9434e0, 0x3e70dbc1, 0x4301162b, 0x438c1f7b, 0x4ad60f93,
    0x549ee15c, 0x55f6470d, 0x56573d03, 0x58c854bc, 0x642ffd7f, 0x6a1e2459, 0x73666cdf,
    0x7c7c2880, 0x83b3eaa3, 0x956aed56, 0x961a120b, 0x99b98ae4, 0x9a5bef3e, 0xa6c38c34,
    0xaab858a4, 0xad3ff9c8, 0xaeeb7554, 0xaf5154ab, 0xc7025e0a, 0xc7ecb4a1, 0xc98ed673,
    0xcba88c4f, 0xcbab442d, 0xcfe1a634, 0xdb070fb9, 0xdd3baa0b, 0xe66b92b8, 0xe7ee66d6,
    0xe97874a7, 0xe9a087b5, 0xecffd2a6, 0xf038e577, 0xf2e7d892, 0xf2ffd542, 0xf78f00bc,
]


class SaveError(ValueError):
    pass


def index(data):
    """{(type id, hash): offset of the value word} for the inline entries.
    Checks the whole table: the type openers 0 to 0x1f in order, none
    missing, then the end record and its pair, all before the pool."""
    if len(data) < TABLE or struct.unpack_from("<I", data, 0)[0] != MAGIC:
        raise SaveError("not a saved game (magic)")
    pool = struct.unpack_from("<I", data, 8)[0]
    if not TABLE <= pool <= len(data):
        raise SaveError("bad pool offset")
    out = {}
    kind = None
    at = TABLE
    while at + 8 <= pool:
        first, second = struct.unpack_from("<II", data, at)
        if first == 0:
            expected = 0 if kind is None else kind + 1
            if second != expected:
                raise SaveError(f"type {second:#x} where {expected:#x} was due")
            if second == END:
                if at + 16 > pool:
                    raise SaveError("the end record's pair runs into the pool")
                return out
            kind = second
        elif kind is None:
            raise SaveError("an entry before the first type")
        elif kind in TYPES.values():
            if (kind, first) in out:
                raise SaveError(f"{first:#x} twice in type {kind}")
            out[(kind, first)] = at + 4
        at += 8
    raise SaveError("no end record before the pool")


SLOT = re.compile(r"slot_\d\d$", re.IGNORECASE)


def _forbidden_roots():
    roots = []
    for name in ("APPDATA", "LOCALAPPDATA"):
        value = os.environ.get(name)
        if value:
            roots.append(os.path.normcase(os.path.realpath(value)))
    return roots


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def check_path(path):
    """The resolved path of a baseline save, or SaveError."""
    real = os.path.realpath(path)
    norm = os.path.normcase(real)
    for root in _forbidden_roots():
        if norm == root or norm.startswith(root.rstrip(os.sep) + os.sep):
            raise SaveError(f"{path}: under %APPDATA% or %LOCALAPPDATA%, never touched")
    parts = os.path.normpath(real).split(os.sep)
    if (len(parts) < 5 or parts[-1].lower() != "progress.sav" or not SLOT.match(parts[-2])
            or parts[-5].lower() != "autotest-save"):
        raise SaveError(f"{path}: not <kit>/autotest-save/<user>/<title>/slot_NN/progress.sav")
    return real


def expand(patterns):
    """Glob patterns to checked save paths; an unmatched pattern is an error."""
    out = []
    for pattern in patterns:
        hits = sorted(glob.glob(pattern)) if glob.has_magic(pattern) else [pattern]
        if not hits:
            raise SaveError(f"{pattern}: matches no file")
        out.extend(check_path(h) for h in hits)
    return out


def write_save(path, old, new):
    """Backs `old` up next to the file, then replaces it with `new` through a
    temp file. Refuses when the file changed since it was read."""
    index(new)  # the changed bytes are still a whole save
    with open(path, "rb") as f:
        if f.read() != old:
            raise SaveError(f"{path}: changed since it was read (is the game running?)")
    stem = f"{path}.bak-{time.strftime('%Y%m%d-%H%M%S')}"
    backup, n = stem, 0
    while os.path.exists(backup):
        n += 1
        backup = f"{stem}-{n}"
    shutil.copyfile(path, backup)
    with open(backup, "rb") as f:
        if f.read() != old:
            raise SaveError(f"{path}: backup {backup} does not match; nothing written")
    tmp = f"{path}.tmp-{os.getpid()}"
    try:
        with open(tmp, "wb") as f:
            f.write(new)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return backup


def parse_flag(text):
    kind, _, rest = text.partition(":")
    if kind not in TYPES:
        raise SaveError(f"type {kind!r}: use one of {', '.join(TYPES)}")
    name, eq, value = rest.partition("=")
    return TYPES[kind], int(name, 0), (int(value, 0) if eq else None)


def get(data, kind, h):
    at = index(data).get((kind, h))
    return None if at is None else struct.unpack_from("<I", data, at)[0]


def set_values(data, changes):
    """Sets (type id, hash, value) in a bytearray. Returns [(hash, old, new)].
    Every flag must already be in the save; nothing is changed otherwise."""
    idx = index(data)
    missing = [hex(h) for kind, h, _ in changes if (kind, h) not in idx]
    if missing:
        raise SaveError("not in this save: " + ", ".join(missing))
    done = []
    for kind, h, value in changes:
        at = idx[(kind, h)]
        old = struct.unpack_from("<I", data, at)[0]
        if kind == TYPES["bool"]:
            value = 1 if value else 0
            new = (old & ~0xff) | value
        else:
            new = value & 0xffffffff
        struct.pack_into("<I", data, at, new)
        done.append((h, old, new))
    return done


def bomb_changes(data):
    idx = index(data)
    orders = [struct.unpack_from("<I", data, idx[(TYPES["uint"], h)])[0]
              for h in GET_ORDERS if (TYPES["uint"], h) in idx]
    changes = []
    for kind, h, value in BOMBS:
        at = idx.get((TYPES[kind], h))
        if at is not None and struct.unpack_from("<I", data, at)[0] >= value:
            continue  # already there (a real bomb save): never lowered
        changes.append((TYPES[kind], h, value))
    if get(data, TYPES["uint"], BOMB_GET_ORDER) == 0:
        changes.append((TYPES["uint"], BOMB_GET_ORDER, max(orders, default=0) + 1))
    return changes


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    ls = sub.add_parser("list")
    ls.add_argument("autotest_save")
    g = sub.add_parser("get")
    g.add_argument("save")
    g.add_argument("flags", nargs="+")
    s = sub.add_parser("set")
    s.add_argument("save")
    s.add_argument("flags", nargs="+")
    b = sub.add_parser("bombs")
    b.add_argument("saves", nargs="+")
    a = p.parse_args(argv)
    try:
        if a.cmd == "list":
            pattern = os.path.join(a.autotest_save, "*", "*", "slot_*", "progress.sav")
            for path in expand([pattern]):
                data = read_file(path)
                print(f"{path}: bombs {get(data, 2, 0xdb1bcd4f)}, "
                      f"bomb bag {get(data, 2, 0xb0a1bf51)}")
            return 0
        if a.cmd == "get":
            for path in expand([a.save]):
                data = read_file(path)
                for f in a.flags:
                    kind, h, _ = parse_flag(f)
                    v = get(data, kind, h)
                    print(f"{path}: {f} = {'not in this save' if v is None else v}")
            return 0
        changes = []
        if a.cmd == "set":
            for f in a.flags:
                kind, h, value = parse_flag(f)
                if value is None:
                    raise SaveError(f"{f}: no value (type:hash=value)")
                changes.append((kind, h, value))
        for path in expand([a.save] if a.cmd == "set" else a.saves):
            old = read_file(path)
            data = bytearray(old)
            done = set_values(data, changes if a.cmd == "set" else bomb_changes(data))
            if bytes(data) == old:
                print(f"{path}: nothing to change")
                continue
            backup = write_save(path, old, bytes(data))
            for h, before, after in done:
                print(f"{path}: {h:#010x} {before} -> {after}")
            print(f"{path}: old file kept as {os.path.basename(backup)}")
        return 0
    except (SaveError, OSError) as e:
        print(f"save_flags: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
