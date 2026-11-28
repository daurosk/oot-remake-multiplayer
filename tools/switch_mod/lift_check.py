#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that the built ExeFS module can be lifted by a recomp, and write its entries file.

    lift_check.py module.elf [--entries <file>]

A recomp that lifts our module as subsdk1 (docs/exefs.md, "Lifting") finds functions by
symbols, unwind tables and an --entries file, and cannot follow an indirect call it does
not know. So the module must:
  - keep its unwind tables (.eh_frame + .eh_frame_hdr, pointed at by MOD0);
  - export its entry points, the hook table `coop_hooks` and every `coop_slot_*`;
  - make no indirect call (`blr`/`br`) except through a slot listed in the hook table,
    or through an import's GOT entry (the PLT);
  - have a valid hook table (hooks.h): magic, version 1, every entry's slot a coop_slot_*.
The entries file lists every exported function, one per line: `0x<module offset> <name>`.
"""
from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass

SHT_SYMTAB = 2
SHT_DYNSYM = 11
STT_FUNC = 2
STT_OBJECT = 1
HOOK_MAGIC = b"COOPHOOK"
HOOK_HEADER = 32
HOOK_ENTRY = 32
KINDS = {1: "call_redirect", 2: "entry", 3: "game_fn"}


class LiftError(Exception):
    pass


@dataclass
class Section:
    name: str
    type: int
    addr: int
    offset: int
    size: int
    link: int


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    type: int
    defined: bool


class Elf:
    def __init__(self, data: bytes):
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            raise LiftError("not a 64-bit little-endian ELF")
        self.data = data
        e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        raw = [struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
        names = raw[e_shstrndx][4]
        self.sections = [Section(self._str(names + r[0]), r[1], r[3], r[4], r[5], r[6]) for r in raw]

    def _str(self, off: int) -> str:
        return self.data[off:self.data.index(b"\0", off)].decode()

    def section(self, name: str) -> Section | None:
        return next((s for s in self.sections if s.name == name), None)

    def symbols(self, sh_type: int) -> list[Symbol]:
        out = []
        for sec in self.sections:
            if sec.type != sh_type:
                continue
            strtab = self.sections[sec.link].offset
            for i in range(1, sec.size // 24):
                st_name, st_info, _other, st_shndx, st_value, st_size = struct.unpack_from(
                    "<IBBHQQ", self.data, sec.offset + i * 24)
                out.append(Symbol(self._str(strtab + st_name), st_value, st_size, st_info & 0xF, st_shndx != 0))
        return out

    def read(self, addr: int, size: int) -> bytes:
        for s in self.sections:
            if s.type != 8 and s.addr <= addr and addr + size <= s.addr + s.size:  # 8 = NOBITS
                return self.data[s.offset + addr - s.addr:s.offset + addr - s.addr + size]
        raise LiftError(f"address {addr:#x} is not in the file")


# ---- The few AArch64 encodings the indirect-call check needs ----
def sext(v: int, bits: int) -> int:
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def decode(insn: int, pc: int):
    """('adrp', rd, page) | ('ldr', rt, rn, imm) | ('addi', rd, rn) | ('blr'|'br', rn) | None."""
    if insn & 0x9F000000 == 0x90000000:
        imm = sext(((insn >> 5) & 0x7FFFF) << 2 | (insn >> 29) & 3, 21)
        return ("adrp", insn & 31, (pc & ~0xFFF) + (imm << 12))
    if insn & 0xFFC00000 == 0xF9400000:  # LDR Xt, [Xn, #imm12*8]
        return ("ldr", insn & 31, (insn >> 5) & 31, ((insn >> 10) & 0xFFF) * 8)
    if insn & 0xFF800000 == 0x91000000:  # ADD Xd, Xn, #imm (64-bit, no flags)
        return ("addi", insn & 31, (insn >> 5) & 31)
    if insn & 0xFFFFFC1F == 0xD63F0000:
        return ("blr", (insn >> 5) & 31)
    if insn & 0xFFFFFC1F == 0xD61F0000:
        return ("br", (insn >> 5) & 31)
    return None


def branch_target(insn: int, pc: int) -> int | None:
    """Target of a direct branch (b, bl, b.cond, cbz/cbnz, tbz/tbnz), else None."""
    if insn & 0x7C000000 == 0x14000000:  # b, bl
        return pc + sext(insn & 0x3FFFFFF, 26) * 4
    if insn & 0xFF000010 == 0x54000000:  # b.cond
        return pc + sext((insn >> 5) & 0x7FFFF, 19) * 4
    if insn & 0x7E000000 == 0x34000000:  # cbz, cbnz
        return pc + sext((insn >> 5) & 0x7FFFF, 19) * 4
    if insn & 0x7E000000 == 0x36000000:  # tbz, tbnz
        return pc + sext((insn >> 5) & 0x3FFF, 14) * 4
    return None


def indirect_calls(code: bytes, base: int):
    """Yield (pc, kind, loaded_from) for every blr/br.

    Conservative: loaded_from is set only for the two exact shapes a lifter can resolve,
        adrp Xm, page ; ldr Xn, [Xm, #off] ; blr|br Xn                       (slot call)
        adrp Xm, page ; ldr Xn, [Xm, #off] ; add Xm, Xm, #off ; br Xn  (Xm != Xn, PLT)
    with no direct branch landing inside the shape (after its adrp). Anything else
    (register-offset loads, ldur, mov, a load through a loaded pointer, a join point
    between the load and the branch) gives None, which the check reports.
    """
    words = [struct.unpack_from("<I", code, off)[0] for off in range(0, len(code) - 3, 4)]
    targets = {t for i, w in enumerate(words) if (t := branch_target(w, base + 4 * i)) is not None}

    def at(i: int):
        return decode(words[i], base + 4 * i) if 0 <= i < len(words) else None

    def shape(i: int, reg: int):
        """loaded_from if words[..i] end in `adrp; ldr reg` (no join after the adrp)."""
        ldr, adrp = at(i), at(i - 1)
        if not (ldr and ldr[0] == "ldr" and ldr[1] == reg and adrp and adrp[0] == "adrp" and adrp[1] == ldr[2]):
            return None
        return ldr, adrp

    for i, w in enumerate(words):
        pc = base + 4 * i
        d = decode(w, pc)
        if d is None or d[0] not in ("blr", "br"):
            continue
        reg, src, first = d[1], None, None
        m = shape(i - 1, reg)
        if m:
            src, first = m[1][2] + m[0][3], i - 2
        elif d[0] == "br":
            add = at(i - 1)
            m = shape(i - 2, reg)
            if m and add and add[0] == "addi" and add[1] == add[2] == m[0][2] and add[1] != reg:
                src, first = m[1][2] + m[0][3], i - 3
        if first is not None and any(base + 4 * k in targets for k in range(first + 1, i + 1)):
            src = None  # something jumps into the middle: the register may come from elsewhere
        yield pc, d[0], src


def hook_table(elf: Elf, table_addr: int) -> list[tuple[int, int, int, int, int]]:
    head = elf.read(table_addr, HOOK_HEADER)
    magic, version, entry_size, count, applied = struct.unpack_from("<8sIIII", head)
    if magic != HOOK_MAGIC or version != 1 or entry_size != HOOK_ENTRY:
        raise LiftError(f"coop_hooks header is {magic!r} v{version} entry {entry_size}, want COOPHOOK v1 entry 32")
    if applied != 0:
        raise LiftError("coop_hooks.applied_by_host must be 0 in the file")
    body = elf.read(table_addr + HOOK_HEADER, count * HOOK_ENTRY)
    return [struct.unpack_from("<HxxIQQQ", body, i * HOOK_ENTRY) for i in range(count)]


def check(elf: Elf) -> tuple[list[str], list[tuple[int, str]]]:
    """Return (problems, entries)."""
    problems: list[str] = []
    for name in (".eh_frame", ".eh_frame_hdr"):
        sec = elf.section(name)
        if sec is None or sec.size == 0:
            problems.append(f"no {name}: build with unwind tables")
    dyn = {s.name: s for s in elf.symbols(SHT_DYNSYM) if s.defined}
    for need in ("_init", "coop_frame_entry", "coop_proto_frame", "coop_hooks"):
        if need not in dyn:
            problems.append(f"{need} is not exported")
    slots = {s.value: s.name for s in dyn.values() if s.name.startswith("coop_slot_")}
    if not slots:
        problems.append("no coop_slot_* exported")

    if "coop_hooks" in dyn:
        try:
            for i, (kind, _insn, main_off, repl, slot) in enumerate(hook_table(elf, dyn["coop_hooks"].value)):
                if kind not in KINDS:
                    problems.append(f"hook {i}: unknown kind {kind}")
                if slot not in slots:
                    problems.append(f"hook {i} (main+{main_off:#x}): slot {slot:#x} is not an exported coop_slot_*")
                if kind == 3 and repl != 0:
                    problems.append(f"hook {i} (main+{main_off:#x}): a game_fn entry has no replacement")
                if kind in (1, 2) and repl not in {s.value for s in dyn.values() if s.type == STT_FUNC}:
                    problems.append(f"hook {i} (main+{main_off:#x}): replacement {repl:#x} is not an exported function")
        except LiftError as e:
            problems.append(str(e))

    got = [s for s in elf.sections if s.name in (".got", ".got.plt")]
    text = elf.section(".text")
    if text is None:
        problems.append("no .text")
    else:
        names = sorted((s.value, s.name) for s in elf.symbols(SHT_SYMTAB) if s.type == STT_FUNC)
        for pc, kind, src in indirect_calls(elf.data[text.offset:text.offset + text.size], text.addr):
            if src in slots or any(g.addr <= (src or -1) < g.addr + g.size for g in got):
                continue
            where = max((n for n in names if n[0] <= pc), default=(0, "?"))[1]
            problems.append(f"{kind} at {pc:#x} (in {where}) does not go through a coop_slot_* or an import")

    entries = sorted((s.value, s.name) for s in dyn.values() if s.type == STT_FUNC)
    return problems, entries


def write_entries(path: str, entries: list[tuple[int, str]]) -> None:
    with open(path, "w", encoding="ascii", newline="\n") as f:
        for value, name in entries:
            f.write(f"{value:#x} {name}\n")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("elf")
    ap.add_argument("--entries")
    a = ap.parse_args(argv)
    try:
        problems, entries = check(Elf(open(a.elf, "rb").read()))
    except (OSError, LiftError) as e:
        print(f"lift_check: {e}", file=sys.stderr)
        return 1
    for p in problems:
        print(f"lift_check: {p}", file=sys.stderr)
    if problems:
        return 1
    if a.entries:
        write_entries(a.entries, entries)
    print(f"lift_check: ok, {len(entries)} exported functions")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
