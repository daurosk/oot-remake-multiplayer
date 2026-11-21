"""The Cap trim's blocks (puppet_pack.look_cap) and the model reader under it (puppet_pack.bfres).
Synthetic models and textures only: nothing of the game."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import bfres, look_cap as lc, look_metal as lm  # noqa: E402
from puppet_pack.tunic import TunicError  # noqa: E402

SRC = os.path.join(os.path.dirname(__file__), "..", "..", "..", "hookshot", "src")


class Out:
    """A file put together piece by piece: each piece's offset is known when it is added."""

    def __init__(self, size):
        self.b = bytearray(size)

    def add(self, data, align=8):
        while len(self.b) % align:
            self.b.append(0)
        at = len(self.b)
        self.b += data
        return at

    def string(self, s):
        return self.add(struct.pack("<H", len(s)) + s.encode() + b"\0", 2)

    def q(self, at, v):
        struct.pack_into("<Q", self.b, at, v)


def model(bones, table, materials, shapes):
    """A BFRES as bfres.Bfres reads it. `materials`: [(name, [texture names])]; `shapes`: [(name,
    material, {attribute: (format, [rows])}, [(first vertex, [indices])])]."""
    o = Out(0x100)
    o.b[0:8] = bfres.MAGIC
    struct.pack_into("<I", o.b, 8, 0x000A0400)
    vertex_data, index_data = bytearray(), bytearray()
    fmdl = o.add(bytes(0x78))
    o.b[fmdl:fmdl + 4] = b"FMDL"
    o.q(0x28, fmdl)
    dic = o.add(struct.pack("<Ii", 0, 1) + bytes(16))
    o.q(0x30, dic)
    info = o.add(bytes(0x18))
    o.q(0xB0, info)
    o.q(fmdl + 8, o.string("Player_Test"))
    # the skeleton
    fskl = o.add(bytes(0x40))
    o.b[fskl:fskl + 4] = b"FSKL"
    arr = o.add(bytes(0x58 * len(bones)))
    for k, name in enumerate(bones):
        o.q(arr + 0x58 * k, o.string(name))
    o.q(fskl + 0x10, arr)
    o.q(fskl + 0x18, o.add(struct.pack(f"<{len(table)}H", *table)))
    struct.pack_into("<HHH", o.b, fskl + 0x38, len(bones), len(table), 0)
    o.q(fmdl + 0x18, fskl)
    # the materials
    mats = o.add(bytes(0xB0 * len(materials)))
    for k, (name, textures) in enumerate(materials):
        m = mats + 0xB0 * k
        o.b[m:m + 4] = b"FMAT"
        o.q(m + 8, o.string(name))
        names = o.add(bytes(8 * len(textures)))
        for t, tex in enumerate(textures):
            o.q(names + 8 * t, o.string(tex))
        o.q(m + 0x20, names)
        o.b[m + 0xA2] = o.b[m + 0xA3] = len(textures)
    o.q(fmdl + 0x38, mats)
    # the shapes, each with its vertex buffer (one buffer, the attributes side by side)
    fvtx = o.add(bytes(0x58 * len(shapes)))
    fshp = o.add(bytes(0x60 * len(shapes)))
    sizes = {0x0B01: 4, 0x0B03: 4, 0x1202: 4, 0x1705: 8, 0x1805: 12}
    for k, (name, material, attrs, lods) in enumerate(shapes):
        v, s = fvtx + 0x58 * k, fshp + 0x60 * k
        o.b[v:v + 4] = b"FVTX"
        o.b[s:s + 4] = b"FSHP"
        names = list(attrs)
        stride = sum(sizes[attrs[n][0]] for n in names)
        count = len(next(iter(attrs.values()))[1])
        table_at = o.add(bytes(16 * len(names)))
        rows = [bytearray() for _ in range(count)]
        off = 0
        for i, n in enumerate(names):
            fmt, values = attrs[n]
            o.q(table_at + 16 * i, o.string(n))
            struct.pack_into("<HHHH", o.b, table_at + 16 * i + 8, fmt, 0, off, 0)
            code = {0x0B01: "4B", 0x0B03: "4B", 0x1202: "2h", 0x1705: "2f", 0x1805: "3f"}[fmt]
            for r, val in enumerate(values):
                rows[r] += struct.pack("<" + code, *val)
            off += sizes[fmt]
        while len(vertex_data) % 8:
            vertex_data.append(0)
        start = len(vertex_data)
        for r in rows:
            vertex_data += r
        o.q(v + 8, table_at)
        o.q(v + 0x30, o.add(struct.pack("<I", stride * count) + bytes(12)))
        o.q(v + 0x38, o.add(struct.pack("<I", stride) + bytes(12)))
        struct.pack_into("<IBBHI", o.b, v + 0x48, start, len(names), 1, k, count)
        meshes = o.add(bytes(0x38 * len(lods)))
        for j, (first, idx) in enumerate(lods):
            at = len(index_data)
            index_data += struct.pack(f"<{len(idx)}H", *idx)
            struct.pack_into("<IIIII", o.b, meshes + 0x38 * j + 0x20, at, bfres.TRIANGLES, 1, len(idx), first)
        o.q(s + 8, o.string(name))
        o.q(s + 0x10, v)
        o.q(s + 0x18, meshes)
        struct.pack_into("<HHHHHBB", o.b, s + 0x50, k, material, 0, k, 0, 4, len(lods))
    o.q(fmdl + 0x20, fvtx)
    o.q(fmdl + 0x28, fshp)
    struct.pack_into("<HHH", o.b, fmdl + 0x68, len(shapes), len(shapes), len(materials))
    # the buffers: vertices, then indices
    pool = o.add(bytes(vertex_data), 0x1000)   # (the vertex buffers' offsets are from its start)
    index_at = o.add(bytes(index_data), 8)
    for k, (_, _, _, lods) in enumerate(shapes):
        meshes = struct.unpack_from("<Q", o.b, fshp + 0x60 * k + 0x18)[0]
        for j in range(len(lods)):
            a = meshes + 0x38 * j + 0x20
            struct.pack_into("<I", o.b, a, struct.unpack_from("<I", o.b, a)[0] + index_at - pool)
    o.q(info + 8, pool)
    return bytes(o.b)


