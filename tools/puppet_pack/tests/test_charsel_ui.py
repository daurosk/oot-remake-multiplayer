"""Tests for co-op's own Characters tab (charsel_ui.py): the delta format and the pack step on a fake game folder.
Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_charsel_ui.py
"""
import json
import os
import random
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from puppet_pack import charsel_ui as cu, rstb, sarc, ui_pack  # noqa: E402
from test_ui_pack import make_msbt  # noqa: E402


class PlainDicts:
    """The pack's dicts for files that are stored plain (the test's '.zs' files are not compressed)."""

    @staticmethod
    def decompress(raw):
        return raw


def recompress(_dicts, _raw, plain):
    return plain


def noise(n, seed):
    r = random.Random(seed)
    return bytes(r.randrange(256) for _ in range(n))


class DeltaTest(unittest.TestCase):
    def test_round_trip(self):
        src = noise(5000, 1)
        dst = src[:1200] + b"new bytes of ours" + src[3000:4000] + src[100:900] + noise(300, 2)
        d = cu.make_delta(src, dst)
        self.assertEqual(cu.apply_delta(src, d), dst)
        self.assertLess(len(d), 700)            # the copies are references, not bytes

    def test_new_file_and_empty(self):
        for dst in (b"", b"only inserted"):
            self.assertEqual(cu.apply_delta(b"", cu.make_delta(b"", dst)), dst)

    def test_other_source_refused(self):
        src = noise(800, 3)
        d = cu.make_delta(src, src[::-1])
        with self.assertRaises(cu.CharselError):
            cu.apply_delta(src[:-1] + b"x", d)

    def test_damaged_delta_refused(self):
        src = noise(800, 4)
        d = bytearray(cu.make_delta(src, src[:400] + b"abc"))
        d[-3] ^= 0xFF                           # a byte of the insert
        with self.assertRaises(cu.CharselError):
            cu.apply_delta(src, bytes(d))
        with self.assertRaises(cu.CharselError):
            cu.apply_delta(src, b"CDLT0\0" + bytes(d[6:]))


class Table:
    """rstb.Rstb's calls the step makes."""

    def __init__(self, sizes):
        self.sizes = dict(sizes)

    def get(self, key):
        return self.sizes.get(key)

    def set(self, key, size):
        self.sizes[key] = size

    def add_new(self, key, size):
        self.sizes[key] = size


class PackStepTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = self.tmp.name
        self.romfs = os.path.join(root, "romfs")
        self.data = os.path.join(root, "data")
        os.makedirs(os.path.join(self.romfs, "Sequence"))
        os.makedirs(os.path.join(self.romfs, "UI"))
        os.makedirs(os.path.join(self.romfs, "Mals"))
        os.makedirs(self.data)
        self.graph = noise(3000, 5)
        self.layout = noise(4000, 6)
        self.overlay = noise(2000, 7)
        with open(os.path.join(self.romfs, "Sequence", "Minus.ainb"), "wb") as f:
            f.write(self.graph)
        arc = sarc.write([("ImportantMenu_01.buil", self.layout), ("MenuOverlay_00.buil", self.overlay)] * 2,
                         data_align=16)
        with open(os.path.join(self.romfs, "UI", "Common.blarc.zs"), "wb") as f:
            f.write(arc)
        for lang in ("USen", "JPja"):
            msgs = sarc.write([("LayoutMsg/MenuOverlay_00.msbt", make_msbt([("Important", "Key Items")]))])
            with open(os.path.join(self.romfs, "Mals", lang + ".sarc.zs"), "wb") as f:
                f.write(msgs)
        # what Snail's build made of them, for Link
        self.graph_new = self.graph[:2000] + b"ours" + self.graph[2000:]
        self.page = self.layout[:3000] + b"the page's own panes" + self.layout[500:1500]
        self.overlay_new = self.overlay[:100] + b"\x00\x00\x2c\xc4" + self.overlay[104:]
        self.menu_graph = self.graph[:1000] + b"a new graph from the game's"
        self.custom_msg = make_msbt([("T_Head_00", "Customize")])
        deltas = {
            "minus.cdelta": cu.make_delta(self.graph, self.graph_new),
            "menu.cdelta": cu.make_delta(self.graph, self.menu_graph),
            "page.cdelta": cu.make_delta(self.layout, self.page),
            "overlay.cdelta": cu.make_delta(self.overlay, self.overlay_new),
            "custom_msg.cdelta": cu.make_delta(b"", self.custom_msg),
        }
        for name, d in deltas.items():
            with open(os.path.join(self.data, name), "wb") as f:
                f.write(d)
        self.manifest = {"format": "coop-charsel 1", "files": [
            {"rel": "Sequence/Minus.ainb", "zs": False, "from": "Sequence/Minus.ainb", "delta": "minus.cdelta"},
            {"rel": "Sequence/CharacterMenu.ainb", "zs": False, "from": "Sequence/Minus.ainb", "delta": "menu.cdelta"},
            {"rel": "UI/Common.blarc.zs", "zs": True, "align": 16, "members": [
                {"name": "MenuOverlay_00.buil", "from": "MenuOverlay_00.buil", "delta": "overlay.cdelta"},
                {"name": "CharacterMenu_00.buil", "from": "ImportantMenu_01.buil", "delta": "page.cdelta",
                 "copies": 2}]},
            {"glob": "Mals/*.sarc.zs", "zs": True, "align": 16, "members": [
                {"name": "LayoutMsg/CharacterCustomize_00.msbt", "from": None, "delta": "custom_msg.cdelta"}],
             "labels": {"LayoutMsg/MenuOverlay_00.msbt": {"Characters": "Characters"}}},
        ]}
        self.table = Table({"Sequence/Minus.ainb": 4000, "UI/Common.blarc": 20000, "Mals/USen.sarc": 3000,
                            "Mals/JPja.sarc": 3000})

    def tearDown(self):
        self.tmp.cleanup()

    def run_step(self):
        written = {}
        files = cu.build(self.romfs, "out", PlainDicts, self.table, lambda root, rel, d: written.__setitem__(rel, d),
                         recompress, log=lambda line: None, manifest=self.manifest, data_dir=self.data)
        return files, written

    def test_files_made_from_the_players_own(self):
        files, written = self.run_step()
        self.assertEqual(sorted(files), sorted(written))
        self.assertEqual(written["Sequence/Minus.ainb"], self.graph_new)
        self.assertEqual(written["Sequence/CharacterMenu.ainb"], self.menu_graph)
        common = sarc.read_entries(written["UI/Common.blarc.zs"])
        self.assertEqual([d for n, d in common if n == "MenuOverlay_00.buil"], [self.overlay_new] * 2)
        self.assertEqual([d for n, d in common if n == "CharacterMenu_00.buil"], [self.page] * 2)
        self.assertEqual([d for n, d in common if n == "ImportantMenu_01.buil"], [self.layout] * 2)
        for lang in ("USen", "JPja"):
            msgs = dict(sarc.read_entries(written["Mals/%s.sarc.zs" % lang]))
            self.assertEqual(msgs["LayoutMsg/CharacterCustomize_00.msbt"], self.custom_msg)
            self.assertIn("Characters", ui_pack.labels_of(msgs["LayoutMsg/MenuOverlay_00.msbt"])
                          if hasattr(ui_pack, "labels_of") else
                          msgs["LayoutMsg/MenuOverlay_00.msbt"].decode("latin-1"))

    def test_size_table(self):
        _files, written = self.run_step()
        self.assertGreater(self.table.get("UI/Common.blarc"), 20000)
        self.assertEqual(self.table.get("Sequence/CharacterMenu.ainb"), len(self.menu_graph) + 0x1000)

    def test_all_or_nothing_on_another_game(self):
        with open(os.path.join(self.romfs, "Sequence", "Minus.ainb"), "wb") as f:
            f.write(self.graph[:-1] + b"?")
        written = {}
        with self.assertRaises(cu.CharselError):
            cu.build(self.romfs, "out", PlainDicts, self.table, lambda root, rel, d: written.__setitem__(rel, d),
                     recompress, log=lambda line: None, manifest=self.manifest, data_dir=self.data)
        self.assertEqual(written, {})
        self.assertIsNone(self.table.get("Sequence/CharacterMenu.ainb"))

    def test_shared_view_for_the_pause_menu(self):
        run = ui_pack.PackRun(self.romfs, self.table)
        cu.build(self.romfs, "out", PlainDicts, self.table, lambda *a: None, recompress, log=lambda line: None,
                 run=run, manifest=self.manifest, data_dir=self.data)
        common = dict(sarc.read_entries(run.read("UI/Common.blarc.zs")))
        self.assertEqual(common["MenuOverlay_00.buil"], self.overlay_new)   # pause_menu reads the four-tab header

    def test_code_patch_written(self):
        src = os.path.join(self.data, "x.ips")
        with open(src, "wb") as f:
            f.write(b"IPS32EEOF")
        mod = os.path.join(self.tmp.name, "pack")
        cu.write_code_patch(mod, src)
        with open(os.path.join(mod, "exefs", cu.BUILD_ID + ".ips"), "rb") as f:
            self.assertEqual(f.read(), b"IPS32EEOF")

    def test_code_patch_removed(self):
        mod = os.path.join(self.tmp.name, "pack")
        src = os.path.join(self.data, "x.ips")
        with open(src, "wb") as f:
            f.write(b"IPS32EEOF")
        cu.write_code_patch(mod, src)
        cu.remove_code_patch(mod)
        self.assertFalse(os.path.exists(os.path.join(mod, "exefs", cu.code_patch_name())))
        cu.remove_code_patch(mod)   # (none there: no error)

    def test_our_words_per_language(self):
        """The new message files get strings/<language> over his English: the archive's language, en elsewhere."""
        strings = os.path.join(self.tmp.name, "strings")
        os.makedirs(strings)
        with open(os.path.join(strings, "en.txt"), "w", encoding="utf-8") as f:
            f.write("[LayoutMsg/CharacterCustomize_00.msbt]\nT_Head_00 = Customize Link\n")
        with open(os.path.join(strings, "ja.txt"), "w", encoding="utf-8") as f:
            f.write("[LayoutMsg/CharacterCustomize_00.msbt]\nT_Head_00 = カスタマイズ\n")
        self.manifest["files"][3]["strings"] = ["LayoutMsg/CharacterCustomize_00.msbt"]
        files, _g, _n = cu.make(ui_pack.PackRun(self.romfs, self.table), PlainDicts, recompress, self.manifest,
                                self.data, strings=strings)

        def head(lang):
            m = dict(sarc.read_entries(files["Mals/%s.sarc.zs" % lang]))["LayoutMsg/CharacterCustomize_00.msbt"]
            _h, _s, labels, texts = ui_pack.read_msbt(m)
            return texts[labels["T_Head_00"]].decode("utf-8").rstrip("\0")
        self.assertEqual(head("USen"), "Customize Link")
        self.assertEqual(head("JPja"), "カスタマイズ")

    def test_sources(self):
        self.assertEqual(cu.sources(self.manifest), ["Sequence/Minus.ainb", "Sequence/CharacterMenu.ainb",
                                                     "UI/Common.blarc.zs"])

    def test_shipped_manifest_reads(self):
        if os.path.exists(cu.MANIFEST):
            m = cu.load_manifest()
            for e in m["files"]:
                for name in [e.get("delta")] + [x["delta"] for x in e.get("members", [])]:
                    if name:
                        self.assertTrue(os.path.exists(os.path.join(cu.DATA, name)), name)

    def test_data_hash_pinned(self):
        """A change of charsel/data alone must change charsel_ui.py (the tool's hash: packs are made again)."""
        if os.path.exists(cu.MANIFEST):
            self.assertEqual(cu.data_hash(), cu.DATA_SHA256,
                             "charsel/data changed: set charsel_ui.DATA_SHA256 to %s" % cu.data_hash())
        d = os.path.join(self.tmp.name, "hd")
        os.makedirs(d)
        with open(os.path.join(d, "a.cdelta"), "wb") as f:
            f.write(b"x")
        one = cu.data_hash(d)
        with open(os.path.join(d, "a.cdelta"), "wb") as f:
            f.write(b"y")
        self.assertNotEqual(cu.data_hash(d), one)

    def test_shipped_strings_have_the_page_note(self):
        """The page's line crediting Snail (make_data.py stage 2's T_Note_00) in English, with its line break."""
        en = ui_pack.load_strings("USen")
        note = en["LayoutMsg/CharacterMenu_00.msbt"]["T_Note_00"]
        self.assertIn("Snail", note)
        self.assertIn("\n", note)


if __name__ == "__main__":
    unittest.main()
