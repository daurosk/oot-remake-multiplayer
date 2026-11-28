"""Game-free tests of Hookshot patch 0023, co-op's null guards (nx2/coop_null_guards.py, a64_lift.py's Lifter.unless,
nx2/fastfp.py's copy of the emit, nx2/chunks.py's tables), on Longshot's source with our patches applied. Ours, not in
the patch: Longshot's Linux AppImage ships no tests/, so a patch hunk there stops the Linux kit (module_test.cpp, "every
patch touches only what Longshot's Linux AppImage ships"). tools/hookshot/null_guards_test.py runs it.

- The install, with a fake lifter and image: every load goes in for the build it was made for; a changed word, a Player
  load whose call before it is not `bl main+0x82acc0`, a load that is there already, or another build leave it out.
- The words: each is a load through x0 (Player) or the slot-10 chain (x8 to x8), as the docstring says.
- A real a64_lift.Lifter over a made-up function (no translator, so no compiler): the guarded instruction is wrapped in
  `if (<null base>) { <C instead> } else { ... }` and nothing else in the text changes.
- The lift's own chain (nx2/lift.py's module level: fastfp's copy of emit_function, guest locals, entry hooks, poll;
  A64_FASTFP on, its default), in a process of its own: the guard is in the written text and counted as emitted, a
  guarded load joins no fast float run, and nx2/chunks.py sends Lifter.unless to the emit's new processes (Windows).
- The C, compiled into a small program (with $CC, else clang or gcc on PATH; skipped when there is none) around a
  stand-in for the load: a null base gives 0 and a log line, a real pointer reads memory as shipped.

COOP_LONGSHOT_SOURCE=<patched source> python -I -B tools/hookshot/null_guards_cases.py
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

if not os.environ.get("COOP_LONGSHOT_SOURCE"):
    sys.exit("null_guards_cases.py: set COOP_LONGSHOT_SOURCE to Longshot's source with our patches applied "
             "(or run tools/hookshot/null_guards_test.py <source>)")
ROOT = Path(os.environ["COOP_LONGSHOT_SOURCE"])
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path)
import a64_lift  # noqa: E402
import coop_null_guards as g  # noqa: E402


def bl(pc, target):
    return 0x94000000 | (((target - pc) >> 2) & 0x3FFFFFF)


def game_words():
    """The listed words, and `bl main+0x82acc0` before each Player load (main offsets)"""
    words = {off: w for off, w in g.PLAYER_LOADS + g.SLOT10_LOADS}
    for off, _ in g.PLAYER_LOADS:
        words[off - 4] = bl(off - 4, g.PLAYER_OF)
    for off, w, call, between in g.PLAYER_LOADS_LATER:
        words[off] = w
        words[call] = bl(call, g.PLAYER_OF)
        words.update(between)
    return words


class Image:
    def __init__(self, words):
        self.words = words

    def word(self, pc):
        return self.words[pc - g.MAIN]


class FakeLifter:
    def __init__(self, unless=None):
        self.unless = dict(unless or {})


LATER = tuple((off, w) for off, w, _, _ in g.PLAYER_LOADS_LATER)
ALL = g.PLAYER_LOADS + LATER + g.SLOT10_LOADS


class InstallTests(unittest.TestCase):
    def test_all_go_in(self):
        lifter = FakeLifter()
        self.assertEqual(g.install(lifter, Image(game_words()), g.BUILD_ID), len(ALL))
        self.assertEqual(sorted(lifter.unless), sorted(g.MAIN + off for off, _ in ALL))
        cond, instead = lifter.unless[g.MAIN + 0x2dceef8]
        self.assertEqual(cond, "c->x[0] < 0x100000ULL")
        self.assertTrue(instead.startswith("c->x[8] = 0; "))
        self.assertIn("main+0x2dceef8", instead)
        cond, instead = lifter.unless[g.MAIN + 0x26cf2a4]
        self.assertTrue(instead.startswith("c->x[20] = 0; "))
        cond, instead = lifter.unless[g.MAIN + 0x4b85e8]
        self.assertEqual(cond, "c->x[8] < 0x100000ULL")

    def test_another_build_is_left_out(self):
        lifter = FakeLifter()
        self.assertEqual(g.install(lifter, Image(game_words()), "0" * 40), 0)
        self.assertEqual(lifter.unless, {})

    def test_a_changed_word_leaves_that_one_out(self):
        for off, _ in ALL:
            words = game_words()
            words[off] ^= 1 << 10   # (another offset: the same kind of load, not the same code)
            lifter = FakeLifter()
            self.assertEqual(g.install(lifter, Image(words), g.BUILD_ID), len(ALL) - 1, hex(off))
            self.assertNotIn(g.MAIN + off, lifter.unless)

    def test_a_player_load_needs_its_call(self):
        for off, _ in g.PLAYER_LOADS:
            words = game_words()
            words[off - 4] = bl(off - 4, g.PLAYER_OF + 4)
            lifter = FakeLifter()
            self.assertEqual(g.install(lifter, Image(words), g.BUILD_ID), len(ALL) - 1, hex(off))
            self.assertNotIn(g.MAIN + off, lifter.unless)

    def test_a_later_load_needs_its_call_and_the_words_between(self):
        off, _, call, between = g.PLAYER_LOADS_LATER[0]
        self.assertEqual((off, call), (0x27975f0, 0x27975d0))
        cases = [(call, bl(call, g.PLAYER_OF + 4))] + [(o, w ^ 1) for o, w in between]
        for at, word in cases:
            words = game_words()
            words[at] = word
            lifter = FakeLifter()
            self.assertEqual(g.install(lifter, Image(words), g.BUILD_ID), len(ALL) - 1, hex(at))
            self.assertNotIn(g.MAIN + off, lifter.unless)
        lifter = FakeLifter()
        g.install(lifter, Image(game_words()), g.BUILD_ID)
        cond, instead = lifter.unless[g.MAIN + off]
        self.assertEqual(cond, "c->x[0] < 0x100000ULL")
        self.assertTrue(instead.startswith("c->x[8] = 0; "))

    def test_a_word_that_cannot_be_read_is_left_out(self):
        words = game_words()
        del words[0x2dceef8]
        lifter = FakeLifter()
        self.assertEqual(g.install(lifter, Image(words), g.BUILD_ID), len(ALL) - 1)

    def test_keeps_a_guard_that_is_there(self):
        pc = g.MAIN + 0x2dceef8
        lifter = FakeLifter({pc: ("0", "/* another */")})
        self.assertEqual(g.install(lifter, Image(game_words()), g.BUILD_ID), len(ALL) - 1)
        self.assertEqual(lifter.unless[pc], ("0", "/* another */"))


class WordTests(unittest.TestCase):
    def test_player_loads_read_through_x0(self):
        offs = [off for off, _ in g.PLAYER_LOADS + LATER]
        self.assertEqual(len(offs), len(set(offs)))
        self.assertEqual(len(ALL), 29)
        for off, w in g.PLAYER_LOADS + LATER:
            rt, rn, imm = g.decode(w)
            self.assertEqual(rn, 0, hex(off))
            self.assertNotEqual(rt, 0, hex(off))   # (the guard's condition reads the base after nothing wrote it)
        self.assertEqual(g.decode(0xb947d808), (8, 0, 0x7d8))      # ldr w8, [x0, #0x7d8]
        self.assertEqual(g.decode(0xb94e5814), (20, 0, 0xe58))     # ldr w20, [x0, #0xe58]
        self.assertEqual(g.decode(0x39797c08), (8, 0, 0xe5f))      # ldrb w8, [x0, #0xe5f]
        self.assertEqual(g.decode(0x395f0408), (8, 0, 0x7c1))      # ldrb w8, [x0, #0x7c1]

    def test_the_words_between_leave_x0_alone(self):
        for off, _, call, between in g.PLAYER_LOADS_LATER:
            # (every instruction from the call to the branch is listed)
            self.assertEqual([o for o, _ in between], list(range(call + 4, call + 4 + 4 * len(between), 4)))
            for o, w in between:
                self.assertNotEqual(w & 31, 0, hex(o))   # (no instruction between writes x0)
            tbz = between[-1][1]
            self.assertEqual(tbz & 0x7f000000, 0x36000000)
            self.assertEqual(between[-1][0] + ((tbz >> 5) & 0x3fff) * 4, off)

    def test_slot10_chain(self):
        self.assertEqual([g.decode(w) for _, w in g.SLOT10_LOADS], [(8, 8, 0x50), (8, 8, 0x20), (8, 8, 0x1c8)])
        self.assertEqual([off for off, _ in g.SLOT10_LOADS], [0x4b85e4, 0x4b85e8, 0x4b85ec])

    def test_only_plain_loads(self):
        self.assertIsNone(g.decode(0xaa0003f7))   # mov x23, x0
        self.assertIsNone(g.decode(0xf8408c08))   # ldr x8, [x0, #8]! (writes its base back)
        self.assertTrue(g.calls(bl(0x100, 0x82acc0), 0x100, 0x82acc0))
        self.assertFalse(g.calls(0xaa0003f7, 0x100, 0x82acc0))
        self.assertFalse(g.calls(None, 0x100, 0x82acc0))


BASE = 0x80000000
FN = BASE + 0x1000
RET = 0xd65f03c0
ADD = 0x91000400            # add x0, x0, #1
LOAD = 0xb947d808           # ldr w8, [x0, #0x7d8]
FN_WORDS = {FN: ADD, FN + 4: LOAD, FN + 8: ADD, FN + 12: RET}


class FnImage:
    entries = {FN}
    hooks = {}
    names = {}

    def word(self, pc):
        return FN_WORDS[pc]

    def in_code(self, pc):
        return pc in FN_WORDS

    def read(self, pc, n):
        return b"".join(struct.pack("<I", FN_WORDS[pc + 4 * i]) for i in range(n // 4))


def fn_texts():
    return {ADD: ("    c->x[0] += 1;", 0), LOAD: ("    c->x[8] = LOAD32(c->x[0] + 0x7d8);", 0)}


class LifterTests(unittest.TestCase):
    def emit(self, unless):
        lifter = a64_lift.Lifter(FnImage())
        lifter.discover()
        lifter.unless.update(unless)
        return lifter.emit_function(FN, fn_texts())[0]

    def test_a_lifter_made_without_init_has_no_guards(self):
        # (Longshot's tests/test_fastfp_override.py and a new emit process's view make it so: the class default)
        bare = object.__new__(a64_lift.Lifter)
        self.assertEqual(len(bare.unless), 0)
        with self.assertRaises(TypeError):
            bare.unless[FN] = ("1", "")
        self.assertIsNot(a64_lift.Lifter(FnImage()).unless, bare.unless)

    def test_unguarded_is_unchanged(self):
        self.assertEqual(self.emit({}), self.emit({BASE + 0x9999: ("1", "")}))

    def test_wraps_only_the_load(self):
        plain = self.emit({}).splitlines()
        text = self.emit({FN + 4: ("c->x[0] < 0x100000ULL", "c->x[8] = 0;")}).splitlines()
        i = plain.index("    c->x[8] = LOAD32(c->x[0] + 0x7d8);")
        self.assertEqual(text[:i], plain[:i])
        self.assertEqual(text[i:i + 3], ["if (c->x[0] < 0x100000ULL) { c->x[8] = 0; } else {",
                                         "    c->x[8] = LOAD32(c->x[0] + 0x7d8);", "}"])
        self.assertEqual(text[i + 3:], plain[i + 1:])
        self.assertEqual(sum(1 for line in text if re.search(r"c->x\[0\] \+= 1;", line)), 2)


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t x[32]; } Ctx;
static uint8_t mem[0x200000];
static int said;
void hos_log(const char *fmt, ...) { if (strstr(fmt, "%s")) said++; }
static uint64_t LOAD(uint64_t a, int n) { uint64_t v = 0; memcpy(&v, mem + a, n); return v; }
static void run(Ctx *c) {
    if (%s) { %s } else { c->x[%d] = LOAD(c->x[%d] + %#x, %d); }
}
int main(void) {
    Ctx c;
    memset(&c, 0, sizeof c); c.x[%d] = 7; c.x[%d] = 0;   /* (the base last: it may be the same register) */
    run(&c);
    if (c.x[%d] != 0 || said != 1) return 1;
    memset(mem + 0x100000 + %#x, 0x5a, 8);
    memset(&c, 0, sizeof c); c.x[%d] = 7; c.x[%d] = 0x100000;
    run(&c);
    if (c.x[%d] == 0 || c.x[%d] == 7 || said != 1) return 2;
    puts("ok");
    return 0;
}
"""


