"""Tests for the title screen's co-op rows (title_menu.py). Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_title_menu.py
"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import title_menu as tm  # noqa: E402

PANES = ["RootPane", "N_All_00", "A_BtnAlign_00", "Pa_Button_00", "Pa_Button_01", "Pa_Button_02", "Pa_Button_03"]
PARENT = [0xFFFFFFFF, 0, 1, 2, 2, 2, 2]
END = [7, 7, 7, 4, 5, 6, 7]
PART = 0x02555313


def make_layout():
    """A Layout2 layout shaped as Title_00: header, Default and StringPool sections, the UIL root with its
    pane and part dictionaries (the rest of the root's dictionaries left empty), pane and part records."""
    d = bytearray(0x800)
    d[0:4] = b"BFFH"
    d[0x28:0x28 + 35] = b"Nintendo.Ui.ResModels.LayoutResData"
    d[0xa8:0xac] = b"BFSI"
    d[0xc0:0xc7] = b"Default"
    d[0xd0:0xd4] = b"BFSI"
    d[0xe8:0xf2] = b"StringPool"
    d[0xf8:0xfb] = b"UIL"
    struct.pack_into("<Q", d, 0x100, 0xce)
    recs, parts = 0x300, 0x500
    pool = len(d)
    strings = bytearray()
    names = []
    for n in PANES + ["PaCommonBtnTitle_00"]:
        names.append(pool + len(strings))
        strings += n.encode() + b"\0"
    keys, vals, ids = 0x200, 0x240, 0x280
    for i, _ in enumerate(PANES):
        rec = recs + 0x44 * i
        struct.pack_into("<Q", d, keys + 8 * i, names[i])
        struct.pack_into("<Q", d, vals + 8 * i, rec)
        struct.pack_into("<Q", d, ids + 8 * i, 0x1111 * (i + 1))
        struct.pack_into("<ff", d, rec + 0x10, 1.0, 1.0)
        struct.pack_into("<f", d, rec + 0x20, -65.0 - 110.0 * max(0, i - 3))
        struct.pack_into("<I", d, rec + 0x28, PARENT[i])
        struct.pack_into("<I", d, rec + 0x34, END[i])
        struct.pack_into("<I", d, rec + 0x38, PART if i >= 3 else 0x02555003)
        struct.pack_into("<HH", d, rec + 0x40, 0xFFFF, i - 3 if i >= 3 else 0)
    struct.pack_into("<QQQII", d, 0x118, keys, vals, ids, len(PANES), 8)
    pk, pv, pi = 0x2c0, 0x2e0, 0x700
    for j in range(4):
        rec = parts + 0x58 * j
        struct.pack_into("<Q", d, pk + 8 * j, names[3 + j])
        struct.pack_into("<Q", d, pv + 8 * j, rec)
        struct.pack_into("<Q", d, pi + 8 * j, 0x1111 * (4 + j))
        struct.pack_into("<Q", d, rec, names[-1])
        struct.pack_into("<QQ", d, rec + 0x38, 0, 3 + j)
    struct.pack_into("<QQQII", d, 0x1b8, pk, pv, pi, 4, 8)
    d += strings
    while len(d) % 8:
        d.append(0)
    struct.pack_into("<II", d, 0xac, 0xf8, pool - 0xf8)
    struct.pack_into("<II", d, 0xd4, pool, len(d) - pool)
    struct.pack_into("<I", d, 0x08, len(d))
    return bytes(d)


def make_msbt(labels, texts):
    header = bytearray(b"MsgStdBn\xff\xfe\0\0\0\x03\x02\0" + b"\0" * 0x10)
    nb = 2
    secs = [(b"LBL1", struct.pack("<I", nb) + b"\0" * 8 * nb), (b"TXT2", struct.pack("<I", 0))]
    lab = {l: i for i, l in enumerate(labels)}
    return tm.write_msbt(bytes(header), secs, lab, [(t + "\0").encode() for t in texts])


class LayoutTest(unittest.TestCase):
    def test_reads_panes_and_parts(self):
        panes, parts = tm.read_layout(make_layout())
        self.assertEqual([p[0] for p in panes], PANES)
        self.assertEqual([p[0] for p in parts], PANES[3:])

    def test_three_more_buttons_after_the_last(self):
        d = make_layout()
        n = tm.add_buttons(d)
        panes, parts = tm.read_layout(n)
        self.assertEqual([p[0] for p in panes], PANES + ["Pa_Button_04", "Pa_Button_05", "Pa_Button_06"])
        self.assertEqual([p[0] for p in parts][-3:], ["Pa_Button_04", "Pa_Button_05", "Pa_Button_06"])
        # the old records stay where they were (nothing that points at them moves)
        self.assertEqual([p[1] for p in panes[:7]], [p[1] for p in tm.read_layout(d)[0]])
        for k, (name, rec, ident) in enumerate(panes[7:]):
            parent, = struct.unpack_from("<I", n, rec + 0x28)
            end, = struct.unpack_from("<I", n, rec + 0x34)
            y, = struct.unpack_from("<f", n, rec + 0x20)
            part_no, = struct.unpack_from("<H", n, rec + 0x42)
            self.assertEqual(parent, 2)
            self.assertEqual(end, 8 + k)
            self.assertAlmostEqual(y, -395.0 - 110.0 * (k + 1))
            self.assertEqual(part_no, 4 + k)
            self.assertNotIn(ident, [p[2] for p in panes[:7]])
        ends = [struct.unpack_from("<I", n, p[1] + 0x34)[0] for p in panes[:7]]
        self.assertEqual(ends, [10, 10, 10, 4, 5, 6, 7])   # the ancestors end after the new rows; Pa_Button_03 not
        for name, rec, _ in parts[4:]:
            self.assertEqual(struct.unpack_from("<Q", n, rec + 0x40)[0], [p[0] for p in panes].index(name))
            self.assertEqual(n[struct.unpack_from("<Q", n, rec)[0]:].split(b"\0")[0], b"PaCommonBtnTitle_00")
        secs = tm._sections(n)
        self.assertEqual(secs["StringPool"][1] + secs["StringPool"][2], len(n))
        self.assertEqual(struct.unpack_from("<I", n, 8)[0], len(n))
        self.assertEqual(n[:len(d)][0x800:], d[0x800:])   # the old strings unchanged

    def test_refuses_a_layout_that_has_them(self):
        with self.assertRaises(tm.TitleMenuError):
            tm.add_buttons(tm.add_buttons(make_layout()))
        with self.assertRaises(tm.TitleMenuError):
            tm.read_layout(b"BFFH" + b"\0" * 0x200)


class MsbtTest(unittest.TestCase):
    def test_round_trip_and_rows(self):
        m = make_msbt(["0000", "0001", "0002"], ["New Game", "Continue", "Options"])
        h, s, labels, texts = tm.read_msbt(m)
        self.assertEqual(labels, {"0000": 0, "0001": 1, "0002": 2})
        self.assertEqual(tm.write_msbt(h, s, labels, texts), m)
        h, s, labels, texts = tm.read_msbt(tm.add_rows(m))
        self.assertEqual(labels["0100"], 3)
        self.assertEqual([t[:-1].decode() for t in texts], ["New Game", "Continue", "Options", "Host", "Join",
                                                            "Co-op Settings"])
        self.assertEqual(struct.unpack_from("<I", tm.add_rows(m), 0x12)[0], len(tm.add_rows(m)))

    def test_labels_land_in_their_hash_bucket(self):
        m = tm.add_rows(make_msbt(["0000"], ["x"]))
        h, s, labels, texts = tm.read_msbt(m)
        data = dict(s)[b"LBL1"]
        nb = struct.unpack_from("<I", data, 0)[0]
        for b in range(nb):
            cnt, off = struct.unpack_from("<II", data, 4 + 8 * b)
            for _ in range(cnt):
                ln = data[off]
                self.assertEqual(tm._label_hash(data[off + 1:off + 1 + ln].decode(), nb), b)
                off += 5 + ln


if __name__ == "__main__":
    unittest.main()
