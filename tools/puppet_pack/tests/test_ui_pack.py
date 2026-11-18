"""Tests for the UI pack library (ui_pack.py). Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_ui_pack.py
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import msbt, sarc, ui_pack as up  # noqa: E402
from puppet_pack.ui_pack import Copy, UiPackError  # noqa: E402

ROOT = 0x100
PART_KIND, NULL_KIND = 0x02555313, 0x02555003

# A menu shaped as the game's pause menu layout: four tabs under their own null pane, which ends
# before the last pane; a gauge; a footer with two guides, the last pane.
MENU = [("RootPane", -1), ("N_All_00", 0), ("N_Tabs_00", 1), ("Pa_Tab_00", 2), ("Pa_Tab_01", 2),
        ("Pa_Tab_02", 2), ("Pa_Tab_03", 2), ("N_Gauge_00", 1), ("Pa_Gauge_00", 7), ("N_Footer_00", 0),
        ("Pa_Guide_00", 9), ("Pa_Guide_01", 9)]
MENU_PARTS = {"Pa_Tab_%02d" % i: ("PaTab_00", [("State", float(i)), ("TextIn", 15.0)]) for i in range(4)}
MENU_PARTS.update({"Pa_Gauge_00": ("PaGauge_00", []), "Pa_Guide_00": ("PaGuide_00", []),
                   "Pa_Guide_01": ("PaGuide_00", [])})
MENU_ENDS = [12, 9, 7, 4, 5, 6, 7, 9, 9, 12, 11, 12]


def make_layout(tree=MENU, parts=MENU_PARTS, overrides=(), props=()):
    """A Layout2 layout built here: header, Default and StringPool sections, the UIL root with its
    pane and part dictionaries (its other dictionaries left empty), pane records in pre-order, part
    records each followed by its parameters and its properties list, and the strings at the end.
    tree: [(name, parent index)]; parts: {pane: (part layout, [(parameter, value)])}; the parts
    named in `overrides` get an override list (+0x08), those in `props` a properties list."""
    n = len(tree)
    ends = [i + 1 for i in range(n)]
    for i in reversed(range(n)):
        if tree[i][1] >= 0:
            ends[tree[i][1]] = max(ends[tree[i][1]], ends[i])
    pool, pool_at = bytearray(), {}

    def string(s):
        if s not in pool_at:
            pool_at[s] = len(pool)
            pool.extend(s.encode() + b"\0")
        return pool_at[s]

    body = bytearray(0x300)
    fix = []   # (position, string offset in the pool)

    def alloc(size):
        while len(body) % 8:
            body.append(0)
        at = len(body)
        body.extend(b"\0" * size)
        return at

    def ref(at, s):
        fix.append((at, string(s)))

    part_names = [name for name, _ in tree if name in parts]
    keys, vals, ids = alloc(8 * n), alloc(8 * n), alloc(8 * n)
    for i, (name, parent) in enumerate(tree):
        rec = alloc(0x44)
        ref(keys + 8 * i, name)
        struct.pack_into("<Q", body, vals + 8 * i, rec)
        struct.pack_into("<Q", body, ids + 8 * i, 0x1111 * (i + 1))
        struct.pack_into("<ff", body, rec + 0x10, 1.0, 1.0)
        struct.pack_into("<ff", body, rec + 0x1c, 100.0 * i, -10.0 * i)
        struct.pack_into("<I", body, rec + 0x28, parent & 0xFFFFFFFF)
        struct.pack_into("<I", body, rec + 0x34, ends[i])
        struct.pack_into("<I", body, rec + 0x38, PART_KIND if name in parts else NULL_KIND)
        struct.pack_into("<HHHH", body, rec + 0x3c, 0xFFFF, 0xFFFF, 0xFFFF,
                         part_names.index(name) if name in parts else 0)
    struct.pack_into("<QQQII", body, ROOT + 0x18, keys, vals, ids, n, 8)
    m = len(part_names)
    pkeys, pvals, pids = alloc(8 * m), alloc(8 * m), alloc(8 * m)
    names = [name for name, _ in tree]
    for j, name in enumerate(part_names):
        layout, params = parts[name]
        rec = alloc(0x48)
        ref(pkeys + 8 * j, name)
        struct.pack_into("<Q", body, pvals + 8 * j, rec)
        struct.pack_into("<Q", body, pids + 8 * j, 0x1111 * (names.index(name) + 1))
        ref(rec, layout)
        if name in overrides:
            struct.pack_into("<QII", body, rec + 0x08, rec, 1, 8)
        if params:
            arr = alloc(8 * len(params))
            struct.pack_into("<QII", body, rec + 0x28, arr, len(params), 8)
            for k, (pname, value) in enumerate(params):
                e = alloc(16)
                struct.pack_into("<Q", body, arr + 8 * k, e)
                ref(e, pname)
                struct.pack_into("<f", body, e + 8, value)
        plist = alloc(16)
        if name in props:
            struct.pack_into("<QII", body, plist, rec, 1, 8)
        struct.pack_into("<Q", body, rec + 0x38, plist)
        struct.pack_into("<Q", body, rec + 0x40, names.index(name))
    struct.pack_into("<QQQII", body, ROOT + 0xb8, pkeys, pvals, pids, m, 8)
    while len(body) % 8:
        body.append(0)
    pool_off = len(body)
    ref(ROOT + 0x10, "Menu_00")
    for at, s in fix:
        struct.pack_into("<Q", body, at, pool_off + s)
    d = body + pool
    d[0:4] = b"BFFH"
    d[0x28:0x28 + 35] = b"Nintendo.Ui.ResModels.LayoutResData"
    d[0xa8:0xac] = b"BFSI"
    d[0xc0:0xc7] = b"Default"
    d[0xd0:0xd4] = b"BFSI"
    d[0xe8:0xf2] = b"StringPool"
    d[0xf8:0xfb] = b"UIL"
    struct.pack_into("<II", d, 0xac, 0xf8, pool_off - 0xf8)
    struct.pack_into("<II", d, 0xd4, pool_off, len(d) - pool_off)
    struct.pack_into("<I", d, 0x08, len(d))
    return bytes(d)


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def q(d, o):
    return struct.unpack_from("<Q", d, o)[0]


def cstr(d, o):
    return d[o:d.index(b"\0", o)].decode()


def params(d, part_rec):
    """[(name, value, entry offset)] of a part record's parameters, and the list's offset."""
    arr, n = q(d, part_rec + 0x28), u32(d, part_rec + 0x30)
    out = []
    for k in range(n):
        e = q(d, arr + 8 * k)
        out.append((cstr(d, q(d, e)), struct.unpack_from("<f", d, e + 8)[0], e))
    return out, arr


class LayoutTest(unittest.TestCase):
    def assert_rest_unchanged(self, old, new, bumped=()):
        """Every byte the old layout had is the same in the new one, but for the header's size, the
        string pool's size, the root's pane and part dictionaries and the ends of the panes in
        `bumped`; every old pane and part reads the same (name, record, id)."""
        allowed = [(8, 12), (ROOT + 0x18, ROOT + 0x38), (ROOT + 0xb8, ROOT + 0xd8)]
        hdr = up.sections(old)["StringPool"][0]
        allowed.append((hdr + 8, hdr + 12))
        panes, parts = up.read_layout(old)
        allowed += [(panes[i].record + 0x34, panes[i].record + 0x38) for i in bumped]
        self.assertGreater(len(new), len(old))
        for at in range(len(old)):
            if old[at] != new[at]:
                self.assertTrue(any(a <= at < b for a, b in allowed), "byte %#x changed" % at)
        npanes, nparts = up.read_layout(new)
        self.assertEqual(npanes[:len(panes)], panes)
        self.assertEqual(nparts[:len(parts)], parts)
        sec = up.sections(new)["StringPool"]
        self.assertEqual(sec[1] + sec[2], len(new))
        self.assertEqual(u32(new, 8), len(new))
        self.assertEqual(len(new) % 8, 0)

    def test_reads_panes_and_parts(self):
        d = make_layout()
        panes, parts = up.read_layout(d)
        self.assertEqual([p.name for p in panes], [n for n, _ in MENU])
        self.assertEqual([p.name for p in parts], [n for n, _ in MENU if n in MENU_PARTS])
        self.assertEqual([u32(d, p.record + 0x34) for p in panes], MENU_ENDS)
        self.assertEqual(panes[3].id, parts[0].id)

    def test_refuses_what_is_not_a_layout(self):
        for bad in (b"BFFH" + b"\0" * 0x200, b"SARC" + b"\0" * 0x200, make_layout()[:0x150]):
            with self.assertRaises(UiPackError):
                up.read_layout(bad)

    def test_copy_of_the_last_pane_keeps_the_order(self):
        d = make_layout()
        n = up.clone_part(d, "Pa_Guide_01", "Pa_Guide_02", move=(0.0, -50.0))
        panes, parts = up.read_layout(n)
        self.assertEqual(len(panes), len(MENU) + 1)
        self.assertEqual(len(parts), len(MENU_PARTS) + 1)
        self.assertEqual((panes[-1].name, parts[-1].name), ("Pa_Guide_02", "Pa_Guide_02"))
        self.assertEqual(panes[-1].id, parts[-1].id)
        self.assertEqual(panes[-1].id, up.part_id("Pa_Guide_02"))
        new = panes[-1].record
        src = panes[11].record
        self.assertEqual(u32(n, new + 0x28), 9)                    # the footer's child
        self.assertEqual(u32(n, new + 0x34), 13)                   # its own subtree
        self.assertEqual(struct.unpack_from("<H", n, new + 0x42)[0], len(MENU_PARTS))
        self.assertEqual(struct.unpack_from("<ff", n, new + 0x1c), (1100.0, -160.0))
        self.assertEqual(n[new:new + 0x1c], n[src:src + 0x1c])
        self.assertEqual(n[new + 0x24:new + 0x34], n[src + 0x24:src + 0x34])
        self.assertEqual(n[new + 0x38:new + 0x42], n[src + 0x38:src + 0x42])
        self.assertEqual(q(n, parts[-1].record + 0x40), len(MENU))
        self.assertEqual(cstr(n, q(n, parts[-1].record)), "PaGuide_00")
        ends = [u32(n, p.record + 0x34) for p in panes]
        self.assertEqual(ends[:12], [13, 9, 7, 4, 5, 6, 7, 9, 9, 13, 11, 12])   # root and footer reach it
        self.assert_rest_unchanged(d, n, bumped=(0, 9))

    def test_copy_inside_the_tree_needs_out_of_order(self):
        d = make_layout()
        with self.assertRaises(UiPackError) as e:
            up.clone_part(d, "Pa_Tab_03", "Pa_Tab_04", move=(460.0, 0.0))
        self.assertIn("out_of_order", str(e.exception))
        n = up.clone_part(d, "Pa_Tab_03", "Pa_Tab_04", move=(460.0, 0.0), out_of_order=True)
        panes, parts = up.read_layout(n)
        self.assertEqual([p.name for p in panes], [m for m, _ in MENU] + ["Pa_Tab_04"])
        self.assertEqual(parts[-1].name, "Pa_Tab_04")
        new = panes[-1].record
        self.assertEqual(u32(n, new + 0x28), 2)                    # the tabs' pane, by index
        self.assertEqual(u32(n, new + 0x34), 13)
        self.assertEqual(struct.unpack_from("<ff", n, new + 0x1c), (1060.0, -60.0))
        ends = [u32(n, p.record + 0x34) for p in panes]
        # only the root reaches the copy: the tabs' pane ends where it did, and the footer, which
        # reaches the last pane, is not the copy's parent
        self.assertEqual(ends[:12], [13] + MENU_ENDS[1:])
        self.assert_rest_unchanged(d, n, bumped=(0,))

    def test_the_copy_has_its_own_parameters_and_properties(self):
        d = make_layout()
        n = up.clone_part(d, "Pa_Tab_02", "Pa_Tab_04", params={"State": 4.0}, out_of_order=True)
        panes, parts = up.read_layout(n)
        src, new = parts[2].record, parts[-1].record
        (old_params, old_arr), (new_params, new_arr) = params(n, src), params(n, new)
        self.assertEqual([(p, v) for p, v, _ in old_params], [("State", 2.0), ("TextIn", 15.0)])
        self.assertEqual([(p, v) for p, v, _ in new_params], [("State", 4.0), ("TextIn", 15.0)])
        self.assertGreaterEqual(new_arr, len(d))
        self.assertTrue(all(e >= len(d) for _, _, e in new_params))
        for (_, _, a), (_, _, b) in zip(old_params, new_params):
            self.assertEqual(q(n, a), q(n, b))                     # the names: the same strings
        self.assertEqual(n[new + 0x30:new + 0x38], n[src + 0x30:src + 0x38])
        self.assertNotEqual(q(n, new + 0x38), q(n, src + 0x38))
        self.assertGreaterEqual(q(n, new + 0x38), len(d))
        self.assertEqual(n[q(n, new + 0x38):q(n, new + 0x38) + 16], b"\0" * 16)
        self.assert_rest_unchanged(d, n, bumped=(0,))

    def test_several_copies_at_once(self):
        d = make_layout()
        n = up.clone_parts(d, [Copy("Pa_Guide_01", "Pa_Guide_02", (0.0, -50.0)),
                               ("Pa_Guide_01", "Pa_Guide_03", (0.0, -100.0))])
        panes, parts = up.read_layout(n)
        self.assertEqual([p.name for p in panes[-2:]], ["Pa_Guide_02", "Pa_Guide_03"])
        self.assertEqual([p.name for p in parts[-2:]], ["Pa_Guide_02", "Pa_Guide_03"])
        self.assertEqual([u32(n, p.record + 0x34) for p in panes], [14] + MENU_ENDS[1:9] + [14, 11, 12, 13, 14])
        self.assertEqual([q(n, p.record + 0x40) for p in parts[-2:]], [12, 13])
        self.assertEqual([struct.unpack_from("<H", n, p.record + 0x42)[0] for p in panes[-2:]], [7, 8])
        self.assertEqual(len({p.id for p in panes}), len(panes))
        self.assert_rest_unchanged(d, n, bumped=(0, 9))

    def test_a_copy_of_a_copy(self):
        d = up.clone_part(make_layout(), "Pa_Tab_03", "Pa_Tab_04", move=(460.0, 0.0), out_of_order=True)
        n = up.clone_part(d, "Pa_Tab_04", "Pa_Tab_05", move=(460.0, 0.0), out_of_order=True)
        panes, parts = up.read_layout(n)
        self.assertEqual([p.name for p in panes[-2:]], ["Pa_Tab_04", "Pa_Tab_05"])
        self.assertEqual(struct.unpack_from("<f", n, panes[-1].record + 0x1c)[0], 1520.0)
        self.assertEqual(u32(n, panes[0].record + 0x34), 14)
        self.assert_rest_unchanged(d, n, bumped=(0,))

    def test_ids_from_the_caller(self):
        n = up.clone_part(make_layout(), "Pa_Guide_01", "Pa_Guide_02", new_id=lambda name: 0xABCDEF)
        panes, parts = up.read_layout(n)
        self.assertEqual((panes[-1].id, parts[-1].id), (0xABCDEF, 0xABCDEF))
        with self.assertRaises(UiPackError):
            up.clone_part(make_layout(), "Pa_Guide_01", "Pa_Guide_02", new_id=lambda name: 0x1111)

    def test_refusals(self):
        d = make_layout()
        bad = [
            dict(source="Pa_Nothing_00", name="Pa_X_00"),           # no such pane
            dict(source="N_Tabs_00", name="Pa_X_00"),               # not a part
            dict(source="Pa_Guide_01", name="Pa_Tab_00"),           # the name is taken
            dict(source="Pa_Guide_01", name="N_All_00"),
            dict(source="Pa_Guide_01", name="Pa Guide"),            # not a pane name
            dict(source="Pa_Guide_01", name=""),
            dict(source="Pa_Guide_01", name="Pa_Guïde_02"),
            dict(source="Pa_Guide_01", name="Pa_Guide_02", params={"State": 1.0}),   # no such parameter
            dict(source="Pa_Guide_01", name="Pa_Guide_02", move=(1.0,)),
            dict(source="Pa_Guide_01", name="Pa_Guide_02", move=("1", 0.0)),
            dict(source="Pa_Guide_01", name="Pa_Guide_02", move=(1e300, 0.0)),
            dict(source="Pa_Guide_01", name="Pa_Guide_02", move=5.0),
            dict(source="Pa_Tab_03", name="Pa_Tab_04", params={"State": "4"}, out_of_order=True),
            dict(source="Pa_Tab_03", name="Pa_Tab_04", params={"State": float("nan")}, out_of_order=True),
        ]
        for kw in bad:
            with self.subTest(**{k: str(v) for k, v in kw.items()}), self.assertRaises(UiPackError):
                up.clone_part(d, **kw)
        with self.assertRaises(UiPackError):   # the same new name twice
            up.clone_parts(d, [("Pa_Guide_01", "Pa_Guide_02"), ("Pa_Guide_01", "Pa_Guide_02")])
        with self.assertRaises(UiPackError):   # something after the string pool
            up.clone_part(d + b"\0" * 8, "Pa_Guide_01", "Pa_Guide_02")
        for kw in (dict(overrides=("Pa_Guide_01",)), dict(props=("Pa_Guide_01",))):
            with self.subTest(**kw), self.assertRaises(UiPackError):
                up.clone_part(make_layout(**kw), "Pa_Guide_01", "Pa_Guide_02")
        tree = MENU + [("N_Under_00", 11)]   # the last guide with a pane under it
        with self.assertRaises(UiPackError):
            up.clone_part(make_layout(tree), "Pa_Guide_01", "Pa_Guide_02", out_of_order=True)
        self.assertEqual(up.clone_parts(d, []), d)

    def test_every_copy_in_an_archive(self):
        layout = make_layout()
        archive = sarc.write([("Menu_00.buil", layout)] * 4 + [("Other_00.buil", b"other")], data_align=16)
        calls = []

        def edit(data):
            calls.append(data)
            return up.clone_part(data, "Pa_Tab_03", "Pa_Tab_04", move=(460.0, 0.0), out_of_order=True)

        entries, done = up.edit_entries(sarc.read_entries(archive), "Menu_00.buil", edit)
        self.assertEqual(len(calls), 1)                            # equal copies: edited once
        self.assertEqual(list(done), [layout])
        back = sarc.read_entries(sarc.write(entries, data_align=16))
        layouts = [data for name, data in back if name == "Menu_00.buil"]
        self.assertEqual(len(layouts), 4)
        self.assertTrue(all(up.read_layout(x)[1][-1].name == "Pa_Tab_04" for x in layouts))
        self.assertIn(("Other_00.buil", b"other"), back)
        same, none = up.edit_entries(entries, "Missing_00.buil", edit)
        self.assertEqual((same, none), (entries, {}))

    def test_copy_under_a_pane_that_reaches_the_last_pane(self):
        d = make_layout()
        off = up.frame_offset(d, "Pa_Tab_03", "RootPane")
        self.assertEqual(off, (300.0, -30.0))                      # N_Tabs_00 (200, -20) + N_All_00 (100, -10)
        n = up.clone_parts(d, [Copy("Pa_Tab_03", "Pa_Tab_04", (100.0 + off[0], off[1]), parent="RootPane")])
        panes, parts = up.read_layout(n)
        new = panes[-1].record
        self.assertEqual(u32(n, new + 0x28), 0)                    # the root's child
        self.assertEqual(u32(n, new + 0x34), 13)
        self.assertEqual(struct.unpack_from("<ff", n, new + 0x1c), (1000.0, -90.0))   # one step on, root's frame
        self.assertEqual(parts[-1].name, "Pa_Tab_04")
        ends = [u32(n, p.record + 0x34) for p in panes]
        self.assertEqual(ends[:12], [13] + MENU_ENDS[1:])
        self.assert_rest_unchanged(d, n, bumped=(0,))
        # the footer reaches the last pane too: a copy there keeps the order, and the footer reaches it
        f = up.clone_parts(d, [Copy("Pa_Tab_03", "Pa_Tab_04", parent="N_Footer_00")])
        fp = up.read_layout(f)[0]
        self.assertEqual(u32(f, fp[-1].record + 0x28), 9)
        self.assertEqual([u32(f, p.record + 0x34) for p in fp][:12], [13, 9, 7, 4, 5, 6, 7, 9, 9, 13, 11, 12])

    def test_copy_under_a_pane_that_ends_early(self):
        d = make_layout()
        with self.assertRaises(UiPackError) as e:
            up.clone_parts(d, [Copy("Pa_Tab_03", "Pa_Tab_04", parent="N_Gauge_00")])
        self.assertIn("Copy.parent", str(e.exception))
        n = up.clone_parts(d, [Copy("Pa_Tab_03", "Pa_Tab_04", parent="N_Gauge_00")], out_of_order=True)
        self.assertEqual(u32(n, up.read_layout(n)[0][-1].record + 0x28), 7)
        with self.assertRaises(UiPackError):
            up.clone_parts(d, [Copy("Pa_Tab_03", "Pa_Tab_04", parent="N_Nothing_00")])

    def test_a_copy_stored_hidden(self):
        d = make_layout()
        shown = up.clone_parts(d, [Copy("Pa_Guide_01", "Pa_Guide_02")])
        hidden = up.clone_parts(d, [Copy("Pa_Guide_01", "Pa_Guide_02", hidden=True)])
        word = lambda n: u32(n, up.read_layout(n)[0][-1].record + 0x38)
        self.assertEqual(word(shown), PART_KIND)                    # (the source's: bit 0 set)
        self.assertEqual(word(hidden), PART_KIND & ~1)              # only bit 0 cleared
        self.assert_rest_unchanged(d, hidden, bumped=(0, 9))

    def test_move_panes(self):
        d = make_layout()
        n = up.move_panes(d, {"Pa_Tab_00": (-50.0, 5.0), "N_Footer_00": (0.0, 10.0)})
        self.assertEqual(len(n), len(d))
        panes, _ = up.read_layout(n)
        self.assertEqual(struct.unpack_from("<ff", n, panes[3].record + 0x1c), (250.0, -25.0))
        self.assertEqual(struct.unpack_from("<ff", n, panes[9].record + 0x1c), (900.0, -80.0))
        changed = [at for at in range(len(d)) if d[at] != n[at]]
        moved = (panes[3].record + 0x1c, panes[3].record + 0x24, panes[9].record + 0x20, panes[9].record + 0x24)
        self.assertTrue(all(moved[0] <= at < moved[1] or moved[2] <= at < moved[3] for at in changed))
        self.assertEqual(up.move_panes(d, {}), d)
        for bad in ({"Pa_Nothing_00": (1.0, 0.0)}, {"Pa_Tab_00": (1.0,)}, {"Pa_Tab_00": 5},
                    {"Pa_Tab_00": (float("nan"), 0.0)}):
            with self.subTest(moves=repr(bad)), self.assertRaises(UiPackError):
                up.move_panes(d, bad)

    def test_frame_offset(self):
        d = make_layout()
        self.assertEqual(up.frame_offset(d, "Pa_Tab_03", "N_Tabs_00"), (0.0, 0.0))     # its own parent
        self.assertEqual(up.frame_offset(d, "Pa_Guide_01", "RootPane"), (900.0, -90.0))
        for pane, ancestor in (("Pa_Tab_03", "N_Footer_00"), ("Pa_Tab_03", "Pa_Tab_03"), ("RootPane", "N_All_00"),
                               ("Pa_Nothing_00", "RootPane"), ("Pa_Tab_03", "N_Nothing_00")):
            with self.subTest(pane=pane, ancestor=ancestor), self.assertRaises(UiPackError):
                up.frame_offset(d, pane, ancestor)
        scaled = bytearray(d)
        struct.pack_into("<f", scaled, up.read_layout(d)[0][2].record + 0x10, 2.0)   # N_Tabs_00 scaled
        with self.assertRaises(UiPackError):
            up.frame_offset(bytes(scaled), "Pa_Tab_03", "RootPane")
        turned = bytearray(d)
        struct.pack_into("<f", turned, up.read_layout(d)[0][1].record + 0x18, 0.5)   # N_All_00 turned
        with self.assertRaises(UiPackError):
            up.frame_offset(bytes(turned), "Pa_Tab_03", "RootPane")
        self.assertEqual(up.frame_offset(bytes(turned), "Pa_Tab_03", "N_All_00"), (200.0, -20.0))


# ---------------------------------------------------------------------------------------- MSBT

def make_msbt(entries, encoding=0, order="<", buckets=4, extra=()):
    """An MSBT built here: LBL1 (each label in its hash bucket), TXT2, then `extra` sections
    [(magic, data)]. entries: [(label, text)]."""
    codec = {0: "utf-8", 1: "utf-16", 2: "utf-32"}[encoding]
    if encoding:
        codec += "-le" if order == "<" else "-be"
    table = [[] for _ in range(buckets)]
    for i, (label, _) in enumerate(entries):
        h = 0
        for c in label.encode():
            h = (h * 0x492 + c) & 0xFFFFFFFF
        table[h % buckets].append((label, i))
    heads, body = b"", b""
    for b in table:
        heads += struct.pack(order + "II", len(b), 4 + 8 * buckets + len(body))
        for label, i in b:
            body += bytes([len(label)]) + label.encode() + struct.pack(order + "I", i)
    lbl = struct.pack(order + "I", buckets) + heads + body
    texts = [(t + "\0").encode(codec) for _, t in entries]
    offs, at = [], 4 + 4 * len(texts)
    for t in texts:
        offs.append(at)
        at += len(t)
    txt = struct.pack(order + "I", len(texts)) + b"".join(struct.pack(order + "I", o) for o in offs) + b"".join(texts)
    out = b""
    sects = [(b"LBL1", lbl), (b"TXT2", txt)] + list(extra)
    for magic, data in sects:
        sec = magic + struct.pack(order + "I", len(data)) + b"\0" * 8 + data
        out += sec + b"\xab" * (-len(sec) % 16)
    head = (b"MsgStdBn" + (b"\xff\xfe" if order == "<" else b"\xfe\xff") + b"\0\0" + bytes([encoding, 3])
            + struct.pack(order + "H", len(sects)) + b"\0\0" + struct.pack(order + "I", 0x20 + len(out)) + b"\0" * 10)
    return head + out


GAME = [("ItemMenu", "Items"), ("MapMenu", "Map"), ("SystemMenu", "System")]


class CityHashTest(unittest.TestCase):
    def test_the_games_pane_ids(self):
        """part_id is CityHash64 of the name, as every pane id of the game's layouts (these four: PlusMenu_00's)."""
        for name, want in (("RootPane", 0x55a7ab4a9d264968), ("N_All_00", 0x7b2f820ba988bfbc),
                           ("C_3DFade_00", 0xfa7620594596c6d7), ("P_3DFade_02", 0x42002723333a12c8)):
            self.assertEqual(up.part_id(name), want, name)

    def test_every_length(self):
        """Each of the hash's length cases runs and gives 64 bits (0, 1-3, 4-7, 8-16, 17-32, 33-64 bytes)."""
        seen = set()
        for n in (0, 1, 3, 4, 7, 8, 16, 17, 32, 33, 64):
            h = up.city_hash64(bytes(range(65, 65 + n)))
            self.assertTrue(0 <= h < 1 << 64)
            seen.add(h)
        self.assertEqual(len(seen), 11)
        with self.assertRaises(UiPackError):
            up.city_hash64(b"x" * 65)


