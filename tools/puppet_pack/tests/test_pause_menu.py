"""Tests for the pause menu's Multiplayer tab and page (pause_menu.py). Synthetic data only: no
game files.

    python tools/puppet_pack/tests/test_pause_menu.py
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from puppet_pack import msbt, sarc, ui_pack, pause_menu as pm  # noqa: E402
from test_ui_pack import make_layout, make_msbt  # noqa: E402
from test_title_menu import make_layout as title_layout  # noqa: E402
from puppet_pack import title_menu  # noqa: E402

# Shaped as the game's tab strip: four tabs under their own pane, inside the header; the L and R
# labels after them; a footer last. make_layout places pane i at (100 i, -10 i), so the tabs are
# 100 apart, and the header's panes move them (600, -60) from the root.
OVERLAY = [("RootPane", -1), ("N_All_00", 0), ("N_Header_00", 1), ("N_MenuChange_00", 2), ("Pa_Category_00", 3),
           ("Pa_Category_01", 3), ("Pa_Category_02", 3), ("Pa_Category_03", 3), ("N_InOut_01", 2),
           ("T_BtnL_00", 8), ("T_BtnR_00", 8), ("N_Footer_00", 0), ("Pa_Guide_00", 11)]
OVERLAY_PARTS = {"Pa_Category_%02d" % i: ("PaMenuCategory_00", [("State", float(i))]) for i in range(4)}
OVERLAY_PARTS["Pa_Guide_00"] = ("PaGuideKey_00", [])

# Shaped as the game's pages: four pages side by side on a strip, under the menu's In and Out pane,
# which reaches the last pane; an info pane after them.
PAGES = [("RootPane", -1), ("N_All_00", 0), ("N_InOut_00", 1), ("N_MenuChange_01", 2), ("Pa_Collect_00", 3),
         ("Pa_Equip_00", 3), ("Pa_Item_00", 3), ("Pa_System_00", 3), ("N_Info_00", 3), ("T_Text_00", 8),
         ("P_Black_00", 2)]
PAGES_PARTS = {"Pa_Collect_00": ("PaCollectMenu_00", []), "Pa_Equip_00": ("PaEquipmentMenu_00", []),
               "Pa_Item_00": ("PaItemMenu_00", []), "Pa_System_00": ("PaSystemMenu_00", [])}


# Shaped as the System page's own layout (PaSystemMenu_00): the controller picture and five buttons under the
# page's In and Out pane, the last button the last pane.
SYSTEM = [("RootPane", -1), ("N_All_00", 0), ("N_InOut_00", 1), ("Pa_SystemController_00", 2)] +          [("Pa_Button_%02d" % i, 2) for i in range(5)]
SYSTEM_PARTS = {"Pa_SystemController_00": ("PaSystemController_00", [])}
SYSTEM_PARTS.update({"Pa_Button_%02d" % i: ("PaCommonBtnThin_00", []) for i in range(5)})


def overlay():
    return make_layout(OVERLAY, OVERLAY_PARTS)


def system():
    return make_layout(SYSTEM, SYSTEM_PARTS)


def pages():
    return make_layout(PAGES, PAGES_PARTS)


def pane(d, name):
    """(x, y, parent's name, end) of a pane."""
    panes, _ = ui_pack.read_layout(d)
    p = next(p for p in panes if p.name == name)
    x, y = struct.unpack_from("<ff", d, p.record + 0x1c)
    parent = struct.unpack_from("<I", d, p.record + 0x28)[0]
    return x, y, panes[parent].name if parent < len(panes) else None, struct.unpack_from("<I", d, p.record + 0x34)[0]


class PageTest(unittest.TestCase):
    def test_the_page_where_a_shown_page_sits(self):
        d = pages()
        n = pm.add_page(d)
        panes, parts = ui_pack.read_layout(n)
        self.assertEqual([p.name for p in panes[-2:]], ["Pa_Coop_00", "Pa_Settings_00"])
        self.assertEqual([p.name for p in parts[-2:]], ["Pa_Coop_00", "Pa_Settings_00"])
        self.assertEqual(pane(n, "Pa_Settings_00")[:3], pane(n, "Pa_Coop_00")[:3])   # (the same place: the module slides)
        # under N_InOut_00, x 0 (the strip's slide taken off), the System page's y there
        self.assertEqual(pane(n, "Pa_Coop_00"), (0.0, -70.0 - 30.0, "N_InOut_00", len(PAGES) + 1))
        self.assertEqual(pane(n, "Pa_System_00"), pane(d, "Pa_System_00"))
        self.assertEqual(ui_pack._cstr(n, struct.unpack_from("<Q", n, parts[-1].record)[0]), "PaSystemMenu_00")
        self.assertEqual(struct.unpack_from("<I", n, panes[-1].record + 0x38)[0] & 1, 0)   # stored hidden
        # N_InOut_00 and its ancestors reach the copies; P_Black_00, the last pane before, as it was
        self.assertEqual([pane(n, p)[3] for p in ("RootPane", "N_All_00", "N_InOut_00", "P_Black_00")],
                         [len(PAGES) + 2] * 3 + [len(PAGES)])

    def test_refusals(self):
        with self.assertRaises(pm.PauseMenuError):
            pm.add_page(pm.add_page(pages()))
        with self.assertRaises(pm.PauseMenuError):
            pm.add_page(make_layout(PAGES[:7], {k: v for k, v in PAGES_PARTS.items() if k != "Pa_System_00"}))


class RowsTest(unittest.TestCase):
    def test_seven_options_rows_down_the_right_half_stored_hidden(self):
        d = bytearray(system())
        # (the game's buttons override panes of their part: its two override lists, at +0x08 and +0x18, not empty)
        src = next(p for p in ui_pack.read_layout(bytes(d))[1] if p.name == "Pa_Button_00").record
        struct.pack_into("<QII", d, src + 0x08, 0x10, 1, 8)
        d = bytes(d)
        n = pm.add_rows(d)
        panes, parts = ui_pack.read_layout(n)
        self.assertEqual([p.name for p in panes[-pm.ROWS:]], [pm.ROW % i for i in range(pm.ROWS)])
        self.assertEqual([p.name for p in parts[-pm.ROWS:]], [pm.ROW % i for i in range(pm.ROWS)])
        for i in range(pm.ROWS):
            x, y, parent, _ = pane(n, pm.ROW % i)
            self.assertEqual(parent, "N_InOut_00")
            self.assertAlmostEqual(x, pm.ROW_X)
            self.assertAlmostEqual(y, pm.ROW_TOP + pm.ROW_STEP * i)
            rec = panes[len(SYSTEM) + i].record
            self.assertEqual(struct.unpack_from("<I", n, rec + 0x38)[0] & 1, 0)   # (hidden)
            self.assertEqual(struct.unpack_from("<ff", n, rec + 0x2c), pm.ROW_SIZE)
            part = parts[-pm.ROWS + i].record
            self.assertEqual(ui_pack._cstr(n, struct.unpack_from("<Q", n, part)[0]), pm.ROW_LAYOUT)
            self.assertEqual(n[part + 0x08:part + 0x28], bytes(0x20))           # (no overrides)
            self.assertEqual(struct.unpack_from("<QI", n, part + 0x28), (0, 0))  # (no parameters)
        self.assertEqual(struct.unpack_from("<QII", n, src + 0x08), (0x10, 1, 8))   # (the game's button keeps its own)
        for name in [p[0] for p in SYSTEM]:
            self.assertEqual(pane(n, name)[:3], pane(d, name)[:3], name)   # the page's own panes where they were
        with self.assertRaises(pm.PauseMenuError):
            pm.add_rows(n)   # (twice)

    def test_systems_two_buttons_of_ours_rows_below_the_last_stored_hidden(self):
        d = system()
        n = pm.add_system_buttons(pm.add_rows(d))
        panes, parts = ui_pack.read_layout(n)
        ours = [pm.SETTINGS_BUTTON, pm.MULTIPLAYER_BUTTON, pm.CUSTOMIZE_BUTTON]
        self.assertEqual([p.name for p in panes[-3:]], ours)
        self.assertEqual([p.name for p in parts[-3:]], ours)
        self.assertEqual(panes[-1].id, ui_pack.part_id(pm.CUSTOMIZE_BUTTON))
        # one and two row steps (Pa_Button_03 to Pa_Button_04) below Pa_Button_04, beside the page's other buttons
        x3, y3, parent3, _ = pane(n, "Pa_Button_03")
        x4, y4, _, _ = pane(n, "Pa_Button_04")
        self.assertEqual(pane(n, pm.SETTINGS_BUTTON)[:3], (x4, y4 + (y4 - y3), parent3))
        self.assertEqual(pane(n, pm.MULTIPLAYER_BUTTON)[:3], (x4, y4 + 2 * (y4 - y3), parent3))
        self.assertEqual(pane(n, pm.CUSTOMIZE_BUTTON)[:3], (x4, y4 + 3 * (y4 - y3), parent3))
        for part in parts[-3:]:
            self.assertEqual(ui_pack._cstr(n, struct.unpack_from("<Q", n, part.record)[0]), "PaCommonBtnThin_00")
        for p in panes[-2:]:
            self.assertEqual(struct.unpack_from("<I", n, p.record + 0x38)[0] & 1, 0)   # (stored hidden)
        for name in [p[0] for p in SYSTEM]:
            self.assertEqual(pane(n, name)[:3], pane(d, name)[:3], name)   # the page's own panes where they were
        with self.assertRaises(pm.PauseMenuError):
            pm.add_system_buttons(n)   # (twice)

    def test_another_part_layout_needs_its_size_and_takes_no_parameters(self):
        d = system()
        with self.assertRaises(ui_pack.UiPackError):
            ui_pack.clone_parts(d, [ui_pack.Copy("Pa_Button_00", "Pa_X_00", part_layout="PaOther_00")])
        with self.assertRaises(ui_pack.UiPackError):
            ui_pack.clone_parts(d, [ui_pack.Copy("Pa_Button_00", "Pa_X_00", part_layout="PaOther_00", size=(1, 1),
                                                 params={"State": 1.0})])
        with self.assertRaises(ui_pack.UiPackError):
            ui_pack.clone_parts(d, [ui_pack.Copy("Pa_Button_00", "Pa_X_00", size=(1, 1))])


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


LANGS = ("EUfr", "JPja", "USen")


class BuildTest(unittest.TestCase):
    def romfs(self, root, broken=None):
        files = {pm.COMMON_ARCHIVE: sarc.write([(pm.OVERLAY_LAYOUT, overlay()), (pm.PAGES_LAYOUT, pages()),
                                                (pm.SYSTEM_LAYOUT, system()), ("Other_00.buil", b"other")],
                                               data_align=16)}
        for lang in LANGS:
            page = make_msbt([("Pa_System_00-Pa_Button_00-T_Btn_00", "Save")])
            if lang == broken:
                page = page[:0x30]   # cut short: it cannot be read
            files["Mals/%s.Product.110.sarc.zs" % lang] = sarc.write(
                {pm.OVERLAY_MSG: make_msbt([("SystemMenu", "System")]), pm.PAGES_MSG: page,
                 "Other.msbt": b"x"})
        # a language archive without the pause menu's files is left alone
        files["Mals/XXen.Product.110.sarc.zs"] = sarc.write({"Other.msbt": b"x"})
        for rel, data in files.items():
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        return files

    def run_build(self, broken=None, strings=ui_pack.STRINGS):
        with tempfile.TemporaryDirectory() as root:
            src = os.path.join(root, "romfs")
            self.romfs(src, broken)
            table = _Table({pm.COMMON_ARCHIVE[:-3]: 100000, pm.OVERLAY_LAYOUT: 500, pm.PAGES_LAYOUT: 500,
                            pm.SYSTEM_LAYOUT: 500})
            for lang in LANGS:
                table.set("Mals/%s.Product.110.sarc" % lang, 2000)
            before = dict(table.sizes)
            written = {}

            def write(out, rel, data):
                written[rel] = data

            try:
                made = pm.build(src, "out", _Plain(), table, write, lambda d, raw, data: data, log=lambda s: None,
                                strings=strings)
            except (pm.PauseMenuError, struct.error, IndexError, ValueError) as e:
                return None, written, table.sizes, before, e
            return made, written, table.sizes, before, None

    def test_every_file_when_all_can_be_made(self):
        made, written, sizes, before, err = self.run_build()
        self.assertIsNone(err)
        self.assertEqual(set(made), {pm.COMMON_ARCHIVE} | {"Mals/%s.Product.110.sarc.zs" % l for l in LANGS})
        self.assertEqual(set(written), set(made))
        common = dict(sarc.read_entries(written[pm.COMMON_ARCHIVE]))
        self.assertEqual(common[pm.OVERLAY_LAYOUT], overlay())   # (the tab strip: the game's own)
        self.assertIn("Pa_Coop_00", [p.name for p in ui_pack.read_layout(common[pm.PAGES_LAYOUT])[0]])
        self.assertIn(pm.ROW % 6, [p.name for p in ui_pack.read_layout(common[pm.SYSTEM_LAYOUT])[0]])
        self.assertIn(pm.SETTINGS_BUTTON, [p.name for p in ui_pack.read_layout(common[pm.SYSTEM_LAYOUT])[0]])
        self.assertIn(pm.MULTIPLAYER_BUTTON, [p.name for p in ui_pack.read_layout(common[pm.SYSTEM_LAYOUT])[0]])
        self.assertEqual(common["Other_00.buil"], b"other")
        ours = ui_pack.load_strings("USen")
        msgs = sarc.read(written["Mals/USen.Product.110.sarc.zs"])
        self.assertEqual(msbt.read(msgs[pm.OVERLAY_MSG]).text("SystemMenu"), ["System"])   # (as it was)
        page = msbt.read(msgs[pm.PAGES_MSG])
        for i in range(5):
            label = "Pa_Coop_00-Pa_Button_%02d-T_Btn_00" % i
            self.assertEqual(page.text(label), [ours[pm.PAGES_MSG][label]])
        self.assertEqual(page.text("Pa_System_00-Pa_Button_00-T_Btn_00"), ["Save"])
        self.assertEqual(page.text("Pa_System_00-Pa_Button_05-T_Btn_00"), ["Longshot Settings"])
        self.assertEqual(page.text("Pa_System_00-Pa_Button_06-T_Btn_00"), ["Multiplayer"])
        # The module's live texts (coop_pause_tab_module.cpp, kLiveSlots / kLivePlaceholder): Coop_Live_00..47 (the
        # rows' and the browser's cells'), each exactly 64 dots, in the built message file (the module refuses any
        # other text or length).
        for i in range(48):
            label = "Coop_Live_%02d" % i
            self.assertEqual(page.text(label), ["." * 64], label)
        self.assertTrue(all(sizes[k] > before[k] for k in before
                            if k not in (pm.OVERLAY_LAYOUT, pm.PAGES_LAYOUT, pm.SYSTEM_LAYOUT)))
        self.assertTrue(sizes[pm.SYSTEM_LAYOUT] > before[pm.SYSTEM_LAYOUT])

    def test_a_translation_over_english(self):
        with tempfile.TemporaryDirectory() as folder:
            with open(os.path.join(ui_pack.STRINGS, "en.txt"), encoding="utf-8") as f:
                en = f.read()
            with open(os.path.join(folder, "en.txt"), "w", encoding="utf-8") as f:
                f.write(en)
            with open(os.path.join(folder, "fr.txt"), "w", encoding="utf-8") as f:
                f.write("[LayoutMsg/PlusMenu_00.msbt]\nPa_System_00-Pa_Button_06-T_Btn_00 = Multijoueur\n")
            made, written, _, _, err = self.run_build(strings=folder)
            self.assertIsNone(err)
            fr = msbt.read(sarc.read(written["Mals/EUfr.Product.110.sarc.zs"])[pm.PAGES_MSG])
            self.assertEqual(fr.text("Pa_System_00-Pa_Button_06-T_Btn_00"), ["Multijoueur"])
            en_page = msbt.read(sarc.read(written["Mals/EUfr.Product.110.sarc.zs"])[pm.PAGES_MSG])
            self.assertEqual(en_page.text("Pa_Coop_00-Pa_Button_04-T_Btn_00"), ["Leave"])   # English kept

    def test_the_title_rows_then_the_tab_keep_both_label_sets_and_grow_once(self):
        """title_menu.build then pause_menu.build on one made-up romfs with one ui_pack.PackRun (as build.py
        runs them): each language archive has both steps' labels, and its size-table entry grew once from the
        game's original (its size before, plus the file's growth over both steps, plus 0x1000)."""
        with tempfile.TemporaryDirectory() as root:
            src = os.path.join(root, "romfs")
            files = {pm.COMMON_ARCHIVE: sarc.write([(pm.OVERLAY_LAYOUT, overlay()), (pm.PAGES_LAYOUT, pages()),
                                                    (pm.SYSTEM_LAYOUT, system())], data_align=16),
                     title_menu.TITLE_ARCHIVE: sarc.write([(title_menu.TITLE_LAYOUT, title_layout())], data_align=16)}
            for lang in LANGS:
                files["Mals/%s.Product.110.sarc.zs" % lang] = sarc.write(
                    {pm.OVERLAY_MSG: make_msbt([("SystemMenu", "System")]),
                     pm.PAGES_MSG: make_msbt([("Pa_System_00-Pa_Button_00-T_Btn_00", "Save")]),
                     title_menu.MSG_FILE: make_msbt([("0000", "New Game")])})
            for rel, data in files.items():
                path = os.path.join(src, *rel.split("/"))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(data)
            table = _Table({pm.COMMON_ARCHIVE[:-3]: 100000, title_menu.TITLE_ARCHIVE[:-3]: 50000})
            for lang in LANGS:
                table.set("Mals/%s.Product.110.sarc" % lang, 2000)
            before = dict(table.sizes)
            written = {}
            run = ui_pack.PackRun(src, table)
            same = lambda d, raw, data: data
            title_menu.build(src, "out", _Plain(), table, lambda o, r, data: written.__setitem__(r, data), same,
                             log=lambda s: None, run=run)
            pm.build(src, "out", _Plain(), table, lambda o, r, data: written.__setitem__(r, data), same,
                     log=lambda s: None, run=run)
            for lang in LANGS:
                rel = "Mals/%s.Product.110.sarc.zs" % lang
                msgs = sarc.read(written[rel])
                self.assertEqual(msbt.read(msgs[title_menu.MSG_FILE]).text("0100"), ["Host"])   # the title's rows
                self.assertEqual(msbt.read(msgs[pm.PAGES_MSG]).text("Pa_System_00-Pa_Button_06-T_Btn_00"), ["Multiplayer"])
                self.assertEqual(msbt.read(msgs[pm.PAGES_MSG]).text("Pa_Coop_00-Pa_Button_04-T_Btn_00"), ["Leave"])
                grown = len(written[rel]) - len(files[rel])
                self.assertEqual(table.sizes[rel[:-3]], before[rel[:-3]] + grown + 0x1000)   # once, from the original
            self.assertEqual(table.sizes[pm.COMMON_ARCHIVE[:-3]],
                             before[pm.COMMON_ARCHIVE[:-3]] + len(written[pm.COMMON_ARCHIVE]) - len(files[pm.COMMON_ARCHIVE])
                             + 0x1000)

    def test_the_last_runs_files_are_not_read(self):
        """The mod's folder holds the last run's files: this step starts from the game's again (a second run made the
        page twice: "the layout has Pa_Coop_00 already")."""
        with tempfile.TemporaryDirectory() as root:
            src, out = os.path.join(root, "romfs"), os.path.join(root, "out")
            self.romfs(src)
            table = _Table({pm.COMMON_ARCHIVE[:-3]: 100000})
            for lang in LANGS:
                table.set("Mals/%s.Product.110.sarc" % lang, 2000)

            def write(o, r, data):
                path = os.path.join(o, *r.split("/"))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(data)

            for _ in range(2):
                made = pm.build(src, out, _Plain(), table, write, lambda d, raw, data: data, log=lambda s: None)
                self.assertIn(pm.COMMON_ARCHIVE, made)

    def test_nothing_written_when_one_language_fails(self):
        made, written, sizes, before, err = self.run_build(broken="JPja")
        self.assertIsNotNone(err)
        self.assertEqual(written, {})
        self.assertEqual(sizes, before)


class NupFilesTest(unittest.TestCase):
    def test_the_menus_files_newest_language_archives(self):
        """A .nup build extracts the menus' archives too, and each language's newest message archive only."""
        import fnmatch
        from puppet_pack import build

        class FakeNup:
            files = ["UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs", pm.COMMON_ARCHIVE,
                     "Mals/USen.Product.100.sarc.zs", "Mals/USen.Product.110.sarc.zs", "Mals/EUfr.Product.110.sarc.zs",
                     "Mals/JPja.Product.100.sarc.zs", "Pack/ZsDic.pack.zs"]

            def glob(self, pattern):
                d, _, name = pattern.rpartition("/")
                return sorted(f for f in self.files if f.rpartition("/")[0] == d and fnmatch.fnmatchcase(f.rpartition("/")[2], name))

        self.assertEqual(build.nup_menu_files(FakeNup()), [
            "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs", pm.COMMON_ARCHIVE,
            "Mals/EUfr.Product.110.sarc.zs", "Mals/JPja.Product.100.sarc.zs", "Mals/USen.Product.110.sarc.zs"])


if __name__ == "__main__":
    unittest.main()
