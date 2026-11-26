"""A small reader of the game's model files (BFRES version 10, `Model/*.bfres`): what the pack needs of
a model, read only, from the user's own romfs.

What it reads of the first model: its skeleton's bone names and the table from a vertex's skin index
to a bone, its materials (name, the textures they sample) and its shapes: the material, the vertex
attributes (`_p0`, `_u0`, `_i0`, `_w0` ...) and the triangles of every level of detail.

The layout (little-endian, every pointer a u64 offset from the file's start):

- header: "FRES    ", u32 version (0x000A....: 10), +0x28 the model array, +0x30 its dictionary
  (u32 0, s32 count), +0xB0 the buffer info (+0x08: where the vertex and index data start);
- model "FMDL": +0x08 name, +0x18 skeleton, +0x20 vertex buffers (0x58 each), +0x28 shapes (0x60
  each), +0x38 materials (0xB0 each), u16 counts at +0x68 (vertex buffers), +0x6A (shapes), +0x6C
  (materials);
- skeleton "FSKL": +0x10 bones (0x58 each, the name first), +0x18 the skin index -> bone table (u16),
  u16 bone count at +0x38, then the smooth and rigid skin counts (the table's length);
- material "FMAT": +0x08 name, +0x20 the texture names, u8 count at +0xA3;
- shape "FSHP": +0x08 name, +0x10 its vertex buffer, +0x18 its meshes (0x38 each, one per level of
  detail), u16 material at +0x52, u16 bone at +0x54, u8 skin count at +0x5A, u8 mesh count at +0x5B;
- mesh: u32 index data offset (+0x20, from the buffer start), u32 primitive (3: triangles), u32 index
  size (0 u8, 1 u16, 2 u32), u32 index count, u32 first vertex (added to every index);
- vertex buffer "FVTX": +0x08 attributes (16 bytes each: name, u16 format, u16 offset at +0x0C, u16
  buffer at +0x0E), +0x30 buffer sizes, +0x38 strides (16 bytes each, u32 first), u32 data offset at
  +0x48 (the buffers one after another, each 8-aligned), u8 attribute count +0x4C, u8 buffer count
  +0x4D, u32 vertex count +0x50.

A strings' entry: u16 length, then the characters.
"""
import struct

MAGIC = b"FRES    "
TRIANGLES = 3

# An attribute format's high byte: (components, bytes each); its low byte the kind.
_LAYOUT = {0x02: (1, 1), 0x09: (2, 1), 0x0B: (4, 1), 0x12: (2, 2), 0x15: (4, 2), 0x17: (2, 4), 0x18: (3, 4),
           0x19: (4, 4)}
UNORM, SNORM, UINT, SINT, FLOAT = 1, 2, 3, 4, 5
_CODE = {(1, UNORM): "B", (1, SNORM): "b", (1, UINT): "B", (1, SINT): "b", (2, UNORM): "H", (2, SNORM): "h",
         (2, UINT): "H", (2, SINT): "h", (2, FLOAT): "e", (4, UNORM): "I", (4, SNORM): "i", (4, UINT): "I",
         (4, SINT): "i", (4, FLOAT): "f"}


class BfresError(ValueError):
    pass


class Material:
    def __init__(self, name, textures):
        self.name = name
        self.textures = textures


class Shape:
    def __init__(self, name, material, bone, skin_count, vertices, meshes):
        self.name = name
        self.material = material      # index in Model.materials
        self.bone = bone              # its bone (a shape with no skin index follows it)
        self.skin_count = skin_count  # skin indices per vertex (0: none)
        self.vertices = vertices      # its vertex buffer's offset in the file
        self.meshes = meshes          # [(index offset, primitive, index size, count, first vertex)]


class Model:
    def __init__(self, name, bones, matrix_bones, materials, shapes):
        self.name = name
        self.bones = bones                # bone names
        self.matrix_bones = matrix_bones  # a skin index -> its bone's number
        self.materials = materials
        self.shapes = shapes


