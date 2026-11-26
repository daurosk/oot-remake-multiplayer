"""Tests for merge_tables.py (#48: a seed from OOTRR's website beside the puppet pack). Made-up tables only.

    python tools/puppet_pack/tests/test_merge_tables.py
"""
import contextlib
import io
import os
import random
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import byml, merge_tables as mt, rstb, sarc, ui_pack  # noqa: E402

try:
    import zstandard
except ImportError:  # pragma: no cover
    zstandard = None

NAME = "ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs"
PACK = "contents/0400486024DEC000/coop-puppet"


def read(path):
    with open(path, "rb") as f:
        return f.read()


@contextlib.contextmanager
def stopped_at(*paths, nth=1):
    """merge_tables.write_atomic stops (OSError) at the nth write to any of `paths`, as a power cut or a killed
    process would between two writes of one merge: the writes before it are done, the later ones never happen."""
    real, seen = mt.write_atomic, []

    def stopping(path, data):
        if os.path.normcase(os.path.abspath(path)) in {os.path.normcase(os.path.abspath(p)) for p in paths}:
            seen.append(path)
            if len(seen) == nth:
                raise OSError("stopped")
        real(path, data)

    mt.write_atomic = stopping
    try:
        yield seen
    finally:
        mt.write_atomic = real


@unittest.skipIf(zstandard is None, "needs zstandard")
class MergeTablesTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="merge_tables_test_")
        self.mods = os.path.join(self.dir, "mods")
        self.romfs = os.path.join(self.dir, "romfs")
        self.lines = []
        os.makedirs(self.mods)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def say(self, line):
        self.lines.append(line)

    def path(self, mod, *rest):
        return os.path.join(self.mods, *mod.split("/"), *rest)

    def put_table(self, mod, by_crc, by_name=None, compress=True, dict_data=None):
        t = rstb.Rstb(by_crc=by_crc, by_name=by_name or {})
        raw = t.dumps()
        if compress:
            raw = zstandard.ZstdCompressor(dict_data=dict_data, write_content_size=True).compress(raw)
        p = self.path(mod, "romfs", "System", "Resource", NAME)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(raw)
        return p

    def table(self, mod):
        with open(self.path(mod, "romfs", "System", "Resource", NAME), "rb") as f:
            return mt.Codec(self.romfs).load(f.read())

    def mark_ours(self, mod):
        """A seed the game built (coop_rando writes coop-rando.txt into each); one put in by hand has none."""
        with open(self.path(mod, mt.SEED_MARK), "w", encoding="utf-8", newline="\n") as f:
            f.write("default\n")

    def run_merge(self):
        self.lines = []
        return mt.merge_mods(self.mods, self.romfs, self.say)

    def test_a_site_seed_that_wins_gets_the_pack_entries_and_keeps_its_own(self):
        self.put_table(PACK, {1: 100, 2: 50, 3: 9}, {"Puppet/new.bfres": 77})
        site = self.put_table("ootrr-site", {1: 120, 2: 40, 4: 5})
        with open(site, "rb") as f:
            own = f.read()
        self.assertEqual(self.run_merge(), 0)
        t = self.table("ootrr-site")
        self.assertEqual(t.by_crc, {1: 120, 2: 50, 3: 9, 4: 5})  # each entry, the larger size
        self.assertEqual(t.by_name, {"Puppet/new.bfres": 77})
        self.assertIn("ootrr-site", self.lines[0])
        self.assertIn("the puppet pack", self.lines[0])
        with open(self.path("ootrr-site", mt.KEEP_DIR, NAME), "rb") as f:
            self.assertEqual(f.read(), own)  # the original, kept
        # The next start: nothing to do; the stamp is what the module checks.
        self.run_merge()
        self.assertIn("already", self.lines[0])
        stamp = read(self.path("ootrr-site", mt.KEEP_DIR, mt.STAMP)).decode("utf-8").split("\n")
        self.assertEqual(stamp[0], "coop-merged 1")
        self.assertTrue(stamp[1].startswith(f"out ootrr-site/romfs/System/Resource/{NAME} "))
        self.assertTrue(stamp[2].startswith(f"in {PACK}/romfs/System/Resource/{NAME} "))
        # The module's mark (coop_rando MergedByKit): the table written and the pack's, by FNV-1a 64.
        mark = read(self.path("ootrr-site", mt.MARK)).decode("utf-8")
        table_hash = mt.fnv64(site)[0]
        pack_hash = mt.fnv64(self.path(PACK, "romfs", "System", "Resource", NAME))[0]
        self.assertEqual(mark, f"coop-merged 1\ntable {NAME} {table_hash}\npack {pack_hash}\nrule merged\n")
        # Undo puts the site's own table back.
        mt.undo(self.mods, self.say)
        self.assertFalse(os.path.exists(self.path("ootrr-site", mt.MARK)))
        with open(site, "rb") as f:
            self.assertEqual(f.read(), own)
        self.assertFalse(os.path.exists(self.path("ootrr-site", mt.KEEP_DIR)))

    def test_a_pack_made_again_is_merged_again_from_the_original(self):
        self.put_table(PACK, {1: 100})
        self.put_table("ootrr-site", {2: 5})
        self.run_merge()
        self.put_table(PACK, {1: 100, 3: 30})  # (a kit update made the pack again)
        self.run_merge()
        self.assertEqual(self.table("ootrr-site").by_crc, {1: 100, 2: 5, 3: 30})
        self.assertEqual(rstb.Rstb.loads(zstandard.ZstdDecompressor().decompress(read(
            self.path("ootrr-site", mt.KEEP_DIR, NAME)))).by_crc, {2: 5})  # still the site's own

    def test_a_new_seed_put_over_the_merged_one_is_kept_as_the_new_original(self):
        self.put_table(PACK, {1: 100})
        self.put_table("ootrr-site", {2: 5})
        self.run_merge()
        self.put_table("ootrr-site", {7: 70})  # another seed from the site, same folder
        self.run_merge()
        self.assertEqual(self.table("ootrr-site").by_crc, {1: 100, 7: 70})
        mt.undo(self.mods, self.say)
        self.assertEqual(self.table("ootrr-site").by_crc, {7: 70})

    def test_undo_leaves_a_new_seed_put_in_after_the_merge_alone(self):
        self.put_table(PACK, {1: 100})
        self.put_table("ootrr-site", {2: 5})
        self.run_merge()
        self.put_table("ootrr-site", {7: 70})  # another seed from the site, no start since
        mt.undo(self.mods, self.say)
        self.assertEqual(self.table("ootrr-site").by_crc, {7: 70})  # not the old seed's {2: 5}
        self.assertIn("left as it is", self.lines[-1])
        self.assertFalse(os.path.exists(self.path("ootrr-site", mt.KEEP_DIR)))
        self.assertFalse(os.path.exists(self.path("ootrr-site", mt.MARK)))

    def test_a_merge_stopped_after_the_stamp_keeps_the_true_original(self):
        self.put_table(PACK, {1: 100})
        self.put_table("ootrr-site", {2: 5})
        real = mt.write_atomic
        calls = []

        def stop_at_the_table(path, data):
            if path.endswith(NAME):
                calls.append(path)
                raise OSError("stopped")
            real(path, data)

        mt.write_atomic = stop_at_the_table
        try:
            self.assertEqual(self.run_merge(), 1)
        finally:
            mt.write_atomic = real
        self.assertTrue(calls)  # the stamp was written, the table not
        self.run_merge()
        self.assertEqual(self.table("ootrr-site").by_crc, {1: 100, 2: 5})
        mt.undo(self.mods, self.say)
        self.assertEqual(self.table("ootrr-site").by_crc, {2: 5})  # the site's own, not a merged one

    def test_no_zstandard_is_one_line_not_a_stop(self):
        self.put_table(PACK, {1: 100})
        self.put_table("ootrr-site", {2: 5})
        real = mt.zs._zstd

        def none():
            raise SystemExit("this tool needs the 'zstandard' Python package")

        mt.zs._zstd = none
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                self.assertEqual(mt.main(["--mods", self.mods]), 1)
        finally:
            mt.zs._zstd = real
        self.assertIn("no zstandard", out.getvalue())

    def test_our_seeds_are_left_alone(self):
        self.put_table(PACK, {1: 100})
        self.put_table("randomizer-S1", {2: 5})  # ours: built beside every table already
        self.mark_ours("randomizer-S1")
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(self.lines, [])
        self.assertEqual(self.table("randomizer-S1").by_crc, {2: 5})
        self.assertEqual(self.table(PACK).by_crc, {1: 100})

    def test_a_hand_put_seed_that_wins_is_merged_like_any_mod(self):
        # Nax 2026-11-20: a seed built with an OOTRR from source, its randomizer-<seed> folder copied to every player
        # (no coop-rando.txt). Not ours: it gets the pack's entries, its own table kept (which coop_rando's
        # SeedFilesHash reads, so two players' copies still match after their merges).
        self.put_table(PACK, {1: 100, 3: 9})
        hand = self.put_table("randomizer-NAX1", {1: 120, 2: 5})
        own = read(hand)
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(self.table("randomizer-NAX1").by_crc, {1: 120, 2: 5, 3: 9})
        self.assertEqual(read(self.path("randomizer-NAX1", mt.KEEP_DIR, NAME)), own)
        self.assertTrue(os.path.isfile(self.path("randomizer-NAX1", mt.MARK)))
        self.assertIn("randomizer-NAX1", self.lines[0])

    def test_a_hand_put_seed_under_a_winning_pack_is_merged_into_it(self):
        self.put_table(PACK, {1: 100})
        self.put_table("randomizer-NAX1", {4: 4})
        self.put_table("randomizer-S1", {5: 5})
        self.mark_ours("randomizer-S1")
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"coop-mods 1\non {PACK}\non randomizer-S1\non randomizer-NAX1\n")
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(self.table(PACK).by_crc, {1: 100, 4: 4})  # the hand seed's in, our seed's not

    def test_a_mod_under_the_pack_by_name_gets_its_entries_into_the_packs_table(self):
        # Player report 101: "bestbuddy" (RSDB tables made larger, its own size table) sorts under "contents/", so
        # the game reads the pack's table; without bestbuddy's sizes the game aborted 1 s into the start.
        pack = self.put_table(PACK, {1: 100, 2: 50}, {"Puppet/new.bfres": 77})
        own = read(pack)
        self.put_table("bestbuddy", {2: 80, 3: 3}, {"RSDB/Challenge.Product.111.rstbl.byml": 9000})
        self.assertEqual([n for n, _ in mt.load_order(self.mods)], ["bestbuddy", PACK])
        self.assertEqual(self.run_merge(), 0)
        t = self.table(PACK)
        self.assertEqual(t.by_crc, {1: 100, 2: 80, 3: 3})
        self.assertEqual(t.by_name, {"Puppet/new.bfres": 77, "RSDB/Challenge.Product.111.rstbl.byml": 9000})
        self.assertIn("the puppet pack's", self.lines[0])
        self.assertIn("bestbuddy", self.lines[0])
        self.assertEqual(read(self.path(PACK, mt.KEEP_DIR, NAME)), own)  # the pack's own, kept
        self.assertEqual(self.table("bestbuddy").by_crc, {2: 80, 3: 3})  # never changed
        # The next start: nothing to write (a pack table rewritten at every start would make our seeds be built again).
        before = read(pack)
        self.run_merge()
        self.assertIn("already", self.lines[0])
        self.assertEqual(read(pack), before)
        # The pack made again (a kit update): merged again from the new pack table.
        self.put_table(PACK, {1: 100, 5: 55})
        self.run_merge()
        self.assertEqual(self.table(PACK).by_crc, {1: 100, 2: 80, 3: 3, 5: 55})
        # Undo: the pack's own table back.
        mt.undo(self.mods, self.say)
        self.assertEqual(self.table(PACK).by_crc, {1: 100, 5: 55})
        self.assertFalse(os.path.exists(self.path(PACK, mt.KEEP_DIR)))

    def test_our_seed_under_a_winning_pack_is_not_merged_into_it(self):
        self.put_table(PACK, {1: 100})
        self.put_table("bestbuddy", {3: 3})
        self.put_table("randomizer-S1", {4: 4})
        self.mark_ours("randomizer-S1")
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"coop-mods 1\non {PACK}\non randomizer-S1\n")
        self.assertEqual([n for n, _ in mt.load_order(self.mods)], ["bestbuddy", "randomizer-S1", PACK])
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(self.table(PACK).by_crc, {1: 100, 3: 3})  # bestbuddy's in, the seed's not
        self.assertNotIn("randomizer-S1", self.lines[0])
        self.assertEqual(self.table("randomizer-S1").by_crc, {4: 4})

    def test_a_mod_switched_off_under_the_pack_is_not_merged(self):
        self.put_table(PACK, {1: 100})
        self.put_table("bestbuddy", {3: 3})
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"coop-mods 1\non {PACK}\noff bestbuddy\n")
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(self.lines, [])
        self.assertEqual(self.table(PACK).by_crc, {1: 100})

    def mark_rule(self, mod):
        try:
            text = read(self.path(mod, mt.MARK)).decode("utf-8")
        except OSError:
            return None
        return next((l[5:] for l in text.split("\n") if l.startswith("rule ")), None)

    def test_a_table_holding_the_pack_entries_already_is_left_as_it_is_and_marked_contains(self):
        # (a skins mod whose maker merged the pack's entries in by hand: larger sizes and extra entries of its own)
        self.put_table(PACK, {1: 100, 2: 50}, {"Puppet/new.bfres": 77})
        own = read(self.put_table("skins", {1: 100, 2: 60, 5: 5}, {"Puppet/new.bfres": 80}))
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(read(self.path("skins", "romfs", "System", "Resource", NAME)), own)  # untouched
        self.assertFalse(os.path.exists(self.path("skins", mt.KEEP_DIR)))
        self.assertEqual(self.mark_rule("skins"), "contains")
        mark = read(self.path("skins", mt.MARK)).decode("utf-8").split("\n")
        self.assertEqual(mark[1], f"table {NAME} {mt.fnv64_bytes(own)}")
        self.assertIn("holds every puppet pack entry", self.lines[0])
        # A pack made again with a new entry: no longer contained, so merged as any other seed.
        self.put_table(PACK, {1: 100, 2: 50, 3: 9}, {"Puppet/new.bfres": 77})
        self.run_merge()
        self.assertEqual(self.table("skins").by_crc, {1: 100, 2: 60, 3: 9, 5: 5})
        self.assertEqual(self.mark_rule("skins"), "merged")

    def test_a_smaller_size_is_not_contained(self):
        self.put_table(PACK, {1: 100})
        self.put_table("skins", {1: 99})
        self.run_merge()
        self.assertEqual(self.mark_rule("skins"), "merged")
        self.assertEqual(self.table("skins").by_crc, {1: 100})

    def test_a_pack_name_row_held_as_a_crc_row_counts(self):
        name = "Puppet/new.bfres"
        self.put_table(PACK, {}, {name: 77})
        self.put_table("skins", {rstb.Rstb.crc(name): 77})
        self.assertEqual(mt.missing_pack_entries(self.table("skins"), self.table(PACK)), 0)
        self.put_table("skins", {rstb.Rstb.crc(name): 76})
        self.assertEqual(mt.missing_pack_entries(self.table("skins"), self.table(PACK)), 1)

    def test_with_another_mods_table_too_the_winner_is_still_merged(self):
        # (holding the pack's entries is not enough: the third mod's entries belong in the one table too)
        self.put_table(PACK, {1: 100})
        self.put_table("aaa-textures", {7: 7})
        self.put_table("skins", {1: 100})
        self.run_merge()
        self.assertEqual(self.table("skins").by_crc, {1: 100, 7: 7})
        self.assertEqual(self.mark_rule("skins"), "merged")

    def test_a_mod_under_contents_that_wins_is_merged_like_any_mod(self):
        # Player report 259: Classic Link Voices (mods/contents/<title>/, its own size table) won the table, was never
        # rewritten, and the game read it without the other mods' sizes: an abort at every boot.
        title_mod = "contents/0400486024DEC000/skins"   # (by name above coop-puppet: its table wins)
        self.put_table(PACK, {1: 100, 2: 50})
        own = read(self.put_table(title_mod, {1: 90, 4: 4}))
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(self.table(title_mod).by_crc, {1: 100, 2: 50, 4: 4})
        self.assertEqual(self.mark_rule(title_mod), "merged")
        self.assertEqual(read(self.path(title_mod, mt.KEEP_DIR, NAME)), own)
        self.run_merge()
        self.assertIn("already", self.lines[0])
        mt.undo(self.mods, self.say)
        self.assertEqual(read(self.path(title_mod, "romfs", "System", "Resource", NAME)), own)
        self.assertIsNone(self.mark_rule(title_mod))
        # One that holds the pack's entries already is left as it is, as any mod.
        self.put_table(title_mod, {1: 100, 2: 50, 3: 3})
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(self.mark_rule(title_mod), "contains")
        # Without the pack's table, two other mods' tables are merged too (nothing of the pack's in either).
        os.remove(self.path(PACK, "romfs", "System", "Resource", NAME))
        self.put_table("aaa-textures", {7: 7})
        self.put_table(title_mod, {1: 1})
        self.assertEqual(self.run_merge(), 0)
        self.assertEqual(self.table(title_mod).by_crc, {1: 1, 7: 7})

    def test_the_mods_list_decides_the_winner_and_mods_switched_off_are_left_out(self):
        self.put_table(PACK, {1: 100})
        self.put_table("zz-site", {2: 5})
        self.put_table("aa-site", {3: 6})
        self.put_table("off-mod", {9: 9})
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 1\non aa-site\non zz-site\noff off-mod\n")
        self.run_merge()
        self.assertEqual(self.table("aa-site").by_crc, {1: 100, 2: 5, 3: 6})  # the first row wins, 9 left out
        self.assertEqual(self.table("zz-site").by_crc, {2: 5})
        self.assertEqual([n for n, _ in mt.load_order(self.mods)], [PACK, "zz-site", "aa-site"])

    def test_with_a_list_an_unlisted_mod_wins_only_over_the_pack_by_name(self):
        # Patch 0011: listed rows above everything; the unlisted mods and the pack by _stricmp name, the last wins.
        self.put_table(PACK, {1: 100})
        self.put_table("Aaa-Site", {2: 5})  # unlisted, sorts under contents/...: the pack's table is read
        self.put_table("listed-off", {9: 9})
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 1\noff listed-off\n")
        self.assertEqual(self.run_merge(), 0)
        # (report 101: the pack's table takes the unlisted mod's entries, never the switched-off one's)
        self.assertIn("the puppet pack's", self.lines[0])
        self.assertEqual(self.table(PACK).by_crc, {1: 100, 2: 5})
        self.assertEqual(self.table("Aaa-Site").by_crc, {2: 5})
        self.assertEqual([n for n, _ in mt.load_order(self.mods)], ["Aaa-Site", PACK])
        self.put_table("Zzz-Site", {3: 6})  # unlisted, sorts over contents/...: it wins and gets the others
        self.run_merge()
        self.assertEqual(self.table("Zzz-Site").by_crc, {1: 100, 2: 5, 3: 6})
        self.assertEqual(self.table("Aaa-Site").by_crc, {2: 5})
        self.put_table("bbb-listed", {4: 4})  # a listed row is above every unlisted one
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 1\non bbb-listed\noff listed-off\n")
        self.run_merge()
        self.assertEqual(self.table("bbb-listed").by_crc, {1: 100, 2: 5, 3: 6, 4: 4})
        self.assertEqual([n for n, _ in mt.load_order(self.mods)][-1], "bbb-listed")

    def test_a_default_off_list_leaves_out_every_mod_it_does_not_name(self):
        # Original game (report 266, hos_coop_mods.h "default off"): a mod put in after the list was written is off,
        # as the game leaves it out; the pack too when not named on.
        self.put_table(PACK, {1: 100})
        self.put_table("late-seed", {2: 5})
        self.put_table("named-on", {3: 6})
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 1\r\ndefault off\r\non named-on\r\n")
        self.assertEqual([n for n, _ in mt.load_order(self.mods)], ["named-on"])
        self.assertEqual(mt.read_list_default(self.mods), ([("named-on", False)], True))
        self.assertEqual(mt.read_list(self.mods), [("named-on", False)])
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 1\non named-on\n")   # without the line: the unnamed ones are on, as before
        self.assertEqual(sorted(n for n, _ in mt.load_order(self.mods)), sorted([PACK, "late-seed", "named-on"]))

    def test_folders_with_a_leading_dash_or_underscore_are_no_mods(self):
        self.put_table(PACK, {1: 100})
        self.put_table("-old-site", {2: 5})
        self.put_table("_backup", {3: 5})
        self.assertEqual([n for n, _ in mt.load_order(self.mods)], [PACK])

    def test_an_uncompressed_table_stays_uncompressed(self):
        self.put_table(PACK, {1: 100})
        self.put_table("site", {2: 5}, compress=False)
        self.run_merge()
        raw = read(self.path("site", "romfs", "System", "Resource", NAME))
        self.assertEqual(rstb.Rstb.loads(raw).by_crc, {1: 100, 2: 5})

    def test_a_table_with_the_games_dictionary_is_merged_with_it(self):
        rnd = random.Random(2)
        samples = [bytes(rnd.randrange(8) for _ in range(200)) for _ in range(300)]
        d = zstandard.train_dictionary(2048, samples, dict_id=7)
        os.makedirs(os.path.join(self.romfs, "Pack"))
        with open(os.path.join(self.romfs, "Pack", "ZsDic.pack.zs"), "wb") as f:
            f.write(zstandard.ZstdCompressor().compress(sarc.write({"zs.zsdic": d.as_bytes()})))
        self.put_table(PACK, {1: 100}, dict_data=d)
        self.put_table("site", {2: 5}, dict_data=d)
        self.run_merge()
        raw = read(self.path("site", "romfs", "System", "Resource", NAME))
        self.assertEqual(mt.zs.frame_dict_id(raw), 7)  # compressed as the site's was
        self.assertEqual(self.table("site").by_crc, {1: 100, 2: 5})

    def test_a_table_that_needs_a_missing_dictionary_is_said_and_left_as_it_was(self):
        rnd = random.Random(3)
        d = zstandard.train_dictionary(2048, [bytes(rnd.randrange(8) for _ in range(200)) for _ in range(300)],
                                       dict_id=7)
        self.put_table(PACK, {1: 100})
        site = self.put_table("site", {2: 5}, dict_data=d)
        before = read(site)
        self.assertEqual(self.run_merge(), 1)
        self.assertIn("could not be merged", self.lines[0])
        self.assertEqual(read(site), before)
        self.assertFalse(os.path.exists(self.path("site", mt.KEEP_DIR, mt.STAMP)))
        self.assertFalse(os.path.exists(self.path("site", mt.MARK)))

    def test_the_stamp_line_is_the_modules(self):
        p = os.path.join(self.mods, "a b", "t.bin")
        os.makedirs(os.path.dirname(p))
        with open(p, "wb") as f:
            f.write(b"abc")
        self.assertEqual(mt.file_line(self.mods, p), "a b/t.bin 3 e71fa2190541574b")  # FNV-1a 64 of "abc"
        self.assertEqual(mt.fnv64(p), ("e71fa2190541574b", 3))


