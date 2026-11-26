"""Tests for coop_menu.py: co-op's own - menu screen CoopMenu_00 (tabs-overhaul-01110 step 2)."""
import hashlib
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

from puppet_pack import byml, charsel_ui, coop_menu as cm, pause_menu as pm, sarc, ui_pack  # noqa: E402
from test_ui_pack import make_layout, make_msbt  # noqa: E402
from test_pause_menu import _Plain, _Table, pages, system  # noqa: E402

# Shaped as Key Items' layout (ImportantMenu_01): the vignette window and the guide group with the ring of items, the
# detail box, the demo message.
KEY_ITEMS = [("RootPane", -1), ("N_All_00", 0), ("N_IsOpen_00", 1), ("W_Window_00", 2), ("N_GuideIn_00", 2),
             ("N_IsGuide_00", 4), ("Pa_Parts_00", 5)] + [("N_Info_%02d" % i, 5) for i in range(9)] + \
            [("N_DetailIn_00", 2), ("T_Name_00", 16), ("N_DemoMessage", 0), ("T_Message_00", 18)]
KEY_ITEMS_PARTS = {"Pa_Parts_00": ("PaImportantInfo_00", [])}


def key_items():
    return make_layout(KEY_ITEMS, KEY_ITEMS_PARTS)


def shown(d, name):
    panes, _ = ui_pack.read_layout(d)
    p = next(p for p in panes if p.name == name)
    return bool(struct.unpack_from("<I", d, p.record + 0x38)[0] & 1)


def parent_of(d, name):
    panes, _ = ui_pack.read_layout(d)
    p = next(p for p in panes if p.name == name)
    return panes[struct.unpack_from("<I", d, p.record + 0x28)[0]].name


class LayoutTest(unittest.TestCase):
    def test_rename(self):
        d = cm.rename(key_items(), "CoopMenu_00")
        self.assertEqual(cm.layout_name(d), "CoopMenu_00")
        self.assertEqual([p.name for p in ui_pack.read_layout(d)[0]], [n for n, _ in KEY_ITEMS])
        self.assertEqual(struct.unpack_from("<I", d, 8)[0], len(d))

    def test_layout(self):
        d = cm.make_layout(key_items(), (-600.0, 40.0), (2600.0, 1400.0))
        self.assertEqual(cm.layout_name(d), cm.SCREEN)
        for n in cm.HIDE:
            self.assertFalse(shown(d, n), n)
        for n in ("W_Window_00", "N_IsGuide_00", "N_All_00"):
            self.assertTrue(shown(d, n), n)
        self.assertEqual(parent_of(d, pm.MINUS_PAGE), cm.PAGE_PARENT)
        self.assertFalse(shown(d, pm.MINUS_PAGE))   # (the module shows it with its words)
        x, y = ui_pack.frame_offset(d, pm.MINUS_PAGE, "RootPane")
        px, py = struct.unpack_from("<ff", d, next(p for p in ui_pack.read_layout(d)[0]
                                                   if p.name == pm.MINUS_PAGE).record + 0x1c)
        self.assertAlmostEqual(x + px, -600.0, places=3)
        self.assertAlmostEqual(y + py, 40.0, places=3)

    def test_layout_refused(self):
        with self.assertRaises(cm.CoopMenuError):
            cm.make_layout(make_layout([("RootPane", -1), ("N_All_00", 0)], {}), (0, 0), (1, 1))
        with self.assertRaises(cm.CoopMenuError):   # (ours already there)
            cm.make_layout(cm.make_layout(key_items(), (0, 0), (1, 1)), (0, 0), (1, 1))