class Bfres:
    def __init__(self, data):
        self.data = data
        if len(data) < 0xC0 or data[:8] != MAGIC:
            raise BfresError("not a model file (BFRES)")
        version = self._u32(8)
        if version >> 16 != 10:
            raise BfresError(f"model file version {version >> 16}.{version >> 8 & 0xFF} is not read (10 is)")
        self.pool = self._q(self._q(0xB0) + 8)
        if self._u32(self._q(0x30) + 4) < 1:
            raise BfresError("the model file has no model")
        self.model = self._model(self._q(0x28))

    # ---- raw reads, every one checked against the file's end ----

    def _at(self, at, size):
        if not 0 <= at <= len(self.data) - size:
            raise BfresError(f"the model file is cut short (0x{at:x})")
        return at

    def _q(self, at):
        return struct.unpack_from("<Q", self.data, self._at(at, 8))[0]

    def _u32(self, at):
        return struct.unpack_from("<I", self.data, self._at(at, 4))[0]

    def _u16(self, at):
        return struct.unpack_from("<H", self.data, self._at(at, 2))[0]

    def _u8(self, at):
        return self.data[self._at(at, 1)]

    def _str(self, at):
        n = self._u16(at)
        return self.data[self._at(at + 2, n):at + 2 + n].decode("ascii", "replace")

    def _magic(self, at, magic):
        if self.data[self._at(at, 4):at + 4] != magic:
            raise BfresError(f"no {magic.decode()} at 0x{at:x}")

    # ---- the parts ----

    def _model(self, at):
        self._magic(at, b"FMDL")
        sk = self._q(at + 0x18)
        self._magic(sk, b"FSKL")
        bones_at = self._q(sk + 0x10)
        bones = [self._str(self._q(bones_at + 0x58 * k)) for k in range(self._u16(sk + 0x38))]
        table_len = self._u16(sk + 0x3A) + self._u16(sk + 0x3C)
        table_at = self._q(sk + 0x18)
        matrix_bones = list(struct.unpack_from(f"<{table_len}H", self.data, self._at(table_at, 2 * table_len))) \
            if table_len else []
        materials = []
        mats = self._q(at + 0x38)
        for k in range(self._u16(at + 0x6C)):
            m = mats + 0xB0 * k
            self._magic(m, b"FMAT")
            names = self._q(m + 0x20)
            textures = [self._str(self._q(names + 8 * t)) for t in range(self._u8(m + 0xA3))] if names else []
            materials.append(Material(self._str(self._q(m + 8)), textures))
        shapes = []
        shp = self._q(at + 0x28)
        for k in range(self._u16(at + 0x6A)):
            s = shp + 0x60 * k
            self._magic(s, b"FSHP")
            meshes_at = self._q(s + 0x18)
            meshes = [struct.unpack_from("<IIIII", self.data, self._at(meshes_at + 0x38 * j + 0x20, 20))
                      for j in range(self._u8(s + 0x5B))]
            vertices = self._q(s + 0x10)
            self._magic(vertices, b"FVTX")
            shapes.append(Shape(self._str(self._q(s + 8)), self._u16(s + 0x52), self._u16(s + 0x54),
                                self._u8(s + 0x5A), vertices, meshes))
        return Model(self._str(self._q(at + 8)), bones, matrix_bones, materials, shapes)

    def vertex_count(self, shape):
        return self._u32(shape.vertices + 0x50)

    def attributes(self, shape):
        """{name: (format, offset in the vertex, buffer)} of a shape's vertex buffer."""
        v = shape.vertices
        arr = self._q(v + 8)
        out = {}
        for k in range(self._u8(v + 0x4C)):
            a = arr + 16 * k
            out[self._str(self._q(a))] = (self._u16(a + 8), self._u16(a + 0x0C), self._u16(a + 0x0E))
        return out

    def attribute(self, shape, name):
        """A vertex attribute of a shape, one tuple per vertex (normalized formats as -1..1 or 0..1);
        None when the shape has no such attribute."""
        found = self.attributes(shape).get(name)
        if found is None:
            return None
        fmt, offset, buf = found
        v = shape.vertices
        count = self._u32(v + 0x50)
        nbuf = self._u8(v + 0x4D)
        if buf >= nbuf:
            raise BfresError(f"{shape.name}: attribute {name} names buffer {buf} of {nbuf}")
        sizes, strides = self._q(v + 0x30), self._q(v + 0x38)
        at = self.pool + self._u32(v + 0x48)
        for k in range(buf):
            at = (at + self._u32(sizes + 16 * k) + 7) & ~7
        stride = self._u32(strides + 16 * buf)
        layout = _LAYOUT.get(fmt >> 8)
        code = _CODE.get((layout[1], fmt & 0xFF)) if layout else None
        if code is None:
            raise BfresError(f"{shape.name}: attribute {name} format 0x{fmt:04x} is not read")
        n, size = layout
        if offset + n * size > stride:
            raise BfresError(f"{shape.name}: attribute {name} does not fit its vertex")
        self._at(at, stride * count)
        rec = struct.Struct(f"<{offset}x{n}{code}{stride - offset - n * size}x")
        rows = rec.iter_unpack(self.data[at:at + stride * count])
        kind = fmt & 0xFF
        if kind == UNORM:
            scale = float((1 << (8 * size)) - 1)
            return [tuple(x / scale for x in r) for r in rows]
        if kind == SNORM:
            scale = float((1 << (8 * size - 1)) - 1)
            return [tuple(max(-1.0, x / scale) for x in r) for r in rows]
        return list(rows)

    def triangles(self, shape, lods=None):
        """The triangles (vertex numbers) of a shape's levels of detail (all, or those listed)."""
        out = []
        for j, (offset, prim, isize, count, first) in enumerate(shape.meshes):
            if lods is not None and j not in lods:
                continue
            if prim != TRIANGLES:
                raise BfresError(f"{shape.name}: mesh {j} is not triangles (primitive {prim})")
            code = {0: "B", 1: "H", 2: "I"}.get(isize)
            if code is None or count % 3:
                raise BfresError(f"{shape.name}: mesh {j} has index size {isize}, count {count}")
            width = struct.calcsize(code)
            at = self._at(self.pool + offset, width * count)
            idx = struct.unpack_from(f"<{count}{code}", self.data, at)
            out += [(idx[k] + first, idx[k + 1] + first, idx[k + 2] + first) for k in range(0, count, 3)]
        return out