AI = "ActorInfo.Product.111.rstbl.byml.zs"
GAI = "GameActorInfo.Product.111.rstbl.byml.zs"
AI_KEY = "RSDB/ActorInfo.Product.111.rstbl.byml"
SEED = "randomizer-adzfteae2h83-s89dbd7a9"


MALS = "Mals/USen.Product.110.sarc.zs"
TITLE = "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs"
SCREENS = "RSDB/UIScreen.Product.111.rstbl.byml.zs"   # (one of co-op's Characters tab's files)


class ParkMenusTest(unittest.TestCase):
    """Row 235: park_menus and main() move the pack's menu files aside when the merge cannot check the size table the
    game reads (no zstandard, a step that raised), but only where another mod that is on could win a pack table."""

    REL = "Mals/EUen.Nin_NX_NVN.zs"

    def make(self, other):
        d = tempfile.mkdtemp(prefix="merge_tables_park_")
        self.addCleanup(shutil.rmtree, d, True)
        self.mods = os.path.join(d, "mods")
        self.pack = os.path.join(self.mods, *PACK.split("/"))
        self.put(os.path.join(self.pack, "romfs", *self.REL.split("/")), b"x")
        self.put(os.path.join(self.pack, *mt.RESOURCE, NAME), b"pack table")
        self.put(os.path.join(self.pack, "coop_puppet.json"), b'{"title_menu": ["%s"], "pause_menu": []}' % self.REL.encode())
        if other:
            self.put(os.path.join(self.mods, "OoT Character Select", *mt.RESOURCE, NAME), b"cs table")
        return self.mods

    @staticmethod
    def put(path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    def in_play(self):
        return os.path.isfile(os.path.join(self.pack, "romfs", *self.REL.split("/")))

    def kept(self):
        return os.path.isfile(os.path.join(self.pack, mt.MENU_KEEP, *self.REL.split("/")))

    def test_another_mods_table_could_win_so_unchecked_park_moves_the_files(self):
        mods = self.make(other=True)
        lines = []
        self.assertEqual(mt.park_menus(mods, lines.append), 1)
        self.assertTrue(self.kept() and not self.in_play())
        self.assertEqual(len(lines), 1)
        self.assertEqual(mt.park_menus(mods, lines.append), 0)   # (nothing left to move)

    def test_the_pack_alone_keeps_its_menus(self):
        mods = self.make(other=False)
        self.assertEqual(mt.park_menus(mods, lambda _l: None), 0)
        self.assertTrue(self.in_play() and not self.kept())

    def test_a_mod_that_is_off_does_not_count(self):
        mods = self.make(other=True)
        self.put(os.path.join(mods, "coop-mods.list"), b"coop-mods 1\noff OoT Character Select\n")
        self.assertEqual(mt.park_menus(mods, lambda _l: None), 0)
        self.assertTrue(self.in_play())

    def run_main(self, other, boom):
        mods = self.make(other)
        real = mt.merge_rows
        mt.merge_rows = boom
        try:
            import contextlib, io
            with contextlib.redirect_stdout(io.StringIO()) as out:
                code = mt.main(["--mods", mods])
        finally:
            mt.merge_rows = real
        return code, out.getvalue()

    def test_main_parks_when_zstandard_is_missing(self):
        def boom(*_a):
            raise SystemExit(1)
        code, said = self.run_main(True, boom)
        self.assertEqual(code, 1)
        self.assertIn("zstandard", said)
        self.assertTrue(self.kept() and not self.in_play())

    def test_main_parks_when_a_merge_step_raises(self):
        def boom(*_a):
            raise KeyError("a table row")
        code, said = self.run_main(True, boom)
        self.assertEqual(code, 1)
        self.assertIn("not merged", said)
        self.assertTrue(self.kept() and not self.in_play())

    def test_main_with_the_pack_alone_parks_nothing_when_a_step_raises(self):
        def boom(*_a):
            raise KeyError("a table row")
        code, _said = self.run_main(False, boom)
        self.assertEqual(code, 1)
        self.assertTrue(self.in_play() and not self.kept())


@unittest.skipIf(zstandard is None, "needs zstandard")
class MenuGuardTest(unittest.TestCase):
    """guard_menus: the pack's menu files (--title-rows / --pause-tab) only where the size table the game reads holds
    their sizes (0.11.5's test: an old pack left as contents/.../coop-puppet.old-b26 won the table and the language
    archive failed to load at boot: a crash)."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="merge_tables_menus_")
        self.mods = os.path.join(self.dir, "mods")
        self.romfs = os.path.join(self.dir, "romfs")
        self.lines = []
        self.pack = os.path.join(self.mods, *PACK.split("/"))
        self.mals = 2373416 + 336         # the game's entry (made up near the real one)
        self.grown = self.mals + 1048 + 0x1000
        self.put_table(PACK, {self.crc(MALS): self.grown, self.crc(TITLE): 230000, 7: 70})
        for rel in (MALS, TITLE):
            self.put(os.path.join(self.pack, "romfs", *rel.split("/")), b"pack " + rel.encode())
        self.put(os.path.join(self.pack, "coop_puppet.json"),
                 b'{"title_rows": true, "title_menu": ["%s", "%s"], "pause_menu": ["%s"]}'
                 % (TITLE.encode(), MALS.encode(), MALS.encode()))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    @staticmethod
    def crc(rel):
        return rstb.Rstb.crc(rel[:-3])

    @staticmethod
    def put(path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    def put_table(self, mod, by_crc):
        raw = zstandard.ZstdCompressor(write_content_size=True).compress(rstb.Rstb(by_crc=by_crc).dumps())
        self.put(os.path.join(self.mods, *mod.split("/"), "romfs", "System", "Resource", NAME), raw)

    def guard(self):
        self.lines = []
        return mt.guard_menus(self.mods, self.romfs, self.lines.append)

    def in_play(self, rel):
        return os.path.isfile(os.path.join(self.pack, "romfs", *rel.split("/")))

    def kept(self, rel):
        return os.path.isfile(os.path.join(self.pack, mt.MENU_KEEP, *rel.split("/")))

    def test_the_menu_files_are_the_manifests_menu_lists(self):
        self.assertEqual(mt.menu_files(self.pack), [TITLE, MALS])
        os.remove(os.path.join(self.pack, "coop_puppet.json"))
        self.assertEqual(mt.menu_files(self.pack), [])

    def charsel_pack(self, menu_keys=True):
        """The pack with co-op's own Characters tab (charsel_native: one of its files, the screens table)."""
        self.put(os.path.join(self.pack, "romfs", *SCREENS.split("/")), b"pack screens")
        keys = b', "menu_keys": ["title_menu", "pause_menu", "charsel_native"]' if menu_keys else b""
        self.put(os.path.join(self.pack, "coop_puppet.json"),
                 b'{"title_menu": ["%s"], "pause_menu": ["%s"], "charsel_native": ["%s"]%s}'
                 % (TITLE.encode(), MALS.encode(), SCREENS.encode(), keys))
        self.put_table(PACK, {self.crc(MALS): self.grown, self.crc(TITLE): 230000, self.crc(SCREENS): 9000, 7: 70})

    def test_the_menu_files_are_the_lists_the_pack_names_in_menu_keys(self):
        # (charsel-native-0119: co-op's Characters tab's files go with the menus; one list, ui_pack.MENU_KEYS, written
        # by build.py into the pack's json; a pack without "menu_keys" has the two old lists)
        self.assertEqual(list(ui_pack.MENU_KEYS), ["title_menu", "pause_menu", "charsel_native"])
        self.charsel_pack()
        self.assertEqual(mt.menu_files(self.pack), [TITLE, MALS, SCREENS])
        self.charsel_pack(menu_keys=False)
        self.assertEqual(mt.menu_files(self.pack), [TITLE, MALS])

    def test_a_table_holding_the_menus_but_not_the_characters_tab_keeps_them_all_aside(self):
        # All or nothing: another mod's table wins with the menus' sizes but not the Characters tab's screens table
        # (a seed's own table): every one of them aside, the game keeps its own menus and screens.
        self.charsel_pack()
        self.put_table("oot-remake-randomizer", {self.crc(MALS): self.grown, self.crc(TITLE): 230000, 7: 70})
        self.guard()
        self.assertFalse(self.in_play(MALS) or self.in_play(TITLE) or self.in_play(SCREENS))
        self.assertTrue(self.kept(MALS) and self.kept(TITLE) and self.kept(SCREENS))

    def test_the_characters_tab_is_parked_exactly_while_its_files_are_kept_aside(self):
        # (MsgNotFound on slot 0: the kit's code-patch rule, kit-paths.ps1 Sync-KitCharselNativePatch and build.sh
        # charsel_native_sync, leaves the tab's code out while this says parked, so code and files never split)
        self.assertFalse(mt.charsel_native_parked(self.pack))   # (no charsel_native list)
        self.charsel_pack()
        self.assertFalse(mt.charsel_native_parked(self.pack))   # (in romfs: in play)
        self.put_table("oot-remake-randomizer", {self.crc(MALS): self.grown, self.crc(TITLE): 230000, 7: 70})
        self.guard()
        self.assertTrue(self.kept(SCREENS))
        self.assertTrue(mt.charsel_native_parked(self.pack))
        shutil.rmtree(os.path.join(self.mods, "oot-remake-randomizer"))
        self.guard()   # (put back: the code goes in again at the next need check)
        self.assertTrue(self.in_play(SCREENS))
        self.assertFalse(mt.charsel_native_parked(self.pack))
        # A pack made since (its file in romfs again) beside an old kept copy: in play, not parked.
        self.put(os.path.join(self.pack, mt.MENU_KEEP, *SCREENS.split("/")), b"old screens")
        self.assertFalse(mt.charsel_native_parked(self.pack))
        # The tab left out ("charsel_native": []) or an unreadable manifest: nothing parked.
        self.put(os.path.join(self.pack, "coop_puppet.json"), b'{"title_menu": [], "charsel_native": []}')
        self.assertFalse(mt.charsel_native_parked(self.pack))
        self.put(os.path.join(self.pack, "coop_puppet.json"), b"{")
        self.assertFalse(mt.charsel_native_parked(self.pack))

    def test_the_pack_reading_its_own_table_keeps_its_menus(self):
        self.assertEqual(self.guard(), 0)
        self.assertTrue(self.in_play(MALS) and self.in_play(TITLE))
        self.assertEqual(self.lines, [])
        self.assertFalse(os.path.isdir(os.path.join(self.pack, mt.MENU_KEEP)))

    def test_an_old_pack_left_in_contents_takes_the_menus_out_and_they_come_back_without_it(self):
        old = PACK + ".old-b26"   # (sorts after the pack: its table wins; the guard alone, no merge)
        self.put_table(old, {self.crc(MALS): self.mals, 7: 70})
        self.assertEqual(self.guard(), 0)
        self.assertFalse(self.in_play(MALS) or self.in_play(TITLE))
        self.assertTrue(self.kept(MALS) and self.kept(TITLE))
        self.assertEqual(len(self.lines), 1)
        self.assertIn("coop-puppet.old-b26", self.lines[0])
        self.assertIn("Multiplayer tab are off", self.lines[0])
        self.guard()   # (every start while it wins: said again, nothing lost)
        self.assertTrue(self.kept(MALS) and self.kept(TITLE))
        shutil.rmtree(os.path.join(self.mods, *old.split("/")))
        self.assertEqual(self.guard(), 0)
        self.assertTrue(self.in_play(MALS) and self.in_play(TITLE))
        self.assertFalse(os.path.isdir(os.path.join(self.pack, mt.MENU_KEEP)))
        self.assertIn("are back (2 files)", self.lines[0])
        with open(os.path.join(self.pack, "romfs", *MALS.split("/")), "rb") as f:
            self.assertEqual(f.read(), b"pack " + MALS.encode())

    def test_a_winning_table_without_one_entry_counts_as_lacking_it(self):
        self.put_table("zz-site-seed", {self.crc(TITLE): 230000})   # (no Mals entry: the game's own would be used)
        self.guard()
        self.assertFalse(self.in_play(MALS))
        self.assertIn("1 of the puppet pack's menu files", self.lines[0])

    def test_a_winning_table_that_holds_the_sizes_keeps_the_menus(self):
        self.put_table("zz-site-seed", {self.crc(MALS): self.grown + 5, self.crc(TITLE): 230000})   # (merged)
        self.assertEqual(self.guard(), 0)
        self.assertTrue(self.in_play(MALS) and self.in_play(TITLE))
        self.assertEqual(self.lines, [])

    def test_a_pack_made_again_while_its_menus_were_out_keeps_its_new_files(self):
        self.put_table(PACK + ".old-b26", {self.crc(MALS): self.mals})
        self.guard()
        self.put(os.path.join(self.pack, "romfs", *MALS.split("/")), b"new pack")   # (build.py ran again)
        shutil.rmtree(os.path.join(self.mods, *(PACK + ".old-b26").split("/")))
        self.guard()
        with open(os.path.join(self.pack, "romfs", *MALS.split("/")), "rb") as f:
            self.assertEqual(f.read(), b"new pack")
        self.assertTrue(self.in_play(TITLE))
        self.assertFalse(os.path.isdir(os.path.join(self.pack, mt.MENU_KEEP)))

    def test_a_mod_switched_off_in_the_mods_list_does_not_take_the_menus_out(self):
        self.put_table("zz-site-seed", {self.crc(MALS): self.mals})
        self.put(os.path.join(self.mods, mt.LIST_FILE), b"coop-mods 1\noff zz-site-seed\n")
        self.guard()
        self.assertTrue(self.in_play(MALS) and self.in_play(TITLE))

    SCREENS = "RSDB/UIScreen.Product.111.rstbl.byml.zs"

    def screens(self, mod, archives):
        """A UIScreen table a mod ships (Title_00's ArchiveCategory as given) beside its romfs size table."""
        raw = zstandard.ZstdCompressor(write_content_size=True).compress(
            byml.dumps([{"ArchiveCategory": ["Common"], "Name": "Other_00"},
                        {"ArchiveCategory": archives, "Name": "Title_00"}]))
        self.put(os.path.join(self.mods, *mod.split("/"), "romfs", *self.SCREENS.split("/")), raw)

    def with_screens_pack(self):
        """The pack with a UIScreen table in its menu files (CoopMenu_00 's row)."""
        self.put(os.path.join(self.pack, "coop_puppet.json"),
                 b'{"title_rows": true, "title_menu": ["%s", "%s", "%s"], "pause_menu": ["%s"]}'
                 % (TITLE.encode(), MALS.encode(), self.SCREENS.encode(), MALS.encode()))
        self.screens(PACK, ["Title", "Common"])

    def test_a_pack_with_a_screens_table_keeps_its_title_archive_whatever_screens_table_wins(self):
        """The title has no part from Common (the Host toggle is gone), so no UIScreen table parks the Title archive;
        the pack's UIScreen row for CoopMenu_00 (pause menu) must not park it either."""
        self.with_screens_pack()
        self.put_table("zz-site-seed", {self.crc(MALS): self.grown + 5, self.crc(TITLE): 230000})
        for archives in (["Title"], ["Title", "Common"]):
            self.screens("zz-site-seed", archives)
            self.assertEqual(self.guard(), 0)
            self.assertTrue(self.in_play(TITLE) and self.in_play(MALS))
            self.assertFalse(self.kept(TITLE))
            self.assertEqual(self.lines, [])
        os.remove(os.path.join(self.mods, "zz-site-seed", "romfs", *self.SCREENS.split("/")))   # (no screens table at all)
        self.assertEqual(self.guard(), 0)
        self.assertTrue(self.in_play(TITLE) and not self.kept(TITLE))

    def test_every_start_runs_the_guard_after_the_merge(self):
        # The guard sees the tables as the merge left them: an old copy left under contents/ that wins the table gets
        # the pack's entries (player report 259: merged like any mod), so the menus stay.
        old = PACK + ".old-b26"
        self.put_table(old, {self.crc(MALS): self.mals})
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            mt.main(["--mods", self.mods, "--romfs", self.romfs])
        self.assertTrue(self.in_play(MALS) and self.in_play(TITLE))
        self.assertIn("Size tables: the game reads contents/0400486024DEC000/coop-puppet.old-b26's", out.getvalue())
        # A winning table the merge cannot read: the guard takes the menus out.
        self.put(os.path.join(self.mods, *old.split("/"), "romfs", "System", "Resource", NAME), b"not a table")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(mt.main(["--mods", self.mods, "--romfs", self.romfs]), 1)
        self.assertFalse(self.in_play(MALS))
        self.assertIn("Multiplayer tab are off", out.getvalue())


@unittest.skipIf(zstandard is None, "needs zstandard")
class MergeRowsTest(unittest.TestCase):
    """Player report 144/145: a seed with its own RSDB/ActorInfo hid the puppet pack's rows (create refused)."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="merge_rows_test_")
        self.mods = os.path.join(self.dir, "mods")
        self.romfs = os.path.join(self.dir, "romfs")
        self.lines = []
        os.makedirs(self.mods)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def say(self, line):
        self.lines.append(line)

    def path(self, mod, *rest):
        return os.path.join(self.mods, *mod.split("/"), *rest)

    def put_rows(self, mod, rows, fname=AI):
        p = self.path(mod, "romfs", "RSDB", fname)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(zstandard.ZstdCompressor(write_content_size=True).compress(byml.dumps(rows)))
        return p

    def rows(self, mod, fname=AI):
        with open(self.path(mod, "romfs", "RSDB", fname), "rb") as f:
            return byml.loads(mt.Codec(self.romfs).unpack(f.read()))

    def put_sizes(self, mod, ai_size):
        t = rstb.Rstb(by_crc={rstb.Rstb.crc(AI_KEY): ai_size, 7: 70})
        p = self.path(mod, "romfs", "System", "Resource", NAME)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(zstandard.ZstdCompressor(write_content_size=True).compress(t.dumps()))
        return p

    def sizes(self, mod):
        with open(self.path(mod, "romfs", "System", "Resource", NAME), "rb") as f:
            return mt.Codec(self.romfs).load(f.read())

    def table_bytes(self, mod):
        return read(self.path(mod, "romfs", "RSDB", AI)), read(self.path(mod, "romfs", "System", "Resource", NAME))

    def run_all(self):
        self.lines = []
        failed, fit = mt.merge_rows(self.mods, self.romfs, self.say)
        return failed + mt.merge_mods(self.mods, self.romfs, self.say, fit), fit

    def game_rows(self):
        return [{"__RowId": "Npc_Mido", "Heap": 1}, {"__RowId": "Player_Child", "Heap": 2}]

    def pack_rows(self):
        return self.game_rows() + [{"__RowId": "CoopPuppet_Child", "Heap": 3}, {"__RowId": "CoopShot_Seed", "Heap": 4}]

    def seed_with_own_tables(self):
        self.put_rows(PACK, self.pack_rows())
        self.put_sizes(PACK, 5000)
        own = self.put_rows(SEED, [{"__RowId": "Npc_Mido", "Heap": 9}, {"__RowId": "Player_Child", "Heap": 2},
                                   {"__RowId": "Rando_Item", "Heap": 5}])
        self.put_sizes(SEED, 4000)
        with open(self.path(SEED, mt.SEED_MARK), "w", encoding="utf-8", newline="\n") as f:
            f.write("default\n")  # (a seed the game built)
        return read(own)

    def test_a_seed_with_its_own_actor_table_gets_the_pack_rows_and_a_size_that_fits(self):
        own = self.seed_with_own_tables()
        failed, fit = self.run_all()
        self.assertEqual(failed, 0)
        ids = [r["__RowId"] for r in self.rows(SEED)]
        self.assertEqual(ids, sorted(ids, key=lambda s: s.encode("utf-8")))
        self.assertEqual(set(ids), {"Npc_Mido", "Player_Child", "Rando_Item", "CoopPuppet_Child", "CoopShot_Seed"})
        self.assertEqual(next(r for r in self.rows(SEED) if r["__RowId"] == "Npc_Mido")["Heap"], 9)  # the seed's own
        self.assertEqual(read(self.path(SEED, mt.ROWS_KEEP, AI)), own)
        # The seed's own size table (the one the game reads) holds the merged file's size; the pack's is untouched.
        self.assertGreater(fit[AI_KEY], 5000)
        self.assertGreaterEqual(self.sizes(SEED).get(AI_KEY), fit[AI_KEY])
        self.assertEqual(self.sizes(PACK).get(AI_KEY), 5000)
        # The module's mark (coop_rando RowsMergedByKit): the table written and the pack's, by FNV-1a 64.
        mark = read(self.path(SEED, mt.ROWS_MARK)).decode("utf-8")
        ai = mt.fnv64(self.path(SEED, "romfs", "RSDB", AI))[0]
        pack = mt.fnv64(self.path(PACK, "romfs", "RSDB", AI))[0]
        self.assertEqual(mark, f"coop-rows 1\ntable {AI} {ai} {pack}\n")
        # The next start: nothing written.
        before = self.table_bytes(SEED)
        self.assertEqual(self.run_all()[0], 0)
        self.assertTrue(any("already" in line for line in self.lines))
        self.assertEqual(self.table_bytes(SEED), before)
        # Undo puts the seed's own actor table back.
        mt.undo(self.mods, self.say)
        self.assertEqual(read(self.path(SEED, "romfs", "RSDB", AI)), own)
        self.assertFalse(os.path.exists(self.path(SEED, mt.ROWS_MARK)))
        self.assertFalse(os.path.exists(self.path(SEED, mt.ROWS_KEEP)))

    def test_a_pack_made_again_is_merged_again_from_the_kept_copy(self):
        self.seed_with_own_tables()
        self.run_all()
        self.put_rows(PACK, self.game_rows() + [{"__RowId": "CoopPuppet_Adult", "Heap": 7}])
        self.assertEqual(self.run_all()[0], 0)
        ids = {r["__RowId"] for r in self.rows(SEED)}
        self.assertIn("CoopPuppet_Adult", ids)
        self.assertNotIn("CoopPuppet_Child", ids)  # (from the kept copy, not the merged one)

    def test_the_size_a_merge_needs_never_counts_what_an_earlier_merge_raised(self):
        # The seed's size table is raised to the merged actor table's size (fit_in_place) and kept as it shipped. A
        # second merge (the pack made again) sized itself from the raised entry: need = E + d + d' for E + d'.
        own = self.seed_with_own_tables()
        own_sizes = read(self.path(SEED, "romfs", "System", "Resource", NAME))
        codec = mt.Codec(self.romfs)

        def want():
            """The size the merge needs, from the files as they shipped: the pack's table and the seed's kept ones."""
            new = len(codec.unpack(read(self.path(SEED, "romfs", "RSDB", AI))))
            pack = len(codec.unpack(read(self.path(PACK, "romfs", "RSDB", AI))))
            kept = len(codec.unpack(read(self.path(SEED, mt.ROWS_KEEP, AI))))
            return max(5000 + max(0, new - pack), 4000 + max(0, new - kept))

        fit1 = self.run_all()[1]
        self.assertEqual(fit1[AI_KEY], want())
        self.assertEqual(self.sizes(SEED).get(AI_KEY), fit1[AI_KEY])
        written = read(self.path(SEED, "romfs", "System", "Resource", NAME))
        self.run_all()
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), written)   # a second start: same
        self.put_rows(PACK, self.pack_rows() + [{"__RowId": "CoopPuppet_Adult", "Heap": 7}])   # (a longer pack)
        fit2 = self.run_all()[1]
        self.assertEqual(fit2[AI_KEY], want())
        self.assertEqual(self.sizes(SEED).get(AI_KEY), want())
        self.assertEqual(read(self.path(SEED, mt.KEEP_DIR, NAME)), own_sizes)   # (the seed's own, as it shipped)
        mt.undo(self.mods, self.say)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), own_sizes)
        self.assertFalse(os.path.exists(self.path(SEED, mt.KEEP_DIR)))
        self.assertEqual(read(self.path(SEED, "romfs", "RSDB", AI)), own)

    def test_a_size_table_that_needs_less_now_goes_back_down_to_the_one_the_mod_shipped(self):
        # fit_in_place raises from the table as it shipped; with nothing to raise now the earlier start's raise goes.
        own = self.seed_with_own_tables()
        own_sizes = read(self.path(SEED, "romfs", "System", "Resource", NAME))
        self.assertEqual(self.run_all()[0], 0)
        self.assertGreater(self.sizes(SEED).get(AI_KEY), 4000)
        self.assertTrue(os.path.isfile(self.path(SEED, mt.KEEP_DIR, mt.STAMP)))
        self.put_rows(PACK, self.game_rows())   # (a pack without puppets: the merged table grows no more)
        self.put_sizes(PACK, 3000)
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), own_sizes)
        self.assertFalse(os.path.exists(self.path(SEED, mt.KEEP_DIR)))
        self.assertTrue(any("needs no raise now" in l for l in self.lines), self.lines)
        self.assertEqual(self.run_all()[0], 0)   # (the next start: nothing to do, nothing written)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), own_sizes)
        self.assertFalse(os.path.exists(self.path(SEED, mt.KEEP_DIR)))

    def test_a_size_table_goes_back_down_also_when_the_fit_is_empty(self):
        # The pack's actor table is gone: the merged actor tables need nothing from the size table any more (fit {}).
        own = self.seed_with_own_tables()
        own_sizes = read(self.path(SEED, "romfs", "System", "Resource", NAME))
        self.assertEqual(self.run_all()[0], 0)
        self.assertGreater(self.sizes(SEED).get(AI_KEY), 4000)
        os.remove(self.path(PACK, "romfs", "RSDB", AI))
        failed, fit = self.run_all()
        self.assertEqual((failed, fit), (0, {}))
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), own_sizes)
        self.assertFalse(os.path.exists(self.path(SEED, mt.KEEP_DIR)))
        self.assertEqual(self.run_all()[0], 0)   # (the next start: nothing to do)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), own_sizes)

    def test_a_new_size_table_that_holds_the_fit_leaves_no_kept_copy_of_the_old_one(self):
        self.seed_with_own_tables()
        self.assertEqual(self.run_all()[0], 0)
        self.assertTrue(os.path.isfile(self.path(SEED, mt.KEEP_DIR, NAME)))
        new_sizes = read(self.put_sizes(SEED, 90000))   # (a new seed's table: larger than the fit needs)
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), new_sizes)
        self.assertFalse(os.path.exists(self.path(SEED, mt.KEEP_DIR, NAME)))
        self.assertFalse(os.path.exists(self.path(SEED, mt.KEEP_DIR, mt.STAMP)))
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), new_sizes)

    # A re-raise writes its stamp before the table. Stopped between the two, the stamp names a table that is not
    # there and the table is the earlier raise: it must not be taken as the mod's own table (restamp).
    def stop_a_re_raise(self):
        """A seed with its own tables, raised once; then a longer pack makes a second raise that stops after its
        stamp. Returns (the seed's size table as it shipped, as the first raise left it)."""
        self.seed_with_own_tables()
        shipped = read(self.path(SEED, "romfs", "System", "Resource", NAME))
        self.assertEqual(self.run_all()[0], 0)
        raised = read(self.path(SEED, "romfs", "System", "Resource", NAME))
        self.assertNotEqual(raised, shipped)
        self.put_rows(PACK, self.pack_rows() + [{"__RowId": "CoopPuppet_Adult", "Heap": 7}])   # (a longer pack, its
        self.put_sizes(PACK, 9000)   # entry larger: the fit grows, a second raise)
        with stopped_at(self.path(SEED, "romfs", "System", "Resource", NAME)) as seen:
            self.assertEqual(self.run_all()[0], 1)
        self.assertTrue(seen)   # (the stamp was written, the table not)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), raised)
        return shipped, raised

    def test_a_re_raise_stopped_after_its_stamp_leaves_undo_the_shipped_table(self):
        shipped, _ = self.stop_a_re_raise()
        mt.undo(self.mods, self.say)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), shipped)
        self.assertFalse(os.path.exists(self.path(SEED, mt.KEEP_DIR)))

    def test_a_re_raise_stopped_after_its_stamp_then_a_shorter_pack_gives_the_shipped_table_back(self):
        shipped, _ = self.stop_a_re_raise()
        self.put_rows(PACK, self.game_rows())   # (a pack without puppets: the fit is empty now)
        self.put_sizes(PACK, 3000)
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), shipped)
        self.assertFalse(os.path.exists(self.path(SEED, mt.KEEP_DIR)))

    def test_a_re_raise_stopped_after_its_stamp_then_a_shorter_pack_raises_from_the_shipped_table(self):
        shipped, raised = self.stop_a_re_raise()
        self.put_rows(PACK, self.pack_rows())   # (the first pack again: the first raise's fit, not the second's)
        self.put_sizes(PACK, 5000)
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), raised)
        self.assertEqual(read(self.path(SEED, mt.KEEP_DIR, NAME)), shipped)   # (still the shipped one, not the raise)
        mt.undo(self.mods, self.say)
        self.assertEqual(read(self.path(SEED, "romfs", "System", "Resource", NAME)), shipped)

    def test_a_re_merge_of_actor_rows_stopped_after_its_stamp_leaves_undo_the_seeds_own_table(self):
        own = self.seed_with_own_tables()
        self.assertEqual(self.run_all()[0], 0)
        merged = read(self.path(SEED, "romfs", "RSDB", AI))
        self.assertNotEqual(merged, own)
        self.put_rows(PACK, self.pack_rows() + [{"__RowId": "CoopPuppet_Adult", "Heap": 7}])
        with stopped_at(self.path(SEED, "romfs", "RSDB", AI)) as seen:
            self.assertGreaterEqual(self.run_all()[0], 1)
        self.assertTrue(seen)
        self.assertEqual(read(self.path(SEED, "romfs", "RSDB", AI)), merged)
        self.assertEqual(self.run_all()[0], 0)   # (the next start finishes it, from the seed's own rows)
        self.assertEqual(sorted(r["__RowId"] for r in self.rows(SEED)),
                         sorted(["Npc_Mido", "Player_Child", "Rando_Item", "CoopPuppet_Child", "CoopShot_Seed",
                                 "CoopPuppet_Adult"]))
        mt.undo(self.mods, self.say)
        self.assertEqual(read(self.path(SEED, "romfs", "RSDB", AI)), own)

    def test_a_stamp_names_a_table_input_by_its_mod_folder(self):
        self.assertEqual(mt.mod_of_table(os.path.join("a", "b", "mods", "m x", *mt.RESOURCE, NAME)),
                         os.path.join("a", "b", "mods", "m x"))
        with self.assertRaises(ValueError):
            mt.mod_of_table(os.path.join("a", NAME))

    def test_a_new_seed_put_over_the_merged_one_is_kept_as_the_new_original(self):
        self.seed_with_own_tables()
        self.run_all()
        new_raw = read(self.put_rows(SEED, [{"__RowId": "Npc_Mido", "Heap": 11}]))
        self.put_sizes(SEED, 4000)
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(self.path(SEED, mt.ROWS_KEEP, AI)), new_raw)
        self.assertIn("CoopPuppet_Child", {r["__RowId"] for r in self.rows(SEED)})
        self.assertGreater(self.sizes(SEED).get(AI_KEY), 4000)

    def test_every_table_the_pack_ships_is_merged(self):
        self.seed_with_own_tables()
        self.put_rows(PACK, [{"__RowId": "Player_Child"}, {"__RowId": "CoopPuppet_Child"}], GAI)
        self.put_rows(SEED, [{"__RowId": "Player_Child"}], GAI)
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual([r["__RowId"] for r in self.rows(SEED, GAI)], ["CoopPuppet_Child", "Player_Child"])
        mark = read(self.path(SEED, mt.ROWS_MARK)).decode("utf-8").split("\n")
        self.assertEqual([line.split()[1] for line in mark[1:] if line], [AI, GAI])

    def test_the_pack_winning_or_a_seed_without_actor_tables_is_left_alone(self):
        self.put_rows(PACK, self.pack_rows())
        self.put_sizes(PACK, 5000)
        self.put_sizes(SEED, 4000)  # (an OOTRR 0.5.0 seed: no ActorInfo of its own)
        with open(self.path(SEED, mt.SEED_MARK), "w", encoding="utf-8", newline="\n") as f:
            f.write("default\n")  # (a seed the game built: its size table is left alone)
        pack_raw = read(self.path(PACK, "romfs", "RSDB", AI))
        self.assertEqual(self.run_all(), (0, {}))
        self.assertEqual(read(self.path(PACK, "romfs", "RSDB", AI)), pack_raw)
        self.assertFalse(os.path.exists(self.path(SEED, mt.ROWS_MARK)))
        self.assertEqual(self.sizes(SEED).get(AI_KEY), 4000)

    def test_a_site_mod_that_wins_both_tables_gets_the_rows_and_the_size_in_its_merged_table(self):
        self.put_rows(PACK, self.pack_rows())
        self.put_sizes(PACK, 5000)
        self.put_rows("ootrr-site", self.game_rows() + [{"__RowId": "Rando_Item"}])
        self.put_sizes("ootrr-site", 4000)
        failed, fit = self.run_all()
        self.assertEqual(failed, 0)
        self.assertIn("CoopPuppet_Child", {r["__RowId"] for r in self.rows("ootrr-site")})
        self.assertGreaterEqual(self.sizes("ootrr-site").get(AI_KEY), fit[AI_KEY])
        self.assertTrue(os.path.exists(self.path("ootrr-site", mt.MARK)))
        self.assertIn(f"fit {AI_KEY} {fit[AI_KEY]}",
                      read(self.path("ootrr-site", mt.KEEP_DIR, mt.STAMP)).decode("utf-8"))
        # The next start: both stamps hold; nothing written.
        before = self.table_bytes("ootrr-site")
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(self.table_bytes("ootrr-site"), before)

    def test_a_mod_under_contents_that_wins_the_actor_table_gets_the_rows_and_its_size(self):
        self.put_rows(PACK, self.pack_rows())
        self.put_sizes(PACK, 5000)
        title = "contents/0400486024DEC000/zz-skins"
        raw = read(self.put_rows(title, self.game_rows()))
        self.put_sizes(title, 4000)
        failed, fit = self.run_all()
        self.assertEqual(failed, 0)
        self.assertIn("CoopPuppet_Child", {r["__RowId"] for r in self.rows(title)})
        self.assertGreaterEqual(self.sizes(title).get(AI_KEY), fit[AI_KEY])
        self.assertTrue(os.path.exists(self.path(title, mt.ROWS_MARK)))
        mt.undo(self.mods, self.say)
        self.assertEqual(read(self.path(title, "romfs", "RSDB", AI)), raw)

    def test_a_mod_under_contents_with_only_a_size_table_that_wins_holds_the_merged_actor_tables_size(self):
        # Player report 259 as it was: the pack and the Character Select mod ship RSDB/ActorInfo (merged into the
        # Character Select mod's copy, larger than the game's), Classic Link Voices (under contents/) ships only its
        # own size table and wins it. The table the game reads must hold the merged actor table's size, in any order.
        import itertools
        cs, voices = "OoT Character Select", "contents/0400486024DEC000/Classic Link Voices"
        game = os.path.join(self.romfs, "RSDB", AI)
        os.makedirs(os.path.dirname(game), exist_ok=True)
        with open(game, "wb") as f:
            f.write(zstandard.ZstdCompressor(write_content_size=True).compress(byml.dumps(self.game_rows())))
        for order in itertools.permutations([PACK, cs, voices]):
            with self.subTest(order=order):
                shutil.rmtree(self.mods)
                os.makedirs(self.mods)
                self.put_rows(PACK, self.pack_rows())
                self.put_sizes(PACK, 5000)
                self.put_rows(cs, self.game_rows() + [{"__RowId": "Npc_CharSel_" + "x" * 40, "Heap": 6}] * 1
                              + [{"__RowId": "Player_Snail", "Heap": 7}])
                t = rstb.Rstb(by_crc={rstb.Rstb.crc(AI_KEY): 3000, 7: 70, 8: 80})   # (the game's sizes, its own)
                p = self.path(voices, "romfs", "System", "Resource", NAME)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(zstandard.ZstdCompressor(write_content_size=True).compress(t.dumps()))
                with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
                    f.write("coop-mods 1\n" + "".join(f"on {n}\n" for n in order))   # (the first row wins)
                failed, fit = self.run_all()
                self.assertEqual(failed, 0, self.lines)
                order_now = mt.load_order(self.mods)
                rows_win = [n for n, p_ in order_now if AI in mt.rsdb_tables_of(p_)][-1]
                size_win = [n for n, p_ in order_now if NAME in mt.tables_of(p_)][-1]
                ids = {r["__RowId"] for r in self.rows(rows_win)}
                self.assertTrue({"CoopPuppet_Child", "Player_Snail"} <= ids)
                served = self.sizes(size_win)
                self.assertGreaterEqual(served.get(AI_KEY), fit[AI_KEY])   # (the abort: an entry too small)
                self.assertEqual((served.by_crc.get(7), served.by_crc.get(8)), (70, 80))   # (both tables' entries)
                self.assertTrue(os.path.exists(self.path(size_win, mt.MARK)) or size_win == PACK)

    def test_a_mod_switched_off_in_the_mods_list_is_not_merged(self):
        own = self.seed_with_own_tables()
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"coop-mods 1\noff {SEED}\n")
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(self.path(SEED, "romfs", "RSDB", AI)), own)


