"""Tests of puppet_pack.self_tunic (your own tunic files at each start, colour-agree). CI runs
them through test_puppet_pack.py."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import self_tunic  # noqa: E402


class SelfTunicTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.pack = os.path.join(self.dir, "coop-puppet")
        os.makedirs(os.path.join(self.pack, "coop_self"))
        for name, text in (("red.Angin_Tile_S_Alb.bntx.zs", "REDS"), ("red.Player_Child.bfres.zs", "REDCHILD"),
                           ("black.Angin_Tile_S_Alb.bntx.zs", "BLACKS")):
            with open(os.path.join(self.pack, "coop_self", name), "w") as f:
                f.write(text)
        self.manifest({"tunics": {"red": ["x"], "black": ["x"], "green": ["y"]}, "self_files": {
            "red": {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/red.Angin_Tile_S_Alb.bntx.zs",
                    "Model/Player_Child.Player_Child.bfres.zs": "coop_self/red.Player_Child.bfres.zs",
                    "Model/Other.bfres.zs": "coop_self/red.Player_Child.bfres.zs"},
            "black": {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/black.Angin_Tile_S_Alb.bntx.zs"}}})
        self.tile = os.path.join(self.pack, "romfs", "Tex", "Angin_Tile_S_Alb.bntx.zs")
        self.model = os.path.join(self.pack, "romfs", "Model", "Player_Child.Player_Child.bfres.zs")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def manifest(self, m):
        with open(os.path.join(self.pack, "coop_puppet.json"), "w") as f:
            f.write(m if isinstance(m, str) else json.dumps(m))

    def text(self, path):
        with open(path) as f:
            return f.read()

    def test_colours_as_the_module_reads_them(self):
        self.assertEqual(self_tunic.tunic_of("Pink"), "pink")
        for c in ("#FF8020", "FF8020", '"#FF8020"', "0xFF8020", "#FF8020FF", "rgb(255,128,32)"):
            self.assertEqual(self_tunic.tunic_of(c), "orange", c)
        self.assertEqual(self_tunic.tunic_of("gray"), "white")
        self.assertEqual(self_tunic.tunic_of("brown"), "black")
        self.assertEqual(self_tunic.tunic_of("#222"), "black")
        self.assertEqual(self_tunic.tunic_of(""), "")
        self.assertEqual(self_tunic.tunic_of("nope"), "")
        # every pack colour is a colour the module knows by name, and is its own nearest
        for c in self_tunic.TUNICS:
            self.assertEqual(self_tunic.tunic_of(c), c)

    def test_start_colour_switch_then_options_then_coop_ini(self):
        opts, coop = os.path.join(self.dir, "coop-options.ini"), os.path.join(self.dir, "coop.ini")
        with open(opts, "w") as f:
            f.write("# color = red\n")
        with open(coop, "w") as f:
            f.write("server = 127.0.0.1\ncolor = pink\n")
        self.assertEqual(self_tunic.start_color("", opts, coop), "pink")
        with open(opts, "w") as f:
            f.write("color = white\n")
        self.assertEqual(self_tunic.start_color("", opts, coop), "white")
        self.assertEqual(self_tunic.start_color("black", opts, coop), "black")
        self.assertEqual(self_tunic.start_color("black", opts, coop, tunics="0"), "")
        self.assertEqual(self_tunic.start_color("", None, os.path.join(self.dir, "none.ini")), "")

    def test_files_follow_the_colour_on_every_start(self):
        # the owner's lane1 run: black files left from an earlier start, pink chosen
        self.assertEqual(self_tunic.put(self.pack, "black"), "black")
        self.assertEqual(self.text(self.tile), "BLACKS")
        self.assertEqual(self_tunic.put(self.pack, "pink"), "")   # no pink in this pack: none, not black
        self.assertFalse(os.path.exists(self.tile))
        self.assertEqual(self_tunic.put(self.pack, "#FF3030"), "red")
        self.assertEqual(self.text(self.tile), "REDS")
        self.assertEqual(self.text(self.model), "REDCHILD")
        self.assertFalse(os.path.exists(os.path.join(self.pack, "romfs", "Model", "Other.bfres.zs")))
        self.assertEqual(self_tunic.put(self.pack, "green"), "")   # (a pack with no green own files)
        self.assertFalse(os.path.exists(self.tile) or os.path.exists(self.model))

    def test_green_and_none_wear_greens_own_files(self):
        # npc-colour: the game's tunic textures are on Mido's and Saria's models (every Kokiri), so
        # your own Link wears its own copies in green too, and with no colour set
        with open(os.path.join(self.pack, "coop_self", "green.Angin_Tile_S_Alb.bntx.zs"), "w") as f:
            f.write("GREENS")
        m = json.loads(self.text(os.path.join(self.pack, "coop_puppet.json")))
        m["self_files"]["green"] = {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/green.Angin_Tile_S_Alb.bntx.zs"}
        self.manifest(m)
        for colour in ("green", "", "nope"):
            self.assertEqual(self_tunic.put(self.pack, "red"), "red")
            self.assertEqual(self_tunic.put(self.pack, colour), "green", colour)
            self.assertEqual(self.text(self.tile), "GREENS")
            self.assertFalse(os.path.exists(self.model))   # (red's model taken away)

    def test_any_colour_wears_greens_files_when_the_module_paints_it(self):
        # Longshot's module paints the exact colour on green's files (coop_look/green.bin): green's are
        # put in whatever the colour, so the game's memory holds what it paints from
        with open(os.path.join(self.pack, "coop_self", "green.Angin_Tile_S_Alb.bntx.zs"), "w") as f:
            f.write("GREENS")
        m = json.loads(self.text(os.path.join(self.pack, "coop_puppet.json")))
        m["self_files"]["green"] = {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/green.Angin_Tile_S_Alb.bntx.zs"}
        m["look_files"] = {"green": "coop_look/green.bin", "red": "coop_look/red.bin"}
        self.manifest(m)
        # no green.bin on disk yet: the nearest preset's files, as before
        self.assertEqual(self_tunic.put(self.pack, "red"), "red")
        os.makedirs(os.path.join(self.pack, "coop_look"))
        with open(os.path.join(self.pack, "coop_look", "green.bin"), "w") as f:
            f.write("LOOK")
        for colour in ("red", "#FF3333", "black", "", "green"):
            self.assertEqual(self_tunic.put(self.pack, "red", painted=False), "red")
            self.assertEqual(self_tunic.put(self.pack, colour), "green", colour)
            self.assertEqual(self.text(self.tile), "GREENS")
            self.assertFalse(os.path.exists(self.model))   # (red's model taken away)
        # without the module (painted=False) the nearest preset's
        self.assertEqual(self_tunic.put(self.pack, "black", painted=False), "black")
        self.assertEqual(self.text(self.tile), "BLACKS")

    def test_older_and_broken_packs(self):
        self.assertEqual(self_tunic.put(self.pack, "red"), "red")
        old = os.path.join(self.pack, "romfs", "Tex", "Angin_Tile_A_Alb.bntx.zs")
        with open(old, "w") as f:
            f.write("STALE")
        self.manifest('{"tunics": {"red": ["x"]}, "self_files": {}}')   # no green puppet: none
        self.assertEqual(self_tunic.put(self.pack, "red"), "")
        self.assertFalse(os.path.exists(old) or os.path.exists(self.model))
        self.manifest("{broken")
        self.assertEqual(self_tunic.put(self.pack, "red"), "")
        self.assertEqual(self_tunic.put(os.path.join(self.dir, "no-pack"), "red"), "")

    def test_colours_are_the_modules(self):
        # NAMED and the presets are lichii/hook/coop_colors.cpp's kNamed and kTunics: no drift
        import re
        src = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))))), "lichii", "hook", "coop_colors.cpp")
        with open(src, encoding="utf-8") as f:
            text = f.read()
        named = text[text.index("kNamed[]"):text.index("};", text.index("kNamed[]"))]
        self.assertEqual({n: int(v, 16) for n, v in re.findall(r'\{"(\w+)", 0x([0-9A-Fa-f]{6})\}', named)},
                         self_tunic.NAMED)
        tunics = text[text.index("kTunics[]"):text.index("};", text.index("kTunics[]"))]
        suffixes = re.findall(r'\{0x[0-9A-Fa-f]{6}, "(\w*)"\}', tunics)
        self.assertEqual(sorted(s.lower() or "green" for s in suffixes), sorted(self_tunic.TUNICS))

    def test_main(self):
        coop = os.path.join(self.dir, "coop.ini")
        with open(coop, "w") as f:
            f.write("color = red\n")
        self.assertEqual(self_tunic.main(["--pack", self.pack, "--color", "", "--coop-ini", coop]), 0)
        self.assertEqual(self.text(self.tile), "REDS")


if __name__ == "__main__":
    unittest.main()
