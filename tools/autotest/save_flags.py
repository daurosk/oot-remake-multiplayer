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

  save_flags.py get SAVE int:0xdb1bcd4f [bool:0x2bb00768 ...]
  save_flags.py set SAVE int:0xdb1bcd4f=20 [bool:0x2bb00768=1 ...]
  save_flags.py bombs SAVE [SAVE ...]     what a real small bomb bag sets

Close the game first: it writes its saves on every scene change.
"""
import argparse
import struct
import sys

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
    """{(type id, hash): offset of the value word} for the inline entries."""
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
            if second == END:
                return out
            kind = second
        elif kind is None:
            raise SaveError("an entry before the first type")
        elif kind in TYPES.values():
            out[(kind, first)] = at + 4
        at += 8
    raise SaveError("no end record before the pool")


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
        if a.cmd == "get":
            data = open(a.save, "rb").read()
            for f in a.flags:
                kind, h, _ = parse_flag(f)
                v = get(data, kind, h)
                print(f"{f} = {'not in this save' if v is None else v}")
            return 0
        paths = [a.save] if a.cmd == "set" else a.saves
        for path in paths:
            data = bytearray(open(path, "rb").read())
            if a.cmd == "set":
                changes = []
                for f in a.flags:
                    kind, h, value = parse_flag(f)
                    if value is None:
                        raise SaveError(f"{f}: no value (type:hash=value)")
                    changes.append((kind, h, value))
            else:
                changes = bomb_changes(data)
            done = set_values(data, changes)
            if done:
                with open(path, "wb") as out:
                    out.write(data)
            for h, old, new in done:
                print(f"{path}: {h:#010x} {old} -> {new}")
            if not done:
                print(f"{path}: nothing to change")
        return 0
    except (SaveError, OSError) as e:
        print(f"save_flags: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
