"""Tests for the title screen's co-op rows (title_menu.py). Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_title_menu.py
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import byml, sarc, title_menu as tm  # noqa: E402

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


def make_key_layout(side="L", icon_x=213.3, font=b"External_00"):
    """A controller key as Common's PaControllerKeyL_00 / R_00: RootPane, N_IconGuide<side>_00 and its icon and label
    text panes (the icon at icon_x), no parts, the font's name in the string pool."""
    panes = ["RootPane", "N_IconGuide%s_00" % side, "T_Icon%s_00" % side, "T_Guide%s_00" % side]
    parent = [0xFFFFFFFF, 0, 1, 1]
    end = [4, 4, 3, 4]
    d = bytearray(0x500)
    d[0:4] = b"BFFH"
    d[0x28:0x28 + 35] = b"Nintendo.Ui.ResModels.LayoutResData"
    d[0xa8:0xac] = b"BFSI"
    d[0xc0:0xc7] = b"Default"
    d[0xd0:0xd4] = b"BFSI"
    d[0xe8:0xf2] = b"StringPool"
    d[0xf8:0xfb] = b"UIL"
    struct.pack_into("<Q", d, 0x100, 0xce)
    recs, pool = 0x300, len(d)
    strings, names = bytearray(), []
    for n in panes:
        names.append(pool + len(strings))
        strings += n.encode() + b"\0"
    strings += font + b"\0"
    keys, vals, ids = 0x200, 0x240, 0x280
    for i, _ in enumerate(panes):
        rec = recs + 0x44 * i
        struct.pack_into("<Q", d, keys + 8 * i, names[i])
        struct.pack_into("<Q", d, vals + 8 * i, rec)
        struct.pack_into("<Q", d, ids + 8 * i, 0x2222 * (i + 1))
        struct.pack_into("<ff", d, rec + 0x10, 1.0, 1.0)
        struct.pack_into("<f", d, rec + 0x1c, icon_x if i == 2 else 0.0)
        struct.pack_into("<I", d, rec + 0x28, parent[i])
        struct.pack_into("<I", d, rec + 0x34, end[i])
        struct.pack_into("<I", d, rec + 0x38, 0x02555003)
    struct.pack_into("<QQQII", d, 0x118, keys, vals, ids, len(panes), 8)
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

    def test_the_marker_after_the_last_button_stored_hidden(self):
        d = make_layout()
        n = tm.add_marker(d)
        panes, parts = tm.read_layout(n)
        self.assertEqual([p[0] for p in panes], PANES + [tm.MARKER])
        self.assertEqual([p[0] for p in parts][-1], tm.MARKER)
        # the old records stay where they were (nothing that points at them moves)
        self.assertEqual([p[1] for p in panes[:7]], [p[1] for p in tm.read_layout(d)[0]])
        name, rec, ident = panes[7]
        parent, = struct.unpack_from("<I", n, rec + 0x28)
        y, = struct.unpack_from("<f", n, rec + 0x20)
        self.assertEqual(parent, 2)
        self.assertAlmostEqual(y, -395.0)
        self.assertNotIn(ident, [p[2] for p in panes[:7]])
        kind, = struct.unpack_from("<I", n, rec + 0x38)
        self.assertEqual(kind & 1, 0)   # (stored hidden: never drawn; bit 0 of the kind word, as the source's is set)
        self.assertEqual(struct.unpack_from("<I", d, tm.read_layout(d)[0][6][1] + 0x38)[0] & 1, 1)
        self.assertFalse(any(p[0].startswith("Pa_Button_") and p[0] > "Pa_Button_03" for p in panes))
        secs = tm._sections(n)
        self.assertEqual(secs["StringPool"][1] + secs["StringPool"][2], len(n))
        self.assertEqual(struct.unpack_from("<I", n, 8)[0], len(n))
        self.assertEqual(n[:len(d)][0x800:], d[0x800:])   # the old strings unchanged

    def test_the_list_buttons_and_the_hint_after_the_marker_stored_hidden(self):
        d = tm.add_marker(make_layout())
        n = tm.add_menu(make_layout())
        panes, parts = tm.read_layout(n)
        names = [p[0] for p in panes]
        want = ["Pa_Button_%02d" % i for i in range(4, 4 + tm.LIST_BUTTONS)] + [tm.HINT]
        self.assertEqual(names, PANES + [tm.MARKER] + want)   # (no toggle: add_menu's notes)
        self.assertEqual([p[0] for p in parts][-len(want):], want)
        self.assertEqual([p[1] for p in panes[:8]], [p[1] for p in tm.read_layout(d)[0]])   # (nothing moves)
        for i, name in enumerate(want):
            rec = panes[names.index(name)][1]
            self.assertEqual(struct.unpack_from("<I", n, rec + 0x28)[0], 2)   # (the buttons' alignment pane)
            self.assertAlmostEqual(struct.unpack_from("<f", n, rec + 0x20)[0], -395.0 + tm.ROW_STEP_Y * (i + 1))
            self.assertEqual(struct.unpack_from("<I", n, rec + 0x38)[0] & 1, 0)   # (stored hidden)
        with self.assertRaises(tm.TitleMenuError):
            tm.add_list(n)
        with self.assertRaises(tm.TitleMenuError):
            tm.add_list(make_layout())   # (no marker)

    def test_the_toggle_in_the_first_buttons_place_of_the_options_layout_stored_hidden(self):
        n = tm.add_toggle(tm.add_menu(make_layout()))   # (not in the pack: check_own_parts refuses it, below)
        panes, parts = tm.read_layout(n)
        names = [p[0] for p in panes]
        self.assertEqual(names[-1], tm.TOGGLE)
        self.assertEqual(parts[-1][0], tm.TOGGLE)
        rec, src = panes[-1][1], panes[names.index("Pa_Button_00")][1]
        self.assertEqual(struct.unpack_from("<I", n, rec + 0x28)[0], struct.unpack_from("<I", n, src + 0x28)[0])
        x, y = struct.unpack_from("<ff", n, rec + 0x1c)
        sx, sy = struct.unpack_from("<ff", n, src + 0x1c)
        self.assertEqual((x, y), (sx + tm.TOGGLE_MOVE[0], sy + tm.TOGGLE_MOVE[1]))
        for got, want in zip(struct.unpack_from("<ff", n, rec + 0x10), tm.TOGGLE_SCALE):
            self.assertAlmostEqual(got, want, places=6)
        self.assertEqual(struct.unpack_from("<ff", n, rec + 0x2c), tm.TOGGLE_SIZE)
        self.assertEqual(struct.unpack_from("<I", n, rec + 0x38)[0] & 1, 0)   # (stored hidden)
        part = parts[-1][1]
        layout = struct.unpack_from("<Q", n, part)[0]
        self.assertEqual(n[layout:n.index(b"\0", layout)].decode(), tm.TOGGLE_LAYOUT)
        self.assertEqual(struct.unpack_from("<I", n, part + 0x30)[0], 0)   # (no parameters of the title button's)
        self.assertFalse(any(name.startswith("Pa_Button_") and name > "Pa_Button_13" for name in names))
        with self.assertRaises(tm.TitleMenuError):
            tm.add_toggle(n)
        with self.assertRaises(tm.TitleMenuError):
            tm.add_toggle(tm.add_marker(make_layout()))   # (no list)
        for bad in ((0.0, 1.0), (1.0,), (1.0, float("nan")), (True, 1.0), [1.0, 1.0], (1.0, 17.0)):
            with self.assertRaises(tm.TitleMenuError):
                tm.ui_pack.clone_parts(make_layout(), [tm.ui_pack.Copy("Pa_Button_00", "Pa_X_00", scale=bad)])

    def test_refuses_a_layout_that_has_them(self):
        with self.assertRaises(tm.TitleMenuError):
            tm.add_marker(tm.add_marker(make_layout()))
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
        self.assertEqual([t[:-1].decode() for t in texts], ["New Game", "Continue", "Options", "Host a game", "Join a game",
                                                            "Co-op Settings", "Start", "Multiplayer", "Options",
                                                            "Back"] + ["." * 64] * (len(tm.LIVE) - 1 - len(tm.TOGGLE_LIVE)) + ["." * 160]
                         + ["." * 64] * len(tm.TOGGLE_LIVE))   # (the live texts' placeholders)
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


