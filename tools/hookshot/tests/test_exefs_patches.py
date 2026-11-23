"""Tests for hookshot/patches 0035: the functions it adds to Longshot's nx2/exefs_patches.py (functions_written,
written_words, the co-op Mods tab's list). Since Longshot 3.1 the file is Longshot's own (our 0034, which added it, is
dropped), so the test takes 0035's added block for that file out of the patch and runs it alone: no Longshot source,
no game, made-up data only.

    python tools/hookshot/tests/test_exefs_patches.py
"""
import json
import os
import re
import shutil
import tempfile
import types
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
PATCHES = os.path.join(REPO, "hookshot", "patches")
FILE = "nx2/exefs_patches.py"
MAIN = 0x80020000   # main's link address, as snaildev99's crash lines have it


def load():
    """0035's added block for FILE (the hunk with coop_list), run as a module with what it uses of the file (os, json,
    ENV)"""
    name = next(n for n in sorted(os.listdir(PATCHES)) if n.startswith("0035-"))
    with open(os.path.join(PATCHES, name), encoding="utf-8") as f:
        text = f.read()
    part = text[text.index("diff --git a/%s b/%s" % (FILE, FILE)):]
    end = part.find("\ndiff --git", 1)
    part = part if end < 0 else part[:end]
    for hunk in re.split(r"\n(?=@@ )", part)[1:]:
        added = [line[1:] for line in hunk.split("\n") if line.startswith("+")]
        if any(line.startswith("def coop_list(") for line in added):
            mod = types.ModuleType("exefs_patches_0035")
            mod.__dict__.update(os=os, json=json, ENV="OOT_EXEFS_PATCHES")
            exec(compile("\n".join(added) + "\n", name, "exec"), mod.__dict__)
            return mod, tempfile.mkdtemp(prefix="exefs_patches_")
    raise AssertionError("%s: no hunk adding coop_list to %s" % (name, FILE))


class ExefsPatchesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m, cls.work = load()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def test_the_functions_a_mod_patch_writes_into_keep_the_context(self):
        # The case of the crash: the mod's hook branches from the game's f_80c35c30 into the cave (lifted as that
        # function's code), and its helper f_83c662f0 is a function of its own in the cave; a game function the patch
        # does not touch keeps its locals.
        hook_site, helper, untouched = 0x80c35c30, 0x83c662f0, 0x81000000
        funcs = {
            hook_site: ({hook_site: 0, 0x80c35c64: 0, 0x83c66360: 0, 0x83c6637c: 0}, set()),
            helper: ({helper + 4 * i: 0 for i in range(27)}, set()),
            untouched: ({untouched + 4 * i: 0 for i in range(10)}, set()),
        }
        words = {0x80c35c64 - MAIN, 0x83c66360 - MAIN, helper + 8 - MAIN}
        self.assertEqual(self.m.functions_written(funcs, MAIN, words), {hook_site, helper})
        self.assertEqual(self.m.functions_written(funcs, MAIN, set()), set())       # no patches: as without them
        self.assertEqual(self.m.functions_written(funcs, MAIN, {0x2000000}), set())  # data only: no function

    def test_the_written_words_are_the_code_and_data_files_bytes_by_word(self):
        path = os.path.join(self.work, "patches.json")
        files = [{"mod": "A", "file": "x.ips", "kind": "code", "writes": [[0x100, 1], [0x101, 2], [0x107, 3]]},
                 {"mod": "B", "file": "y.ips", "kind": "data", "writes": [[0x2001, 9]]},
                 {"mod": "C", "file": "z.ips", "kind": "off", "writes": [[0x3000, 1]]},
                 {"mod": "D", "file": "w.ips", "kind": "other", "writes": [[0x4000, 1]]}]
        with open(path, "w", encoding="utf-8") as f:
            json.dump(files, f)
        self.assertEqual(self.m.written_words(path), {0x100, 0x104, 0x2000})
        old = os.environ.pop(self.m.ENV, None)
        try:
            self.assertEqual(self.m.written_words(), set())   # no patches' list: none
        finally:
            if old is not None:
                os.environ[self.m.ENV] = old

    def test_the_co_op_mods_tab_list_sets_the_switches_and_the_order(self):
        root = os.path.join(self.work, "mods")
        os.makedirs(root, exist_ok=True)
        self.assertIsNone(self.m.coop_list(root))   # no list: mods.json's rules, as Longshot
        with open(os.path.join(root, self.m.COOP_LIST), "w", encoding="utf-8") as f:
            f.write("coop-mods 1\non Second\noff Off Mod\non first\non FIRST\n")
        rows = self.m.coop_list(root)
        self.assertEqual(rows, [("Second", True), ("Off Mod", False), ("first", True)])   # (a name twice: its first)
        # mods.json's order and switches (Longshot's ordered): alpha is off there
        mods = [("zeta", "/z", True), ("first", "/f", True), ("Off Mod", "/o", True), ("alpha", "/a", False),
                ("Second", "/s", True)]
        got = self.m.coop_ordered(mods, rows)
        # host/mods.c coop_mods_rank's order: an off one first, then the mods the list does not name in mods.json's order
        # and with its switches, then the list from its last row up, so the first row is indexed last and its patch wins
        # a byte two patches write.
        self.assertEqual([(n, on) for n, _, on in got],
                         [("Off Mod", False), ("zeta", True), ("alpha", False), ("first", True), ("Second", True)])
        with open(os.path.join(root, self.m.COOP_LIST), "w", encoding="utf-8") as f:
            f.write("coop-mods 2\non Second\n")
        self.assertIsNone(self.m.coop_list(root))   # another version: not ours

    def test_a_default_off_list_builds_no_code_of_a_mod_it_does_not_name(self):
        # Original game (report 266): its list names every mod off and says "default off", so a code mod put into the
        # folder later is built out, as host/mods.c leaves its files out (hos_coop_mods_rank).
        root = os.path.join(self.work, "mods-default-off")
        os.makedirs(root, exist_ok=True)
        with open(os.path.join(root, self.m.COOP_LIST), "w", encoding="utf-8") as f:
            f.write("coop-mods 1\r\n# Original game\r\ndefault off\r\noff OoT Character Select\r\non kept\r\n")
        rows, default_off = self.m.coop_list_default(root)
        self.assertTrue(default_off)
        self.assertEqual(rows, [("OoT Character Select", False), ("kept", True)])
        self.assertEqual(self.m.coop_list(root), rows)
        mods = [("randomizer-SpeedyKokiri", "/r", True), ("OoT Character Select", "/c", True), ("kept", "/k", True)]
        got = self.m.coop_ordered(mods, rows, default_off)
        self.assertEqual([(n, on) for n, _, on in got],
                         [("randomizer-SpeedyKokiri", False), ("OoT Character Select", False), ("kept", True)])
        # Without the line the unnamed one keeps mods.json's switch and its place under the named ones, as before (the
        # off row first, then it, then the named "on" row).
        self.assertEqual([(n, on) for n, _, on in self.m.coop_ordered(mods, rows)],
                         [("OoT Character Select", False), ("randomizer-SpeedyKokiri", True), ("kept", True)])
        for near in ("default  off", "Default off", "default on"):
            with open(os.path.join(root, self.m.COOP_LIST), "w", encoding="utf-8") as f:
                f.write("coop-mods 1\n%s\n" % near)
            self.assertEqual(self.m.coop_list_default(root), ([], False))


if __name__ == "__main__":
    unittest.main()
