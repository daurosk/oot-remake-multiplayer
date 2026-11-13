"""The adult tunic's other textures (#202): recolour only their green parts, in place.

Adult Link's tunic is three materials: `Mt_Angin` (hat and upper body: the tile, puppet_pack.tunic),
`Mt_Vest` (mesh `Hood_A`: the lower tunic and back, texture `Mt_Vest_Player_Adult_Alb`, ASTC 8x8)
and `Mt_Angin_Acs` (rope and hood parts, `Mt_Angin_Acs_Player_Adult_Alb`, BC1). With only the tile
recoloured, the adult's lower tunic stayed green on your own Link and on the puppets.

These textures hold more than the tunic, so only green end colours change (`is_green`): every
block of a BC1/BC3/ASTC texture has a few end colours and its texels are blends of them, so
changing the green ends recolours the green texels and leaves the rest. No encoder, no change of
size or layout: the game's size-table entry stands. ASTC blocks keep their weights; their colour
values are re-encoded in the same range (base+offset modes become direct ones, same value count).
A block that cannot be re-encoded exactly as asked stays as it was.

Nothing from the game is in this file: everything is made from the user's own romfs.
"""
import struct

from . import texture as tx
from .tunic import COLORS, TunicError, _brti, _lin, _luma, _srgb, _bc1_order, pack565, unpack565, rename

# texture name in Tex/ -> the player models (variants) that name it
EXTRA = {
    "Mt_Vest_Player_Adult_Alb": ("Adult",),
    "Mt_Angin_Acs_Player_Adult_Alb": ("Adult",),
    # The child's cap knot, headband and collar (Mt_Cloth_Parts: Angin_Knot_Head_Band,
    # Angin_Parts_Collar_R) and the string at the back (Mt_Vest: String_Back_1): about a third
    # green each, the green edge of the cap seen in the hand test of a768c36.
    "Mt_Cloth_Parts_Player_Child_Alb": ("Child",),
    "Mt_Vest_Player_Child_Alb": ("Child",),
}

BC1, BC3 = 0x1A, 0x1C


def extra_name(name, color):
    """The puppet copy's name: as long as the game's (`_Player_` -> `_Coop_R_`)."""
    if "_Player_" not in name:
        raise TunicError(f"{name}: no _Player_ in the name")
    return name.replace("_Player_", f"_Coop_{COLORS[color][0]}_", 1)


def hue(rgb):
    r, g, b = rgb[:3]
    hi, lo = max(r, g, b), min(r, g, b)
    if hi == lo:
        return None
    d = hi - lo
    if hi == r:
        return (60 * (g - b) / d) % 360
    if hi == g:
        return 60 * (b - r) / d + 120
    return 60 * (r - g) / d + 240


def is_green(rgb):
    """Tunic green, olive included (hue 40..180); not brown leather (about 30), grey or white."""
    hi, lo = max(rgb[:3]), min(rgb[:3])
    h = hue(rgb)
    return h is not None and 40 <= h <= 180 and hi >= 6 and hi - lo >= max(3, 0.08 * hi)


class Shade:
    """A green end colour -> the target, as bright (linear luma) against `base` as it was."""

    def __init__(self, target, base):
        if base <= 0:
            raise TunicError("no green in the texture")
        self.t = tuple(_lin(c) for c in target)
        self.base = base
        self.memo = {}

    def __call__(self, rgb):
        rgb = tuple(rgb[:3])
        out = self.memo.get(rgb)
        if out is None:
            k = _luma(tuple(_lin(c) for c in rgb)) / self.base
            out = self.memo[rgb] = tuple(_srgb(t * k) for t in self.t)
        return out


def _green_luma(colors):
    total = n = 0
    for c in colors:
        if is_green(c):
            total += _luma(tuple(_lin(x) for x in c[:3]))
            n += 1
    return total / n if n else 0.0


# --- BC1 / BC3 ---

def _bc_blocks(data):
    fmt, start, end = _brti(data)
    if fmt == BC3:
        step, off = 16, 8
    elif fmt == BC1:
        step, off = 8, 0
    else:
        return None
    return fmt, [p + off for p in range(start, end - step + 1, step)]