def bntx(fmt, w, h, levels):
    """A one-texture BNTX as look_metal.info reads it (block height 1 GOB), `levels` its mip images."""
    brti_at, ptrs_at = 0x40, 0x100
    data_at = ptrs_at + 8 * len(levels)
    image = b"".join(levels)
    head = bytearray(data_at)
    head[0:4] = b"BNTX"
    struct.pack_into("<Q", head, 0x28, 0x38)
    struct.pack_into("<Q", head, 0x38, brti_at)
    head[brti_at:brti_at + 4] = b"BRTI"
    struct.pack_into("<H", head, brti_at + 0x16, len(levels))
    struct.pack_into("<IIIIIII", head, brti_at + 0x1C, fmt << 8 | 1, 0, w, h, 1, 1, 0)
    struct.pack_into("<I", head, brti_at + 0x50, len(image))
    struct.pack_into("<Q", head, brti_at + 0x70, ptrs_at)
    at = data_at
    for k, level in enumerate(levels):
        struct.pack_into("<Q", head, ptrs_at + 8 * k, at)
        at += len(level)
    return bytes(head) + image


BONES = ["Root", "Head", "Spine", "Ph_Cap_1", "Ph_HatFringe_2_L"]
CAP_W = (3, 0, 0, 0), (255, 0, 0, 0)     # Ph_Cap_1 alone
HEAD_W = (1, 2, 0, 0), (200, 55, 0, 0)   # mostly the head: the cap's (the hood)
BODY_W = (3, 2, 0, 0), (100, 155, 0, 0)  # mostly the spine: not the cap's
UNORM8, UINT8, SNORM16, FLOAT32 = 0x0B01, 0x0B03, 0x1202, 0x1705


def uv16(u, v):
    return (round(u * 32767), round(v * 32767))