class _Plain:
    """Dictionaries that do nothing: the synthetic files are not compressed."""
    def decompress(self, raw):
        return raw


class _Table:
    def __init__(self, sizes):
        self.sizes = dict(sizes)

    def get(self, key):
        return self.sizes.get(key)

    def set(self, key, value):
        self.sizes[key] = value


SCREENS = byml.dumps([{"ArchiveCategory": ["Title"], "BodyLayoutFile": "Title_00", "Class": "UITitleScreen", "Name": "Title_00"},
                       {"ArchiveCategory": ["Common"], "Name": "Other_00"}])


class BuildTest(unittest.TestCase):
    def romfs(self, root, broken=None):
        files = {tm.TITLE_ARCHIVE: sarc.write([(tm.TITLE_LAYOUT, make_layout())] * 2, data_align=16)}
        files["RSDB/UIScreen.Product.111.rstbl.byml.zs"] = SCREENS
        for lang in ("EUen", "JPja", "USen"):
            msbt = make_msbt(["0000", "0001", "0002"], ["a", "b", "c"])
            if lang == broken:
                msbt = msbt[:0x30]   # cut short: it cannot be read
            files["Mals/%s.Product.110.sarc.zs" % lang] = sarc.write({tm.MSG_FILE: msbt, "Other.msbt": b"x"})
        for rel, data in files.items():
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        return files

    def run_build(self, broken=None):
        with tempfile.TemporaryDirectory() as root:
            src = os.path.join(root, "romfs")
            self.romfs(src, broken)
            table = _Table({tm.TITLE_ARCHIVE[:-3]: 1000, tm.TITLE_LAYOUT: 500})
            for lang in ("EUen", "JPja", "USen"):
                table.set("Mals/%s.Product.110.sarc" % lang, 2000)
            before = dict(table.sizes)
            written = {}

            def write(out, rel, data):
                written[rel] = data

            try:
                made = tm.build(src, "out", _Plain(), table, write, lambda d, raw, data: data, log=lambda s: None)
            except (tm.TitleMenuError, struct.error, IndexError, ValueError) as e:
                return None, written, table.sizes, before, e
            return made, written, table.sizes, before, None

    def test_every_file_when_all_can_be_made(self):
        made, written, sizes, before, err = self.run_build()
        self.assertIsNone(err)
        self.assertEqual(len(made), 4)   # (the archive and three languages: not the screens table)
        self.assertEqual(set(written), set(made))
        layouts = [d for n, d in sarc.read_entries(written[tm.TITLE_ARCHIVE]) if n == tm.TITLE_LAYOUT]
        self.assertEqual(len(layouts), 2)
        self.assertTrue(all(len(tm.read_layout(d)[0]) == len(PANES) + 2 + tm.LIST_BUTTONS for d in layouts))
        self.assertNotIn(tm.TOGGLE, [p[0] for p in tm.read_layout(layouts[0])[0]])
        _, _, labels, _ = tm.read_msbt(sarc.read(written["Mals/JPja.Product.110.sarc.zs"])[tm.MSG_FILE])
        self.assertIn("0106", labels)
        self.assertTrue(all(sizes[k] > before[k] for k in before))

    def test_no_part_from_another_archive_and_the_screens_table_untouched(self):
        """LANE1 2026-11-22 (tabs-overhaul-01110 and title-01110): the toggle's part (Common's PaOptionBtnL_00) in
        the Title archive crashed every boot, Common in Title_00's archives or not. The pack refuses such a layout."""
        made, written, sizes, before, err = self.run_build()
        self.assertIsNone(err)
        self.assertNotIn("RSDB/UIScreen.Product.111.rstbl.byml.zs", written)
        entries = sarc.read_entries(written[tm.TITLE_ARCHIVE])
        tm.ui_pack.check_own_parts(entries, entries, tm.TITLE_ARCHIVE)   # (what the pack writes passes)
        toggled = [(n, tm.add_toggle(d) if n == tm.TITLE_LAYOUT else d) for n, d in entries]
        with self.assertRaisesRegex(tm.TitleMenuError, "PaOptionBtnL_00"):
            tm.ui_pack.check_own_parts(entries, toggled, tm.TITLE_ARCHIVE)
        # (the toggle's layout in the archive too: allowed, as a layout of its own archive)
        tm.ui_pack.check_own_parts(entries, toggled + [("PaOptionBtnL_00.buil", make_layout())], tm.TITLE_ARCHIVE)
        rows = byml.loads(SCREENS)
        tm.add_common_archive(rows)
        tm.add_common_archive(rows)   # (twice: Common once)
        self.assertEqual(rows[0]["ArchiveCategory"], ["Title", "Common"])
        with self.assertRaises(tm.TitleMenuError):
            tm.add_common_archive([{"ArchiveCategory": ["Common"], "Name": "Title_00"}])   # (not Title first)
        with self.assertRaises(tm.TitleMenuError):
            tm.add_common_archive([{"ArchiveCategory": ["Title"], "Name": "Else_00"}])

    def test_edit_screens_chains_through_the_run_and_adds_a_row(self):
        """The API other builders use (tabs-overhaul's CoopMenu_00 row): what an earlier builder made is read."""
        import tempfile
        made, written, sizes, before, err = self.run_build()
        self.assertIsNone(err)
        k = "RSDB/UIScreen.Product.111.rstbl.byml.zs"

        class Run:   # (a PackRun's reader: the screens table as title_menu made it)
            def glob(self, pattern):
                return [k]

            def read(self, rel):
                return SCREENS

        row = {"ArchiveCategory": ["Common"], "BodyLayoutFile": "CoopMenu_00", "Name": "CoopMenu_00"}
        rel, data, grew = tm.ui_pack.edit_screens(Run(), _Plain(), lambda d, raw, new: new, lambda rows: rows + [row])
        got = byml.loads(data)
        self.assertEqual([r["Name"] for r in got], ["Title_00", "Other_00", "CoopMenu_00"])
        self.assertEqual(got[0]["ArchiveCategory"], ["Title"])   # (the game's own row as it was)
        self.assertEqual((grew[0], grew[1] < grew[2], grew[3]), (k, True, True))

    def test_the_newest_copy_of_the_screens_table_is_the_one_edited(self):
        self.assertEqual(tm.ui_pack.newest(["RSDB/UIScreen.Product.100.rstbl.byml.zs", "RSDB/UIScreen.Product.111.rstbl.byml.zs"]),
                         ["RSDB/UIScreen.Product.111.rstbl.byml.zs"])
        self.assertEqual(tm.ui_pack.newest(["RSDB/UIScreen.Product.111.rstbl.byml.zs", "RSDB/UIScreen.Product.100.rstbl.byml.zs"]),
                         ["RSDB/UIScreen.Product.111.rstbl.byml.zs"])
        self.assertEqual(tm.ui_pack.newest([]), [])

    def test_nothing_written_when_one_language_fails(self):
        made, written, sizes, before, err = self.run_build(broken="JPja")
        self.assertIsNotNone(err)
        self.assertEqual(written, {})
        self.assertEqual(sizes, before)