class TablesTest(unittest.TestCase):
    def test_category(self):
        cat = {"Common": ["ActionGuide_00", "CookMenu_00", "DemoMenu_00", cm.SOURCE], "Title": ["Title_00"]}
        out = cm.add_category(cat)
        self.assertEqual(out["Common"], ["ActionGuide_00", "CookMenu_00", cm.SCREEN, "DemoMenu_00", cm.SOURCE])
        self.assertEqual(cm.add_category(out)["Common"].count(cm.SCREEN), 1)
        with self.assertRaises(cm.CoopMenuError):
            cm.add_category({"Common": ["Other_00"]})

    def test_row(self):
        rows = [{"Name": "ImportantMenu_00", "BodyLayoutFile": cm.SOURCE}, {"Name": "MenuBG_00"}]
        out = cm.add_row(rows)
        self.assertEqual([r["Name"] for r in out], ["CoopMenu_00", "ImportantMenu_00", "MenuBG_00"])
        self.assertEqual(out[0], cm.ROW)
        self.assertEqual(len(cm.add_row(out)), 3)
        with self.assertRaises(cm.CoopMenuError):
            cm.add_row([{"Name": "MenuBG_00"}])

    def test_row_draws_over_the_pages(self):
        """Over the menu pages' screens (the Map's stays open under ours: at the same key it drew over ours, LANE1
        2026-11-22), under the HUD's and the strip's; no IsEnableControl (the pad's screen unchanged)."""
        self.assertGreater(cm.ROW["SortKey"], 5000)
        self.assertLess(cm.ROW["SortKey"], 6000)
        self.assertNotIn("IsEnableControl", cm.ROW)


MAP_GAME = b"AIB " + bytes(range(256)) * 4
MAP_OURS = MAP_GAME[:600] + b"co-op's page" * 20 + MAP_GAME[600:]
LANGS = ("EUfr", "USen")


class BuildTest(unittest.TestCase):
    def setUp(self):
        self.saved = cm.DATA, cm.MAP_DST_SHA1
        self.data = tempfile.TemporaryDirectory()
        with open(os.path.join(self.data.name, "map.cdelta"), "wb") as f:
            f.write(charsel_ui.make_delta(MAP_GAME, MAP_OURS))
        cm.DATA, cm.MAP_DST_SHA1 = self.data.name, hashlib.sha1(MAP_OURS).hexdigest()

    def tearDown(self):
        cm.DATA, cm.MAP_DST_SHA1 = self.saved
        self.data.cleanup()

    def romfs(self, root, map_file=MAP_GAME):
        files = {
            cm.COMMON_ARCHIVE: sarc.write([(cm.SOURCE + ".buil", key_items())] * 4 +
                                          [(pm.PAGES_LAYOUT, pages()), (pm.SYSTEM_LAYOUT, system())], data_align=16),
            cm.CATEGORY: byml.dumps({"Common": ["ActionGuide_00", cm.SOURCE, "MenuBG_00"]}),
            "RSDB/UIScreen.Product.111.rstbl.byml.zs": byml.dumps([{"Name": "ImportantMenu_00",
                                                                    "BodyLayoutFile": cm.SOURCE}]),
            cm.MAP: map_file,
        }
        for lang in LANGS:
            files["Mals/%s.Product.110.sarc.zs" % lang] = sarc.write(
                {cm.MSG % cm.SOURCE: make_msbt([("T_Name_00", "Key")]), "Other.msbt": b"x"})
        files["Mals/XXen.Product.110.sarc.zs"] = sarc.write({"Other.msbt": b"x"})
        for rel, data in files.items():
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)

    def run_build(self, map_file=MAP_GAME):
        with tempfile.TemporaryDirectory() as root:
            self.romfs(root, map_file)
            table = _Table({cm.COMMON_ARCHIVE[:-3]: 100000, cm.CATEGORY[:-3]: 900,
                            "RSDB/UIScreen.Product.111.rstbl.byml": 900})
            for lang in LANGS:
                table.set("Mals/%s.Product.110.sarc" % lang, 2000)
            written = {}
            try:
                cm.build(root, "out", _Plain(), table, lambda out, rel, data: written.__setitem__(rel, data),
                         lambda d, raw, data: data, log=lambda s: None)
            except (cm.CoopMenuError, charsel_ui.CharselError, ui_pack.UiPackError) as e:
                return written, table, e
            return written, table, None

    def test_every_file(self):
        written, table, err = self.run_build()
        self.assertIsNone(err)
        self.assertEqual(written[cm.MAP], MAP_OURS)
        entries = sarc.read_entries(written[cm.COMMON_ARCHIVE])
        ours = [d for n, d in entries if n == cm.SCREEN + ".buil"]
        self.assertEqual(len(ours), cm.COPIES)
        self.assertEqual(cm.layout_name(ours[0]), cm.SCREEN)
        self.assertIn(pm.MINUS_PAGE, [p.name for p in ui_pack.read_layout(ours[0])[0]])
        self.assertIn(cm.SCREEN, byml.loads(written[cm.CATEGORY])["Common"])
        self.assertIn(cm.ROW, byml.loads(written["RSDB/UIScreen.Product.111.rstbl.byml.zs"]))
        for lang in LANGS:
            msgs = sarc.read(written["Mals/%s.Product.110.sarc.zs" % lang])
            self.assertEqual(msgs[cm.MSG % cm.SCREEN], msgs[cm.MSG % cm.SOURCE])
        self.assertNotIn("Mals/XXen.Product.110.sarc.zs", written)
        self.assertGreater(table.get(cm.COMMON_ARCHIVE[:-3]), 100000)

    def test_another_map_sequence_makes_nothing(self):
        written, _, err = self.run_build(map_file=MAP_GAME[:-1] + b"!")
        self.assertIsInstance(err, charsel_ui.CharselError)
        self.assertEqual(written, {})


