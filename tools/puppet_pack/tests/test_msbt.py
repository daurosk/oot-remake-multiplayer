"""Tests for the MSBT reader (msbt.py). Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_msbt.py
"""
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import msbt, sarc  # noqa: E402
from puppet_pack.msbt import Tag, TagEnd  # noqa: E402

try:
    import zstandard
except ImportError:  # pragma: no cover
    zstandard = None

MSBT_PY = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "msbt.py")
LABELS = ["Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot"]


def label_hash(name, buckets):
    h = 0
    for c in name.encode("ascii"):
        h = (h * 0x492 + c) & 0xFFFFFFFF
    return h % buckets


def encode_text(items, encoding, order):
    """A text's bytes: characters, tags (0x0E) and closing tags (0x0F), then a 0 character."""
    unit = {"utf-8": 1, "utf-16": 2, "utf-32": 4}[encoding]
    codec = encoding if unit == 1 else encoding + ("-le" if order == "<" else "-be")

    def char(c):
        return struct.pack(order + {1: "B", 2: "H", 4: "I"}[unit], c)

    out = b""
    for item in items:
        if isinstance(item, Tag):
            out += char(0x0E) + struct.pack(order + "HHH", item.group, item.type, len(item.params)) + item.params
        elif isinstance(item, TagEnd):
            out += char(0x0F) + struct.pack(order + "HH", item.group, item.type)
        else:
            out += item.encode(codec)
    return out + char(0)


def make_msbt(entries, encoding="utf-16", order="<", buckets=4, atr=None, extra=(), labels=None):
    """An MSBT: LBL1 (each label in its hash bucket), ATR1 when `atr` is given, TXT2, then `extra`
    sections. entries: [(label, items)]; labels: [(label, index)] in place of the entries' own."""
    texts = [encode_text(items, encoding, order) for _, items in entries]
    table = [[] for _ in range(buckets)]
    for label, i in labels if labels is not None else [(lab, i) for i, (lab, _) in enumerate(entries)]:
        table[label_hash(label, buckets)].append((label, i))
    heads, body = b"", b""
    for bucket in table:
        heads += struct.pack(order + "II", len(bucket), 4 + 8 * buckets + len(body))
        for label, i in bucket:
            body += bytes([len(label)]) + label.encode("ascii") + struct.pack(order + "I", i)
    sections = [("LBL1", struct.pack(order + "I", buckets) + heads + body)]
    if atr is not None:
        size = len(atr[0]) if atr else 0
        sections.append(("ATR1", struct.pack(order + "II", len(atr), size) + b"".join(atr)))
    offsets, at = [], 4 + 4 * len(texts)
    for t in texts:
        offsets.append(at)
        at += len(t)
    sections.append(("TXT2", struct.pack(order + "I%dI" % len(texts), len(texts), *offsets) + b"".join(texts)))
    sections += list(extra)
    out = b""
    for magic, data in sections:
        sec = magic.encode("ascii") + struct.pack(order + "I", len(data)) + b"\0" * 8 + data
        out += sec + b"\xab" * (-len(sec) % 16)
    bom = b"\xff\xfe" if order == "<" else b"\xfe\xff"
    enc = {"utf-8": 0, "utf-16": 1, "utf-32": 2}[encoding]
    header = (b"MsgStdBn" + bom + b"\0\0" + bytes([enc, 3])
              + struct.pack(order + "HHI", len(sections), 0, 0x20 + len(out)) + b"\0" * 10)
    return header + out


GREETING = ["Hi ", Tag(0, 3, b"\x01\x00\xff\x00"), "there", TagEnd(0, 3), "!"]
# In 4 buckets, Lines is in bucket 1 and the other two in bucket 3: the file's label order is
# Lines, Greeting, Blank.
SAMPLE = [("Greeting", GREETING), ("Blank", []), ("Lines", ["One\nTwo"])]


