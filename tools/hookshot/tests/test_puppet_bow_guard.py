"""Tests for hookshot/patches 0038 (nx2/coop_puppet_bow_guard.py): the guard that sends a bow or slingshot worn by
another player's Link (a puppet: no Player component) through the bow update's own exit instead of reading guest 0x7d8.

The test takes the new file out of the patch and runs it alone (no Longshot source, no game): the install rules with a
made-up image and lifter, the checked words decoded (the branches and the read the docstring names), and, where a C
compiler is found, the guard's C in a small harness: no Player component (no table, or none in slot 107) -> the exit
label, a Player component -> on to the game's own read.

    python tools/hookshot/tests/test_puppet_bow_guard.py
"""
import os
import shutil
import subprocess
import tempfile
import types
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
PATCHES = os.path.join(REPO, "hookshot", "patches")
FILE = "nx2/coop_puppet_bow_guard.py"


def patch_text():
    name = next(n for n in sorted(os.listdir(PATCHES)) if n.startswith("0038-"))
    with open(os.path.join(PATCHES, name), encoding="utf-8") as f:
        return name, f.read()


def new_file(text, path):
    part = text[text.index("diff --git a/%s b/%s" % (path, path)):]
    end = part.find("\ndiff --git", 1)
    part = part if end < 0 else part[:end]
    body = part[part.index("\n@@ ") + 1:]
    lines = body.split("\n")[1:]
    return "\n".join(line[1:] for line in lines if line.startswith("+")) + "\n"


def load():
    name, text = patch_text()
    mod = types.ModuleType("coop_puppet_bow_guard")
    exec(compile(new_file(text, FILE), name, "exec"), mod.__dict__)
    return mod


class Img:
    def __init__(self, words):
        self.words = dict(words)

    def word(self, pc):
        return self.words[pc]


class Lifter:
    def __init__(self):
        self.after = {}


def image_of(g):
    return Img({g.MAIN + off: w for off, w in g.WORDS})


class InstallTest(unittest.TestCase):
    def setUp(self):
        self.g = load()

    def test_goes_in_after_the_wearer_move(self):
        lifter = Lifter()
        self.assertEqual(self.g.install(lifter, image_of(self.g), self.g.BUILD_ID), 1)
        self.assertEqual(list(lifter.after), [self.g.MAIN + 0x2dceef0])
        self.assertIn("goto L_82deef04;", lifter.after[self.g.MAIN + 0x2dceef0])

    def test_keeps_text_already_there(self):
        lifter = Lifter()
        lifter.after[self.g.MAIN + self.g.SITE] = "/* other */"
        self.g.install(lifter, image_of(self.g), self.g.BUILD_ID)
        self.assertTrue(lifter.after[self.g.MAIN + self.g.SITE].startswith("/* other */\n{ uint64_t coop_t"))

    def test_other_build_left_out(self):
        lifter = Lifter()
        self.assertEqual(self.g.install(lifter, image_of(self.g), "0" * 40), 0)
        self.assertEqual(lifter.after, {})

    def test_any_word_differing_leaves_it_out(self):
        for off, w in self.g.WORDS:
            img = image_of(self.g)
            img.words[self.g.MAIN + off] = w ^ 1
            lifter = Lifter()
            self.assertEqual(self.g.install(lifter, img, self.g.BUILD_ID), 0, hex(off))
            self.assertEqual(lifter.after, {})

    def test_unreadable_word_leaves_it_out(self):
        img = image_of(self.g)
        del img.words[self.g.MAIN + self.g.SITE]
        lifter = Lifter()
        self.assertEqual(self.g.install(lifter, img, self.g.BUILD_ID), 0)


def cbz_target(pc, w):
    imm = (w >> 5) & 0x7FFFF
    if imm & 0x40000:
        imm -= 0x80000
    return pc + imm * 4


def bl_target(pc, w):
    imm = w & 0x3FFFFFF
    if imm & 0x2000000:
        imm -= 0x4000000
    return pc + imm * 4


