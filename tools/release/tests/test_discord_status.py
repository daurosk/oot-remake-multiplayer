# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/release/kit/scripts/discord_status.py: the kit's lines in longshot_options.ini, the player's kept."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kit", "scripts"))
import discord_status as ds  # noqa: E402

ID = ds.APP_ID


class Merge(unittest.TestCase):
    def test_new_file(self):
        self.assertEqual(ds.merge("", "0.11.4").splitlines(),
                         [f"discord_app_id={ID}", "discord_image=none", "discord_details=Squadala! 0.11.4",
                          "discord_state=Solo"])

    def test_no_release(self):
        self.assertIn("discord_details=Squadala!\n", ds.merge("x=1\n", ""))

    def test_other_lines_kept_in_place(self):
        old = "output_res=1920x1080\npad_a=1\nkey_b=2\nmemory_budget=0\n"
        new = ds.merge(old, "0.11.4")
        self.assertTrue(new.startswith(old))
        self.assertEqual(new.count("\n"), 8)

    def test_new_file_is_lf_everywhere(self):
        self.assertNotIn("\r", ds.merge("", "0.11.4"))

    def test_crlf_kept(self):
        new = ds.merge("output_res=1920x1080\r\nmemory_budget=0", "0.11.4")
        self.assertNotIn("\n", new.replace("\r\n", ""))
        self.assertIn("memory_budget=0\r\n", new)

    def test_update_replaces_the_kits_own_release(self):
        old = f"discord_app_id={ID}\ndiscord_image=none\ndiscord_details=Squadala! 0.11.3\ndiscord_state=Solo\nx=1\n"
        self.assertEqual(ds.merge(old, "0.11.4"), old.replace("0.11.3", "0.11.4"))

    def test_players_own_lines_kept(self):
        old = ("discord_app_id=1570559772111933578\ndiscord_image=longshot\ndiscord_details=My run\n"
               "discord_state=Any% speedrun\n")
        self.assertEqual(ds.merge(old, "0.11.4"), old)

    def test_idempotent(self):
        once = ds.merge("a=1\n", "0.11.4")
        self.assertEqual(ds.merge(once, "0.11.4"), once)

    def test_values_fit_the_patch(self):
        # host/discord_rpc.c (patch 0026): an id of digits; words 2 to 127 bytes, no control character
        self.assertTrue(ID.isdigit() and len(ID) <= 20)
        for key, value, _ in ds.wanted("0.11.4-rc.1+abc"):
            if key not in ("discord_app_id", "discord_image"):
                self.assertTrue(2 <= len(value.encode()) <= 127, value)
                self.assertTrue(all(ord(c) >= 0x20 for c in value), value)


class Files(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.ini = os.path.join(self.dir.name, "longshot_options.ini")
        self.ver = os.path.join(self.dir.name, "VERSION.txt")
        with open(self.ver, "w", encoding="utf-8") as f:
            f.write("kit: Squadala-v0.11.4 (release v0.11.4)\ncommit=abc\n")

    def tearDown(self):
        self.dir.cleanup()

    def read(self):
        with open(self.ini, encoding="utf-8", newline="") as f:
            return f.read()

    def test_release_of(self):
        self.assertEqual(ds.release_of(self.ver), "0.11.4")
        self.assertEqual(ds.release_of(os.path.join(self.dir.name, "none.txt")), "")
        self.assertEqual(ds.release_of(None), "")

    def test_main_creates_then_updates(self):
        self.assertEqual(ds.main(["--options", self.ini, "--version-file", self.ver]), 0)
        self.assertIn("discord_details=Squadala! 0.11.4", self.read())
        with open(self.ini, "a", encoding="utf-8", newline="") as f:
            f.write("output_res=1280x720\n")
        self.assertEqual(ds.main(["--options", self.ini]), 0)
        text = self.read()
        self.assertIn("discord_details=Squadala!\n", text)
        self.assertIn("output_res=1280x720\n", text)
        self.assertEqual([n for n in os.listdir(self.dir.name) if n.startswith(".discord-")], [])

    def test_main_never_fails_the_start(self):
        missing = os.path.join(self.dir.name, "no-such-folder", "longshot_options.ini")
        self.assertEqual(ds.main(["--options", missing]), 0)


if __name__ == "__main__":
    unittest.main()
