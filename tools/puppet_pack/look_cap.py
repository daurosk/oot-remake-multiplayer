"""Which blocks of Link's tunic textures hold only his cap: the Customize page's Cap trim
(docs/puppet.md, "The Link customizer").

Link's cap cloth shares the tunic's tiling weave (Angin_Tile_A_Alb: no texel of it is the cap's
alone), so the cap's cloth always has the tunic's colour. But three of his unwrapped detail textures
have regions only the cap's triangles use: the child's headband and knot
(Mt_Cloth_Parts_Player_Child_Alb), the adult's hood trims (Mt_Vest_Player_Adult_Alb,
Mt_Angin_Acs_Player_Adult_Alb). The module paints those blocks in the Cap trim colour over the
tunic's painting (hookshot/src/coop_look_parts.cpp, TunicPainted). This file works them out from
the user's own romfs, once, when the pack is made:

- the shapes of Link's model (Model/Player_<age>.Player_<age>.bfres, every level of detail) whose
  material samples the texture;
- a vertex belongs to the cap when its heaviest bone is the cap's (Ph_Cap*, Ph_HatFringe*,
  Ph_HatString*, or the head itself: the hood is skinned to it); a triangle is the cap's when all
  three of its vertices are;
- the triangles' texture coordinates (`_u0`, the set that samples these unwrapped textures, scale 1)
  over the texture's block grid, every mip level (BC1 4x4, ASTC its own block size; texture
  coordinates repeat): a block is the cap's when a cap triangle covers some of it and no other
  triangle of those shapes does.

`coop_look/cap.bin` beside green.bin (green.bin holds the textures' own texels). Little-endian:

    "COOPCAP1", u32 count, then per texture: its name in the game (64 bytes, NUL-filled), u32 the
    bytes of one block (8 BC1, 16 BC3 and ASTC), u32 n, then n u32 byte offsets of its cap blocks
    in the texture's texels (green.bin's image: every mip level, block-linear as the game holds it).

Nothing from the game is in this file: offsets only, made from the user's own romfs.
"""
import math
import struct

from . import bfres
from . import look_metal as lm
from .tunic import TunicError

FILE = "cap.bin"
MAGIC = b"COOPCAP1"
NAME_BYTES = 64

# (texture, the model whose shapes sample it): the textures with cap-only regions. The module's
# CoopLookParts::TunicTextures has all three (green.bin holds them).
CAP = (
    ("Mt_Cloth_Parts_Player_Child_Alb", "Child"),   # headband and knot
    ("Mt_Vest_Player_Adult_Alb", "Adult"),          # the hood's trims
    ("Mt_Angin_Acs_Player_Adult_Alb", "Adult"),     # the hood's and cap's trims
)
TEXTURES = tuple(t for t, _ in CAP)
CAP_BONES = ("Ph_Cap", "Ph_HatFringe", "Ph_HatString")
HEAD_BONES = ("Head", "Head_2")
UV = "_u0"


def model_path(variant):
    return f"Model/Player_{variant}.Player_{variant}.bfres.zs"


def is_cap_bone(name):
    return name in HEAD_BONES or name.startswith(CAP_BONES)


def vertex_bones(model_file, shape):
    """Each vertex's heaviest bone (its name) in a shape."""
    m = model_file.model
    if shape.skin_count == 0:
        name = m.bones[shape.bone] if shape.bone < len(m.bones) else ""
        return [name] * model_file.vertex_count(shape)
    best = None
    for k in range(4):
        idx = model_file.attribute(shape, f"_i{k}")
        if idx is None:
            break
        w = model_file.attribute(shape, f"_w{k}")
        if best is None:
            best = [(-1.0, -1)] * len(idx)
        for v, row in enumerate(idx):
            weights = w[v] if w is not None else (1.0,) + (0.0,) * (len(row) - 1)
            for i, wt in zip(row, weights):
                if wt > best[v][0]:
                    best[v] = (wt, int(i))
    if best is None:
        raise TunicError(f"{shape.name}: skinned, but no _i0")
    out = []
    for _, i in best:
        b = m.matrix_bones[i] if 0 <= i < len(m.matrix_bones) else -1
        out.append(m.bones[b] if 0 <= b < len(m.bones) else "")
    return out


