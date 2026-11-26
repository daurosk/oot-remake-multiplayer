"""Read the game's main (an NSO) the way the game sees it in memory, for the study tools.

vtables.py and callers.py work on this image: the three segments placed at their addresses, the
relocations applied (RELR and RELA), and the imported symbols noted by the slot that holds them.
Nothing is written; the player's own main is only read.

Addresses are the ones co-op's code and docs use: Longshot loads rtld at 0x80000000 and main after
it at 0x80020000 (BASE), so the title screen's vtable is 0x84c106e8 and "main+0x3406710" is
0x83426710. A compressed NSO is read too (LZ4, slower in pure Python than a plain one).

Library only; the tools import it:
    m = nso.Main.load(path)
    m.u64(0x84c106e8), m.cstr(addr), m.in_text(addr), m.imports[slot]
"""
import struct

BASE = 0x80020000
PAGE = 0x1000

# Dynamic section tags (ELF, plus RELR and its older Android number).
DT_NULL, DT_PLTRELSZ, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0, 2, 5, 6, 7, 8
DT_JMPREL = 23
DT_RELRSZ, DT_RELR = 0x23, 0x24
DT_ANDROID_RELR, DT_ANDROID_RELRSZ = 0x6FFFE000, 0x6FFFE001

R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027


class NsoError(ValueError):
    pass


def lz4_block(src, size):
    """One LZ4 block (no frame) decompressed to `size` bytes."""
    try:
        return _lz4_block(src, size)
    except IndexError:
        raise NsoError("LZ4 block cut short") from None


def _lz4_block(src, size):
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break
        off = src[i] | src[i + 1] << 8
        i += 2
        if off == 0:
            raise NsoError("LZ4 match with offset 0")
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        if start < 0:
            raise NsoError("LZ4 match before the start")
        if off >= ml:
            out += out[start:start + ml]
        else:
            for k in range(ml):
                out.append(out[start + k])
    if len(out) != size:
        raise NsoError(f"LZ4 block made {len(out)} bytes, the header says {size}")
    return bytes(out)


def parse_addr(text, base=BASE):
    """An address as people write it: 0x84c106e8, 84c106e8, f_8088ffc0, main+0x3406710."""
    t = text.strip().lower()
    if t.startswith("main+"):
        return base + int(t[5:], 16)
    if t.startswith("f_"):
        t = t[2:]
    return int(t, 16)


