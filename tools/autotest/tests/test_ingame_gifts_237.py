"""Selftests of row 237's one-time gifts test (tools/autotest/ingame/test_gifts_237.py) on the fake games
(ingame/fakes/gifts_237.py), no game. Run by test_ingame.py's load_tests (CI) or alone."""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ingame"))
import test_ingame as T   # noqa: E402
import once_gifts         # noqa: E402

TESTS = ("test_once_gifts_listed", "test_once_gifts_shared_once")


class OnceGiftsTable(unittest.TestCase):
    """The reader of coop_shop_sale.h: the module's whole table, in order, or an error (never a short list)."""

    def test_reads_the_whole_table(self):
        g = once_gifts.read()
        self.assertEqual(len(g), 22)    # tests/lichii/shop_sale_test.cpp:117 CHECK(kOnceGifts.size() == 22)
        names = [n for _, n, _ in g]
        self.assertEqual(len(set(names)), len(names))
        # the two the old row judged (offline/u5-rando-run.ps1:194 Medigoron, :209 Storms) and the cow (hs-u5-cow)
        hashes = {n: h for h, n, _ in g}
        self.assertEqual(hashes["HaveTalked_Special.Npc_Goron_Medigoron_Adult_Talk_Event26"], 0x0e3dad8a)
        self.assertEqual(hashes["MiniGame_FrogTenors_StormSong"], 0xd578df95)
        self.assertEqual(hashes["HavePlayedEvent.Dm_StaffCredit_0027"], 0x4f4c1315)

    def test_a_short_table_is_an_error(self):
        tmp = tempfile.mkdtemp()
        try:
            p = os.path.join(tmp, "h.h")
            with open(p, "w", encoding="utf-8") as f:
                f.write('inline constexpr std::array<GiftFlag, 3> kOnceGifts{{\n'
                        '    {0x0e3dad8a, "A", "a"},\n    {0xd578df95, "B", "b"},\n}};\n')
            with self.assertRaises(ValueError):
                once_gifts.read(p)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class Row237OnceGifts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_gifts(self, tag, mode=None, more=()):
        out = os.path.join(self.tmp, tag)
        env = {"BT_FAKE_GIFTS": mode} if mode else None
        p = T.pytest_run(["test_gifts_237.py", "--fake", "--no-rerun", "--out", out, "-q"] + list(more), env=env)
        return p, T.summary(out)

    def test_pass_and_again_on_the_same_boot(self):
        """PASS, and again twice on one boot (--repeat 3): the way back leaves every gift Bool at 0."""
        p, s = self.run_gifts("ok", more=["--repeat", "3"])
        self.assertEqual(s["counts"].get("PASS"), 2 * 3, p.stdout[-2000:])
        self.assertIn("RESULT: PASS", p.stdout)

    def test_fail_when_each_game_keeps_its_own(self):
        """The bug as reported: no gift Bool is shared, each player is paid again."""
        p, s = self.run_gifts("off", mode="off")
        self.assertEqual(T.results(s), {t: "FAIL" for t in TESTS}, p.stdout[-2000:])
        self.assertIn("still 0:", p.stdout)

    def test_fail_when_a_zero_goes_out(self):
        p, s = self.run_gifts("back", mode="back")
        r = T.results(s)
        self.assertEqual(r["test_once_gifts_shared_once"], "FAIL", p.stdout[-2000:])
        self.assertEqual(r["test_once_gifts_listed"], "PASS", p.stdout[-2000:])
        self.assertIn("a 1 must never go back to 0", p.stdout)

    def test_fail_when_part_of_the_table_is_shared(self):
        p, s = self.run_gifts("some", mode="some")
        self.assertEqual(T.results(s), {t: "FAIL" for t in TESTS}, p.stdout[-2000:])


if __name__ == "__main__":
    unittest.main()
