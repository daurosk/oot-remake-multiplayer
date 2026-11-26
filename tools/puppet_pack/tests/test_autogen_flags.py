"""Tests for autogen_flags.py (the unnamed room bools of the dungeons). Synthetic data only.

    python tools/puppet_pack/tests/test_autogen_flags.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import autogen_flags as af  # noqa: E402


def node(kind, inst="", ins=(), outs=()):
    return {"type": kind, "imm": {"InstanceName": inst} if inst else {}, "in": {"bool": list(ins)},
            "out": {"bool": list(outs)}}


class AutogenFlagsTest(unittest.TestCase):
    def test_murmur3_is_the_flag_hash(self):
        # A named flag of coop_dungeon.cpp and the empty string's known value.
        self.assertEqual(af.murmur3("HavePlayedEvent.FireTemple_LargePillarFallCamera"), 0x16E53BFC)
        self.assertEqual(af.murmur3(""), 0)
        for tail in ("a", "ab", "abc", "abcd", "abcde"):  # every tail length runs
            self.assertIsInstance(af.murmur3(tail), int)

    def test_the_two_names_checked_in_a_game(self):
        # Deku Tree torch plate: group hash, top-level graph, a positive GUID; seen set in a game.
        name = af.flag_name(12728001227791831731, "", 72078894)
        self.assertEqual(name, "AutogenGameDataLogicNode.1272800122779183173172078894")
        self.assertEqual(af.murmur3(name), 0x0F8F3FA3)
        # Fire Temple big pillar: the GUID word 3062117726 is -1232849570 signed; the event flow names it.
        name = af.flag_name(15744991116824525035, "", 3062117726)
        self.assertEqual(name, "AutogenGameDataLogicNode.15744991116824525035-1232849570")
        self.assertEqual(af.murmur3(name), 0xA82CC7A2)

    def test_a_nested_module_adds_its_path_without_slashes(self):
        self.assertEqual(af.module_path("_85c6/_4c4c/AiGroup"), "_85c6_4c4c")
        self.assertEqual(af.module_path("_9ee1/AiGroup"), "_9ee1")
        self.assertEqual(af.module_path("AiGroup"), "")
        self.assertEqual(af.flag_name(1, "_85c6", 5), "AutogenGameDataLogicNode.1_85c65")

    def test_room_state_is_shared_and_a_players_own_is_not(self):
        pillar = node("ActorLogicHammerDrop", "Obj_FireTemple_HammerDropPillar_A_01_ca81", outs=("Buried",))
        alloc = node(af.NODE, ins=[("IsWrite", 0, 0)])
        self.assertEqual(af.kind_of([pillar, alloc], alloc)[0], "room")

        pot = node("ActorLogicSimpleActor", "Obj_Pot_A_05_Respawn_eea3", outs=("Dead",))
        pulse = node("OperateANDPulse", ins=[("BoolMulti", 0, 0)], outs=("False", "True"))
        refill = node(af.NODE, ins=[("Bool", 1, 1), ("IsWrite", 1, 1)])
        self.assertEqual(af.kind_of([pot, pulse, refill], refill)[0], "player")

        area = node("HoldActorObserverBySensorHitInfo", outs=("IsHit", "IsNotHit"))
        gate = node("OperateANDFixInputTrue", ins=[("BoolMulti", 0, 0)], outs=("False", "True"))
        walked = node(af.NODE, ins=[("IsWrite", 1, 1)])
        self.assertEqual(af.kind_of([area, gate, walked], walked)[0], "player")

        stone = node(af.NODE, ins=[("IsWrite", 0, 0)])
        self.assertEqual(af.kind_of([pillar, stone], stone, "GossipStoneWithFairySpirit_d931")[0], "player")

    def test_only_dungeons_are_scanned_by_default(self):
        self.assertIn("FireTemple", af.DUNGEONS)
        self.assertNotIn("GerudoFortress", af.DUNGEONS)
        self.assertEqual(len(af.DUNGEONS), 13)


if __name__ == "__main__":
    unittest.main()