def _bc_recolor(data, target, base=None):
    fmt, offs = _bc_blocks(data)
    ends = []
    for q in offs:
        c0, c1, idx = struct.unpack_from("<HHI", data, q)
        if c0 or c1 or idx:
            ends += [unpack565(c0), unpack565(c1)]
    f = Shade(target, base or _green_luma(ends))
    out = bytearray(data)
    for q in offs:
        c0, c1, idx = struct.unpack_from("<HHI", out, q)
        if not (c0 or c1 or idx):
            continue
        e0, e1 = unpack565(c0), unpack565(c1)
        g0, g1 = is_green(e0), is_green(e1)
        if not (g0 or g1):
            continue
        n0 = pack565(f(e0)) if g0 else c0
        n1 = pack565(f(e1)) if g1 else c1
        if fmt == BC1:
            n0, n1, idx = _bc1_order(c0 > c1, n0, n1, idx)
        struct.pack_into("<HHI", out, q, n0, n1, idx)
    return bytes(out)


# --- ASTC ---

ASTC_FIRST, ASTC_LAST = 0x2D, 0x3A
_QUANT = {}
_TCACHE = {}


def _quant(rng):
    """value 0..255 -> the nearest (digit, low bits) of the colour range."""
    q = _QUANT.get(rng)
    if q is None:
        t, qq, b = tx._ISE[rng]
        digits = range(3) if t else range(5) if qq else (None,)
        cands = [(tx._unq_colour(d or 0, m, b, rng), d, m) for d in digits for m in range(1 << b)]
        q = _QUANT[rng] = [min(cands, key=lambda c: abs(c[0] - v))[1:] for v in range(256)]
    return q


def _group_code(kind, digits, n, outside):
    """The trit (T, 8 bits) or quint (Q, 7 bits) code whose first `n` digits are `digits` and
    whose bits in `outside` (past the end of the sequence) are zero."""
    key = (kind, digits[:n], n, outside)
    c = _TCACHE.get(key)
    if c is None:
        dec, width = (tx._trits, 8) if kind == "t" else (tx._quints, 7)
        c = next((x for x in range(1 << width) if not x & outside and dec(x)[:n] == digits[:n]), -1)
        _TCACHE[key] = c
    if c < 0:
        raise ValueError("no code")
    return c


def _ise_encode(vals, rng):
    """[(digit, low bits)] -> (int, bit count) of the integer sequence."""
    t, q, b = tx._ISE[rng]
    count = len(vals)
    length = tx._ise_bits(count, rng)
    out = pos = 0
    if not t and not q:
        for _, m in vals:
            out |= m << pos
            pos += b
        return out, length
    spans = ((2, 0), (2, 2), (1, 4), (2, 5), (1, 7)) if t else ((3, 0), (2, 3), (2, 5))
    per = 5 if t else 3
    for g in range(0, count, per):
        grp = vals[g:g + per]
        n = len(grp)
        digits = tuple(d for d, _ in grp) + (0,) * (per - n)
        ms = [m for _, m in grp] + [0] * (per - n)
        # where each code bit lands, to keep the ones past the end zero
        p, places = pos, []
        for i, (w, sh) in enumerate(spans):
            p += b
            places += [(sh + k, p + k) for k in range(w)]
            p += w
        outside = sum(1 << bit for bit, at in places if at >= length)
        code = _group_code("t" if t else "q", digits, n, outside)
        for i, (w, sh) in enumerate(spans):
            out |= ms[i] << pos
            pos += b
            out |= ((code >> sh) & ((1 << w) - 1)) << pos
            pos += w
    return out & ((1 << length) - 1), length


def _ise_read(v, start, count, rng):
    length = tx._ise_bits(count, rng)
    vm = (v >> start) & ((1 << length) - 1)  # past the end reads as zero
    return [(d, m) for d, m, _ in tx._ise_decode(vm, 0, count, rng)]


