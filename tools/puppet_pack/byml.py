"""BYML version 7 (little-endian `YB`), read and write.

Values map to Python as: dict (hash node, string keys), list (array node),
str, bool, int (Int), float (Float), and the wrappers below for the other
number kinds, so a file read and written back keeps its exact node types.
"""
import struct

T_HASH32 = 0x20
T_HASH32_REMAP = 0x21
T_STRING = 0xA0
T_BINARY = 0xA1
T_FILE = 0xA2
T_ARRAY = 0xC0
T_DICT = 0xC1
T_STRTAB = 0xC2
T_BOOL = 0xD0
T_INT = 0xD1
T_FLOAT = 0xD2
T_UINT = 0xD3
T_INT64 = 0xD4
T_UINT64 = 0xD5
T_DOUBLE = 0xD6
T_NULL = 0xFF


class _Num:
    __slots__ = ("v",)

    def __init__(self, v):
        self.v = v

    def __eq__(self, o):
        return type(o) is type(self) and o.v == self.v

    def __hash__(self):
        return hash((type(self).__name__, self.v))

    def __repr__(self):
        return f"{type(self).__name__}({self.v!r})"


class UInt(_Num):
    pass


class Int64(_Num):
    pass


class UInt64(_Num):
    pass


class Double(_Num):
    pass


class Binary(_Num):
    """Binary data node (0xA1)."""


class File(_Num):
    """File data node (0xA2): bytes plus an alignment."""

    __slots__ = ("align",)

    def __init__(self, v, align=0):
        super().__init__(v)
        self.align = align


class Hash32(dict):
    """Hash map keyed by u32 (0x20). Keys are ints."""


class Hash32Remap(dict):
    """Hash map keyed by u32 with a remap index (0x21): values are (value, index)."""


class BymlError(ValueError):
    pass


# --------------------------------------------------------------------------- read


def loads(data):
    data = bytes(data)
    if data[:2] != b"YB":
        raise BymlError("not a little-endian BYML (magic YB)")
    version, keys_off, strs_off, root_off = struct.unpack_from("<HIII", data, 2)
    if version < 2 or version > 10:
        raise BymlError(f"unexpected BYML version {version}")

    def strtab(off):
        if off == 0:
            return []
        if data[off] != T_STRTAB:
            raise BymlError(f"bad string table at {off:#x}")
        n = int.from_bytes(data[off + 1:off + 4], "little")
        offs = struct.unpack_from(f"<{n + 1}I", data, off + 4)
        out = []
        for i in range(n):
            s = off + offs[i]
            e = data.index(b"\0", s)
            out.append(data[s:e].decode("utf-8"))
        return out

    keys = strtab(keys_off)
    strs = strtab(strs_off)

    def value(t, raw):
        if t == T_STRING:
            return strs[raw]
        if t == T_BOOL:
            return raw != 0
        if t == T_INT:
            return struct.unpack("<i", struct.pack("<I", raw))[0]
        if t == T_FLOAT:
            return struct.unpack("<f", struct.pack("<I", raw))[0]
        if t == T_UINT:
            return UInt(raw)
        if t == T_NULL:
            return None
        if t == T_INT64:
            return Int64(struct.unpack_from("<q", data, raw)[0])
        if t == T_UINT64:
            return UInt64(struct.unpack_from("<Q", data, raw)[0])
        if t == T_DOUBLE:
            return Double(struct.unpack_from("<d", data, raw)[0])
        if t == T_BINARY:
            n = struct.unpack_from("<I", data, raw)[0]
            return Binary(data[raw + 4:raw + 4 + n])
        if t == T_FILE:
            n, al = struct.unpack_from("<II", data, raw)
            return File(data[raw + 8:raw + 8 + n], al)
        if t in (T_ARRAY, T_DICT, T_HASH32, T_HASH32_REMAP):
            return node(raw)
        raise BymlError(f"unknown node type {t:#x}")

    def node(off):
        t = data[off]
        n = int.from_bytes(data[off + 1:off + 4], "little")
        if t == T_ARRAY:
            types = data[off + 4:off + 4 + n]
            vo = off + 4 + ((n + 3) & ~3)
            vals = struct.unpack_from(f"<{n}I", data, vo)
            return [value(types[i], vals[i]) for i in range(n)]
        if t == T_DICT:
            out = {}
            for i in range(n):
                e = off + 4 + 8 * i
                k = int.from_bytes(data[e:e + 3], "little")
                out[keys[k]] = value(data[e + 3], struct.unpack_from("<I", data, e + 4)[0])
            return out
        if t == T_HASH32:
            ents = [struct.unpack_from("<II", data, off + 4 + 8 * i) for i in range(n)]
            types = data[off + 4 + 8 * n:off + 4 + 9 * n]
            return Hash32({h: value(types[i], v) for i, (h, v) in enumerate(ents)})
        if t == T_HASH32_REMAP:
            ents = [struct.unpack_from("<III", data, off + 4 + 12 * i) for i in range(n)]
            types = data[off + 4 + 12 * n:off + 4 + 13 * n]
            return Hash32Remap({h: (value(types[i], v), ix) for i, (h, v, ix) in enumerate(ents)})
        raise BymlError(f"unknown container type {t:#x} at {off:#x}")

    if root_off == 0:
        return None
    return node(root_off)


