#!/usr/bin/env python3
"""List the game's vtables with the class names the game itself gives them.

Most of main is built without C++ type information: only a few dozen classes (the standard library's)
have it. But the game has thousands of tiny functions that return a class's name (ADRP x0, ADD x0,
RET), and most classes keep one in their vtable. This tool:

1. finds the vtables: an offset-to-top word, a type info word (0 without type information), then
   pointers into code (or pure virtual imports);
2. names each from its type information when it has some, else from the name functions in its slots;
3. follows the functions of a small named vtable (a factory or info object) to the vtable they store
   into the object they make, so a screen made by a named factory gets the name too
   (UITitleScreen: its factory's vtable 0x84c10a50 names the screen's 0x84c106e8).

Usage: python -I tools/game_scan/vtables.py <main> [--name TEXT] [--vtable ADDR] [--all] [--base ADDR]
  <main>         the player's own main NSO (plain or compressed), e.g. a Longshot game folder's
                 .longshot_exefs/main
  default        one line per named vtable: address, slots, name, how it was named
  --name TEXT    only names containing TEXT (any case)
  --vtable ADDR  that vtable's slots, each with what is known about it
  --all          also the vtables with no name

Addresses are Longshot's (main at 0x80020000, nso.py). Nothing of the game goes into the repository:
the output is for reading, and only names and addresses go into docs.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import nso  # noqa: E402

RET = 0xD65F03C0
RETS = (RET, 0xD65F0BFF, 0xD65F0FFF)   # RET, RETAA, RETAB
NAME = re.compile(rb"[A-Za-z_][A-Za-z0-9_:<>,. ]{2,95}\0")
PURE = ("__cxa_pure_virtual", "__cxa_deleted_virtual")
SMALL = 8          # a vtable this small whose name function is in it is followed as a factory
FOLLOW_DEPTH = 2   # calls followed from a factory's function
FOLLOW_WORDS = 160  # instructions read per function


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def adrp_page(pc, w):
    imm = ((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3
    return (pc & ~0xFFF) + (sx(imm, 21) << 12)


def is_adrp(w):
    return w & 0x9F000000 == 0x90000000


def is_add_imm64(w):
    return w & 0xFFC00000 == 0x91000000   # ADD Xd, Xn, #imm12 (no shift)


def name_functions(m):
    """{function address: name} for every ADRP x0 / ADD x0, x0 / RET that returns an identifier-like string."""
    words = m.text_words()
    t0 = m.text[0]
    out = {}
    n = len(words) - 2
    for i in range(n):
        w = words[i]
        if w & 0x9F00001F != 0x90000000:
            continue
        a = words[i + 1]
        if a & 0xFFC003FF != 0x91000000 or words[i + 2] != RET:
            continue
        pc = t0 + 4 * i
        s = adrp_page(pc, w) + (a >> 10 & 0xFFF)
        o = s - m.base
        if 0 <= o < len(m.image):
            hit = NAME.match(m.image, o)
            if hit:
                out[pc] = hit.group()[:-1].decode()
    return out


def type_infos(m):
    """The addresses of type information objects (their first word is an import of a cxxabiv1 type_info vtable)."""
    return {slot for slot, name in m.imports.items() if "__cxxabiv1" in name and "type_info" in name}


def demangle(s):
    """A type info name made readable: St9exception -> std::exception, N3gmd3FooE -> gmd::Foo. Else as it is."""
    parts, i, nested = [], 0, False
    if s.startswith("N"):
        nested, i = True, 1
    while i < len(s):
        if s.startswith("St", i):
            parts.append("std")
            i += 2
            continue
        if nested and s[i] == "E" and i == len(s) - 1:
            i += 1
            break
        m = re.match(r"\d+", s[i:])
        if not m:
            return s
        n = int(m.group())
        i += len(m.group())
        parts.append(s[i:i + n])
        i += n
        if not nested:
            break
    return "::".join(parts) if i == len(s) and parts else s


class Vtable:
    __slots__ = ("addr", "top", "info", "slots", "names", "how")

    def __init__(self, addr, top, info, slots):
        self.addr, self.top, self.info, self.slots = addr, top, info, slots
        self.names = []     # [(slot index, name)] from name functions
        self.how = ""


def find_vtables(m):
    """Every vtable in ro and data, in address order."""
    lo, words = m.data_words()
    t0, t1 = m.text
    infos = type_infos(m)
    pure = {slot for slot, name in m.imports.items() if name.startswith(PURE)}

    def entry(k):
        v = words[k]
        return t0 <= v < t1 or (v == 0 and lo + 8 * k in pure)

    out = []
    n, k = len(words), 2
    while k < n:
        if not entry(k):
            k += 1
            continue
        info, top = words[k - 1], sx(words[k - 2], 64)
        a = lo + 8 * k
        if (info == 0 or info in infos) and not entry(k - 1) and not entry(k - 2) and -0x100000 <= top <= 0 \
                and a - 8 not in m.imports and a - 16 not in m.imports:
            start = k
            while k < n and entry(k):
                k += 1
            slots = [m.imports.get(lo + 8 * j) if words[j] == 0 else words[j] for j in range(start, k)]
            if len(slots) >= 2 or info:   # one code pointer after two zeros is as often a global as a vtable
                out.append(Vtable(a, top, info, slots))
        else:
            k += 1
    return out


def stored_vtables(m, fn, vt_addrs, words, depth, seen):
    """The vtable addresses a function stores into memory (in order, with the store's offset), calls followed."""
    out = []
    if fn in seen or not m.in_text(fn):
        return out
    seen.add(fn)
    t0 = m.text[0]
    regs = {}
    i = (fn - t0) // 4
    for k in range(i, min(i + FOLLOW_WORDS, len(words))):
        w = words[k]
        pc = t0 + 4 * k
        if is_adrp(w):
            regs[w & 31] = adrp_page(pc, w)
        elif is_add_imm64(w) and (w >> 5 & 31) in regs:
            regs[w & 31] = regs[w >> 5 & 31] + (w >> 10 & 0xFFF)
        elif w & 0xFFC00000 == 0xF9000000:   # STR Xt, [Xn, #imm]
            v = regs.get(w & 31)
            if v in vt_addrs:
                out.append((v, (w >> 10 & 0xFFF) * 8))
        elif w & 0xFC000000 in (0x94000000, 0x14000000):   # BL, B
            if depth > 0:
                target = pc + 4 * sx(w & 0x3FFFFFF, 26)
                out += stored_vtables(m, target, vt_addrs, words, depth - 1, seen)
            if w & 0xFC000000 == 0x14000000:
                break
        elif w in RETS:
            break
    return out


def name_all(m, vts):
    """Fill names and how: type information first, then name functions, then what named factories make."""
    names = name_functions(m)
    by_addr = {v.addr: v for v in vts}
    for v in vts:
        if v.info:
            s = m.cstr(m.u64(v.info + 8)) if m.in_image(v.info + 8) else None
            if s:
                v.how = "type info"
                v.names = [(-1, demangle(s))]
                continue
        v.names = [(i, names[s]) for i, s in enumerate(v.slots) if isinstance(s, int) and s in names]
        if v.names:
            v.how = "name function in slot " + ",".join(str(i) for i, _ in v.names)
    words = m.text_words()
    vt_addrs = set(by_addr)
    for v in vts:
        if not v.names or v.how == "type info" or len(v.slots) > SMALL:
            continue
        made = []
        named_slots = {i for i, _ in v.names}
        for i, s in enumerate(v.slots):
            if i in named_slots or not isinstance(s, int):
                continue
            made += [a for a, off in stored_vtables(m, s, vt_addrs, words, FOLLOW_DEPTH, set()) if off == 0]
        if made:
            # The class itself: the largest vtable stored at offset 0 (a class has at least its bases' slots; a
            # member's small vtable is stored after it), the last of equals (bases are stored first).
            order = {a: i for i, a in enumerate(made)}
            target = by_addr[max(set(made), key=lambda a: (len(by_addr[a].slots), order[a]))]
            if target is not v and not target.names:
                target.names = [(-1, v.names[0][1])]
                target.how = f"made by 0x{v.addr:x}"
    return names


def show_vtable(m, v, names):
    print(f"0x{v.addr:x}  offset to top {v.top}  type info 0x{v.info:x}  {len(v.slots)} slots  "
          f"{', '.join(n for _, n in v.names) or '(no name)'}  {v.how}")
    for i, s in enumerate(v.slots):
        if isinstance(s, int):
            note = f"name function: {names[s]}" if s in names else ""
            print(f"  [{i:3}] 0x{v.addr + 8 * i:x}  f_{s:x}  {note}".rstrip())
        else:
            print(f"  [{i:3}] 0x{v.addr + 8 * i:x}  import {s}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("main", help="the player's own main NSO")
    ap.add_argument("--name", help="only names containing this text")
    ap.add_argument("--vtable", help="show this vtable's slots")
    ap.add_argument("--all", action="store_true", help="also vtables with no name")
    ap.add_argument("--base", default=hex(nso.BASE), help="main's load address (default 0x80020000)")
    a = ap.parse_args(argv)
    m = nso.Main.load(a.main, int(a.base, 16))
    vts = find_vtables(m)
    names = name_all(m, vts)
    if a.vtable:
        want = nso.parse_addr(a.vtable, m.base)
        for v in vts:
            if v.addr <= want < v.addr + 8 * max(1, len(v.slots)):
                show_vtable(m, v, names)
                return 0
        print(f"no vtable at 0x{want:x}", file=sys.stderr)
        return 1
    shown = 0
    for v in vts:
        label = ", ".join(n for _, n in v.names)
        if not label and not a.all:
            continue
        if a.name and a.name.lower() not in label.lower():
            continue
        print(f"0x{v.addr:x}\t{len(v.slots)}\t{label or '-'}\t{v.how or '-'}")
        shown += 1
    named = sum(1 for v in vts if v.names)
    print(f"# {len(vts)} vtables, {named} named, {shown} shown; {len(names)} name functions; build {m.build_id[:16]}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