class ReadTest(unittest.TestCase):
    def test_utf16_little_endian_with_tags(self):
        m = msbt.read(make_msbt(SAMPLE, atr=[b"\x01\x02", b"\x03\x04", b"\x05\x06"]))
        self.assertFalse(m.big_endian)
        self.assertEqual((m.encoding, m.version), ("utf-16", 3))
        self.assertEqual(m.text("Greeting"), GREETING)
        self.assertEqual(msbt.render(m.text("Greeting")), "Hi {0.3:0100ff00}there{/0.3}!")
        self.assertEqual(m.text("Blank"), [])
        self.assertEqual(msbt.render(m.text("Blank")), "")
        self.assertEqual(msbt.render(m.text("Lines")), "One\\nTwo")
        self.assertEqual(m.attributes, [b"\x01\x02", b"\x03\x04", b"\x05\x06"])
        self.assertEqual(m.section_names(), ["LBL1", "ATR1", "TXT2"])
        self.assertEqual(msbt.tag_counts(m), {(0, 3): 1})

    def test_utf8(self):
        items = ["Café ", Tag(1, 0), "→ ", Tag(2, 7, b"\x05\x00"), "x", TagEnd(2, 7)]
        m = msbt.read(make_msbt([("Menu", items), ("Empty", [])], encoding="utf-8"))
        self.assertEqual(m.encoding, "utf-8")
        self.assertEqual(m.text("Menu"), items)
        self.assertEqual(msbt.render(m.text("Menu")), "Café {1.0}→ {2.7:0500}x{/2.7}")
        self.assertEqual(m.text("Empty"), [])
        self.assertIsNone(m.attributes)

    def test_big_endian(self):
        m = msbt.read(make_msbt(SAMPLE, order=">"))
        self.assertTrue(m.big_endian)
        self.assertEqual(m.encoding, "utf-16")
        self.assertEqual([lab.name for lab in m.labels], [lab for lab, _ in self.file_order(SAMPLE, 4)])
        self.assertEqual(m.text("Greeting"), GREETING)
        self.assertEqual(msbt.render(m.text("Greeting")), "Hi {0.3:0100ff00}there{/0.3}!")

    def test_utf32(self):
        m = msbt.read(make_msbt([("Wide", ["a\U0001f600", Tag(4, 1, b"\x09")])], encoding="utf-32"))
        self.assertEqual(m.text("Wide"), ["a\U0001f600", Tag(4, 1, b"\x09")])

    @staticmethod
    def file_order(entries, buckets):
        table = [[] for _ in range(buckets)]
        for i, (label, _) in enumerate(entries):
            table[label_hash(label, buckets)].append((label, i))
        return [x for bucket in table for x in bucket]

    def test_labels_in_file_order_across_buckets(self):
        entries = [(lab, ["text %d" % i]) for i, lab in enumerate(LABELS)]
        order = self.file_order(entries, 3)
        # (the data is what the test needs: several buckets, one with more than one label,
        # and a file order other than the text order)
        self.assertGreater(len({label_hash(lab, 3) for lab in LABELS}), 1)
        self.assertGreater(max(sum(label_hash(lab, 3) == b for lab in LABELS) for b in range(3)), 1)
        self.assertNotEqual([lab for lab, _ in order], LABELS)
        m = msbt.read(make_msbt(entries, buckets=3))
        self.assertEqual(m.buckets, 3)
        self.assertEqual([(lab.name, lab.index) for lab in m.labels], order)
        for i, lab in enumerate(LABELS):
            self.assertEqual(m.text(lab), ["text %d" % i])
        self.assertEqual([r[0] for r in m.rows()], [lab for lab, _ in order])
        with self.assertRaises(KeyError):
            m.text("Golf")

    def test_texts_without_a_label_and_other_sections(self):
        extra = [("NLI1", struct.pack("<I", 0)), ("TSY1", b"\0" * 12)]
        m = msbt.read(make_msbt(SAMPLE, labels=[("Greeting", 0), ("Lines", 2)], extra=extra))
        self.assertEqual(m.section_names(), ["LBL1", "TXT2", "NLI1", "TSY1"])
        self.assertEqual(dict(m.sections)["TSY1"], b"\0" * 12)
        self.assertEqual([(n, msbt.render(t)) for n, t in m.rows()],
                         [("Lines", "One\\nTwo"), ("Greeting", msbt.render(GREETING)), ("#1", "")])

    def test_render_escapes(self):
        self.assertEqual(msbt.render("a\\b{c}\td\re\x01 　\U000f0000"),
                         "a\\\\b\\{c}\\td\\re\\x01 \\ue000\\u3000\\U000f0000")
        self.assertEqual(msbt.render([Tag(65535, 1), TagEnd(65535, 1)]), "{65535.1}{/65535.1}")

    def test_refuses_broken_files(self):
        good = make_msbt(SAMPLE)
        with self.assertRaises(msbt.MsbtError):
            msbt.read(b"MsgStdBX" + good[8:])
        with self.assertRaises(msbt.MsbtError):
            msbt.read(good[:8] + b"\x00\x00" + good[10:])   # no byte-order mark
        with self.assertRaises(msbt.MsbtError):
            msbt.read(good[:0x0C] + b"\x07" + good[0x0D:])   # no such encoding
        with self.assertRaises(msbt.MsbtError):
            msbt.read(good[:0x40])                            # a section cut short
        # a tag whose parameters run past its section's end
        tag = struct.pack("<HHHH", 0x0E, 0, 3, 0)
        good = make_msbt([("A", ["x", Tag(0, 3)])])
        self.assertEqual(good.count(tag), 1)
        with self.assertRaisesRegex(msbt.MsbtError, "parameters of tag 0.3"):
            msbt.read(good.replace(tag, struct.pack("<HHHH", 0x0E, 0, 3, 40)))
        # a label that names a text the file does not have
        with self.assertRaises(msbt.MsbtError):
            msbt.read(make_msbt(SAMPLE, labels=[("Greeting", 5)]))


