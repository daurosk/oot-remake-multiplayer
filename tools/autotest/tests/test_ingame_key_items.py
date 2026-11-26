"""Row 223's selftest (ingame/test_key_items.py + ingame/fakes/key_items.py; owner port-sweep-c131-10), run by
test_ingame.py's load_tests on its own boot (live-key-items-223): NO-REF on the fake (look shots, no references),
SETUP-FAIL on a module without the state's `minus` member, FAIL when B never gets A's shared flags, when page 2's
count differs from the game's flags, or when ZR does not turn the page."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

NAME = "test_key_items_pages"
BOOT = "live-key-items-223"


class KeyItems(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_KEYITEMS_BUG"] = bug
        p = T.pytest_run([".", "--fake", "--out", out, "--boot", BOOT] + (["--no-rerun"] if bug else []) +
                         ["-q", "-p", "no:cacheprovider", "-k", "test_key_items"], env=env)
        return p, T.summary(out)

    def result(self, s):
        return [(t["result"], t["why"]) for t in s["tests"] if t["test"].endswith(NAME)]

    def one(self, name, bug=None):
        p, s = self.run_fake(name, bug)
        r = self.result(s)
        self.assertEqual(len(r), 1, (p.stdout, s))
        return r[0]

    def test_fake_no_ref(self):
        r = self.one("ok")
        self.assertEqual(r[0], "NO-REF", r)
        self.assertIn("no stored reference", r[1])

    def test_no_minus_member_is_setup_fail(self):
        r = self.one("nominus", "nominus")
        self.assertEqual(r[0], "SETUP-FAIL", r)
        self.assertIn("no 'minus' member", r[1])

    def test_no_shared_flags_fails(self):
        r = self.one("nosync", "nosync")
        self.assertEqual(r[0], "FAIL", r)
        self.assertIn("B holds every counted flag", r[1])

    def test_bad_count_fails(self):
        r = self.one("badcount", "badcount")
        self.assertEqual(r[0], "FAIL", r)
        self.assertIn("page2-full: key items", r[1])

    def test_page_not_turned_fails(self):
        r = self.one("nopage", "nopage")
        self.assertEqual(r[0], "FAIL", r)
        self.assertIn("details page 0, want 1", r[1])


class SharedPad(unittest.TestCase):
    """B #138 (15:41): fakes/key_items.py took MINUS on every boot and fakegame stopped at the first True press hook,
    so map_arrow's fake never saw its MINUS. Every press hook now gets every press, and key_items reacts only on
    its own boot."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ingame"))
        import fakegame
        self.fakegame = fakegame

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def game(self, mods, boot=""):
        g = self.fakegame.FakeGame("A", os.path.join(self.tmp, "A"), 1, (), mods, boot=boot)
        g.other = g   # (one game is enough here: its state reads the other's place)
        self.addCleanup(g.close)
        return g

    def test_two_fakes_sharing_a_button_both_get_it(self):
        got = []

        class First:
            @staticmethod
            def press(game, buttons):
                got.append(("first", tuple(buttons)))
                return True

        class Second:
            @staticmethod
            def press(game, buttons):
                got.append(("second", tuple(buttons)))
                return True

        self.game([First, Second]).press(["MINUS"])
        self.assertEqual(got, [("first", ("MINUS",)), ("second", ("MINUS",))])

    def test_key_items_fake_only_on_its_own_boot(self):
        mod = [m for m in self.fakegame.load_fakes() if m.__name__ == "fakes_key_items"][0]
        other = self.game([mod], boot="live-kakariko-child")
        self.assertFalse(mod.press(other, ["MINUS"]))
        self.assertNotIn("minus", other.state().get("menu") or {})
        own = self.game([mod], boot=BOOT)
        self.assertTrue(mod.press(own, ["MINUS"]))


if __name__ == "__main__":
    unittest.main()