def compiler():
    for cc in (os.environ.get("CC"), "clang", "gcc", "cc"):
        if cc and shutil.which(cc):
            return shutil.which(cc)
    return None


@unittest.skipUnless(compiler(), "no C compiler ($CC, clang or gcc)")
class EmittedCTests(unittest.TestCase):
    def test_reads_as_the_emulator_does(self):
        lifter = FakeLifter()
        g.install(lifter, Image(game_words()), g.BUILD_ID)
        sizes = {0xf9400000: 8, 0xb9400000: 4, 0x79400000: 2, 0x39400000: 1}
        for off, w in (ALL[0], (0x2dceef8, 0xb947d808), (0x26cf2a4, 0xb94e5814), LATER[0], g.SLOT10_LOADS[1]):
            rt, rn, imm = g.decode(w)
            cond, instead = lifter.unless[g.MAIN + off]
            marker = "main+%#x" % off
            src = HARNESS.replace('strstr(fmt, "%s")', 'strstr(fmt, "' + marker + '")') % (
                cond, instead, rt, rn, imm, sizes[w & 0xffc00000], rt, rn, rt, imm, rt, rn, rt, rt)
            with tempfile.TemporaryDirectory() as d:
                c = os.path.join(d, "t.c")
                exe = os.path.join(d, "t.exe")
                Path(c).write_text(src)
                built = subprocess.run([compiler(), "-std=c11", "-Wall", "-Werror", c, "-o", exe],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr + src)
                r = subprocess.run([exe], capture_output=True, text=True)
                self.assertEqual((r.returncode, r.stdout.strip()), (0, "ok"), hex(off))


