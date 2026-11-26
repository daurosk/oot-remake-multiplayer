"""Selftests of rows 288/207's bridge test (tools/autotest/ingame/test_charpage.py) on the fake games
(ingame/fakes/charpage.py), and of hookshot.py seed_layout. Run by test_ingame.py's load_tests (CI) or alone."""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import test_ingame as T   # noqa: E402
import hookshot           # noqa: E402

SEED = ["--env", "A:OOT_TEST_SEED_LAYOUT=1", "--env", "A:OOT_TEST_NO_MERGE=1"]
MERGE = SEED + ["--env", "A:OOT_COOP_START_MERGE=1", "--env", "A:OOT_COOP_RESTART=1"]


class CharactersPageHeld(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_k(self, k, args, env=None):
        out = os.path.join(self.tmp, "o")
        T.pytest_run([".", "--fake", "--out", out, "-q", "-k", k, "--no-rerun"] + args, env=env)
        s = T.summary(out)
        return s, {r["test"].split("::")[-1]: r for r in s["tests"]}

    def test_held_has_no_fail_and_wants_the_shots(self):
        s, r = self.run_k("characters_page_held", SEED)
        self.assertEqual(r["test_characters_page_held"]["result"], "NO-REF", s)   # (shots kept for a person)

    def test_fails_when_the_page_opens_unheld(self):
        s, r = self.run_k("characters_page_held", SEED, env={"BT_FAKE_CHARPAGE": "nohold"})
        self.assertEqual(r["test_characters_page_held"]["result"], "FAIL", s)
        self.assertIn("aborted", r["test_characters_page_held"]["why"])   # (the crash: the bridge is gone)

    def test_setup_fail_without_the_seed(self):
        s, r = self.run_k("characters_page_held", SEED, env={"BT_FAKE_CHARPAGE": "noseed"})
        self.assertEqual(r["test_characters_page_held"]["result"], "SETUP-FAIL", s)

    def test_deselected_without_the_switches(self):
        s, r = self.run_k("characters_page", [])
        self.assertNotIn("test_characters_page_held", r)
        self.assertTrue(any("characters_page_held" in d["test"] for d in s["deselected"]), s["deselected"])

    def test_merge_setup_fail_without_a_kit_above(self):
        s, r = self.run_k("characters_page_merged", MERGE)
        res = r["test_characters_page_merged_at_restart"]
        self.assertEqual(res["result"], "SETUP-FAIL", s)
        self.assertIn("merge_tables.py", res["why"])


class SeedLayout(unittest.TestCase):
    def setUp(self):
        self.g = tempfile.mkdtemp()
        lay = os.path.join(self.g, "romfs", "UI", "Layout2Archive")
        os.makedirs(lay)
        os.makedirs(os.path.join(self.g, "romfs", "Pack"))
        with open(os.path.join(lay, "Common.Product.100.Nin_Ounce_NVN2.blarc.zs"), "wb") as f:
            f.write(b"own")
        os.makedirs(os.path.join(self.g, "mods"))
        self.lst = os.path.join(self.g, "mods", "coop-mods.list")
        self.rows = "coop-mods 1\non oot-remake-randomizer\non OoT Character Select\n"
        with open(self.lst, "w", encoding="utf-8", newline="\n") as f:
            f.write(self.rows)

    def tearDown(self):
        shutil.rmtree(self.g, ignore_errors=True)

    def read(self):
        with open(self.lst, encoding="utf-8") as f:
            return f.read()

    def test_on_puts_the_seed_first_and_off_puts_all_back(self):
        said = hookshot.seed_layout(self.g, {"OOT_TEST_SEED_LAYOUT": "1"}, lambda s: None)
        self.assertIn("test seed layout in", said[0])
        self.assertEqual(self.read().splitlines()[1], "on " + hookshot.SEED_MOD)
        arc = os.path.join(self.g, "mods", hookshot.SEED_MOD, "romfs", "UI", "Layout2Archive",
                           "Common.Product.100.Nin_Ounce_NVN2.blarc.zs")
        with open(arc, "rb") as f:
            self.assertEqual(f.read(), b"own")
        hookshot.seed_layout(self.g, {"OOT_TEST_SEED_LAYOUT": "1"}, lambda s: None)   # (twice: one row)
        self.assertEqual(self.read().count(hookshot.SEED_ROW), 1)
        hookshot.seed_layout(self.g, {}, lambda s: None)
        self.assertEqual(self.read(), self.rows)
        self.assertFalse(os.path.exists(os.path.join(self.g, "mods", hookshot.SEED_MOD)))

    def test_off_on_a_clean_game_changes_nothing(self):
        self.assertEqual(hookshot.seed_layout(self.g, {}, lambda s: None), [])
        self.assertEqual(self.read(), self.rows)

    def test_every_start_runs_it_kit_starts_too(self):
        """review-c123-11: a died seed run's mod must be gone at the next boot of ANY kind, a kit start as well: in
        run()'s launch, the call sits on the same level as `if kits[x]:`, before it (not in either branch)."""
        import inspect
        src = inspect.getsource(hookshot.run).splitlines()
        call = [i for i, ln in enumerate(src) if "seed_layout(g, env" in ln]
        kit = [i for i, ln in enumerate(src) if ln.strip() == "if kits[x]:" and "the kit's own start" in src[i + 1]]
        self.assertEqual((len(call), len(kit)), (1, 1), (call, kit))
        indent = lambda ln: len(ln) - len(ln.lstrip())   # noqa: E731
        self.assertEqual(call[0] + 1, kit[0])
        self.assertEqual(indent(src[call[0]]), indent(src[kit[0]]))

    def test_wrong_list_head_says_not_made(self):
        with open(self.lst, "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 2\non OoT Character Select\n")
        said = hookshot.seed_layout(self.g, {"OOT_TEST_SEED_LAYOUT": "1"}, lambda s: None)
        self.assertIn("NOT made", said[0])

    def test_no_archive_says_not_made(self):
        shutil.rmtree(os.path.join(self.g, "romfs", "UI"))
        said = hookshot.seed_layout(self.g, {"OOT_TEST_SEED_LAYOUT": "1"}, lambda s: None)
        self.assertIn("NOT made", said[0])
        self.assertEqual(self.read(), self.rows)


if __name__ == "__main__":
    unittest.main()
