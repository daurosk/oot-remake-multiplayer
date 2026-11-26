"""Tests for the Character Select voice rules (voice_map.py). Made-up names and numbers only: no game files.

    python tools/puppet_pack/tests/test_voice_map.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import voice_map as vm  # noqa: E402

LINK_NAMES = {"Child": {"Vo_PlChild_SwordAttack_S_01", "Vo_PlChild_SwordAttack_S_02", "Vo_PlChild_Wait_01"}}


def clip(link_name, source="NV_Custom_Saria_SurpriseS00", length=0.30, longest=0.25, rms=-24.0, link_rms=-24.0):
    return vm.Clip("Custom_Saria", "Child", link_name, source, length, longest, rms, link_rms)


def full(**kw):
    return [clip(n, **kw) for n in sorted(LINK_NAMES["Child"])]


class Names(unittest.TestCase):
    def test_event_of(self):
        self.assertEqual(vm.event_of("Vo_PlAdult_SwordAttack_S_03"), "SwordAttack_S")
        self.assertEqual(vm.event_of("Vo_PlChild_DrinkBreath00"), "DrinkBreath00")
        self.assertEqual(vm.event_of("Vo_PlChild_Wait_31_02"), "Wait_31")
        self.assertIsNone(vm.event_of("NV_Saria_SurpriseS00"))

    def test_kind_of(self):
        self.assertEqual(vm.kind_of("NV_Biggoron_SurpriseS02"), "SurpriseS")
        self.assertEqual(vm.kind_of("NV_Hylia_Elderly_Woman_ApothecaryOldWoman_Sigh00"), "Sigh")
        self.assertIsNone(vm.kind_of("Vo_PlChild_Jump_S_01"))


class Classes(unittest.TestCase):
    def test_speech_laughs_and_cutscene_lines_are_refused(self):
        for name in ("NV_Hylia_Woman_1_TalkPositive00", "NV_Hylia_Boy_LaughS01", "NV_Hylia_Woman_1_Greeting00",
                     "NV_Hylia_Girl_Spry_Humming00", "NV_Custom_Saria_Dm_Forest_0010", "NV_Custom_Saria_Sp03",
                     "NV_Hylia_Woman_1_Sad00", "NV_Hylia_Woman_1_QuestionNormal01"):
            self.assertIsNone(vm.clip_class(name), name)

    def test_wordless_kinds_are_classed(self):
        self.assertEqual(vm.clip_class("NV_Custom_Impa_WeaponSwingS00"), "ATTACK")
        self.assertEqual(vm.clip_class("NV_Hylia_Boy_Painful01"), "DAMAGE")
        self.assertEqual(vm.clip_class("NV_Hylia_Boy_SurpriseL00"), "SURPRISE")
        self.assertEqual(vm.clip_class("NV_Hylia_Woman_1_Stretch00"), "BREATH")

    def test_enemy_clips(self):
        self.assertEqual(vm.clip_class("Enemy_Wolfos_AttackVoice_00"), "ATTACK")
        self.assertEqual(vm.clip_class("Enemy_Wolfos_DamageVoice_01"), "DAMAGE")
        self.assertIsNone(vm.clip_class("Enemy_Wolfos_AttackVoice_Delay_00"))
        self.assertIsNone(vm.clip_class("Enemy_Wolfos_Footstep_00"))
        self.assertEqual(vm.clip_class("NV_Cow_Angry00"), "ATTACK")

    def test_every_event_has_every_class(self):
        for ev in ("SwordAttack_S", "Damage_B", "Jump_S", "Shield_B", "Wait_31", "Dm_TimeTemple_0030", "Nod",
                   "FallingScream", "HorseSpurs_B01", "SomethingNew"):
            self.assertEqual(sorted(vm.prefs(ev)), sorted(vm.CLASSES), ev)

    def test_event_classes(self):
        self.assertEqual(vm.event_class("SwordAttack_B"), "ATTACK")
        self.assertEqual(vm.event_class("Damage_S"), "DAMAGE")
        self.assertEqual(vm.event_class("FallingScream"), "DAMAGE")
        self.assertEqual(vm.event_class("Climb"), "EFFORT")
        self.assertEqual(vm.event_class("Dm_SageRoom_0005"), "SURPRISE")
        self.assertEqual(vm.event_class("Wait_42"), "BREATH")
        self.assertEqual(vm.prefs("Shield_S")[0], "EFFORT")

    def test_cow_clips(self):
        self.assertEqual(vm.clip_class("SE_Animal_Cow_DamagedAngry_00"), "DAMAGE")
        self.assertEqual(vm.clip_class("SE_Animal_Cow_Roar_01"), "ATTACK_BIG")
        self.assertEqual(vm.clip_class("NV_Cow_Sad00"), "DIE")
        self.assertEqual(vm.clip_class("NV_Cow_Happy01"), "BREATH")
        self.assertEqual(vm.clip_class("NV_Cow_TalkPositive02"), "BREATH")
        self.assertIsNone(vm.clip_class("SE_Animal_Cow_Moo_00"))   # the same sound as TalkPositive
        self.assertIsNone(vm.clip_class("SE_Animal_Cow_DummyBlank"))

    def test_own_classes(self):
        self.assertEqual(vm.own("SwordAttack_B"), ("ATTACK_BIG", "ATTACK"))
        self.assertEqual(vm.own("JumpAttack"), ("ATTACK_BIG", "ATTACK"))
        self.assertEqual(vm.own("Death"), ("DIE", "DAMAGE"))
        self.assertEqual(vm.own("SwordAttack_S"), ("ATTACK",))
        self.assertEqual(vm.own("Damage_S"), ("DAMAGE",))
        self.assertEqual(vm.own("Wait_31"), ("BREATH",))
        self.assertEqual(vm.own("Shield_B"), ("EFFORT",))
        for ev in ("SwordAttack_B", "Death", "Wait_31", "Shield_B", "SomethingNew"):
            self.assertEqual(vm.prefs(ev)[:len(vm.own(ev))], vm.own(ev), ev)


class Clips(unittest.TestCase):
    def test_good_clips_pass(self):
        self.assertEqual(vm.check_clips(full(), LINK_NAMES), [])

    def test_a_clip_of_links_voice_is_refused_by_itself(self):
        # Each gate stands alone: check_clips refuses Link's voice without check_map having run.
        c = full()
        c[0] = c[0].__class__(**{**c[0].__dict__, "voice": "Dark_Link"})
        c[1] = c[1].__class__(**{**c[1].__dict__, "source": "PlayerVoice_Child_Saria_Jump"})
        bad = vm.check_clips(c, LINK_NAMES)
        self.assertTrue(bad[0].startswith("Dark_Link Child Vo_PlChild_SwordAttack_S_01"), bad)
        self.assertTrue(bad[0].endswith("an archive made from Link's voice"), bad)
        self.assertTrue(bad[1].endswith("(PlayerVoice_Child_Saria_Jump): Link's own voice"), bad)

    def test_frequent_event_over_the_cap_is_an_outlier(self):
        bad = vm.check_clips(full(length=0.41), LINK_NAMES)
        self.assertEqual(len(bad), 2)
        self.assertIn("longer than Link's 0.25 s + 0.15 s", bad[0])

    def test_exactly_at_the_cap_passes(self):
        self.assertEqual(vm.check_clips(full(length=0.25 + vm.MARGIN_S), LINK_NAMES), [])

    def test_rare_event_has_no_length_cap(self):
        c = [clip(n, length=3.0 if "Wait" in n else 0.3) for n in sorted(LINK_NAMES["Child"])]
        self.assertEqual(vm.check_clips(c, LINK_NAMES), [])

    def test_loudness(self):
        self.assertEqual(len(vm.check_clips(full(rms=-22.9), LINK_NAMES)), 3)
        self.assertEqual(len(vm.check_clips(full(rms=-27.1), LINK_NAMES)), 3)
        self.assertEqual(vm.check_clips(full(rms=-23.0), LINK_NAMES), [])
        self.assertEqual(vm.check_clips(full(rms=-27.0), LINK_NAMES), [])

    def test_speech_and_links_own_voice_are_refused(self):
        c = full()
        c[0].source = "NV_Custom_Saria_TalkPositive00"
        c[1].source = "Vo_PlChild_SwordAttack_S_02"
        bad = vm.check_clips(c, LINK_NAMES)
        self.assertEqual(len(bad), 2)
        self.assertIn("not allowed", bad[0])
        self.assertIn("Link's own voice", bad[1])

    def test_a_missing_slot_would_play_links_voice(self):
        bad = vm.check_clips(full()[:2], LINK_NAMES)
        self.assertEqual(len(bad), 1)
        self.assertIn("1 of Link's clips have no stand-in", bad[0])

    def test_unknown_link_name(self):
        bad = vm.check_clips([clip("Vo_PlChild_Nope_01")] + full(), LINK_NAMES)
        self.assertIn("not one of Link's voice clips", bad[0])


class Map(unittest.TestCase):
    VOICES = {"Custom_Saria", "Hylia_Girl_Spry", "Enemy_Wolfos", "Enemy_ReDead"}
    CREATURES = {"Enemy_Wolfos", "Enemy_ReDead"}

    def test_good_map(self):
        chars = [vm.Character("Link", (), "Link"),
                 vm.Character("Saria", ("Custom_Saria",), "Hylia_Girl_Spry"),
                 vm.Character("Gibdo", ("Enemy_ReDead",), "Enemy_ReDead", creature=True)]
        self.assertEqual(vm.check_map(chars, self.VOICES, self.CREATURES), [])

    def test_link_voice_unbuilt_voice_and_wrong_defaults(self):
        chars = [vm.Character("Saria", ("Link",), "Hylia_Girl_Spry"),
                 vm.Character("Mido", ("Mido",), "Hylia_Boy"),
                 vm.Character("Wolfos", ("Enemy_Wolfos",), "Hylia_Girl_Spry", creature=True),
                 vm.Character("Malon", ("Custom_Saria",), "Enemy_Wolfos")]
        bad = vm.check_map(chars, self.VOICES, self.CREATURES)
        self.assertEqual(len(bad), 4)
        self.assertIn("keeps Link's voice", bad[0])
        self.assertIn("default voice Hylia_Boy was not built", bad[1])
        self.assertIn("a creature with the person's voice", bad[2])
        self.assertIn("a person with the creature voice", bad[3])

    def test_a_voice_made_from_links_clips_is_refused(self):
        chars = [vm.Character("Dark Link", ("Enemy_MiddleBoss_DarkLink",), "Hylia_Girl_Spry"),
                 vm.Character("Saria", ("Custom_Saria",), "Player_Child")]
        bad = vm.check_map(chars, self.VOICES | {"Enemy_MiddleBoss_DarkLink", "Player_Child"}, self.CREATURES)
        self.assertEqual(len(bad), 2)
        self.assertIn("Dark Link: keeps Link's voice (Enemy_MiddleBoss_DarkLink)", bad[0])
        self.assertIn("Saria: keeps Link's voice (Player_Child)", bad[1])

    def test_links_models_keep_links_clips(self):
        self.assertEqual(vm.LINK_VARIANTS, {"Link", "Retro Link", "Link (opening)", "Link (adult)", "Dark Link"})
        chars = [vm.Character(n, (), vm.LINK) for n in sorted(vm.LINK_VARIANTS)]
        self.assertEqual(vm.check_map(chars, self.VOICES, self.CREATURES), [])

    def test_only_links_models_may_keep_links_clips(self):
        bad = vm.check_map([vm.Character("Saria", (), vm.LINK)], self.VOICES, self.CREATURES)
        self.assertEqual(bad, ["Saria: keeps Link's voice but is not one of Link's models"])

    def test_a_chain_voice_without_clips_is_passed_over(self):
        chars = [vm.Character("Ruto", ("Custom_Ruto_Adult",), "Hylia_Girl_Spry")]
        self.assertEqual(vm.check_map(chars, self.VOICES, self.CREATURES), [])

    def test_check_raises_with_every_problem(self):
        chars = [vm.Character("Saria", ("Custom_Saria",), "Hylia_Girl_Spry")]
        vm.check(chars, self.VOICES, self.CREATURES, full(), LINK_NAMES)
        with self.assertRaises(ValueError) as e:
            vm.check(chars, set(), self.CREATURES, full(length=1.0), LINK_NAMES)
        self.assertIn("was not built", str(e.exception))
        self.assertIn("longer than", str(e.exception))


if __name__ == "__main__":
    unittest.main()
