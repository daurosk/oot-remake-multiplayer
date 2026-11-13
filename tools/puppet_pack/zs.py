"""The engine's .zs files: zstd frames, most with a dictionary from Pack/ZsDic.pack.zs.

The dictionary is picked by the id in each frame's header, never by a fixed number:
the ids differ between games of this engine family.
"""
import os

from . import sarc

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"


class _Dict:
    """One dictionary: its bytes and the id the frames name it by."""

    def __init__(self, data):
        self.data = bytes(data)
        # A zstd dictionary starts with its magic (0xEC30A437), then its id.
        self._id = int.from_bytes(self.data[4:8], "little") if self.data[:4] == bytes.fromhex("37a430ec") else 0

    def dict_id(self):
        return self._id


def _backend():
    """(decompress(raw, dict bytes or None), compress(data, dict bytes or None, level)) from the
    `zstandard` package, else Python 3.14's own compression.zstd."""
    try:
        import zstandard as z

        def dec(raw, dic=None):
            d = z.ZstdDecompressor(dict_data=z.ZstdCompressionDict(dic)) if dic else z.ZstdDecompressor()
            return d.decompressobj().decompress(raw)

        def comp(data, dic=None, level=16):
            if dic is None:
                c = z.ZstdCompressor(level=level, write_content_size=True, write_checksum=False)
            else:
                c = z.ZstdCompressor(level=level, dict_data=z.ZstdCompressionDict(dic),
                                     write_content_size=True, write_checksum=False, write_dict_id=True)
            return c.compress(data)
        return dec, comp
    except ImportError:
        pass
    try:
        from compression import zstd as z
    except ImportError as e:  # pragma: no cover - depends on the Python
        raise SystemExit("this tool needs the 'zstandard' Python package, or Python 3.14") from e

    def dec(raw, dic=None):
        return z.decompress(raw, zstd_dict=z.ZstdDict(dic)) if dic else z.decompress(raw)

    def comp(data, dic=None, level=16):
        P = z.CompressionParameter
        options = {P.compression_level: level, P.content_size_flag: 1, P.checksum_flag: 0, P.dict_id_flag: 1}
        return z.compress(data, options=options, zstd_dict=z.ZstdDict(dic) if dic else None)
    return dec, comp


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
        self._dec, self._comp = _backend()
        raw = zsdic_pack_bytes
        if raw[:4] == ZSTD_MAGIC:
            raw = self._dec(raw)
        self.by_name = {}
        self.by_id = {}
        for name, data in sarc.read(raw).items():
            d = _Dict(data)
            self.by_name[name] = d
            self.by_id[d.dict_id()] = d

    @classmethod
    def from_romfs(cls, romfs):
        with open(os.path.join(romfs, "Pack", "ZsDic.pack.zs"), "rb") as f:
            return cls(f.read())

    def decompress(self, raw):
        did = frame_dict_id(raw)
        if did == 0:
            return self._dec(raw)
        if did not in self.by_id:
            raise ValueError(f"no dictionary with id {did}")
        return self._dec(raw, self.by_id[did].data)

    def compress(self, data, dict_name=None, level=16):
        return self._comp(data, None if dict_name is None else self.by_name[dict_name].data, level)


def maybe_decompress(dicts, raw):
    return dicts.decompress(raw) if raw[:4] == ZSTD_MAGIC else raw
