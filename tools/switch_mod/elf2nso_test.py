#!/usr/bin/env python3
"""Unit tests for elf2nso.py on small ELF files made here (no compiler needed)."""
from __future__ import annotations

import hashlib
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import elf2nso  # noqa: E402


def make_elf(loads, sections):
    """loads: [(flags, vaddr, payload, memsz)], sections: {name: (addr, size)}."""
    ph_off = 0x40
    ph_size = 0x38
    body_off = ph_off + ph_size * len(loads)
    body = b""
    phdrs = b""
    for flags, vaddr, payload, memsz in loads:
        phdrs += struct.pack("<IIQQQQQQ", 1, flags, body_off + len(body), vaddr, vaddr,
                             len(payload), memsz, 0x1000)
        body += payload
    names = b"\0" + b"".join(n.encode() + b"\0" for n in sections) + b".shstrtab\0"
    shstr_off = body_off + len(body)
    sh_off = shstr_off + len(names)
    shdrs = struct.pack("<IIQQQQIIQQ", *([0] * 10))
    pos = 1
    for n, (addr, size) in sections.items():
        shdrs += struct.pack("<IIQQQQIIQQ", pos, 1, 0, addr, 0, size, 0, 0, 1, 0)
        pos += len(n) + 1
    shdrs += struct.pack("<IIQQQQIIQQ", pos, 3, 0, 0, shstr_off, len(names), 0, 0, 1, 0)
    shnum = len(sections) + 2
    ehdr = bytearray(0x40)
    ehdr[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<QQ", ehdr, 0x20, ph_off, sh_off)
    struct.pack_into("<HHHHH", ehdr, 0x36, ph_size, len(loads), 0x40, shnum, shnum - 1)
    return bytes(ehdr) + phdrs + body + names + shdrs


TEXT = b"\x01" * 0x1000
RO = b"\x02" * 0x800
DATA = b"\x03" * 0x100
GOOD_LOADS = [(5, 0, TEXT, 0x1000), (4, 0x1000, RO, 0x800), (6, 0x2000, DATA, 0x500)]
GOOD_SECTIONS = {".dynsym": (0x1100, 0x48), ".dynstr": (0x1200, 0x20)}


class Elf2NsoTest(unittest.TestCase):
    def test_header_and_segments(self):
        nso = elf2nso.build_nso(make_elf(GOOD_LOADS, GOOD_SECTIONS))
        self.assertEqual(nso[:4], b"NSO0")
        self.assertEqual(struct.unpack_from("<I", nso, 0x0C)[0], 0)
        self.assertEqual(struct.unpack_from("<III", nso, 0x10), (0x100, 0, 0x1000))
        self.assertEqual(struct.unpack_from("<III", nso, 0x20), (0x1100, 0x1000, 0x800))
        self.assertEqual(struct.unpack_from("<III", nso, 0x30), (0x1900, 0x2000, 0x100))
        self.assertEqual(struct.unpack_from("<I", nso, 0x3C)[0], 0x400)  # bss
        self.assertEqual(struct.unpack_from("<III", nso, 0x60), (0x1000, 0x800, 0x100))
        self.assertEqual(struct.unpack_from("<II", nso, 0x90), (0x200, 0x20))  # dynstr, ro-relative
        self.assertEqual(struct.unpack_from("<II", nso, 0x98), (0x100, 0x48))
        self.assertEqual(nso[0xA0:0xC0], hashlib.sha256(TEXT).digest())
        self.assertEqual(nso[0x100:0x1100], TEXT)
        self.assertEqual(nso[0x1900:], DATA)

    def test_rejects_wrong_segment_count(self):
        with self.assertRaises(elf2nso.NsoError):
            elf2nso.build_nso(make_elf(GOOD_LOADS[:2], GOOD_SECTIONS))

    def test_rejects_writable_text(self):
        loads = [(7, 0, TEXT, 0x1000)] + GOOD_LOADS[1:]
        with self.assertRaises(elf2nso.NsoError):
            elf2nso.build_nso(make_elf(loads, GOOD_SECTIONS))

    def test_rejects_bss_outside_data(self):
        loads = [GOOD_LOADS[0], (4, 0x1000, RO, 0x900), GOOD_LOADS[2]]
        with self.assertRaises(elf2nso.NsoError):
            elf2nso.build_nso(make_elf(loads, GOOD_SECTIONS))

    def test_rejects_dynsym_outside_rodata(self):
        sections = {".dynsym": (0x2000, 0x48), ".dynstr": (0x1200, 0x20)}
        with self.assertRaises(elf2nso.NsoError):
            elf2nso.build_nso(make_elf(GOOD_LOADS, sections))


if __name__ == "__main__":
    unittest.main()