class StripTest(unittest.TestCase):
    def test_strip_without_the_page(self):
        """With co-op's own screen the strip gets the - menu's tabs but not the page (it is the screen's)."""
        from test_pause_menu import BuildTest as PauseBuild
        with tempfile.TemporaryDirectory() as root:
            PauseBuild().romfs(root)
            for page, want in ((True, True), (False, False)):
                files, _ = pm.make(ui_pack.PackRun(root, _Table({})), _Plain(), lambda d, raw, data: data,
                                   minus_page=page)
                strip = dict(sarc.read_entries(files[pm.COMMON_ARCHIVE]))[pm.OVERLAY_LAYOUT]
                names = [p.name for p in ui_pack.read_layout(strip)[0]]
                self.assertIn("Pa_Category_04", names)
                self.assertEqual(pm.MINUS_PAGE in names, want)


GAME = os.environ.get("COOP_TEST_ROMFS", "")


@unittest.skipUnless(GAME and os.path.isdir(GAME), "COOP_TEST_ROMFS: the game's romfs (read only), for the real files")
class GameFilesTest(unittest.TestCase):
    def test_real_files(self):
        from puppet_pack import zs
        dicts = zs.Dicts.from_romfs(GAME)
        files, growth = cm.make(ui_pack.PackRun(GAME, _Table({})), dicts, lambda d, raw, data: data)
        self.assertEqual(hashlib.sha1(files[cm.MAP]).hexdigest(), cm.MAP_DST_SHA1)
        # (L back to the Map goes through a two-frame gate: the game's file has no wait node)
        self.assertIn(b"SeqExecuteGenericWaitFrame", files[cm.MAP])
        rows = byml.loads(files["RSDB/UIScreen.Product.111.rstbl.byml.zs"])
        keys = {r["Name"]: r.get("SortKey") for r in rows}
        # every screen at the pages' 5000 is a page of the pause menu (the - menu's three, the + menu's and its Action
        # Guide list), none a popup that ours (5001) would now hide: those are all far above (windows 8751, the
        # message dialog 9000)
        self.assertEqual(sorted(n for n, k in keys.items() if k == 5000),
                         ["ChallengeMenu_00", "ImportantMenu_00", "MapMenu_00", "PlusMenu_00", "SystemActionGuide_00"])
        self.assertGreater(keys[cm.SCREEN], 5000)
        self.assertEqual([n for n, k in keys.items() if k is not None and 5000 < k < 6000], [cm.SCREEN])
        for over in ("MenuOverlay_00", "SystemWindow_00", "AmiiboWindow_00", "MessageDialog_00"):
            self.assertGreater(keys[over], keys[cm.SCREEN], over)
        windows = [n for n in keys if "Window" in n or "Dialog" in n]
        self.assertTrue(windows)
        for n in windows:
            self.assertGreater(keys[n], keys[cm.SCREEN], n)
        lay = next(d for n, d in sarc.read_entries(files[cm.COMMON_ARCHIVE]) if n == cm.SCREEN + ".buil")
        names = [p.name for p in ui_pack.read_layout(lay)[0]]
        self.assertIn(pm.MINUS_PAGE, names)
        self.assertEqual(cm.layout_name(lay), cm.SCREEN)
        self.assertGreaterEqual(len([r for r in growth if r[0].startswith("Mals/")]), 1)


if __name__ == "__main__":
    unittest.main()
