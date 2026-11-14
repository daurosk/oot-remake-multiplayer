"""SARC archives (little-endian), read and write."""
import struct


class SarcError(ValueError):
    pass


def name_hash(name, mult=0x65):
    h = 0
    for c in name.encode("utf-8"):
        h = (h * mult + (c if c < 0x80 else c - 0x100)) & 0xFFFFFFFF
    return h


def read(data):
    """Returns {name: bytes} in archive order."""
    data = bytes(data)
    if data[:4] != b"SARC":
        raise SarcError("not a SARC")
    if data[6:8] != b"\xff\xfe":
        raise SarcError("big-endian SARC not supported")
    hdr, = struct.unpack_from("<H", data, 4)
    data_off, = struct.unpack_from("<I", data, 0x0C)
    if data[hdr:hdr + 4] != b"SFAT":
        raise SarcError("no SFAT")
    cnt, = struct.unpack_from("<H", data, hdr + 6)
    nodes = hdr + 0x0C
    sfnt = nodes + 0x10 * cnt
    if data[sfnt:sfnt + 4] != b"SFNT":
        raise SarcError("no SFNT")
    names_at = sfnt + 8
    out = {}
    for i in range(cnt):
        _h, attr, b, e = struct.unpack_from("<IIII", data, nodes + 0x10 * i)
        if not attr & 0x01000000:
            raise SarcError("nameless SARC entry not supported")
        no = names_at + (attr & 0xFFFF) * 4
        end = data.index(b"\0", no)
        out[data[no:end].decode("utf-8")] = data[data_off + b:data_off + e]
    return out


def _align_for(name, data):
    # Nested archives and BYML want 8; a plain alignment of 8 for all keeps it simple
    # and is what the engine's own small packs use.
    return 8


def read_entries(data):
    """[(name, bytes)] in archive order, every entry (some of the engine's archives hold a file
    more than once under one name, as Title's layouts four times each)."""
    data = bytes(data)
    if data[:4] != b"SARC" or data[6:8] != b"\xff\xfe":
        raise SarcError("not a little-endian SARC")
    hdr, = struct.unpack_from("<H", data, 4)
    data_off, = struct.unpack_from("<I", data, 0x0C)
    cnt, = struct.unpack_from("<H", data, hdr + 6)
    nodes = hdr + 0x0C
    names_at = nodes + 0x10 * cnt + 8
    out = []
    for i in range(cnt):
        _h, attr, b, e = struct.unpack_from("<IIII", data, nodes + 0x10 * i)
        if not attr & 0x01000000:
            raise SarcError("nameless SARC entry not supported")
        no = names_at + (attr & 0xFFFF) * 4
        out.append((data[no:data.index(b"\0", no)].decode("utf-8"), data[data_off + b:data_off + e]))
    return out


def write(files, data_align=8):
    """files: {name: bytes}, or [(name, bytes)] with a name more than once (read_entries). Entries are
    sorted by name hash (stable: repeats keep their order), as the engine binary-searches them."""
    items = sorted(files.items() if hasattr(files, "items") else list(files), key=lambda kv: name_hash(kv[0]))
    names = bytearray()
    name_offs = []
    for n, _ in items:
        name_offs.append(len(names) // 4)
        names += n.encode("utf-8") + b"\0"
        names += b"\0" * (-len(names) % 4)
    sfat = bytearray(b"SFAT" + struct.pack("<HHI", 0x0C, len(items), 0x65))
    blob = bytearray()
    for (n, d), no in zip(items, name_offs):
        blob += b"\0" * (-len(blob) % max(data_align, _align_for(n, d)))
        b = len(blob)
        blob += d
        sfat += struct.pack("<IIII", name_hash(n), 0x01000000 | no, b, len(blob))
    sfnt = b"SFNT" + struct.pack("<HH", 0x08, 0) + bytes(names)
    head_len = 0x14 + len(sfat) + len(sfnt)
    data_off = (head_len + data_align - 1) & ~(data_align - 1)
    total = data_off + len(blob)
    hdr = b"SARC" + struct.pack("<HHIIHH", 0x14, 0xFEFF, total, data_off, 0x0100, 0)
    out = hdr + bytes(sfat) + sfnt
    out += b"\0" * (data_off - len(out))
    return out + bytes(blob)
