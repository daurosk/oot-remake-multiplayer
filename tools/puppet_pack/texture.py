"""Reads textures from the game's .bntx files (platform "Ounc") into RGBA pixels.

Only what the co-op HUD needs: 2D textures, one mip, formats BC4 and ASTC 4x4 (LDR).
Pure Python (no numpy), so it runs with the same Python as the puppet pack tool; slow
on big pictures, fine on the small HUD icons.
"""
import struct
import zlib


class TextureError(ValueError):
    pass


FORMAT_BC4 = 0x1D
FORMAT_ASTC_4X4 = 0x2D


class Texture:
    def __init__(self, name, fmt, width, height, block_height_log2, data, channels):
        self.name = name
        self.format = fmt  # high byte: the kind (0x1d BC4, 0x2d ASTC 4x4); low: unorm/srgb
        self.width = width
        self.height = height
        self.block_height_log2 = block_height_log2
        self.data = data
        self.channels = channels  # four source ids, R G B A: 0 zero, 1 one, 2-5 r g b a


def read_bntx(b, wanted=None):
    """{name: Texture} of a .bntx; only the names in `wanted` when it is given."""
    if b[:4] != b"BNTX":
        raise TextureError("not a .bntx file")
    count = struct.unpack_from("<I", b, 0x24)[0]
    table = struct.unpack_from("<Q", b, 0x28)[0]
    out = {}
    for i in range(count):
        p = struct.unpack_from("<Q", b, table + 8 * i)[0]
        if b[p:p + 4] != b"BRTI":
            raise TextureError(f"texture {i} has no BRTI header")
        name_at = struct.unpack_from("<Q", b, p + 0x60)[0]
        n = struct.unpack_from("<H", b, name_at)[0]
        name = b[name_at + 2:name_at + 2 + n].decode("utf-8")
        if wanted is not None and name not in wanted:
            continue
        mips = struct.unpack_from("<H", b, p + 0x16)[0]
        fmt, _access, w, h, _depth, _layers, bh = struct.unpack_from("<IIIIIII", b, p + 0x1C)
        size = struct.unpack_from("<I", b, p + 0x50)[0]
        swz = struct.unpack_from("<I", b, p + 0x58)[0]
        ptrs = struct.unpack_from("<Q", b, p + 0x70)[0]
        at = struct.unpack_from("<Q", b, ptrs)[0]
        if mips < 1:
            raise TextureError(f"{name}: no mip levels")
        out[name] = Texture(name, fmt, w, h, bh, b[at:at + size],
                            tuple((swz >> (8 * k)) & 0xFF for k in range(4)))
    return out


def gob_offset(x, y):
    """Byte offset of byte column x, row y inside a 512-byte GOB (64 bytes x 8 rows), as
    the "Ounc" .bntx files have it. It differs from the Switch 1 GOB (Tegra X1) in two
    bits: x bit 4 and y bit 1 trade places (found by decoding the HUD's own pictures)."""
    return ((x & 15) | ((y & 1) << 4) | (((y >> 1) & 1) << 5) | (((x >> 4) & 1) << 6)
            | (((y >> 2) & 1) << 7) | (((x >> 5) & 1) << 8))


