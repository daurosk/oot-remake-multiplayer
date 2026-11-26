#!/usr/bin/env python3
"""Find what in the game's main uses an address: who calls a function, who loads a global or a string.

For each address (or range with --size) it lists:
- call / jump: a BL or B to it;
- address / read / write: an ADRP followed by an ADD, or by a load or store, that comes to it
  (the usual way code reaches a global, a vtable or a string); also ADR and LDR (literal);
- via pointer: a load of an 8-byte slot (a GOT entry, a global pointer) that holds it;
- pointer: a slot in ro or data that holds it, with the vtable and slot index when the slot is in one.
Each code hit says the function it is in: from Longshot's functions.txt with --functions, else the
nearest call target or vtable entry before it (marked "near").

Usage: python -I tools/game_scan/callers.py <main> ADDR [ADDR ...] [--size N] [--functions FILE] [--base ADDR]
  ADDR  0x84c106e8, 84c106e8, f_8088ffc0 or main+0x3406710 (main at 0x80020000, nso.py)
  --size N           a range: ADDR .. ADDR+N (a struct, a table)
  --functions FILE   Longshot's work/sa/gen/functions.txt (start and size per line, hex)
  --no-vtables       skip naming the vtables of pointer hits (a little faster)

Nothing of the game goes into the repository: the output is for reading, and only names and
addresses go into docs.
"""
import argparse
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import nso  # noqa: E402
import vtables  # noqa: E402

WINDOW = 8   # instructions after an ADRP that may use its page


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Targets:
    def __init__(self, ranges):
        self.ranges = ranges
        self.pages = set()
        for lo, hi in ranges:
            if hi - lo <= 64 * nso.PAGE:
                self.pages.update(range(lo & ~0xFFF, hi, nso.PAGE))
            else:
                self.pages = None
                break

    def page_hit(self, page):
        if self.pages is None:
            return any(lo & ~0xFFF <= page < hi for lo, hi in self.ranges)
        return page in self.pages

    def hit(self, a):
        for lo, hi in self.ranges:
            if lo <= a < hi:
                return lo
        return None


def is_cas(w):
    """Whether a load/store instruction is a compare and swap: CAS (o2 bit 23 and o1 bit 21 set) or CASP (o1
    set, o2 clear, size 0x; with size 1x it is STXP or LDXP)."""
    return w & 0x3F200000 == 0x08200000 and (w >> 23 & 1 == 1 or w >> 31 == 0)


def is_store(w):
    """Whether a load/store instruction stores (its Rt is read, not written). A compare and swap stores Rt (the
    old value goes to Rs); an atomic (LDADD, SWP) loads the old value into Rt."""
    if w & 0x0A000000 != 0x08000000:
        return False                       # not a load or store
    if is_cas(w):
        return True
    if w & 0x3A000000 == 0x28000000 or w & 0x3F000000 == 0x08000000:
        return not w >> 22 & 1             # pair, or exclusive/ordered: L is bit 22
    if w & 0x3B200C00 == 0x38200000:
        return False                       # atomic: bits 23 and 22 are acquire and release, not opc
    if w & 0x38000000 == 0x38000000:       # register: unsigned immediate, unscaled, indexed, register offset
        v, opc = w >> 26 & 1, w >> 22 & 3
        return opc & 1 == 0 if v else opc == 0   # SIMD: opc 0 and 2 store (STR b/h/s/d, STR q)
    return False


def loaded(w):
    """The X registers a load/store instruction may write: what it loads (an atomic's or a compare and swap's
    old value too), a store exclusive's status, and its base register when it writes it back."""
    rt, rt2, rn, rs = w & 31, w >> 10 & 31, w >> 5 & 31, w >> 16 & 31
    if is_cas(w):
        return {rs} if w >> 23 & 1 else {rs, rs + 1}   # CASP: Rs and Rs+1
    if w & 0x3F000000 == 0x08000000:       # exclusive or ordered: L is bit 22, o2 bit 23, o1 bit 21
        if w >> 22 & 1:
            return {rt, rt2} if w >> 21 & 1 else {rt}   # LDXR, LDAXR, LDAR; LDXP and LDAXP load Rt2 too
        return set() if w >> 23 & 1 else {rs}           # STLR; STXR and STXP put their status in Ws
    regs = set() if is_store(w) or w >> 26 & 1 else {rt}   # a SIMD load writes V registers, not X
    if w & 0x3A000000 == 0x28000000:       # pair: LDP, LDPSW and LDNP load Rt2 too
        if regs:
            regs.add(rt2)
        if w >> 23 & 1:
            regs.add(rn)                   # pre or post index: Rn is written back
    elif w & 0x3B200400 == 0x38000400 or w & 0xBE800000 == 0x0C800000:
        regs.add(rn)                       # one register or SIMD structures, pre or post index: Rn is written back
    return regs


def writes(w, rd):
    """Whether an instruction may put a new value in X register rd (so it no longer holds an ADRP page)."""
    if w & 0x0A000000 == 0x08000000:       # loads and stores
        return rd in loaded(w)
    if w & 0x1C000000 == 0x14000000:       # branches, exceptions, system
        return False
    return w & 31 == rd                    # data processing: Rd


def ls_unsigned(w):
    """(kind, scale, imm12) for a load or store with an unsigned immediate, else None."""
    if w & 0x3B000000 != 0x39000000:
        return None
    size, v, opc = w >> 30, w >> 26 & 1, w >> 22 & 3
    scale = 4 if v and opc & 2 else size
    kind = "write" if is_store(w) else "read"
    return kind, scale, w >> 10 & 0xFFF


