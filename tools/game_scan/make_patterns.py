#!/usr/bin/env python3
"""Make the signature-scan patterns for lichii/hook/coop_game_scan.h from your own main.

Issue 97: co-op finds the main offsets of a game build by searching main's code for short masked
instruction patterns (docs/game-builds.md, "Signature scan"). This tool writes those patterns: for
every MainOffsets field it takes the 1.1.1 value from lichii/hook/coop_game_build.h, finds a spot in
your plain main NSO that leads to it (the function's first instructions, a call to it, or the
ADRP pair that loads a global), masks every bit a recompile or relink moves (branch targets, page
offsets, low 12 bits after an ADRP, the hooked words), and grows the window until it matches once.

Usage: python -I tools/game_scan/make_patterns.py <main.plain> [--check]
  writes lichii/hook/coop_game_scan_patterns.inc; with --check only scans and compares.

The main must be the plain NSO of build 1.1.1 from your own game (integration's tools/plain_nso
makes one). Nothing of the game goes into the repo except the masked patterns, a few words each.
"""
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TABLE = os.path.join(REPO, 'lichii', 'hook', 'coop_game_build.h')
OUT = os.path.join(REPO, 'lichii', 'hook', 'coop_game_scan_patterns.inc')

MIN_WORDS = 8   # short patterns match once in 1.1.1 but may hit a wrong spot in another build
MAX_WORDS = 24
# Words the loader patches before the game runs: never part of a pattern.
PATCHED = ('svc_hook_rare', 'svc_hook_frame', 'hold_hook')
# Placed by rule from where rodata starts, not by a pattern.
DERIVED = ('svc_cave', 'cave_end', 'hold_cave', 'hold_table')
FUNCTIONS = ('create_actor', 'malloc', 'request_delete', 'take_created', 'drop_create', 'play_as',
             'set_as_frame', 'sync_as_slot', 'equip', 'equip_change_state', 'unequip_armor',
             'set_matrix', 'set_time', 'cut_foliage', 'frame_step')
GLOBALS = ('actor_manager_ptr', 'game_flags_ptr', 'game_ui_module_ptr', 'scene_module_ptr',
           'banc_module_ptr', 'event_manager_ptr', 'world_mgr_ptr', 'foliage_module_ptr',
           'actor_vtable', 'got_create_default')


def load_table():
    text = open(TABLE, encoding='utf-8').read()
    fields = re.findall(r'std::uint64_t (\w+);', text.split('struct Build')[0])
    values = dict(re.findall(r'\.(\w+) = (0x[0-9a-f]+)', text))
    return fields, {k: int(v, 16) for k, v in values.items()}


def load_text(path):
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0':
        sys.exit('not an NSO: ' + path)
    if struct.unpack_from('<I', d, 0x0C)[0] & 0x87:
        sys.exit('compressed NSO: make a plain one first (integration tools/plain_nso)')
    off, mem, size = struct.unpack_from('<III', d, 0x10)
    ro_mem = struct.unpack_from('<I', d, 0x24)[0]
    text = np.frombuffer(d[off:off + size], dtype='<u4').copy()
    return d[0x40:0x48].hex(), text, ro_mem


def load_relative(path):
    """{slot: value} for main's R_AARCH64_RELATIVE relocations (what the loader writes)."""
    d = open(path, 'rb').read()
    mem = bytearray(0x6000000)
    for i in range(3):
        off, at, size = struct.unpack_from('<III', d, 0x10 + i * 0x10)
        mem[at:at + size] = d[off:off + size]
    mod0 = struct.unpack_from('<I', mem, 4)[0]
    dyn = mod0 + struct.unpack_from('<i', mem, mod0 + 4)[0]
    tags = {}
    while True:
        tag, val = struct.unpack_from('<QQ', mem, dyn)
        if tag == 0:
            break
        tags[tag] = val
        dyn += 16
    out = {}
    rela, size = tags.get(7, 0), tags.get(8, 0)
    for at in range(rela, rela + size, 24):
        where, info, addend = struct.unpack_from('<QQq', mem, at)
        if info & 0xffffffff == 1027:
            out[where] = addend
    # RELR: an address, then bitmaps of the next 63 words; the addend is in place.
    relr, size, where = tags.get(36, 0), tags.get(35, 0), 0
    for at in range(relr, relr + size, 8):
        e = struct.unpack_from('<Q', mem, at)[0]
        if e & 1 == 0:
            out[e] = struct.unpack_from('<Q', mem, e)[0]
            where = e + 8
        else:
            for bit in range(1, 64):
                if e >> bit & 1:
                    w = where + (bit - 1) * 8
                    out[w] = struct.unpack_from('<Q', mem, w)[0]
            where += 63 * 8
    return out


