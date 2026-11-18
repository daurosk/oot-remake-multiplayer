"""Tests for tools/autotest/proof.py (screenshot checks) and the UNPROVEN verdict in groups.py. Frames are drawn here
(flat colours and a block standing for a lift up or down); no game shot is used or needed."""
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import groups as g  # noqa: E402
import proof  # noqa: E402

W, H = 128, 72   # a small frame: regions are given in 1280x720 and scaled


def frame(block=None, bg=(40, 40, 40), fg=(220, 200, 120), noise=0):
    """RGB rows: a dark frame, block = (x, y, w, h) in frame pixels painted bright; noise shifts every pixel."""
    rows = []
    for y in range(H):
        r = bytearray()
        for x in range(W):
            inside = block and block[0] <= x < block[0] + block[2] and block[1] <= y < block[1] + block[3]
            c = fg if inside else bg
            r += bytes(min(255, v + noise) for v in c)
        rows.append(r)
    return rows


def save(path, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    proof.write_png(path, W, H, rows)
    return path


UP = (40, 20, 40, 30)      # the lift up: a tall block
DOWN = (40, 42, 40, 8)     # down: a thin one low in the region
REGION = "300,150,600,450"  # 1280x720 units: frame pixels 30..90 x 15..60


def filtered_png(path, rows, ch=3):
    """The same picture with every row filter type in turn (1 Sub, 2 Up, 3 Average, 4 Paeth, 0 None)."""
    out, prev = b"", bytes(W * ch)
    for y, row in enumerate(rows):
        ft = [1, 2, 3, 4, 0][y % 5]
        enc = bytearray()
        for i, v in enumerate(row):
            a = row[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            pred = {0: 0, 1: a, 2: b, 3: (a + b) // 2, 4: proof._paeth(a, b, c)}[ft]
            enc.append((v - pred) & 255)
        out += bytes([ft]) + bytes(enc)
        prev = bytes(row)

    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(out)) + chunk(b"IEND", b""))


class PngTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_every_filter_reads_back(self):
        rows = frame(UP, noise=0)
        rows = [bytes((v + x * 7 + y * 3) & 255 for x, v in enumerate(r)) for y, r in enumerate(rows)]
        p = os.path.join(self.d, "f.png")
        filtered_png(p, rows)
        w, h, ch, back = proof.read_png(p)
        self.assertEqual((w, h, ch), (W, H, 3))
        self.assertEqual(back, [bytes(r) for r in rows])

    def test_not_a_png(self):
        p = os.path.join(self.d, "x.png")
        with open(p, "wb") as f:
            f.write(b"hello")
        with self.assertRaises(proof.ProofError):
            proof.read_png(p)

    def test_distance_same_and_different(self):
        a = save(os.path.join(self.d, "a.png"), frame(UP))
        b = save(os.path.join(self.d, "b.png"), frame(UP, noise=2))
        c = save(os.path.join(self.d, "c.png"), frame(DOWN))
        reg = proof.parse_region(REGION)
        self.assertLess(proof.distance(proof.grid(a, reg), proof.grid(b, reg)), 3)
        self.assertGreater(proof.distance(proof.grid(a, reg), proof.grid(c, reg)), 30)


class ParseTest(unittest.TestCase):
    def test_parse(self):
        c = proof.parse_proofs("x  A-up1-A  330,300,580,280  refs  refs=lifts  # note\n"
                               "y  B-open-B  0,0,640,360  change  from=B-shut-B  min=15\n"
                               "z  A-hud-A  0,0,200,100  same  max=4 cells=8x4\n")
        self.assertEqual(c["x"]["refs"], "lifts")
        self.assertEqual(c["x"]["max"], proof.MAX_GOOD)
        self.assertEqual((c["y"]["from"], c["y"]["min"]), ("B-shut-B", 15.0))
        self.assertEqual(c["z"]["cells"], (8, 4))

    def test_errors(self):
        for text in ("x A-up1-A 1,2,3 refs\n", "x A 1,2,3,4 look\n", "x A 1,2,3,4 change min=3\n",
                     "x A 1,2,3,4 change from=B\n", "x A 1,2,3,4 same\n", "x A 1,2,3,4 refs\nx A 1,2,3,4 refs\n",
                     "x A 1,2,3,4 refs color=red\n", "x A\n"):
            with self.assertRaises(proof.ProofError, msg=text):
                proof.parse_proofs(text)

    def test_the_repo_checks_parse_and_every_see_is_defined(self):
        checks = proof.load_proofs()
        gdir = os.path.join(proof.HERE, "groups")
        for f in sorted(os.listdir(gdir)):
            if not f.endswith(".txt"):
                continue
            with open(os.path.join(gdir, f), encoding="utf-8-sig") as fh:
                for _, runs in g.parse_groups(fh.read()):
                    for r in runs:
                        for name in r["see"]:
                            self.assertIn(name, checks, "%s: %s see=%s" % (f, r["scenario"], name))
                        self.assertFalse(r["see"] and r["logonly"], "%s: %s has see= and logonly=" % (
                            f, r["scenario"]))


class GradeTest(unittest.TestCase):
    """The deku-platforms case in small: the presser's game shows the lift up, the other game down."""

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.kit = os.path.join(self.d, "kit")
        self.run = os.path.join(self.d, "run")
        refs = os.path.join(self.kit, "proof-refs", "lifts")
        save(os.path.join(refs, "good-01.png"), frame(UP, noise=1))
        save(os.path.join(refs, "good-02.png"), frame(UP, noise=3))
        save(os.path.join(refs, "bad-01.png"), frame(DOWN, noise=2))
        save(os.path.join(self.run, "hs-x-A-up1-A.png"), frame(DOWN))       # A: never rose (the false PASS)
        save(os.path.join(self.run, "hs-x-A-up1-B.png"), frame(UP))         # B: the presser's game: up
        save(os.path.join(self.run, "hs-x-A-ready-B.png"), frame(None))     # before: nothing there
        save(os.path.join(self.run, "hs-x-A-mid-B.png"), frame(None, fg=(0, 0, 0), bg=(128, 128, 128)))
        self.checks = proof.parse_proofs(
            "lifts-A  A-up1-A  %s refs refs=lifts\n"
            "lifts-B  A-up1-B  %s refs refs=lifts\n"
            "rise-B   A-up1-B  %s change from=A-ready-B min=10\n"
            "still-B  A-up1-B  %s change from=A-ready-B max=2\n"
            "pair     A-up1-A  %s same max=5\n"
            "nothere  A-zzz-A  %s refs refs=lifts\n"
            "norefs   A-up1-A  %s refs refs=nothing\n" % ((REGION,) * 7))

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def grade(self, name):
        return proof.grade(self.run, self.checks[name], self.kit)[0]

    def test_refs(self):
        self.assertEqual(self.grade("lifts-A"), "BAD")
        self.assertEqual(self.grade("lifts-B"), "OK")

    def test_change_and_same(self):
        self.assertEqual(self.grade("rise-B"), "OK")
        self.assertEqual(self.grade("still-B"), "BAD")
        self.assertEqual(self.grade("pair"), "BAD")   # A's and B's views differ: not in step

    def test_missing(self):
        self.assertEqual(self.grade("nothere"), "MISSING")
        self.assertEqual(self.grade("norefs"), "MISSING")

    def test_unclear_between_refs(self):
        # Halfway between up and down: neither reference is nearer by the margin.
        save(os.path.join(self.run, "hs-x-A-up1-A.png"), frame((40, 31, 40, 19)))
        self.assertEqual(self.grade("lifts-A"), "UNCLEAR")

    def test_only_good_refs_never_fail(self):
        os.remove(os.path.join(self.kit, "proof-refs", "lifts", "bad-01.png"))
        self.assertEqual(self.grade("lifts-A"), "MISSING")

    def test_the_shot_itself_is_no_reference(self):
        shutil.copy2(os.path.join(self.run, "hs-x-A-up1-A.png"),
                     os.path.join(self.kit, "proof-refs", "lifts", "good-03.png"))
        self.assertEqual(self.grade("lifts-A"), "BAD")

    def test_cli(self):
        p = os.path.join(self.d, "proofs.txt")
        with open(p, "w") as f:
            f.write("lifts-B  A-up1-B  %s refs refs=lifts\n" % REGION)
        self.assertEqual(proof.main(["--kit-root", self.kit, "--proofs", p, "grade", self.run, "lifts-B"]), 0)
        self.assertEqual(proof.main(["--kit-root", self.kit, "ref", "add", "lifts", "bad",
                                     os.path.join(self.run, "hs-x-A-up1-A.png")]), 0)
        self.assertEqual(len(proof.refs(self.kit, "lifts")[1]), 2)


class VerdictTest(unittest.TestCase):
    """groups.py: no PASS from log lines alone."""

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.kit = os.path.join(self.d, "kit")
        refs = os.path.join(self.kit, "proof-refs", "lifts")
        save(os.path.join(refs, "good-01.png"), frame(UP, noise=1))
        save(os.path.join(refs, "bad-01.png"), frame(DOWN, noise=2))
        self.checks = proof.parse_proofs("lifts-A  A-up1-A  %s refs refs=lifts\n"
                                         "lifts-B  A-up1-B  %s refs refs=lifts\n" % (REGION, REGION))
        self.root = os.path.join(self.d, "runs")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def folder(self, name, meta, a_up=True, b_up=True):
        f = os.path.join(self.root, "g1", name, "20261118-060000")
        os.makedirs(f)
        with open(os.path.join(f, "steps.txt"), "w") as fh:
            fh.write("scenario played\n")
        with open(os.path.join(f, "hookshot_log-A.txt"), "w") as fh:
            fh.write("coop: player 2 started the event Lift in this scene\n")
        with open(os.path.join(f, "hookshot_log-B.txt"), "w") as fh:
            fh.write("coop: our game started the event Lift: sent to the others\n")
        with open(os.path.join(f, "run.json"), "w") as fh:
            json.dump(dict({"group": "g1", "scenario": name, "exit": 0,
                            "expects": [["B", "our game started the event Lift"], ["A", "player 2 started"]]},
                           **meta), fh)
        save(os.path.join(f, "%s-A-up1-A.png" % name), frame(UP if a_up else DOWN))
        save(os.path.join(f, "%s-A-up1-B.png" % name), frame(UP if b_up else DOWN))
        return f

    def judge(self, f):
        return g.judge(f, kit_root=self.kit, checks=self.checks)

    def test_logs_alone_are_unproven(self):
        f = self.folder("plain", {})
        self.assertEqual(self.judge(f)[0], "UNPROVEN")

    def test_the_false_pass_now_fails(self):
        f = self.folder("lifts", {"see": ["lifts-A", "lifts-B"]}, a_up=False)
        result, evidence = self.judge(f)
        self.assertEqual(result, "FAIL")
        self.assertIn("BAD lifts-A", evidence)

    def test_seen_in_both_passes(self):
        f = self.folder("lifts", {"see": ["lifts-A", "lifts-B"]})
        self.assertEqual(self.judge(f)[0], "PASS")

    def test_missing_expect_still_fails_with_good_shots(self):
        f = self.folder("lifts", {"see": ["lifts-A", "lifts-B"], "expects": [["A", "never logged"]]})
        self.assertEqual(self.judge(f)[0], "FAIL")

    def test_unknown_check_or_no_refs_is_unproven(self):
        f = self.folder("lifts", {"see": ["lifts-A", "nope"]})
        self.assertEqual(self.judge(f)[0], "UNPROVEN")
        self.assertEqual(g.judge(f, kit_root=os.path.join(self.d, "empty"), checks=self.checks)[0], "UNPROVEN")

    def test_logonly_passes(self):
        f = self.folder("connect", {"logonly": "connect line only"})
        self.assertEqual(self.judge(f), ("PASS", self.judge(f)[1]))
        self.assertIn("[log only: connect line only]", self.judge(f)[1])

    def test_merge_counts_unproven_as_not_passed(self):
        self.folder("plain", {})
        self.folder("connect", {"logonly": "connect"})
        table = g.merge([self.root], kit_root=self.kit, checks=self.checks)
        self.assertTrue(table.endswith("2 run(s): 1 PASS, 1 UNPROVEN -> UNPROVEN"), table)
        self.assertIn("| g1 plain | UNPROVEN |", table)

    def test_regrade_takes_today_s_group_lines(self):
        self.folder("lifts", {}, a_up=False)   # an old run: its run.json has no see=
        defs = {("lifts", ""): (("lifts-A", "lifts-B"), "")}
        table = g.merge([self.root], kit_root=self.kit, defs=defs, checks=self.checks)
        self.assertIn("| g1 lifts | FAIL |", table)
        self.assertTrue(table.endswith("-> FAIL"))

    def test_group_lines(self):
        gs = g.parse_groups('[g1]\nhs-x  expect="A:y"  see=lifts-A  see=lifts-B\nwalk  logonly="connect"\n')
        self.assertEqual(gs[0][1][0]["see"], ["lifts-A", "lifts-B"])
        self.assertEqual(gs[0][1][1]["logonly"], "connect")
        for bad in ("[g1]\nx see=a/b\n", "[g1]\nx logonly=\n"):
            with self.assertRaises(g.Fail):
                g.parse_groups(bad)
        p = os.path.join(self.d, "a.txt")
        with open(p, "w") as f:
            f.write('[g1]\nhs-x  label="L"  see=lifts-A\n[g2]\nhs-x  label="L"  see=lifts-B\nwalk  logonly="c"\n')
        defs = g.group_defs([p])
        self.assertNotIn(("hs-x", "L"), defs)   # two lines, two proofs: ambiguous, left out
        self.assertEqual(defs[("walk", "")], ((), "c"))


class RepoGroupsTest(unittest.TestCase):
    """The real tools/autotest/groups/*.txt and proofs.txt agree (owner rule: no PASS from log lines alone)."""

    def setUp(self):
        here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
        gdir = os.path.join(here, "groups")
        self.files = sorted(os.path.join(gdir, x) for x in os.listdir(gdir) if x.endswith(".txt"))
        self.checks = proof.load_proofs()

    def test_every_log_graded_line_has_proof_or_logonly(self):
        for path in self.files:
            with open(path, encoding="utf-8-sig") as f:
                for name, runs in g.parse_groups(f.read()):
                    for r in runs:
                        where = "%s [%s] %s %s" % (os.path.basename(path), name, r["scenario"], r["label"])
                        if r["expects"]:
                            self.assertTrue(r["see"] or r["logonly"], where + ": expect= without see= or logonly=")
                        for c in r["see"]:
                            self.assertIn(c, self.checks, where + ": see=%s is not in proofs.txt" % c)

    def test_no_two_lines_disagree_on_proof(self):
        seen = {}
        for path in self.files:
            with open(path, encoding="utf-8-sig") as f:
                for _, runs in g.parse_groups(f.read()):
                    for r in runs:
                        k, v = (r["scenario"], r["label"]), (tuple(r["see"]), r["logonly"])
                        self.assertEqual(seen.setdefault(k, v), v, "%s: two group lines give different proofs" % (k,))


if __name__ == "__main__":
    unittest.main()