def scan_code(m, t):
    """[(pc, kind, target, slot)] and the call targets seen (for the function starts)."""
    words = m.text_words()
    t0 = m.text[0]
    hits, calls = [], set()
    n = len(words)
    for i in range(n):
        w = words[i]
        op = w & 0xFC000000
        if op == 0x94000000 or op == 0x14000000:   # BL, B
            pc = t0 + 4 * i
            dest = pc + 4 * sx(w & 0x3FFFFFF, 26)
            if op == 0x94000000:
                calls.add(dest)
            lo = t.hit(dest)
            if lo is not None:
                hits.append((pc, "call" if op == 0x94000000 else "jump", dest, None))
            continue
        top = w & 0x9F000000
        if top == 0x90000000:   # ADRP
            pc = t0 + 4 * i
            page = (pc & ~0xFFF) + (sx(((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3, 21) << 12)
            rd = w & 31
            for j in range(i + 1, min(i + 1 + WINDOW, n)):
                u = words[j]
                if (u >> 5) & 31 != rd:
                    if u & 0xFC000000 in (0x94000000, 0x14000000) or u == vtables.RET or writes(u, rd):
                        break   # a branch, or rd now holds something else (mov x8, x0)
                    continue
                if u & 0xFF800000 == 0x91000000:   # ADD Xd, Xn, #imm{, lsl 12}
                    a = page + ((u >> 10 & 0xFFF) << (12 if u >> 22 & 1 else 0))
                    if t.hit(a) is not None:
                        hits.append((t0 + 4 * j, "address", a, None))
                    if u & 31 == rd:
                        break   # the register no longer holds the page
                    continue
                ls = ls_unsigned(u)
                if ls:
                    kind, scale, imm = ls
                    a = page + (imm << scale)
                    if t.page_hit(page) and t.hit(a) is not None:
                        hits.append((t0 + 4 * j, kind, a, None))
                    elif kind == "read" and scale == 3 and m.in_image(a):
                        v = m.u64(a)
                        if t.hit(v) is not None:
                            hits.append((t0 + 4 * j, "via pointer", v, a))
                    if kind == "read" and u & 31 == rd:
                        break
                elif writes(u, rd):
                    break
            continue
        if top == 0x10000000:   # ADR
            pc = t0 + 4 * i
            a = pc + sx(((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3, 21)
            if t.hit(a) is not None:
                hits.append((pc, "address", a, None))
            continue
        if w & 0xBF000000 == 0x18000000:   # LDR (literal), 32 or 64-bit
            pc = t0 + 4 * i
            a = pc + 4 * sx((w >> 5) & 0x7FFFF, 19)
            if t.hit(a) is not None:
                hits.append((pc, "read", a, None))
    return hits, calls


def scan_data(m, t):
    """[(slot, value)] for every 8-byte slot in ro and data that holds a target address."""
    lo, words = m.data_words()
    out = []
    for k, v in enumerate(words):
        if t.hit(v) is not None:
            out.append((lo + 8 * k, v))
    return out


def load_functions(path):
    starts = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            try:
                starts.append(int(parts[0], 16))
            except (IndexError, ValueError):
                continue   # an empty line, a comment or a header
    return sorted(starts)


def where(starts, pc, exact):
    k = bisect.bisect_right(starts, pc) - 1
    if k < 0:
        return ""
    f = starts[k]
    return f"{'in' if exact else 'near'} f_{f:x}+0x{pc - f:x}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("main", help="the player's own main NSO")
    ap.add_argument("addr", nargs="+", help="addresses to look for")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=1, help="look for ADDR .. ADDR+SIZE")
    ap.add_argument("--functions", help="Longshot's functions.txt, for the function each hit is in")
    ap.add_argument("--no-vtables", action="store_true", help="do not name the vtables of pointer hits")
    ap.add_argument("--base", default=hex(nso.BASE), help="main's load address (default 0x80020000)")
    a = ap.parse_args(argv)
    m = nso.Main.load(a.main, int(a.base, 16))
    ranges = [(x, x + max(1, a.size)) for x in (nso.parse_addr(s, m.base) for s in a.addr)]
    t = Targets(ranges)
    code, calls = scan_code(m, t)
    data = scan_data(m, t)
    vts = []
    if data and not a.no_vtables:
        vts = vtables.find_vtables(m)
        vtables.name_all(m, vts)
    if a.functions:
        starts, exact = load_functions(a.functions), True
    else:
        starts = set(calls)
        for v in vts:
            starts.update(s for s in v.slots if isinstance(s, int))
        starts, exact = sorted(starts), False
    vts = sorted(vts, key=lambda v: v.addr)
    vt_addrs = [v.addr for v in vts]
    for pc, kind, target, slot in sorted(code):
        extra = f" (slot 0x{slot:x})" if slot is not None else ""
        print(f"0x{pc:x}\t{kind}\t0x{target:x}{extra}\t{where(starts, pc, exact)}")
    for slot, value in data:
        note = ""
        k = bisect.bisect_right(vt_addrs, slot) - 1
        if k >= 0:
            v = vts[k]
            if v.addr <= slot < v.addr + 8 * len(v.slots):
                name = ", ".join(n for _, n in v.names) or "no name"
                note = f"vtable 0x{v.addr:x} ({name}) slot {(slot - v.addr) // 8}"
        print(f"0x{slot:x}\tpointer\t0x{value:x}\t{note}".rstrip())
    print(f"# {len(code)} code hits, {len(data)} pointers; build {m.build_id[:16]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