class WordsTest(unittest.TestCase):
    """The checked words say what the docstring says (decoded here, not trusted)."""

    def setUp(self):
        self.g = load()
        self.w = dict(self.g.WORDS)

    def test_wearer_null_check_goes_to_the_exit(self):
        w = self.w[0x2dceeec]
        self.assertEqual(w & 0xFF00001F, 0xB4000000)   # cbz x0
        self.assertEqual(cbz_target(0x2dceeec, w), self.g.SKIP)

    def test_site_is_mov_x23_x0(self):
        self.assertEqual(self.g.SITE, 0x2dceef0)
        self.assertEqual(self.w[self.g.SITE], 0xAA0003F7)

    def test_calls_and_the_read(self):
        self.assertEqual(self.w[0x2dceee8] >> 26, 0x25)
        self.assertEqual(bl_target(0x2dceee8, self.w[0x2dceee8]), 0x9375F0)   # the wearer from the handle
        self.assertEqual(self.w[0x2dceef4] >> 26, 0x25)
        self.assertEqual(bl_target(0x2dceef4, self.w[0x2dceef4]), 0x82ACC0)   # the Player component getter
        ldr = self.w[0x2dceef8]
        self.assertEqual(ldr & 0xFFC003FF, 0xB9400008)   # ldr w8, [x0, #imm]
        self.assertEqual(((ldr >> 10) & 0xFFF) * 4, 0x7D8)
        self.assertEqual(self.w[0x2dceefc], 0x3100051F)   # cmn w8, #1

    def test_exit_follows_the_check(self):
        b_ne = self.w[0x2dcef00]
        self.assertEqual(b_ne & 0xFF00001F, 0x54000001)   # b.ne
        self.assertIn(0x2dcef04, self.w)


class HooksTest(unittest.TestCase):
    def test_coop_hooks_installs_it_after_the_puppet_guard(self):
        _, text = patch_text()
        self.assertIn("     coop_puppet_guard.install(lifter, img, build_id)\n"
                      "+    import coop_puppet_bow_guard", text)
        self.assertIn("+    coop_puppet_bow_guard.install(lifter, img, build_id)\n", text)


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
typedef struct { uint64_t x[32]; } Ctx;
static uint8_t mem[0x4000];
#define A64_PTR(a) (mem + (uint32_t)(a))
static int logged;
void hos_log(const char *fmt, ...) { (void)fmt; logged++; }
static int run(Ctx *c) {
%s
    return 0;
L_82deef04:;
    return 1;
}
static void put(uint32_t a, uint64_t v) { *(uint64_t *)A64_PTR(a) = v; }
int main(void) {
    Ctx c = {{0}};
    c.x[0] = 0x100;                       /* the wearer */
    int no_table = run(&c);
    put(0x100 + 0x270, 0x800);            /* its component table */
    int no_player = run(&c);
    put(0x800 + 0x358, 0x1000);           /* its Player component */
    int player = run(&c);
    printf("%%d %%d %%d %%d\n", no_table, no_player, player, logged);
    return 0;
}
"""


@unittest.skipUnless(shutil.which("cc") or shutil.which("gcc") or shutil.which("clang"), "no C compiler")
class GuardCTest(unittest.TestCase):
    def test_no_player_component_takes_the_exit(self):
        g = load()
        lifter = Lifter()
        g.install(lifter, image_of(g), g.BUILD_ID)
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        d = tempfile.mkdtemp(prefix="bow_guard_")
        try:
            src = os.path.join(d, "t.c")
            exe = os.path.join(d, "t.exe" if os.name == "nt" else "t")
            with open(src, "w", encoding="utf-8") as f:
                f.write(HARNESS % lifter.after[g.MAIN + g.SITE])
            subprocess.run([cc, "-std=c11", "-Wall", "-Werror", "-o", exe, src], check=True)
            out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout.split()
        finally:
            shutil.rmtree(d, ignore_errors=True)
        self.assertEqual(out, ["1", "1", "0", "2"])


if __name__ == "__main__":
    unittest.main()