def test_model():
    """A 16x16 texture's coordinates: cap triangles over blocks (0, 0) and (0, 1), a wrapped one
    over (3, 0), body triangles over (0, 1) and (2..3, 2..3); a second level of detail (its indices
    from its first vertex) with a cap triangle over (1, 0); a mixed triangle; and a shape of another
    material whose cap triangle is not counted."""
    verts = []   # (uv, weights)

    def tri(points, w):
        base = len(verts)
        verts.extend((p, w) for p in points)
        return [base, base + 1, base + 2]

    lod0 = []
    lod0 += tri([(0, 0), (0.25, 0), (0, 0.25)], CAP_W)                # block (0, 0)
    lod0 += tri([(0, 0.25), (0.25, 0.25), (0, 0.5)], HEAD_W)          # block (0, 1), the cap's...
    lod0 += tri([(0.25, 0.5), (0, 0.5), (0.25, 0.25)], BODY_W)        # ...but the body's too
    lod0 += tri([(-0.25, 0), (0, 0), (-0.25, 0.25)], CAP_W)           # wraps to block (3, 0)
    lod0 += tri([(0.5, 0.5), (1.0, 0.5), (0.5, 1.0)], BODY_W)         # blocks (2..3, 2..3)
    mixed = tri([(0.5, 0.75), (0.75, 0.75), (0.5, 1.0)], CAP_W)       # block (2, 3), one vertex the body's
    verts[mixed[2]] = (verts[mixed[2]][0], BODY_W)
    lod0 += mixed
    first = len(verts)
    lod1 = [i - first for i in tri([(0.25, 0), (0.5, 0), (0.25, 0.25)], CAP_W)]   # block (1, 0)
    attrs = {"_u0": (FLOAT32, [v[0] for v in verts]), "_i0": (UINT8, [v[1][0] for v in verts]),
             "_w0": (UNORM8, [v[1][1] for v in verts])}
    other = [(uv16(0.75, 0.75), CAP_W), (uv16(1.0, 0.75), CAP_W), (uv16(0.75, 1.0), CAP_W)]
    other_attrs = {"_u0": (SNORM16, [v[0] for v in other]), "_i0": (UINT8, [v[1][0] for v in other]),
                   "_w0": (UNORM8, [v[1][1] for v in other])}
    return model(BONES, [0, 1, 2, 3, 4], [("Mt_Test", ["Tex_A", "Tex_A_Nrm"]), ("Mt_Other", ["Tex_B"])],
                 [("Hood__Mt_Test", 0, attrs, [(0, lod0), (first, lod1)]),
                  ("Hair__Mt_Other", 1, other_attrs, [(0, [0, 1, 2])])])


def texture():
    """Tex_A: BC1, 16x16 and its 8x8 level, one GOB each."""
    return bntx(lm.BC1, 16, 16, [bytes(range(256)) * 2, bytes(512)])


def blocks_at(cells, wide, rows):
    return sorted(lm.block_at(x, y, wide * 8, rows, 1, 8) for x, y in cells)


class BfresTest(unittest.TestCase):

    def test_reads_the_model(self):
        m = bfres.Bfres(test_model())
        self.assertEqual(m.model.name, "Player_Test")
        self.assertEqual(m.model.bones, BONES)
        self.assertEqual(m.model.matrix_bones, [0, 1, 2, 3, 4])
        self.assertEqual([x.name for x in m.model.materials], ["Mt_Test", "Mt_Other"])
        self.assertEqual(m.model.materials[0].textures, ["Tex_A", "Tex_A_Nrm"])
        hood, hair = m.model.shapes
        self.assertEqual((hood.name, hood.material, len(hood.meshes)), ("Hood__Mt_Test", 0, 2))
        self.assertEqual(hair.material, 1)
        uv = m.attribute(hood, "_u0")
        self.assertEqual(uv[1], (0.25, 0.0))
        self.assertEqual(uv[9][0], -0.25)
        self.assertAlmostEqual(m.attribute(hair, "_u0")[1][0], 1.0, places=4)   # (snorm16, as the game's)
        self.assertEqual(m.attribute(hood, "_i0")[0], (3, 0, 0, 0))
        self.assertAlmostEqual(m.attribute(hood, "_w0")[3][0], 200 / 255)
        self.assertIsNone(m.attribute(hood, "_u1"))
        tris = m.triangles(hood)
        self.assertEqual(len(tris), 7)
        self.assertEqual(tris[-1], (18, 19, 20))   # the second level's indices from its first vertex
        self.assertEqual(m.triangles(hood, lods=[1]), [(18, 19, 20)])

    def test_refuses_what_it_does_not_read(self):
        with self.assertRaises(bfres.BfresError):
            bfres.Bfres(b"FRES    " + bytes(0x100))   # version 0
        with self.assertRaises(bfres.BfresError):
            bfres.Bfres(b"BNTX" + bytes(0x100))
        data = bytearray(test_model())
        with self.assertRaises(bfres.BfresError):
            bfres.Bfres(bytes(data[:0x400]))   # cut short