# -------------------------------------------------------------------------- write


def _collect(v, keys, strs):
    if isinstance(v, dict) and not isinstance(v, (Hash32, Hash32Remap)):
        for k, x in v.items():
            keys.add(k)
            _collect(x, keys, strs)
    elif isinstance(v, Hash32Remap):
        for x, _ in v.values():
            _collect(x, keys, strs)
    elif isinstance(v, (list, Hash32)):
        for x in (v.values() if isinstance(v, Hash32) else v):
            _collect(x, keys, strs)
    elif isinstance(v, str):
        strs.add(v)


def _type(v):
    if v is None:
        return T_NULL
    if isinstance(v, bool):
        return T_BOOL
    if isinstance(v, int):
        return T_INT
    if isinstance(v, float):
        return T_FLOAT
    if isinstance(v, str):
        return T_STRING
    if isinstance(v, UInt):
        return T_UINT
    if isinstance(v, Int64):
        return T_INT64
    if isinstance(v, UInt64):
        return T_UINT64
    if isinstance(v, Double):
        return T_DOUBLE
    if isinstance(v, File):
        return T_FILE
    if isinstance(v, Binary):
        return T_BINARY
    if isinstance(v, Hash32Remap):
        return T_HASH32_REMAP
    if isinstance(v, Hash32):
        return T_HASH32
    if isinstance(v, dict):
        return T_DICT
    if isinstance(v, list):
        return T_ARRAY
    raise BymlError(f"cannot write {type(v).__name__}")


def _key(v):
    """A hashable form of a value that tells all node types apart (1 != 1.0 != True)."""
    t = _type(v)
    if t == T_DICT:
        return (t, tuple((k, _key(x)) for k, x in sorted(v.items())))
    if t == T_ARRAY:
        return (t, tuple(_key(x) for x in v))
    if t == T_HASH32:
        return (t, tuple((h, _key(x)) for h, x in sorted(v.items())))
    if t == T_HASH32_REMAP:
        return (t, tuple((h, _key(x), ix) for h, (x, ix) in sorted(v.items())))
    if t == T_FLOAT:
        return (t, struct.pack("<f", v))
    if t == T_DOUBLE:
        return (t, struct.pack("<d", v.v))
    if t == T_FILE:
        return (t, v.v, v.align)
    if isinstance(v, _Num):
        return (t, v.v)
    return (t, v)


def _strtab(items):
    n = len(items)
    blobs = [s.encode("utf-8") + b"\0" for s in items]
    head = 4 + 4 * (n + 1)
    offs, pos = [], head
    for b in blobs:
        offs.append(pos)
        pos += len(b)
    offs.append(pos)
    out = bytes([T_STRTAB]) + n.to_bytes(3, "little") + struct.pack(f"<{n + 1}I", *offs) + b"".join(blobs)
    return out + b"\0" * (-len(out) % 4)


