"""Tests for the other players' map icons (map_icons.py). Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_map_icons.py
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from puppet_pack import sarc, ui_pack, map_icons as mi  # noqa: E402
from test_ui_pack import make_layout  # noqa: E402
from test_pause_menu import _Plain, _Table  # noqa: E402

# Shaped as the game's MapMenu_00: the icon chain under N_Common_00, the one icon part last under its clip, a pane after.
MAP = [("RootPane", -1), ("N_InOut_00", 0), ("N_Common_00", 1), ("N_MapIcon_00", 2), ("N_PatternField_01", 3),
       ("N_PatternFieldMini_01", 4), ("N_PatternDungeon_01", 5), ("N_PatternDungeonMini_01", 6), ("S_MapIcon_00", 7),
       ("Pa_MapIcon_00", 8), ("N_CursorPos_00", 2)]
MAP_PARTS = {"Pa_MapIcon_00": ("PaMapIcon_00", [])}


def mapmenu():
    return make_layout(MAP, MAP_PARTS)


class IconsTest(unittest.TestCase):
    def test_copies_hidden_under_the_clip(self):
        d = mi.add_icons(mapmenu())
        panes, parts = ui_pack.read_layout(d)
        by = {p.name: (i, p) for i, p in enumerate(panes)}
        clip = by[mi.ICON_PARENT][0]
        for n in mi.copy_names():
            self.assertIn(n, by)
            i, p = by[n]
            self.assertEqual(struct.unpack_from("<I", d, p.record + 0x28)[0], clip)
            self.assertEqual(p.id, ui_pack.part_id(n))   # (found by name as the game finds its panes)
        self.assertEqual(dict(ui_pack.part_layouts(d))[mi.COPY_NAME % 3], "PaMapIcon_00")
        self.assertEqual(len(parts), 1 + mi.MAP_ICONS)
        self.assertEqual(len(set(mi.copy_names())), mi.MAP_ICONS)
        # the game's own part and the panes after it are still there, unchanged in name
        self.assertIn("Pa_MapIcon_00", by)
        self.assertIn("N_CursorPos_00", by)

    def test_copy_zero_carries_the_turn_mark(self):
        src = mapmenu()
        d = mi.add_icons(src)
        panes, _ = ui_pack.read_layout(d)
        by = {p.name: p for p in panes}
        source_turn = struct.unpack_from("<I", d, by[mi.ICON_PART].record + 0x18)[0]
        self.assertEqual(struct.unpack_from("<I", d, by[mi.COPY_NAME % 0].record + 0x18)[0], mi.TURN_MARK_BITS)
        for n in mi.copy_names()[1:]:
            self.assertEqual(struct.unpack_from("<I", d, by[n].record + 0x18)[0], source_turn)
        self.assertNotEqual(source_turn, mi.TURN_MARK_BITS)
        self.assertEqual(struct.pack("<I", mi.TURN_MARK_BITS), struct.pack("<f", 1.2345678))   # (1.2345678f)

    def test_turn_mark_is_the_modules(self):
        # one word in two places: coop_map_icons.h kTurnMarkBits (the module looks for it) and TURN_MARK_BITS (the pack)
        import re
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
        with open(os.path.join(root, "hookshot", "src", "coop_map_icons.h"), encoding="utf-8") as f:
            h = f.read()
        m = re.search(r"kTurnMarkBits = (0x[0-9A-Fa-f]+);", h)
        self.assertIsNotNone(m)
        self.assertEqual(int(m.group(1), 16), mi.TURN_MARK_BITS)
        self.assertIn("kTurnMarkAt = 0x68;", h)

    def test_turn_bits_refused_when_not_a_u32(self):
        for bad in (-1, 1 << 32, 1.5, True):
            with self.assertRaises(ui_pack.UiPackError):
                ui_pack.clone_parts(mapmenu(), [ui_pack.Copy(mi.ICON_PART, "Pa_X_00", parent=mi.ICON_PARENT,
                                                             turn_bits=bad)], out_of_order=True)

    def test_one_per_other_player(self):
        self.assertEqual(mi.MAP_ICONS, 15)

    def test_refuses_twice_and_other_layouts(self):
        d = mi.add_icons(mapmenu())
        with self.assertRaises(mi.MapIconsError):
            mi.add_icons(d)
        with self.assertRaises(mi.MapIconsError):
            mi.add_icons(make_layout([("RootPane", -1), ("N_A_00", 0)], {}))


class BuildTest(unittest.TestCase):
    def test_build_through_a_run(self):
        with tempfile.TemporaryDirectory() as root:
            src = os.path.join(root, "romfs")
            arc = sarc.write([(mi.MAP_LAYOUT, mapmenu()), ("PaMapIcon_00.buil", b"icon"), ("Other_00.buil", b"o")],
                             data_align=16)
            path = os.path.join(src, *mi.COMMON_ARCHIVE.split("/"))
            os.makedirs(os.path.dirname(path))
            with open(path, "wb") as f:
                f.write(arc)
            table = _Table({mi.COMMON_ARCHIVE[:-3]: 100000})
            written = {}
            run = ui_pack.PackRun(src, table)
            files = mi.build(src, os.path.join(root, "out"), _Plain(), table,
                             lambda out, rel, data: written.__setitem__(rel, data),
                             lambda dicts, raw, archive: archive, log=lambda *_: None, run=run)
            self.assertEqual(files, [mi.COMMON_ARCHIVE])
            entries = dict(sarc.read_entries(written[mi.COMMON_ARCHIVE]))
            self.assertIn(mi.COPY_NAME % 0, {p.name for p in ui_pack.read_layout(entries[mi.MAP_LAYOUT])[0]})
            self.assertEqual(entries["Other_00.buil"], b"o")
            self.assertGreater(table.get(mi.COMMON_ARCHIVE[:-3]), 100000)
            self.assertIs(run.read(mi.COMMON_ARCHIVE), written[mi.COMMON_ARCHIVE])   # (the next step reads ours)

    def test_no_map_layout(self):
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, *mi.COMMON_ARCHIVE.split("/"))
            os.makedirs(os.path.dirname(path))
            with open(path, "wb") as f:
                f.write(sarc.write([("Other_00.buil", b"o")], data_align=16))
            with self.assertRaises(mi.MapIconsError):
                mi.make(ui_pack.PackRun(root, _Table({})), _Plain(), lambda dicts, raw, archive: archive)


if __name__ == "__main__":
    unittest.main()
