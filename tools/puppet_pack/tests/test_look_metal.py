"""The metal parts' metalness blocks (#202, puppet_pack.look_metal)."""
import os
import random
import re
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import look_live, look_metal as lm, texture as tx, tunic  # noqa: E402

SRC = os.path.join(os.path.dirname(__file__), "..", "..", "..", "hookshot", "src")


def full_bntx(fmt, w, h, bh, levels):
    """A one-texture BNTX as look_metal.info reads it: the texture table at +0x28, a BRTI with the
    mip count, format, size, block height and the mip pointers (+0x70), then the levels."""
    brti_at, ptrs_at = 0x40, 0x100
    data_at = ptrs_at + 8 * len(levels)
    image = b"".join(levels)
    head = bytearray(data_at)
    head[0:4] = b"BNTX"
    struct.pack_into("<Q", head, 0x28, 0x38)
    struct.pack_into("<Q", head, 0x38, brti_at)
    head[brti_at:brti_at + 4] = b"BRTI"
    struct.pack_into("<H", head, brti_at + 0x16, len(levels))
    struct.pack_into("<IIIIIII", head, brti_at + 0x1C, fmt << 8 | 1, 0, w, h, 1, 1, bh)
    struct.pack_into("<I", head, brti_at + 0x50, len(image))
    struct.pack_into("<Q", head, brti_at + 0x70, ptrs_at)
    at = data_at
    for k, level in enumerate(levels):
        struct.pack_into("<Q", head, ptrs_at + 8 * k, at)
        at += len(level)
    return bytes(head) + image


