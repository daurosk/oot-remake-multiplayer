"""Tests for the co-op HUD files (hud.py, texture.py). Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_hud.py
"""
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import hud, sarc, texture  # noqa: E402


def swizzle(linear, width_bytes, rows, bh_log2):
    """The inverse of texture.deswizzle, for the tests."""
    gobs_tall = 1 << bh_log2
    gobs_wide = (width_bytes + 63) // 64
    blocks_tall = (rows + 8 * gobs_tall - 1) // (8 * gobs_tall)
    out = bytearray(gobs_wide * 512 * gobs_tall * blocks_tall)
    for y in range(rows):
        for x in range(width_bytes):
            a = ((y // (8 * gobs_tall)) * 512 * gobs_tall * gobs_wide + (x // 64) * 512 * gobs_tall
                 + ((y % (8 * gobs_tall)) // 8) * 512 + texture.gob_offset(x, y))
            out[a] = linear[y * width_bytes + x]
    return bytes(out)


def bc4(r0, r1, indices):
    bits = 0
    for i, v in enumerate(indices):
        bits |= v << (3 * i)
    return bytes((r0, r1)) + bits.to_bytes(6, "little")


def make_bntx(textures):
    """A minimal "Ounc" .bntx: [(name, format, width, height, bh_log2, data, channels)]."""
    n = len(textures)
    table = 0x40
    brti_at = table + 8 * n
    brti_size = 0x80
    names_at = brti_at + brti_size * n
    names = b""
    name_offs = []
    for t in textures:
        name_offs.append(names_at + len(names))
        nb = t[0].encode()
        names += struct.pack("<H", len(nb)) + nb + b"\0" * ((-len(nb) - 2) % 8)
    ptrs_at = names_at + len(names)
    data_at = ptrs_at + 8 * n
    data = b""
    data_offs = []
    for t in textures:
        data_offs.append(data_at + len(data))
        data += t[5] + b"\0" * ((-len(t[5])) % 16)
    b = bytearray(data_at + len(data))
    b[0:4] = b"BNTX"
    b[0x20:0x24] = b"Ounc"
    struct.pack_into("<IQ", b, 0x24, n, table)
    for i, (name, fmt, w, h, bh, d, ch) in enumerate(textures):
        p = brti_at + brti_size * i
        struct.pack_into("<Q", b, table + 8 * i, p)
        b[p:p + 4] = b"BRTI"
        struct.pack_into("<H", b, p + 0x16, 1)
        struct.pack_into("<IIIIIII", b, p + 0x1C, fmt, 0, w, h, 1, 1, bh)
        struct.pack_into("<I", b, p + 0x50, len(d))
        struct.pack_into("<I", b, p + 0x58, ch[0] | (ch[1] << 8) | (ch[2] << 16) | (ch[3] << 24))
        struct.pack_into("<Q", b, p + 0x60, name_offs[i])
        struct.pack_into("<Q", b, p + 0x70, ptrs_at + 8 * i)
        struct.pack_into("<Q", b, ptrs_at + 8 * i, data_offs[i])
    b[names_at:names_at + len(names)] = names
    b[data_at:] = data
    return bytes(b)


def make_msbt(entries, utf16=False):
    """An MSBT with one label group per entry, LBL1 + TXT2."""
    labels = list(entries)
    lbl = struct.pack("<I", len(labels))
    groups = b""
    body_off = 4 + 8 * len(labels)
    for i, label in enumerate(labels):
        lb = label.encode()
        lbl += struct.pack("<II", 1, body_off + len(groups))
        groups += bytes((len(lb),)) + lb + struct.pack("<I", i)
    lbl += groups
    texts = [(entries[k].encode("utf-16-le") + b"\0\0") if utf16 else (entries[k].encode() + b"\0")
             for k in labels]
    txt = struct.pack("<I", len(texts))
    off = 4 + 4 * len(texts)
    for t in texts:
        txt += struct.pack("<I", off)
        off += len(t)
    txt += b"".join(texts)

    def section(magic, body):
        return magic + struct.pack("<I", len(body)) + b"\0" * 8 + body + b"\xab" * ((-len(body)) % 16)

    secs = section(b"LBL1", lbl) + section(b"TXT2", txt)
    head = bytearray(0x20)
    head[0:8] = b"MsgStdBn"
    head[8:10] = b"\xff\xfe"
    head[0x0C] = 1 if utf16 else 0
    struct.pack_into("<H", head, 0x0E, 2)
    return bytes(head) + secs


def obfuscate_font(plain):
    key = hud.FONT_KEY * ((len(plain) + 3) // 4)
    size = bytes(x ^ k for x, k in zip(len(plain).to_bytes(4, "big"), hud.FONT_KEY))
    return b"\x00" * 4 + size + bytes(x ^ k for x, k in zip(plain, key))


class SwizzleTest(unittest.TestCase):
    def test_gob_offset_covers_the_gob(self):
        seen = {texture.gob_offset(x, y) for x in range(64) for y in range(8)}
        self.assertEqual(seen, set(range(512)))

    def test_deswizzle_undoes_swizzle(self):
        for width_bytes, rows, bh in ((192, 24, 2), (64, 8, 0), (384, 40, 3), (48, 5, 1)):
            linear = bytes((i * 7 + 3) & 0xFF for i in range(width_bytes * rows))
            self.assertEqual(texture.deswizzle(swizzle(linear, width_bytes, rows, bh), width_bytes, rows, bh),
                             linear)


class Bc4Test(unittest.TestCase):
    def test_eight_step_palette(self):
        px = texture.bc4_block(bc4(255, 0, [0, 1, 2, 7] + [0] * 12))
        self.assertEqual([p[0] for p in px[:4]], [255, 0, 218, 36])
        self.assertEqual(px[0], (255, 255, 255, 255))

    def test_six_step_palette_has_black_and_white(self):
        px = texture.bc4_block(bc4(10, 200, [6, 7] + [0] * 14))
        self.assertEqual((px[0][0], px[1][0]), (0, 255))


class AstcTest(unittest.TestCase):
    def test_void_extent_is_one_colour(self):
        v = 0x1FC | (0x3 << 10) | (0x1FFF << 12) | (0x1FFF << 25) | (0x1FFF << 38) | (0x1FFF << 51)
        for k, c in enumerate((0x1234, 0xFF00, 0x0080, 0xFFFF)):
            v |= c << (64 + 16 * k)
        self.assertEqual(texture.astc_block(v.to_bytes(16, "little")), [(0x12, 0xFF, 0x00, 0xFF)] * 16)

    def test_luminance_block_with_a_4x2_weight_grid(self):
        # Block mode 1: weights 4 wide, 2 tall, one bit each; one partition, colour mode 0
        # (luminance), endpoints 0 and 255 as 8-bit values from bit 17.
        v = 1 | (0 << 17) | (255 << 25)
        weights = [1, 1, 1, 1, 0, 0, 0, 0]  # top row on, bottom row off
        for i, w in enumerate(weights):
            v |= w << (127 - i)  # stored from bit 127 down
        px = texture.astc_block(v.to_bytes(16, "little"))
        self.assertEqual([p[0] for p in px[0:4]], [255] * 4)
        self.assertEqual([p[0] for p in px[12:16]], [0] * 4)
        self.assertTrue(0 < px[4][0] < 255)
        self.assertEqual(px[0][3], 255)

    def test_reserved_block_mode_is_magenta(self):
        self.assertEqual(texture.astc_block(bytes(16))[0], (255, 0, 255, 255))

    def test_trit_and_quint_tables_round_trip(self):
        # Every packed trit/quint byte decodes to digits in range.
        for T in range(256):
            self.assertTrue(all(0 <= d <= 2 for d in texture._trits(T)))
        for Q in range(128):
            self.assertTrue(all(0 <= d <= 4 for d in texture._quints(Q)))
        self.assertEqual(len({texture._trits(T) for T in range(256)}), 243)
        self.assertEqual(len({texture._quints(Q) for Q in range(128)}), 125)


class DecodeTest(unittest.TestCase):
    def test_bc4_texture_through_bntx(self):
        w = h = 8
        blocks = [bc4(255, 255, [0] * 16), bc4(0, 0, [0] * 16), bc4(128, 128, [0] * 16), bc4(255, 0, [1] * 16)]
        linear = b"".join(blocks)  # 2 x 2 blocks, row by row
        data = swizzle(linear, 2 * 8, 2, 0)
        b = make_bntx([("Mask", 0x1D01, w, h, 0, data, (1, 1, 1, 2))])
        t = texture.read_bntx(b)["Mask"]
        px = texture.decode(t)
        self.assertEqual(px[0:4], bytes((255, 255, 255, 255)))  # alpha from red
        self.assertEqual(px[4 * 4:4 * 4 + 4], bytes((255, 255, 255, 0)))
        self.assertEqual(px[(4 * w) * 4 + 3], 128)
        self.assertEqual(px[(4 * w + 4) * 4 + 3], 0)

    def test_png_is_readable(self):
        p = texture.png(2, 1, bytes((1, 2, 3, 4, 5, 6, 7, 8)))
        self.assertEqual(p[:8], b"\x89PNG\r\n\x1a\n")
        idat = p.index(b"IDAT")
        n = struct.unpack(">I", p[idat - 4:idat])[0]
        self.assertEqual(zlib.decompress(p[idat + 4:idat + 4 + n]), b"\0" + bytes(range(1, 9)))


class FontTest(unittest.TestCase):
    def test_unpacks_the_font(self):
        plain = b"OTTO" + bytes(range(37))
        self.assertEqual(hud.deobfuscate_font(obfuscate_font(plain)), plain)

    def test_refuses_what_is_no_font(self):
        with self.assertRaises(hud.HudError):
            hud.deobfuscate_font(obfuscate_font(b"NOPE" + bytes(8)))
        with self.assertRaises(hud.HudError):
            hud.deobfuscate_font(obfuscate_font(b"OTTO" + bytes(8))[:-1])


class PlacesTest(unittest.TestCase):
    def test_msbt_utf8_and_utf16(self):
        for utf16 in (False, True):
            got = hud.parse_msbt(make_msbt({"FieldA": "Field A", "TownB": "Town é B"}, utf16))
            self.assertEqual(got, {"FieldA": "Field A", "TownB": "Town é B"})

    def test_scene_base(self):
        self.assertEqual(hud.scene_base("KokiriForest_Child"), "KokiriForest")
        self.assertEqual(hud.scene_base("KakarikoVillage_Adult_Alternative"), "KakarikoVillage")
        self.assertEqual(hud.scene_base("GohmaStage"), "GohmaStage")

    def test_folder_label_names_its_scenes(self):
        got = hud.place_names({"FieldA": "Field A", "Rush": "Hall"},
                              {"Rush": ["BossStage", "Other_Child"], "NoLabel": ["X"]})
        self.assertEqual(got, {"FieldA": "Field A", "Rush": "Hall", "BossStage": "Hall", "Other": "Hall"})


class BuildTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="hud-test-")
        self.romfs = os.path.join(self.tmp, "romfs")
        self.out = os.path.join(self.tmp, "out")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write(self, rel, data):
        p = os.path.join(self.romfs, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    def make_game(self, with_hearts=True):
        self.write("Pack/ZsDic.pack.zs", sarc.write({}))
        for font in hud.FONTS.values():
            self.write(f"{hud.FONT_DIR}/{font}.Nin_Ounce_NVN2.bfotf.zs", obfuscate_font(b"OTTO" + font.encode()))
        full = swizzle(b"".join(bc4(255, 255, [0] * 16) for _ in range(4)), 16, 2, 0)
        tex = [(n, 0x1D01, 8, 8, 0, full, (2, 2, 2, 2)) for n in hud.TEXTURES if with_hearts or n != hud.HEART_FULL]
        self.write("UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs", make_bntx(tex))
        mals = sarc.write({hud.LOCATIONS: make_msbt({"FieldA": "Field A", "Rush": "Hall"})})
        self.write("Mals/USen.Product.110.sarc.zs", mals)
        self.write("Banc/Rush/BossStage_Static.bcett.byml.zs", b"")
        self.write("Banc/FieldA/FieldA_Child_Static.bcett.byml.zs", b"")

    def test_builds_everything(self):
        self.make_game()
        made = hud.build(hud.DirReader(self.romfs), self.out, log=lambda *_: None)
        self.assertEqual(made["hearts"], [f"heart-{q}.png" for q in range(5)])
        with open(os.path.join(self.out, "font-regular.otf"), "rb") as f:
            self.assertEqual(f.read(), b"OTTO" + hud.FONTS["font-regular.otf"].encode())
        with open(os.path.join(self.out, "places.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f), {"FieldA": "Field A", "Rush": "Hall", "BossStage": "Hall"})
        with open(os.path.join(self.out, "hud.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["format"], hud.FORMAT)
        for q in range(5):
            self.assertTrue(os.path.getsize(os.path.join(self.out, f"heart-{q}.png")) > 60)

    def test_missing_picture_is_an_error(self):
        self.make_game(with_hearts=False)
        with self.assertRaises(hud.HudError):
            hud.build(hud.DirReader(self.romfs), self.out, log=lambda *_: None)

    def test_compose_hearts_fills_by_quarters(self):
        w = h = 2
        solid = bytes((200, 10, 10, 255)) * 4
        left = bytes((0, 0, 0, 255, 0, 0, 0, 0)) * 2  # a mask: left column only
        px = {hud.HEART_FULL: solid, hud.HEART_BASE: bytes((128, 128, 128, 255)) * 4,
              hud.HEART_SHADOW: bytes(16)}
        for n in hud.HEART_QUARTERS:
            px[n] = left
        hearts = hud.compose_hearts(px, w, h)
        self.assertEqual(hearts[4][0:3], bytes((200, 10, 10)))
        self.assertNotEqual(hearts[0][0:3], bytes((200, 10, 10)))
        self.assertEqual(hearts[2][0:3], bytes((200, 10, 10)))  # left filled
        self.assertEqual(hearts[2][4:7], hearts[0][4:7])          # right empty


if __name__ == "__main__":
    unittest.main()