def mask_for(words):
    """Mask out what moves between builds: branch and page targets, lo12 after ADRP."""
    masks = []
    adrp_regs = set()
    for w in words:
        w = int(w)
        m = 0xffffffff
        if (w & 0x7c000000) == 0x14000000:      # B, BL
            m = 0xfc000000
        elif (w & 0xff000010) == 0x54000000:    # B.cond
            m = 0xff00001f
        elif (w & 0x7e000000) == 0x34000000:    # CBZ, CBNZ
            m = 0xff00001f
        elif (w & 0x7e000000) == 0x36000000:    # TBZ, TBNZ
            m = 0xfff8001f
        elif (w & 0x1f000000) == 0x10000000:    # ADR, ADRP
            m = 0x9f00001f
            if w & 0x80000000:
                adrp_regs.add(w & 31)
        elif (w & 0x3b000000) == 0x18000000:    # LDR literal
            m = 0xff00001f
        elif (w & 0x3b000000) == 0x39000000 and ((w >> 5) & 31) in adrp_regs:  # LDR/STR uimm
            m = 0xffc003ff
        elif (w & 0x7f000000) == 0x11000000 and ((w >> 5) & 31) in adrp_regs:  # ADD imm
            m = 0xffc003ff
        masks.append(m)
    return masks


def matches(text, vals, masks):
    n = len(vals)
    # Anchor on the word with the most fixed bits.
    a = max(range(n), key=lambda i: bin(masks[i]).count('1'))
    lim = len(text) - n + 1
    cand = np.nonzero((text[a:a + lim] & masks[a]) == vals[a])[0]
    for k in range(n):
        if k == a or masks[k] == 0 or len(cand) == 0:
            continue
        cand = cand[(text[cand + k] & masks[k]) == vals[k]]
    return cand


def window(text, start, n, zero=()):
    words = [int(w) for w in text[start:start + n]]
    masks = mask_for(words)
    for i in zero:
        if 0 <= i < n:
            masks[i] = 0
    vals = [w & m for w, m in zip(words, masks)]
    return vals, masks


def grow(text, start, at, zero=(), need=0):
    """Shortest window [at-back, ...) that matches only at `start`."""
    for n in range(max(MIN_WORDS, need), MAX_WORDS + 1):
        vals, masks = window(text, start, n, [z - start for z in zero])
        if sum(1 for m in masks if m) < 3:
            continue
        hits = matches(text, vals, masks)
        if len(hits) == 1 and hits[0] == start:
            return vals, masks
    return None


def bl_target(at, w):
    imm = w & 0x3ffffff
    if imm & 0x2000000:
        imm -= 0x4000000
    return at + imm * 4


def adrp_target(at, w):
    imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
    if imm & 0x100000:
        imm -= 0x200000
    return (at & ~0xfff) + (imm << 12)


def lo12(w):
    imm = (w >> 10) & 0xfff
    if (w & 0x7f000000) == 0x11000000:
        return imm
    return imm << ((w >> 30) & 3)   # LDR/STR uimm scaled by size


def find_xrefs(text, target):
    """(adrp word index, user word index) for ADRP + ADD/LDR/STR pairs that reach `target`."""
    idx = np.arange(len(text), dtype=np.int64)
    w = text.astype(np.int64)
    is_adrp = (w & 0x9f000000) == 0x90000000
    imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
    imm = np.where(imm & 0x100000, imm - 0x200000, imm)
    page = ((idx * 4) & ~0xfff) + (imm << 12)
    hits = np.nonzero(is_adrp & (page == (target & ~0xfff)))[0]
    out = []
    for i in hits:
        rd = int(text[i]) & 31
        for j in range(i + 1, min(i + 9, len(text))):
            u = int(text[j])
            if ((u & 0x3b000000) == 0x39000000 or (u & 0x7f000000) == 0x11000000) \
                    and ((u >> 5) & 31) == rd and adrp_target(i * 4, int(text[i])) + lo12(u) == target:
                out.append((int(i), j))
                break
    return out


