# SPDX-License-Identifier: GPL-3.0-or-later
"""exefs spike: pack an AArch64 ELF (link.ld's three PT_LOAD segments) into an uncompressed NSO0.

usage: python elf2nso.py <in.elf> <out nso>

The NSO header (0x100 bytes): segments not compressed and not hash-checked (flags 0), each segment's file offset,
memory offset and size, the bss size, a module id (the first 32 bytes of the code's SHA-256), and the dynstr/dynsym
extents relative to the read-only segment.
"""
import hashlib
import struct
import sys

PT_LOAD = 1


def main(src, dst):
    e = open(src, 'rb').read()
    if e[:4] != b'\x7fELF' or e[4] != 2 or struct.unpack_from('<H', e, 0x12)[0] != 0xB7:
        sys.exit('not a 64-bit AArch64 ELF: ' + src)
    phoff, = struct.unpack_from('<Q', e, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', e, 0x36)
    loads = []
    for i in range(phnum):
        p_type, p_flags, p_off, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from('<IIQQQQQQ', e, phoff + i * phentsize)
        if p_type == PT_LOAD:
            loads.append((p_vaddr, e[p_off:p_off + p_filesz], p_memsz, p_flags))
    loads.sort()
    if [l[3] for l in loads] != [5, 4, 6]:
        sys.exit('want three PT_LOAD segments RX, R, RW (link.ld); got flags %s' % [l[3] for l in loads])
    (tv, text, _, _), (rv, ro, _, _), (dv, data, dmem, _) = loads
    if tv != 0 or rv & 0xfff or dv & 0xfff or rv < len(text) or dv < rv + len(ro):
        sys.exit('segments not page-aligned in order from 0')
    bss = dmem - len(data)

    # .dynstr / .dynsym extents (relative to the read-only segment), from the section headers
    shoff, = struct.unpack_from('<Q', e, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', e, 0x3A)
    secs = [struct.unpack_from('<IIQQQQIIQQ', e, shoff + i * shentsize) for i in range(shnum)]
    names_off = secs[shstrndx][4]

    def name(s):
        o = names_off + s[0]
        return e[o:e.index(b'\0', o)].decode()

    ext = {}
    for s in secs:
        if name(s) in ('.dynstr', '.dynsym'):
            ext[name(s)] = (s[3] - rv, s[5])
    dynstr, dynsym = ext.get('.dynstr', (0, 0)), ext.get('.dynsym', (0, 0))

    h = bytearray(0x100)
    h[0:4] = b'NSO0'
    off = 0x100
    segs = [(text, tv), (ro, rv), (data, dv)]
    for i, (b, v) in enumerate(segs):
        struct.pack_into('<III', h, 0x10 + i * 0x10, off, v, len(b))
        struct.pack_into('<I', h, 0x60 + i * 4, len(b))
        off += len(b)
    struct.pack_into('<I', h, 0x1C, 0)          # module name offset (none)
    struct.pack_into('<I', h, 0x2C, 0)
    struct.pack_into('<I', h, 0x3C, bss)
    h[0x40:0x60] = hashlib.sha256(text).digest()
    struct.pack_into('<IIIIII', h, 0x88, 0, 0, dynstr[0], dynstr[1], dynsym[0], dynsym[1])
    out = bytes(h) + text + ro + data
    open(dst, 'wb').write(out)
    print('%s: text 0x%x ro 0x%x@0x%x data 0x%x@0x%x bss 0x%x, module id %s' %
          (dst, len(text), len(ro), rv, len(data), dv, bss, h[0x40:0x50].hex()))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
