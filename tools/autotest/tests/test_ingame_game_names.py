"""The ONE item name table of the in-game tests (tools/autotest/ingame/game_names.py) against the module (gate-hilt,
2026-11-27: test_sword_handle compared equips.weapon with 'KokiriSword', a name the game never has; the fake said the
same, so only the real lane found it). Each name must be an item of coop/host/src/game_items.cpp (its IsGet.<name> or
PouchData.<name>.StockNum flag), the fake's equips must come from the table, and no in-game test copies one of the
table's names or uses a short form.
"""
import glob
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
AUTOTEST = os.path.dirname(HERE)
INGAME = os.path.join(AUTOTEST, "ingame")
REPO = os.path.dirname(os.path.dirname(AUTOTEST))
sys.path.insert(0, INGAME)
import game_names   # noqa: E402

SHORT_FORMS = ("KokiriSword", "DekuShield", "SkullMask", "HoverBoots", "GoronTunic")   # (never a game value)


class GameNames(unittest.TestCase):
    def test_every_name_is_an_item_of_the_module(self):
        with open(os.path.join(REPO, "coop", "host", "src", "game_items.cpp"), encoding="utf-8") as f:
            items = f.read()
        for name in game_names.ALL:
            self.assertTrue('"IsGet.%s"' % name in items or '"PouchData.%s.StockNum"' % name in items,
                            "%s is not an item of coop/host/src/game_items.cpp" % name)

    def test_no_ingame_file_copies_a_name_or_uses_a_short_form(self):
        own = os.path.join(INGAME, "game_names.py")
        for path in glob.glob(os.path.join(INGAME, "**", "*.py"), recursive=True):
            if os.path.abspath(path) == os.path.abspath(own):
                continue
            with open(path, encoding="utf-8") as f:
                text = f.read()
            for short in SHORT_FORMS:
                self.assertNotRegex(text, r"['\"]%s['\"]" % short, "%s: the short form %r is no game value" % (path, short))
            for name in game_names.ALL:
                for m in re.finditer(r"^(?!\s*#).*=\s*['\"]%s['\"]" % re.escape(name), text, re.M):
                    self.fail("%s: %r assigned as a literal (import game_names): %s" % (path, name, m.group(0).strip()))

    def test_the_fake_states_the_game_own_weapon_name(self):
        import fakegame
        self.assertIn("game_names.KOKIRI_SWORD", open(fakegame.__file__, encoding="utf-8").read())


if __name__ == "__main__":
    unittest.main()