class LookCapTest(unittest.TestCase):

    def test_cap_bones(self):
        for name in ("Ph_Cap_1", "Ph_HatFringe_2_L", "Ph_HatString_3", "Head", "Head_2"):
            self.assertTrue(lc.is_cap_bone(name), name)
        for name in ("Spine_2", "Ph_Skirt_2_Back", "Headlight", "Root", ""):
            self.assertFalse(lc.is_cap_bone(name), name)

    def test_vertices_by_their_heaviest_bone(self):
        m = bfres.Bfres(test_model())
        bones = lc.vertex_bones(m, m.model.shapes[0])
        self.assertEqual(bones[0], "Ph_Cap_1")
        self.assertEqual(bones[3], "Head")
        self.assertEqual(bones[6], "Spine")

    def test_triangles_of_the_texture_only(self):
        m = bfres.Bfres(test_model())
        cap, other, shapes = lc.uv_triangles(m, "Tex_A")
        self.assertEqual(shapes, ["Hood__Mt_Test"])   # Mt_Other samples no Tex_A
        self.assertEqual((len(cap), len(other)), (4, 3))   # the mixed triangle is not the cap's
        self.assertEqual(lc.uv_triangles(m, "Tex_C"), ([], [], []))

    def test_overlap_needs_area(self):
        t = ((0, 0), (4, 0), (0, 4))
        self.assertTrue(lc._overlaps(t, 0, 0, 4, 4))
        self.assertFalse(lc._overlaps(t, 4, 0, 8, 4))   # touching an edge only
        self.assertFalse(lc._overlaps(t, 2.5, 2.5, 4, 4))   # beyond the long edge
        self.assertTrue(lc._overlaps(t, 1, 1, 3, 3))
        self.assertFalse(lc._overlaps(((0, 0), (1, 1), (2, 2)), 0, 0, 4, 4))   # no area

    def test_covered_blocks_wrap(self):
        self.assertEqual(lc.covered([((0, 0), (0.25, 0), (0, 0.25))], 16, 16, 4, 4), {(0, 0)})
        self.assertEqual(lc.covered([((-0.25, 0), (0, 0), (-0.25, 0.25))], 16, 16, 4, 4), {(3, 0)})
        self.assertEqual(lc.covered([((1.0, 2.0), (1.25, 2.0), (1.0, 2.25))], 16, 16, 4, 4), {(0, 0)})
        # a block size that does not divide the texture (ASTC 8x5 over 16 rows): the last row is partial
        self.assertEqual(lc.covered([((0, 0.9), (0.4, 0.9), (0, 1.0))], 16, 16, 8, 5), {(0, 2), (0, 3)})

    def test_blocks_every_level(self):
        m = bfres.Bfres(test_model())
        cap, other, _ = lc.uv_triangles(m, "Tex_A")
        offs, first = lc.blocks(texture(), cap, other)
        level0 = blocks_at([(0, 0), (1, 0), (3, 0)], 4, 4)
        level1 = [512 + a for a in blocks_at([(1, 0)], 2, 2)]   # (0, 0) holds the body's too
        self.assertEqual(offs, sorted(level0 + level1))
        self.assertEqual(first, (3, 16))

    def test_cap_file(self):
        logs = []
        tex = texture()
        blob = lc.cap(lambda n: tex if n == "Mt_Cloth_Parts_Player_Child_Alb" else None,
                      lambda v: test_model() if v == "Child" else None, logs.append)
        self.assertIsNone(blob)   # the test model's material samples Tex_A, not the game's name
        self.assertTrue(any("0 of 16 blocks" in line for line in logs), logs)
        self.assertTrue(any("no Mt_Vest_Player_Adult_Alb" in line for line in logs), logs)
        # The texture under the name a material of the model samples.
        old = lc.CAP
        try:
            lc.CAP = (("Tex_A", "Child"),)
            blob = lc.cap(lambda n: tex, lambda v: test_model(), logs.append)
        finally:
            lc.CAP = old
        entries = lc.unpack(blob)
        self.assertEqual(len(entries), 1)
        name, step, offs = entries[0]
        self.assertEqual((name, step), ("Tex_A", 8))
        self.assertEqual(len(offs), 4)
        self.assertTrue(any("3 of 16 blocks hold only the cap (18.8%" in line for line in logs), logs)
        # a model that cannot be read: said, nothing made
        logs.clear()
        self.assertIsNone(lc.cap(lambda n: tex, lambda v: b"FRES    " + bytes(0x100), logs.append))
        self.assertTrue(any("is not read" in line for line in logs), logs)

    def test_pack_round_trip(self):
        entries = [("Mt_Vest_Player_Adult_Alb", 16, [0, 16, 4096]), ("Mt_Angin_Acs_Player_Adult_Alb", 8, [])]
        blob = lc.pack(entries)
        self.assertEqual(blob[:8], b"COOPCAP1")
        self.assertEqual(lc.unpack(blob), entries)
        with self.assertRaises(TunicError):
            lc.unpack(blob[:-1])
        with self.assertRaises(TunicError):
            lc.unpack(b"COOPMTL1" + blob[8:])

    def test_the_module_paints_these_textures(self):
        # Each texture is a tunic texture of the module's (green.bin holds its texels), painted in green.
        with open(os.path.join(SRC, "coop_look_parts.cpp"), encoding="utf-8") as f:
            src = f.read()
        for name in lc.TEXTURES:
            self.assertIn(f'{{"{name}", Format::', src)
        self.assertTrue('"COOPCAP1"' in src, "the module reads cap.bin (CoopLookParts::ParseCap)")


if __name__ == "__main__":
    unittest.main()