def dumps(root, version=7):
    keys, strs = set(), set()
    _collect(root, keys, strs)
    # Tables sorted by raw bytes: the engine binary-searches them.
    keys = sorted(keys, key=lambda s: s.encode("utf-8"))
    strs = sorted(strs, key=lambda s: s.encode("utf-8"))
    kix = {k: i for i, k in enumerate(keys)}
    six = {s: i for i, s in enumerate(strs)}

    buf = bytearray(16)
    if keys:
        keys_off = len(buf)
        buf += _strtab(keys)
    else:
        keys_off = 0
    if strs:
        strs_off = len(buf)
        buf += _strtab(strs)
    else:
        strs_off = 0

    def align(n=4):
        buf.extend(b"\0" * (-len(buf) % n))

    # Equal values written once and shared by offset, as the game's own files do.
    shared = {}

    def inline(v):
        """Returns the u32 slot for a value, writing big values out first."""
        t = _type(v)
        if t == T_NULL:
            return 0
        if t == T_BOOL:
            return 1 if v else 0
        if t == T_INT:
            return struct.unpack("<I", struct.pack("<i", v))[0]
        if t == T_FLOAT:
            return struct.unpack("<I", struct.pack("<f", v))[0]
        if t == T_STRING:
            return six[v]
        if t == T_UINT:
            return v.v
        key = _key(v)
        if key in shared:
            return shared[key]
        align(8 if t in (T_INT64, T_UINT64, T_DOUBLE) else 4)
        off = len(buf)
        shared[key] = off
        if t in (T_ARRAY, T_DICT, T_HASH32, T_HASH32_REMAP):
            got = write_node(v)
            assert got == off
            return off
        if t == T_INT64:
            buf.extend(struct.pack("<q", v.v))
        elif t == T_UINT64:
            buf.extend(struct.pack("<Q", v.v))
        elif t == T_DOUBLE:
            buf.extend(struct.pack("<d", v.v))
        elif t == T_BINARY:
            buf.extend(struct.pack("<I", len(v.v)) + v.v)
        elif t == T_FILE:
            buf.extend(struct.pack("<II", len(v.v), v.align) + v.v)
        return off

    def write_node(v):
        t = _type(v)
        if t == T_ARRAY:
            n = len(v)
            align()
            off = len(buf)
            hdr = bytes([T_ARRAY]) + n.to_bytes(3, "little") + bytes(_type(x) for x in v)
            hdr += b"\0" * (-len(hdr) % 4)
            buf.extend(hdr)
            slots = len(buf)
            buf.extend(b"\0" * (4 * n))
            for i, x in enumerate(v):
                struct.pack_into("<I", buf, slots + 4 * i, inline(x))
            return off
        if t == T_DICT:
            items = sorted(v.items(), key=lambda kv: kix[kv[0]])
            n = len(items)
            align()
            off = len(buf)
            buf.extend(bytes([T_DICT]) + n.to_bytes(3, "little"))
            ents = len(buf)
            buf.extend(b"\0" * (8 * n))
            for i, (k, x) in enumerate(items):
                e = ents + 8 * i
                buf[e:e + 3] = kix[k].to_bytes(3, "little")
                buf[e + 3] = _type(x)
                struct.pack_into("<I", buf, e + 4, inline(x))
            return off
        if t == T_HASH32:
            items = sorted(v.items())
            n = len(items)
            align()
            off = len(buf)
            buf.extend(bytes([T_HASH32]) + n.to_bytes(3, "little"))
            ents = len(buf)
            buf.extend(b"\0" * (8 * n) + bytes(_type(x) for _, x in items))
            align()
            for i, (h, x) in enumerate(items):
                struct.pack_into("<II", buf, ents + 8 * i, h, inline(x))
            return off
        if t == T_HASH32_REMAP:
            items = sorted(v.items())
            n = len(items)
            align()
            off = len(buf)
            buf.extend(bytes([T_HASH32_REMAP]) + n.to_bytes(3, "little"))
            ents = len(buf)
            buf.extend(b"\0" * (12 * n) + bytes(_type(x) for _, (x, _i) in items))
            align()
            for i, (h, (x, ix)) in enumerate(items):
                struct.pack_into("<III", buf, ents + 12 * i, h, inline(x), ix)
            return off
        raise BymlError("root must be a container")

    root_off = write_node(root) if root is not None else 0
    struct.pack_into("<2sHIII", buf, 0, b"YB", version, keys_off, strs_off, root_off)
    align()
    return bytes(buf)