def find_calls(text, target):
    w = text.astype(np.int64)
    idx = np.arange(len(text), dtype=np.int64)
    imm = w & 0x3ffffff
    imm = np.where(imm & 0x2000000, imm - 0x4000000, imm)
    hits = np.nonzero(((w & 0xfc000000) == 0x94000000) & (idx + imm == target // 4))[0]
    return [int(i) for i in hits]


def best(cands):
    cands = [c for c in cands if c]
    return min(cands, key=lambda c: len(c['vals'])) if cands else None


def got_users(text, relative, target):
    """(adrp, ldr, add) for GOT loads of `target - imm` followed by `add xd, xd, #imm`."""
    out = []
    for slot, value in relative.items():
        if not 0 <= target - value < 0x100:
            continue
        for a, u in find_xrefs(text, slot):
            rt = int(text[u]) & 31
            for k in range(u + 1, min(u + 20, len(text))):
                w = int(text[k])
                if (w & 0xffc00000) == 0x91000000 and ((w >> 5) & 31) == rt                         and value + ((w >> 10) & 0xfff) == target:
                    out.append((a, u, k))
                    break
    return out


def make(name, value, text, patched_words, relative, limit=40):
    if name in PATCHED:
        i = value // 4
        g = None
        for back in range(0, 12):
            g = grow(text, i - back, i, zero=patched_words)
            if g:
                return {'kind': 'Site', 'at': back, 'at2': 0, 'vals': g[0], 'masks': g[1]}
        return None
    cands = []
    if name in FUNCTIONS:
        g = grow(text, value // 4, value // 4, zero=patched_words)
        if g:
            cands.append({'kind': 'Site', 'at': 0, 'at2': 0, 'vals': g[0], 'masks': g[1]})
        for c in find_calls(text, value)[:limit]:
            for back in (0, 2, 4, 6):
                g = grow(text, c - back, c, zero=patched_words)
                if g:
                    cands.append({'kind': 'Call', 'at': back, 'at2': 0, 'vals': g[0], 'masks': g[1]})
                    break
    else:
        for a, u in find_xrefs(text, value)[:limit]:
            for back in (0, 2, 4):
                g = grow(text, a - back, a, zero=patched_words, need=u - a + back + 1)
                if g:
                    cands.append({'kind': 'Page', 'at': back, 'at2': back + u - a,
                                  'vals': g[0], 'masks': g[1]})
                    break
        if not cands:
            for a, u, k in got_users(text, relative, value)[:limit]:
                for back in (0, 2, 4):
                    g = grow(text, a - back, a, zero=patched_words, need=k - a + back + 1)
                    if g:
                        cands.append({'kind': 'Got', 'at': back, 'at2': back + u - a,
                                      'at3': back + k - a, 'vals': g[0], 'masks': g[1]})
                        break
    return best(cands)


def scan(p, text, relative):
    """The same rules as coop_game_scan.h: each pattern must match once."""
    hits = matches(text, p['vals'], p['masks'])
    if len(hits) != 1:
        return None, len(hits)
    s = int(hits[0])
    if p['kind'] == 'Site':
        return (s + p['at']) * 4, 1
    if p['kind'] == 'Call':
        i = s + p['at']
        return bl_target(i * 4, int(text[i])), 1
    i, j = s + p['at'], s + p['at2']
    page = adrp_target(i * 4, int(text[i])) + lo12(int(text[j]))
    if p['kind'] == 'Page':
        return page, 1
    # Got: the slot holds main + value (the loader's relocation), then add #imm.
    return relative.get(page, -1) + ((int(text[s + p['at3']]) >> 10) & 0xfff), 1


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__)
    fields, table = load_table()
    build, text, ro_mem = load_text(args[0])
    if build != 'e0f3ccfaf50065ae':
        sys.exit('patterns are made from build 1.1.1 (e0f3ccfaf50065ae), this main is ' + build)
    patched_words = [table[n] // 4 for n in PATCHED]
    relative = load_relative(args[0])
    pats = {}
    bad = []
    for name in fields:
        if name in DERIVED:
            continue
        p = make(name, table[name], text, patched_words, relative)
        got = scan(p, text, relative)[0] if p else None
        if got != table[name]:
            bad.append(name)
            continue
        pats[name] = p
        print(f'{name:22} {p["kind"]:4} {len(p["vals"]):2} words -> {got:#x}')
    print(f'rodata at {ro_mem:#x}, cave_end {table["cave_end"]:#x}')
    if bad:
        sys.exit('no unique pattern for: ' + ', '.join(bad))
    if '--check' in sys.argv:
        return
    lines = ['// Generated by tools/game_scan/make_patterns.py from build 1.1.1 (e0f3ccfaf50065ae).',
             '// Masked instruction patterns, value & mask per word; see coop_game_scan.h.',
             '// Do not edit by hand: run the tool again.']
    for name, p in pats.items():
        words = ', '.join(f'{{0x{v:08x}, 0x{m:08x}}}' for v, m in zip(p['vals'], p['masks']))
        lines.append(f'COOP_SCAN_PATTERN({name}, {p["kind"]}, {p["at"]}, {p["at2"]}, {p.get("at3", 0)}, {words})')
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote', os.path.relpath(OUT, REPO))


if __name__ == '__main__':
    main()