class KeysTest(unittest.TestCase):
    """The list's ZL and ZR as the game's own button icons on the title (menu-title-01113)."""

    def test_two_hidden_parts_of_our_key_copies_beside_the_first_row(self):
        d = tm.add_keys(tm.add_menu(make_layout()))
        panes, _ = tm.read_layout(d)
        by = {p[0]: p[1] for p in panes}
        layouts = dict(tm.ui_pack.part_layouts(d))
        x = tm.KEY_ICON_X + tm.KEY_ICON_IN
        for name, (_, ours), sign in zip(tm.KEYS, tm.KEY_LAYOUTS, (-1, 1)):
            self.assertEqual(layouts[name], ours)
            tx, ty = struct.unpack_from("<ff", d, by[name] + 0x1c)
            self.assertAlmostEqual(tx, sign * x, places=3)
            self.assertAlmostEqual(ty, -65.0, places=3)   # (level with Pa_Button_00)
            kind, = struct.unpack_from("<I", d, by[name] + 0x38)
            self.assertEqual(kind & 1, 0)                 # (stored hidden: the module shows them)
        with self.assertRaises(tm.TitleMenuError):
            tm.add_keys(d)                                # (once)
        with self.assertRaises(tm.TitleMenuError):
            tm.add_keys(make_layout())                    # (after the list only)

    def test_the_key_layouts_copied_under_our_names_or_refused(self):
        common = [("PaControllerKeyL_00.buil", make_key_layout("L", 213.3)),
                  ("PaControllerKeyR_00.buil", make_key_layout("R", -213.3))]
        got = tm.key_layouts(common)
        self.assertEqual([n for n, _ in got], ["PaCoopKeyTitleL_00.buil", "PaCoopKeyTitleR_00.buil"])
        from puppet_pack import coop_menu
        self.assertEqual(coop_menu.layout_name(got[0][1]), "PaCoopKeyTitleL_00")
        self.assertEqual([p[0] for p in tm.read_layout(got[1][1])[0]],
                         ["RootPane", "N_IconGuideR_00", "T_IconR_00", "T_GuideR_00"])
        with self.assertRaises(tm.TitleMenuError):
            tm.key_layouts(common[:1])                                        # (no R key)
        with self.assertRaises(tm.TitleMenuError):
            tm.key_layouts([common[0], ("PaControllerKeyR_00.buil", make_key_layout("R", -100.0))])   # (moved icon)
        with self.assertRaises(tm.TitleMenuError):
            tm.key_layouts([common[0], ("PaControllerKeyR_00.buil", make_key_layout("R", -213.3, b"Normal"))])

    def test_the_category_lists_them_under_title_in_name_order(self):
        cat = {"Title": ["PaCommonBtnTitle_00", "Title_00", "TitleMenuBG_00"], "Common": ["X"]}
        tm.add_category(cat)
        tm.add_category(cat)   # (twice: once each)
        self.assertEqual(cat["Title"], ["PaCommonBtnTitle_00", "PaCoopKeyTitleL_00", "PaCoopKeyTitleR_00", "Title_00",
                                        "TitleMenuBG_00"])
        with self.assertRaises(tm.TitleMenuError):
            tm.add_category({"Common": ["Title_00"]})

    def test_the_labels_are_the_games_zl_and_zr_letters_in_every_language(self):
        for rel in ("Mals/USen.Product.110.sarc.zs", "Mals/CNzh.Product.110.sarc.zs"):
            rows = dict(tm.rows_for(rel))
            self.assertEqual(rows["Coop_Key_ZL"], "")
            self.assertEqual(rows["Coop_Key_ZR"], "")

    def test_a_build_with_the_common_keys_adds_them_and_one_without_keeps_the_title_as_before(self):
        with tempfile.TemporaryDirectory() as root:
            src = os.path.join(root, "romfs")
            BuildTest().romfs(src)
            common = [("PaControllerKeyL_00.buil", make_key_layout("L", 213.3)),
                      ("PaControllerKeyR_00.buil", make_key_layout("R", -213.3))]
            extra = {tm.COMMON_ARCHIVE: sarc.write(common, data_align=16),
                     tm.CATEGORY: byml.dumps({"Title": ["PaCommonBtnTitle_00", "Title_00", "TitleMenuBG_00"]})}
            for rel, data in extra.items():
                path = os.path.join(src, *rel.split("/"))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(data)
            table = _Table({tm.TITLE_ARCHIVE[:-3]: 1000, tm.TITLE_LAYOUT: 500, tm.CATEGORY[:-3]: 300})
            for lang in ("EUen", "JPja", "USen"):
                table.set("Mals/%s.Product.110.sarc" % lang, 2000)
            written = {}
            made = tm.build(src, "out", _Plain(), table, lambda out, rel, data: written.__setitem__(rel, data),
                            lambda d, raw, data: data, log=lambda s: None)
            self.assertIn(tm.CATEGORY, made)
            entries = sarc.read_entries(written[tm.TITLE_ARCHIVE])
            names = [n for n, _ in entries]
            self.assertEqual(names.count("PaCoopKeyTitleL_00.buil"), tm.COPIES)
            self.assertEqual(names.count("PaCoopKeyTitleR_00.buil"), tm.COPIES)
            tm.ui_pack.check_own_parts(entries, entries, tm.TITLE_ARCHIVE)   # (every part's layout in the archive)
            layout = dict(entries)[tm.TITLE_LAYOUT]
            self.assertTrue(set(tm.KEYS) <= {p[0] for p in tm.read_layout(layout)[0]})
            self.assertIn("PaCoopKeyTitleL_00", byml.loads(written[tm.CATEGORY])["Title"])
        # (no Common archive: the title as before, no keys, no category written)
        made, written, sizes, before, err = BuildTest().run_build()
        self.assertIsNone(err)
        self.assertNotIn(tm.CATEGORY, written)
        layout = [d for n, d in sarc.read_entries(written[tm.TITLE_ARCHIVE]) if n == tm.TITLE_LAYOUT][0]
        self.assertFalse(set(tm.KEYS) & {p[0] for p in tm.read_layout(layout)[0]})


class LanguageTest(unittest.TestCase):
    def test_chinese_rows_and_the_live_placeholders(self):
        # (#307's rows per language, with #308's live texts in every language's file)
        cn = dict(tm.rows_for("Mals/CNzh.Product.110.sarc.zs"))
        self.assertEqual(cn["0100"], "创建房间")
        self.assertEqual(dict(tm.rows_for("Mals/TWzh.Product.110.sarc.zs"))["0104"], "連線")
        en = dict(tm.rows_for("Mals/USen.Product.110.sarc.zs"))
        self.assertEqual(en["0100"], "Host a game")
        for rows in (cn, en):
            for label, _ in tm.LIVE:
                self.assertIn(label, rows)
            self.assertIn("Coop_Live_T13", rows)
            self.assertIn("Coop_Live_Hint", rows)


if __name__ == "__main__":
    unittest.main()
