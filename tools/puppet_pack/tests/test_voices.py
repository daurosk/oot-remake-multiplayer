"""Tests for the Character Select voice builder (voices.py). Made-up archives and clips only: no game files.

    python tools/puppet_pack/tests/test_voices.py
"""
import os
import sys
import unittest
import zlib
from types import SimpleNamespace
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from puppet_pack import bwav, voice_map as vm, voices  # noqa: E402
from test_bwav import amta, sine, template  # noqa: E402

RATE = 48000


def clip(x, prefetch=False):
    """(archive BWAV, stream file or None) for samples x."""
    (coefs, data), = bwav.encode_adpcm([x])
    return bwav.clip_bytes(template(1, 280, len(x)) if prefetch else template(0, 14), len(x), coefs, data)


class Files:
    """A made-up game: archives {rel: [(name, samples, prefetch)]}."""

    def __init__(self, archives):
        self.archives, self.streams = {}, {}
        for rel, rows in archives.items():
            assets = []
            for name, x, prefetch in rows:
                head, stream = clip(x, prefetch)
                if stream is not None:
                    self.streams[f"{voices.STREAM}/{name}.bwav"] = stream
                assets.append((zlib.crc32(name.encode()), amta(name), head))
            self.archives[rel] = bwav.bars_build(assets)

    def bars(self, rel):
        return self.archives.get(rel)

    def stream(self, rel):
        return self.streams.get(rel)


def game(saria=True):
    link = {}
    for age in ("Child", "Adult"):
        p = f"Vo_Pl{age}_"
        link[f"{voices.RESOURCE}/Player_{age}.bars"] = [
            (p + "SwordAttack_S_01", sine(int(0.30 * RATE), 300), False),
            (p + "SwordAttack_S_02", sine(int(0.25 * RATE), 320), False),
            (p + "Wait_01", sine(int(0.80 * RATE), 200, 3000), True),
            ("SE_Player_Not_A_Voice", sine(2000), False)]
    banks = {
        "NV_Hylia_Girl_Spry": [("NV_Hylia_Girl_Spry_Painful00", sine(int(0.30 * RATE), 600, 2000), False)],
        "Enemy_ReDead": [("Enemy_ReDead_DamageVoice_00", sine(int(0.40 * RATE), 90, 12000), False),
                         ("Enemy_ReDead_Footstep_00", sine(int(0.10 * RATE), 90), False)],
    }
    if saria:
        banks["NV_USen_Custom_Saria"] = [
            ("NV_Custom_Saria_WeaponSwingS00", sine(int(0.33 * RATE), 700, 1500), False),
            ("NV_Custom_Saria_WeaponSwingS01", sine(int(0.90 * RATE), 650, 1500), False),   # too long for a swing
            ("NV_Custom_Saria_TalkPositive00", sine(int(0.30 * RATE), 500), False),         # speech: never used
            ("NV_Custom_Saria_Ease00", sine(int(1.20 * RATE), 400, 1500), True)]
    link.update({f"{voices.RESOURCE}/{b}.bars": rows for b, rows in banks.items()})
    return Files(link)


TABLE = {"Link": {"link": True},
         "Saria": {"chain": ["Custom_Saria"], "default": "Hylia_Girl_Spry", "creature": False},
         "Fado": {"chain": ["Kokiri_Girl_Spry"], "default": "Hylia_Girl_Spry", "creature": False},
         "Gibdo": {"chain": ["Enemy_ReDead"], "default": "Enemy_ReDead", "creature": True}}
CHARS = {0: "Link", 2: "Saria", 7: "Fado", 108: "Gibdo"}