class Main:
    """The main NSO as loaded: `image` covers base .. end of bss."""

    def __init__(self, data, base=BASE):
        if data[:4] != b"NSO0":
            raise NsoError("not an NSO (no NSO0 magic)")
        flags = struct.unpack_from("<I", data, 0x0C)[0]
        segs = []
        for k, at in enumerate((0x10, 0x20, 0x30)):
            file_off, mem_off, size = struct.unpack_from("<III", data, at)
            comp = struct.unpack_from("<I", data, 0x60 + 4 * k)[0]
            raw = data[file_off:file_off + (comp if flags & (1 << k) else size)]
            if flags & (1 << k):
                raw = lz4_block(raw, size)
            if len(raw) != size:
                raise NsoError(f"segment {k} is cut short")
            segs.append((mem_off, raw))
        bss = struct.unpack_from("<I", data, 0x3C)[0]
        self.build_id = data[0x40:0x60].hex()
        self.base = base
        end = segs[2][0] + len(segs[2][1]) + bss
        self.image = bytearray(end)
        for mem_off, raw in segs:
            self.image[mem_off:mem_off + len(raw)] = raw
        self.text = (base + segs[0][0], base + segs[0][0] + len(segs[0][1]))
        self.ro = (base + segs[1][0], base + segs[1][0] + len(segs[1][1]))
        self.data = (base + segs[2][0], base + segs[2][0] + len(segs[2][1]))
        self.bss = (self.data[1], base + end)
        self.imports = {}       # slot address -> symbol name (an import, or a symbol with its addend)
        self.relocated = 0
        self._relocate()

    @classmethod
    def load(cls, path, base=BASE):
        with open(path, "rb") as f:
            return cls(f.read(), base)

    # ------------------------------------------------------------------------------------ reads
    def u32(self, a):
        return struct.unpack_from("<I", self.image, a - self.base)[0]

    def u64(self, a):
        return struct.unpack_from("<Q", self.image, a - self.base)[0]

    def cstr(self, a, limit=256):
        o = a - self.base
        if not 0 <= o < len(self.image):
            return None
        e = self.image.find(b"\0", o, o + limit)
        if e < 0:
            return None
        return self.image[o:e].decode("utf-8", "replace")

    def in_text(self, a):
        return self.text[0] <= a < self.text[1]

    def in_image(self, a):
        return self.base <= a < self.base + len(self.image)

    def text_words(self):
        """The text segment as 32-bit words (index i is address text[0] + 4 * i)."""
        o = self.text[0] - self.base
        return memoryview(bytes(self.image[o:o + (self.text[1] - self.text[0])])).cast("I")

    def data_words(self):
        """(first address, 64-bit words) of ro and data together (they are adjacent or nearly)."""
        lo, hi = self.ro[0], self.data[1]
        o = lo - self.base
        n = (hi - lo) // 8 * 8
        return lo, memoryview(bytes(self.image[o:o + n])).cast("Q")

    # ------------------------------------------------------------------------------ relocations
    def _put64(self, a, v):
        struct.pack_into("<Q", self.image, a - self.base, v & 0xFFFFFFFFFFFFFFFF)

    def _relocate(self):
        img = self.image
        mod0 = struct.unpack_from("<I", img, 4)[0]
        if img[mod0:mod0 + 4] != b"MOD0":
            raise NsoError("no MOD0 header")
        dyn = mod0 + struct.unpack_from("<i", img, mod0 + 4)[0]
        tags = {}
        o = dyn
        while o + 16 <= len(img):
            tag, val = struct.unpack_from("<qQ", img, o)
            o += 16
            if tag == DT_NULL:
                break
            tags.setdefault(tag, val)
        relr = tags.get(DT_RELR, tags.get(DT_ANDROID_RELR))
        relrsz = tags.get(DT_RELRSZ, tags.get(DT_ANDROID_RELRSZ, 0))
        if relr is not None:
            where = 0
            for k in range(relrsz // 8):
                e = struct.unpack_from("<Q", img, relr + 8 * k)[0]
                if e & 1 == 0:
                    self._add_base(e)
                    where = e + 8
                else:
                    bits, i = e >> 1, 0
                    while bits:
                        if bits & 1:
                            self._add_base(where + 8 * i)
                        bits >>= 1
                        i += 1
                    where += 63 * 8
        symtab, strtab = tags.get(DT_SYMTAB), tags.get(DT_STRTAB)
        for start, size in ((tags.get(DT_RELA), tags.get(DT_RELASZ, 0)),
                            (tags.get(DT_JMPREL), tags.get(DT_PLTRELSZ, 0))):
            if start is None:
                continue
            for r in range(start, start + size, 24):
                off, info, addend = struct.unpack_from("<QQq", img, r)
                typ, sym = info & 0xFFFFFFFF, info >> 32
                slot = self.base + off
                if typ == R_AARCH64_RELATIVE:
                    self._put64(slot, self.base + addend)
                    self.relocated += 1
                elif typ in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT) and symtab is not None:
                    name_off, _info, shndx, value = struct.unpack_from("<IBxHQ", img, symtab + 24 * sym)
                    name = self.cstr(self.base + strtab + name_off) or f"sym{sym}"
                    if shndx and value:
                        self._put64(slot, self.base + value + addend)
                        self.relocated += 1
                    else:
                        self.imports[slot] = name + (f"+{addend}" if addend else "")

    def _add_base(self, off):
        struct.pack_into("<Q", self.image, off, struct.unpack_from("<Q", self.image, off)[0] + self.base)
        self.relocated += 1