def deswizzle(data, width_bytes, rows, block_height_log2):
    """Block-linear (GOBs, `1 << block_height_log2` GOBs tall per block) to linear rows."""
    gobs_tall = 1 << block_height_log2
    gobs_wide = (width_bytes + 63) // 64
    out = bytearray(width_bytes * rows)
    need = gobs_wide * 512 * gobs_tall * ((rows + 8 * gobs_tall - 1) // (8 * gobs_tall))
    if len(data) < need:
        data = data + bytes(need - len(data))
    for y in range(rows):
        row_base = ((y // (8 * gobs_tall)) * 512 * gobs_tall * gobs_wide
                    + ((y % (8 * gobs_tall)) // 8) * 512)
        o = y * width_bytes
        for x in range(0, width_bytes, 16):  # 16 bytes stay together in a GOB
            a = row_base + (x // 64) * 512 * gobs_tall + gob_offset(x, y)
            n = min(16, width_bytes - x)
            out[o + x:o + x + n] = data[a:a + n]
    return bytes(out)


# ---- BC4 ----

def bc4_block(blk):
    r0, r1 = blk[0], blk[1]
    bits = int.from_bytes(blk[2:8], "little")
    if r0 > r1:
        pal = [r0, r1] + [((7 - i) * r0 + i * r1) // 7 for i in range(1, 7)]
    else:
        pal = [r0, r1] + [((5 - i) * r0 + i * r1) // 5 for i in range(1, 5)] + [0, 255]
    return [(pal[(bits >> (3 * i)) & 7],) * 3 + (255,) for i in range(16)]


# ---- ASTC (LDR, 2D, 4x4 blocks) ----

# Integer sequence encoding: range -> (trits, quints, bits).
_ISE = {2: (0, 0, 1), 3: (1, 0, 0), 4: (0, 0, 2), 5: (0, 1, 0), 6: (1, 0, 1), 8: (0, 0, 3),
        10: (0, 1, 1), 12: (1, 0, 2), 16: (0, 0, 4), 20: (0, 1, 2), 24: (1, 0, 3), 32: (0, 0, 5),
        40: (0, 1, 3), 48: (1, 0, 4), 64: (0, 0, 6), 80: (0, 1, 4), 96: (1, 0, 5), 128: (0, 0, 7),
        160: (0, 1, 5), 192: (1, 0, 6), 256: (0, 0, 8)}
_WEIGHT_RANGES = (2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32)
_COLOUR_RANGES = (2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 256)


def _ise_bits(count, rng):
    t, q, b = _ISE[rng]
    if t:
        return (8 * count + 4) // 5 + count * b
    if q:
        return (7 * count + 2) // 3 + count * b
    return count * b


def _bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def _trits(T):
    if _bits(T, 2, 3) == 7:
        C = (_bits(T, 5, 3) << 2) | _bits(T, 0, 2)
        t4 = t3 = 2
    else:
        C = _bits(T, 0, 5)
        if _bits(T, 5, 2) == 3:
            t4, t3 = 2, _bits(T, 7, 1)
        else:
            t4, t3 = _bits(T, 7, 1), _bits(T, 5, 2)
    if _bits(C, 0, 2) == 3:
        t2, t1 = 2, _bits(C, 4, 1)
        t0 = (_bits(C, 3, 1) << 1) | (_bits(C, 2, 1) & ~_bits(C, 3, 1) & 1)
    elif _bits(C, 2, 2) == 3:
        t2, t1, t0 = 2, 2, _bits(C, 0, 2)
    else:
        t2, t1 = _bits(C, 4, 1), _bits(C, 2, 2)
        t0 = (_bits(C, 1, 1) << 1) | (_bits(C, 0, 1) & ~_bits(C, 1, 1) & 1)
    return (t0, t1, t2, t3, t4)


def _quints(Q):
    if _bits(Q, 1, 2) == 3 and _bits(Q, 5, 2) == 0:
        q2 = (_bits(Q, 0, 1) << 2) | ((_bits(Q, 4, 1) & ~_bits(Q, 0, 1) & 1) << 1) \
            | (_bits(Q, 3, 1) & ~_bits(Q, 0, 1) & 1)
        return (4, 4, q2)
    if _bits(Q, 1, 2) == 3:
        q2 = 4
        C = (_bits(Q, 3, 2) << 3) | ((~_bits(Q, 5, 2) & 3) << 1) | _bits(Q, 0, 1)
    else:
        q2 = _bits(Q, 5, 2)
        C = _bits(Q, 0, 5)
    if _bits(C, 0, 3) == 5:
        q1, q0 = 4, _bits(C, 3, 2)
    else:
        q1, q0 = _bits(C, 3, 2), _bits(C, 0, 3)
    return (q0, q1, q2)


def _ise_decode(v, start, count, rng):
    """[(digit or None, low bits, bit count)] of `count` values from bit `start` of int v."""
    t, q, b = _ISE[rng]
    out = []
    pos = start
    if t:
        spans = ((2, 0), (2, 2), (1, 4), (2, 5), (1, 7))  # m0 T0T1 m1 T2T3 m2 T4 m3 T5T6 m4 T7
        while len(out) < count:
            m, T = [], 0
            for n, sh in spans:
                m.append(_bits(v, pos, b))
                pos += b
                T |= _bits(v, pos, n) << sh
                pos += n
            out.extend((d, m[i], b) for i, d in enumerate(_trits(T)))
        return out[:count]
    if q:
        spans = ((3, 0), (2, 3), (2, 5))  # m0 Q0Q1Q2 m1 Q3Q4 m2 Q5Q6
        while len(out) < count:
            m, Q = [], 0
            for n, sh in spans:
                m.append(_bits(v, pos, b))
                pos += b
                Q |= _bits(v, pos, n) << sh
                pos += n
            out.extend((d, m[i], b) for i, d in enumerate(_quints(Q)))
        return out[:count]
    for _ in range(count):
        out.append((None, _bits(v, pos, b), b))
        pos += b
    return out


def _replicate(value, bits, to):
    if bits == 0:
        return 0
    out = have = 0
    while have < to:
        out = (out << bits) | value
        have += bits
    return out >> (have - to)


def _unq_colour(d, m, b, rng):
    t, q, _ = _ISE[rng]
    if not t and not q:
        return _replicate(m, b, 8)
    A = 0x1FF if m & 1 else 0
    bb, c, dd, e, f = ((m >> k) & 1 for k in range(1, 6))
    if t:
        C = {1: 204, 2: 93, 3: 44, 4: 22, 5: 11, 6: 5}[b]
        B = {1: 0, 2: (bb << 8) | (bb << 4) | (bb << 2) | (bb << 1),
             3: (c << 8) | (bb << 7) | (c << 3) | (bb << 2) | (c << 1) | bb,
             4: (dd << 8) | (c << 7) | (bb << 6) | (dd << 2) | (c << 1) | bb,
             5: (e << 8) | (dd << 7) | (c << 6) | (bb << 5) | (e << 1) | dd,
             6: (f << 8) | (e << 7) | (dd << 6) | (c << 5) | (bb << 4) | f}[b]
    else:
        C = {1: 113, 2: 54, 3: 26, 4: 13, 5: 6}[b]
        B = {1: 0, 2: (bb << 8) | (bb << 3) | (bb << 2),
             3: (c << 8) | (bb << 7) | (c << 2) | (bb << 1) | c,
             4: (dd << 8) | (c << 7) | (bb << 6) | (dd << 1) | c,
             5: (e << 8) | (dd << 7) | (c << 6) | (bb << 5) | e}[b]
    T = (d * C + B) ^ A
    return (A & 0x80) | (T >> 2)


def _unq_weight(d, m, b, rng):
    t, q, _ = _ISE[rng]
    if not t and not q:
        w = _replicate(m, b, 6)
    elif b == 0:
        w = (0, 32, 63)[d] if t else (0, 16, 32, 47, 63)[d]
    else:
        A = 0x7F if m & 1 else 0
        bb, c = (m >> 1) & 1, (m >> 2) & 1
        if t:
            C, B = {1: (50, 0), 2: (23, (bb << 6) | (bb << 2) | bb),
                    3: (11, (c << 6) | (bb << 5) | (c << 1) | bb)}[b]
        else:
            C, B = {1: (28, 0), 2: (13, (bb << 6) | (bb << 1))}[b]
        T = (d * C + B) ^ A
        w = (A & 0x20) | (T >> 2)
    return w + 1 if w > 32 else w


def _hash52(p):
    M = 0xFFFFFFFF
    p ^= p >> 15
    p = (p - (p << 17)) & M
    p = (p + (p << 7)) & M
    p = (p + (p << 4)) & M
    p ^= p >> 5
    p = (p + (p << 16)) & M
    p ^= p >> 7
    p ^= p >> 3
    p = (p ^ (p << 6)) & M
    p ^= p >> 17
    return p


def _partition(seed, x, y, count):
    x <<= 1  # a small block (under 31 texels)
    y <<= 1
    seed += (count - 1) * 1024
    r = _hash52(seed)
    s = [(r >> 0) & 15, (r >> 4) & 15, (r >> 8) & 15, (r >> 12) & 15, (r >> 16) & 15,
         (r >> 20) & 15, (r >> 24) & 15, (r >> 28) & 15]
    s = [v * v for v in s]
    if seed & 1:
        sh1 = 4 if seed & 2 else 5
        sh2 = 5 if count == 3 else 6
    else:
        sh1 = 5 if count == 3 else 6
        sh2 = 4 if seed & 2 else 5
    s = [v >> (sh1 if i % 2 == 0 else sh2) for i, v in enumerate(s)]
    a = (s[0] * x + s[1] * y + (r >> 14)) & 0x3F
    b = (s[2] * x + s[3] * y + (r >> 10)) & 0x3F
    c = (s[4] * x + s[5] * y + (r >> 6)) & 0x3F if count >= 3 else 0
    d = (s[6] * x + s[7] * y + (r >> 2)) & 0x3F if count >= 4 else 0
    if a >= b and a >= c and a >= d:
        return 0
    if b >= c and b >= d:
        return 1
    return 2 if c >= d else 3


def _clamp(v):
    return 0 if v < 0 else 255 if v > 255 else v


def _bts(a, b):
    """bit_transfer_signed: (a, b) -> (signed a, b)."""
    b = (b >> 1) | (a & 0x80)
    a = (a >> 1) & 0x3F
    if a & 0x20:
        a -= 0x40
    return a, b


def _blue(r, g, b, a):
    return ((r + b) >> 1, (g + b) >> 1, b, a)


def _endpoints(mode, v):
    if mode == 0:
        return (v[0], v[0], v[0], 255), (v[1], v[1], v[1], 255)
    if mode == 1:
        l0 = (v[0] >> 2) | (v[1] & 0xC0)
        l1 = min(l0 + (v[1] & 0x3F), 255)
        return (l0, l0, l0, 255), (l1, l1, l1, 255)
    if mode == 4:
        return (v[0], v[0], v[0], v[2]), (v[1], v[1], v[1], v[3])
    if mode == 5:
        d1, b0 = _bts(v[1], v[0])
        d3, b2 = _bts(v[3], v[2])
        l1, a1 = _clamp(b0 + d1), _clamp(b2 + d3)
        return (b0, b0, b0, b2), (l1, l1, l1, a1)
    if mode in (6, 10):
        a0, a1 = (v[4], v[5]) if mode == 10 else (255, 255)
        return (((v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, a0),
                (v[0], v[1], v[2], a1))
    if mode in (8, 12):
        a0, a1 = (v[6], v[7]) if mode == 12 else (255, 255)
        if v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]:
            return (v[0], v[2], v[4], a0), (v[1], v[3], v[5], a1)
        return _blue(v[1], v[3], v[5], a1), _blue(v[0], v[2], v[4], a0)
    if mode in (9, 13):
        d1, b0 = _bts(v[1], v[0])
        d3, b2 = _bts(v[3], v[2])
        d5, b4 = _bts(v[5], v[4])
        d7, b6 = _bts(v[7], v[6]) if mode == 13 else (0, 255)
        if d1 + d3 + d5 >= 0:
            return (b0, b2, b4, b6), tuple(_clamp(x) for x in (b0 + d1, b2 + d3, b4 + d5, b6 + d7))
        e0 = _blue(*(_clamp(x) for x in (b0 + d1, b2 + d3, b4 + d5, b6 + d7)))
        return e0, _blue(b0, b2, b4, b6)
    return None  # HDR modes: not used by the HUD


_ERROR = [(255, 0, 255, 255)] * 16


def _block_mode(mode):
    """(weights wide, weights tall, dual plane, weight range) or None."""
    R = (mode >> 4) & 1
    H = (mode >> 9) & 1
    D = (mode >> 10) & 1
    A = (mode >> 5) & 3
    if mode & 3:
        R |= (mode & 3) << 1
        B = (mode >> 7) & 3
        kind = (mode >> 2) & 3
        if kind == 0:
            w, h = B + 4, A + 2
        elif kind == 1:
            w, h = B + 8, A + 2
        elif kind == 2:
            w, h = A + 2, B + 8
        elif mode & 0x100:
            w, h = (B & 1) + 2, A + 2
        else:
            w, h = A + 2, (B & 1) + 6
    else:
        R |= ((mode >> 2) & 3) << 1
        if (mode >> 2) & 3 == 0:
            return None
        B = (mode >> 9) & 3
        kind = (mode >> 7) & 3
        if kind == 0:
            w, h = 12, A + 2
        elif kind == 1:
            w, h = A + 2, 12
        elif kind == 2:
            w, h = A + 6, B + 6
            D = H = 0
        elif A == 0:
            w, h = 6, 10
        elif A == 1:
            w, h = 10, 6
        else:
            return None
    q = R - 2 + 6 * H
    if q < 0 or q >= len(_WEIGHT_RANGES):
        return None
    return w, h, bool(D), _WEIGHT_RANGES[q]


def astc_block(blk):
    """16 RGBA texels (row by row) of one 4x4 ASTC block (LDR)."""
    v = int.from_bytes(blk, "little")
    if _bits(v, 0, 9) == 0x1FC:  # void extent: one colour (UNORM16 each)
        return [tuple(_bits(v, 64 + 16 * k, 16) >> 8 for k in range(4))] * 16
    bm = _block_mode(_bits(v, 0, 11))
    if bm is None:
        return _ERROR
    gw, gh, dual, wrange = bm
    parts = _bits(v, 11, 2) + 1
    wcount = gw * gh * (2 if dual else 1)
    if gw > 4 or gh > 4 or wcount > 64 or (dual and parts == 4):
        return _ERROR
    below = 128 - _ise_bits(wcount, wrange)
    seed = 0
    if parts == 1:
        cems = [_bits(v, 13, 4)]
        colour_start = 17
    else:
        seed = _bits(v, 13, 10)
        colour_start = 29
        enc = _bits(v, 23, 6)
        if enc & 3 == 0:
            cems = [(enc >> 2) & 0xF] * parts
        else:
            extra = 3 * parts - 4
            below -= extra
            enc |= _bits(v, below, extra) << 6
            base = (enc & 3) - 1
            cems = [((base + ((enc >> (2 + i)) & 1)) << 2) | ((enc >> (2 + parts + 2 * i)) & 3)
                    for i in range(parts)]
    ccs = 0
    if dual:
        below -= 2
        ccs = _bits(v, below, 2)
    nvals = sum(2 * ((c >> 2) + 1) for c in cems)
    if nvals > 18:
        return _ERROR
    crange = next((r for r in reversed(_COLOUR_RANGES)
                   if _ise_bits(nvals, r) <= below - colour_start), None)
    if crange is None:
        return _ERROR
    vals = [_unq_colour(d or 0, m, b, crange) for d, m, b in _ise_decode(v, colour_start, nvals, crange)]
    eps = []
    k = 0
    for c in cems:
        n = 2 * ((c >> 2) + 1)
        e = _endpoints(c, vals[k:k + n])
        if e is None:
            return _ERROR
        eps.append(e)
        k += n
    # Weights are stored from bit 127 down: read the bit-reversed block.
    rv = int(format(v, "0128b")[::-1], 2)
    raw = [_unq_weight(d or 0, m, b, wrange) for d, m, b in _ise_decode(rv, 0, wcount, wrange)]
    planes = 2 if dual else 1

    def grid(ix, iy, pl):
        return raw[(iy * gw + ix) * planes + pl] if ix < gw and iy < gh else 0

    ds = (1024 + 2) // 3  # the block is 4 texels wide and tall
    texels = []
    for t in range(4):
        for s in range(4):
            gs = (ds * s * (gw - 1) + 32) >> 6
            gt = (ds * t * (gh - 1) + 32) >> 6
            js, fs, jt, ft = gs >> 4, gs & 15, gt >> 4, gt & 15
            w11 = (fs * ft + 8) >> 4
            w10, w01 = ft - w11, fs - w11
            w00 = 16 - fs - ft + w11
            ws = [(grid(js, jt, pl) * w00 + grid(js + 1, jt, pl) * w01 + grid(js, jt + 1, pl) * w10
                   + grid(js + 1, jt + 1, pl) * w11 + 8) >> 4 for pl in range(planes)]
            e0, e1 = eps[_partition(seed, s, t, parts) if parts > 1 else 0]
            px = []
            for ch in range(4):
                w = ws[1] if dual and ch == ccs else ws[0]
                c0, c1 = (e0[ch] << 8) | 0x80, (e1[ch] << 8) | 0x80
                px.append(((c0 * (64 - w) + c1 * w + 32) >> 6) >> 8)
            texels.append(tuple(px))
    return texels


# ---- whole pictures ----

def decode(tex):
    """The texture as RGBA bytes (width * height * 4), the channel swizzle applied."""
    kind = tex.format >> 8
    if kind == FORMAT_BC4:
        bpb, block = 8, bc4_block
    elif kind == FORMAT_ASTC_4X4:
        bpb, block = 16, astc_block
    else:
        raise TextureError(f"{tex.name}: format {tex.format:#x} is not read")
    wb, hb = (tex.width + 3) // 4, (tex.height + 3) // 4
    lin = deswizzle(tex.data, wb * bpb, hb, tex.block_height_log2)
    w, h = tex.width, tex.height
    out = bytearray(w * h * 4)
    for by in range(hb):
        for bx in range(wb):
            o = (by * wb + bx) * bpb
            texels = block(lin[o:o + bpb])
            for k in range(16):
                x, y = bx * 4 + (k & 3), by * 4 + (k >> 2)
                if x < w and y < h:
                    src = texels[k]
                    out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(
                        0 if c == 0 else src[c - 2] if 2 <= c <= 5 else 255 for c in tex.channels)
    return bytes(out)


def png(width, height, rgba):
    """A PNG file (8-bit RGBA) of the pixels."""
    raw = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
