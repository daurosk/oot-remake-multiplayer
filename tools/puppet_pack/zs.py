"""The engine's .zs files: zstd frames, most with a dictionary from Pack/ZsDic.pack.zs.

The dictionary is picked by the id in each frame's header, never by a fixed number:
the ids differ between games of this engine family.
"""
import os

from . import sarc

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"


def _zstd():
    try:
        import zstandard
    except ImportError as e:  # pragma: no cover - depends on the venv
        raise SystemExit("this tool needs the 'zstandard' Python package") from e
    return zstandard


def frame_dict_id(b):
    if b[:4] != ZSTD_MAGIC:
        raise ValueError("not a zstd frame")
    fhd = b[4]
    n = (0, 1, 2, 4)[fhd & 3]
    off = 5 + (0 if (fhd >> 5) & 1 else 1)
    return int.from_bytes(b[off:off + n], "little") if n else 0


class Dicts:
    """The dictionaries from one ZsDic.pack.zs, by member name and by id."""

    def __init__(self, zsdic_pack_bytes):
        z = _zstd()
        raw = zsdic_pack_bytes
        if raw[:4] == ZSTD_MAGIC:
            raw = z.ZstdDecompressor().decompressobj().decompress(raw)
        self.by_name = {}
        self.by_id = {}
        for name, data in sarc.read(raw).items():
            d = z.ZstdCompressionDict(data)
            self.by_name[name] = d
            self.by_id[d.dict_id()] = d

    @classmethod
    def from_romfs(cls, romfs):
        with open(os.path.join(romfs, "Pack", "ZsDic.pack.zs"), "rb") as f:
            return cls(f.read())

    def decompress(self, raw):
        z = _zstd()
        did = frame_dict_id(raw)
        if did == 0:
            dctx = z.ZstdDecompressor()
        else:
            if did not in self.by_id:
                raise ValueError(f"no dictionary with id {did}")
            dctx = z.ZstdDecompressor(dict_data=self.by_id[did])
        return dctx.decompressobj().decompress(raw)

    def compress(self, data, dict_name=None, level=16):
        z = _zstd()
        if dict_name is None:
            c = z.ZstdCompressor(level=level, write_content_size=True, write_checksum=False)
        else:
            c = z.ZstdCompressor(level=level, dict_data=self.by_name[dict_name],
                                 write_content_size=True, write_checksum=False, write_dict_id=True)
        return c.compress(data)


def maybe_decompress(dicts, raw):
    return dicts.decompress(raw) if raw[:4] == ZSTD_MAGIC else raw
