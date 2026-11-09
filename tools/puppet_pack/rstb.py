"""The resource size table (`RESTBL`, version 1).

Layout: magic `RESTBL`, u32 version, u32 name length, u32 hash count, u32 name count,
then (u32 crc32 of the path, u32 size) rows sorted by crc, then (name, u32 size) rows
for paths whose crc collides. Paths are the romfs path without `.zs`.
"""
import struct
import zlib

MAGIC = b"RESTBL"


class RstbError(ValueError):
    pass


class Rstb:
    def __init__(self, version=1, name_len=160, by_crc=None, by_name=None):
        self.version = version
        self.name_len = name_len
        self.by_crc = dict(by_crc or {})
        self.by_name = dict(by_name or {})

    @classmethod
    def loads(cls, data):
        if data[:6] != MAGIC:
            raise RstbError("not a RESTBL")
        version, name_len, nc, nn = struct.unpack_from("<IIII", data, 6)
        o = 0x16
        need = o + 8 * nc + (name_len + 4) * nn
        if len(data) < need:
            raise RstbError("RESTBL is truncated")
        by_crc = {}
        for _ in range(nc):
            c, s = struct.unpack_from("<II", data, o)
            by_crc[c] = s
            o += 8
        by_name = {}
        for _ in range(nn):
            n = data[o:o + name_len].split(b"\0", 1)[0].decode("utf-8")
            by_name[n], = struct.unpack_from("<I", data, o + name_len)
            o += name_len + 4
        return cls(version, name_len, by_crc, by_name)

    def dumps(self):
        out = bytearray(MAGIC + struct.pack("<IIII", self.version, self.name_len,
                                            len(self.by_crc), len(self.by_name)))
        for c in sorted(self.by_crc):
            out += struct.pack("<II", c, self.by_crc[c])
        for n in sorted(self.by_name, key=lambda s: s.encode("utf-8")):
            b = n.encode("utf-8")
            if len(b) >= self.name_len:
                raise RstbError(f"name too long for the table: {n}")
            out += b + b"\0" * (self.name_len - len(b)) + struct.pack("<I", self.by_name[n])
        return bytes(out)

    @staticmethod
    def crc(path):
        return zlib.crc32(path.encode("utf-8")) & 0xFFFFFFFF

    def get(self, path):
        if path in self.by_name:
            return self.by_name[path]
        return self.by_crc.get(self.crc(path))

    def set(self, path, size):
        """Sets a size. A path whose crc is taken by another path goes to the name rows.

        Without a list of every romfs path we cannot tell a collision from an existing
        entry for the same path, so callers only call this for paths they own or for
        paths whose entry they read with get().
        """
        if path in self.by_name:
            self.by_name[path] = size
        else:
            self.by_crc[self.crc(path)] = size

    def add_new(self, path, size):
        """Adds a path the game does not have. If its crc already belongs to a game file,
        the entry goes to the name rows, which the engine checks before the crc rows."""
        c = self.crc(path)
        if path in self.by_name or c in self.by_crc:
            self.by_name[path] = size
        else:
            self.by_crc[c] = size