MUL = 0x6e22dc23            # fmul v3.4s, v1.4s, v2.4s (a vector multiply: what a fast float run takes)
MUL_TEXT = "a64_vfpx_mul32(c,c->vreg[3],c->vreg[1],c->vreg[2],4);"


def pipeline():
    """(the lift's chain, in this process): FN's load guarded, and a run of 16 multiplies with a guard on the 9th; prints
    what the tests below read"""
    import lift  # noqa: F401  (its module level installs the chain the game's lift uses)
    import chunks
    import json
    lifter = a64_lift.Lifter(FnImage())
    lifter.discover()
    lifter.unless[FN + 4] = ("c->x[0] < 0x100000ULL", "c->x[8] = 0;")
    text = lifter.emit_function(FN, fn_texts())[0]
    run = object.__new__(a64_lift.Lifter)   # (a made-up function of 16 multiplies, as tests/test_fastfp_override.py)
    for k, v in vars(lifter).items():
        setattr(run, k, v)
    e = 0x2000
    body = {e + 4 * i: (("op",), MUL) for i in range(16)}
    body[e + 64] = (("ret",), RET)
    run.funcs = {e: (body, set())}
    run.unless = {e + 32: ("c->x[0] < 0x100000ULL", "/* guarded */")}
    run.stats = type(lifter.stats)()
    run_text = run.emit_function(e, {MUL: (MUL_TEXT, 0)})[0]
    print(json.dumps({"emit": "%s.%s" % (a64_lift.Lifter.emit_function.__module__,
                                         a64_lift.Lifter.emit_function.__qualname__),
                      "text": text, "emitted": lifter.stats.get("unless_emitted", 0),
                      "run_text": run_text, "run_stats": dict(run.stats),
                      "tables": list(chunks.EMIT_TABLES)}))


