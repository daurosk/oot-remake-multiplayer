"""Which blocks of a gear item's metalness texture lie under each colour part (#202).

The game draws Link's swords, the Hylian and Mirror Shields and the gauntlets with a metalness
texture beside the colour one (`Mt_..._Mtl`, BC4: 0 cloth or wood, up to 255 metal). A metal texel's
colour is drawn as the colour of what it reflects, with no diffuse light, so a part painted in the
customizer's colour (coop_look_paint: only the colour texture) shows much darker than the colour
picked when that part is metal: the Hylian Shield's grey frame (metal 154 of 255 on average, its
blue face and red bird 0), every blade, both gauntlets, the Mirror Shield's mirror.

So when a metal part is painted, the module also paints its metal down: the BC4 blocks of the
metalness texture under that part become 0 (hookshot/src/coop_look_parts.cpp, Metal). This file
works out those blocks from the user's own romfs, once, when the pack is made:

- a block of the colour texture belongs to a part when all its end colours have that part's tone
  (the module's table: the first rule whose tones match, as the painter decides it; black ends
  aside); a block mixing the part with an unpainted tone (the gold Triforce, a gold guard) does not,
  so the gold keeps its metal;
- a block of the metalness texture, in every mip level, is listed for a part when it lies under a
  colour block of that part (by texture coordinates: both textures cover the same UVs) and is not
  already all 0.

`coop_look/metal.bin` beside gear.bin (gear.bin holds the metalness textures' own texels too, as
GEAR names them). Little-endian:

    "COOPMTL1", u32 count, then per entry: the metalness texture's name in the game (64 bytes,
    NUL-filled), u32 the part (CoopLook::Part's number), u32 n, then n u32 byte offsets of BC4 blocks
    in that texture's texels (gear.bin's image: every mip level).

Nothing from the game is in this file: offsets only, made from the user's own romfs.
"""
import struct

from . import texture as tx
from . import tunic_extra
from .tunic import TunicError

FILE = "metal.bin"
MAGIC = b"COOPMTL1"
NAME_BYTES = 64

# The module's tones (hookshot/src/coop_look_paint.h Tone).
BLACK, GREY, RED, BROWN, GOLD, YELLOW, GREEN, CYAN, BLUE, PURPLE, PINK, WARM_GREY = (1 << k for k in range(12))

# (metalness texture, its colour texture, [(part number, tones)] in the module's rule order:
# hookshot/src/coop_look_parts.cpp Gear(), the same parts and tones). Only the textures whose model
# names both (Model/*.bfres): the Biggoron model's colour is Alb.00 (Alb.01 is its animation's).
METAL = (
    ("Mt_Weapon_Shield_Hylian_Mtl", "Mt_Weapon_Shield_Hylian_Alb", ((3, GREY | WARM_GREY), (4, BLUE | PURPLE), (5, RED))),
    ("Mt_Weapon_Shield_Mirror_Mtl", "Mt_Weapon_Shield_Mirror_Alb", ((6, RED), (7, GREY | BLUE | PURPLE | CYAN))),
    ("Mt_Weapon_Sword_Kokiri_Mtl", "Mt_Weapon_Sword_Kokiri_Alb", ((9, GREY), (10, BROWN | RED))),
    ("Mt_Weapon_Sword_Master_Mtl", "Mt_Weapon_Sword_Master_Alb", ((11, GREY), (12, BLUE | PURPLE))),
    ("Mt_Weapon_Sword_Biggoron_Mtl", "Mt_Weapon_Sword_Biggoron_Alb.00", ((13, GREY), (14, BLUE | PURPLE))),
    ("Mt_Silver_Armor_Adult_Gauntlet_Silver_Mtl", "Mt_Silver_Armor_Adult_Gauntlet_Silver_Alb", ((16, GREY),)),
    ("Mt_Gold_Armor_Adult_Gauntlet_Gold_Mtl", "Mt_Gold_Armor_Adult_Gauntlet_Gold_Alb", ((16, GOLD | BROWN),)),
)
TEXTURES = tuple(m for m, _, _ in METAL)