def swizzle(blocks_by_row, block_bytes, gobs_tall):
    """Rows of blocks laid out block-linear (the inverse of texture.deswizzle) through block_at."""
    rows, wide = len(blocks_by_row), len(blocks_by_row[0])
    width_bytes = wide * block_bytes
    gobs_wide = (width_bytes + 63) // 64
    size = gobs_wide * 512 * gobs_tall * ((rows + 8 * gobs_tall - 1) // (8 * gobs_tall))
    out = bytearray(size)
    for y, row in enumerate(blocks_by_row):
        for x, blk in enumerate(row):
            a = lm.block_at(x, y, width_bytes, rows, gobs_tall, block_bytes)
            out[a:a + block_bytes] = blk
    return bytes(out)


GREY = struct.pack("<HHI", 0x8410, 0x8410, 0)      # both ends (132, 130, 132)
BLUE = struct.pack("<HHI", 0x001F, 0x0010, 0)      # blue ends
GOLDGREY = struct.pack("<HHI", 0xFE60, 0x8410, 0)  # a gold end beside a grey one
WARMGREY = struct.pack("<HHI", 0xB551, 0x8410, 0)  # a pale warm grey end (181, 170, 140) beside a grey one
METAL = bytes([200, 120, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24])
NONE = bytes(8)


class LookMetalTest(unittest.TestCase):

    def test_tones_as_the_module(self):
        self.assertEqual(lm.tone_of((130, 130, 132)), lm.GREY)
        self.assertEqual(lm.tone_of((10, 12, 9)), lm.BLACK)
        self.assertEqual(lm.tone_of((30, 40, 200)), lm.BLUE)
        self.assertEqual(lm.tone_of((200, 30, 30)), lm.RED)
        self.assertEqual(lm.tone_of((240, 200, 30)), lm.GOLD)
        self.assertEqual(lm.tone_of((120, 70, 30)), lm.BROWN)
        # Row 185: the Hylian frame's pale warm grey takes WARM_GREY beside GOLD; the real gold and the
        # darker browns do not (the module's look_paint_test checks the same colours).
        self.assertEqual(lm.tone_of((168, 168, 136)), lm.GOLD | lm.WARM_GREY)
        self.assertEqual(lm.tone_of((176, 160, 144)), lm.GOLD | lm.WARM_GREY)
        self.assertEqual(lm.tone_of((240, 192, 96)), lm.GOLD)
        self.assertEqual(lm.tone_of((213, 180, 113)), lm.GOLD)
        self.assertEqual(lm.tone_of((128, 112, 96)), lm.BROWN)
        # the module's numbers (hookshot/src/coop_look_paint.h Tone)
        with open(os.path.join(SRC, "coop_look_paint.h"), encoding="utf-8") as f:
            src = f.read()
        for name, bit in (("kGrey", lm.GREY), ("kGold", lm.GOLD), ("kPink", lm.PINK), ("kWarmGrey", lm.WARM_GREY)):
            k = int(re.search(name + r" = 1u << (\d+)", src).group(1))
            self.assertEqual(bit, 1 << k)

    def test_block_at_is_what_deswizzle_reads(self):
        rnd = random.Random(5)
        for wide, rows, gobs in ((2, 2, 1), (12, 20, 2), (40, 33, 4), (3, 70, 16)):
            blocks = [[bytes(rnd.randrange(256) for _ in range(8)) for _ in range(wide)] for _ in range(rows)]
            lin = tx.deswizzle(swizzle(blocks, 8, gobs), wide * 8, rows, gobs.bit_length() - 1)
            self.assertEqual(lin, b"".join(b"".join(r) for r in blocks))

    def test_smaller_levels_have_fewer_gobs(self):
        self.assertEqual(lm.mip_gobs_tall(4, 128), 16)
        self.assertEqual(lm.mip_gobs_tall(4, 64), 8)
        self.assertEqual(lm.mip_gobs_tall(4, 33), 8)
        self.assertEqual(lm.mip_gobs_tall(4, 32), 4)
        self.assertEqual(lm.mip_gobs_tall(4, 1), 1)
        self.assertEqual(lm.mip_gobs_tall(0, 100), 1)

    def textures(self):
        # colour 8x8 (BC1, 2x2 blocks): left column grey (the frame), right column blue on top,
        # a gold-and-grey block below it (the Triforce: no part)
        alb = full_bntx(0x1A, 8, 8, 0, [swizzle([[GREY, BLUE], [GREY, GOLDGREY]], 8, 1)])
        # metal 8x8 (BC4): every block metal but the top right; one 4x4 level below it
        mtl = full_bntx(0x1D, 8, 8, 0, [swizzle([[METAL, NONE], [METAL, METAL]], 8, 1),
                                        swizzle([[METAL]], 8, 1)])
        return alb, mtl

    def test_the_blocks_under_each_part(self):
        alb, mtl = self.textures()
        rules = ((3, lm.GREY), (4, lm.BLUE | lm.PURPLE), (5, lm.RED))
        _, _, _, _, mips, image = lm.info(mtl)
        got = lm.blocks(mtl, alb, rules)
        at = lambda x, y: lm.block_at(x, y, 16, 2, 1, 8)
        # the frame: the left column, and the small level's one block (it covers all); the top right
        # is not metal, the bottom right lies under the gold: kept
        self.assertEqual(got[3], sorted([at(0, 0), at(0, 1), mips[1]]))
        # the face: only the small level's block (its first level's block is not metal)
        self.assertEqual(got[4], [mips[1]])
        self.assertNotIn(5, got)
        for offs in got.values():
            for a in offs:
                self.assertEqual(image[a:a + 8], METAL)

    def test_the_file(self):
        alb, mtl = self.textures()
        have = {"Mt_Weapon_Shield_Hylian_Alb": alb, "Mt_Weapon_Shield_Hylian_Mtl": mtl}
        said = []
        blob = lm.metal(have.get, said.append)
        self.assertEqual(blob[:8], b"COOPMTL1")
        entries = lm.unpack(blob)
        self.assertEqual([(n, p) for n, p, _ in entries],
                         [("Mt_Weapon_Shield_Hylian_Mtl", 3), ("Mt_Weapon_Shield_Hylian_Mtl", 4)])
        self.assertEqual(len(said), len(lm.METAL) - 1)   # each other item: no textures, said
        self.assertIsNone(lm.metal(lambda name: None, said.append))
        with self.assertRaises(tunic.TunicError):
            lm.unpack(blob[:-1])
        with self.assertRaises(tunic.TunicError):
            lm.unpack(b"COOPMTL2" + blob[8:])
        # a texture in another format is said, not a crash
        said.clear()
        bad = {"Mt_Weapon_Shield_Hylian_Alb": alb, "Mt_Weapon_Shield_Hylian_Mtl": alb}
        self.assertIsNone(lm.metal(bad.get, said.append))
        self.assertTrue(any("not BC4" in s for s in said))

    def test_gear_holds_the_metal_textures(self):
        """gear.bin holds the metalness textures (the module paints and finds them), and the item
        copies rename them with the colour textures (look_items.model_for: every GEAR name)."""
        for name in lm.TEXTURES:
            self.assertIn(name, look_live.GEAR)
            self.assertTrue(name.startswith("Mt_") and name.endswith("_Mtl"))

    def test_the_frame_takes_its_warm_grey_blocks(self):
        # Row 185: a frame block whose ends are the pale warm grey and grey is the frame's with the
        # frame's rule (GREY | WARM_GREY), so its metal is cleared; with GREY alone it was no part's
        # (its warm end read as the Triforce's gold) and kept its metal. A real gold block still keeps it.
        self.assertEqual(lm.tone_of(lm._unpack565(0xB551)), lm.GOLD | lm.WARM_GREY)
        alb = full_bntx(0x1A, 8, 8, 0, [swizzle([[WARMGREY, BLUE], [GREY, GOLDGREY]], 8, 1)])
        mtl = full_bntx(0x1D, 8, 8, 0, [swizzle([[METAL, NONE], [METAL, METAL]], 8, 1)])
        at = lambda x, y: lm.block_at(x, y, 16, 2, 1, 8)
        new = lm.blocks(mtl, alb, ((3, lm.GREY | lm.WARM_GREY), (4, lm.BLUE | lm.PURPLE), (5, lm.RED)))
        old = lm.blocks(mtl, alb, ((3, lm.GREY), (4, lm.BLUE | lm.PURPLE), (5, lm.RED)))
        self.assertEqual(new[3], sorted([at(0, 0), at(0, 1)]))
        self.assertEqual(old[3], [at(0, 1)])
        self.assertIn(dict(lm.METAL[0][2])[3] & lm.WARM_GREY, (lm.WARM_GREY,))   # the Hylian frame's rule

    def test_parts_and_tones_are_the_modules(self):
        """Each metal item's parts and tones, in the module's rule order (coop_look_parts.cpp Gear(),
        coop_look.h Part): the blocks follow what the painter paints."""
        with open(os.path.join(SRC, "coop_look.h"), encoding="utf-8") as f:
            numbers = {n: int(v) for n, v in re.findall(r"^\s+(\w+) = (\d+),", f.read(), re.M)}
        with open(os.path.join(SRC, "coop_look_parts.cpp"), encoding="utf-8") as f:
            src = f.read()
        tones = {n: getattr(lm, re.sub(r"(?<!^)(?=[A-Z])", "_", n[1:]).upper()) for n in re.findall(r"T::(k\w+)", src)}
        for _, alb, rules in lm.METAL:
            entry = re.search(r'\{"' + re.escape(alb) + r'", Format::\w+,\s*\{(.*?)\}\},\n', src, re.S)
            self.assertIsNotNone(entry, alb)
            cpp = [(numbers[p], sum(tones[t] for t in re.findall(r"T::(k\w+)", ts)))
                   for p, ts in re.findall(r"\{Part::(\w+), ([^}]*)\}", entry.group(1))]
            self.assertEqual(list(rules), cpp, alb)


if __name__ == "__main__":
    unittest.main()


# Row 185 reopened: the Hylian frame's own end pairs (565, read from the game's texture).
FRAME = struct.pack("<HHI", 0xA632, 0xA473, 0xE4E4E4E4)     # (165, 199, 148) green + (165, 142, 156) grey
FRAME_DARK = struct.pack("<HHI", 0xA513, 0x6B2B, 0xE4E4E4E4)  # grey + (107, 101, 90) dark brown
TRIFORCE = struct.pack("<HHI", 0xFE6D, 0xA632, 0)            # gold (255, 207, 107) + the green end


class LookRegionTest(unittest.TestCase):
    RULES = ((3, lm.GREY | lm.WARM_GREY), (4, lm.BLUE | lm.PURPLE), (5, lm.RED))

    def test_forceable_as_the_module(self):
        tones = lm.GREY | lm.WARM_GREY | lm.BLUE | lm.PURPLE | lm.RED
        for c in ((165, 199, 148), (107, 101, 90), (66, 60, 49), (8, 4, 0)):
            self.assertTrue(lm.forceable(c, tones), c)
        for c in ((247, 207, 107), (140, 60, 30), (41, 44, 90), (165, 142, 156)):
            self.assertFalse(lm.forceable(c, tones), c)
        with open(os.path.join(SRC, "coop_look_paint.cpp"), encoding="utf-8") as f:
            self.assertIn(f"hi - lo < {lm.SPREAD} * hi", f.read())

    def textures(self):
        # colour 8x8 (BC1, 2x2 blocks): the frame's pairs on the left, the same pair top right (not on
        # metal), the Triforce's gold beside a green end bottom right; metal everywhere but the top
        # right; a second 4x4 level, one frame block over all of it (the top right too)
        alb = full_bntx(0x1A, 8, 8, 0, [swizzle([[FRAME, FRAME], [FRAME_DARK, TRIFORCE]], 8, 1),
                                        swizzle([[FRAME]], 8, 1)])
        mtl = full_bntx(0x1D, 8, 8, 0, [swizzle([[METAL, NONE], [METAL, METAL]], 8, 1)])
        return alb, mtl

    def test_the_frame_blocks_wholly_on_metal(self):
        alb, mtl = self.textures()
        _, _, _, _, mips, image = lm.info(alb)
        at = lambda x, y: lm.block_at(x, y, 16, 2, 1, 8)
        got = lm.region_blocks(mtl, alb, self.RULES)
        # the left column; not the top right (no metal), the Triforce's (its gold is no part's), nor the
        # small level's block (it lies on the top right's no-metal too)
        self.assertEqual(got, {3: sorted([at(0, 0), at(0, 1)])})
        self.assertEqual(len(mips), 2)
        self.assertEqual(image[at(0, 0):at(0, 0) + 8], FRAME)

    def test_a_block_with_nothing_to_force_is_left_out(self):
        alb = full_bntx(0x1A, 8, 8, 0, [swizzle([[GREY, BLUE], [GREY, GREY]], 8, 1)])
        _, mtl = self.textures()
        self.assertEqual(lm.region_blocks(mtl, alb, self.RULES), {})

    def test_the_file(self):
        alb, mtl = self.textures()
        have = {"Mt_Weapon_Shield_Hylian_Alb": alb, "Mt_Weapon_Shield_Hylian_Mtl": mtl}
        blob = lm.regions(have.get, lambda s: None)
        self.assertEqual(blob[:8], b"COOPRGN1")
        entries = lm.unpack(blob, lm.REGION_MAGIC)
        self.assertEqual([(n, p, len(o)) for n, p, o in entries], [("Mt_Weapon_Shield_Hylian_Alb", 3, 2)])
        with self.assertRaises(tunic.TunicError) as e:
            lm.unpack(blob)   # never read as a metal file
        self.assertEqual(str(e.exception), "not a metal file")
        with self.assertRaises(tunic.TunicError) as e:
            lm.unpack(b"COOPMTL1" + blob[8:], lm.REGION_MAGIC)
        self.assertEqual(str(e.exception), "not a region file")
        self.assertIsNone(lm.regions(lambda name: None))
