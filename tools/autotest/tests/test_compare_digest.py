"""Tests for tools/autotest/compare_digest.py (the sync digest compare). Standard library only.

    python tools/autotest/tests/test_compare_digest.py -v
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import compare_digest  # noqa: E402

HEAD = "[12:00:00] [hos Presentation Thread] "


def room(t, scene, self_, n=1):
    return HEAD + "coop: digest: t=%d scene=%s self=%d room objects=%d h=00000000" % (t, scene, self_, n)


def obj(t, scene, self_, life, owner=1, oid="p11", local=None):
    extra = " local.owner_life=%d" % local if local is not None else ""
    return HEAD + ("coop: digest: t=%d scene=%s self=%d obj=%s name=Enemy_Stalchild owner=%d epoch=0 life=%d"
                   " max=4 dead=%d%s h=0" % (t, scene, self_, oid, owner, life, 1 if life <= 0 else 0, extra))


class CompareDigest(unittest.TestCase):
    def run_tool(self, a_lines, b_lines, *args):
        paths = []
        with tempfile.TemporaryDirectory() as d:
            for i, lines in enumerate((a_lines, b_lines)):
                path = os.path.join(d, "game%d.log" % i)
                with open(path, "w", encoding="utf-8") as f:
                    f.write("\n".join(["noise line"] + lines) + "\n")
                paths.append(path)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = compare_digest.main(paths + list(args))
        return code, out.getvalue()

    def test_parse(self):
        f = compare_digest.parse_line(obj(2000, "S", 1, 3, local=4))
        self.assertEqual(f["t"], 2000)
        self.assertEqual(f["life"], "3")
        self.assertEqual(f["local.owner_life"], "4")
        self.assertEqual(compare_digest.parse_line(room(2000, "S", 1))["room"], "1")
        self.assertIsNone(compare_digest.parse_line("coop: objects: + Enemy_Stalchild"))

    def test_agree(self):
        a = [obj(0, "S", 1, 4), room(0, "S", 1)] + [room(t, "S", 1) for t in range(2000, 12000, 2000)]
        b = [obj(500, "S", 2, 4, local=4), room(500, "S", 2)] + [room(t + 500, "S", 2) for t in range(2000, 12000, 2000)]
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 0, out)
        self.assertIn("RESULT: AGREE", out)

    def test_short_difference_settles(self):
        # A hit: the owner's life drops at 4 s, the copy follows by 6 s (one tick): not a desync.
        a = [obj(0, "S", 1, 4), obj(4000, "S", 1, 3)] + [room(t, "S", 1) for t in range(0, 14000, 2000)]
        b = [obj(0, "S", 2, 4), obj(6000, "S", 2, 3)] + [room(t, "S", 2) for t in range(0, 14000, 2000)]
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 0, out)
        self.assertIn("1 settled", out)

    def test_lasting_difference_is_a_desync(self):
        # The copy took the owner's sample (local.owner_life=3) but its game stays at 4: named with both values.
        a = [obj(0, "S", 1, 4), obj(4000, "S", 1, 3)] + [room(t, "S", 1) for t in range(0, 14000, 2000)]
        b = [obj(0, "S", 2, 4), obj(6000, "S", 2, 4, local=3)] + [room(t, "S", 2) for t in range(0, 14000, 2000)]
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 1, out)
        self.assertIn("DESYNC S p11 Enemy_Stalchild: life from t=4000 to t=12000", out)
        self.assertIn("| A: life=3 | B: life=4 local.owner_life=3", out)

    def test_owner_disagreement_is_a_desync(self):
        a = [obj(0, "S", 1, 4, owner=1)] + [room(t, "S", 1) for t in range(0, 10000, 2000)]
        b = [obj(0, "S", 2, 4, owner=2)] + [room(t, "S", 2) for t in range(0, 10000, 2000)]
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 1, out)
        self.assertIn(": owner from t=0", out)

    def test_apart_and_one_side_are_not_compared(self):
        # B is in another scene: nothing compared.
        a = [obj(0, "S", 1, 4)] + [room(t, "S", 1) for t in range(0, 10000, 2000)]
        b = [obj(0, "T", 2, 1)] + [room(t, "T", 2) for t in range(0, 10000, 2000)]
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 2, out)
        # Same scene, an object only A holds (out of B's load range): no desync.
        b = [room(t, "S", 2) for t in range(0, 10000, 2000)]
        code, out = self.run_tool(a, b, "--all")
        self.assertEqual(code, 0, out)
        self.assertIn("one side only: S p11", out)

    def test_gone_and_a_new_visit(self):
        # A's object is gone at 4 s (killed and removed); B still holds it alive: a desync.
        a = [obj(0, "S", 1, 4), HEAD + "coop: digest: t=4000 scene=S self=1 obj=p11 gone"]
        a += [room(t, "S", 1) for t in range(0, 12000, 2000)]
        b = [obj(0, "S", 2, 4)] + [room(t, "S", 2) for t in range(0, 12000, 2000)]
        code, out = self.run_tool(a, b, "--all")
        self.assertEqual(code, 0, out)  # gone on one side only is "one side", not a field difference
        # A leaves S at 4 s and comes back at 20 s: its old visit's lines do not count in the new one.
        a = [obj(0, "S", 1, 4)] + [room(t, "S", 1) for t in (0, 2000)]
        a += [room(t, "T", 1) for t in range(4000, 20000, 2000)] + [room(t, "S", 1) for t in range(20000, 30000, 2000)]
        b = [obj(0, "S", 2, 2)] + [room(t, "S", 2) for t in range(0, 30000, 2000)]
        code, out = self.run_tool(a, b)
        self.assertEqual(code, 0, out)  # 0..2 s: shorter than --hold; from 20 s A holds nothing of its old visit
        self.assertIn("1 settled", out)

    def test_no_digest(self):
        code, out = self.run_tool(["x"], ["y"])
        self.assertEqual(code, 2)
        self.assertIn("NOTHING COMPARED", out)


if __name__ == "__main__":
    unittest.main()