# ASTC block sizes by the BNTX format's high byte.
ASTC = {0x2D: (4, 4), 0x2E: (5, 4), 0x2F: (5, 5), 0x30: (6, 5), 0x31: (6, 6), 0x32: (8, 5), 0x33: (8, 6),
        0x34: (8, 8), 0x35: (10, 5), 0x36: (10, 6), 0x37: (10, 8), 0x38: (10, 10), 0x39: (12, 10),
        0x3A: (12, 12)}
BC1, BC3, BC4 = 0x1A, 0x1C, 0x1D


def tone_of(c):
    """The module's ToneOf (hookshot/src/coop_look_paint.cpp), for an sRGB colour."""
    r, g, b = c[:3]
    hi, lo = max(r, g, b), min(r, g, b)
    if hi < 25:
        return BLACK
    if hi - lo < max(12.0, 0.15 * hi):
        return GREY
    h = tunic_extra.hue((r, g, b))
    if h < 15 or h >= 345:
        return RED
    gold = GOLD | WARM_GREY if hi - lo < 0.25 * hi else GOLD   # row 185: the frame's pale warm grey
    if h < 45:
        return gold if hi >= 150 else BROWN
    if h < 70:
        return gold if hi >= 150 else YELLOW
    if h < 160:
        return GREEN
    if h < 200:
        return CYAN
    if h < 260:
        return BLUE
    if h < 300:
        return PURPLE
    return PINK


def _unpack565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def info(bntx):
    """(format byte, width, height, block height log2, [mip offsets from the image's start], image)
    of a one-texture BNTX."""
    if bntx[:4] != b"BNTX":
        raise TunicError("not a BNTX file")
    table = struct.unpack_from("<Q", bntx, 0x28)[0]
    p = struct.unpack_from("<Q", bntx, table)[0]
    if bntx[p:p + 4] != b"BRTI":
        raise TunicError("no BRTI header")
    mips = struct.unpack_from("<H", bntx, p + 0x16)[0]
    fmt, _access, w, h, _depth, _layers, bh = struct.unpack_from("<IIIIIII", bntx, p + 0x1C)
    size = struct.unpack_from("<I", bntx, p + 0x50)[0]
    ptrs = struct.unpack_from("<Q", bntx, p + 0x70)[0]
    at = [struct.unpack_from("<Q", bntx, ptrs + 8 * k)[0] for k in range(max(mips, 1))]
    return fmt >> 8 & 0xFF, w, h, bh, [a - at[0] for a in at], bntx[at[0]:at[0] + size]


