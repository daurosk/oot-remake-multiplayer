"""Tests for the study tools (nso.py, vtables.py, callers.py). Synthetic data only: no game files.

    python tools/game_scan/tests/test_game_scan.py
"""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import callers  # noqa: E402
import nso  # noqa: E402
import vtables  # noqa: E402

B = nso.BASE
TEXT, RO, DATA, SEG = 0x0, 0x1000, 0x2000, 0x1000

# Code (module offsets)
NAME_FN, CTOR, CREATE, CALLER = 0x100, 0x200, 0x300, 0x400
SCREEN_FNS = (0x500, 0x510, 0x520)
TYPED_FN, PURE_FN = 0x530, 0x540
OVERWRITE, SIMD = 0x600, 0x640   # an ADRP whose register is overwritten; a SIMD store and load
OVERWRITES, KEEP = 0x680, 0x700  # ADRPs whose register a pair, an atomic, a CAS or a STXR overwrites; one they keep
# Read-only data
MOD0, NAME_STR, TYPE_NAME, DYNSTR, DYNSYM, DYNAMIC, RELA, RELR = (
    0x1000, 0x1100, 0x1120, 0x1200, 0x1300, 0x1400, 0x1500, 0x1600)
# Data: four vtables, a type info object, a global pointer and a global value
FACTORY_VT, SCREEN_VT, TYPED_VT, PURE_VT = 0x2010, 0x2030, 0x2058, 0x2070
TYPE_INFO, GLOBAL_PTR, GLOBAL_VAL, WIDE, COUNTER = 0x2100, 0x2200, 0x2208, 0x2210, 0x2220


def adrp(rd, pc, target):
    imm = (target >> 12) - (pc >> 12)
    return 0x90000000 | (imm & 3) << 29 | ((imm >> 2) & 0x7FFFF) << 5 | rd


def add(rd, rn, imm):
    return 0x91000000 | imm << 10 | rn << 5 | rd


