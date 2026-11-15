#!/usr/bin/env python3
"""Turn the prototype's linked ELF (three PT_LOAD segments: RX, R, RW+bss) into an NSO module.

Our own small converter, so the prototype needs only LLVM (clang + ld.lld). The NSO is written
uncompressed and without segment hash checks (flags 0), which emulators load as is
(Fritchii src/core/loader/nso.cpp). Layout of the header: switchbrew.org/wiki/NSO.

    elf2nso.py module.elf subsdk9
"""
from __future__ import annotations

import hashlib
import struct
import sys

PT_LOAD = 1
HEADER_SIZE = 0x100
PAGE = 0x1000


class NsoError(Exception):
    pass


def read_elf(data: bytes):
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise NsoError("not a 64-bit little-endian ELF")
    (e_phoff, e_shoff) = struct.unpack_from("<QQ", data, 0x20)
    (e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHHHH", data, 0x36)
    loads = []
    for i in range(e_phnum):
        p_type, p_flags, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, _align = struct.unpack_from(
            "<IIQQQQQQ", data, e_phoff + i * e_phentsize)
        if p_type == PT_LOAD:
            loads.append((p_flags, p_offset, p_vaddr, p_filesz, p_memsz))
    sections = {}
    if e_shoff and e_shnum:
        shdrs = [struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
        strtab = shdrs[e_shstrndx]
        for sh in shdrs:
            name_off = strtab[4] + sh[0]
            name = data[name_off:data.index(b"\0", name_off)].decode()
            sections[name] = (sh[3], sh[5])  # addr, size
    return loads, sections


def build_nso(data: bytes) -> bytes:
    loads, sections = read_elf(data)
    if len(loads) != 3:
        raise NsoError(f"expected 3 PT_LOAD segments (text, rodata, data), found {len(loads)}")
    loads.sort(key=lambda s: s[2])
    expected_flags = (5, 4, 6)  # R+X, R, R+W
    for (flags, _off, vaddr, filesz, memsz), want in zip(loads, expected_flags):
        if flags != want:
            raise NsoError(f"segment at {vaddr:#x} has flags {flags}, want {want}")
        if vaddr % PAGE:
            raise NsoError(f"segment at {vaddr:#x} is not page aligned")
    if loads[0][2] != 0:
        raise NsoError("text must start at 0")
    for flags, _off, vaddr, filesz, memsz in loads[:2]:
        if filesz != memsz:
            raise NsoError(f"only the data segment may have bss (segment at {vaddr:#x})")

    segs = [data[off:off + filesz] for (_f, off, _v, filesz, _m) in loads]
    data_seg = loads[2]
    bss_size = data_seg[4] - data_seg[3]
    ro_addr = loads[1][2]

    def ro_rel(name: str):
        if name not in sections:
            raise NsoError(f"no {name} section")
        addr, size = sections[name]
        if not (ro_addr <= addr and addr + size <= ro_addr + loads[1][3]):
            raise NsoError(f"{name} is not inside the rodata segment")
        return addr - ro_addr, size

    dynstr = ro_rel(".dynstr")
    dynsym = ro_rel(".dynsym")

    header = bytearray(HEADER_SIZE)
    struct.pack_into("<4sII", header, 0, b"NSO0", 0, 0)
    struct.pack_into("<I", header, 0x0C, 0)  # flags: no compression, no hash checks
    file_off = HEADER_SIZE
    seg_headers = (0x10, 0x20, 0x30)
    for i, (seg, load) in enumerate(zip(segs, loads)):
        struct.pack_into("<III", header, seg_headers[i], file_off, load[2], len(seg))
        struct.pack_into("<I", header, 0x60 + 4 * i, len(seg))  # file size == size (uncompressed)
        header[0xA0 + 0x20 * i:0xC0 + 0x20 * i] = hashlib.sha256(seg).digest()
        file_off += len(seg)
    struct.pack_into("<I", header, 0x3C, bss_size)
    module_id = hashlib.sha256(b"".join(segs)).digest()[:0x20]
    header[0x40:0x60] = module_id
    struct.pack_into("<II", header, 0x88, 0, 0)  # api info: none
    struct.pack_into("<II", header, 0x90, *dynstr)
    struct.pack_into("<II", header, 0x98, *dynsym)
    return bytes(header) + b"".join(segs)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    try:
        nso = build_nso(open(argv[1], "rb").read())
    except (OSError, NsoError) as e:
        print(f"elf2nso: {e}", file=sys.stderr)
        return 1
    with open(argv[2], "wb") as f:
        f.write(nso)
    print(f"elf2nso: {argv[2]}: {len(nso)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