def block_at(bx, by, width_bytes, rows, gobs_tall, block_bytes):
    """Byte offset of block (bx, by) in a block-linear mip level (the layout texture.deswizzle reads)."""
    gobs_wide = (width_bytes + 63) // 64
    x, y = bx * block_bytes, by
    return ((y // (8 * gobs_tall)) * 512 * gobs_tall * gobs_wide + ((y % (8 * gobs_tall)) // 8) * 512
            + (x // 64) * 512 * gobs_tall + tx.gob_offset(x, y))


def mip_gobs_tall(block_height_log2, rows):
    """GOBs per block in a mip level of `rows` block rows: the first level's, halved while the level
    is no taller than half of it (the GPU's rule for smaller levels)."""
    g = 1 << block_height_log2
    while g > 1 and rows <= 8 * (g // 2):
        g //= 2
    return g


def colour_parts(bntx, rules):
    """(blocks wide, blocks tall, block w, block h, width, height, {(bx, by): part}) of a colour
    texture's first mip level: a block's part is the first rule matching its end colours, when all of
    them (black aside) give the same one."""
    fmt, w, h, bh, _, image = info(bntx)
    if fmt in ASTC:
        fw, fh = ASTC[fmt]
        step = 16
    elif fmt in (BC1, BC3):
        fw = fh = 4
        step = 8 if fmt == BC1 else 16
    else:
        raise TunicError(f"colour texture format 0x{fmt:02x} is not read")
    wb, hb = (w + fw - 1) // fw, (h + fh - 1) // fh
    lin = tx.deswizzle(image, wb * step, hb, bh)
    parts = {}
    for by in range(hb):
        for bx in range(wb):
            blk = lin[(by * wb + bx) * step:(by * wb + bx + 1) * step]
            if fmt in ASTC:
                ends = tunic_extra._astc_ends(int.from_bytes(blk, "little"))
            else:
                o = 8 if fmt == BC3 else 0
                c0, c1, idx = struct.unpack_from("<HHI", blk, o)
                ends = [_unpack565(c0), _unpack565(c1)] if (c0 or c1 or idx) else []
            # Only a block wholly of one part (its every end colour but black): a block that mixes
            # a part with a tone no rule paints (the Hylian Shield's gold Triforce, a sword's gold
            # guard) keeps its metal.
            # Other tones no rule paints (dark shading) do not count.
            found = set()
            for t in (tone_of(e) for e in ends):
                p = next((p for p, tones in rules if tones & t), None)
                if p is not None:
                    found.add(p)
                elif t & (GOLD | YELLOW):
                    found.add(None)
            if len(found) == 1 and None not in found:
                parts[(bx, by)] = found.pop()
    return wb, hb, fw, fh, w, h, parts


def blocks(mtl_bntx, alb_bntx, rules):
    """{part: [byte offsets of the metalness texture's BC4 blocks under it, every mip level]}; blocks
    already all 0 are left out, and so is a part with none."""
    fmt, w, h, bh, mips, image = info(mtl_bntx)
    if fmt != BC4:
        raise TunicError(f"metalness texture format 0x{fmt:02x} is not BC4")
    wb, hb, fw, fh, aw, ah, parts = colour_parts(alb_bntx, rules)
    out = {}
    for level, start in enumerate(mips):
        lw, lh = max(1, w >> level), max(1, h >> level)
        mw, mh = (lw + 3) // 4, (lh + 3) // 4
        g = mip_gobs_tall(bh, mh)
        for my in range(mh):
            for mx in range(mw):
                a = start + block_at(mx, my, mw * 8, mh, g, 8)
                if a + 8 > len(image) or image[a:a + 8] == bytes(8):
                    continue
                if max(v[0] for v in tx.bc4_block(image[a:a + 8])) == 0:
                    continue
                # The colour blocks under this one (same texture coordinates).
                x0, x1 = mx * 4 * aw // lw // fw, min(wb - 1, ((mx + 1) * 4 * aw - 1) // lw // fw)
                y0, y1 = my * 4 * ah // lh // fh, min(hb - 1, ((my + 1) * 4 * ah - 1) // lh // fh)
                under = {parts.get((x, y)) for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)}
                for part in under - {None}:
                    out.setdefault(part, []).append(a)
    return {p: sorted(set(v)) for p, v in out.items()}


def metal(read_tex, log=print):
    """The metal file from `read_tex(name)` (a texture's BNTX, or None); None when no item has both
    of its textures."""
    entries = []
    for mtl, alb, rules in METAL:
        m, a = read_tex(mtl), read_tex(alb)
        if m is None or a is None:
            log(f"look: no {mtl if m is None else alb} in the romfs: its metal stays when painted")
            continue
        try:
            for part, offs in sorted(blocks(m, a, rules).items()):
                entries.append((mtl, part, offs))
        except (TunicError, ValueError, struct.error) as e:
            log(f"look: {mtl}: {e}; its metal stays when painted")
    if not entries:
        return None
    return pack(entries)


def pack(entries):
    out = [MAGIC, struct.pack("<I", len(entries))]
    for name, part, offs in entries:
        n = name.encode("ascii")
        if not 0 < len(n) < NAME_BYTES:
            raise TunicError(f"texture name '{name}' does not fit {NAME_BYTES - 1} bytes")
        out += [n.ljust(NAME_BYTES, b"\0"), struct.pack("<II", part, len(offs)),
                struct.pack(f"<{len(offs)}I", *offs)]
    return b"".join(out)


def unpack(blob):
    """The metal file -> [(name, part, [offsets])]."""
    if blob[:8] != MAGIC:
        raise TunicError("not a metal file")
    count = struct.unpack_from("<I", blob, 8)[0]
    out, at = [], 12
    for _ in range(count):
        if at + NAME_BYTES + 8 > len(blob):
            raise TunicError("the metal file is cut short")
        name = blob[at:at + NAME_BYTES].rstrip(b"\0").decode("ascii")
        part, n = struct.unpack_from("<II", blob, at + NAME_BYTES)
        at += NAME_BYTES + 8
        if at + 4 * n > len(blob):
            raise TunicError("the metal file is cut short")
        out.append((name, part, list(struct.unpack_from(f"<{n}I", blob, at))))
        at += 4 * n
    return out