class LabelsTest(unittest.TestCase):
    def test_new_labels_after_the_files_own(self):
        m = make_msbt(GAME)
        n = up.add_labels(m, {"CoopMenu": "Multiplayer", "CoopNote": "Two\nlines"})
        r = msbt.read(n)
        self.assertEqual(sorted((lab.name, lab.index) for lab in r.labels),
                         [("CoopMenu", 3), ("CoopNote", 4), ("ItemMenu", 0), ("MapMenu", 1), ("SystemMenu", 2)])
        self.assertEqual([r.text(label) for label, _ in GAME], [[t] for _, t in GAME])
        self.assertEqual(r.text("CoopMenu"), ["Multiplayer"])
        self.assertEqual(r.text("CoopNote"), ["Two\nlines"])
        self.assertEqual(r.buckets, 4)
        self.assertEqual(u32(n, 0x12), len(n))
        self.assertEqual(up.read_msbt(n)[3][:3], up.read_msbt(m)[3])   # the file's own texts, byte for byte

    def test_an_existing_label(self):
        m = make_msbt(GAME)
        with self.assertRaises(UiPackError):
            up.add_labels(m, {"MapMenu": "Ours"})
        r = msbt.read(up.add_labels(m, {"MapMenu": "Ours"}, replace=True))
        self.assertEqual(r.text("MapMenu"), ["Ours"])
        self.assertEqual(len(r.texts), 3)
        self.assertEqual([lab.index for lab in r.labels if lab.name == "MapMenu"], [1])

    def test_the_files_encoding(self):
        for encoding in (1, 2):
            with self.subTest(encoding=encoding):
                r = msbt.read(up.add_labels(make_msbt(GAME, encoding), {"CoopMenu": "Multijöueur 多人"}))
                self.assertEqual(r.text("CoopMenu"), ["Multijöueur 多人"])
                self.assertEqual(r.text("ItemMenu"), ["Items"])

    def test_attributes_grow(self):
        atr = struct.pack("<II", 3, 4) + b"\1\0\0\0\2\0\0\0\3\0\0\0"
        r = msbt.read(up.add_labels(make_msbt(GAME, extra=[(b"ATR1", atr)]), {"CoopMenu": "Multiplayer"}))
        self.assertEqual(r.attributes, [b"\1\0\0\0", b"\2\0\0\0", b"\3\0\0\0", b"\0\0\0\0"])

    def test_a_panes_own_label(self):
        """A text pane's label is its path through the parts, joined by - (the game's own)."""
        label = "Pa_Coop_00-Pa_Button_00-T_Btn_00"
        r = msbt.read(up.add_labels(make_msbt(GAME + [("Pa_System_00-Pa_Button_00-T_Btn_00", "Save")]),
                                    {label: "Session"}))
        self.assertEqual(r.text(label), ["Session"])
        self.assertEqual(r.text("Pa_System_00-Pa_Button_00-T_Btn_00"), ["Save"])
        self.assertEqual(up.parse_strings("[F.msbt]\n%s = Session\n" % label), {"F.msbt": {label: "Session"}})

    def test_refusals(self):
        m = make_msbt(GAME)
        for labels in ({"Coop Menu": "x"}, {"": "x"}, {"Coopé": "x"}, {"Coop/Menu": "x"}, {"CoopMenu": ""},
                       {"CoopMenu": "a\x0eb"}, {"CoopMenu": "a\tb"}, {"CoopMenu": 5}):
            with self.subTest(labels=repr(labels)), self.assertRaises(UiPackError):
                up.add_labels(m, labels)
        for bad in (make_msbt(GAME, order=">"), make_msbt(GAME, extra=[(b"TSY1", b"\0" * 12)]), m[:0x30],
                    b"not a message file at all, not at all"):
            with self.assertRaises(UiPackError):
                up.add_labels(bad, {"CoopMenu": "Multiplayer"})