def _astc_layout(v):
    """(cem field position or None, [cem], colour start, value count, colour range) or None."""
    bm = tx._block_mode(tx._bits(v, 0, 11))
    if bm is None:
        return None
    gw, gh, dual, wrange = bm
    parts = tx._bits(v, 11, 2) + 1
    wcount = gw * gh * (2 if dual else 1)
    if wcount > 64 or (dual and parts == 4):
        return None
    below = 128 - tx._ise_bits(wcount, wrange)
    if parts == 1:
        cems, cstart, cem_at = [tx._bits(v, 13, 4)], 17, 13
    else:
        cstart = 29
        enc = tx._bits(v, 23, 6)
        if enc & 3 == 0:
            cems, cem_at = [(enc >> 2) & 0xF] * parts, 25
        else:
            extra = 3 * parts - 4
            below -= extra
            enc |= tx._bits(v, below, extra) << 6
            base = (enc & 3) - 1
            cems = [((base + ((enc >> (2 + i)) & 1)) << 2) | ((enc >> (2 + parts + 2 * i)) & 3)
                    for i in range(parts)]
            cem_at = None
    if dual:
        below -= 2
    nvals = sum(2 * ((c >> 2) + 1) for c in cems)
    if nvals > 18:
        return None
    crange = next((r for r in reversed(tx._COLOUR_RANGES) if tx._ise_bits(nvals, r) <= below - cstart), None)
    if crange is None:
        return None
    return cem_at, cems, cstart, nvals, crange


def _decode_vals(dm, rng):
    b = tx._ISE[rng][2]
    return [tx._unq_colour(d or 0, m, b, rng) for d, m in dm]


def _cl(x):
    return min(max(int(round(x)), 0), 255)


def _scaled(c, total):
    s = sum(c) or 1
    return tuple(_cl(x * total / s) for x in c)


def _direct(n0, n1, a0, a1, alpha):
    """Mode 8/12 value candidates for end colours n0, n1 (the weights stay, so the ends cannot
    swap). Plain values decode as asked only when the second end's sum is not below the first's;
    else blue contraction, which cannot hold a bright red over little blue. So also: both ends
    their average, or the brighter end dimmed / the darker one lightened to the other's sum."""
    def plain(x0, x1):
        return [x0[0], x1[0], x0[1], x1[1], x0[2], x1[2]] + ([a0, a1] if alpha else [])
    contr = [_cl(2 * n1[0] - n1[2]), _cl(2 * n0[0] - n0[2]), _cl(2 * n1[1] - n1[2]), _cl(2 * n0[1] - n0[2]),
             n1[2], n0[2]] + ([a1, a0] if alpha else [])
    avg = tuple(_cl((x + y) / 2) for x, y in zip(n0, n1))
    return [plain(n0, n1), contr, plain(avg, avg), plain(_scaled(n0, sum(n1)), n1), plain(n0, _scaled(n1, sum(n0)))]


