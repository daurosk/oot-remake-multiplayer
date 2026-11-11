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
        6: [(sf.LIFT_GRADE, sf.LIFT_NONE)],
        20: [(sf.BOMB_GET_ORDER, 0), (0xcba88c4f, 18), (0xaf5154ab, 15)],
    })


class SaveFlagsTest(unittest.TestCase):
    def test_bombs_sets_what_a_real_bag_sets(self):
        data = autotest_like()
        before = bytes(data)
        done = sf.set_values(data, sf.bomb_changes(data))
        self.assertEqual(len(done), len(sf.BOMBS) + 2)  # + the get order and the lift grade
        self.assertEqual(sf.get(data, 2, 0xdb1bcd4f), 20)
        self.assertEqual(sf.get(data, 2, 0xe753508b), 20)
        self.assertEqual(sf.get(data, 0, 0x6e5e7d9a), 1)
        self.assertEqual(sf.get(data, 20, sf.BOMB_GET_ORDER), 19)
        # Issue 125: the bracelet and its lift grade, or the bomb cannot be lifted.
        self.assertEqual(sf.get(data, 2, 0xd1317941), 1)
        self.assertEqual(sf.get(data, 0, 0x9880707e), 1)
        self.assertEqual(sf.get(data, 6, sf.LIFT_GRADE), sf.LIFT_LOW)
        self.assertEqual(sf.get(data, 0, 0xdb1bcd4f), 0)  # the Bool of the same hash: untouched
        self.assertEqual(len(data), len(before))
        changed = sum(1 for a, b in zip(before, data) if a != b)
        # Only the changed values' bytes, nothing else (an enum value is a hash: up to 4 bytes).
        expected = sum(sum(1 for a, b in zip(struct.pack("<I", old), struct.pack("<I", new)) if a != b)
                       for _, old, new in done)
        self.assertEqual(changed, expected)

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

    def test_a_higher_lift_grade_is_kept(self):
        data = autotest_like()
        high = 0x165af889  # flag_hash("High"): the Golden Gauntlets
        sf.set_values(data, [(6, sf.LIFT_GRADE, high)])
        changes = sf.bomb_changes(data)
        self.assertFalse(any(h == sf.LIFT_GRADE for _, h, _ in changes))
        sf.set_values(data, changes)
        self.assertEqual(sf.get(data, 6, sf.LIFT_GRADE), high)

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

    def test_type_openers_must_all_be_there_in_order(self):
        good = make_save({})
        sf.index(good)
        skipped = bytearray(good)
        struct.pack_into("<II", skipped, 0x20 + 8 * 5, 0, 7)  # type 5 opener says 7
        with self.assertRaises(sf.SaveError):
            sf.index(skipped)
        early_end = bytearray(good)
        struct.pack_into("<I", early_end, 8, 0x20 + 8 * 0x20 + 8)  # pool cuts the end pair
        with self.assertRaises(sf.SaveError):
            sf.index(early_end)


class CommandLineTest(unittest.TestCase):
    """Runs main() on files in a made-up kit, with %APPDATA% and
    %LOCALAPPDATA% pointed elsewhere (the temp folder is under the latter)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.realpath(self.tmp.name)
        self.env = {k: os.environ.get(k) for k in ("APPDATA", "LOCALAPPDATA")}
        os.environ["APPDATA"] = os.path.join(self.root, "roaming")
        os.environ["LOCALAPPDATA"] = os.path.join(self.root, "local")
        self.slots = []
        for n in range(2):
            d = os.path.join(self.root, "kit", "autotest-save", "USER", "TITLE", f"slot_0{n}")
            os.makedirs(d)
            self.slots.append(os.path.join(d, "progress.sav"))
            with open(self.slots[-1], "wb") as f:
                f.write(autotest_like())

    def tearDown(self):
        for k, v in self.env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.tmp.cleanup()

    def test_bombs_on_a_pattern_keeps_a_backup(self):
        old = sf.read_file(self.slots[0])
        pattern = os.path.join(os.path.dirname(os.path.dirname(self.slots[0])), "slot_0*",
                               "progress.sav")
        self.assertEqual(sf.main(["bombs", pattern]), 0)
        for path in self.slots:
            self.assertEqual(sf.get(sf.read_file(path), 2, 0xdb1bcd4f), 20)
            # The baseline folder holds only the save (the runner copies it all
            # into the game's saves); the old file is in autotest-save-backup.
            self.assertEqual(os.listdir(os.path.dirname(path)), ["progress.sav"])
            kept = sf.backup_dir(path)
            self.assertIn(os.sep + "autotest-save-backup" + os.sep, kept)
            backups = os.listdir(kept)
            self.assertEqual(len(backups), 1)
            self.assertTrue(backups[0].startswith("progress.sav.bak-"))
            self.assertEqual(sf.read_file(os.path.join(kept, backups[0])), old)
        self.assertEqual(sf.main(["bombs", self.slots[0]]), 0)  # nothing to change: no new backup
        self.assertEqual(len(os.listdir(sf.backup_dir(self.slots[0]))), 1)
        self.assertEqual(sf.main(["set", self.slots[0], "int:0xdb1bcd4f=7"]), 0)
        self.assertEqual(sf.get(sf.read_file(self.slots[0]), 2, 0xdb1bcd4f), 7)
        self.assertEqual(sf.main(["set", self.slots[0], "int:0x99=1"]), 1)  # not in the save
        self.assertEqual(sf.main(["set", self.slots[0], "float:0x1=1"]), 1)  # type not handled
        self.assertEqual(sf.main(["list", os.path.join(self.root, "kit", "autotest-save")]), 0)

    def test_only_baseline_saves_are_touched(self):
        def refused(path):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(autotest_like())
            before = sf.read_file(path)
            self.assertEqual(sf.main(["bombs", path]), 1)
            self.assertEqual(sf.main(["get", path, "int:0xdb1bcd4f"]), 1)
            self.assertEqual(sf.read_file(path), before)
            self.assertEqual(os.listdir(os.path.dirname(path)), ["progress.sav"])
        # A kit's live NAND, not its baseline.
        refused(os.path.join(self.root, "kit", "user", "nand", "USER", "TITLE", "slot_00",
                             "progress.sav"))
        # A player's own saves: under %APPDATA%, even in a folder named autotest-save.
        refused(os.path.join(self.root, "roaming", "eden", "autotest-save", "USER", "TITLE",
                             "slot_00", "progress.sav"))
        refused(os.path.join(self.root, "local", "autotest-save", "USER", "TITLE", "slot_00",
                             "progress.sav"))
        # Not a slot's progress.sav.
        refused(os.path.join(self.root, "kit", "autotest-save", "USER", "TITLE", "slot_00",
                             "caption.sav").replace("caption.sav", "x", 1) + os.sep + "progress.sav")
        self.assertEqual(sf.main(["bombs", os.path.join(self.root, "nothing*", "progress.sav")]), 1)

    def test_a_save_changed_meanwhile_is_not_written(self):
        old = sf.read_file(self.slots[0])
        new = bytearray(old)
        sf.set_values(new, sf.bomb_changes(new))
        with open(self.slots[0], "wb") as f:
            f.write(old[:-1] + b"")  # the game wrote it
        with self.assertRaises(sf.SaveError):
            sf.write_save(self.slots[0], old, bytes(new))
        self.assertFalse(os.path.exists(sf.backup_dir(self.slots[0])))


if __name__ == "__main__":
    unittest.main()
