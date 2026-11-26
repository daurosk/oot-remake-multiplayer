"""Checks the names file tools/game_scan/game_names.tsv (docs/game-ui.md, "The names file"). No game files.

    python tools/game_scan/tests/test_game_names.py
"""
import os
import re
import unittest

NAMES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "game_names.tsv")

HEADER = ["address", "name", "kind", "status", "note"]
STATUSES = {"checked", "code", "notes", "run"}

# Main of build 1.1.1 (e0f3ccfaf50065ae) as Longshot loads it, from its NSO header (nso.py): [start, end).
TEXT = (0x80020000, 0x83C661D0)
RO = (0x83C67000, 0x8486C080)
DATA = (0x8486D000, 0x84D66B80)
BSS = (0x84D66B80, 0x84EC0000)
SEGMENTS = {"text": TEXT, "ro": RO, "data": DATA, "bss": BSS}
# Where each kind may live.
KINDS = {
    "function": ("text",),
    "string": ("ro",),
    "vtable": ("ro", "data"),
    "global": ("ro", "data", "bss"),
}
ADDRESS = re.compile(r"0x8[0-9a-f]{7}")


def segment(addr):
    for name, (start, end) in SEGMENTS.items():
        if start <= addr < end:
            return name
    return None


def check(text):
    """The problems in a names file's text, one string each (empty: the file is right)."""
    problems = []
    if not text.endswith("\n"):
        problems.append("the file does not end with a new line")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if not lines or lines[0].split("\t") != HEADER:
        problems.append("line 1: the header must be " + "<tab>".join(HEADER))
        return problems
    last = None
    seen = set()
    for no, line in enumerate(lines[1:], start=2):
        fields = line.split("\t")
        if len(fields) != len(HEADER):
            problems.append(f"line {no}: {len(fields)} fields, not {len(HEADER)}")
            continue
        addr, name, kind, status, note = fields
        if any(f != f.strip() for f in fields):
            problems.append(f"line {no}: a field starts or ends with a space")
        if not ADDRESS.fullmatch(addr):
            problems.append(f"line {no}: address {addr!r} is not 0x8 and 7 lowercase hex digits")
            continue
        a = int(addr, 16)
        if not name.strip():
            problems.append(f"line {no}: no name")
        if not note.strip():
            problems.append(f"line {no}: no note")
        if status not in STATUSES:
            problems.append(f"line {no}: status {status!r} is not one of {sorted(STATUSES)}")
        seg = segment(a)
        if seg is None:
            problems.append(f"line {no}: {addr} is outside main")
        elif kind not in KINDS:
            problems.append(f"line {no}: kind {kind!r} is not one of {sorted(KINDS)}")
        elif seg not in KINDS[kind]:
            problems.append(f"line {no}: a {kind} at {addr} is in {seg}, not {' or '.join(KINDS[kind])}")
        if a in seen:
            problems.append(f"line {no}: {addr} is listed twice")
        elif last is not None and a < last:
            problems.append(f"line {no}: {addr} is not sorted (after 0x{last:08x})")
        seen.add(a)
        last = a
    return problems


def tsv(*rows):
    return "\n".join("\t".join(r) for r in [HEADER, *rows]) + "\n"


class TestTheFile(unittest.TestCase):
    def test_the_names_file_is_right(self):
        with open(NAMES, encoding="utf-8") as f:
            self.assertEqual(check(f.read()), [])

    def test_the_names_file_is_not_empty(self):
        with open(NAMES, encoding="utf-8") as f:
            self.assertGreater(len(f.read().splitlines()), 50)


class TestTheCheck(unittest.TestCase):
    GOOD = (
        ("0x80020000", "First function", "function", "checked", "the start of main"),
        ("0x83c67000", "A name", "string", "notes", "in ro"),
        ("0x84c106e8", "A vtable", "vtable", "checked", "in data"),
        ("0x84ebffff", "A global", "global", "code", "the last byte of bss"),
    )

    def test_a_good_file(self):
        self.assertEqual(check(tsv(*self.GOOD)), [])

    def problems(self, *rows):
        return " | ".join(check(tsv(*rows)))

    def test_the_header(self):
        self.assertIn("header", " ".join(check("addr\tname\n")))
        self.assertIn("header", " ".join(check("")))

    def test_the_last_new_line(self):
        self.assertIn("new line", " ".join(check(tsv(*self.GOOD)[:-1])))

    def test_unsorted_and_twice(self):
        a, b = self.GOOD[0], self.GOOD[2]
        self.assertIn("not sorted", self.problems(b, a))
        self.assertIn("listed twice", self.problems(a, a))

    def test_addresses(self):
        for addr in ("0x8002000", "0X80020000", "0x8002000G", "80020000", "0x80A20000", "0x900200000"):
            self.assertIn("is not 0x8", self.problems((addr, "n", "function", "checked", "note")), addr)
        self.assertIn("outside main", self.problems(("0x80000000", "rtld", "function", "checked", "note")))
        self.assertIn("outside main", self.problems(("0x84ec0000", "past bss", "global", "checked", "note")))

    def test_kinds_and_segments(self):
        self.assertIn("kind 'class'", self.problems(("0x84c106e8", "n", "class", "checked", "note")))
        self.assertIn("is in data, not text", self.problems(("0x84c106e8", "n", "function", "checked", "note")))
        self.assertIn("is in text, not ro", self.problems(("0x80020000", "n", "string", "checked", "note")))
        self.assertIn("is in bss", self.problems(("0x84ea4530", "n", "vtable", "checked", "note")))

    def test_names_notes_and_statuses(self):
        self.assertIn("no name", self.problems(("0x80020000", "", "function", "checked", "note")))
        self.assertIn("no note", self.problems(("0x80020000", "n", "function", "checked", "")))
        self.assertIn("status 'seen'", self.problems(("0x80020000", "n", "function", "seen", "note")))
        self.assertIn("4 fields", self.problems(("0x80020000", "n", "function", "checked")))
        self.assertIn("space", self.problems(("0x80020000", "n ", "function", "checked", "note")))


if __name__ == "__main__":
    unittest.main()