@unittest.skipIf(zstandard is None, "needs zstandard")
class MergeRowsBetweenModsTest(MergeRowsTest):
    """Player report 206: a randomizer seed and the Character Select mod each ship RSDB/ActorInfo; the game read one
    mod's whole table and the other's rows were gone. Every lower mod's changed rows go into the winner's copy."""

    CS = "OoT Character Select"
    HAND = "OoT_Randomizer_0123456789abcdef"   # (a seed put in by hand: sorts after the Character Select mod, wins)

    def put_game(self):
        p = os.path.join(self.romfs, "RSDB", AI)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(zstandard.ZstdCompressor(write_content_size=True).compress(byml.dumps(self.game_rows())))

    def both_mods(self):
        self.put_game()
        cs = self.put_rows(self.CS, [{"__RowId": "Npc_Mido", "Heap": 1}, {"__RowId": "Player_Child", "Heap": 22}])
        seed = self.put_rows(self.HAND, self.game_rows() + [{"__RowId": "Rando_Item", "Heap": 5}])
        return read(cs), read(seed)

    def heaps(self, mod):
        return {r["__RowId"]: r["Heap"] for r in self.rows(mod)}

    def test_the_winning_seed_gets_the_row_the_character_mod_changed(self):
        cs, seed = self.both_mods()
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(self.heaps(self.HAND), {"Npc_Mido": 1, "Player_Child": 22, "Rando_Item": 5})
        self.assertEqual(read(self.path(self.CS, "romfs", "RSDB", AI)), cs)   # the lower mod is not written
        self.assertFalse(os.path.exists(self.path(self.HAND, mt.ROWS_MARK)))  # no puppet pack: no mark for the module
        before = read(self.path(self.HAND, "romfs", "RSDB", AI))
        self.assertEqual(self.run_all()[0], 0)
        self.assertTrue(any("already" in line for line in self.lines))
        self.assertEqual(read(self.path(self.HAND, "romfs", "RSDB", AI)), before)
        mt.undo(self.mods, self.say)
        self.assertEqual(read(self.path(self.HAND, "romfs", "RSDB", AI)), seed)

    def test_the_screens_table_has_no_row_id_its_rows_are_keyed_by_name(self):
        """The pack's Title_00 (Common added to its archives) and its CoopMenu_00 row, Character Select's CharacterMenu_00
        row: all in the winner's UIScreen table, in the game's order then added rows, the winner's own rows kept."""
        screens = "UIScreen.Product.111.rstbl.byml.zs"
        title = {"ArchiveCategory": ["Title"], "Class": "UITitleScreen", "Name": "Title_00"}
        other = {"ArchiveCategory": ["Common"], "Name": "Other_00"}
        p = os.path.join(self.romfs, "RSDB", screens)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(zstandard.ZstdCompressor(write_content_size=True).compress(byml.dumps([title, other])))
        title_common = dict(title, ArchiveCategory=["Title", "Common"])
        coop = {"ArchiveCategory": ["Common"], "BodyLayoutFile": "CoopMenu_00", "Name": "CoopMenu_00"}
        charsel = {"ArchiveCategory": ["Common"], "BodyLayoutFile": "CharacterMenu_00", "Name": "CharacterMenu_00"}
        self.put_rows(PACK, [title_common, other, coop], screens)
        self.put_rows(self.CS, [title, other, charsel], screens)
        seed = read(self.put_rows(self.HAND, [title, other], screens))
        self.assertEqual(self.run_all()[0], 0)
        got = self.rows(self.HAND, screens)
        self.assertEqual([r["Name"] for r in got], ["CharacterMenu_00", "CoopMenu_00", "Other_00", "Title_00"])   # (sorted by Name, as the game's)
        self.assertEqual(got[3]["ArchiveCategory"], ["Title", "Common"])
        before = read(self.path(self.HAND, "romfs", "RSDB", screens))
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(self.path(self.HAND, "romfs", "RSDB", screens)), before)   # (merged once: stamps hold)
        mt.undo(self.mods, self.say)
        self.assertEqual(read(self.path(self.HAND, "romfs", "RSDB", screens)), seed)

    def test_a_table_is_keyed_by_row_id_or_by_name_never_a_mix(self):
        mt.rows_of(byml.dumps([{"__RowId": "A"}, {"__RowId": "B", "Name": "x"}]))
        mt.rows_of(byml.dumps([{"Name": "A"}, {"Name": "B"}]))
        with self.assertRaises(mt.MergeError):
            mt.rows_of(byml.dumps([{"__RowId": "A"}, {"Name": "B"}]))
        with self.assertRaises(mt.MergeError):
            mt.rows_of(byml.dumps([{"Heap": 1}]))

    def test_a_winner_that_changed_the_title_row_keeps_its_own(self):
        screens = "UIScreen.Product.111.rstbl.byml.zs"
        title = {"ArchiveCategory": ["Title"], "Name": "Title_00", "SortKey": 1}
        p = os.path.join(self.romfs, "RSDB", screens)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(zstandard.ZstdCompressor(write_content_size=True).compress(byml.dumps([title])))
        self.put_rows(PACK, [dict(title, ArchiveCategory=["Title", "Common"])], screens)
        self.put_rows(self.HAND, [dict(title, SortKey=2)], screens)
        self.run_all()
        self.assertEqual(self.rows(self.HAND, screens), [dict(title, SortKey=2)])

    def test_the_other_order_gives_the_same_rows(self):
        self.both_mods()
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"coop-mods 1\non {self.CS}\non {self.HAND}\n")   # the first row wins
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(self.heaps(self.CS), {"Npc_Mido": 1, "Player_Child": 22, "Rando_Item": 5})

    def test_with_the_pack_all_three_are_in_the_table_the_game_reads(self):
        self.both_mods()
        self.put_rows(PACK, self.pack_rows())
        self.put_sizes(PACK, 5000)
        self.put_sizes(self.HAND, 4000)
        failed, fit = self.run_all()
        self.assertEqual(failed, 0)
        self.assertEqual(self.heaps(self.HAND), {"Npc_Mido": 1, "Player_Child": 22, "Rando_Item": 5,
                                                 "CoopPuppet_Child": 3, "CoopShot_Seed": 4})
        self.assertGreater(fit[AI_KEY], 5000)
        mark = read(self.path(self.HAND, mt.ROWS_MARK)).decode("utf-8")
        self.assertTrue(mark.startswith(f"coop-rows 1\ntable {AI} "))
        # The Character Select mod taken out: merged again from the seed's kept copy, without its row.
        shutil.rmtree(self.path(self.CS))
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(self.heaps(self.HAND)["Player_Child"], 2)
        self.assertIn("CoopPuppet_Child", self.heaps(self.HAND))

    def test_a_row_both_changed_stays_the_winners_and_is_named(self):
        self.put_game()
        self.put_rows(self.CS, [{"__RowId": "Npc_Mido", "Heap": 7}, {"__RowId": "Player_Child", "Heap": 22}])
        self.put_rows(self.HAND, [{"__RowId": "Npc_Mido", "Heap": 9}, {"__RowId": "Player_Child", "Heap": 2}])
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(self.heaps(self.HAND), {"Npc_Mido": 9, "Player_Child": 22})
        self.assertTrue(any("changed Npc_Mido too" in line for line in self.lines))

    def test_without_the_games_own_table_nothing_is_written_and_it_is_said(self):
        cs, seed = self.both_mods()
        os.remove(os.path.join(self.romfs, "RSDB", AI))
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(self.path(self.HAND, "romfs", "RSDB", AI)), seed)
        self.assertTrue(any("Not merged without the game's own copy" in line for line in self.lines))

    def write_list(self, text):
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 1\n" + text)

    def test_the_other_mod_off_gone_or_moved_up_gives_the_winner_its_own_table_back(self):
        cs, seed = self.both_mods()
        seed_file, cs_file = self.path(self.HAND, "romfs", "RSDB", AI), self.path(self.CS, "romfs", "RSDB", AI)
        self.run_all()
        self.assertNotEqual(read(seed_file), seed)
        self.write_list(f"on {self.HAND}\noff {self.CS}\n")   # switched off in the Mods tab
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(seed_file), seed)
        self.assertFalse(os.path.exists(self.path(self.HAND, mt.ROWS_KEEP)))
        self.assertTrue(any("own" in line and "back" in line for line in self.lines))
        self.write_list(f"on {self.HAND}\non {self.CS}\n")
        self.run_all()
        self.assertNotEqual(read(seed_file), seed)
        self.write_list(f"on {self.CS}\non {self.HAND}\n")    # moved over the seed: the mod wins now
        self.run_all()
        self.assertEqual(read(seed_file), seed)
        self.assertEqual(self.heaps(self.CS), {"Npc_Mido": 1, "Player_Child": 22, "Rando_Item": 5})
        shutil.rmtree(self.path(self.HAND))                    # the seed taken out
        self.run_all()
        self.assertEqual(read(cs_file), cs)

    def test_a_pack_taken_out_gives_the_seed_its_own_table_back_and_no_mark(self):
        own = self.seed_with_own_tables()
        self.run_all()
        self.assertTrue(os.path.exists(self.path(SEED, mt.ROWS_MARK)))
        shutil.rmtree(self.path(PACK))
        self.run_all()
        self.assertEqual(read(self.path(SEED, "romfs", "RSDB", AI)), own)
        self.assertFalse(os.path.exists(self.path(SEED, mt.ROWS_MARK)))

    def test_a_table_put_in_since_the_merge_is_not_overwritten_by_the_kept_copy(self):
        self.both_mods()
        self.run_all()
        new = self.put_rows(self.HAND, self.game_rows() + [{"__RowId": "Other_Seed", "Heap": 8}])
        shutil.rmtree(self.path(self.CS))
        self.run_all()
        self.assertEqual(read(self.path(self.HAND, "romfs", "RSDB", AI)), read(new))
        self.assertFalse(os.path.exists(self.path(self.HAND, mt.ROWS_KEEP)))

    def test_a_mod_switched_off_gives_no_rows(self):
        cs, seed = self.both_mods()
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"coop-mods 1\non {self.HAND}\noff {self.CS}\n")
        self.assertEqual(self.run_all()[0], 0)
        self.assertEqual(read(self.path(self.HAND, "romfs", "RSDB", AI)), seed)