# ---------------------------------------------------------------------------------------- strings

class StringsTest(unittest.TestCase):
    def write(self, folder, files):
        for name, text in files.items():
            with open(os.path.join(folder, name), "w", encoding="utf-8") as f:
                f.write(text)

    def test_parse(self):
        text = ("\ufeff# ours\n\n[LayoutMsg/Menu_00.msbt]\n  CoopMenu   =  Multiplayer  \nCoopNote=a = b # c\n"
                "CoopTwo = one\\ntwo \\\\ three\n# [not a section]\n[LayoutMsg/Other_00.msbt]\nCoopMenu = Other\n")
        self.assertEqual(up.parse_strings(text), {
            "LayoutMsg/Menu_00.msbt": {"CoopMenu": "Multiplayer", "CoopNote": "a = b # c",
                                       "CoopTwo": "one\ntwo \\ three"},
            "LayoutMsg/Other_00.msbt": {"CoopMenu": "Other"}})

    def test_parse_refusals(self):
        bad = {
            "CoopMenu = x\n": "before any",                                   # no [file] yet
            "[LayoutMsg/Menu_00.msbt]\nCoopMenu\n": "line 2",                 # no "="
            "[LayoutMsg/Menu_00.msbt]\nCoopMenu = x\nCoopMenu = y\n": "already",
            "[LayoutMsg/Menu_00.msbt]\n[LayoutMsg/Menu_00.msbt]\n": "already",
            "[LayoutMsg/Menu_00.msbt]\nCoop Menu = x\n": "not a label",
            "[LayoutMsg/Menu_00.msbt]\nCoopMenu =\n": "empty",
            "[LayoutMsg/Menu_00.msbt]\nCoopMenu = a\\tb\n": "\\t",
            "[LayoutMsg/Menu_00.msbt]\nCoopMenu = a\\\n": "line 2",
            "[Menu_00.txt]\nCoopMenu = x\n": "line 1",
        }
        for text, words in bad.items():
            with self.subTest(text=text), self.assertRaises(UiPackError) as e:
                up.parse_strings(text, "x.txt")
            self.assertIn(words, str(e.exception))

    def test_english_the_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            self.write(folder, {
                "en.txt": "[LayoutMsg/Menu_00.msbt]\nCoopMenu = Multiplayer\nCoopLobby = Lobby\n"
                          "[LayoutMsg/Title_00.msbt]\nCoopHost = Host\n",
                "fr.txt": "[LayoutMsg/Menu_00.msbt]\nCoopMenu = Multijoueur\n",
                "USfr.txt": "[LayoutMsg/Title_00.msbt]\nCoopHost = Héberger\n",
                "zh.txt": "[LayoutMsg/Menu_00.msbt]\nCoopLobby = 大厅\n"})
            en = {"LayoutMsg/Menu_00.msbt": {"CoopMenu": "Multiplayer", "CoopLobby": "Lobby"},
                  "LayoutMsg/Title_00.msbt": {"CoopHost": "Host"}}
            self.assertEqual(up.load_strings("USen", folder), en)
            self.assertEqual(up.load_strings("JPja", folder), en)
            self.assertEqual(up.load_strings("EUfr", folder)["LayoutMsg/Menu_00.msbt"],
                             {"CoopMenu": "Multijoueur", "CoopLobby": "Lobby"})
            self.assertEqual(up.load_strings("EUfr", folder)["LayoutMsg/Title_00.msbt"], {"CoopHost": "Host"})
            usfr = up.load_strings("USfr", folder)
            self.assertEqual(usfr["LayoutMsg/Title_00.msbt"], {"CoopHost": "Héberger"})
            self.assertEqual(usfr["LayoutMsg/Menu_00.msbt"]["CoopMenu"], "Multijoueur")
            self.assertEqual(up.load_strings("CNzh", folder)["LayoutMsg/Menu_00.msbt"]["CoopLobby"], "大厅")
            self.assertEqual(up.load_strings("fr", folder)["LayoutMsg/Menu_00.msbt"]["CoopMenu"], "Multijoueur")
            self.write(folder, {"de.txt": "[LayoutMsg/Menu_00.msbt]\nCoopTypo = Mehrspieler\n"})
            with self.assertRaises(UiPackError):
                up.load_strings("EUde", folder)
            for bad in ("../en", "", "e", "USen.txt"):
                with self.subTest(language=bad), self.assertRaises(UiPackError):
                    up.load_strings(bad, folder)
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(UiPackError):
                up.load_strings("USen", folder)

    def test_language_names(self):
        self.assertEqual(up.language_files("USfr"), ["en", "fr", "USfr"])
        self.assertEqual(up.language_files("USen"), ["en", "USen"])
        self.assertEqual(up.language_files("fr"), ["en", "fr"])
        self.assertEqual(up.language_files("en"), ["en"])
        self.assertEqual(up.language_of("Mals/USfr.Product.110.sarc.zs"), "USfr")
        self.assertEqual(up.language_of("romfs\\Mals\\CNzh.Product.110.sarc.zs"), "CNzh")

    def test_our_strings_files(self):
        """Every file in strings/ reads, and its labels are en.txt's: what the pack step will add."""
        names = sorted(f[:-4] for f in os.listdir(up.STRINGS) if f.endswith(".txt"))
        self.assertIn("en", names)
        for name in names:
            with self.subTest(file=name):
                self.assertTrue(up.load_strings(name))
        ours = up.load_strings("EUfr")["LayoutMsg/MenuOverlay_00.msbt"]
        r = msbt.read(up.add_labels(make_msbt(GAME), ours))
        self.assertEqual(r.text("Pa_Category_04-T_Text_00"), [ours["Pa_Category_04-T_Text_00"]])


if __name__ == "__main__":
    unittest.main()