def uv_triangles(model_file, texture):
    """(cap triangles, other triangles) of the shapes whose material samples `texture`, every level
    of detail, as ((u, v), (u, v), (u, v)); and the names of those shapes."""
    m = model_file.model
    cap, other, shapes = [], [], []
    for s in m.shapes:
        if s.material >= len(m.materials) or texture not in m.materials[s.material].textures:
            continue
        uv = model_file.attribute(s, UV)
        if uv is None:
            raise TunicError(f"{s.name}: no {UV}")
        bones = vertex_bones(model_file, s)
        shapes.append(s.name)
        for t in model_file.triangles(s):
            if max(t) >= len(uv) or max(t) >= len(bones):
                raise TunicError(f"{s.name}: a triangle names vertex {max(t)} of {len(uv)}")
            tri = tuple(uv[i][:2] for i in t)
            (cap if all(is_cap_bone(bones[i]) for i in t) else other).append(tri)
    return cap, other, shapes


def _overlaps(tri, x0, y0, x1, y1):
    """Whether a triangle and the box [x0, x1] x [y0, y1] share some area (separating axes)."""
    xs = [p[0] for p in tri]
    ys = [p[1] for p in tri]
    if max(xs) <= x0 or min(xs) >= x1 or max(ys) <= y0 or min(ys) >= y1:
        return False
    corners = ((x0, y0), (x1, y0), (x0, y1), (x1, y1))
    for k in range(3):
        (ax, ay), (bx, by) = tri[k], tri[(k + 1) % 3]
        nx, ny = by - ay, ax - bx   # the edge's normal
        cx, cy = tri[(k + 2) % 3]
        side = nx * (cx - ax) + ny * (cy - ay)
        if side == 0:
            return False   # no area
        # the box wholly on the other side of this edge from the triangle
        if all((nx * (px - ax) + ny * (py - ay)) * side <= 0 for px, py in corners):
            return False
    return True