class CliTest(unittest.TestCase):
    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        code = msbt.main(list(argv), out=out, err=err)
        return code, out.getvalue(), err.getvalue()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.plain = self.put("Menu_00.msbt", make_msbt(SAMPLE))
        self.archive = self.put("EUen.Product.110.sarc", sarc.write({
            "LayoutMsg/Menu_00.msbt": make_msbt(SAMPLE),
            "LayoutMsg/Other_00.msbt": make_msbt([("Only", ["one"])], encoding="utf-8"),
            "Font/notes.txt": b"not a message file",
        }))

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, name, data):
        path = os.path.join(self.dir, *name.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def test_plain_file(self):
        code, out, err = self.run_cli(self.plain)
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines(), ["Lines\tOne\\nTwo", "Greeting\tHi {0.3:0100ff00}there{/0.3}!", "Blank\t"])
        self.assertIn("3 labels, 3 texts, utf-16 little-endian, sections LBL1 TXT2, tags 0.3 x1", err)
        code, out, err = self.run_cli(self.plain, "--label", "Lines")
        self.assertEqual((code, out), (0, "Lines\tOne\\nTwo\n"))

    def test_archive(self):
        code, out, err = self.run_cli(self.archive, "--list")
        self.assertEqual(code, 0)
        self.assertEqual(sorted(out.splitlines()), ["LayoutMsg/Menu_00.msbt", "LayoutMsg/Other_00.msbt"])
        self.assertIn("2 .msbt files, 3 files in all", err)
        code, out, err = self.run_cli(self.archive, "--file", "LayoutMsg/Other_00.msbt")
        self.assertEqual((code, out), (0, "Only\tone\n"))
        code, out, err = self.run_cli(self.archive, "--file", "LayoutMsg/Menu_00.msbt", "--label", "Greeting")
        self.assertEqual((code, out), (0, "Greeting\tHi {0.3:0100ff00}there{/0.3}!\n"))

    def test_mistakes_exit_1(self):
        for argv in ([self.archive],                                    # an archive without --file
                     [self.archive, "--file", "LayoutMsg/Nothing.msbt"],
                     [self.archive, "--file", "LayoutMsg/Menu_00.msbt", "--label", "Nobody"],
                     [self.plain, "--list"],                            # not an archive
                     [os.path.join(self.dir, "missing.msbt")]):
            code, out, err = self.run_cli(*argv)
            self.assertEqual(code, 1, argv)
            self.assertTrue(err.startswith("msbt: "), err)
        # a .zs file outside a romfs, without --romfs: no dictionaries to read it with
        zs_file = self.put("Elsewhere/EUen.Product.110.sarc.zs", b"\x28\xb5\x2f\xfd" + b"\0" * 16)
        code, out, err = self.run_cli(zs_file, "--list")
        self.assertEqual(code, 1)
        self.assertIn("--romfs", err)

    @unittest.skipUnless(zstandard, "needs the zstandard package")
    def test_zstd_archive_in_a_romfs(self):
        # The dictionary pack's only member is not used (the archive is compressed without one);
        # the tool finds the romfs from the archive's Mals folder.
        self.put("romfs/Pack/ZsDic.pack.zs", sarc.write({"zs.zsdic": b"unused dictionary " * 8}))
        with open(self.archive, "rb") as f:
            packed = zstandard.ZstdCompressor().compress(f.read())
        path = self.put("romfs/Mals/EUen.Product.110.sarc.zs", packed)
        code, out, err = self.run_cli(path, "--file", "LayoutMsg/Other_00.msbt")
        self.assertEqual((code, out), (0, "Only\tone\n"), err)

    def test_runs_as_a_script_in_isolated_mode(self):
        r = subprocess.run([sys.executable, "-I", MSBT_PY, self.archive, "--file", "LayoutMsg/Menu_00.msbt",
                            "--label", "Greeting"], capture_output=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.decode("utf-8").replace("\r\n", "\n"), "Greeting\tHi {0.3:0100ff00}there{/0.3}!\n")


if __name__ == "__main__":
    unittest.main()