GD = "GameDataList.Product.110.byml.zs"
GD_KEY = "GameData/GameDataList.Product.110.byml"


@unittest.skipIf(zstandard is None, "needs zstandard")
class MergeRowsUnderContentsTest(MergeRowsBetweenModsTest):
    """The same, both mods under contents/<title id>/ (player report 259: such a winner was never rewritten)."""
    CS = "contents/0400486024DEC000/OoT Character Select"
    HAND = "contents/0400486024DEC000/OoT_Randomizer_0123456789abcdef"


class MergeGameDataTest(unittest.TestCase):
    """Player report 206: a seed changes defaults in the game's list of saved values, the Character Select mod adds
    values and a save layout; the game read one mod's list. The winner's copy gets both."""

    CS = "OoT Character Select"
    HAND = "OoT_Randomizer_0123456789abcdef"   # (sorts after the Character Select mod: it wins)

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="merge_gamedata_test_")
        self.mods = os.path.join(self.dir, "mods")
        self.romfs = os.path.join(self.dir, "romfs")
        self.lines = []
        os.makedirs(self.mods)
        self.put(os.path.join(self.romfs, "GameData", GD), self.game_list())
        cs = self.game_list()
        cs["Data"]["Int"].append({"Hash": byml.UInt(900), "DefaultValue": 0})
        cs["MetaData"]["AllDataSaveSize"] = 108
        self.cs_raw = self.put(os.path.join(self.mods, self.CS, "romfs", "GameData", GD), cs)
        seed = self.game_list()
        seed["Data"]["Bool"][1]["DefaultValue"] = True
        self.seed_raw = self.put(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD), seed)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    @staticmethod
    def game_list():
        return {"Data": {"Bool": [{"Hash": byml.UInt(1), "DefaultValue": False},
                                  {"Hash": byml.UInt(2), "DefaultValue": False}],
                         "Int": [{"Hash": byml.UInt(3), "DefaultValue": 5}]},
                "MetaData": {"AllDataSaveSize": 100}}

    @staticmethod
    def put(path, root):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        raw = zstandard.ZstdCompressor(write_content_size=True).compress(byml.dumps(root, big=True))
        with open(path, "wb") as f:
            f.write(raw)
        return raw

    def served(self, mod):
        data = mt.Codec(self.romfs).unpack(read(os.path.join(self.mods, mod, "romfs", "GameData", GD)))
        self.assertEqual(data[:2], b"BY")   # big-endian, as the game's own
        root = byml.loads(data)
        return ({(k, int(e["Hash"].v)): e["DefaultValue"] for k, es in root["Data"].items() for e in es},
                root["MetaData"]["AllDataSaveSize"])

    def merge(self):
        self.lines = []
        return mt.merge_gamedata(self.mods, self.romfs, self.lines.append)

    def test_the_winning_seed_keeps_its_defaults_and_gets_the_added_values_and_their_layout(self):
        self.merge()
        want = ({("Bool", 1): False, ("Bool", 2): True, ("Int", 3): 5, ("Int", 900): 0}, 108)
        self.assertEqual(self.served(self.HAND), want)
        self.assertEqual(read(os.path.join(self.mods, self.CS, "romfs", "GameData", GD)), self.cs_raw)
        before = read(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD))
        self.merge()
        self.assertIn("already", self.lines[0])
        self.assertEqual(read(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD)), before)
        mt.undo(self.mods, say=self.lines.append)
        self.assertEqual(read(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD)), self.seed_raw)
        self.assertFalse(os.path.exists(os.path.join(self.mods, self.HAND, mt.GAMEDATA_KEEP)))

    def test_a_re_merge_stopped_after_its_stamp_leaves_undo_the_seeds_own_list(self):
        self.merge()
        served = read(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD))
        cs = self.game_list()
        cs["Data"]["Int"].append({"Hash": byml.UInt(900), "DefaultValue": 1})   # (the mod's value changed)
        cs["MetaData"]["AllDataSaveSize"] = 108
        self.put(os.path.join(self.mods, self.CS, "romfs", "GameData", GD), cs)
        with stopped_at(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD)) as seen:
            self.merge()
        self.assertTrue(seen)   # (the stamp was written, the list not)
        self.assertEqual(read(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD)), served)
        self.merge()   # (the next start finishes it)
        self.assertEqual(self.served(self.HAND)[0][("Int", 900)], 1)
        mt.undo(self.mods, say=self.lines.append)
        self.assertEqual(read(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD)), self.seed_raw)

    def test_the_other_order_gives_the_same_list(self):
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"coop-mods 1\non {self.CS}\non {self.HAND}\n")   # the first row wins
        self.merge()
        self.assertEqual(self.served(self.CS),
                         ({("Bool", 1): False, ("Bool", 2): True, ("Int", 3): 5, ("Int", 900): 0}, 108))

    def test_the_size_the_merged_list_needs_is_given_for_the_size_table(self):
        t = rstb.Rstb(by_crc={rstb.Rstb.crc(GD_KEY): 3000})
        p = os.path.join(self.mods, self.HAND, "romfs", "System", "Resource", NAME)
        os.makedirs(os.path.dirname(p))
        with open(p, "wb") as f:
            f.write(zstandard.ZstdCompressor(write_content_size=True).compress(t.dumps()))
        self.assertGreater(self.merge()[GD_KEY], 3000)

    def test_two_mods_that_both_add_values_are_said_and_nothing_is_written(self):
        seed = self.game_list()
        seed["Data"]["Int"].append({"Hash": byml.UInt(901), "DefaultValue": 1})
        raw = self.put(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD), seed)
        self.assertEqual(self.merge(), {})
        self.assertIn("each add saved values", self.lines[0])
        self.assertEqual(read(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD)), raw)

    def test_the_other_mod_off_gone_or_moved_up_gives_the_winner_its_own_list_back(self):
        seed_file = os.path.join(self.mods, self.HAND, "romfs", "GameData", GD)
        cs_file = os.path.join(self.mods, self.CS, "romfs", "GameData", GD)

        def write_list(text):
            with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
                f.write("coop-mods 1\n" + text)
        self.merge()
        self.assertNotEqual(read(seed_file), self.seed_raw)
        write_list(f"on {self.HAND}\noff {self.CS}\n")   # switched off in the Mods tab
        self.merge()
        self.assertEqual(read(seed_file), self.seed_raw)
        self.assertFalse(os.path.exists(os.path.join(self.mods, self.HAND, mt.GAMEDATA_KEEP)))
        write_list(f"on {self.HAND}\non {self.CS}\n")
        self.merge()
        self.assertNotEqual(read(seed_file), self.seed_raw)
        write_list(f"on {self.CS}\non {self.HAND}\n")    # moved over the seed: the mod wins now
        self.merge()
        self.assertEqual(read(seed_file), self.seed_raw)
        self.assertEqual(self.served(self.CS)[0][("Bool", 2)], True)   # the seed's default, from its own list
        shutil.rmtree(os.path.join(self.mods, self.HAND))               # the seed taken out
        self.merge()
        self.assertEqual(read(cs_file), self.cs_raw)

    def test_a_value_the_adding_mod_took_out_is_said_not_a_stop(self):
        cs = self.game_list()
        del cs["Data"]["Bool"][1]   # (the value the seed changes)
        cs["Data"]["Int"].append({"Hash": byml.UInt(900), "DefaultValue": 0})
        self.put(os.path.join(self.mods, self.CS, "romfs", "GameData", GD), cs)
        self.assertEqual(self.merge(), {})
        self.assertIn("could not be merged", self.lines[-1])
        self.assertIn("has no Bool value", self.lines[-1])
        self.assertEqual(read(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD)), self.seed_raw)

    def test_without_the_games_own_list_nothing_is_written(self):
        os.remove(os.path.join(self.romfs, "GameData", GD))
        self.merge()
        self.assertIn("Not merged without the game's own copy", self.lines[0])
        self.assertEqual(read(os.path.join(self.mods, self.HAND, "romfs", "GameData", GD)), self.seed_raw)


