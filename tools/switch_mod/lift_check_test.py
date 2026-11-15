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


def code(*insns: int) -> bytes:
    return b"".join(struct.pack("<I", i) for i in insns)


class DecodeTest(unittest.TestCase):
    def test_adrp_forward_and_back(self):
        self.assertEqual(lift_check.decode(adrp(8, 0x1004, 0x5010), 0x1004), ("adrp", 8, 0x5000))
        self.assertEqual(lift_check.decode(adrp(3, 0x9000, 0x2000), 0x9000), ("adrp", 3, 0x2000))

    def test_ldr_blr_br_ret(self):
        self.assertEqual(lift_check.decode(ldr(16, 16, 0x18), 0), ("ldr", 16, 16, 0x18))
        self.assertEqual(lift_check.decode(blr(8), 0), ("blr", 8))
        self.assertEqual(lift_check.decode(br(17), 0), ("br", 17))
        self.assertEqual(lift_check.decode(0xD65F03C0, 0), ("end",))
        self.assertIsNone(lift_check.decode(0xD503201F, 0))  # nop


class IndirectCallTest(unittest.TestCase):
    def test_call_through_a_slot_is_traced(self):
        c = code(adrp(8, 0x1000, 0x6010), ldr(8, 8, 0x10), 0xD503201F, blr(8))
        self.assertEqual(list(lift_check.indirect_calls(c, 0x1000)), [(0x100C, "blr", 0x6010)])

    def test_plt_style_branch(self):
        c = code(adrp(16, 0x2000, 0x7000), ldr(17, 16, 0x20), br(17))
        self.assertEqual(list(lift_check.indirect_calls(c, 0x2000)), [(0x2008, "br", 0x7020)])

    def test_unknown_source_after_ret_or_overwrite(self):
        c = code(adrp(8, 0, 0x6000), 0xD65F03C0, ldr(8, 8, 0), blr(8))
        self.assertEqual(list(lift_check.indirect_calls(c, 0)), [(0xC, "blr", None)])
        c = code(adrp(8, 0, 0x6000), ldr(8, 8, 0), ldr(8, 8, 0), blr(8))  # loads through a loaded pointer
        self.assertEqual(list(lift_check.indirect_calls(c, 0)), [(0xC, "blr", None)])


if __name__ == "__main__":
    unittest.main()