class Build(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = game()
        cls.out = voices.build(cls.files, CHARS, TABLE, log=lambda *_: None)

    def test_each_character_but_link_gets_an_archive_per_age(self):
        for age in ("Child", "Adult"):
            got = self.out["lists"][age]
            self.assertNotIn(0, got)
            self.assertEqual(got[2], f"CoopVoice_{age}_Custom_Saria.bars")
            self.assertEqual(got[7], f"CoopVoice_{age}_Kokiri_Girl_Spry.bars")
            self.assertEqual(got[108], f"CoopVoice_{age}_Enemy_ReDead.bars")
            self.assertIn(f"{voices.RESOURCE}/{got[2]}", self.out["files"])

    def test_every_link_voice_clip_has_a_stand_in_and_nothing_else(self):
        d = self.out["files"][f"{voices.RESOURCE}/CoopVoice_Child_Custom_Saria.bars"]
        assets, _ = bwav.read_bars(d)
        want = {zlib.crc32(n.encode()) for n in ("Vo_PlChild_SwordAttack_S_01", "Vo_PlChild_SwordAttack_S_02",
                                                 "Vo_PlChild_Wait_01")}
        self.assertEqual(set(assets), want)

    def test_clips_come_from_the_characters_own_voice_and_fit(self):
        by = {(c.voice, c.link_name): c for c in self.out["clips"]}
        swing = by["CoopVoice_Child_Custom_Saria", "Vo_PlChild_SwordAttack_S_01"]
        self.assertEqual(swing.source, "NV_Custom_Saria_WeaponSwingS00")
        self.assertLessEqual(swing.length, swing.link_longest + 0.15)
        self.assertEqual(by["CoopVoice_Child_Custom_Saria", "Vo_PlChild_Wait_01"].source,
                         "NV_Custom_Saria_Ease00")
        self.assertNotIn("NV_Custom_Saria_TalkPositive00", {c.source for c in self.out["clips"]})
        # A voice type the game lacks falls to the default of the character's sex and age.
        self.assertEqual(by["CoopVoice_Child_Kokiri_Girl_Spry", "Vo_PlChild_Wait_01"].source,
                         "NV_Hylia_Girl_Spry_Painful00")
        self.assertEqual(by["CoopVoice_Child_Enemy_ReDead", "Vo_PlChild_SwordAttack_S_01"].source,
                         "Enemy_ReDead_DamageVoice_00")

    def test_loudness_meets_links(self):
        for c in self.out["clips"]:
            self.assertLessEqual(abs(c.rms_db - c.link_rms_db), 3.0, (c.voice, c.link_name))

    def test_a_streamed_slot_gets_its_own_stream_file(self):
        d = self.out["files"][f"{voices.RESOURCE}/CoopVoice_Child_Custom_Saria.bars"]
        assets, _ = bwav.read_bars(d)
        meta, at = assets[zlib.crc32(b"Vo_PlChild_Wait_01")]
        name = bwav.amta_name(meta)
        self.assertEqual(len(name), len("Vo_PlChild_Wait_01"))
        self.assertRegex(name, r"^V[0-9][a-z]PlChild_")   # never the mod's V<2 letters>Pl... nor Link's Vo_Pl...
        stream = self.out["files"][f"{voices.STREAM}/{name}.bwav"]
        self.assertEqual(stream[8:12], d[at + 8:at + 12])
        x, _ = bwav.decode(stream)
        self.assertGreater(np.abs(x.astype(np.int32)).max(), 1000)

    def test_list_entry_puts_the_voice_archive_first(self):
        base = {"Bars": ["Player.bars", "Player_Child.bars"], "ChoiceAsset": ["a.casset.byml"]}
        e = voices.list_entry(base, "CoopVoice_Child_Custom_Saria.bars")
        self.assertEqual(e["Bars"], ["CoopVoice_Child_Custom_Saria.bars", "Player.bars", "Player_Child.bars"])
        self.assertEqual(e["ChoiceAsset"], ["a.casset.byml"])
        self.assertEqual(base["Bars"], ["Player.bars", "Player_Child.bars"])


def burst(seconds, at, amp=20000, rate=RATE):
    """A quiet clip `seconds` long with one loud 20 ms burst `at` seconds in."""
    x = sine(int(seconds * rate), 300, 500)
    i = int(at * rate)
    x[i:i + rate // 50] = sine(rate // 50, 300, amp)
    return x


def chain(*voices_):
    """Voices for choose(): each a {class: [(name, samples)]}."""
    return [SimpleNamespace(voice="V%d" % i, by_class={c: [(nm, x, voices.rms_db(x)) for nm, x in rows]
                                                       for c, rows in v.items()})
            for i, v in enumerate(voices_)]


class Choose(unittest.TestCase):
    def test_a_capped_clip_keeps_its_peak(self):
        x = burst(1.0, 0.60)
        y = voices.cut(x, RATE, 0.40)
        self.assertEqual(len(y), int(0.40 * RATE))
        self.assertGreater(np.abs(y.astype(np.int32)).max(), 15000)
        early = burst(1.0, 0.05)
        self.assertTrue(np.array_equal(voices.cut(early, RATE, 0.40)[:4800], early[:4800]))   # from the start
        self.assertIs(voices.cut(x, RATE, 2.0), x)

    def test_a_capped_event_keeps_every_peak_and_rotates_its_clips(self):
        v = chain({"ATTACK": [("A%d" % i, burst(1.0, 0.5 + 0.1 * i)) for i in range(3)]})
        got = voices.choose("SwordAttack_S", v, 5, 0.40, RATE)
        self.assertEqual([g[2] for g in got], ["A0", "A1", "A2", "A0", "A1"])
        for g in got:
            self.assertEqual(len(g[3]), int(0.40 * RATE))
            self.assertGreater(np.abs(g[3].astype(np.int32)).max(), 15000, g[2])

    def test_a_class_missing_from_the_first_voice_is_found_later_in_the_chain(self):
        v = chain({"DAMAGE": [("hurt", sine(9600))]}, {"ATTACK": [("swing", sine(9600))]})
        self.assertEqual(voices.choose("SwordAttack_S", v, 1, None, RATE)[0][:3], ("V1", "ATTACK", "swing"))
        self.assertEqual(voices.choose("Damage_S", v, 1, None, RATE)[0][:3], ("V0", "DAMAGE", "hurt"))

    def test_the_first_voice_keeps_what_no_voice_has_the_events_own_class_for(self):
        v = chain({"SURPRISE": [("gasp", sine(9600))]}, {"BREATH": [("sigh", sine(9600))]})
        self.assertEqual(voices.choose("SwordAttack_S", v, 1, None, RATE)[0][:3], ("V0", "SURPRISE", "gasp"))
        v = chain({"ATTACK": [("mine", sine(9600))]}, {"ATTACK": [("theirs", sine(9600))]})
        self.assertEqual(voices.choose("SwordAttack_S", v, 1, None, RATE)[0][:3], ("V0", "ATTACK", "mine"))

    def test_a_chain_whose_first_voice_has_every_class_picks_as_before(self):
        def before(event, voices_, n, cap):   # choose() of 50b46faf when a clip fits
            for v in voices_:
                for c in vm.prefs(event):
                    fits = [k for k in v.by_class.get(c, []) if cap is None or len(k[1]) <= cap * RATE]
                    if fits:
                        return [(v.voice, c) + fits[i % len(fits)] for i in range(n)]
        own = {c: [("own_%s%d" % (c, i), sine(int((0.2 + 0.05 * i) * RATE))) for i in range(2)]
               for c in ("ATTACK", "EFFORT", "DAMAGE", "SURPRISE", "BREATH")}
        v = chain(own, {c: [("default_" + c, sine(4800))] for c in own})
        for ev in ("SwordAttack_S", "SwordAttack_B", "JumpAttack", "Jump_S", "Climb", "Shield_S", "Damage_B", "Death",
                   "Wait", "Wait_31", "Dm_TimeTemple_0030", "FallingScream", "SomethingNew"):
            for cap in (None, 0.30):
                got = voices.choose(ev, v, 4, cap, RATE)
                self.assertEqual([g[:3] for g in got], [g[:3] for g in before(ev, v, 4, cap)], (ev, cap))
                self.assertEqual({g[0] for g in got}, {"V0"}, ev)

    def test_mido_keeps_his_own_voice_but_for_the_class_he_lacks(self):
        # Mido's own voice has surprise only; Kokiri_Boy after it has attack, surprise and breath.
        mido = chain({"SURPRISE": [("NV_Mido_Notice00", sine(9600))]},
                     {"ATTACK": [("NV_Kokiri_Boy_WeaponSwingS00", sine(9600))],
                      "SURPRISE": [("NV_Kokiri_Boy_SurpriseS00", sine(9600))],
                      "BREATH": [("NV_Kokiri_Boy_Ease00", sine(9600))]})
        for ev in ("Climb", "Jump_S", "Shield_S", "Damage_S", "Dm_TimeTemple_0030"):   # nobody's own class, or his
            self.assertEqual(voices.choose(ev, mido, 1, None, RATE)[0][:3], ("V0", "SURPRISE", "NV_Mido_Notice00"), ev)
        self.assertEqual(voices.choose("SwordAttack_S", mido, 1, None, RATE)[0][2], "NV_Kokiri_Boy_WeaponSwingS00")
        self.assertEqual(voices.choose("Wait", mido, 1, None, RATE)[0][2], "NV_Kokiri_Boy_Ease00")

    def test_the_events_own_class_is_cut_before_a_worse_class_that_fits(self):
        v = chain({"ATTACK": [("long", burst(1.0, 0.6))], "DAMAGE": [("hurt", sine(4800))]})
        got = voices.choose("SwordAttack_S", v, 1, 0.40, RATE)[0]
        self.assertEqual(got[:3], ("V0", "ATTACK", "long"))
        self.assertEqual(len(got[3]), int(0.40 * RATE))

    def test_a_fitting_clip_from_a_later_voice_wins_over_cutting_the_first_voices_clip(self):
        v = chain({"ATTACK": [("long", burst(1.0, 0.6))]}, {"ATTACK": [("short", burst(0.3, 0.1))]})
        got = voices.choose("SwordAttack_S", v, 2, 0.40, RATE)
        self.assertEqual([g[:3] for g in got], [("V1", "ATTACK", "short")] * 2)
        self.assertEqual(len(got[0][3]), int(0.3 * RATE))   # whole, not cut
        # Breath: the own voice has only a 2 s clip, the default a 0.3 s one.
        v = chain({"BREATH": [("own_sigh", burst(2.0, 1.2))]}, {"BREATH": [("default_sigh", burst(0.3, 0.1))]})
        got = voices.choose("Wait", v, 1, 0.40, RATE)[0]
        self.assertEqual(got[:3], ("V1", "BREATH", "default_sigh"))
        self.assertEqual(len(got[3]), int(0.3 * RATE))

    def test_the_cow(self):
        names = {"NV_Cow": ["NV_Cow_Angry00", "NV_Cow_Angry01", "NV_Cow_Happy00", "NV_Cow_Sad00",
                            "NV_Cow_TalkPositive00"],
                 "Animal_Cow": ["SE_Animal_Cow_DamagedAngry_00", "SE_Animal_Cow_DamagedAngry_01",
                                "SE_Animal_Cow_Roar_00", "SE_Animal_Cow_Moo_00"]}
        cow = []
        for bank in ("NV_Cow", "Animal_Cow"):     # voice_table.json: chain NV_Cow, default Animal_Cow
            by = {}
            for nm in names[bank]:
                if vm.clip_class(nm):
                    by.setdefault(vm.clip_class(nm), []).append((nm, burst(2.5 if "Roar" in nm else 0.8, 0.5)))
            cow.append(by)
        cow = chain(*cow)

        def picks(event, cap=None):
            return {g[2] for g in voices.choose(event, cow, 3, cap, RATE)}
        self.assertEqual(picks("SwordAttack_B", 0.83), {"SE_Animal_Cow_Roar_00"})
        self.assertEqual(picks("SwordAttack_S", 0.40), {"NV_Cow_Angry00", "NV_Cow_Angry01"})
        self.assertEqual(picks("Damage_S"), {"SE_Animal_Cow_DamagedAngry_00", "SE_Animal_Cow_DamagedAngry_01"})
        self.assertEqual(picks("Death"), {"NV_Cow_Sad00"})
        for ev in ("Wait", "Wait_31", "Jump_S", "Climb", "Shield_S", "lifting"):
            self.assertFalse(any("Damaged" in p for p in picks(ev, 0.40)), ev)
        self.assertEqual(picks("Wait"), {"NV_Cow_Happy00", "NV_Cow_TalkPositive00"})


class Silent(unittest.TestCase):
    def test_every_one_of_links_clips_gets_silence(self):
        files = game()
        out = voices.silent(files, log=lambda *_: None)
        for age in ("Child", "Adult"):
            self.assertEqual(out["archives"][age], f"CoopVoice_{age}_Silent.bars")
            d = out["files"][f"{voices.RESOURCE}/CoopVoice_{age}_Silent.bars"]
            got, _ = bwav.read_bars(d)
            link = [n for n in bwav.bars_names(files.bars(f"{voices.RESOURCE}/Player_{age}.bars"))
                    if n.startswith("Vo_Pl")]
            self.assertEqual(sorted(got), sorted(zlib.crc32(n.encode()) for n in link))   # Link's clips, all of them
            for h, (meta, at) in got.items():
                sname = bwav.amta_name(meta)
                if zlib.crc32(sname.encode()) == h:
                    x, _ = bwav.decode(d, at)
                else:
                    self.assertRegex(sname, f"^V9zPl{age}_")
                    x, _ = bwav.decode(out["files"][f"{voices.STREAM}/{sname}.bwav"])
                self.assertFalse(np.any(x), sname)
        self.assertIn(f"{voices.STREAM}/V9zPlChild_Wait_01.bwav", out["files"])

    def test_it_needs_no_numpy(self):
        files = game()
        with mock.patch.object(voices, "np", None), mock.patch.object(bwav, "np", None):
            out = voices.silent(files, log=lambda *_: None)
            self.assertEqual(len(out["archives"]), 2)
            with self.assertRaisesRegex(ImportError, "numpy"):
                voices.build(files, CHARS, TABLE, log=lambda *_: None)

    def test_stream_codes_never_match_the_mods(self):
        codes = [voices.stream_code(i) for i in range(259)]
        self.assertEqual(len(set(codes)), 259)
        self.assertNotIn(voices.SILENT_CODE, codes)
        self.assertTrue(all(c[0].isdigit() and c[1].islower() for c in codes))
        with self.assertRaises(voices.VoiceError):
            voices.stream_code(259)


class Refused(unittest.TestCase):
    def test_a_character_not_in_the_table(self):
        with self.assertRaises(voices.VoiceError):
            voices.build(game(), {2: "Saria", 50: "Someone New"}, TABLE, log=lambda *_: None)

    def test_a_missing_default_voice_stops_the_voices(self):
        table = dict(TABLE, Saria={"chain": ["Custom_Saria"], "default": "Hylia_Woman_1", "creature": False})
        with self.assertRaises(ValueError) as e:
            voices.build(game(saria=False), {2: "Saria"}, table, log=lambda *_: None)
        self.assertIn("default voice Hylia_Woman_1 was not built", str(e.exception))

    def test_links_other_models_build_with_links_clips(self):
        table = dict(TABLE, **{"Link (opening)": {"link": True}})
        out = voices.build(game(), {1: "Link", 116: "Link (opening)", 2: "Saria"}, table, log=lambda *_: None)
        self.assertEqual(set(out["lists"]["Child"]), {2})

    def test_every_link_model_and_no_other_keeps_links_clips(self):
        table = voices.load_table()
        self.assertEqual({n for n, r in table.items() if r.get("link")},
                         {"Link", "Retro Link", "Link (opening)", "Link (adult)", "Dark Link"})
        chars = [voices.vm.Character("Dark Link", (), voices.vm.LINK)]
        self.assertEqual(voices.vm.check_map(chars, set(), set()), [])   # Dark Link sounds like Link

    def test_bank_names(self):
        self.assertEqual(voices.bank_of("Custom_Saria"), "NV_USen_Custom_Saria")
        self.assertEqual(voices.bank_of("Hylia_Boy"), "NV_Hylia_Boy")
        self.assertEqual(voices.bank_of("Enemy_Wolfos"), "Enemy_Wolfos")
        self.assertEqual(voices.bank_of("NV_Cow"), "NV_Cow")

    def test_the_shipped_table_names_a_default_for_every_character(self):
        table = voices.load_table()
        for name, row in table.items():
            if row.get("link"):
                continue
            self.assertTrue(row["chain"] and row["default"], name)
            for v in row["chain"] + [row["default"]]:
                self.assertFalse(voices.vm.is_link_voice(v), (name, v))   # no non-Link character uses Link's clips
            if row["creature"]:
                self.assertTrue(row["default"].startswith(("Enemy_", "Animal_", "NV_Cow")), name)


if __name__ == "__main__":
    unittest.main()