def covered(tris, width, height, fw, fh):
    """{(bx, by)} of the blocks (fw x fh texels) of a width x height level that the triangles cover
    some of; texture coordinates repeat (a last block past the texture's edge is only the texels
    in it)."""
    wb, hb = (width + fw - 1) // fw, (height + fh - 1) // fh
    out = set()
    for tri in tris:
        # In texels, the triangle moved by whole repeats next to the origin, then each repeat it
        # reaches into.
        ox, oy = math.floor(min(p[0] for p in tri)), math.floor(min(p[1] for p in tri))
        t = tuple(((u - ox) * width, (v - oy) * height) for u, v in tri)
        for ky in range(math.ceil(max(p[1] for p in t) / height)):
            for kx in range(math.ceil(max(p[0] for p in t) / width)):
                s = tuple((x - kx * width, y - ky * height) for x, y in t)
                bx0, bx1 = max(0, int(min(p[0] for p in s) // fw)), min(wb - 1, int(max(p[0] for p in s) // fw))
                by0, by1 = max(0, int(min(p[1] for p in s) // fh)), min(hb - 1, int(max(p[1] for p in s) // fh))
                for by in range(by0, by1 + 1):
                    for bx in range(bx0, bx1 + 1):
                        if (bx, by) not in out and _overlaps(s, bx * fw, by * fh, min((bx + 1) * fw, width),
                                                             min((by + 1) * fh, height)):
                            out.add((bx, by))
    return out


def block_size(fmt):
    """(block width, height, bytes) of a BNTX format's high byte."""
    if fmt in lm.ASTC:
        return lm.ASTC[fmt] + (16,)
    if fmt == lm.BC1:
        return 4, 4, 8
    if fmt == lm.BC3:
        return 4, 4, 16
    raise TunicError(f"texture format 0x{fmt:02x} is not read")


def blocks(bntx, cap_tris, other_tris):
    """(the byte offsets of the texture's cap blocks, every mip level; (cap blocks, blocks) of the
    first level)."""
    fmt, w, h, bh, mips, image = lm.info(bntx)
    fw, fh, step = block_size(fmt)
    offs = []
    first = (0, 0)
    for level, start in enumerate(mips):
        lw, lh = max(1, w >> level), max(1, h >> level)
        wb, hb = (lw + fw - 1) // fw, (lh + fh - 1) // fh
        cap = covered(cap_tris, lw, lh, fw, fh) - covered(other_tris, lw, lh, fw, fh)
        if level == 0:
            first = (len(cap), wb * hb)
        g = lm.mip_gobs_tall(bh, hb)
        for bx, by in cap:
            a = start + lm.block_at(bx, by, wb * step, hb, g, step)
            if a + step <= len(image):
                offs.append(a)
    return sorted(set(offs)), first


def cap(read_tex, read_model, log=print):
    """The cap file from `read_tex(name)` (a texture's BNTX, or None) and `read_model(variant)` (Link's
    model file of that age, or None); None when no texture has a cap block."""
    models = {}
    entries = []
    for name, variant in CAP:
        bntx = read_tex(name)
        if variant not in models:
            raw = read_model(variant)
            try:
                models[variant] = bfres.Bfres(raw) if raw is not None else None
            except bfres.BfresError as e:
                log(f"look: {model_path(variant)}: {e}; the Cap trim leaves that age's cap alone")
                models[variant] = None
        model = models[variant]
        if bntx is None or model is None:
            log(f"look: no {name if bntx is None else model_path(variant)} in the romfs: the Cap trim "
                f"leaves {name} alone")
            continue
        try:
            cap_tris, other_tris, shapes = uv_triangles(model, name)
            offs, (n, total) = blocks(bntx, cap_tris, other_tris)
        except (TunicError, bfres.BfresError, ValueError, struct.error) as e:
            log(f"look: {name}: {e}; the Cap trim leaves it alone")
            continue
        log(f"look: {FILE}: {name}: {n} of {total} blocks hold only the cap ({100.0 * n / max(total, 1):.1f}%; "
            f"{len(offs)} with the smaller mip levels; {len(cap_tris)} cap and {len(other_tris)} other triangles "
            f"of {', '.join(shapes) or 'no shape'})")
        if offs:
            fmt = lm.info(bntx)[0]
            entries.append((name, block_size(fmt)[2], offs))
    if not entries:
        return None
    return pack(entries)


def pack(entries):
    """[(texture name, block bytes, [offsets])] -> the cap file."""
    out = [MAGIC, struct.pack("<I", len(entries))]
    for name, step, offs in entries:
        n = name.encode("ascii")
        if not 0 < len(n) < NAME_BYTES:
            raise TunicError(f"texture name '{name}' does not fit {NAME_BYTES - 1} bytes")
        out += [n.ljust(NAME_BYTES, b"\0"), struct.pack("<II", step, len(offs)), struct.pack(f"<{len(offs)}I", *offs)]
    return b"".join(out)


def unpack(blob):
    """The cap file -> [(name, block bytes, [offsets])]."""
    if blob[:8] != MAGIC:
        raise TunicError("not a cap file")
    count = struct.unpack_from("<I", blob, 8)[0]
    out, at = [], 12
    for _ in range(count):
        if at + NAME_BYTES + 8 > len(blob):
            raise TunicError("the cap file is cut short")
        name = blob[at:at + NAME_BYTES].rstrip(b"\0").decode("ascii")
        step, n = struct.unpack_from("<II", blob, at + NAME_BYTES)
        at += NAME_BYTES + 8
        if at + 4 * n > len(blob):
            raise TunicError("the cap file is cut short")
        out.append((name, step, list(struct.unpack_from(f"<{n}I", blob, at))))
        at += 4 * n
    return out