class PipelineTests(unittest.TestCase):
    """The guards through the lift's own chain, in a new process (nx2/lift.py's installs stay out of the other tests)"""

    @classmethod
    def setUpClass(cls):
        import json
        env = dict(os.environ, A64_FASTFP="1", COOP_LONGSHOT_SOURCE=str(ROOT))
        r = subprocess.run([sys.executable, "-I", "-B", os.path.abspath(__file__), "--pipeline"], env=env,
                           capture_output=True, text=True, timeout=600)
        assert r.returncode == 0, r.stdout + r.stderr
        cls.got = json.loads(r.stdout.strip().splitlines()[-1])

    def test_the_chain_is_not_the_kits_method(self):
        self.assertNotEqual(self.got["emit"], "a64_lift.Lifter.emit_function")

    def test_the_guard_is_written_and_counted(self):
        # (the context's registers, or guest locals where nx2/guest_locals.py gives the function them: x0_, x8_)
        guard = r"if \((c->x\[0\]|x0_) < 0x100000ULL\) \{ (c->x\[8\]|x8_) = 0; \} else \{\n.*LOAD32\(.*0x7d8\);\n\}"
        self.assertRegex(self.got["text"], guard)
        self.assertEqual(self.got["emitted"], 1)

    def test_a_guarded_load_joins_no_fast_float_run(self):
        stats = self.got["run_stats"]
        self.assertEqual(self.got["run_text"].count("/* guarded */"), 1, self.got["run_text"])
        self.assertEqual((stats.get("ff_regions"), stats.get("ff_ops")), (1, 8), self.got["run_text"])

    def test_new_emit_processes_get_the_guards(self):
        self.assertIn("unless", self.got["tables"])


if __name__ == "__main__":
    if "--pipeline" in sys.argv:
        pipeline()
    else:
        unittest.main()