def ldr64(rt, rn, off):
    return 0xF9400000 | (off // 8) << 10 | rn << 5 | rt


def str64(rt, rn, off):
    return 0xF9000000 | (off // 8) << 10 | rn << 5 | rt


def bl(pc, target):
    return 0x94000000 | ((target - pc) // 4) & 0x3FFFFFF


RET = 0xD65F03C0
MOV_X8_X0 = 0xAA0003E8          # orr x8, xzr, x0
STR_Q0 = 0x3D800000             # str q0, [xN, #imm * 16]
LDR_Q1 = 0x3DC00001             # ldr q1, [xN, #imm * 16]
# Each writes x8 (checked with capstone, as are the words in test_writes).
WRITE_X8 = (0xA94023E0,         # ldp x0, x8, [sp]: the second register
            0xF8210008,         # ldadd x1, x8, [x0]: the old value
            0xC8A87C01,         # cas x8, x1, [x0]: the old value goes to Rs
            0xC8087C01)         # stxr w8, x1, [x0]: the status
# Each reads x8 and writes another register or none.
KEEP_X8 = (0xC8097C08,          # stxr w9, x8, [x0]
           0xC8E97C08,          # casa x9, x8, [x0]
           0xF828001F)          # stadd x8, [x0] (ldadd x8, xzr, [x0])


def simd(op, rn, off):
    return op | (off // 16) << 10 | rn << 5


def code(img, at, words):
    for k, w in enumerate(words):
        struct.pack_into("<I", img, at + 4 * k, w)


def make_image():
    """A tiny main: text, ro and data as one module image (offsets from the module's start)."""
    img = bytearray(3 * SEG)
    struct.pack_into("<I", img, 4, MOD0)
    # A name function, a constructor that stores the screen's vtable, a factory function that calls it,
    # and a caller that calls the constructor, reads it through a global pointer and reads a global.
    code(img, NAME_FN, [adrp(0, B + NAME_FN, B + NAME_STR), add(0, 0, NAME_STR & 0xFFF), RET])
    code(img, CTOR, [adrp(8, B + CTOR, B + SCREEN_VT), add(8, 8, SCREEN_VT & 0xFFF), str64(8, 0, 0), RET])
    code(img, CREATE, [bl(CREATE, CTOR), RET])
    code(img, CALLER, [bl(CALLER, CTOR), adrp(9, B + CALLER + 4, B + GLOBAL_PTR),
                       ldr64(10, 9, GLOBAL_PTR & 0xFFF), ldr64(11, 9, GLOBAL_VAL & 0xFFF), RET])
    for f in SCREEN_FNS + (TYPED_FN, PURE_FN):
        code(img, f, [RET])
    # x8 gets the page of GLOBAL_VAL, then something else: the load after it is not a read of GLOBAL_VAL.
    code(img, OVERWRITE, [adrp(8, B + OVERWRITE, B + GLOBAL_VAL), MOV_X8_X0, ldr64(9, 8, GLOBAL_VAL & 0xFFF), RET])
    code(img, SIMD, [adrp(9, B + SIMD, B + WIDE), simd(STR_Q0, 9, WIDE & 0xFFF), simd(LDR_Q1, 9, WIDE & 0xFFF), RET])
    for k, w in enumerate(WRITE_X8):
        at = OVERWRITES + 16 * k
        code(img, at, [adrp(8, B + at, B + GLOBAL_VAL), w, ldr64(9, 8, GLOBAL_VAL & 0xFFF), RET])
    code(img, KEEP, [adrp(8, B + KEEP, B + COUNTER), *KEEP_X8, ldr64(10, 8, COUNTER & 0xFFF), RET])
    img[NAME_STR:NAME_STR + 11] = b"TestScreen\0"
    img[TYPE_NAME:TYPE_NAME + 14] = b"N4test5ThingE\0"
    names = b"\0__cxa_pure_virtual\0_ZTVN10__cxxabiv117__class_type_infoE\0"
    img[DYNSTR:DYNSTR + len(names)] = names
    struct.pack_into("<IBBHQQ", img, DYNSYM + 24, 1, 0x12, 0, 0, 0, 0)    # __cxa_pure_virtual, imported
    struct.pack_into("<IBBHQQ", img, DYNSYM + 48, 20, 0x11, 0, 0, 0, 0)   # the class_type_info vtable, imported
    # Pointers the RELR table relocates (their stored values are module offsets).
    relr = []

    def ptr(at, value):
        struct.pack_into("<Q", img, at, value)
        relr.append(at)

    ptr(FACTORY_VT, CREATE)
    ptr(FACTORY_VT + 8, NAME_FN)
    for k, f in enumerate(SCREEN_FNS):
        ptr(SCREEN_VT + 8 * k, f)
    ptr(TYPED_VT - 8, TYPE_INFO)
    ptr(TYPED_VT, TYPED_FN)
    ptr(PURE_VT, PURE_FN)
    ptr(GLOBAL_PTR, CTOR)
    struct.pack_into("<Q", img, GLOBAL_VAL, 1234)
    for k, at in enumerate(relr):
        struct.pack_into("<Q", img, RELR + 8 * k, at)
    rela = [(TYPE_INFO, 2 << 32 | nso.R_AARCH64_ABS64, 16),          # the type info's vtable (an import)
            (TYPE_INFO + 8, nso.R_AARCH64_RELATIVE, TYPE_NAME),      # its name
            (PURE_VT + 8, 1 << 32 | nso.R_AARCH64_ABS64, 0)]         # a pure virtual slot
    for k, r in enumerate(rela):
        struct.pack_into("<QQq", img, RELA + 24 * k, *r)
    dyn = [(nso.DT_RELR, RELR), (nso.DT_RELRSZ, 8 * len(relr)), (nso.DT_RELA, RELA), (nso.DT_RELASZ, 24 * len(rela)),
           (nso.DT_SYMTAB, DYNSYM), (nso.DT_STRTAB, DYNSTR), (nso.DT_NULL, 0)]
    for k, (tag, val) in enumerate(dyn):
        struct.pack_into("<qQ", img, DYNAMIC + 16 * k, tag, val)
    img[MOD0:MOD0 + 4] = b"MOD0"
    struct.pack_into("<i", img, MOD0 + 4, DYNAMIC - MOD0)
    return img


def literal_block(raw):
    """An LZ4 block holding `raw` as literals only (valid: the last sequence has no match)."""
    n = len(raw)
    out = bytearray([0xF0 if n >= 15 else n << 4])
    if n >= 15:
        rest = n - 15
        while rest >= 255:
            out.append(255)
            rest -= 255
        out.append(rest)
    return bytes(out) + raw


def make_nso(compress_text=False):
    img = make_image()
    segs = [img[TEXT:TEXT + SEG], img[RO:RO + SEG], img[DATA:DATA + SEG]]
    stored = [literal_block(segs[0]) if compress_text else segs[0], segs[1], segs[2]]
    head = bytearray(0x100)
    head[0:4] = b"NSO0"
    struct.pack_into("<I", head, 0x0C, 1 if compress_text else 0)
    off = len(head)
    for k, (mem, raw, body) in enumerate(zip((TEXT, RO, DATA), segs, stored)):
        struct.pack_into("<III", head, 0x10 + 0x10 * k, off, mem, len(raw))
        struct.pack_into("<I", head, 0x60 + 4 * k, len(body))
        off += len(body)
    struct.pack_into("<I", head, 0x3C, 0x100)
    head[0x40:0x48] = bytes.fromhex("0123456789abcdef")
    return bytes(head) + b"".join(stored)


class NsoTest(unittest.TestCase):
    def test_loads_and_relocates(self):
        m = nso.Main(make_nso())
        self.assertEqual(m.text, (B, B + SEG))
        self.assertEqual(m.data, (B + DATA, B + DATA + SEG))
        self.assertEqual(m.bss, (B + DATA + SEG, B + DATA + SEG + 0x100))
        self.assertEqual(m.u64(B + SCREEN_VT), B + SCREEN_FNS[0])           # RELR
        self.assertEqual(m.u64(B + TYPE_INFO + 8), B + TYPE_NAME)           # RELA relative
        self.assertEqual(m.imports[B + PURE_VT + 8], "__cxa_pure_virtual")
        self.assertEqual(m.imports[B + TYPE_INFO], "_ZTVN10__cxxabiv117__class_type_infoE+16")
        self.assertEqual(m.cstr(B + NAME_STR), "TestScreen")
        self.assertTrue(m.build_id.startswith("0123456789abcdef"))

    def test_compressed_text(self):
        plain, packed = nso.Main(make_nso()), nso.Main(make_nso(compress_text=True))
        self.assertEqual(plain.image, packed.image)

    def test_lz4_match(self):
        # One literal 'a', a match of 8 at offset 1 (overlapping), then a last literal 'b'.
        self.assertEqual(nso.lz4_block(bytes([0x14, 0x61, 1, 0, 0x10, 0x62]), 10), b"aaaaaaaaab")

    def test_lz4_cut_short(self):
        with self.assertRaises(nso.NsoError):
            nso.lz4_block(bytes([0xF0]), 20)        # 15 or more literals, but the length byte is missing
        with self.assertRaises(nso.NsoError):
            nso.lz4_block(bytes([0x14, 0x61, 1]), 10)   # the match offset is cut in half

    def test_not_an_nso(self):
        with self.assertRaises(nso.NsoError):
            nso.Main(b"ELF" + bytes(0x200))

    def test_addresses(self):
        self.assertEqual(nso.parse_addr("main+0x200"), B + 0x200)
        self.assertEqual(nso.parse_addr("f_80020200"), 0x80020200)
        self.assertEqual(nso.parse_addr("0x80020200"), 0x80020200)
        self.assertEqual(nso.parse_addr("80020200"), 0x80020200)


class VtablesTest(unittest.TestCase):
    def setUp(self):
        self.m = nso.Main(make_nso())
        self.vts = {v.addr - B: v for v in vtables.find_vtables(self.m)}
        self.names = vtables.name_all(self.m, list(self.vts.values()))

    def test_finds_every_vtable(self):
        self.assertEqual(sorted(self.vts), [FACTORY_VT, SCREEN_VT, TYPED_VT, PURE_VT])
        self.assertEqual(len(self.vts[SCREEN_VT].slots), 3)
        self.assertEqual(self.vts[PURE_VT].slots, [B + PURE_FN, "__cxa_pure_virtual"])

    def test_name_function(self):
        self.assertEqual(self.names, {B + NAME_FN: "TestScreen"})
        f = self.vts[FACTORY_VT]
        self.assertEqual(f.names, [(1, "TestScreen")])
        self.assertEqual(f.how, "name function in slot 1")

    def test_made_by_factory(self):
        s = self.vts[SCREEN_VT]
        self.assertEqual(s.names, [(-1, "TestScreen")])
        self.assertEqual(s.how, f"made by 0x{B + FACTORY_VT:x}")

    def test_type_info(self):
        t = self.vts[TYPED_VT]
        self.assertEqual(t.names, [(-1, "test::Thing")])
        self.assertEqual(t.how, "type info")

    def test_demangle(self):
        self.assertEqual(vtables.demangle("St9exception"), "std::exception")
        self.assertEqual(vtables.demangle("N3gmd3FooE"), "gmd::Foo")
        self.assertEqual(vtables.demangle("NSt3__14pairIiiEE"), "NSt3__14pairIiiEE")   # templates stay as they are

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "main")
            with open(path, "wb") as f:
                f.write(make_nso())
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(vtables.main([path, "--name", "screen"]), 0)
            lines = out.getvalue().splitlines()
            self.assertEqual(len(lines), 2)
            self.assertTrue(lines[1].startswith(f"0x{B + SCREEN_VT:x}\t3\tTestScreen\tmade by"))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(vtables.main([path, "--vtable", f"main+0x{FACTORY_VT:x}"]), 0)
            self.assertIn("name function: TestScreen", out.getvalue())


class CallersTest(unittest.TestCase):
    def setUp(self):
        self.m = nso.Main(make_nso())

    def hits(self, addr, size=1):
        t = callers.Targets([(addr, addr + size)])
        code, calls = callers.scan_code(self.m, t)
        return {(pc - B, kind) for pc, kind, _, _ in code}, callers.scan_data(self.m, t), calls

    def test_calls_and_pointer_loads(self):
        code, data, calls = self.hits(B + CTOR)
        self.assertEqual(code, {(CREATE, "call"), (CALLER, "call"), (CALLER + 8, "via pointer")})
        self.assertEqual(data, [(B + GLOBAL_PTR, B + CTOR)])
        self.assertIn(B + CTOR, calls)

    def test_address_and_read(self):
        code, _, _ = self.hits(B + SCREEN_VT)
        self.assertEqual(code, {(CTOR + 4, "address")})
        code, _, _ = self.hits(B + GLOBAL_VAL)
        self.assertEqual(code, {(CALLER + 12, "read")})   # not OVERWRITE + 8 nor OVERWRITES: x8 was overwritten

    def test_register_kept(self):
        code, _, _ = self.hits(B + COUNTER)
        self.assertEqual(code, {(KEEP + 16, "read")})   # a store, a CAS and an atomic that only read x8

    def test_simd_store_and_load(self):
        code, _, _ = self.hits(B + WIDE)
        self.assertEqual(code, {(SIMD + 4, "write"), (SIMD + 8, "read")})

    def test_is_store(self):
        for word, store in ((0xF9000000, True), (0xF9400000, False),     # STR x, LDR x
                            (0x3D800000, True), (0x3DC00000, False),     # STR q, LDR q
                            (0xFD000000, True), (0xFD400000, False),     # STR d, LDR d
                            (0xA9000000, True), (0xA9400000, False),     # STP x, LDP x
                            (0xF8000000, True), (0xF8400000, False),     # STUR x, LDUR x
                            (0xF8210008, False), (0xF8218008, False),    # LDADD, SWP: the old value goes to Rt
                            (0xC8A87C01, True), (0xC8E87C01, True),      # CAS, CASA: Rt is stored
                            (0x48287C02, True), (0x48687C02, True),      # CASP, CASPA
                            (0xC8087C01, True), (0xC85F7C08, False),     # STXR, LDXR
                            (0xC8280801, True), (0xC87F2001, False),     # STXP, LDXP
                            (0xC89FFC08, True), (0xC8DFFC08, False),     # STLR, LDAR
                            (MOV_X8_X0, False), (RET, False)):
            self.assertEqual(callers.is_store(word), store, hex(word))

    def test_writes(self):
        # Each word (checked with capstone) and the X registers it may write.
        for word, regs in ((0xA94023E0, {0, 8}),         # ldp x0, x8, [sp]
                           (0x29402020, {0, 8}),         # ldp w0, w8, [x1]
                           (0x69402020, {0, 8}),         # ldpsw x0, x8, [x1]
                           (0xA8402020, {0, 8}),         # ldnp x0, x8, [x1]
                           (0xA9002020, set()),          # stp x0, x8, [x1]
                           (0x6D402020, set()),          # ldp d0, d8, [x1]: V registers
                           (0xA8C10500, {0, 1, 8}),      # ldp x0, x1, [x8], #16: x8 written back
                           (0xA9810500, {8}),            # stp x0, x1, [x8, #16]!
                           (0xF8408500, {0, 8}),         # ldr x0, [x8], #8
                           (0xF8008D00, {8}),            # str x0, [x8, #8]!
                           (0xF9400008, {8}),            # ldr x8, [x0]
                           (0xF9000008, set()),          # str x8, [x0]
                           (0x3DC00008, set()),          # ldr q8, [x0]
                           (0x4CDF7100, {8}),            # ld1 {v0.16b}, [x8], #16
                           (0xF8210008, {8}),            # ldadd x1, x8, [x0]
                           (0xF8A10008, {8}),            # ldadda x1, x8, [x0]
                           (0xF8610008, {8}),            # ldaddl x1, x8, [x0]
                           (0xF8E10008, {8}),            # ldaddal x1, x8, [x0]
                           (0xF8211008, {8}),            # ldclr x1, x8, [x0]
                           (0xF8212008, {8}),            # ldeor x1, x8, [x0]
                           (0xF8213008, {8}),            # ldset x1, x8, [x0]
                           (0xF8214008, {8}),            # ldsmax x1, x8, [x0]
                           (0xF8215008, {8}),            # ldsmin x1, x8, [x0]
                           (0xF8216008, {8}),            # ldumax x1, x8, [x0]
                           (0xF8217008, {8}),            # ldumin x1, x8, [x0]
                           (0xF8218008, {8}),            # swp x1, x8, [x0]
                           (0x38210008, {8}),            # ldaddb w1, w8, [x0]
                           (0x78210008, {8}),            # ldaddh w1, w8, [x0]
                           (0xB8210008, {8}),            # ldadd w1, w8, [x0]
                           (0xF828001F, set()),          # stadd x8, [x0]
                           (0xC8A87C01, {8}),            # cas x8, x1, [x0]
                           (0xC8E87C01, {8}),            # casa x8, x1, [x0]
                           (0xC8A8FC01, {8}),            # casl x8, x1, [x0]
                           (0xC8E8FC01, {8}),            # casal x8, x1, [x0]
                           (0x08A87C01, {8}),            # casb w8, w1, [x0]
                           (0x48A87C01, {8}),            # cash w8, w1, [x0]
                           (0x88A87C01, {8}),            # cas w8, w1, [x0]
                           (0x48287C02, {8, 9}),         # casp x8, x9, x2, x3, [x0]
                           (0x48687C02, {8, 9}),         # caspa x8, x9, x2, x3, [x0]
                           (0x4828FC02, {8, 9}),         # caspl x8, x9, x2, x3, [x0]
                           (0x4868FC02, {8, 9}),         # caspal x8, x9, x2, x3, [x0]
                           (0x08287C02, {8, 9}),         # casp w8, w9, w2, w3, [x0]
                           (0xC8087C01, {8}),            # stxr w8, x1, [x0]
                           (0xC808FC01, {8}),            # stlxr w8, x1, [x0]
                           (0x08087C01, {8}),            # stxrb w8, w1, [x0]
                           (0xC8280801, {8}),            # stxp w8, x1, x2, [x0]
                           (0xC8288801, {8}),            # stlxp w8, x1, x2, [x0]
                           (0xC85F7C08, {8}),            # ldxr x8, [x0]
                           (0xC85FFC08, {8}),            # ldaxr x8, [x0]
                           (0xC87F2001, {1, 8}),         # ldxp x1, x8, [x0]
                           (0xC87FA001, {1, 8}),         # ldaxp x1, x8, [x0]
                           (0xC89FFC08, set()),          # stlr x8, [x0]
                           (0xC8DFFC08, {8}),            # ldar x8, [x0]
                           (MOV_X8_X0, {8}), (RET, set())):
            for rd in range(31):
                self.assertEqual(callers.writes(word, rd), rd in regs, f"{word:#x} x{rd}")

    def test_functions_file_skips_other_lines(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "functions.txt")
            with open(path, "w", encoding="utf-8") as f:
                f.write("# start size\n\n80020100 4\nnot hex\n80020000 2\n")
            self.assertEqual(callers.load_functions(path), [0x80020000, 0x80020100])

    def test_cli_slot_zero(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "main")
            with open(path, "wb") as f:
                f.write(make_nso())
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(callers.main([path, f"main+0x{CREATE:x}"]), 0)
            self.assertIn(f"vtable 0x{B + FACTORY_VT:x} (TestScreen) slot 0", out.getvalue())

    def test_cli_names_the_vtable_slot(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "main")
            with open(path, "wb") as f:
                f.write(make_nso())
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(callers.main([path, f"main+0x{NAME_FN:x}"]), 0)
            self.assertIn(f"vtable 0x{B + FACTORY_VT:x} (TestScreen) slot 1", out.getvalue())


if __name__ == "__main__":
    unittest.main()