def _astc_block(v, f):
    """The block with its green end colours through `f`, or None (unchanged)."""
    if tx._bits(v, 0, 9) == 0x1FC:  # void extent: one RGBA16 colour
        c = [tx._bits(v, 64 + 16 * k, 16) >> 8 for k in range(3)]
        if not is_green(c):
            return None
        for k, x in enumerate(f(c)):
            v = (v & ~(0xFFFF << (64 + 16 * k))) | ((x << 8 | x) << (64 + 16 * k))
        return v
    lay = _astc_layout(v)
    if lay is None:
        return None
    cem_at, cems, cstart, nvals, rng = lay
    dm = _ise_read(v, cstart, nvals, rng)
    vals = _decode_vals(dm, rng)
    quant = _quant(rng)
    new_dm = list(dm)
    new_cems = list(cems)
    changed = False
    # one mode field for all partitions: if one base+offset partition becomes direct, all do
    convert_all = cem_at is not None and cems[0] in (9, 13) and len(set(cems)) == 1 and _any_green(cems, vals)
    k = 0
    for i, c in enumerate(cems):
        n = 2 * ((c >> 2) + 1)
        part = vals[k:k + n]
        if c in (6, 10):
            if is_green(part[:3]):
                rgb = f(part[:3])
                for j in range(3):
                    new_dm[k + j] = quant[rgb[j]]
                changed = True
        elif c in (8, 9, 12, 13):
            e0, e1 = tx._endpoints(c, part)
            g0, g1 = is_green(e0), is_green(e1)
            if g0 or g1 or convert_all:
                n0 = f(e0) if g0 else e0[:3]
                n1 = f(e1) if g1 else e1[:3]
                alpha = c in (12, 13)
                best = None
                for cand in _direct(n0, n1, e0[3], e1[3], alpha):
                    qdm = [quant[x] for x in cand]
                    got = tx._endpoints(12 if alpha else 8, _decode_vals(qdm, rng))
                    err = sum(abs(a - b) for a, b in zip(got[0][:3] + got[1][:3], tuple(n0) + tuple(n1)))
                    err += sum(abs(a - b) for a, b in ((got[0][3], e0[3]), (got[1][3], e1[3])))
                    if best is None or err < best[0]:
                        best = (err, qdm)
                if best is None:
                    return None
                new_dm[k:k + n] = best[1]
                new_cems[i] = 12 if alpha else 8
                changed = True
        k += n
    if not changed:
        return None
    if new_cems != cems:
        if cem_at is None or len(set(new_cems)) != 1 or len(set(cems)) != 1:
            return None  # mixed modes: the field cannot say it
        v = (v & ~(0xF << cem_at)) | (new_cems[0] << cem_at)
    try:
        bits, length = _ise_encode(new_dm, rng)
    except ValueError:
        return None
    v = (v & ~(((1 << length) - 1) << cstart)) | (bits << cstart)
    if _ise_read(v, cstart, nvals, rng) != [(d, m) for d, m in new_dm]:
        return None
    return v


def _any_green(cems, vals):
    """Any green end colour in a block's direct or base+offset partitions."""
    k = 0
    for c in cems:
        n = 2 * ((c >> 2) + 1)
        if c in (8, 9, 12, 13):
            e0, e1 = tx._endpoints(c, vals[k:k + n])
            if is_green(e0) or is_green(e1):
                return True
        k += n
    return False


def _astc_ends(v):
    if tx._bits(v, 0, 9) == 0x1FC:
        return [[tx._bits(v, 64 + 16 * k, 16) >> 8 for k in range(3)]]
    lay = _astc_layout(v)
    if lay is None:
        return []
    _, cems, cstart, nvals, rng = lay
    vals = _decode_vals(_ise_read(v, cstart, nvals, rng), rng)
    out, k = [], 0
    for c in cems:
        n = 2 * ((c >> 2) + 1)
        e = tx._endpoints(c, vals[k:k + n])
        if e:
            out += [e[0], e[1]]
        k += n
    return out


def _astc_recolor(data, target, base=None):
    fmt, start, end = _brti(data)
    blocks = [(p, int.from_bytes(data[p:p + 16], "little")) for p in range(start, end - 15, 16)]
    ends = []
    for _, v in blocks:
        ends += _astc_ends(v)
    f = Shade(target, base or _green_luma(ends))
    out = bytearray(data)
    for p, v in blocks:
        nv = _astc_block(v, f)
        if nv is not None:
            out[p:p + 16] = nv.to_bytes(16, "little")
    return bytes(out)


def recolor(data, color, base=None):
    """The texture (a one-texture BNTX) with its green parts in `color`, same name and size.
    `base`: the linear luma that becomes the target (the main tile's average, so each part keeps
    its shade against the tile); default the texture's own green average."""
    target = COLORS[color][1]
    fmt = _brti(data)[0]
    if fmt in (BC1, BC3):
        return _bc_recolor(data, target, base)
    if ASTC_FIRST <= fmt <= ASTC_LAST:
        return _astc_recolor(data, target, base)
    raise TunicError(f"texture format 0x{fmt:02x} is not BC1, BC3 or ASTC")


def for_puppet(data, own, name, color):
    """The puppet's copy: green is the game's texture renamed, the rest `own` renamed."""
    return rename(data if color == "green" else own, name, extra_name(name, color), at_least=1)


def model_names(model, color, names):
    """A player model with each of `names` it names changed to the colour's copy."""
    for n in names:
        model = rename(model, n, extra_name(n, color), at_least=0, at_most=1)
    return model
