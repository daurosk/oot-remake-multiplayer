#!/usr/bin/env python3
"""Unit tests for lift_check.py's instruction decoding and indirect-call tracking (no compiler needed)."""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lift_check  # noqa: E402


def adrp(rd: int, pc: int, target: int) -> int:
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    imm &= (1 << 21) - 1
    return 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd


def ldr(rt: int, rn: int, off: int) -> int:
    return 0xF9400000 | (off // 8) << 10 | rn << 5 | rt


def blr(rn: int) -> int:
    return 0xD63F0000 | rn << 5


def br(rn: int) -> int:
    return 0xD61F0000 | rn << 5


def add(rd: int, rn: int, imm: int) -> int:
    return 0x91000000 | imm << 10 | rn << 5 | rd


NOP = 0xD503201F
RET = 0xD65F03C0
LDR_X8_X8_X0_LSL3 = 0xF8607908  # ldr x8, [x8, x0, lsl #3]
MOV_X8_X9 = 0xAA0903E8  # mov x8, x9 (orr)


def code(*insns: int) -> bytes:
    return b"".join(struct.pack("<I", i) for i in insns)


class DecodeTest(unittest.TestCase):
    def test_adrp_forward_and_back(self):
        self.assertEqual(lift_check.decode(adrp(8, 0x1004, 0x5010), 0x1004), ("adrp", 8, 0x5000))
        self.assertEqual(lift_check.decode(adrp(3, 0x9000, 0x2000), 0x9000), ("adrp", 3, 0x2000))

    def test_ldr_add_blr_br(self):
        self.assertEqual(lift_check.decode(ldr(16, 16, 0x18), 0), ("ldr", 16, 16, 0x18))
        self.assertEqual(lift_check.decode(add(16, 16, 0x18), 0), ("addi", 16, 16))
        self.assertEqual(lift_check.decode(blr(8), 0), ("blr", 8))
        self.assertEqual(lift_check.decode(br(17), 0), ("br", 17))
        self.assertIsNone(lift_check.decode(NOP, 0))
        self.assertIsNone(lift_check.decode(RET, 0))

    def test_branch_targets(self):
        self.assertEqual(lift_check.branch_target(0x14000002, 0x100), 0x108)  # b +8
        self.assertEqual(lift_check.branch_target(0x97FFFFFF, 0x100), 0xFC)   # bl -4
        self.assertEqual(lift_check.branch_target(0x54000040, 0x100), 0x108)  # b.eq +8
        self.assertEqual(lift_check.branch_target(0xB4000048, 0x100), 0x108)  # cbz x8, +8
        self.assertEqual(lift_check.branch_target(0x36000048, 0x100), 0x108)  # tbz w8, #0, +8
        self.assertIsNone(lift_check.branch_target(NOP, 0))


def calls(*insns: int, base: int = 0x1000):
    return list(lift_check.indirect_calls(code(*insns), base))


class IndirectCallTest(unittest.TestCase):
    def test_slot_call(self):
        self.assertEqual(calls(adrp(8, 0x1000, 0x6010), ldr(8, 8, 0x10), blr(8)), [(0x1008, "blr", 0x6010)])

    def test_plt_stub(self):
        c = calls(adrp(16, 0x1000, 0x7000), ldr(17, 16, 0x20), add(16, 16, 0x20), br(17))
        self.assertEqual(c, [(0x100C, "br", 0x7020)])

    def test_add_into_the_target_register_is_not_the_plt(self):
        c = calls(adrp(16, 0x1000, 0x7000), ldr(17, 16, 0x20), add(17, 17, 0x8), br(17))
        self.assertEqual(c, [(0x100C, "br", None)])

    def test_register_offset_load_from_a_table(self):  # the reviewer's case
        c = calls(adrp(8, 0x1000, 0x7000), ldr(8, 8, 0x10), LDR_X8_X8_X0_LSL3, blr(8))
        self.assertEqual(c, [(0x100C, "blr", None)])

    def test_load_through_a_loaded_pointer(self):
        self.assertEqual(calls(adrp(8, 0x1000, 0x6000), ldr(8, 8, 0), ldr(8, 8, 0), blr(8))[0][2], None)

    def test_mov_or_other_write_between(self):
        self.assertEqual(calls(adrp(9, 0x1000, 0x6000), ldr(9, 9, 0), MOV_X8_X9, blr(8))[0][2], None)
        self.assertEqual(calls(adrp(8, 0x1000, 0x6000), ldr(8, 8, 0), NOP, blr(8))[0][2], None)

    def test_ldr_base_not_from_the_adrp(self):
        self.assertEqual(calls(adrp(9, 0x1000, 0x6000), ldr(8, 10, 0), blr(8))[0][2], None)

    def test_ldr_into_another_register(self):
        self.assertEqual(calls(adrp(8, 0x1000, 0x6000), ldr(9, 8, 0), blr(8))[0][2], None)

    def test_branch_into_the_middle(self):
        # b +12 at 0x1000 lands on the ldr at 0x100c: x8 there may not hold the adrp's page.
        c = calls(0x14000003, NOP, adrp(8, 0x1008, 0x6000), ldr(8, 8, 0), blr(8))
        self.assertEqual(c, [(0x1010, "blr", None)])
        # Landing on the adrp itself is fine.
        c = calls(0x14000002, NOP, adrp(8, 0x1008, 0x6000), ldr(8, 8, 0), blr(8))
        self.assertEqual(c, [(0x1010, "blr", 0x6000)])

    def test_br_into_the_middle_of_a_plt_shape(self):
        c = calls(0x14000004, NOP, adrp(16, 0x1008, 0x7000), ldr(17, 16, 0x20), add(16, 16, 0x20), br(17))
        self.assertEqual(c, [(0x1014, "br", None)])


if __name__ == "__main__":
    unittest.main()