@unittest.skipIf(zstandard is None, "no zstandard package")
class OneTimeKeysTest(unittest.TestCase):
    """Row 219 (#84): a seed's shuffled pots dropped their item again in every other game, and a freestanding item
    taken before a save + reload was there again for another player: the one-time keys were only learned from what a
    game saw loaded. The kit's start lists them from the game as served."""

    SEED = "OoT_Randomizer_0123456789abcdef"
    # Hash: (actor in the game's Banc, the game's row (ResetTypeValue, ExtraByte) or None)
    GAME = {0x10: ("Obj_Heart_Piece", (24, None)),        # a heart piece: one-time pickup
            0x11: ("Obj_Rupee_Green", (88, 4)),           # a rupee the game resets: not one-time
            0x12: ("Obj_TBox_A_01", (24, None)),          # a chest: shared by its own list (CoopChests)
            0x13: ("Obj_Pot_A_01", (88, 2)),              # a pot: a holder once a seed keeps it
            0x14: ("Enemy_FlyingPot_IceCavern", (280, None)),
            0x15: ("Shop_Bottle", (25, None)),            # a shop row kept by a seed: no holder
            0x16: ("Enemy_Skulltula_Gold", (24, None)),
            0x17: ("Obj_DekuNut", (24, None)),
            0x18: ("Obj_GerudoValley_Heart_Piece", (24, 3))}   # an ExtraByte: not saved once

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="one_time_keys_test_")
        self.mods = os.path.join(self.dir, "mods")
        self.romfs = os.path.join(self.dir, "romfs")
        self.lines = []
        os.makedirs(self.mods)
        self.put(os.path.join(self.romfs, "GameData", GD), self.rows({}))
        half = list(self.GAME.items())
        for name, part in (("HyruleField_Static", half[:5]), ("IceCavern_Dynamic", half[5:])):
            self.put(os.path.join(self.romfs, "Banc", name.split("_")[0], name + ".bcett.byml.zs"),
                     {"Actors": [{"Gyaml": g, "Hash": byml.UInt64(h)} for h, (g, _) in part]})

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def rows(self, kept):
        """The game's 64-bit-key rows, each of `kept` saved once (as OOTRR pots.rs one_time)."""
        out = []
        for h, (_, row) in self.GAME.items():
            reset, extra = (24, None) if h in kept else row
            r = {"Hash": byml.UInt64(h), "ResetTypeValue": reset}
            if extra is not None:
                r["ExtraByte"] = extra
            out.append(r)
        return {"Data": {"Bool64bitKey": out, "Bool": [{"Hash": byml.UInt(1), "DefaultValue": False}]}}

    put = staticmethod(MergeGameDataTest.put)

    def run_it(self, romfs=None):
        self.lines = []
        return mt.one_time_keys(self.mods, self.romfs if romfs is None else romfs, self.lines.append)

    def listed(self):
        text = read(os.path.join(self.mods, mt.ONE_TIME_FILE)).decode("utf-8").split("\n")
        self.assertEqual(text[0], "coop-one-time-keys 1")
        return (text[2], [int(l[7:], 16) for l in text if l.startswith("pickup ")],
                [int(l[7:], 16) for l in text if l.startswith("holder ")])

    def test_the_games_own_placed_pickups_that_stay_taken(self):
        self.assertEqual(self.run_it(), 0)
        self.assertEqual(self.listed(), ("served - -", [0x10, 0x16, 0x17], []))

    def test_a_seeds_kept_pots_are_holders_and_its_swapped_pickups_keep_their_key(self):
        seed = os.path.join(self.mods, self.SEED, "romfs")
        raw = self.put(os.path.join(seed, "GameData", GD), self.rows({0x13, 0x14, 0x15}))
        # The seed swaps the heart piece for another item (its own Banc): the placement's Hash stays.
        self.put(os.path.join(seed, "Banc", "HyruleField", "HyruleField_Static.bcett.byml.zs"),
                 {"Actors": [{"Gyaml": "Obj_Hookshot", "Hash": byml.UInt64(0x10)}]})
        self.run_it()
        self.assertEqual(self.listed(), (f"served {self.SEED}/romfs/GameData/{GD} {mt.fnv64_bytes(raw)}",
                                         [0x10, 0x16, 0x17], [0x13, 0x14]))
        self.assertIn("2 one-time holders", self.lines[0])
        before = read(os.path.join(self.mods, mt.ONE_TIME_FILE))
        self.run_it()
        self.assertIn("listed already", self.lines[0])
        self.assertEqual(read(os.path.join(self.mods, mt.ONE_TIME_FILE)), before)
        # Another seed (its pots no longer kept): listed again, without holders.
        self.put(os.path.join(seed, "GameData", GD), self.rows(set()))
        self.run_it()
        self.assertEqual(self.listed()[1:], ([0x10, 0x16, 0x17], []))
        mt.undo(self.mods, say=self.lines.append)
        self.assertFalse(os.path.exists(os.path.join(self.mods, mt.ONE_TIME_FILE)))

    def test_a_seed_that_is_off_is_not_served(self):
        self.put(os.path.join(self.mods, self.SEED, "romfs", "GameData", GD), self.rows({0x13}))
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8") as f:
            f.write(f"coop-mods 1\noff {self.SEED}\n")
        self.run_it()
        self.assertEqual(self.listed(), ("served - -", [0x10, 0x16, 0x17], []))

    def test_without_the_games_files_an_old_list_goes(self):
        self.run_it()
        self.assertEqual(self.run_it(romfs=""), 0)
        self.assertIn("not listed", self.lines[0])
        self.assertFalse(os.path.exists(os.path.join(self.mods, mt.ONE_TIME_FILE)))

    def test_a_broken_list_writes_nothing(self):
        self.run_it()
        with open(os.path.join(self.romfs, "GameData", GD), "wb") as f:
            f.write(b"not a list")
        self.assertEqual(self.run_it(), 1)
        self.assertIn("not listed", self.lines[0])
        self.assertFalse(os.path.exists(os.path.join(self.mods, mt.ONE_TIME_FILE)))


