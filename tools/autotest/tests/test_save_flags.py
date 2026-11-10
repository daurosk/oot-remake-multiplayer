"""Tests for tools/autotest/save_flags.py on a small made-up save."""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import save_flags as sf  # noqa: E402


def make_save(entries, pool_extra=b"\x00" * 16):
    """entries: {type id: [(hash, value), ...]}; every type 0..0x1f opened."""
    table = b""
    for t in range(0x20):
        table += struct.pack("<II", 0, t)
        for h, v in entries.get(t, []):
            table += struct.pack("<II", h, v)
    table += struct.pack("<II", 0, 0x20) + struct.pack("<II", 0x1234, 0)
    pool = 0x20 + len(table)
    head = struct.pack("<III", sf.MAGIC, 7, pool) + b"\x00" * 20
    return bytearray(head + table + pool_extra)


def autotest_like():
    """No bombs; the slingshot got 18th, a bool 0xdb1bcd4f to show types apart."""
    return make_save({
        0: [(h, 0) for k, h, _ in sf.BOMBS if k == "bool"] + [(0xdb1bcd4f, 0)],
        2: [(h, 0) for k, h, _ in sf.BOMBS if k == "int"],
        20: [(sf.BOMB_GET_ORDER, 0), (0xcba88c4f, 18), (0xaf5154ab, 15)],
    })


class SaveFlagsTest(unittest.TestCase):
    def test_bombs_sets_what_a_real_bag_sets(self):
        data = autotest_like()
        before = bytes(data)
        done = sf.set_values(data, sf.bomb_changes(data))
        self.assertEqual(len(done), len(sf.BOMBS) + 1)
        self.assertEqual(sf.get(data, 2, 0xdb1bcd4f), 20)
        self.assertEqual(sf.get(data, 2, 0xe753508b), 20)
        self.assertEqual(sf.get(data, 0, 0x6e5e7d9a), 1)
        self.assertEqual(sf.get(data, 20, sf.BOMB_GET_ORDER), 19)
        self.assertEqual(sf.get(data, 0, 0xdb1bcd4f), 0)  # the Bool of the same hash: untouched
        self.assertEqual(len(data), len(before))
        changed = sum(1 for a, b in zip(before, data) if a != b)
        self.assertEqual(changed, len(done))  # one byte per value here, nothing else

    def test_a_real_bomb_save_is_never_lowered(self):
        data = autotest_like()
        sf.set_values(data, [(2, 0xdb1bcd4f, 25), (20, sf.BOMB_GET_ORDER, 35)]
                      + [(0, h, 1) for k, h, _ in sf.BOMBS if k == "bool"])
        changes = sf.bomb_changes(data)
        self.assertNotIn((2, 0xdb1bcd4f, 20), changes)
        self.assertFalse(any(h == sf.BOMB_GET_ORDER for _, h, _ in changes))
        sf.set_values(data, changes)
        self.assertEqual(sf.get(data, 2, 0xdb1bcd4f), 25)
        self.assertEqual(sf.get(data, 20, sf.BOMB_GET_ORDER), 35)

    def test_missing_flag_changes_nothing(self):
        data = make_save({2: [(0xdb1bcd4f, 0)]})
        before = bytes(data)
        with self.assertRaises(sf.SaveError):
            sf.set_values(data, [(2, 0xdb1bcd4f, 20), (0, 0x2bb00768, 1)])
        self.assertEqual(bytes(data), before)

    def test_not_a_save(self):
        with self.assertRaises(sf.SaveError):
            sf.index(bytearray(b"\x00" * 64))
        data = make_save({})
        struct.pack_into("<I", data, 8, len(data) + 8)  # pool past the end
        with self.assertRaises(sf.SaveError):
            sf.index(data)

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "progress.sav")
            with open(path, "wb") as f:
                f.write(autotest_like())
            self.assertEqual(sf.main(["bombs", path]), 0)
            self.assertEqual(sf.main(["set", path, "int:0xdb1bcd4f=7"]), 0)
            data = open(path, "rb").read()
            self.assertEqual(sf.get(data, 2, 0xdb1bcd4f), 7)
            self.assertEqual(sf.main(["set", path, "int:0x99=1"]), 1)  # not in the save
            self.assertEqual(sf.main(["set", path, "float:0x1=1"]), 1)  # type not handled


if __name__ == "__main__":
    unittest.main()