class MergeMembersTest(unittest.TestCase):
    """merge_members: the pack's menu archives into another mod's copy that wins them (the Character Select mod ships
    Mals/USen and the Common layout archive too; the game read the mod's whole file and the co-op menus lost theirs)."""

    MOD = "OoT Character Select"   # (not in coop-mods.list: by name, after contents/...: it wins)

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="merge_tables_members_")
        self.mods = os.path.join(self.dir, "mods")
        self.romfs = os.path.join(self.dir, "romfs")
        self.pack = os.path.join(self.mods, *PACK.split("/"))
        self.mod = os.path.join(self.mods, self.MOD)
        self.game = {"A.msbt": b"game A", "Hdr.msbt": b"game header", "X.msbt": b"game X"}
        self.put(os.path.join(self.romfs, *MALS.split("/")), self.archive(self.game))
        self.put(os.path.join(self.pack, "romfs", *MALS.split("/")), self.archive(
            dict(self.game, **{"A.msbt": b"pack A", "Hdr.msbt": b"pack header", "Coop.msbt": b"pack only"})))
        self.put(os.path.join(self.pack, "coop_puppet.json"), b'{"pause_menu": ["%s"]}' % MALS.encode())
        self.mod_raw = self.archive(dict(self.game, **{"Hdr.msbt": b"mod header", "Char.msbt": b"mod only"}))
        self.put(os.path.join(self.mod, "romfs", *MALS.split("/")), self.mod_raw)
        raw = zstandard.ZstdCompressor(write_content_size=True).compress(
            rstb.Rstb(by_crc={rstb.Rstb.crc(MALS[:-3]): 5000}).dumps())
        self.put(os.path.join(self.pack, "romfs", "System", "Resource", NAME), raw)
        self.lines = []

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    @staticmethod
    def put(path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    @staticmethod
    def archive(members):
        return zstandard.ZstdCompressor(write_content_size=True).compress(sarc.write(members, data_align=16))

    def served(self):
        raw = read(os.path.join(self.mod, "romfs", *MALS.split("/")))
        return sarc.read(zstandard.ZstdDecompressor().decompressobj().decompress(raw))

    def merge(self, romfs=None):
        self.lines = []
        return mt.merge_members(self.mods, self.romfs if romfs is None else romfs, self.lines.append)

    def test_the_winners_copy_gets_the_packs_members_and_keeps_its_own(self):
        failed, fit = self.merge()
        self.assertEqual(failed, 0)
        got = self.served()
        self.assertEqual(got["A.msbt"], b"pack A")         # the pack changed it, the mod did not
        self.assertEqual(got["Coop.msbt"], b"pack only")   # the pack's own
        self.assertEqual(got["Char.msbt"], b"mod only")    # the mod's own
        self.assertEqual(got["Hdr.msbt"], b"mod header")   # both changed it: the mod's stays, and is named
        self.assertEqual(got["X.msbt"], b"game X")
        self.assertIn("Both changed Hdr.msbt", self.lines[0])
        self.assertGreaterEqual(fit[MALS[:-3]], 5000)    # the merged file's size for the table the game reads
        self.assertEqual(read(os.path.join(self.mod, mt.MEMBERS_KEEP, *MALS.split("/"))), self.mod_raw)
        self.merge()                                     # the next start: nothing new
        self.assertIn("already", self.lines[0])
        self.assertEqual(self.served()["A.msbt"], b"pack A")
        mt.undo(self.mods, say=self.lines.append)        # --undo: the mod's own archive back
        self.assertEqual(read(os.path.join(self.mod, "romfs", *MALS.split("/"))), self.mod_raw)
        self.assertFalse(os.path.isdir(os.path.join(self.mod, mt.MEMBERS_KEEP)))

    def test_a_menu_layout_both_changed_gets_co_ops_part_made_on_the_winners(self):
        # lane R1 (0.11.9): Character Select and the pack both change MenuOverlay_00.buil; his alone lost co-op's tabs.
        from unittest import mock
        common = mt.pause_menu.COMMON_ARCHIVE
        game = {"PlusMenu_00.buil": b"game pages", "MenuOverlay_00.buil": b"game strip", "Y.buil": b"game Y"}
        self.put(os.path.join(self.romfs, *common.split("/")), self.archive(game))
        self.put(os.path.join(self.pack, "romfs", *common.split("/")), self.archive(
            dict(game, **{"MenuOverlay_00.buil": b"pack strip", "Y.buil": b"pack Y"})))
        self.put(os.path.join(self.mod, "romfs", *common.split("/")), self.archive(
            dict(game, **{"MenuOverlay_00.buil": b"his strip", "Y.buil": b"his Y"})))
        with mock.patch.object(mt.pause_menu, "shown_page", lambda pages: ((pages, 1.0), (2.0, 3.0))),                 mock.patch.object(mt.pause_menu, "add_tab", lambda d: d + b"+tabs"),                 mock.patch.object(mt.pause_menu, "add_minus_page", lambda d, place, size: d + b"+page"):
            self.merge()
        raw = read(os.path.join(self.mod, "romfs", *common.split("/")))
        got = sarc.read(zstandard.ZstdDecompressor().decompressobj().decompress(raw))
        self.assertEqual(got["MenuOverlay_00.buil"], b"his strip+tabs+page")   # his panes, then co-op's
        self.assertEqual(got["Y.buil"], b"his Y")                              # no co-op edit of it: his kept
        line = next(l for l in self.lines if common in l)
        self.assertIn("Both changed MenuOverlay_00.buil: co-op's part made on", line)
        self.assertIn("Both changed Y.buil:", line)

    def test_a_seeds_title_layout_gets_co_ops_title_rows_made_on_it(self):
        # compat-0119: an OOTRR 0.8 seed points Title_00.buil at its own logo; the seed's alone lost co-op's title rows.
        from unittest import mock
        title = mt.title_menu.TITLE_ARCHIVE
        game = {"Title_00.buil": b"game title", "Z.buil": b"game Z"}
        self.put(os.path.join(self.romfs, *title.split("/")), self.archive(game))
        self.put(os.path.join(self.pack, "romfs", *title.split("/")), self.archive(
            dict(game, **{"Title_00.buil": b"game title+rows", "Z.buil": b"pack Z"})))
        self.put(os.path.join(self.mod, "romfs", *title.split("/")), self.archive(
            dict(game, **{"Title_00.buil": b"seed logo", "Z.buil": b"seed Z"})))
        with mock.patch.object(mt.title_menu, "add_menu", lambda d: d + b"+rows"):
            self.merge()
        raw = read(os.path.join(self.mod, "romfs", *title.split("/")))
        got = sarc.read(zstandard.ZstdDecompressor().decompressobj().decompress(raw))
        self.assertEqual(got["Title_00.buil"], b"seed logo+rows")   # the seed's logo, then co-op's rows
        self.assertEqual(got["Z.buil"], b"seed Z")                   # no co-op edit of it: the seed's kept
        line = next(l for l in self.lines if title in l)
        self.assertIn("Both changed Title_00.buil: co-op's part made on", line)
        self.assertIn("Both changed Z.buil:", line)

    def test_a_title_message_file_both_changed_gets_the_title_rows(self):
        from unittest import mock
        game = {"LayoutMsg/Title_00.msbt": b"game T"}
        self.put(os.path.join(self.romfs, *MALS.split("/")), self.archive(game))
        self.put(os.path.join(self.pack, "romfs", *MALS.split("/")), self.archive({"LayoutMsg/Title_00.msbt": b"pack T"}))
        self.put(os.path.join(self.mod, "romfs", *MALS.split("/")), self.archive({"LayoutMsg/Title_00.msbt": b"seed T"}))
        with mock.patch.object(mt.title_menu, "add_rows", lambda d, rows: d + b"+rows:" + str(len(rows)).encode()):
            self.merge()
        got = self.served()["LayoutMsg/Title_00.msbt"]
        self.assertTrue(got.startswith(b"seed T+rows:"), got)
        self.assertEqual(got, b"seed T+rows:" + str(len(mt.title_menu.rows_for(MALS))).encode())

    def test_a_title_layout_that_cannot_take_the_rows_keeps_the_winners(self):
        title = mt.title_menu.TITLE_ARCHIVE
        game = {"Title_00.buil": b"game title"}
        self.put(os.path.join(self.romfs, *title.split("/")), self.archive(game))
        self.put(os.path.join(self.pack, "romfs", *title.split("/")), self.archive({"Title_00.buil": b"pack title"}))
        self.put(os.path.join(self.mod, "romfs", *title.split("/")), self.archive({"Title_00.buil": b"not a layout"}))
        failed, _ = self.merge()
        raw = read(os.path.join(self.mod, "romfs", *title.split("/")))
        got = sarc.read(zstandard.ZstdDecompressor().decompressobj().decompress(raw))
        self.assertEqual(got["Title_00.buil"], b"not a layout")
        self.assertTrue(any("could not be made" in l for l in self.lines))
        self.assertTrue(any("Both changed Title_00.buil: " in l and "kept" in l for l in self.lines))

    def test_a_message_file_both_changed_gets_co_ops_labels_added_to_the_winners(self):
        from unittest import mock
        added = lambda d, labels, replace=False: d + b"+" + ",".join(sorted(labels)).encode()
        with mock.patch.object(mt.ui_pack, "load_strings", lambda language, *a: {"Hdr.msbt": {"Coop_Row": "x"}}),                 mock.patch.object(mt.ui_pack, "add_labels", added):
            self.merge()
        self.assertEqual(self.served()["Hdr.msbt"], b"mod header+Coop_Row")
        self.assertIn("Both changed Hdr.msbt: co-op's part made on", self.lines[0])

    def test_a_layout_edit_that_fails_keeps_the_winners_and_says_why(self):
        from unittest import mock
        def fails(*a, **k):
            raise mt.ui_pack.UiPackError("no such pane")
        with mock.patch.object(mt.ui_pack, "load_strings", lambda language, *a: {"Hdr.msbt": {"Coop_Row": "x"}}),                 mock.patch.object(mt.ui_pack, "add_labels", fails):
            self.merge()
        self.assertEqual(self.served()["Hdr.msbt"], b"mod header")
        self.assertTrue(any("could not be made" in l and "no such pane" in l for l in self.lines))
        self.assertTrue(any("Both changed Hdr.msbt: " in l and "kept" in l for l in self.lines))

    def test_a_re_merge_stopped_after_its_stamp_leaves_undo_the_mods_own_archive(self):
        self.merge()
        served = read(os.path.join(self.mod, "romfs", *MALS.split("/")))
        self.put(os.path.join(self.pack, "romfs", *MALS.split("/")), self.archive(
            dict(self.game, **{"A.msbt": b"pack A2"})))
        with stopped_at(os.path.join(self.mod, "romfs", *MALS.split("/"))) as seen:
            self.merge()
        self.assertTrue(seen)   # (the stamp was written, the archive not)
        self.assertEqual(read(os.path.join(self.mod, "romfs", *MALS.split("/"))), served)
        self.merge()   # (the next start finishes it)
        got = self.served()
        self.assertEqual(got["A.msbt"], b"pack A2")
        self.assertNotIn("Coop.msbt", got)   # (merged from the mod's own archive, not from the earlier merge)
        mt.undo(self.mods, say=self.lines.append)
        self.assertEqual(read(os.path.join(self.mod, "romfs", *MALS.split("/"))), self.mod_raw)

    def test_a_new_pack_merges_again_from_the_mods_own_copy(self):
        self.merge()
        self.put(os.path.join(self.pack, "romfs", *MALS.split("/")), self.archive(
            dict(self.game, **{"A.msbt": b"pack A2"})))
        self.merge()
        got = self.served()
        self.assertEqual(got["A.msbt"], b"pack A2")
        self.assertNotIn("Coop.msbt", got)               # from the kept copy, not from the last merge
        self.assertEqual(got["Char.msbt"], b"mod only")

    def test_nothing_without_the_games_copy_or_when_the_pack_wins(self):
        failed, _ = self.merge(romfs="")
        self.assertEqual(failed, 1)
        self.assertEqual(read(os.path.join(self.mod, "romfs", *MALS.split("/"))), self.mod_raw)
        self.assertIn("not merged", self.lines[0])
        os.remove(os.path.join(self.mod, "romfs", *MALS.split("/")))   # the mod without that archive
        self.assertEqual(self.merge(), (0, {}))
        self.assertEqual(self.lines, [])

    # Player report 206: a seed's messages and the Character Select mod's, in the same archive.
    HAND = "OoT_Randomizer_0123456789abcdef"   # (sorts after the Character Select mod: it wins)

    def put_seed(self):
        raw = self.archive(dict(self.game, **{"X.msbt": b"seed X"}))
        self.put(os.path.join(self.mods, self.HAND, "romfs", *MALS.split("/")), raw)
        return raw

    def served_by(self, mod):
        raw = read(os.path.join(self.mods, mod, "romfs", *MALS.split("/")))
        return sarc.read(zstandard.ZstdDecompressor().decompressobj().decompress(raw))

    def test_a_seed_that_wins_gets_the_character_mods_members_and_the_packs(self):
        seed = self.put_seed()
        self.assertEqual(self.merge()[0], 0)
        got = self.served_by(self.HAND)
        self.assertEqual(got["X.msbt"], b"seed X")          # the seed's own
        self.assertEqual(got["Char.msbt"], b"mod only")     # the Character Select mod's
        self.assertEqual(got["Hdr.msbt"], b"mod header")    # mod and pack changed it: the mod is nearer the winner
        self.assertEqual(got["A.msbt"], b"pack A")
        self.assertEqual(got["Coop.msbt"], b"pack only")
        self.assertEqual(read(os.path.join(self.mod, "romfs", *MALS.split("/"))), self.mod_raw)   # lower: not written
        before = read(os.path.join(self.mods, self.HAND, "romfs", *MALS.split("/")))
        self.assertEqual(self.merge()[0], 0)
        self.assertIn("already", self.lines[0])
        self.assertEqual(read(os.path.join(self.mods, self.HAND, "romfs", *MALS.split("/"))), before)
        mt.undo(self.mods, say=self.lines.append)
        self.assertEqual(read(os.path.join(self.mods, self.HAND, "romfs", *MALS.split("/"))), seed)

    def test_two_mods_without_the_pack_are_merged_too(self):
        self.put_seed()
        shutil.rmtree(self.pack)
        self.assertEqual(self.merge()[0], 0)
        got = self.served_by(self.HAND)
        self.assertEqual((got["X.msbt"], got["Char.msbt"], got["Hdr.msbt"], got["A.msbt"]),
                         (b"seed X", b"mod only", b"mod header", b"game A"))
        seed_own = read(os.path.join(self.mods, self.HAND, mt.MEMBERS_KEEP, *MALS.split("/")))
        self.assertEqual(self.merge(romfs="")[0], 0)   # without the game's copy: said, no failure of the puppets
        self.assertTrue(any("Not merged without the game's own copy" in line for line in self.lines))
        # ... and what this start cannot merge is not left merged from an earlier one: the seed's own archive is back.
        self.assertEqual(read(os.path.join(self.mods, self.HAND, "romfs", *MALS.split("/"))), seed_own)

    def write_list(self, text):
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 1\n" + text)

    def test_the_other_mod_off_gone_or_moved_up_gives_the_winner_its_own_archive_back(self):
        seed = self.put_seed()
        shutil.rmtree(self.pack)
        seed_file = os.path.join(self.mods, self.HAND, "romfs", *MALS.split("/"))
        self.merge()
        self.assertNotEqual(read(seed_file), seed)
        self.write_list(f"on {self.HAND}\noff {self.MOD}\n")   # switched off in the Mods tab
        self.merge()
        self.assertEqual(read(seed_file), seed)
        self.assertFalse(os.path.exists(os.path.join(self.mods, self.HAND, mt.MEMBERS_KEEP)))
        self.write_list(f"on {self.HAND}\non {self.MOD}\n")
        self.merge()
        self.assertNotEqual(read(seed_file), seed)
        self.write_list(f"on {self.MOD}\non {self.HAND}\n")    # moved over the seed: the mod wins now
        self.merge()
        self.assertEqual(read(seed_file), seed)
        got = self.served_by(self.MOD)
        self.assertEqual((got["X.msbt"], got["Char.msbt"]), (b"seed X", b"mod only"))   # from the seed's own archive
        shutil.rmtree(os.path.join(self.mods, self.HAND))       # the seed taken out
        self.merge()
        self.assertEqual(read(os.path.join(self.mod, "romfs", *MALS.split("/"))), self.mod_raw)

    def test_the_pack_that_wins_gets_the_mods_members(self):
        os.rename(self.mod, os.path.join(self.mods, "a-menu-mod"))
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 1\n")   # (not named by the list, and under contents/ by name: the pack wins)
        self.assertEqual(self.merge()[0], 0)
        got = self.served_by(PACK)
        self.assertEqual((got["Char.msbt"], got["Hdr.msbt"], got["Coop.msbt"]),
                         (b"mod only", b"pack header", b"pack only"))


class SnailCustomizeTest(MergeMembersTest):
    """0.11.9: co-op's customization in Character Select's Customize screen: its message file gets the placeholders
    the module writes co-op's words through (Coop_Custom_00..12), its own messages kept."""

    def test_his_customize_messages_get_the_placeholders(self):
        from puppet_pack import msbt
        from test_ui_pack import make_msbt
        his = make_msbt([("T_Label_00", "Hair Color"), ("T_Val_00_00", "Default")])
        self.mod_raw = self.archive(dict(self.game, **{"Hdr.msbt": b"mod header", mt.SNAIL_MSG: his}))
        self.put(os.path.join(self.mod, "romfs", *MALS.split("/")), self.mod_raw)
        failed, _ = self.merge()
        self.assertEqual(failed, 0)
        got = msbt.read(self.served()[mt.SNAIL_MSG])
        self.assertEqual(got.text("T_Label_00"), ["Hair Color"])   # (his own kept)
        for i in range(mt.SNAIL_SLOTS):
            self.assertEqual(got.text("Coop_Custom_%02d" % i), [mt.SNAIL_PLACEHOLDER])
        self.assertIn("placeholders", self.lines[0])
        self.merge()                                               # the next start: nothing new
        self.assertIn("already", self.lines[0])

    def test_his_picker_names_are_made_readable(self):
        # (menu-customize-01113: his model ids, "Gerudo_GEN_002", were the labels under his icons; charsel_names.py)
        from puppet_pack import charsel_names, msbt
        from test_ui_pack import make_msbt
        his = make_msbt([("T_Slot_000", "Link"), ("T_CellName_000", "Link"), ("T_Slot_001", "Goron_GON_001"),
                         ("T_CellName_001", "Goron_GON_001")])
        self.mod_raw = self.archive(dict(self.game, **{"Hdr.msbt": b"mod header", charsel_names.PICKER_MSG: his}))
        self.put(os.path.join(self.mod, "romfs", *MALS.split("/")), self.mod_raw)
        failed, _ = self.merge()
        self.assertEqual(failed, 0)
        got = msbt.read(self.served()[charsel_names.PICKER_MSG])
        self.assertEqual((got.text("T_Slot_001"), got.text("T_CellName_001")), (["Goron"], ["Goron"]))
        self.assertEqual(got.text("T_Slot_000"), ["Link"])
        self.merge()                                               # the next start: nothing new
        self.assertIn("already", self.lines[0])

    def test_without_his_customize_screen_nothing_is_added(self):
        failed, _ = self.merge()
        self.assertEqual(failed, 0)
        self.assertNotIn(mt.SNAIL_MSG, self.served())


class SnailKeysTest(unittest.TestCase):
    """menu-customize-01113: his Customize screen's Y and A as the game's guide keys (two hidden PaGuideKey_00 parts on
    the footer's line, their words and button letters in his message file); the title then drops its worded hints."""

    # (his layout's shape: RootPane > N_All_00 > N_Custom_00 > N_Mark_00 > Pa_MarkL_00, then his overlay after it)
    TREE = [("RootPane", -1), ("N_All_00", 0), ("N_Custom_00", 1), ("T_Head_00", 2), ("N_Mark_00", 2),
            ("Pa_MarkL_00", 4), ("P_Overlay_00", 0)]

    def entries(self):
        from test_ui_pack import make_layout, make_msbt
        return [(mt.SNAIL_LAYOUT, make_layout(self.TREE, {"Pa_MarkL_00": ("PaCharacterKey_00", [])})),
                (mt.SNAIL_MSG, make_msbt([("T_Head_00", "Customize")])), ("Other.buil", b"other")]

    def test_two_hidden_guide_keys_on_the_footer_line_with_their_messages(self):
        from puppet_pack import msbt
        out, changed = mt.add_snail_keys(self.entries())
        self.assertTrue(changed)
        got = dict(out)
        d = got[mt.SNAIL_LAYOUT]
        panes, _ = ui_pack.read_layout(d)
        names = [p.name for p in panes]
        self.assertEqual(names[-2:], ["Pa_CoopKeyY_00", "Pa_CoopKeyA_00"])
        self.assertEqual(ui_pack.part_layouts(d)[-2:], [("Pa_CoopKeyY_00", "PaGuideKey_00"),
                                                         ("Pa_CoopKeyA_00", "PaGuideKey_00")])
        root = names.index("RootPane")
        for p in panes[-2:]:
            self.assertEqual(struct.unpack_from("<I", d, p.record + 0x28)[0], root)
            self.assertEqual(struct.unpack_from("<I", d, p.record + 0x38)[0] & 1, 0)   # (stored hidden)
        xs = [struct.unpack_from("<ff", d, p.record + 0x1c) for p in panes[-2:]]
        self.assertEqual(xs, [(1510.0, -1018.0), (1050.0, -1018.0)])
        m = msbt.read(got[mt.SNAIL_MSG])
        self.assertEqual(m.text("Pa_CoopKeyY_00-T_Icon_00"), ["\ue043"])
        self.assertEqual(m.text("Pa_CoopKeyY_00-T_Guide_00"), ["Next page"])
        self.assertEqual(m.text("Pa_CoopKeyA_00-T_Icon_00"), ["\ue040"])
        self.assertEqual(m.text("Pa_CoopKeyA_00-T_Guide_00"), ["Fine-tune"])
        self.assertEqual(m.text("T_Head_00"), ["Customize"])   # (his own kept)
        self.assertEqual(got["Other.buil"], b"other")
        again, changed = mt.add_snail_keys(out)                      # (the next start: nothing new)
        self.assertFalse(changed)
        self.assertEqual(again, out)

    def test_a_layout_without_his_key_part_is_left_alone(self):
        from test_ui_pack import make_layout
        d = make_layout([("RootPane", -1), ("N_All_00", 0)], {})
        out, changed = mt.add_snail_keys([(mt.SNAIL_LAYOUT, d)])
        self.assertEqual((out, changed), ([(mt.SNAIL_LAYOUT, d)], False))


class MessageLabelsTest(unittest.TestCase):
    """Row 224: Character Select's - menu tab read 'msgnotfound' (and his page crashed). His messages are only in USen
    and EUen: every other language's archive the game reads (the pack's, a seed's) lacked them. And a message file two
    mods change was taken whole from one, the other's labels lost (the pack's - menu labels too, when he gave it)."""

    MOD = "OoT Character Select"
    SEED = "OoT_Randomizer_0123456789abcdef"   # (sorts after him: it wins)
    US = "Mals/USen.Product.110.sarc.zs"
    FR = "Mals/EUfr.Product.110.sarc.zs"
    OV = "LayoutMsg/MenuOverlay_00.msbt"
    PM = "LayoutMsg/PlusMenu_00.msbt"
    CM = "LayoutMsg/CharacterMenu_00.msbt"

    def setUp(self):
        from test_ui_pack import make_msbt
        self.msbt = make_msbt
        self.dir = tempfile.mkdtemp(prefix="merge_tables_labels_")
        self.mods = os.path.join(self.dir, "mods")
        self.romfs = os.path.join(self.dir, "romfs")
        self.pack = os.path.join(self.mods, *PACK.split("/"))
        self.mod = os.path.join(self.mods, self.MOD)
        self.words = {self.US: ("Items", "Save"), self.FR: ("Objets", "Sauvegarder")}
        for rel, (cat, save) in self.words.items():
            self.put(os.path.join(self.romfs, *rel.split("/")), self.archive(self.game(rel)))
            self.put(os.path.join(self.pack, "romfs", *rel.split("/")), self.archive({
                self.OV: make_msbt([("Pa_Category_00", cat), ("Coop_Minus_00", "Co-op")], encoding=1),
                self.PM: make_msbt([("Save", save), ("Coop_Live_00", "....")], encoding=1)}))
        self.put(os.path.join(self.pack, "coop_puppet.json"),
                 ('{"pause_menu": ["%s", "%s"]}' % (self.US, self.FR)).encode())
        self.put(os.path.join(self.mod, "romfs", *self.US.split("/")), self.archive({
            self.OV: make_msbt([("Pa_Category_00", "Items"), ("Characters", "Characters")], encoding=1),
            self.PM: make_msbt([("Save", "Save")], encoding=1),
            self.CM: make_msbt([("T_Title_00", "Characters")], encoding=1)}))
        self.lines = []

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def game(self, rel):
        cat, save = self.words[rel]
        return {self.OV: self.msbt([("Pa_Category_00", cat)], encoding=1),
                self.PM: self.msbt([("Save", save)], encoding=1)}

    put = staticmethod(MergeMembersTest.put)
    archive = staticmethod(MergeMembersTest.archive)

    def served(self, mod_path, rel):
        from puppet_pack import msbt
        raw = read(os.path.join(mod_path, "romfs", *rel.split("/")))
        files = sarc.read(zstandard.ZstdDecompressor().decompressobj().decompress(raw))
        return {n: dict((k, "".join(x for x in v if isinstance(x, str))) for k, v in msbt.read(d).rows())
                for n, d in files.items()}

    def merge(self):
        self.lines = []
        return mt.merge_members(self.mods, self.romfs, self.lines.append)

    def test_a_language_he_does_not_ship_gets_his_english_messages(self):
        fr_own = read(os.path.join(self.pack, "romfs", *self.FR.split("/")))
        self.assertEqual(self.merge()[0], 0)
        fr = self.served(self.pack, self.FR)   # (the pack's EUfr: the one the game reads in French)
        self.assertEqual(fr[self.OV]["Pa_Category_00"], "Objets")    # the game's French words stay
        self.assertEqual(fr[self.OV]["Characters"], "Characters")    # his tab's label, in his English
        self.assertEqual(fr[self.OV]["Coop_Minus_00"], "Co-op")      # the pack's own
        self.assertEqual(fr[self.CM]["T_Title_00"], "Characters")    # his page's message file
        self.assertEqual(fr[self.PM]["Save"], "Sauvegarder")
        line = next(l for l in self.lines if self.FR in l)
        self.assertIn("OoT Character Select: no archive in this language, its English messages taken", line)
        us = self.served(self.mod, self.US)    # (his USen wins: the pack's labels in it, his kept)
        self.assertEqual((us[self.OV]["Characters"], us[self.OV]["Coop_Minus_00"], us[self.PM]["Coop_Live_00"]),
                         ("Characters", "Co-op", "...."))
        self.merge()                                                 # the next start: nothing new
        self.assertTrue(all("already" in l for l in self.lines if "Menus:" in l))
        mt.undo(self.mods, say=self.lines.append)
        self.assertEqual(read(os.path.join(self.pack, "romfs", *self.FR.split("/"))), fr_own)

    def tree(self, top):
        """Every file under top: {path under top: bytes}."""
        out = {}
        for dirpath, _, names in os.walk(top):
            for n in names:
                p = os.path.join(dirpath, n)
                out[os.path.relpath(p, top).replace(os.sep, "/")] = read(p)
        return out

    def test_every_start_gives_the_same_bytes_whatever_the_order_of_the_languages(self):
        # His English archive (USen) stands in for the language he has none of (EUfr) and is itself merged with the
        # pack's members. The pack lists EUfr first, so a pass merged EUfr from his USen before USen was merged:
        # the next start read the merged USen, and merged EUfr again with other bytes (CNzh and EUde: -352 B).
        self.put(os.path.join(self.pack, "coop_puppet.json"),
                 ('{"pause_menu": ["%s", "%s"]}' % (self.FR, self.US)).encode())
        before = self.tree(os.path.join(self.mods, self.MOD))
        fresh = os.path.join(self.dir, "fresh_mods")
        shutil.copytree(self.mods, fresh)
        shutil.copytree(self.mods, os.path.join(self.dir, "fresh_other_order"))
        self.assertEqual(self.merge()[0], 0)
        first = self.tree(self.mods)
        self.assertNotEqual(first, self.tree(fresh))   # (the merge wrote something)
        self.merge()
        self.assertTrue(all("already" in l for l in self.lines if "Menus:" in l), self.lines)
        self.assertEqual(self.tree(self.mods), first)   # the second start: not one byte moved
        self.merge()
        self.assertEqual(self.tree(self.mods), first)
        self.assertEqual(mt.merge_members(fresh, self.romfs, lambda line: None)[0], 0)
        self.assertEqual(self.tree(fresh), first)       # a fresh run gives the settled bytes at once
        # The other order (USen first) must give the same bytes: the stand-in is his own, never what a pass wrote.
        other = os.path.join(self.dir, "fresh_other_order")
        self.put(os.path.join(other, *PACK.split("/"), "coop_puppet.json"),
                 ('{"pause_menu": ["%s", "%s"]}' % (self.US, self.FR)).encode())
        self.assertEqual(mt.merge_members(other, self.romfs, lambda line: None)[0], 0)
        got, want = self.tree(other), dict(first)
        for tree in (got, want):
            tree.pop("/".join(PACK.split("/") + ["coop_puppet.json"]), None)
        self.assertEqual(got, want)
        self.assertEqual(self.served(os.path.join(self.mods, self.MOD), self.US)[self.OV]["Coop_Minus_00"], "Co-op")
        # The kept original is what stands in, also when his English was merged at an earlier start.
        fr = self.served(self.pack, self.FR)
        self.assertEqual((fr[self.OV]["Characters"], fr[self.OV]["Pa_Category_00"]), ("Characters", "Objets"))
        mt.undo(self.mods, say=self.lines.append)
        self.assertEqual(self.tree(os.path.join(self.mods, self.MOD)), before)   # --undo: his own files back

    def test_a_message_file_three_mods_change_keeps_each_ones_labels(self):
        for rel in (self.US, self.FR):     # a seed: every language, a label of its own in the pause menu's file
            cat, save = self.words[rel]
            self.put(os.path.join(self.mods, self.SEED, "romfs", *rel.split("/")), self.archive({
                self.OV: self.msbt([("Pa_Category_00", cat)], encoding=1),
                self.PM: self.msbt([("Save", save), ("Warp", "Warp")], encoding=1)}))
        self.assertEqual(self.merge()[0], 0)
        for rel in (self.US, self.FR):
            got = self.served(os.path.join(self.mods, self.SEED), rel)
            self.assertEqual(got[self.OV]["Characters"], "Characters")   # his (given before the pack's: kept)
            self.assertEqual(got[self.OV]["Coop_Minus_00"], "Co-op")     # the pack's: was lost after his
            self.assertEqual((got[self.PM]["Warp"], got[self.PM]["Coop_Live_00"]), ("Warp", "...."))
            self.assertEqual(got[self.CM]["T_Title_00"], "Characters")
        self.assertEqual(self.served(os.path.join(self.mods, self.SEED), self.FR)[self.OV]["Pa_Category_00"], "Objets")

    def test_a_label_two_mods_changed_stays_the_nearer_ones_and_is_named(self):
        self.put(os.path.join(self.mods, self.SEED, "romfs", *self.US.split("/")), self.archive({
            self.OV: self.msbt([("Pa_Category_00", "Seed items")], encoding=1),
            self.PM: self.msbt([("Save", "Save")], encoding=1)}))
        self.put(os.path.join(self.mod, "romfs", *self.US.split("/")), self.archive({
            self.OV: self.msbt([("Pa_Category_00", "His items"), ("Characters", "Characters")], encoding=1),
            self.PM: self.msbt([("Save", "Save")], encoding=1)}))
        self.merge()
        got = self.served(os.path.join(self.mods, self.SEED), self.US)
        self.assertEqual((got[self.OV]["Pa_Category_00"], got[self.OV]["Characters"]), ("Seed items", "Characters"))
        self.assertTrue(any("A label both changed, the nearer mod's kept: " + self.OV + " Pa_Category_00" in l
                            for l in self.lines))

    def test_attributes_travel_with_the_labels(self):
        size = 4
        def atr(*xs):
            return (b"ATR1", struct.pack("<II", len(xs), size) + b"".join(xs))
        game = self.msbt([("A", "a")], encoding=1, extra=[atr(b"\1\0\0\0")])
        base = self.msbt([("A", "a"), ("B", "b")], encoding=1, extra=[atr(b"\1\0\0\0", b"\2\0\0\0")])
        theirs = self.msbt([("A", "a"), ("C", "c")], encoding=1, extra=[atr(b"\1\0\0\0", b"\3\0\0\0")])
        out, clash = mt.merge_msbt(base, game, theirs)
        self.assertEqual(clash, [])
        header, sects, have, texts = mt.ui_pack.read_msbt(out)
        self.assertEqual(sorted(have), ["A", "B", "C"])
        self.assertEqual(mt._msbt_attrs(sects)[1][have["C"]], b"\3\0\0\0")
        self.assertEqual(mt.merge_msbt(base, game, base)[0], base)   # nothing to add: the same bytes
        with self.assertRaises(ValueError):
            mt.merge_msbt(b"not a message file", game, theirs)


class SelfModelsTest(unittest.TestCase):
    """Row 202: Character Select's own Link models win Model/Player_<Age>; each copy gets co-op's own tunic names, in
    the old top folder and under contents/ alike, so whichever wins your Link names your own tunic textures."""

    def setUp(self):
        from puppet_pack import tunic
        self.tunic = tunic
        self.dir = tempfile.mkdtemp(prefix="merge_tables_models_")
        self.mods = os.path.join(self.dir, "mods")
        self.pack = os.path.join(self.mods, *PACK.split("/"))
        own = tunic.tile_name("green", tunic.SELF_LETTER)
        self.put(os.path.join(self.pack, "romfs", "Tex", own + ".bntx.zs"), b"own tile")
        self.child = b"FRES" + bytes(12) + b"Angin_Tile_A_Alb\x00Angin_Tile_A_Dark_Alb\x00Mt_Cloth_Parts_Player_Child_Alb\x00"
        self.adult = b"FRES" + bytes(12) + b"Angin_Tile_A_Alb\x00Mt_Vest_Player_Adult_Alb\x00"
        self.top = os.path.join(self.mods, "OoT Character Select")
        self.under = os.path.join(self.mods, "contents", "0400486024DEC000", "OoT Character Select UI")
        for m in (self.top, self.under):
            self.put(os.path.join(m, "romfs", "Model", "Player_Child.Player_Child.bfres"), b"")   # (not the .zs)
            self.put(os.path.join(m, "romfs", *mt.SELF_MODELS["Child"].split("/")), self.child)
            self.put(os.path.join(m, "romfs", *mt.SELF_MODELS["Adult"].split("/")), self.adult)
        self.lines = []

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    @staticmethod
    def put(path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    def model(self, mod, age):
        return read(os.path.join(mod, "romfs", *mt.SELF_MODELS[age].split("/")))

    def test_every_copy_names_your_own_tunic_textures_and_undo_puts_them_back(self):
        t = self.tunic
        own_tile = t.tile_name("green", t.SELF_LETTER).encode() + b"\x00"
        self.assertEqual(mt.merge_self_models(self.mods, "", self.lines.append), 0)
        for m in (self.top, self.under):   # (both: whichever wins, your Link names yours)
            child, adult = self.model(m, "Child"), self.model(m, "Adult")
            self.assertIn(own_tile, child)
            self.assertNotIn(b"Angin_Tile_A_Alb\x00", child)
            self.assertIn(t.dark_name("green", t.SELF_LETTER).encode() + b"\x00", child)
            self.assertNotIn(b"Mt_Cloth_Parts_Player_Child_Alb\x00", child)   # (the child's extra too)
            self.assertIn(own_tile, adult)
            self.assertNotIn(b"Mt_Vest_Player_Adult_Alb\x00", adult)
            self.assertEqual(len(child), len(self.child))                      # (the same length: its size entry holds)
            self.assertEqual(read(os.path.join(m, mt.MODELS_KEEP, *mt.SELF_MODELS["Child"].split("/"))), self.child)
        self.assertEqual(len(self.lines), 4)
        self.lines = []
        self.assertEqual(mt.merge_self_models(self.mods, "", self.lines.append), 0)   # the next start: nothing
        self.assertEqual(self.lines, [])
        mt.undo(self.mods, say=self.lines.append)                                      # --undo: theirs back
        for m in (self.top, self.under):
            self.assertEqual(self.model(m, "Child"), self.child)
            self.assertEqual(self.model(m, "Adult"), self.adult)
            self.assertFalse(os.path.isdir(os.path.join(m, mt.MODELS_KEEP)))

    def test_a_start_stopped_between_the_model_and_its_stamp_leaves_undo_the_mods_own_model(self):
        # (the stamp is written before the model, as the tables': stopped in between, the model is still the mod's own)
        target = os.path.join(self.top, "romfs", *mt.SELF_MODELS["Child"].split("/"))
        stamp = os.path.join(self.top, mt.MODELS_KEEP, *mt.SELF_MODELS["Child"].split("/")) + ".txt"
        with stopped_at(target, stamp, nth=2) as seen:   # (the second of the two writes)
            self.assertGreaterEqual(mt.merge_self_models(self.mods, "", self.lines.append), 1)
        self.assertEqual(len(seen), 2)
        mt.merge_self_models(self.mods, "", self.lines.append)   # (the next start does it again)
        self.assertIn(self.tunic.tile_name("green", self.tunic.SELF_LETTER).encode() + b"\x00", self.model(self.top, "Child"))
        mt.undo(self.mods, say=self.lines.append)
        self.assertEqual(self.model(self.top, "Child"), self.child)

    def test_a_new_copy_of_the_mod_is_done_again_from_its_own(self):
        mt.merge_self_models(self.mods, "", self.lines.append)
        newer = self.child.replace(b"FRES", b"FRES", 1) + b"new\x00"
        self.put(os.path.join(self.top, "romfs", *mt.SELF_MODELS["Child"].split("/")), newer)
        self.lines = []
        mt.merge_self_models(self.mods, "", self.lines.append)
        self.assertEqual(len(self.lines), 1)
        self.assertEqual(read(os.path.join(self.top, mt.MODELS_KEEP, *mt.SELF_MODELS["Child"].split("/"))), newer)
        self.assertTrue(self.model(self.top, "Child").endswith(b"new\x00"))

    def assert_put_back(self, warned=0):
        for m in (self.top, self.under):
            self.assertEqual(self.model(m, "Child"), self.child)
            self.assertEqual(self.model(m, "Adult"), self.adult)
            self.assertFalse(os.path.isdir(os.path.join(m, mt.MODELS_KEEP)))
        self.assertEqual(len(self.lines), 2 + warned)   # (one line a mod, and a warning a mod when the pack is on)
        self.lines = []
        self.assertEqual(mt.merge_self_models(self.mods, "", self.lines.append), 0)   # (the next start: the warnings)
        self.assertEqual(len(self.lines), warned)

    def test_the_pack_off_puts_every_mods_own_models_back(self):
        # (their copies named co-op's own tunic textures, which no mod has with the pack off: never left so)
        mt.merge_self_models(self.mods, "", self.lines.append)
        with open(os.path.join(self.mods, mt.LIST_FILE), "w", encoding="utf-8", newline="\n") as f:
            f.write("coop-mods 1\noff %s\n" % PACK)
        self.lines = []
        self.assertEqual(mt.merge_self_models(self.mods, "", self.lines.append), 0)
        self.assert_put_back()

    def test_the_pack_gone_puts_every_mods_own_models_back(self):
        mt.merge_self_models(self.mods, "", self.lines.append)
        shutil.rmtree(self.pack)
        self.lines = []
        self.assertEqual(mt.merge_self_models(self.mods, "", self.lines.append), 0)
        self.assert_put_back()

    def test_your_own_tunic_textures_gone_puts_every_mods_own_models_back(self):
        mt.merge_self_models(self.mods, "", self.lines.append)
        os.remove(os.path.join(self.pack, "romfs", "Tex",
                               self.tunic.tile_name("green", self.tunic.SELF_LETTER) + ".bntx.zs"))
        self.lines = []
        self.assertEqual(mt.merge_self_models(self.mods, "", self.lines.append), 0)
        self.assert_put_back(warned=2)   # (row 265: the pack on without them, each mod's Link models named)
        self.assertTrue(all("none of your own tunic textures" in l and "self_tunic.py" in l for l in self.lines))

    def test_the_pack_off_keeps_a_model_changed_since(self):
        # (a new copy of the mod since the start that named it: its own, never the old kept one over it)
        mt.merge_self_models(self.mods, "", self.lines.append)
        newer = self.child + b"new\x00"
        self.put(os.path.join(self.top, "romfs", *mt.SELF_MODELS["Child"].split("/")), newer)
        shutil.rmtree(self.pack)
        mt.merge_self_models(self.mods, "", self.lines.append)
        self.assertEqual(self.model(self.top, "Child"), newer)
        self.assertEqual(self.model(self.top, "Adult"), self.adult)
        self.assertFalse(os.path.isdir(os.path.join(self.top, mt.MODELS_KEEP)))

    def test_nothing_without_your_own_tunic_textures_in_the_pack(self):
        os.remove(os.path.join(self.pack, "romfs", "Tex",
                               self.tunic.tile_name("green", self.tunic.SELF_LETTER) + ".bntx.zs"))
        self.assertEqual(mt.merge_self_models(self.mods, "", self.lines.append), 0)
        self.assertEqual(self.model(self.top, "Child"), self.child)
        # Row 265: said, a line for each mod whose Link models win (the lane proof had Character Select's own model
        # and a pack whose start never put your tunic textures in: the tunic stayed the model's green, silently)
        self.assertEqual(len(self.lines), 2)
        self.assertTrue(any("OoT Character Select has its own " + mt.SELF_MODELS["Child"] + ", " +
                            mt.SELF_MODELS["Adult"] in l for l in self.lines))

    def test_no_word_about_a_mod_without_link_models_or_with_the_pack_off(self):
        os.remove(os.path.join(self.pack, "romfs", "Tex",
                               self.tunic.tile_name("green", self.tunic.SELF_LETTER) + ".bntx.zs"))
        for m in (self.top, self.under):
            for rel in mt.SELF_MODELS.values():
                os.remove(os.path.join(m, "romfs", *rel.split("/")))
        mt.merge_self_models(self.mods, "", self.lines.append)
        self.assertEqual(self.lines, [])
        shutil.rmtree(self.pack)   # (no pack: nothing of yours to name, nothing said)
        self.put(os.path.join(self.top, "romfs", *mt.SELF_MODELS["Child"].split("/")), self.child)
        mt.merge_self_models(self.mods, "", self.lines.append)
        self.assertEqual(self.lines, [])


class MergeGameDataUnderContentsTest(MergeGameDataTest):
    """The saved values list, both mods under contents/<title id>/ (player report 259)."""
    CS = "contents/0400486024DEC000/OoT Character Select"
    HAND = "contents/0400486024DEC000/OoT_Randomizer_0123456789abcdef"


class MergeMembersUnderContentsTest(MergeMembersTest):
    """The menu archives, the winning mod under contents/<title id>/ beside the pack (player report 259)."""
    MOD = "contents/0400486024DEC000/zz-voices"


if __name__ == "__main__":
    unittest.main()
