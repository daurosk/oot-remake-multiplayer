"""Tests for the puppet pack builder. Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_puppet_pack.py
"""
import contextlib
import hashlib
import io
import json
import os
import random
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import asb_carry, build, byml, dumpcheck, dungeon_rooms, map_captures, nup, place_rooms, rstb, sarc, zs  # noqa: E402

# The co-op HUD's tests run with these (CI runs this file).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_hud import *  # noqa: E402,F401,F403
from test_self_tunic import *  # noqa: E402,F401,F403
from test_merge_tables import *  # noqa: E402,F401,F403
from test_eye_switches import *  # noqa: E402,F401,F403
from test_room_graph import *  # noqa: E402,F401,F403
from test_shared_anchors import *  # noqa: E402,F401,F403
from test_look_metal import *  # noqa: E402,F401,F403

try:
    import zstandard
except ImportError:  # pragma: no cover
    zstandard = None


def sample_doc():
    return {
        "Str": "hello",
        "Int": -5,
        "Float": 1.5,
        "Bool": True,
        "Null": None,
        "UInt": byml.UInt(4000000000),
        "Int64": byml.Int64(-(1 << 40)),
        "UInt64": byml.UInt64(1 << 63),
        "Double": byml.Double(0.1),
        "Binary": byml.Binary(b"\x01\x02\x03"),
        "Array": [1, "two", [3.0], {"k": "v"}],
        "Shared": [{"same": 1}, {"same": 1}],
        "Empty": {},
        "EmptyArray": [],
        "Hash": byml.Hash32({7: "seven", 3: 3}),
        "Remap": byml.Hash32Remap({9: ("nine", 1)}),
        "Unicode": "é✓",
    }



def murmur3(text, seed=0):
    """MurmurHash3 x86 32-bit, the hash of the game's enum names in its parameter files."""
    data = text.encode()
    h, c1, c2, m = seed, 0xCC9E2D51, 0x1B873593, 0xFFFFFFFF
    n = len(data) // 4
    for i in range(n):
        k = int.from_bytes(data[4 * i:4 * i + 4], "little")
        k = (k * c1) & m
        k = ((k << 15) | (k >> 17)) & m
        h ^= (k * c2) & m
        h = ((h << 13) | (h >> 19)) & m
        h = (h * 5 + 0xE6546B64) & m
    tail = data[4 * n:]
    if tail:
        k = int.from_bytes(tail, "little")
        k = (k * c1) & m
        k = ((k << 15) | (k >> 17)) & m
        h ^= (k * c2) & m
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & m
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & m
    return h ^ (h >> 16)

def with_body(members):
    """fake_player_pack's members with a player body shaped like the game's (docs/native-puppets.md): a
    physics param, a ControllerSet with a $parent, a character controller, shapes, a ragdoll, sensors."""
    ph, cs, cc = "Component/Physics/", "Phive/ControllerSetParam/", "Phive/CharacterControllerParam/"
    out = dict(members)
    out[ph + "P.engine__component__PhysicsParam.bgyml"] = byml.dumps(
        {"ControllerSetPath": "Work/Phive/ControllerSetParam/PC.phive__ControllerSetParam.gyml"})
    out[cs + "PBase.phive__ControllerSetParam.bgyml"] = byml.dumps({
        "InitializerName": "Character",
        "ShapeNamePathAry": [{"FilePath": "Work/Phive/ShapeParam/P.phive__ShapeParam.gyml", "Name": "Player_cct"}],
        "MatterRigidBodyNamePathAry": [{"FilePath": "Work/Phive/CharacterMatterRigidBodyParam/P.phive__"
                                        "CharacterMatterRigidBodyParam.gyml", "Name": "MainMatterRigidBody"}],
        "CColEntityNamePathAry": [{"FilePath": "Work/Phive/ContactCollectionEntityParam/Block.phive__"
                                   "ContactCollectionEntityParam.gyml", "Name": "Contact_0_cct"},
                                  {"FilePath": "Work/Phive/ContactCollectionEntityParam/Ragdoll.phive__"
                                   "ContactCollectionEntityParam.gyml", "Name": "RagdollBody"}],
        "ControllerSensorNamePathAry": [{"FilePath": "Work/Phive/RigidBodyControllerSensorParam/Tgt.phive__"
                                         "RigidBodyControllerSensorParam.gyml", "Name": "Tgt"}],
        "RagdollStructure": [{"FilePath": "Work/Phive/Ragdoll/Structure/P.phive__RagdollStructureParam.gyml",
                              "Name": "Default"}],
        "PathCharacterController": "Work/Phive/CharacterControllerParam/PBase.phive__CharacterControllerParam.gyml",
        "Cloth": [{"FilePath": "", "Name": "Default"}],
    })
    out[cs + "PC.phive__ControllerSetParam.bgyml"] = byml.dumps({
        "$parent": "Work/Phive/ControllerSetParam/PBase.phive__ControllerSetParam.gyml",
        "PathCharacterController": "Work/Phive/CharacterControllerParam/PC.phive__CharacterControllerParam.gyml"})
    out[cc + "PBase.phive__CharacterControllerParam.bgyml"] = byml.dumps(
        {"GravityScale": 1.0, "UserContactCollectionName": "Contact_0_cct"})
    out[cc + "PC.phive__CharacterControllerParam.bgyml"] = byml.dumps(
        {"$parent": "Work/Phive/CharacterControllerParam/PBase.phive__CharacterControllerParam.gyml",
         "UpdateComponentParam": {"Rotate": 1}})
    for path in ("Phive/ShapeParam/P.phive__ShapeParam.bgyml",
                 "Phive/CharacterMatterRigidBodyParam/P.phive__CharacterMatterRigidBodyParam.bgyml",
                 "Phive/ContactCollectionEntityParam/Block.phive__ContactCollectionEntityParam.bgyml",
                 "Phive/ContactCollectionEntityParam/Ragdoll.phive__ContactCollectionEntityParam.bgyml",
                 "Phive/RigidBodyControllerSensorParam/Tgt.phive__RigidBodyControllerSensorParam.bgyml",
                 "Phive/Ragdoll/Structure/P.phive__RagdollStructureParam.bgyml"):
        out[path] = byml.dumps({"X": 1})
    return out


class BymlTest(unittest.TestCase):
    def test_round_trip_keeps_types(self):
        doc = sample_doc()
        out = byml.loads(byml.dumps(doc))
        self.assertEqual(out, doc)
        self.assertIsInstance(out["UInt"], byml.UInt)
        self.assertIsInstance(out["Int"], int)
        self.assertIsInstance(out["Bool"], bool)
        self.assertIsInstance(out["Hash"], byml.Hash32)

    def test_written_twice_is_identical(self):
        b = byml.dumps(sample_doc())
        self.assertEqual(byml.dumps(byml.loads(b)), b)

    def test_header_and_sorted_tables(self):
        b = byml.dumps({"b": "y", "a": "x", "B": "z"})
        self.assertEqual(b[:2], b"YB")
        self.assertEqual(struct.unpack_from("<H", b, 2)[0], 7)
        keys_off = struct.unpack_from("<I", b, 4)[0]
        n = int.from_bytes(b[keys_off + 1:keys_off + 4], "little")
        offs = struct.unpack_from(f"<{n + 1}I", b, keys_off + 4)
        keys = [b[keys_off + offs[i]:b.index(b"\0", keys_off + offs[i])] for i in range(n)]
        self.assertEqual(keys, sorted(keys))

    def test_equal_values_shared(self):
        one = byml.dumps({"a": [{"x": 1, "y": 2}]})
        two = byml.dumps({"a": [{"x": 1, "y": 2}], "b": [{"x": 1, "y": 2}]})
        # The second list is not written again, only one more dict entry and one key.
        self.assertLess(len(two) - len(one), 24)
        self.assertEqual(byml.loads(two)["b"], [{"x": 1, "y": 2}])

    def test_int_float_bool_not_merged(self):
        doc = {"a": [1], "b": [1.0], "c": [True]}
        out = byml.loads(byml.dumps(doc))
        self.assertIsInstance(out["a"][0], int)
        self.assertNotIsInstance(out["a"][0], bool)
        self.assertIsInstance(out["b"][0], float)
        self.assertIs(out["c"][0], True)

    def test_rejects_bad_magic(self):
        with self.assertRaises(byml.BymlError):
            byml.loads(b"XY\x00\x07" + b"\0" * 12)

    def test_reads_big_endian(self):
        # "BY": big-endian, as GameData/GameDataList (boss_beaten reads it). Root: an array of one int, -2.
        data = b"BY\x00\x07" + struct.pack(">III", 0, 0, 0x10) + bytes([0xC0, 0, 0, 1, 0xD1, 0, 0, 0])
        data += struct.pack(">i", -2)
        self.assertEqual(byml.loads(data), [-2])


class SarcTest(unittest.TestCase):
    def test_round_trip(self):
        files = {"Actor/A.bgyml": b"abc", "Component/B.bgyml": b"\x00" * 33, "x": b""}
        data = sarc.write(files)
        self.assertEqual(sarc.read(data), files)
        self.assertEqual(sarc.write(sarc.read(data)), data)

    def test_sorted_by_hash_and_aligned(self):
        files = {f"f{i}": bytes([i]) * (i + 1) for i in range(20)}
        data = sarc.write(files)
        cnt = struct.unpack_from("<H", data, 0x14 + 6)[0]
        hashes = [struct.unpack_from("<I", data, 0x20 + 16 * i)[0] for i in range(cnt)]
        self.assertEqual(hashes, sorted(hashes))
        data_off = struct.unpack_from("<I", data, 0x0C)[0]
        for i in range(cnt):
            b = struct.unpack_from("<I", data, 0x20 + 16 * i + 8)[0]
            self.assertEqual((data_off + b) % 8, 0)

    def test_name_hash(self):
        self.assertEqual(sarc.name_hash(""), 0)
        self.assertEqual(sarc.name_hash("a"), 0x61)
        self.assertEqual(sarc.name_hash("ab"), (0x61 * 0x65 + 0x62) & 0xFFFFFFFF)


class RstbTest(unittest.TestCase):
    def test_round_trip_and_add(self):
        t = rstb.Rstb(by_crc={rstb.Rstb.crc("a/b.pack"): 100}, by_name={"c/d.bgyml": 7})
        data = t.dumps()
        self.assertEqual(data[:6], b"RESTBL")
        u = rstb.Rstb.loads(data)
        self.assertEqual(u.get("a/b.pack"), 100)
        self.assertEqual(u.get("c/d.bgyml"), 7)
        u.add_new("new/file.pack", 55)
        self.assertEqual(rstb.Rstb.loads(u.dumps()).get("new/file.pack"), 55)

    def test_collision_goes_to_names(self):
        t = rstb.Rstb()
        t.by_crc[rstb.Rstb.crc("mine.pack")] = 1  # pretend a game file has this crc
        t.add_new("mine.pack", 2)
        self.assertEqual(t.by_name["mine.pack"], 2)
        self.assertEqual(t.get("mine.pack"), 2)

    def test_truncated(self):
        data = rstb.Rstb(by_crc={1: 2}).dumps()
        with self.assertRaises(rstb.RstbError):
            rstb.Rstb.loads(data[:-1])


def fake_asb(strings=("Root", "Eaten", "Eaten_F", "Eaten_B", "Wait"), refs=("Eaten_F", "Eaten_B"), version=0x41B):
    """An AS root shaped as the game's (asb_carry): header, a body whose u32 fields name pool strings (each of
    `refs` once, plus a command name), then the string pool as the last section (offset word 14, size word 23)."""
    pool = b""
    offs = {}
    for name in strings:
        offs[name] = len(pool)
        pool += name.encode() + b"\0"
    body = bytearray(0x40)
    struct.pack_into("<I", body, 0x00, offs["Eaten"])
    for i, r in enumerate(refs):
        struct.pack_into("<I", body, 0x10 + 4 * i, offs[r])
    head = bytearray(0x80)
    head[:4] = b"ASB "
    struct.pack_into("<I", head, 4, version)
    struct.pack_into("<I", head, asb_carry.POOL_AT, 0x80 + len(body))
    struct.pack_into("<I", head, asb_carry.POOL_SIZE_AT, len(pool))
    return bytes(head + body + pool)


def fake_player_pack(variant):
    """A made-up player pack shaped like the game's: a child ActorParam with a $parent."""
    base = {
        "Components": {
            "ASRef": "?AS/Player_Adult.root.asb",
            "ASInfoRef": "?Component/ASInfo/P.engine__component__ASInfo.bgyml",
            "ModelInfoRef": "?Component/ModelInfo/P.engine__component__ModelInfo.bgyml",
            "AnimationRef": "?Component/AnimationParam/P.engine__component__AnimationParam.bgyml",
            "ActorNameRef": "?ActorSystem/ActorName/Player.engine__actor__ActorName.bgyml",
            "PhysicsRef": "?Component/Physics/P.engine__component__PhysicsParam.bgyml",
            "LODRef": "?Component/LODParam/Shared.engine__component__LODParam.bgyml",
            "BlackboardRef": "?Component/Blackboard/BlackboardInfo/P.engine__component__BlackboardInfo.bgyml",
            "ELinkRef": "?Component/ELink/Player.engine__component__ELinkParam.bgyml",
            "SLinkRef": "?Component/SLink/Player.engine__component__SLinkParam.bgyml",
            "SoundFootStepRef": "?Component/SoundFootStepParam/Player.game__component__SoundFootStepParam.bgyml",
            "XLinkRef": "?Component/XLink/Player.engine__component__XLinkParam.bgyml",
            "AffiliationRef": "?Component/AffiliationParam/Player.game__component__AffiliationParam.bgyml",
            "PerceptionEmitterRef": "?Component/PerceptionEmitterParam/Player.game__component__PerceptionEmitterParam.bgyml",
        }
    }
    child = {
        "$parent": "Work/Actor/Player_Base.engine__actor__ActorParam.gyml",
        "Components": {
            "ModelInfoRef": f"?Component/ModelInfo/{variant}.engine__component__ModelInfo.bgyml",
            "EquipmentUserRef": f"?Component/EquipmentUserParam/{variant}.game__component__EquipmentUserParam.bgyml",
            "SLinkRef": f"?Component/SLink/Player_{variant}.engine__component__SLinkParam.bgyml",
            "SoundFootStepRef": f"?Component/SoundFootStepParam/Player_{variant}.game__component__SoundFootStepParam.bgyml",
        },
    }
    fs = "Component/SoundFootStepParam/"
    eu = "Component/EquipmentUserParam/"
    bb = "Component/Blackboard/BlackboardParamTable/"

    def key(k, inst=True):
        return {"BBKey": k, "InitValConverted": "", "IsInstanceParam": inst, "StringParamType": "String256"}
    asinfo_parent = {"NumSlots": 4}
    files = {
        "Actor/Player_Base.engine__actor__ActorParam.bgyml": byml.dumps(base),
        f"Actor/Player_{variant}.engine__actor__ActorParam.bgyml": byml.dumps(child),
        "Component/ASInfo/P.engine__component__ASInfo.bgyml": byml.dumps(
            {"$parent": "Work/Component/ASInfo/Base.engine__component__ASInfo.gyml", "X": 1}),
        "Component/ASInfo/Base.engine__component__ASInfo.bgyml": byml.dumps(asinfo_parent),
        "Component/ModelInfo/P.engine__component__ModelInfo.bgyml": byml.dumps({"FmdbName": "P"}),
        f"Component/ModelInfo/{variant}.engine__component__ModelInfo.bgyml": byml.dumps({"FmdbName": variant}),
        "Component/AnimationParam/P.engine__component__AnimationParam.bgyml": byml.dumps({"A": []}),
        "Component/Physics/P.engine__component__PhysicsParam.bgyml": byml.dumps({"Rigid": 1}),
        "Component/ELink/Player.engine__component__ELinkParam.bgyml": byml.dumps({"UserName": "Player"}),
        "Component/SLink/Player.engine__component__SLinkParam.bgyml": byml.dumps({"UserName": "Player"}),
        f"Component/SLink/Player_{variant}.engine__component__SLinkParam.bgyml": byml.dumps(
            {"UserName": f"Player_{variant}"}),
        "Component/XLink/Player.engine__component__XLinkParam.bgyml": byml.dumps({"PropertyTableListRef": "P"}),
        f"{fs}Player.game__component__SoundFootStepParam.bgyml": byml.dumps({"IsUseRayCast": True}),
        f"{fs}Player_{variant}.game__component__SoundFootStepParam.bgyml": byml.dumps(
            {"$parent": f"Work/{fs}Player.game__component__SoundFootStepParam.gyml", "LandInterval": 5.0}),
        "AI/Player.root.ainb": b"AIB " + b"\0" * 8,
        "Component/AffiliationParam/Player.game__component__AffiliationParam.bgyml": byml.dumps(
            {"DefaultAffiliation": byml.UInt(145979025), "TargetAffiliations": [byml.UInt(2194130537)]}),
        "Component/PerceptionEmitterParam/Player.game__component__PerceptionEmitterParam.bgyml": byml.dumps(
            {"AdvancedEmitters": {"AimTerror": "Work/Perception/AdvancedEmitterParam/Player_AimTerror.gyml"},
             "IsResidentEmitter": True, "SubLayerSensor": "PerceptionPlayer"}),
        f"{eu}{variant}.game__component__EquipmentUserParam.bgyml": byml.dumps(
            {"$parent": f"Work/{eu}Player.game__component__EquipmentUserParam.gyml", "AccessoryBindBoneName1": "Pod_B"}),
        f"{eu}Player.game__component__EquipmentUserParam.bgyml": byml.dumps(
            {"$parent": f"Work/{eu}Default.game__component__EquipmentUserParam.gyml",
             "BlackboardTableRef": f"Work/{bb}Player.engine__component__BlackboardParamTable.gyml"}),
        f"{eu}Default.game__component__EquipmentUserParam.bgyml": byml.dumps(
            {"BlackboardTableRef": f"Work/{bb}EquipmentUser.engine__component__BlackboardParamTable.gyml"}),
        f"{bb}Player.engine__component__BlackboardParamTable.bgyml": byml.dumps(
            {"$parent": f"Work/{bb}WeaponUser.engine__component__BlackboardParamTable.gyml",
             "BlackboardParamStringArray": [key("EquipmentUser_ArmorUpper"), key("EquipmentUser_Bow")]}),
        f"{bb}WeaponUser.engine__component__BlackboardParamTable.bgyml": byml.dumps(
            {"BlackboardParamStringArray": [key("EquipmentUser_Weapon"), key("EquipmentUser_Shield"),
                                            key("EquipmentUser_Bow", inst=False)],
             "BlackboardParamS32Array": [{"BBKey": "WeaponUser_CurrentEquipDrawnWeaponType", "InitVal": 0,
                                          "IsInstanceParam": False}]}),
        f"{bb}EquipmentUser.engine__component__BlackboardParamTable.bgyml": byml.dumps(
            {"BlackboardParamStringArray": [key("EquipmentUser_SubWeapon")]}),
        "Component/Blackboard/BlackboardInfo/P.engine__component__BlackboardInfo.bgyml": byml.dumps({"T": []}),
    }
    return files


PVP_REFS = {
    "SensorHitRef": "?Component/SensorHitParam/Player.game__component__SensorHitParam.bgyml",
    "LifeRef": "?Component/LifeParam/Player.game__component__LifeParam.bgyml",
    "ReactionRef": "?Component/Reaction/Player.engine__component__ReactionParam.bgyml",
    "TargetRef": "?Component/TargetParam/Player.game__component__TargetParam.bgyml",
}
PVP_ATTACK = "GameBalance/AttackParam/Player.game__gamebalance__AttackParam.bgyml"
PVP_DAMAGE = "GameParameter/DamageParameters/Player.game__DamageParameters.bgyml"


def with_pvp(members, variant="Child"):
    """with_body's members with what `--puppet-pvp` cuts, shaped like the game's (docs/native-puppets.md 5.1):
    the player's sensors (Tgt, Atk, ForArea and their bodies), SensorHit/Life/Reaction/Target params and
    AttackParam rows."""
    out = with_body(members)
    cs = "Phive/ControllerSetParam/PBase.phive__ControllerSetParam.bgyml"
    doc = byml.loads(out[cs])
    rc = "Work/Phive/ContactCollectionSensorParam/"
    rb = "Work/Phive/RigidBodySensorParam/"
    doc["CColSensorNamePathAry"] = [
        {"FilePath": rc + "PlayerReceipt.phive__ContactCollectionSensorParam.gyml", "Name": "Tgt"},
        {"FilePath": rc + "PlayerAttackReceipt.phive__ContactCollectionSensorParam.gyml", "Name": "Atk"},
        {"FilePath": rc + "PlayerReceipt.phive__ContactCollectionSensorParam.gyml", "Name": "ForArea"}]
    doc["RigidBodySensorNamePathAry"] = [
        {"FilePath": rb + "Player_Tgt_Body.phive__RigidBodySensorParam.gyml", "Name": "Body_Tgt"},
        {"FilePath": rb + "Player_Atk_AtkPlayerBody.phive__RigidBodySensorParam.gyml", "Name": "Body"}]
    doc["ControllerSensorNamePathAry"].append(
        {"FilePath": "Work/Phive/RigidBodyControllerSensorParam/Atk.phive__RigidBodyControllerSensorParam.gyml",
         "Name": "Atk"})
    doc["AliasSensorList"] = [{"AliasSensor": "Tgt", "ControllerName": "Tgt", "RigidBodyName": "Body_Tgt"}]
    out[cs] = byml.dumps(doc)
    for path in ("Phive/ContactCollectionSensorParam/PlayerReceipt.phive__ContactCollectionSensorParam.bgyml",
                 "Phive/ContactCollectionSensorParam/PlayerAttackReceipt.phive__ContactCollectionSensorParam.bgyml",
                 "Phive/RigidBodySensorParam/Player_Tgt_Body.phive__RigidBodySensorParam.bgyml",
                 "Phive/RigidBodySensorParam/Player_Atk_AtkPlayerBody.phive__RigidBodySensorParam.bgyml",
                 "Phive/RigidBodyControllerSensorParam/Atk.phive__RigidBodyControllerSensorParam.bgyml",
                 PVP_ATTACK, PVP_DAMAGE):
        out[path] = byml.dumps({"X": 1})
    ap = f"Actor/Player_{variant}.engine__actor__ActorParam.bgyml"
    actor = byml.loads(out[ap])
    actor["Components"].update(PVP_REFS)
    out[ap] = byml.dumps(actor)
    out[PVP_REFS["SensorHitRef"][1:]] = byml.dumps({"TgHitInfoBufferSize": 8, "IsThroughSameAffiliation": False})
    out[PVP_REFS["LifeRef"][1:]] = byml.dumps({"DamageParam": "Work/" + PVP_DAMAGE[:-len(".bgyml")] + ".gyml"})
    out[PVP_REFS["ReactionRef"][1:]] = byml.dumps({"R": 1})
    out[PVP_REFS["TargetRef"][1:]] = byml.dumps({"T": 1})
    return out


def fake_attention_pack():
    """ATTENTION_SOURCE's pack: EnemyCommon's LockOn and AutoAim client params, LockOn with an in-pack $parent."""
    d, x = "Attention/ClientParam/", ".game__attention__ClientParam.bgyml"
    return {
        f"{d}EnemyCommon_LockOn{x}": byml.dumps({"$parent": f"Work/{d}Base.game__attention__ClientParam.gyml",
                                                 "AttentionType": byml.UInt(3016662870), "ClientName": "LockOn"}),
        f"{d}Base{x}": byml.dumps({"PriorityType": 1, "ClientName": "Base"}),
        f"{d}EnemyCommon_AutoAim{x}": byml.dumps({"ClientName": "AutoAim"}),
        "Component/AttentionSenderParam/EnemyCommon.game__component__AttentionSenderParam.bgyml": byml.dumps(
            {"ClientList": [f"Work/{d}EnemyCommon_LockOn.game__attention__ClientParam.gyml"]}),
    }


def fake_ruto_pack():
    """Npc_Zora_Ruto_Child_001's ActorParam and her LiftableParam with an in-pack $parent (her values as the romfs
    has them; the game's file has no $parent, the test's base checks the merge)."""
    lift = "Component/LiftableParam/NpcZoraRutoChild.game__component__LiftableParam.bgyml"
    base = "Component/LiftableParam/LiftableBase.game__component__LiftableParam.bgyml"
    return {
        build.actor_param_path("Npc_Zora_Ruto_Child_001"): byml.dumps(
            {"Components": {"LiftableRef": "?" + lift, "ASRef": "?AS/Npc_Zora_ZU_Ruto_Child.bgyml"}}),
        lift: byml.dumps({"$parent": "Work/" + base[:-len(".bgyml")] + ".gyml", "LiftGrade": byml.UInt(3069107721),
                          "LiftableType": byml.UInt(3056776872), "WeightForCalcThrowPower": 19.0}),
        base: byml.dumps({"LiftGrade": 1, "KeepSensorHitToParent": True}),
    }


def fake_perception_pack():
    """InterestSpotActor's physics as the game has it: a ProjectBase physics param with an empty controller set."""
    return {
        build.PERCEPTION_PHYSICS: byml.dumps(
            {"$parent": "Work/Component/Physics/ProjectBase.engine__component__PhysicsParam.gyml",
             "ControllerSetPath": "Work/Phive/ControllerSetParam/InterestSpotActor.phive__ControllerSetParam.gyml"}),
        build.PERCEPTION_CONTROLLERS: byml.dumps({}),
    }


def fake_emit_ainb():
    """An AI file laid out as the Business Scrub seed's PostUpdateMatrix root (version 0x408): one
    command whose root is a selector (node 0), a query, the emit node and a do-nothing node, the
    emit node's three immediate parameters, then the string pool."""
    names = ["Shootable_ScrubBusinessSees_PostUpdateMatrix.root", "Root", "", "QueryShootableGetState",
             "OneShotXLinkSearchAndEmit", "OneshotDoNothing", "ELinkCall", "SLinkCall", "XLinkKeyName",
             "ScrubBusinessSeed_HitGround"]
    pool, offs = b"", {}
    for n in names:
        offs[n] = len(pool)
        pool += n.encode() + b"\0"
    nodes = ["", "QueryShootableGetState", "OneShotXLinkSearchAndEmit", "OneshotDoNothing"]
    body = bytearray(0x74)
    body[0:4] = b"AIB "
    struct.pack_into("<IIII", body, 4, 0x408, offs[names[0]], 1, len(nodes))
    body += struct.pack("<I", offs["Root"]) + bytes(16) + struct.pack("<hh", 0, -1)
    for i, n in enumerate(nodes):
        node = bytearray(0x3C)
        struct.pack_into("<HHHBBI", node, 0, 1 if i == 0 else 0, i, 0, 4 if i == 0 else 0, 0, offs[n])
        body += node
    for name, value in (("ELinkCall", 1), ("SLinkCall", 1),
                        ("XLinkKeyName", offs["ScrubBusinessSeed_HitGround"])):
        body += struct.pack("<IIII", offs[name], 0xFFFF, 0x01800000, value)
    struct.pack_into("<I", body, 0x24, len(body))
    return bytes(body) + pool


class AinbTest(unittest.TestCase):
    def test_oneshot_root(self):
        from puppet_pack import ainb
        t = fake_emit_ainb()
        self.assertEqual(ainb.describe(t), ("", "ScrubBusinessSeed_HitGround", True, True,
                                            "Shootable_ScrubBusinessSees_PostUpdateMatrix.root"))
        n = ainb.oneshot_root(t, "CoopHit_SeedSpark.root", "Hit_AdditionalAttacker", elink=True, slink=False)
        self.assertEqual(ainb.describe(n), ("OneShotXLinkSearchAndEmit", "Hit_AdditionalAttacker", True,
                                            False, "CoopHit_SeedSpark.root"))
        self.assertEqual(n[0x8C:0x8C + 4 * 0x3C], t[0x8C:0x8C + 4 * 0x3C])  # the nodes stay
        self.assertEqual(n[:len(t)][len(t) - 40:], t[len(t) - 40:])        # the old pool stays
        self.assertTrue(n.endswith(b"CoopHit_SeedSpark.root\0"))
        # A key already in the pool is not added twice.
        same = ainb.oneshot_root(t, "Shootable_ScrubBusinessSees_PostUpdateMatrix.root",
                                 "ScrubBusinessSeed_HitGround")
        self.assertEqual(len(same), len(t))

    def test_refuses_other_files(self):
        from puppet_pack import ainb
        t = fake_emit_ainb()
        with self.assertRaises(ainb.AinbError):
            ainb.oneshot_root(b"XXXX" + t[4:], "a", "b")
        with self.assertRaises(ainb.AinbError):
            ainb.oneshot_root(t[:4] + struct.pack("<I", 0x407) + t[8:], "a", "b")
        no_emit = t.replace(b"OneShotXLinkSearchAndEmit", b"OneShotXLinkSearchAndFade")
        with self.assertRaises(ainb.AinbError):
            ainb.oneshot_root(no_emit, "a", "b")


class BuildPackTest(unittest.TestCase):
    def test_pack_contents(self):
        members = fake_player_pack("Child")
        pack, ap, names, keys = build.build_pack(members, "Child")
        out = sarc.read(pack)
        doc = byml.loads(out["Actor/CoopPuppet_Child.engine__actor__ActorParam.bgyml"])
        comps = doc["Components"]
        self.assertNotIn("$parent", doc)
        self.assertEqual(comps["ModelInfoRef"], "?Component/ModelInfo/Child.engine__component__ModelInfo.bgyml")
        self.assertEqual(comps["ASRef"], "?AS/Player_Adult.root.asb")
        for k in ("ActorNameRef", "PhysicsRef"):
            self.assertNotIn(k, comps)
        self.assertEqual(comps["ActorLogicNodeRef"], build.FIXED_COMPONENTS["ActorLogicNodeRef"])
        # Referenced members come along, through $parent too; the rest stay out.
        self.assertIn("Component/ASInfo/Base.engine__component__ASInfo.bgyml", out)
        self.assertIn("Component/ModelInfo/Child.engine__component__ModelInfo.bgyml", out)
        self.assertNotIn("Component/ModelInfo/P.engine__component__ModelInfo.bgyml", out)
        self.assertNotIn("Component/Physics/P.engine__component__PhysicsParam.bgyml", out)
        self.assertNotIn("AI/Player.root.ainb", out)
        self.assertNotIn("Actor/Player_Base.engine__actor__ActorParam.bgyml", out)
        self.assertEqual(sorted(out), names)
        # Equipment: the EquipmentUserParam chain and the blackboard table it names come
        # along; the player's own BlackboardRef does not.
        self.assertEqual(comps["EquipmentUserRef"],
                         "?Component/EquipmentUserParam/Child.game__component__EquipmentUserParam.bgyml")
        self.assertNotIn("BlackboardRef", comps)
        for p in ("Component/EquipmentUserParam/Child.game__component__EquipmentUserParam.bgyml",
                  "Component/EquipmentUserParam/Player.game__component__EquipmentUserParam.bgyml",
                  "Component/EquipmentUserParam/Default.game__component__EquipmentUserParam.bgyml",
                  "Component/Blackboard/BlackboardParamTable/Player.engine__component__BlackboardParamTable.bgyml",
                  "Component/Blackboard/BlackboardParamTable/WeaponUser.engine__component__BlackboardParamTable.bgyml"):
            self.assertIn(p, out)
        self.assertNotIn("Component/Blackboard/BlackboardInfo/P.engine__component__BlackboardInfo.bgyml", out)
        # The first BlackboardTableRef in the chain wins (Player, not Default), child
        # entries override the $parent's (Bow is an instance key again).
        self.assertEqual(keys, ["EquipmentUser_ArmorUpper", "EquipmentUser_Bow",
                                "EquipmentUser_Shield", "EquipmentUser_Weapon"])
        # Sound (issue 131): the variant's SLink and footstep components (and the footstep
        # $parent) come along; effects (ELink) and the player's XLink property table do not.
        self.assertEqual(comps["SLinkRef"], "?Component/SLink/Player_Child.engine__component__SLinkParam.bgyml")
        # The puppet's own footstep param: Dark Link's changes over the player's variant file.
        own = "Component/SoundFootStepParam/CoopPuppet_Child.game__component__SoundFootStepParam.bgyml"
        self.assertEqual(comps["SoundFootStepRef"], "?" + own)
        fs = byml.loads(out[own])
        self.assertEqual(fs["$parent"],
                         "Work/Component/SoundFootStepParam/Player_Child.game__component__SoundFootStepParam.gyml")
        self.assertIs(fs["IsUseRayCast"], False)
        self.assertIs(fs["IsEnableGrass"], False)
        self.assertIsInstance(fs["ProcType"], byml.UInt)
        self.assertEqual(fs["ProcType"].v, 3069107721)
        self.assertEqual(sorted(fs), ["$parent", "IsEnableGrass", "IsUseRayCast", "ProcType"])
        # The player's XLink param, for the property tables Link's SLink user reads (puppet-looks).
        self.assertEqual(comps["XLinkRef"], "?Component/XLink/Player.engine__component__XLinkParam.bgyml")
        self.assertNotIn("ELinkRef", comps)
        for p in ("Component/SLink/Player_Child.engine__component__SLinkParam.bgyml",
                  "Component/SoundFootStepParam/Player_Child.game__component__SoundFootStepParam.bgyml",
                  "Component/SoundFootStepParam/Player.game__component__SoundFootStepParam.bgyml",
                  "Component/XLink/Player.engine__component__XLinkParam.bgyml"):
            self.assertIn(p, out)
        for p in ("Component/SLink/Player.engine__component__SLinkParam.bgyml",
                  "Component/ELink/Player.engine__component__ELinkParam.bgyml"):
            self.assertNotIn(p, out)

    def test_pack_basic_xlink_and_no_sound(self):
        members = fake_player_pack("Child")
        for kwargs in ({"basic_xlink": True}, {"sound": False}):
            pack, _, names, _ = build.build_pack(members, "Child", **kwargs)
            comps = byml.loads(sarc.read(pack)["Actor/CoopPuppet_Child.engine__actor__ActorParam.bgyml"])["Components"]
            self.assertEqual(comps["XLinkRef"], build.FIXED_COMPONENTS["XLinkRef"])
            self.assertNotIn("Component/XLink/Player.engine__component__XLinkParam.bgyml", names)

    def test_footstep_proc_type_is_none(self):
        # The hashes in the game's files are murmur3 of the enum names: Player and None.
        self.assertEqual(murmur3("Player"), 145979025)
        self.assertEqual(murmur3("None"), build.FOOTSTEP_PROC_NONE)

    def test_pack_player_footsteps(self):
        members = fake_player_pack("Adult")
        pack, _, names, _ = build.build_pack(members, "Adult", player_footsteps=True)
        comps = byml.loads(sarc.read(pack)["Actor/CoopPuppet_Adult.engine__actor__ActorParam.bgyml"])["Components"]
        self.assertEqual(comps["SoundFootStepRef"],
                         "?Component/SoundFootStepParam/Player_Adult.game__component__SoundFootStepParam.bgyml")
        self.assertFalse([n for n in names if "CoopPuppet_Adult.game__" in n])

    def test_pack_perception(self):
        # Issues 150/178: the puppet is a target of the game's perception (build.PERCEPTION_SOURCE).
        members = fake_player_pack("Child")
        seen_by = build.perception_members(fake_perception_pack())
        pack, _, names, _ = build.build_pack(members, "Child", perception=seen_by)
        out = sarc.read(pack)
        comps = byml.loads(out["Actor/CoopPuppet_Child.engine__actor__ActorParam.bgyml"])["Components"]
        own = "Component/PerceptionEmitterParam/CoopPuppet_Child.game__component__PerceptionEmitterParam.bgyml"
        self.assertEqual(comps["PerceptionEmitterRef"], "?" + own)
        self.assertEqual(byml.loads(out[own]), {"IsResidentEmitter": True, "SubLayerSensor": "PerceptionPlayer"})
        self.assertEqual(comps["AffiliationRef"],
                         "?Component/AffiliationParam/Player.game__component__AffiliationParam.bgyml")
        self.assertEqual(comps["PhysicsRef"], "?" + build.PERCEPTION_PHYSICS)
        for p in (build.PERCEPTION_PHYSICS, build.PERCEPTION_CONTROLLERS,
                  "Component/AffiliationParam/Player.game__component__AffiliationParam.bgyml"):
            self.assertIn(p, names)
        # Never the player's own physics or emitter.
        self.assertNotIn("Component/Physics/P.engine__component__PhysicsParam.bgyml", names)
        self.assertNotIn("Component/PerceptionEmitterParam/Player.game__component__PerceptionEmitterParam.bgyml", names)

    def test_pack_body(self):
        # Native puppets phase 1 (--puppet-body): Link's own body, character controller in Free, no sensors.
        members = with_body(fake_player_pack("Child"))
        pack, ap, names, _ = build.build_pack(members, "Child", body=True)
        out = sarc.read(pack)
        phys, cs_path, cct_path = build.body_paths("CoopPuppet_Child")
        self.assertEqual(byml.loads(ap)["Components"]["PhysicsRef"], "?" + phys)
        self.assertEqual(byml.loads(out[phys]),
                         {"ControllerSetPath": "Work/Phive/ControllerSetParam/CoopPuppet_Child.phive__ControllerSetParam.gyml"})
        cs = byml.loads(out[cs_path])
        self.assertNotIn("$parent", cs)
        self.assertEqual(sorted(cs), sorted(build.BODY_KEYS + ("Cloth", "PathCharacterController")))
        self.assertEqual([e["Name"] for e in cs["CColEntityNamePathAry"]], ["Contact_0_cct"])
        self.assertEqual(cs["PathCharacterController"],
                         "Work/Phive/CharacterControllerParam/CoopPuppet_Child.phive__CharacterControllerParam.gyml")
        cct = byml.loads(out[cct_path])  # the player's, $parent merged, child wins, in Free
        self.assertEqual(cct, {"GravityScale": 1.0, "UserContactCollectionName": "Contact_0_cct",
                               "UpdateComponentParam": {"Rotate": 1}, "DefaultMoveState": "Free"})
        for p in ("Phive/ShapeParam/P.phive__ShapeParam.bgyml",
                  "Phive/CharacterMatterRigidBodyParam/P.phive__CharacterMatterRigidBodyParam.bgyml",
                  "Phive/ContactCollectionEntityParam/Block.phive__ContactCollectionEntityParam.bgyml"):
            self.assertIn(p, names)
        # Never the sensors, the ragdoll or the player's own physics files.
        for p in ("Phive/RigidBodyControllerSensorParam/Tgt.phive__RigidBodyControllerSensorParam.bgyml",
                  "Phive/Ragdoll/Structure/P.phive__RagdollStructureParam.bgyml",
                  "Phive/ContactCollectionEntityParam/Ragdoll.phive__ContactCollectionEntityParam.bgyml",
                  "Component/Physics/P.engine__component__PhysicsParam.bgyml",
                  "Phive/ControllerSetParam/PC.phive__ControllerSetParam.bgyml"):
            self.assertNotIn(p, names)

    def test_body_character_controller_default_preset(self):
        # crash-0116-b: the player's "Player" preset builds PlayerRagdoll, which needs the ragdoll the body
        # leaves out (null write at guest 0x70). The body's controller runs the default preset, as NPCs do.
        player = {"PresetTypeName": "Player", "GravityScale": 1.0, "DefaultMoveState": "OnGround",
                  "MoveStateParam": {"PlayerOnGround": {"$type": "phive__CharacterMoveStateOnGroundParam"}},
                  "UpdateComponentParam": {"InteractWithRagdoll": {"Settings": {}}, "Rotate": {"A": 1},
                                           "BounceSuppressor": {}, "FloatWaterExtension": {},
                                           "ForceFieldExtension": {}, "GroundEdgeUnstucker": {}}}
        self.assertEqual(build.body_character_controller(player),
                         {"GravityScale": 1.0, "DefaultMoveState": "Free", "UpdateComponentParam": {"Rotate": {"A": 1}}})
        self.assertEqual(player["PresetTypeName"], "Player")  # the source is not changed
        # Player_Child's own: only InteractWithRagdoll and PlayerOnGround, so both maps go.
        child = {"PresetTypeName": "Player", "UpdateComponentParam": {"InteractWithRagdoll": {}},
                 "MoveStateParam": {"PlayerOnGround": {}}, "UserContactCollectionName": "Contact_0_cct"}
        self.assertEqual(build.body_character_controller(child),
                         {"UserContactCollectionName": "Contact_0_cct", "DefaultMoveState": "Free"})
        # An NPC-like controller keeps what the default preset has.
        npc = {"MoveStateParam": {"OnGround": {"X": 1}}, "UpdateComponentParam": {"FloatWater": {}, "Rotate": {}}}
        self.assertEqual(build.body_character_controller(npc), dict(npc, DefaultMoveState="Free"))
        for name in ("PlayerRagdoll", "InteractWithRagdoll", "PlayerOnGround"):
            self.assertNotIn(name, build.BODY_UPDATE_COMPONENTS + build.BODY_MOVE_STATES)

    def test_pack_body_player_preset_dropped(self):
        # Every body (child and adult; the _Pvp copy's goes through the same puppet_body) drops the Player preset.
        for variant in ("Child", "Adult"):
            members = with_body(fake_player_pack(variant))
            cct_paths = [p for p in members if p.startswith(build.CHARACTER_DIR)]
            for p in cct_paths:
                doc = byml.loads(members[p])
                doc["PresetTypeName"] = "Player"
                doc["UpdateComponentParam"] = dict(doc.get("UpdateComponentParam") or {}, InteractWithRagdoll={})
                members[p] = byml.dumps(doc)
            pack, _, _, _ = build.build_pack(members, variant, body=True)
            cct = byml.loads(sarc.read(pack)[build.body_paths("CoopPuppet_" + variant)[2]])
            self.assertNotIn("PresetTypeName", cct)
            self.assertNotIn("InteractWithRagdoll", cct.get("UpdateComponentParam") or {})
            self.assertEqual(cct["DefaultMoveState"], "Free")

    def test_pack_body_wins_over_perception(self):
        members = with_body(fake_player_pack("Child"))
        seen_by = build.perception_members(fake_perception_pack())
        _, ap, _, _ = build.build_pack(members, "Child", perception=seen_by, body=True)
        comps = byml.loads(ap)["Components"]
        self.assertEqual(comps["PhysicsRef"], "?" + build.body_paths("CoopPuppet_Child")[0])
        self.assertIn("PerceptionEmitterRef", comps)

    def test_pack_body_off_by_default(self):
        members = with_body(fake_player_pack("Child"))
        self.assertEqual(build.build_pack(members, "Child"), build.build_pack(members, "Child", body=False))
        _, ap, names, _ = build.build_pack(members, "Child")
        self.assertNotIn("PhysicsRef", byml.loads(ap)["Components"])
        self.assertFalse([n for n in names if n.startswith("Phive/")])

    def test_pack_liftable(self):
        # Native puppets phase 3 (--puppet-liftable), owner rule: only a young Link is liftable, carried as Ruto.
        # Ruto's LiftableRef, $parent merged, as the child puppet's own; the adult puppet gets none.
        self.assertEqual(build.LIFTABLE_SOURCE, "Npc_Zora_Ruto_Child_001")
        self.assertEqual(build.LIFTABLE_VARIANTS, ("Child",))
        lift = build.liftable_param(fake_ruto_pack())
        self.assertEqual(lift[0], "Component/LiftableParam/NpcZoraRutoChild.game__component__LiftableParam.bgyml")
        self.assertEqual(lift[1], {"LiftGrade": byml.UInt(3069107721), "LiftableType": byml.UInt(3056776872),
                                   "WeightForCalcThrowPower": 19.0, "KeepSensorHitToParent": True})
        members = fake_player_pack("Child")
        pack, ap, names, _ = build.build_pack(members, "Child", liftable=lift)
        own = "Component/LiftableParam/CoopPuppet_Child.game__component__LiftableParam.bgyml"
        self.assertEqual(build.liftable_path("CoopPuppet_Child", lift[0]), own)
        self.assertEqual(byml.loads(ap)["Components"]["LiftableRef"], "?" + own)
        self.assertEqual(byml.loads(sarc.read(pack)[own]), lift[1])
        self.assertNotIn(lift[0], names)  # never Ruto's own file
        # Off (the default): the pack as before.
        self.assertEqual(build.build_pack(members, "Child"), build.build_pack(members, "Child", liftable=None))
        self.assertNotIn("LiftableRef", byml.loads(build.build_pack(members, "Child")[1])["Components"])
        # An adult puppet is never liftable: on or off, its pack is the same, with no LiftableRef.
        adult = fake_player_pack("Adult")
        self.assertEqual(build.build_pack(adult, "Adult", liftable=lift), build.build_pack(adult, "Adult"))
        self.assertNotIn("LiftableRef", byml.loads(build.build_pack(adult, "Adult", liftable=lift)[1])["Components"])
        # A source pack without the param: none.
        self.assertIsNone(build.liftable_param(
            {build.actor_param_path("Npc_Zora_Ruto_Child_001"): byml.dumps({"Components": {}})}))
        self.assertIsNone(build.liftable_param({}))

    def test_pack_pvp(self):
        # Native puppets phase 4 (--puppet-pvp): the _Pvp copy's pieces, cut from the player's and EnemyCommon's.
        clients = build.attention_clients(fake_attention_pack())
        self.assertEqual(clients["LockOn"], {"PriorityType": 1, "ClientName": "LockOn",
                                             "AttentionType": byml.UInt(3016662870)})
        members = with_pvp(fake_player_pack("Child"))
        actor = build.pvp_name("CoopPuppet_Child")
        self.assertEqual(actor, "CoopPuppet_Child_Pvp")
        pack, ap, names, _ = build.build_pack(members, "Child", actor=actor, pvp=clients)
        out = sarc.read(pack)
        comps = byml.loads(ap)["Components"]
        for k, ref in PVP_REFS.items():
            self.assertEqual(comps[k], ref)
            self.assertIn(ref[1:], names)
        self.assertIn(PVP_DAMAGE, names)  # what the Life param names comes along
        self.assertIn(PVP_ATTACK, names)  # option i: the player's AttackParam rows
        sender, client_paths = build.attention_paths(actor)
        self.assertEqual(comps["AttentionSenderRef"], "?" + sender)
        self.assertEqual(byml.loads(out[sender]), {"ClientList": [
            "Work/Attention/ClientParam/CoopPuppet_Child_Pvp_LockOn.game__attention__ClientParam.gyml",
            "Work/Attention/ClientParam/CoopPuppet_Child_Pvp_AutoAim.game__attention__ClientParam.gyml"]})
        self.assertEqual(byml.loads(out[client_paths["LockOn"]]), clients["LockOn"])
        self.assertNotIn("Attention/ClientParam/EnemyCommon_LockOn.game__attention__ClientParam.bgyml", names)
        # Its body: the Tgt sensor only, never the Atk receipt, ForArea or the attack body.
        cs = byml.loads(out[build.body_paths(actor)[1]])
        self.assertEqual([e["Name"] for e in cs["CColSensorNamePathAry"]], ["Tgt"])
        self.assertEqual([e["Name"] for e in cs["ControllerSensorNamePathAry"]], ["Tgt"])
        self.assertEqual([e["Name"] for e in cs["RigidBodySensorNamePathAry"]], ["Body_Tgt"])
        self.assertEqual([e["AliasSensor"] for e in cs["AliasSensorList"]], ["Tgt"])
        self.assertIn("Phive/RigidBodySensorParam/Player_Tgt_Body.phive__RigidBodySensorParam.bgyml", names)
        for p in ("Phive/ContactCollectionSensorParam/PlayerAttackReceipt.phive__ContactCollectionSensorParam.bgyml",
                  "Phive/RigidBodySensorParam/Player_Atk_AtkPlayerBody.phive__RigidBodySensorParam.bgyml",
                  "Phive/RigidBodyControllerSensorParam/Atk.phive__RigidBodyControllerSensorParam.bgyml"):
            self.assertNotIn(p, names)
        # Never on the copy (5.1).
        for k in ("PlayerRef", "ActorNameRef", "PlayerCameraTargetRef", "DeathRef", "ConditionRef", "ChemicalRef"):
            self.assertNotIn(k, comps)

    def test_pack_pvp_off_is_byte_for_byte(self):
        members = with_pvp(fake_player_pack("Child"))
        plain = build.build_pack(members, "Child")
        self.assertEqual(plain, build.build_pack(members, "Child", pvp=None))
        comps = byml.loads(plain[1])["Components"]
        for k in tuple(PVP_REFS) + ("AttentionSenderRef", "PhysicsRef"):
            self.assertNotIn(k, comps)
        self.assertFalse([n for n in plain[2] if n.startswith(("Phive/", "GameBalance/", "Attention/"))])
        # --puppet-body alone: still no sensor.
        _, _, names, _ = build.build_pack(members, "Child", body=True)
        self.assertFalse([n for n in names if "Sensor" in n])

    def test_pack_pvp_checks(self):
        clients = build.attention_clients(fake_attention_pack())
        self.assertIsNone(build.attention_clients({}))
        no_life = with_pvp(fake_player_pack("Child"))
        del no_life[PVP_REFS["LifeRef"][1:]]
        with self.assertRaises(build.BuildError):
            build.build_pack(no_life, "Child", actor="CoopPuppet_Child_Pvp", pvp=clients)
        no_tgt = with_body(fake_player_pack("Child"))  # a ControllerSet with no Tgt receipt
        with self.assertRaises(build.BuildError):
            build.build_pack(no_tgt, "Child", actor="CoopPuppet_Child_Pvp", pvp=clients)

    def test_carry_as(self):
        # Ruto's carried pose on the young copies: the copy's own AS root names Ruto's clip for Eaten's two clips.
        raw = fake_asb()
        out = asb_carry.carry_as(raw)
        self.assertIsNotNone(out)
        changed = [i for i in range(len(raw)) if raw[i] != out[i]]
        allowed = set(range(asb_carry.POOL_SIZE_AT, asb_carry.POOL_SIZE_AT + 4)) | set(range(0x90, 0x98))
        self.assertTrue(set(changed) <= allowed)  # only the pool size and the two clip fields; nothing moves
        new = asb_carry.string_offset(out, "Act_LiftRest_Wait")
        self.assertEqual(new, struct.unpack_from("<I", raw, asb_carry.POOL_SIZE_AT)[0])  # at the old pool's end
        self.assertEqual(struct.unpack_from("<I", out, asb_carry.POOL_SIZE_AT)[0], len(out) - 0x80 - 0x40)
        self.assertEqual(struct.unpack_from("<II", out, 0x80 + 0x10), (new, new))
        # The command name and every other string are where they were.
        self.assertEqual(asb_carry.string_offset(out, "Eaten"), asb_carry.string_offset(raw, "Eaten"))
        self.assertEqual(struct.unpack_from("<I", out, 0x80)[0], asb_carry.string_offset(raw, "Eaten"))
        # Anything not as expected: None (the copy keeps the game's Eaten).
        self.assertIsNone(asb_carry.carry_as(fake_asb(version=0x417)))
        self.assertIsNone(asb_carry.carry_as(raw + b"\0"))  # the pool is not the last section
        self.assertIsNone(asb_carry.carry_as(fake_asb(strings=("Root", "Eaten", "Eaten_F", "Wait"), refs=("Eaten_F",))))
        self.assertIsNone(asb_carry.carry_as(fake_asb(refs=("Eaten_F", "Eaten_B", "Eaten_F"))))  # named twice
        self.assertIsNone(asb_carry.carry_as(b"not an AS"))
        # "Eaten_F" inside "XEaten_F" is no match: whole strings only.
        self.assertIsNone(asb_carry.string_offset(fake_asb(strings=("Root", "Eaten", "XEaten_F", "Eaten_B", "Eaten_F")),
                                                  "Eaten_Q"))
        # Ruto's clip already in the pool: reused, nothing appended.
        have = fake_asb(strings=("Root", "Eaten", "Eaten_F", "Eaten_B", "Act_LiftRest_Wait"))
        self.assertEqual(len(asb_carry.carry_as(have)), len(have))

    def test_pack_liftable_carry(self):
        # With the carry AS made, the young puppet's ASRef is the carry root and its AnimationParam adds Ruto's
        # animations, retargeted (as the game's own child Epona); adult and skin puppets keep theirs.
        lift = build.liftable_param(fake_ruto_pack())
        members = fake_player_pack("Child")
        pack, ap, names, _ = build.build_pack(members, "Child", liftable=lift, carry=True)
        comps = byml.loads(ap)["Components"]
        self.assertEqual(comps["ASRef"], "?" + asb_carry.CARRY_AS)
        own = asb_carry.carry_animation_path("CoopPuppet_Child")
        self.assertEqual(comps["AnimationRef"], "?" + own)
        anim = byml.loads(sarc.read(pack)[own])
        self.assertIn({"IsRetarget": True, "ModelProjectName": asb_carry.RUTO_ANIMATION}, anim["AnimationResources"])
        self.assertEqual(anim["A"], [])  # the player's own fields kept
        # The player's own AnimationParam file is not changed.
        src = "Component/AnimationParam/P.engine__component__AnimationParam.bgyml"
        self.assertEqual(byml.loads(sarc.read(pack)[src]), {"A": []})
        # No carry (the AS could not be made): as before, Link's own Eaten.
        _, plain_ap, _, _ = build.build_pack(members, "Child", liftable=lift)
        self.assertNotEqual(byml.loads(plain_ap)["Components"]["ASRef"], "?" + asb_carry.CARRY_AS)
        # A skin's own AS wins (its clips are retargeted on its own skeleton): no carry there.
        _, skin_ap, _, _ = build.build_pack(members, "Child", liftable=lift, carry=True, as_ref="AS/CoopSkin_X_Child.root.asb")
        self.assertEqual(byml.loads(skin_ap)["Components"]["ASRef"], "?AS/CoopSkin_X_Child.root.asb")
        # Adult: never.
        adult = fake_player_pack("Adult")
        self.assertEqual(build.build_pack(adult, "Adult", liftable=lift, carry=True), build.build_pack(adult, "Adult"))
        # Carry without liftable does nothing.
        self.assertEqual(build.build_pack(members, "Child", carry=True), build.build_pack(members, "Child"))

    def test_pack_carry_and_pvp_together(self):
        # Lift-young (carry) and PvP on real bodies on one copy: the young _Pvp copy gets Ruto's LiftableRef and
        # carried pose AND the PvP pieces; the adult _Pvp copy gets the PvP pieces and is never liftable.
        lift = build.liftable_param(fake_ruto_pack())
        clients = build.attention_clients(fake_attention_pack())
        young = with_pvp(fake_player_pack("Child"), "Child")
        actor = build.pvp_name("CoopPuppet_Child")
        pack, ap, names, _ = build.build_pack(young, "Child", actor=actor, liftable=lift, carry=True, pvp=clients)
        out = sarc.read(pack)
        comps = byml.loads(ap)["Components"]
        own = build.liftable_path(actor, lift[0])
        self.assertEqual(comps["LiftableRef"], "?" + own)
        self.assertIn(own, names)
        self.assertEqual(comps["ASRef"], "?" + asb_carry.CARRY_AS)
        self.assertEqual(comps["AnimationRef"], "?" + asb_carry.carry_animation_path(actor))
        for k, ref in PVP_REFS.items():
            self.assertEqual(comps[k], ref)
        self.assertEqual(comps["AttentionSenderRef"], "?" + build.attention_paths(actor)[0])
        self.assertIn(PVP_ATTACK, names)
        cs = byml.loads(out[build.body_paths(actor)[1]])
        self.assertEqual([e["Name"] for e in cs["CColSensorNamePathAry"]], ["Tgt"])
        # Each side's own pieces are the same as with that switch alone.
        _, pvp_only, _, _ = build.build_pack(young, "Child", actor=actor, pvp=clients)
        _, lift_only, _, _ = build.build_pack(young, "Child", actor=actor, liftable=lift, carry=True)
        pvp_only, lift_only = byml.loads(pvp_only)["Components"], byml.loads(lift_only)["Components"]
        for k in (*PVP_REFS, "AttentionSenderRef", "PhysicsRef"):
            self.assertEqual(comps[k], pvp_only[k], k)
        for k in ("LiftableRef", "ASRef", "AnimationRef"):
            self.assertEqual(comps[k], lift_only[k], k)
        # Adult: the PvP pieces, no lift or carry (byte for byte the PvP-only copy).
        adult = with_pvp(fake_player_pack("Adult"), "Adult")
        a_actor = build.pvp_name("CoopPuppet_Adult")
        both = build.build_pack(adult, "Adult", actor=a_actor, liftable=lift, carry=True, pvp=clients)
        self.assertEqual(both, build.build_pack(adult, "Adult", actor=a_actor, pvp=clients))
        a_comps = byml.loads(both[1])["Components"]
        self.assertNotIn("LiftableRef", a_comps)
        self.assertNotEqual(a_comps.get("ASRef"), "?" + asb_carry.CARRY_AS)
        for k, ref in PVP_REFS.items():
            self.assertEqual(a_comps[k], ref)

    def test_puppet_body_checks(self):
        pc = {"PhysicsRef": "?Component/Physics/P.engine__component__PhysicsParam.bgyml"}
        with self.assertRaises(build.BuildError):  # the made-up pack's physics has no ControllerSet
            build.puppet_body(fake_player_pack("Child"), pc, "CoopPuppet_Child")
        with self.assertRaises(build.BuildError):
            build.puppet_body(with_body(fake_player_pack("Child")), {}, "CoopPuppet_Child")
        members = with_body(fake_player_pack("Child"))
        cc = "Phive/CharacterControllerParam/PBase.phive__CharacterControllerParam.bgyml"
        members[cc] = byml.dumps({"UserContactCollectionName": "Nothing"})
        with self.assertRaises(build.BuildError):
            build.puppet_body(members, pc, "CoopPuppet_Child")
        members = with_body(fake_player_pack("Child"))
        del members["Phive/CharacterControllerParam/PBase.phive__CharacterControllerParam.bgyml"]
        del members["Phive/CharacterControllerParam/PC.phive__CharacterControllerParam.bgyml"]
        with self.assertRaises(build.BuildError):
            build.puppet_body(members, pc, "CoopPuppet_Child")

    def test_merged_files_get_the_whole_chains_entry(self):
        # crash-0116-a: the game parses a param in a heap of its size-table entry, sized for that one file. A file
        # we merge from a `$parent` chain gets the sum of the chain's entries (with the top file's alone the child
        # body's character controller, Player_Child's merged with Player's, crashed GeneralFormatAsync_).
        members = with_body(fake_player_pack("Child"))
        cs, cc = "Phive/ControllerSetParam/", "Phive/CharacterControllerParam/"
        pc_cs, base_cs = cs + "PC.phive__ControllerSetParam.bgyml", cs + "PBase.phive__ControllerSetParam.bgyml"
        pc_cc, base_cc = cc + "PC.phive__CharacterControllerParam.bgyml", cc + "PBase.phive__CharacterControllerParam.bgyml"
        self.assertEqual(build.chain_paths(members, pc_cs), [pc_cs, base_cs])
        self.assertEqual(build.chain_paths(members, base_cs), [base_cs])
        self.assertEqual(build.chain_paths(members, "Not/In.pack.bgyml"), ["Not/In.pack.bgyml"])
        # The game's entries of the cut-down real files (romfs 1.0.x): Player_Child 6,536 + Player 9,504.
        t = rstb.Rstb(by_name={pc_cc: 6536, base_cc: 9504, pc_cs: 27008, base_cs: 34160})
        self.assertEqual(build.chain_entry(t, build.chain_paths(members, pc_cc), b"x" * 1184), 6536 + 9504)
        self.assertEqual(build.chain_entry(t, build.chain_paths(members, pc_cs), b"x" * 5764), 27008 + 34160)
        # A file without entries, or longer than its chain's room, gets its own bytes and the margin.
        self.assertEqual(build.chain_entry(t, ["Not/In.pack.bgyml"], b"x" * 10), 10 + build.PACK_SIZE_MARGIN)
        self.assertEqual(build.chain_entry(rstb.Rstb(by_name={base_cs: 5}), [base_cs], b"x" * 10),
                         10 + build.PACK_SIZE_MARGIN)
        # Every body file of the puppet merges the chain chain_paths names (its source in puppet_body).
        pc = {"PhysicsRef": "?Component/Physics/P.engine__component__PhysicsParam.bgyml"}
        srcs = {path: src for path, (_, src) in build.puppet_body(members, pc, "CoopPuppet_Child").items()}
        self.assertEqual(srcs[build.body_paths("CoopPuppet_Child")[1]], pc_cs)
        self.assertEqual(srcs[build.body_paths("CoopPuppet_Child")[2]], pc_cc)
        # Ruto's liftable param and EnemyCommon's LockOn client merge an in-pack $parent too.
        ruto = fake_ruto_pack()
        lift = build.liftable_param(ruto)[0]
        self.assertEqual(len(build.chain_paths(ruto, lift)), 2)
        amos = fake_attention_pack()
        self.assertEqual(len(build.chain_paths(amos, build._client_src("LockOn"))), 2)
        self.assertEqual(len(build.chain_paths(amos, build._client_src("AutoAim"))), 1)

    def test_pack_no_perception(self):
        _, ap, _, _ = build.build_pack(fake_player_pack("Child"), "Child")
        comps = byml.loads(ap)["Components"]
        for k in ("AffiliationRef", "PerceptionEmitterRef", "PhysicsRef"):
            self.assertNotIn(k, comps)

    def test_perception_members_checks(self):
        self.assertIsNone(build.perception_members({}))
        bad = fake_perception_pack()
        bad[build.PERCEPTION_CONTROLLERS] = byml.dumps({"RigidBodies": [1]})
        with self.assertRaises(build.BuildError):
            build.perception_members(bad)
        bad = fake_perception_pack()
        bad[build.PERCEPTION_PHYSICS] = byml.dumps({"ControllerSetPath": "Work/Phive/Other.gyml"})
        with self.assertRaises(build.BuildError):
            build.perception_members(bad)

    def test_pack_sound_user(self):
        members = fake_player_pack("Child")
        pack, _, names, _ = build.build_pack(members, "Child", sound_user="Enemy_X")
        out = sarc.read(pack)
        comps = byml.loads(out["Actor/CoopPuppet_Child.engine__actor__ActorParam.bgyml"])["Components"]
        own = "Component/SLink/CoopPuppet_Child.engine__component__SLinkParam.bgyml"
        self.assertEqual(comps["SLinkRef"], "?" + own)
        self.assertEqual(byml.loads(out[own]), {"UserName": "Enemy_X"})
        row = build.info_row({"__RowId": "Player_Child", "SLinkUserName": "Player_Child"}, "C", sound_user="Enemy_X")
        self.assertEqual(row["SLinkUserName"], "Enemy_X")
        # Without sound the switch does nothing.
        _, _, names, _ = build.build_pack(members, "Child", sound=False, sound_user="Enemy_X")
        self.assertFalse([n for n in names if "SLink" in n])
        self.assertNotIn("SLinkUserName", build.info_row({"SLinkUserName": "P"}, "C", sound=False, sound_user="E"))

    def test_pack_without_sound(self):
        members = fake_player_pack("Child")
        pack, _, names, _ = build.build_pack(members, "Child", sound=False)
        comps = byml.loads(sarc.read(pack)["Actor/CoopPuppet_Child.engine__actor__ActorParam.bgyml"])["Components"]
        for k in ("SLinkRef", "SoundFootStepRef", "ELinkRef"):
            self.assertNotIn(k, comps)
        self.assertFalse([n for n in names if "SLink" in n or "SoundFootStep" in n])

    def test_no_equipment_fails(self):
        members = fake_player_pack("Child")
        ap = "Actor/Player_Child.engine__actor__ActorParam.bgyml"
        doc = byml.loads(members[ap])
        del doc["Components"]["EquipmentUserRef"]
        members[ap] = byml.dumps(doc)
        with self.assertRaises(build.BuildError):
            build.build_pack(members, "Child")

    def test_equipment_without_table_fails(self):
        members = fake_player_pack("Child")
        for name in ("Player", "Default"):
            p = f"Component/EquipmentUserParam/{name}.game__component__EquipmentUserParam.bgyml"
            doc = byml.loads(members[p])
            doc.pop("BlackboardTableRef")
            members[p] = byml.dumps(doc)
        with self.assertRaises(build.BuildError):
            build.build_pack(members, "Child")

    def test_missing_parent_fails(self):
        members = fake_player_pack("Child")
        del members["Actor/Player_Base.engine__actor__ActorParam.bgyml"]
        with self.assertRaises(build.BuildError):
            build.build_pack(members, "Child")

    def test_tool_hash(self):
        # The same files and hash as launch_test.ps1 (Get-PuppetToolHash): both sides agree.
        d = tempfile.mkdtemp(prefix="puppet_pack_hash_")
        try:
            for name, data in (("b_c.py", b"print(1)\n"), ("Zeta.py", b"x\r\n"), ("__init__.py", b""), ("a.pyc", b"no")):
                with open(os.path.join(d, name), "wb") as f:
                    f.write(data)
            os.mkdir(os.path.join(d, "tests"))
            self.assertEqual(build.tool_hash(d), "b9d822a64cc6285dd1aa300d77f64ea0c86f775240885dad79ae4f2f7694f5e0")
        finally:
            shutil.rmtree(d, ignore_errors=True)
        self.assertEqual(len(build.tool_hash()), 64)  # the tool's own folder

    def test_ref_to_member(self):
        self.assertEqual(build.ref_to_member("?Component/X.bgyml"), "Component/X.bgyml")
        self.assertEqual(build.ref_to_member("Work/Component/X.gyml"), "Component/X.bgyml")
        self.assertIsNone(build.ref_to_member(3))

    def test_info_rows(self):
        rows = [{"__RowId": "B"}, {"__RowId": "D"}]
        player = {"__RowId": "Player_Child", "ActorName": "Player", "ELinkUserName": "Player",
                  "SLinkUserName": "Player_Child", "CalcRadius": 20.0, "FmdbName": "Player_Child"}
        new = build.info_row(player, "C")
        self.assertEqual(new["FmdbName"], "Player_Child")
        self.assertNotIn("ActorName", new)
        self.assertNotIn("ELinkUserName", new)
        self.assertEqual(new["SLinkUserName"], "Player_Child")  # the sound user (issue 131)
        self.assertNotIn("SLinkUserName", build.info_row(player, "C", sound=False))
        self.assertEqual(new["CalcRadius"], 100000.0)
        out = build.upsert_rows(rows, [new, {"__RowId": "A"}])
        self.assertEqual([r["__RowId"] for r in out], ["A", "B", "C", "D"])
        again = build.upsert_rows(out, [new])
        self.assertEqual(len(again), 4)

    def test_stand_in(self):
        # Issue 85: a projectile copy's stand-in is the shootable's model only.
        seed = {"Components": {
            "ModelInfoRef": "?Component/ModelInfo/Seed.engine__component__ModelInfo.bgyml",
            "LODRef": "?Component/LODParam/Shared.engine__component__LODParam.bgyml",
            "PhysicsRef": "?Component/Physics/Seed.engine__component__PhysicsParam.bgyml",
            "ShootableRef": "?Component/ShootableParam/Seed.game__component__ShootableParam.bgyml",
            "ELinkRef": "?Component/ELink/Seed.engine__component__ELinkParam.bgyml",
            "AttackRef": "?GameBalance/AttackParam/Seed.game__gamebalance__AttackParam.bgyml",
        }}
        members = {
            "Actor/Shootable_DekuSeed.engine__actor__ActorParam.bgyml": byml.dumps(seed),
            "Component/ModelInfo/Seed.engine__component__ModelInfo.bgyml": byml.dumps({"FmdbName": "Seed"}),
            "Component/LODParam/Shared.engine__component__LODParam.bgyml": byml.dumps({}),
            "Component/Physics/Seed.engine__component__PhysicsParam.bgyml": byml.dumps({}),
            "Component/ShootableParam/Seed.game__component__ShootableParam.bgyml": byml.dumps({}),
        }
        pack, ap, names = build.build_stand_in(members, "Shootable_DekuSeed", "CoopShot_Seed")
        comps = byml.loads(ap)["Components"]
        self.assertEqual(comps["ModelInfoRef"], seed["Components"]["ModelInfoRef"])
        for k in ("PhysicsRef", "ShootableRef", "ELinkRef", "AttackRef"):
            self.assertNotIn(k, comps)
        self.assertEqual(comps["ActorLogicNodeRef"], build.FIXED_COMPONENTS["ActorLogicNodeRef"])
        self.assertEqual(names, ["Actor/CoopShot_Seed.engine__actor__ActorParam.bgyml",
                                 "Component/LODParam/Shared.engine__component__LODParam.bgyml",
                                 "Component/ModelInfo/Seed.engine__component__ModelInfo.bgyml"])
        self.assertEqual(sorted(sarc.read(pack)), names)
        row = build.stand_in_row({"__RowId": "Shootable_DekuSeed", "ActorName": "Item_DekuSeed",
                                  "ELinkUserName": "Shootable_DekuSeed", "SLinkUserName": "Shootable_DekuSeed",
                                  "FmdbName": "Item_Shootable_DekuSeed"}, "CoopShot_Seed")
        self.assertEqual(row["FmdbName"], "Item_Shootable_DekuSeed")
        for k in ("ActorName", "ELinkUserName", "SLinkUserName"):
            self.assertNotIn(k, row)
        self.assertEqual(row["CalcRadius"], 100000.0)  # calculated wherever it is, as the puppet
        self.assertEqual(row["__RowId"], "CoopShot_Seed")
        with self.assertRaises(build.BuildError):
            build.make_stand_in_param({"PhysicsRef": "x"})
        # The hook's CoopSpawn::StandIn makes these same ones.
        self.assertEqual([n for _, n in build.STAND_INS],
                         ["CoopShot_Seed", "CoopShot_Arrow", "CoopShot_Hook", "CoopShot_HookLong",
                          "CoopShot_Lure", "CoopShot_SinkingLure", "CoopShot_Bomb",
                          "CoopShot_Boomerang", "CoopRide_Epona", "CoopShotFx_Seed", "CoopShotFx_Arrow"])

    def test_effect_stand_in_keeps_the_shootables_xlink_users(self):
        # Issue 166: the hook plays the shootable's hit effect on this copy with the game's own
        # emit, so the copy keeps the shootable's ELink/SLink/XLink params and user names. No AI.
        seed = {"Components": {
            "ModelInfoRef": "?Component/ModelInfo/Seed.engine__component__ModelInfo.bgyml",
            "PhysicsRef": "?Component/Physics/Seed.engine__component__PhysicsParam.bgyml",
            "ELinkRef": "?Component/ELink/Seed.engine__component__ELinkParam.bgyml",
            "SLinkRef": "?Component/SLink/Seed.engine__component__SLinkParam.bgyml",
            "XLinkRef": "?Component/XLink/Seed.engine__component__XLinkParam.bgyml",
            "AIInfoRef": "?AI/AIInfo/Seed.engine__actor__AIInfo.bgyml",
            "AttackRef": "?GameBalance/AttackParam/Seed.game__gamebalance__AttackParam.bgyml",
            "ShootableRef": "?Component/ShootableParam/Seed.game__component__ShootableParam.bgyml",
            "SensorRef": "?Component/Sensor/Seed.engine__component__SensorParam.bgyml",
            "LifeRef": "?Component/Life/Seed.game__component__LifeParam.bgyml",
        }}
        members = {
            "Actor/Shootable_DekuSeed.engine__actor__ActorParam.bgyml": byml.dumps(seed),
            "Component/ModelInfo/Seed.engine__component__ModelInfo.bgyml": byml.dumps({"FmdbName": "Seed"}),
            "Component/Physics/Seed.engine__component__PhysicsParam.bgyml": byml.dumps({}),
            "Component/ELink/Seed.engine__component__ELinkParam.bgyml": byml.dumps({"UserName": "Shootable_DekuSeed"}),
            "Component/SLink/Seed.engine__component__SLinkParam.bgyml": byml.dumps({"UserName": "Shootable_DekuSeed"}),
            "Component/XLink/Seed.engine__component__XLinkParam.bgyml": byml.dumps({}),
        }
        self.assertEqual(build.STAND_IN_XLINK, ("CoopShotFx_Seed", "CoopShotFx_Arrow"))
        pack, ap, names = build.build_stand_in(members, "Shootable_DekuSeed", "CoopShotFx_Seed")
        comps = byml.loads(ap)["Components"]
        for k in ("ModelInfoRef", "ELinkRef", "SLinkRef", "XLinkRef"):
            self.assertEqual(comps[k], seed["Components"][k])
        for k in ("PhysicsRef", "AIInfoRef", "AttackRef", "ShootableRef", "SensorRef", "LifeRef"):
            self.assertNotIn(k, comps)
        # Nothing that can hit, be hit, sense or move by itself: only the model, the xlink users and
        # the shared defaults. (The copy stands where the owner's shot hit an enemy or a switch.)
        self.assertEqual(set(comps), set(build.STAND_IN_COMPONENTS) & set(seed["Components"])
                         | set(build.STAND_IN_XLINK_COMPONENTS) | set(build.FIXED_COMPONENTS))
        files = sarc.read(pack)
        self.assertIn("Component/ELink/Seed.engine__component__ELinkParam.bgyml", files)
        self.assertIn("Component/XLink/Seed.engine__component__XLinkParam.bgyml", files)
        self.assertFalse([n for n in names if n.startswith("AI/")])
        # The plain stand-in of the same shootable is unchanged: the shared XLink param, no users.
        _, plain, _ = build.build_stand_in(members, "Shootable_DekuSeed", "CoopShot_Seed")
        plain = byml.loads(plain)["Components"]
        self.assertEqual(plain["XLinkRef"], build.FIXED_COMPONENTS["XLinkRef"])
        self.assertNotIn("ELinkRef", plain)
        source_row = {"__RowId": "Shootable_DekuSeed", "ActorName": "Item_DekuSeed",
                      "ELinkUserName": "Shootable_DekuSeed", "SLinkUserName": "Shootable_DekuSeed"}
        row = build.stand_in_row(source_row, "CoopShotFx_Seed")
        self.assertEqual(row["ELinkUserName"], "Shootable_DekuSeed")
        self.assertEqual(row["SLinkUserName"], "Shootable_DekuSeed")
        self.assertNotIn("ActorName", row)
        self.assertEqual(row["__RowId"], "CoopShotFx_Seed")
        self.assertNotIn("ELinkUserName", build.stand_in_row(source_row, "CoopShot_Seed"))

    def test_seed_stand_in_plays_its_tail(self):
        # Issue 198: with the impacts' AI template the seed stand-in also plays the seed's Tail.
        from puppet_pack import ainb
        seed = {"Components": {
            "ModelInfoRef": "?Component/ModelInfo/Seed.engine__component__ModelInfo.bgyml",
            "ELinkRef": "?Component/ELink/Seed.engine__component__ELinkParam.bgyml",
        }}
        members = {
            "Actor/Shootable_DekuSeed.engine__actor__ActorParam.bgyml": byml.dumps(seed),
            "Component/ModelInfo/Seed.engine__component__ModelInfo.bgyml": byml.dumps({"FmdbName": "Seed"}),
        }
        pack, ap, names = build.build_stand_in(members, "Shootable_DekuSeed", "CoopShot_Seed",
                                               fake_emit_ainb())
        comps = byml.loads(ap)["Components"]
        self.assertEqual(comps["ModelInfoRef"], seed["Components"]["ModelInfoRef"])
        self.assertEqual(comps["ELinkRef"],
                         "?Component/ELink/CoopShot_Seed.engine__component__ELinkParam.bgyml")
        self.assertNotIn("SLinkRef", comps)
        files = sarc.read(pack)
        self.assertEqual(sorted(files), names)
        self.assertEqual(byml.loads(files[comps["ELinkRef"][1:]]), {"UserName": "Shootable_DekuSeed"})
        self.assertEqual(byml.loads(files[comps["AIInfoRef"][1:]]),
                         {"RootAIRef": "Work/AI/Root/CoopShot_Seed.root.ain"})
        self.assertEqual(ainb.describe(files["AI/CoopShot_Seed.root.ainb"]),
                         ("OneShotXLinkSearchAndEmit", "Tail", True, False, "CoopShot_Seed.root"))
        row = build.stand_in_row({"__RowId": "Shootable_DekuSeed", "ELinkUserName": "Shootable_DekuSeed"},
                                 "CoopShot_Seed", True)
        self.assertEqual(row["ELinkUserName"], "Shootable_DekuSeed")
        # Without the template (or for other stand-ins) it stays model-only.
        _, plain, _ = build.build_stand_in(members, "Shootable_DekuSeed", "CoopShot_Seed")
        self.assertNotIn("ELinkRef", byml.loads(plain)["Components"])
        _, arrow, _ = build.build_stand_in(members, "Shootable_DekuSeed", "CoopShot_Arrow", fake_emit_ainb())
        self.assertNotIn("AIInfoRef", byml.loads(arrow)["Components"])
        self.assertNotIn("ELinkUserName", build.stand_in_row({"ELinkUserName": "x"}, "CoopShot_Arrow", True))

    def test_impact(self):
        # Issue 166: an impact actor plays one xlink key where a copy lands.
        from puppet_pack import ainb
        files = build.impact_files(fake_emit_ainb(), "CoopHit_SeedSpark", "Shootable_DekuSeed", None,
                                   "Hit_AdditionalAttacker")
        comps = byml.loads(files["Actor/CoopHit_SeedSpark.engine__actor__ActorParam.bgyml"])["Components"]
        self.assertEqual(comps["ELinkRef"],
                         "?Component/ELink/CoopHit_SeedSpark.engine__component__ELinkParam.bgyml")
        self.assertNotIn("SLinkRef", comps)
        self.assertNotIn("ModelInfoRef", comps)
        self.assertEqual(comps["ActorLogicNodeRef"], build.FIXED_COMPONENTS["ActorLogicNodeRef"])
        self.assertEqual(byml.loads(files[comps["ELinkRef"][1:]]), {"UserName": "Shootable_DekuSeed"})
        info = byml.loads(files[comps["AIInfoRef"][1:]])
        self.assertEqual(info, {"RootAIRef": "Work/AI/Root/CoopHit_SeedSpark.root.ain"})
        self.assertIn("AI/CoopHit_SeedSpark.root.ainb", files)
        self.assertEqual(ainb.describe(files["AI/CoopHit_SeedSpark.root.ainb"]),
                         ("OneShotXLinkSearchAndEmit", "Hit_AdditionalAttacker", True, False,
                          "CoopHit_SeedSpark.root"))
        sound = build.impact_files(fake_emit_ainb(), "CoopHit_SeedSound", None, "ReactionHit",
                                   "Hit_SlingShot_Bullet")
        comps = byml.loads(sound["Actor/CoopHit_SeedSound.engine__actor__ActorParam.bgyml"])["Components"]
        self.assertNotIn("ELinkRef", comps)
        self.assertEqual(byml.loads(sound[comps["SLinkRef"][1:]]), {"UserName": "ReactionHit"})
        self.assertEqual(ainb.describe(sound["AI/CoopHit_SeedSound.root.ainb"])[1:4],
                         ("Hit_SlingShot_Bullet", False, True))
        row = build.impact_row({"__RowId": "Shootable_ScrubBusinessSees", "ActorName": "Item_DekuNut",
                                "ELinkUserName": "Shootable_ScrubBusinessSees", "SLinkUserName": "Item_DekuNut",
                                "FmdbName": "Item_ScrubBusinessSeed",
                                "ModelProjectName": "Item_ScrubBusinessSeed",
                                "InstanceHeapSize": 129832, "PreActorRenderInfoHeapSize": 7384},
                               "CoopHit_SeedSound", None, "ReactionHit")
        self.assertEqual(row, {"__RowId": "CoopHit_SeedSound", "SLinkUserName": "ReactionHit",
                               "InstanceHeapSize": 129832, "PreActorRenderInfoHeapSize": 0,
                               "CalcRadius": 100000.0, "CoreUsedLevel": byml.UInt(0)})
        # The hook's CoopProjectiles::ImpactActors asks for these.
        self.assertEqual([i[0] for i in build.IMPACTS],
                         ["CoopHit_SeedSpark", "CoopHit_SeedSound", "CoopHit_Arrow", "CoopHit_ArrowSound"])

    def test_bomb_stand_in_keeps_its_animations(self):
        # Issue 169: the held bomb's stand-in has the model and the animation set (its fuse can
        # blink), and no physics, life, liftable or AI.
        bomb = {"Components": {
            "ModelInfoRef": "?Component/ModelInfo/Item_Bomb.engine__component__ModelInfo.bgyml",
            "ASRef": "?AS/LiftableExplodeObject_BallBomb.root.asb",
            "ASInfoRef": "?Component/ASInfo/Default.engine__component__ASInfo.bgyml",
            "AnimationRef": "?Component/AnimationParam/Bomb.engine__component__AnimationParam.bgyml",
            "PhysicsRef": "?Component/Physics/Item_Bomb.engine__component__PhysicsParam.bgyml",
            "LifeRef": "?Component/LifeParam/Item_Bomb.game__component__LifeParam.bgyml",
            "LiftableRef": "?Component/LiftableParam/Item_Bomb.game__component__LiftableParam.bgyml",
            "AIInfoRef": "?AI/AIInfo/Item_Bomb.engine__actor__AIInfo.bgyml",
        }}
        members = {
            "Actor/Item_Bomb.engine__actor__ActorParam.bgyml": byml.dumps(bomb),
            "Component/ModelInfo/Item_Bomb.engine__component__ModelInfo.bgyml":
                byml.dumps({"FmdbName": "Obj_Bomb"}),
            "Component/AnimationParam/Bomb.engine__component__AnimationParam.bgyml": byml.dumps({}),
            "Component/Physics/Item_Bomb.engine__component__PhysicsParam.bgyml": byml.dumps({}),
        }
        pack, ap, names = build.build_stand_in(members, "Item_Bomb", "CoopShot_Bomb")
        comps = byml.loads(ap)["Components"]
        for k in ("ModelInfoRef", "ASRef", "ASInfoRef", "AnimationRef"):
            self.assertEqual(comps[k], bomb["Components"][k])
        for k in ("PhysicsRef", "LifeRef", "LiftableRef", "AIInfoRef"):
            self.assertNotIn(k, comps)
        self.assertIn("Component/AnimationParam/Bomb.engine__component__AnimationParam.bgyml", names)
        self.assertNotIn("Component/Physics/Item_Bomb.engine__component__PhysicsParam.bgyml", names)
        # Other stand-ins stay model-only.
        _, seed_ap, _ = build.build_stand_in(
            {"Actor/Shootable_DekuSeed.engine__actor__ActorParam.bgyml": byml.dumps(bomb)},
            "Shootable_DekuSeed", "CoopShot_Seed")
        self.assertNotIn("ASRef", byml.loads(seed_ap)["Components"])
        # Issue 85: the hookshot tip keeps its animations (the chain), the boomerang does not.
        for src, name in (("Shootable_Hook", "CoopShot_Hook"),
                          ("Shootable_HookLong", "CoopShot_HookLong")):
            _, hook_ap, _ = build.build_stand_in(
                {f"Actor/{src}.engine__actor__ActorParam.bgyml": byml.dumps(bomb)}, src, name)
            hook = byml.loads(hook_ap)["Components"]
            self.assertEqual(hook["ASRef"], bomb["Components"]["ASRef"])
            self.assertNotIn("AIInfoRef", hook)
        _, boom_ap, _ = build.build_stand_in(
            {"Actor/Weapon_Boomerang.engine__actor__ActorParam.bgyml": byml.dumps(bomb)},
            "Weapon_Boomerang", "CoopShot_Boomerang")
        boom = byml.loads(boom_ap)["Components"]
        self.assertNotIn("ASRef", boom)
        self.assertNotIn("PhysicsRef", boom)
        self.assertIn("ModelInfoRef", boom)
        # Issue 199: the ridden horse keeps her animations (the hook plays the rider's horse's), and
        # none of her AI, physics or riding.
        horse_src = dict(bomb, Components=dict(bomb["Components"], AIScheduleRef="x", RidableRef="y",
                                               HorseRef="z"))
        _, horse_ap, _ = build.build_stand_in(
            {"Actor/Animal_Epona_Adult.engine__actor__ActorParam.bgyml": byml.dumps(horse_src)},
            "Animal_Epona_Adult", "CoopRide_Epona")
        horse = byml.loads(horse_ap)["Components"]
        self.assertEqual(horse["ASRef"], bomb["Components"]["ASRef"])
        for k in ("AIScheduleRef", "RidableRef", "HorseRef", "PhysicsRef"):
            self.assertNotIn(k, horse)

    def test_actor_param_chain(self):
        members = fake_player_pack("Adult")
        self.assertEqual([p for p, _ in build.actor_param_chain(members, "Player_Adult")],
                         ["Actor/Player_Adult.engine__actor__ActorParam.bgyml",
                          "Actor/Player_Base.engine__actor__ActorParam.bgyml"])
        # No $parent: the file alone. A deeper chain: every file.
        solo = {"Actor/S.engine__actor__ActorParam.bgyml": byml.dumps({"Components": {}})}
        self.assertEqual(len(build.actor_param_chain(solo, "S")), 1)
        deep = dict(members)
        base = byml.loads(deep["Actor/Player_Base.engine__actor__ActorParam.bgyml"])
        base["$parent"] = "Work/Actor/Root.engine__actor__ActorParam.gyml"
        deep["Actor/Player_Base.engine__actor__ActorParam.bgyml"] = byml.dumps(base)
        deep["Actor/Root.engine__actor__ActorParam.bgyml"] = byml.dumps({"Components": {}})
        self.assertEqual(len(build.actor_param_chain(deep, "Player_Adult")), 3)

    def test_info_row_heap(self):
        adult = {"__RowId": "Player_Adult", "InstanceHeapSize": 10245064}
        self.assertEqual(build.info_row(adult, "A")["InstanceHeapSize"], 10245064)  # the player's
        row = build.info_row(adult, "A", 20000000)  # --heap
        self.assertEqual(row["InstanceHeapSize"], 20000000)
        self.assertEqual(row["__RowId"], "A")
        self.assertEqual(adult["InstanceHeapSize"], 10245064)  # the game's row is not changed
        big = build.info_row({"__RowId": "P", "InstanceHeapSize": byml.UInt(9000000)}, "A", 6000000)
        self.assertIsInstance(big["InstanceHeapSize"], byml.UInt)
        self.assertEqual(big["InstanceHeapSize"].v, 6000000)


@unittest.skipIf(zstandard is None, "needs the zstandard package")
def fake_bntx(name, blocks=None, fmt=0x1C):
    """A one-texture BNTX as tunic.py reads it: the name, a BRTI with the format at +0x1C and a
    BRTD (size at +8) holding the blocks (the image size at BRTI +0x50)."""
    if blocks is None:
        blocks = []
        for i in range(8):  # BC3: 8 alpha bytes, two greens, indices
            blocks.append(bytes(8) + struct.pack("<HHI", 0x2a05 + i * 0x20, 0x1903, 0xE4E4E4E4))
        blocks.append(bytes(16))  # padding
    # The string section has room for either tunic name, as in the game's two tiles.
    strs = b"_STR" + struct.pack("<H", len(name)) + name.encode() + b"\x00"
    head = b"BNTX" + bytes(12) + strs + bytes(0x30 - len(strs)) + b"_DIC" + bytes(4)
    data = b"".join(blocks)
    # (the image's own size at +0x50: look_live.texels)
    brti = b"BRTI" + bytes(0x18) + struct.pack("<I", fmt << 8 | 6) + bytes(0x30) + struct.pack("<I", len(data)) + bytes(4)
    brtd = b"BRTD" + bytes(4) + struct.pack("<Q", 0x10 + len(data)) + data
    return head + brti + brtd


def void_block(r, g, b, a=255):
    """A one-colour ASTC block (void extent, RGBA16)."""
    v = 0x1FC | (3 << 10) | (((1 << 52) - 1) << 12)
    for k, c in enumerate((r, g, b, a)):
        v |= (c << 8 | c) << (64 + 16 * k)
    return v.to_bytes(16, "little")


def bc1_block(c0, c1, idx=0xE4E4E4E4):
    return struct.pack("<HHI", c0, c1, idx)


class TunicExtraTest(unittest.TestCase):
    """The adult's other tunic textures (#202): only green parts change, in place."""

    def test_look_file_round_trip(self):
        from puppet_pack import look_live, tunic
        a, b = fake_bntx("Angin_Tile_A_Alb"), fake_bntx("Mt_Vest_Player_Adult_Alb", [bc1_block(1, 2)], fmt=0x1A)
        blob = look_live.pack([("Angin_Tile_A_Alb", a), ("Mt_Vest_Player_Adult_Alb", b)])
        self.assertEqual(blob[:16], b"COOPLOOK" + struct.pack("<II", 1, 2))
        self.assertEqual(look_live.unpack(blob), [("Angin_Tile_A_Alb", look_live.texels(a)),
                                                  ("Mt_Vest_Player_Adult_Alb", bc1_block(1, 2))])
        self.assertEqual(len(look_live.texels(a)), 9 * 16)
        # each texture's texels with where they start in its file: the data block's header in front
        offs = [o for _, o, _ in look_live.unpack_offsets(blob)]
        self.assertEqual(offs, [look_live.offset(a), look_live.offset(b)])
        self.assertEqual(a[offs[0] - 16:offs[0] - 12], b"BRTD")
        with self.assertRaises(tunic.TunicError):
            look_live.unpack(blob[:-1])
        with self.assertRaises(tunic.TunicError):
            look_live.unpack(b"COOPLOOX" + blob[8:])
        with self.assertRaises(tunic.TunicError):
            look_live.pack([("n" * 64, a)])

    def test_gear_file(self):
        from puppet_pack import look_live
        have = {"Mt_Weapon_Shield_Hylian_Alb": fake_bntx("Mt_Weapon_Shield_Hylian_Alb", [bc1_block(1, 2)], fmt=0x1A),
                "Mt_Weapon_Sword_Master_Alb": fake_bntx("Mt_Weapon_Sword_Master_Alb")}
        said = []
        blob = look_live.gear(have.get, said.append)
        self.assertEqual([n for n, _ in look_live.unpack(blob)],
                         ["Mt_Weapon_Shield_Hylian_Alb", "Mt_Weapon_Sword_Master_Alb"])   # GEAR's order
        self.assertEqual(len(said), 1)
        self.assertIn("Mt_Weapon_Shield_Mirror_Alb", said[0])
        self.assertIsNone(look_live.gear(lambda name: None, said.append))

    def test_gear_names_match_the_module(self):
        """The module's table (hookshot/src/coop_look_parts.cpp) names the same colour textures; the
        metalness ones after them come with metal.bin (look_metal)."""
        import re
        from puppet_pack import look_live, look_metal
        src = os.path.join(os.path.dirname(__file__), "..", "..", "..", "hookshot", "src", "coop_look_parts.cpp")
        with open(src, encoding="utf-8") as f:
            names = re.findall(r'\{"(Mt_[A-Za-z0-9_.]+)", Format::', f.read())
        self.assertEqual(tuple(names) + look_metal.TEXTURES, look_live.GEAR)

    def test_body_boots(self):
        """Each tunic colour's body model names boots of its own, as the module names and marks them
        (hookshot/src/coop_look_parts.cpp BootsName, kLetters, BootsTexels)."""
        import re
        from puppet_pack import look_items, look_live, tunic
        for b in look_items.BODY_BOOTS:
            self.assertIn(b, look_live.GEAR)   # your own Link's are painted as gear
            self.assertEqual(len(look_items.boots_name(b, "pink")), len(b))
        self.assertEqual(look_items.boots_name("Mt_Leather_Player_Child_Alb", "pink"), "PM_Leather_Player_Child_Alb")
        self.assertEqual(look_items.boots_mark("pink"), ord("M"))
        with self.assertRaises(look_items.TunicError):
            look_items.boots_name("Angin_Tile_A_Alb", "red")
        model = b"\0Mt_Boots_Player_Adult_Alb\0Mt_Boots\0Mt_Leather_Player_Child_Alb\0"
        got = look_items.body_model_for(model, "red")
        self.assertEqual(got, b"\0PR_Boots_Player_Adult_Alb\0Mt_Boots\0PR_Leather_Player_Child_Alb\0")
        self.assertEqual(look_items.body_model_for(b"\0nothing\0", "red"), b"\0nothing\0")
        src = os.path.join(os.path.dirname(__file__), "..", "..", "..", "hookshot", "src", "coop_look_parts.cpp")
        with open(src, encoding="utf-8") as f:
            text = f.read()
        letters = dict(re.findall(r"""\{"([a-z]+)", '([A-Z])'\}""", text))
        self.assertEqual(letters, {c: v[0] for c, v in tunic.COLORS.items()})
        boots = re.search(r"kBoots = \{([^}]*)\}", text).group(1)
        self.assertEqual(tuple(re.findall(r'"([^"]+)"', boots)), look_items.BODY_BOOTS)

    def test_names(self):
        from puppet_pack import tunic_extra as te
        for n in te.EXTRA:
            self.assertEqual(len(te.extra_name(n, "red")), len(n))
        self.assertEqual(te.extra_name("Mt_Vest_Player_Adult_Alb", "red"), "Mt_Vest_Coop_R_Adult_Alb")

    def test_is_green(self):
        from puppet_pack import tunic_extra as te
        for c in ((55, 66, 47), (65, 83, 43), (67, 65, 42)):  # tunic greens, olive
            self.assertTrue(te.is_green(c), c)
        for c in ((120, 96, 72), (146, 134, 114), (200, 200, 200), (0, 0, 0), (208, 32, 32)):
            self.assertFalse(te.is_green(c), c)  # leather, cloth, grey, black, red

    def test_ise_round_trip(self):
        # every colour range, every value count of a block: encode, read back the same
        from puppet_pack import texture as tx, tunic_extra as te
        import random
        rnd = random.Random(7)
        for rng in tx._COLOUR_RANGES:
            t, q, b = tx._ISE[rng]
            for count in range(2, 19, 2):
                vals = [(rnd.randrange(3) if t else rnd.randrange(5) if q else None, rnd.randrange(1 << b))
                        for _ in range(count)]
                bits, length = te._ise_encode(vals, rng)
                self.assertEqual(length, tx._ise_bits(count, rng))
                self.assertLess(bits, 1 << length)
                self.assertEqual(te._ise_read(bits << 17, 17, count, rng), vals, (rng, count))

    def test_quant(self):
        from puppet_pack import texture as tx, tunic_extra as te
        for rng in (256, 192, 80, 24, 12):
            b = tx._ISE[rng][2]
            for v in (0, 37, 128, 255):
                d, m = te._quant(rng)[v]
                got = tx._unq_colour(d or 0, m, b, rng)
                self.assertLessEqual(abs(got - v), 256 // rng + 2, (rng, v, got))

    def test_astc_void_extent(self):
        from puppet_pack import tunic_extra as te
        src = fake_bntx("Mt_Vest_Player_Adult_Alb", blocks=[void_block(55, 66, 47), void_block(146, 134, 114)],
                        fmt=0x34)
        out = te.recolor(src, "red")
        self.assertEqual(len(out), len(src))
        start = out.find(b"BRTD") + 0x10
        v = int.from_bytes(out[start:start + 16], "little")
        r, g, b = ((v >> (64 + 16 * k)) & 0xFF for k in range(3))
        self.assertGreater(r, g)
        self.assertGreater(r, b)
        self.assertEqual(out[start + 16:start + 32], src[start + 16:start + 32])  # not green: kept

    def test_bc1_only_green(self):
        from puppet_pack import tunic, tunic_extra as te
        green = tunic.pack565((55, 66, 47))
        brown = tunic.pack565((120, 96, 72))
        src = fake_bntx("Mt_Angin_Acs_Player_Adult_Alb", blocks=[bc1_block(green, green - 1), bc1_block(brown, brown - 1)],
                        fmt=0x1A)
        out = te.recolor(src, "blue")
        start = out.find(b"BRTD") + 0x10
        c0 = tunic.unpack565(struct.unpack_from("<H", out, start)[0])
        self.assertGreater(c0[2], c0[1])  # blue now
        self.assertEqual(out[start + 8:start + 16], src[start + 8:start + 16])  # brown kept
        self.assertEqual(te.for_puppet(src, out, "Mt_Angin_Acs_Player_Adult_Alb", "green"),
                         src.replace(b"Mt_Angin_Acs_Player_Adult_Alb\x00", b"Mt_Angin_Acs_Coop_G_Adult_Alb\x00"))

    def test_direct_never_swaps(self):
        # a bright red first end over a darker second: plain values would swap them, blue
        # contraction cannot hold the red; the candidates still decode in order
        from puppet_pack import texture as tx, tunic_extra as te
        ok = []
        for cand in te._direct((188, 28, 28), (120, 20, 20), 255, 255, False):
            e0, e1 = tx._endpoints(8, cand)
            ok.append(e0[0] > 100 and e1[0] > 100 and e0[0] > 2 * e0[1] and e1[0] > 2 * e1[1])
        self.assertTrue(any(ok[2:]))  # the fallbacks hold a red pair


class TunicTest(unittest.TestCase):
    def test_dark_tile(self):
        # #202: the child's tunic body is the dark tile (ASTC in the game): your colour's copy is
        # the main BC3 tile recoloured darker, under the dark tile's name.
        from puppet_pack import tunic
        tile, dark = fake_bntx(tunic.TILE), fake_bntx(tunic.DARK, blocks=[b"\x77" * 16] * 4, fmt=0x2E)
        own = tunic.self_dark_tile(tile, dark, "red")
        self.assertEqual(len(own), len(tile))
        self.assertEqual(own.count(b"Angin_Tile_A_Dark_Alb\x00"), 1)
        self.assertNotIn(b"Angin_Tile_A_Alb\x00", own)
        self.assertEqual(tunic._brti(own)[0], tunic.BC3)
        light = tunic.self_tile(tile, "red")
        self.assertLess(tunic.average_luma(own), tunic.average_luma(light))
        # the puppet's: renamed to the colour's dark name; green is the game's dark tile renamed
        self.assertIn(b"Angin_Tile_R_Dark_Alb\x00", tunic.dark_tile_for(tile, dark, "red"))
        self.assertEqual(tunic.dark_tile_for(tile, dark, "green"),
                         dark.replace(b"Angin_Tile_A_Dark_Alb\x00", b"Angin_Tile_G_Dark_Alb\x00"))
        # the child model names both; the adult only the main tile
        child = b"FRES\x00Angin_Tile_A_Alb\x00Angin_Tile_A_Dark_Alb\x00"
        self.assertEqual(tunic.model_for(child, "red"),
                         b"FRES\x00Angin_Tile_R_Alb\x00Angin_Tile_R_Dark_Alb\x00")
        self.assertEqual(tunic.model_for(child, "red", dark=False),
                         b"FRES\x00Angin_Tile_R_Alb\x00Angin_Tile_A_Dark_Alb\x00")
        self.assertEqual(tunic.model_for(b"FRES\x00Angin_Tile_A_Alb\x00", "red"), b"FRES\x00Angin_Tile_R_Alb\x00")
        with self.assertRaises(tunic.TunicError):  # string sections not in the same place
            tunic.with_name_of(tile, b"BNTX" + bytes(4) + dark[16:])

    def test_recolor_bc3(self):
        from puppet_pack import tunic
        src = fake_bntx("Angin_Tile_A_Alb")
        out = tunic.recolor_bntx(src, (0xD0, 0x20, 0x20), "Angin_Tile_A_Alb", "Angin_Tile_R_Alb")
        self.assertEqual(len(out), len(src))
        self.assertIn(b"Angin_Tile_R_Alb\x00", out)
        self.assertNotIn(b"Angin_Tile_A_Alb\x00", out)
        start = out.find(b"BRTD") + 0x10
        reds = 0
        for k in range(8):
            q = start + 16 * k
            self.assertEqual(out[q:q + 8], src[q:q + 8])  # alpha untouched
            self.assertEqual(out[q + 12:q + 16], src[q + 12:q + 16])  # indices untouched
            for (c,) in struct.iter_unpack("<H", out[q + 8:q + 12]):
                r, g, b = tunic.unpack565(c)
                self.assertGreaterEqual(r, g)
                self.assertGreaterEqual(r, b)
                reds += r > 2 * g
        self.assertGreater(reds, 8)
        pad = start + 16 * 8
        self.assertEqual(out[pad:pad + 16], bytes(16))  # padding stays zero

    def test_average_becomes_target(self):
        from puppet_pack import tunic
        src = fake_bntx("Angin_Tile_A_Alb")
        out = tunic.recolor_bntx(src, (0x20, 0x50, 0xE0), "Angin_Tile_A_Alb", "Angin_Tile_B_Alb")
        lin = [tunic._lin(c) for c in (0x20, 0x50, 0xE0)]
        self.assertAlmostEqual(tunic.average_luma(out), tunic._luma(lin), delta=0.02)

    def test_bc1_keeps_mode(self):
        from puppet_pack import tunic
        four, n0, n1, idx = True, 5, 9, 0b11100100  # mode flips: swap and remap
        a, b, x = tunic._bc1_order(four, n0, n1, idx)
        self.assertEqual((a, b), (9, 5))
        self.assertEqual(x & 0xFF, 0b10110001)
        a, b, x = tunic._bc1_order(False, 9, 5, 0b11100100)  # 3-colour: 2 and 3 stay
        self.assertEqual((a, b, x & 0xFF), (5, 9, 0b11100001))
        self.assertEqual(tunic._bc1_order(True, 7, 7, 0xFFFF), (7, 7, 0))

    def test_errors(self):
        from puppet_pack import tunic
        with self.assertRaises(tunic.TunicError):
            tunic.recolor_bntx(fake_bntx("Angin_Tile_A_Alb", fmt=0x20), (1, 2, 3), "Angin_Tile_A_Alb", "Angin_Tile_R_Alb")
        with self.assertRaises(tunic.TunicError):
            tunic.rename(b"x", "Angin_Tile_A_Alb", "Angin_Tile_RR_Alb")
        with self.assertRaises(tunic.TunicError):
            tunic.model_for(b"no tile here", "red")

    def test_names_fit(self):
        from puppet_pack import tunic
        letters = [l for l, _ in tunic.COLORS.values()]
        self.assertEqual(len(set(letters)), len(letters))
        self.assertNotIn("A", letters)
        for c in tunic.COLORS:
            self.assertEqual(len(tunic.tile_name(c)), len(tunic.TILE))
            self.assertTrue(tunic.tile_name(c).endswith("_Alb"))


class FullBuildTest(unittest.TestCase):
    """The whole tool on a made-up romfs with made-up dictionaries."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="puppet_pack_test_")
        self.romfs = os.path.join(self.dir, "romfs")
        rnd = random.Random(1)
        words = [f"Component{i}/Param{i}.bgyml" for i in range(50)]
        samples = [" ".join(rnd.choice(words) for _ in range(40)).encode() for _ in range(400)]
        zsdic = {}
        for name, did in (("zs.zsdic", 1), ("pack.zsdic", 4)):
            d = zstandard.train_dictionary(4096, samples, dict_id=did)
            zsdic[name] = d.as_bytes()
        self.dicts_raw = zstandard.ZstdCompressor().compress(sarc.write(zsdic))
        self.write("Pack/ZsDic.pack.zs", self.dicts_raw)
        pack_dict = zstandard.ZstdCompressionDict(zsdic["pack.zsdic"])
        for v in ("Child", "Adult"):
            p = sarc.write(fake_player_pack(v))
            self.write(f"Pack/Actor/Player_{v}.pack.zs",
                       zstandard.ZstdCompressor(dict_data=pack_dict).compress(p))
        plain = zstandard.ZstdCompressor()
        ai = [{"__RowId": "Animal", "CalcRadius": 1.0},
              {"__RowId": "Player_Adult", "ActorName": "Player", "FmdbName": "Player_Adult",
               "InstanceHeapSize": 10245064, "SLinkUserName": "Player_Adult"},
              {"__RowId": "Player_Base", "FmdbName": "Player_Adult", "InstanceHeapSize": 6012792},
              {"__RowId": "Player_Child", "ActorName": "Player", "FmdbName": "Player_Child",
               "InstanceHeapSize": 5692448}]
        gai = [{"__RowId": r["__RowId"]} for r in ai]
        self.ai_bytes = byml.dumps(ai)
        self.write("RSDB/ActorInfo.Product.999.rstbl.byml.zs", plain.compress(self.ai_bytes))
        self.write("RSDB/GameActorInfo.Product.999.rstbl.byml.zs", plain.compress(byml.dumps(gai)))
        t = rstb.Rstb()
        t.set("RSDB/ActorInfo.Product.999.rstbl.byml", 1000)
        t.set("RSDB/GameActorInfo.Product.999.rstbl.byml", 500)
        for v in ("Child", "Adult"):
            t.set(f"Actor/Player_{v}.engine__actor__ActorParam.bgyml", 8000)
        t.set("Actor/Player_Base.engine__actor__ActorParam.bgyml", 13336)
        self.write("System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs", plain.compress(t.dumps()))
        # The tunic tile and the two player models (puppet_pack.tunic).
        self.write("Tex/Angin_Tile_A_Alb.bntx.zs", plain.compress(fake_bntx("Angin_Tile_A_Alb")))
        self.write("Tex/Angin_Tile_A_Dark_Alb.bntx.zs",
                   plain.compress(fake_bntx("Angin_Tile_A_Dark_Alb", blocks=[b"\x77" * 16] * 4, fmt=0x2E)))
        self.write("Tex/Mt_Vest_Player_Adult_Alb.bntx.zs",
                   plain.compress(fake_bntx("Mt_Vest_Player_Adult_Alb", blocks=[void_block(55, 66, 47)] * 2, fmt=0x34)))
        self.write("Tex/Mt_Angin_Acs_Player_Adult_Alb.bntx.zs",
                   plain.compress(fake_bntx("Mt_Angin_Acs_Player_Adult_Alb",
                                            blocks=[struct.pack("<HHI", 0x3A25, 0x2A05, 0)] * 2, fmt=0x1A)))
        # The child's cap knot, headband, collar and back string (hand test of a768c36).
        self.write("Tex/Mt_Cloth_Parts_Player_Child_Alb.bntx.zs",
                   plain.compress(fake_bntx("Mt_Cloth_Parts_Player_Child_Alb", blocks=[void_block(55, 66, 47)] * 2,
                                            fmt=0x34)))
        self.write("Tex/Mt_Vest_Player_Child_Alb.bntx.zs",
                   plain.compress(fake_bntx("Mt_Vest_Player_Child_Alb", blocks=[void_block(55, 66, 47)] * 2, fmt=0x34)))
        # The child's straps (BC1, half green).
        self.write("Tex/Mt_PantsParts_Player_Child_Alb.bntx.zs",
                   plain.compress(fake_bntx("Mt_PantsParts_Player_Child_Alb",
                                            blocks=[struct.pack("<HHI", 0x3A25, 0x2A05, 0)] * 2, fmt=0x1A)))
        for v in ("Child", "Adult"):
            dark = (b"\x15\x00Angin_Tile_A_Dark_Alb\x00\x1f\x00Mt_Cloth_Parts_Player_Child_Alb\x00"
                    b"\x18\x00Mt_Vest_Player_Child_Alb\x00\x1e\x00Mt_PantsParts_Player_Child_Alb\x00") if v == "Child" else \
                b"\x18\x00Mt_Vest_Player_Adult_Alb\x00\x1d\x00Mt_Angin_Acs_Player_Adult_Alb\x00"
            self.write(f"Model/Player_{v}.Player_{v}.bfres.zs",
                       plain.compress(b"FRES" + bytes(12) + b"\x10\x00Angin_Tile_A_Alb\x00" + dark
                                      + f"Player_{v}\x00".encode()))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def write(self, rel, data):
        p = os.path.join(self.romfs, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    def add_hylian_shield(self):
        """The Hylian Shield in the test romfs: its pack, model, texture and ActorInfo rows."""
        from puppet_pack import zs
        plain = zstandard.ZstdCompressor()
        item = "Weapon_Shield_Hylian"
        info = f"Component/ModelInfo/{item}.engine__component__ModelInfo.bgyml"
        ap = {"Components": {"ModelInfoRef": "?" + info, "ShieldRef": "?Component/ShieldParam/X.bgyml"}}
        members = {f"Actor/{item}.engine__actor__ActorParam.bgyml": byml.dumps(ap),
                   info: byml.dumps({"FmdbName": item, "ModelProjectName": item}),
                   "Component/ShieldParam/X.bgyml": byml.dumps({"A": 1})}
        self.write(f"Pack/Actor/{item}.pack.zs", plain.compress(sarc.write(members)))
        model = b"FRES" + bytes(12) + b"\x1b\x00Mt_Weapon_Shield_Hylian_Alb\x00" + f"{item}\x00".encode()
        self.write(f"Model/{item}.{item}.bfres.zs", plain.compress(model))
        tex = fake_bntx("Mt_Weapon_Shield_Hylian_Alb", [bc1_block(1, 2)], fmt=0x1A)
        self.write("Tex/Mt_Weapon_Shield_Hylian_Alb.bntx.zs", plain.compress(tex))
        ai = byml.loads(zs.Dicts.from_romfs(self.romfs).decompress(
            build.read(os.path.join(self.romfs, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))
        ai.append({"__RowId": item, "ActorName": item, "FmdbName": item, "ModelProjectName": item,
                   "InstanceHeapSize": 124720})
        self.write("RSDB/ActorInfo.Product.999.rstbl.byml.zs", plain.compress(byml.dumps(ai)))
        gai = [{"__RowId": r["__RowId"]} for r in ai]
        self.write("RSDB/GameActorInfo.Product.999.rstbl.byml.zs", plain.compress(byml.dumps(gai)))
        return item, model, tex

    def test_look_items(self):
        # Gear colours on the puppets (look_items): per slot, a copy of each coloured item with
        # its own model project and textures, the game's own left as they were.
        from puppet_pack import look_items, look_live, zs
        item, model, tex = self.add_hylian_shield()
        mod = os.path.join(self.dir, "mod")
        said = []
        m = build.build(self.romfs, mod, log=said.append, tunics=("red",), look_slots=2)
        d = zs.Dicts.from_romfs(self.romfs)
        out = os.path.join(mod, "romfs")
        self.assertIsNotNone(m["look_items"], said)
        self.assertEqual(m["look_items"]["slots"], 2)
        self.assertEqual(m["look_items"]["actors"], [item])
        self.assertTrue(any("Weapon_Shield_Mirror" in line for line in said))   # not in this romfs: said
        rows = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(out, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            out, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        for k in (1, 2):
            actor = f"{item}_Look{k}"
            self.assertEqual(rows[actor]["ActorName"], actor)
            self.assertEqual(rows[actor]["ModelProjectName"], actor)
            self.assertEqual(rows[actor]["FmdbName"], item)
            self.assertEqual(rows[actor]["InstanceHeapSize"], 124720)
            files = sarc.read(d.decompress(build.read(os.path.join(out, f"Pack/Actor/{actor}.pack.zs"))))
            self.assertNotIn(f"Actor/{item}.engine__actor__ActorParam.bgyml", files)
            comps = byml.loads(files[f"Actor/{actor}.engine__actor__ActorParam.bgyml"])["Components"]
            self.assertEqual(comps["ShieldRef"], "?Component/ShieldParam/X.bgyml")
            mi = byml.loads(files[comps["ModelInfoRef"][1:]])
            self.assertEqual(mi, {"FmdbName": item, "ModelProjectName": actor})
            got = d.decompress(build.read(os.path.join(out, f"Model/{actor}.{item}.bfres.zs")))
            self.assertEqual(len(got), len(model))
            self.assertIn(f"C{k}_Weapon_Shield_Hylian_Alb\x00".encode(), got)
            self.assertNotIn(b"Mt_Weapon_Shield_Hylian_Alb\x00", got)
            name = look_items.tex_name("Mt_Weapon_Shield_Hylian_Alb", k)
            got = d.decompress(build.read(os.path.join(out, f"Tex/{name}.bntx.zs")))
            renamed = tex.replace(b"Mt_Weapon_Shield_Hylian_Alb\x00", name.encode() + b"\x00")
            self.assertEqual(got, look_live.mark_bntx(renamed, k))   # marked: told from your own by its texels
            self.assertEqual(look_live.texels(got)[:-1], look_live.texels(renamed)[:-1])   # only the last byte
            self.assertEqual(look_live.texels(got)[-1], look_live.texels(renamed)[-1] ^ k)
            for f in (f"Pack/Actor/{actor}.pack", f"Actor/{actor}.engine__actor__ActorParam.bgyml",
                      f"Model/{actor}.{item}.bfres", f"Tex/{name}.bntx"):
                self.assertGreater(t.get(f) or 0, 0, f)
        self.assertEqual(m["look_items"]["copies"], 4)   # two per slot
        self.assertTrue(os.path.exists(os.path.join(out, f"Pack/Actor/{item}_Look4.pack.zs")))
        self.assertFalse(os.path.exists(os.path.join(out, f"Pack/Actor/{item}_Look5.pack.zs")))

        # The game's own item is not in the pack.
        self.assertFalse(os.path.exists(os.path.join(out, f"Pack/Actor/{item}.pack.zs")))
        self.assertFalse(os.path.exists(os.path.join(out, "Tex/Mt_Weapon_Shield_Hylian_Alb.bntx.zs")))
        # None asked for: none made.
        m = build.build(self.romfs, os.path.join(self.dir, "mod0"), log=lambda *_: None, tunics=("red",), look_slots=0)
        self.assertIsNone(m["look_items"])

    def test_blade_copies(self):
        # held-piece: every player item with the game's weapon component (WeaponRef, through its
        # $parents) gets one copy per player number, `<item>_Blade<n>`, on a model project copy of
        # its own per number (the game's bytes and textures), read from the romfs; a shield (no
        # WeaponRef), an enemy's weapon (PerimeterAnalyserRef) and a `_Base` get none.
        from puppet_pack import look_items, zs
        plain = zstandard.ZstdCompressor()
        item = "Weapon_Sword_Kokiri"
        d = zs.Dicts.from_romfs(self.romfs)
        ai = byml.loads(d.decompress(build.read(os.path.join(self.romfs, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))

        def add(name, project, comps, parent=None):
            info = f"Component/ModelInfo/{name}.engine__component__ModelInfo.bgyml"
            ap = {"Components": dict(comps, ModelInfoRef="?" + info)}
            members = {info: byml.dumps({"FmdbName": project, "ModelProjectName": project})}
            if parent:
                ap["$parent"] = "Work/Actor/Weapon_Sword_Base.engine__actor__ActorParam.gyml"
                members["Actor/Weapon_Sword_Base.engine__actor__ActorParam.bgyml"] = byml.dumps(
                    {"Components": {"WeaponRef": "?Component/WeaponParam/W.bgyml"}})
            members[f"Actor/{name}.engine__actor__ActorParam.bgyml"] = byml.dumps(ap)
            self.write(f"Pack/Actor/{name}.pack.zs", plain.compress(sarc.write(members)))
            ai.append({"__RowId": name, "ActorName": name, "FmdbName": project, "ModelProjectName": project,
                       "InstanceHeapSize": 1000})

        add(item, item, {}, parent=True)                                   # WeaponRef from its parent
        add("Weapon_Sword_KokiriTwin", item, {"WeaponRef": "?W"})          # the same model project
        add("Weapon_Shield_Wood", "Weapon_Shield_Wood", {"ShieldRef": "?S"})
        add("Weapon_PoeLamp", item, {"WeaponRef": "?W", "PerimeterAnalyserRef": "?P"})
        add("Weapon_Sword_Base", item, {"WeaponRef": "?W"})
        model = b"FRES" + bytes(12) + b"\x1b\x00Mt_Weapon_Sword_Kokiri_Alb\x00" + f"{item}\x00".encode()
        raw = plain.compress(model)
        self.write(f"Model/{item}.{item}.bfres.zs", raw)
        self.write("RSDB/ActorInfo.Product.999.rstbl.byml.zs", plain.compress(byml.dumps(ai)))
        self.write("RSDB/GameActorInfo.Product.999.rstbl.byml.zs",
                   plain.compress(byml.dumps([{"__RowId": r["__RowId"]} for r in ai])))
        mod = os.path.join(self.dir, "modb")
        said = []
        m = build.build(self.romfs, mod, log=said.append, tunics=(), look_slots=0, blade_copies=3)
        out = os.path.join(mod, "romfs")
        self.assertIsNone(m["look_items"])
        self.assertEqual(m["blade_items"]["copies"], 3, said)
        self.assertEqual(m["blade_items"]["actors"], [item, "Weapon_Sword_KokiriTwin"])
        rows = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(out, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            out, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        model_copies = [f for f in m["blade_items"]["files"] if f.startswith("Model/")]
        self.assertEqual(len(model_copies), 3)   # once per project and number, not per item
        for n in (1, 2, 3):
            project = look_items.blade_name(item, n)
            for name in (item, "Weapon_Sword_KokiriTwin"):
                actor = look_items.blade_name(name, n)
                self.assertEqual(rows[actor]["ModelProjectName"], project)
                self.assertEqual(rows[actor]["FmdbName"], item)
                files = sarc.read(d.decompress(build.read(os.path.join(out, f"Pack/Actor/{actor}.pack.zs"))))
                comps = byml.loads(files[f"Actor/{actor}.engine__actor__ActorParam.bgyml"])["Components"]
                self.assertEqual(byml.loads(files[comps["ModelInfoRef"][1:]]),
                                 {"FmdbName": item, "ModelProjectName": project})
            actor = look_items.blade_name(item, n)
            # The model under the copy's project, byte for byte the game's: the game's textures.
            self.assertEqual(build.read(os.path.join(out, f"Model/{actor}.{item}.bfres.zs")), raw)
            for f in (f"Pack/Actor/{actor}.pack", f"Model/{actor}.{item}.bfres"):
                self.assertGreater(t.get(f) or 0, 0, f)
        self.assertFalse(os.path.exists(os.path.join(out, f"Pack/Actor/{item}_Blade4.pack.zs")))
        self.assertEqual(look_items.BLADE_COPIES, 16)   # the protocol's kMaxPlayers
        for none in ("Weapon_Shield_Wood", "Weapon_PoeLamp", "Weapon_Sword_Base"):
            self.assertFalse(os.path.exists(os.path.join(out, f"Pack/Actor/{none}_Blade1.pack.zs")), none)
        m = build.build(self.romfs, os.path.join(self.dir, "modb0"), log=lambda *_: None, tunics=(), blade_copies=0)
        self.assertIsNone(m["blade_items"])

    def test_look_copy_keeps_the_whole_model_info(self):
        # held-piece: a copy's ModelInfo is the item's own with only the model project renamed,
        # its `$parent`s merged in (the Giant's Knife names its model only through one), so the
        # Biggoron's colour anims and the Giant's broken SubModel stay on the copies.
        from puppet_pack import look_items
        mi = "Component/ModelInfo/{}.engine__component__ModelInfo.bgyml"
        item = "Weapon_Sword_Giant"
        members = {
            f"Actor/{item}.engine__actor__ActorParam.bgyml": byml.dumps(
                {"Components": {"ModelInfoRef": "?" + mi.format(item + "_With_SubModel")}}),
            mi.format(item + "_With_SubModel"): byml.dumps({
                "$parent": "Work/" + mi.format(item)[:-len(".bgyml")] + ".gyml",
                "IsExclusiveControl": True,
                "ModelVariationAnims": [{"Fmab": "A.fmab", "Frame": 1.0}, {"Fmab": "B.fmab", "Frame": 1.0}],
                "SubModels": [{"FmdbName": "Weapon_Sword_BrokenBiggoron", "ModelProjectName": "Weapon_Sword_Biggoron",
                               "SearchModelKey": "Broken"},
                              {"FmdbName": "Other", "ModelProjectName": "Other"}],
            }),
            mi.format(item): byml.dumps({
                "DebugModelScale": {"X": 1.0, "Y": 1.0, "Z": 1.0},
                "FmdbName": "Weapon_Sword_Biggoron", "ModelProjectName": "Weapon_Sword_Biggoron",
                "ModelVariationAnims": [{"Fmab": "A.fmab", "Frame": 1.0}],
            }),
        }
        self.assertEqual(look_items.model_info(members, item)[:2], ("Weapon_Sword_Biggoron", "Weapon_Sword_Biggoron"))
        pack, ap, info = look_items.copy_pack(members, item, 2)
        got = byml.loads(info)
        self.assertNotIn("$parent", got)
        self.assertEqual(got["FmdbName"], "Weapon_Sword_Biggoron")
        self.assertEqual(got["ModelProjectName"], f"{item}_Look2")
        self.assertEqual(got["DebugModelScale"], {"X": 1.0, "Y": 1.0, "Z": 1.0})
        self.assertTrue(got["IsExclusiveControl"])
        self.assertEqual([a["Fmab"] for a in got["ModelVariationAnims"]], ["A.fmab", "B.fmab"])   # the child's list
        self.assertEqual(got["SubModels"][0]["ModelProjectName"], f"{item}_Look2")
        self.assertEqual(got["SubModels"][0]["FmdbName"], "Weapon_Sword_BrokenBiggoron")
        self.assertEqual(got["SubModels"][1]["ModelProjectName"], "Other")   # not the item's project: kept
        self.assertEqual(sarc.read(pack)[look_items.model_info_path(f"{item}_Look2")], info)
        # Its size-table entry is made from both files (crash-0116-a: build.chain_entry).
        self.assertEqual(look_items.model_info_chain(members, item),
                         [mi.format(item + "_With_SubModel"), mi.format(item)])
        self.assertIn(item, look_items.ITEMS)
        self.assertIn("Weapon_Sword_BrokenGiant", look_items.ITEMS)
        # A $parent that is not in the pack, or that loops, is said, not guessed.
        broken = dict(members)
        del broken[mi.format(item)]
        with self.assertRaises(look_items.TunicError):
            look_items.model_info(broken, item)
        looped = dict(members)
        looped[mi.format(item)] = byml.dumps({"$parent": "Work/" + mi.format(item + "_With_SubModel")[:-6] + ".gyml"})
        with self.assertRaises(look_items.TunicError):
            look_items.model_info(looped, item)

    def test_build_from_nup_has_the_gear(self):
        # The 0.11.0 kit made its pack from the player's .nup, which took out only dumpcheck's files:
        # coop_look/gear.bin had the boots alone (your own sword, shields and gauntlets kept the
        # game's colours) and no item copies were made (the puppets' gear neither).
        from puppet_pack import look_live
        item, model, _ = self.add_hylian_shield()
        # Every model of the item's project (a shield's burnt one too), not one hit per pattern.
        self.write(f"Model/{item}.{item}_fracture.bfres.zs", zstandard.ZstdCompressor().compress(model))
        tid = int(dumpcheck.TITLE_ID, 16)
        game = os.path.join(self.dir, "game.nup")
        nup.write_nup(game, [(tid | 0x800, 0x20000, nup.KIND_UPDATE,
                              {k: (v, nup.CODEC_RAW) for k, v in self.romfs_files().items()})])
        with nup.Nup(game) as n:
            from puppet_pack import zs
            got = build.nup_look_files(n, zs.Dicts.from_romfs(self.romfs))
        self.assertEqual(sorted(got), ["Model/Weapon_Shield_Hylian.Weapon_Shield_Hylian.bfres.zs",
                                       "Model/Weapon_Shield_Hylian.Weapon_Shield_Hylian_fracture.bfres.zs",
                                       "Pack/Actor/Weapon_Shield_Hylian.pack.zs",
                                       "Tex/Mt_Weapon_Shield_Hylian_Alb.bntx.zs"])
        a, b = os.path.join(self.dir, "gear_dir"), os.path.join(self.dir, "gear_nup")
        quiet = io.StringIO()
        with contextlib.redirect_stdout(quiet), contextlib.redirect_stderr(quiet):
            self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", a, "--any-version"]), 0)
            self.assertEqual(build.main(["--romfs", game, "--mod-dir", b, "--any-version"]), 0)
        gear = os.path.join(look_live.DIR, look_live.GEAR_FILE)
        names = [n for n, _ in look_live.unpack(build.read(os.path.join(b, gear)))]
        self.assertIn("Mt_Weapon_Shield_Hylian_Alb", names)
        self.assertEqual(build.read(os.path.join(a, gear)), build.read(os.path.join(b, gear)))
        ma, mb = (json.loads(build.read(os.path.join(d, "coop_puppet.json"))) for d in (a, b))
        self.assertEqual(mb["look_items"]["actors"], [item])
        self.assertEqual(ma["look_items"], mb["look_items"])

    def test_look_item_names(self):
        from puppet_pack import look_items, look_live
        for g in look_live.GEAR:
            for k in (1, 4):
                self.assertEqual(len(look_items.tex_name(g, k)), len(g))
        self.assertEqual(look_items.tex_name("Mt_Weapon_Sword_Biggoron_Alb.00", 3), "C3_Weapon_Sword_Biggoron_Alb.00")
        with self.assertRaises(look_items.TunicError):
            look_items.tex_name("Angin_Tile_A_Alb", 1)
        self.assertEqual(look_items.actor_name("Armor_Adult_Zora", 2), "Armor_Adult_Zora_Look2")

    def test_build(self):
        from puppet_pack import zs
        before = {}
        for root, _, names in os.walk(self.romfs):
            for n in names:
                p = os.path.join(root, n)
                before[p] = build.read(p)
        mod = os.path.join(self.dir, "mod")
        m = build.build(self.romfs, mod, log=lambda *_: None)
        # Input untouched.
        for p, b in before.items():
            self.assertEqual(build.read(p), b)
        d = zs.Dicts.from_romfs(self.romfs)
        out = os.path.join(mod, "romfs")
        for v in ("Child", "Adult"):
            raw = build.read(os.path.join(out, f"Pack/Actor/CoopPuppet_{v}.pack.zs"))
            self.assertEqual(zs.frame_dict_id(raw), 4)
            files = sarc.read(d.decompress(raw))
            self.assertIn(f"Actor/CoopPuppet_{v}.engine__actor__ActorParam.bgyml", files)
        ai_raw = build.read(os.path.join(out, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))
        self.assertEqual(zs.frame_dict_id(ai_raw), 0)
        rows = {r["__RowId"]: r for r in byml.loads(d.decompress(ai_raw))}
        self.assertEqual(list(rows), ["Animal", "CoopPuppet_Adult", "CoopPuppet_Child", "Player_Adult",
                                      "Player_Base", "Player_Child"])
        # Each puppet keeps its player's instance heap; the game's rows stay.
        self.assertEqual(rows["CoopPuppet_Adult"]["InstanceHeapSize"], 10245064)
        self.assertEqual(rows["CoopPuppet_Child"]["InstanceHeapSize"], 5692448)
        self.assertEqual(rows["Player_Adult"]["InstanceHeapSize"], 10245064)
        self.assertEqual(rows["CoopPuppet_Adult"]["SLinkUserName"], "Player_Adult")
        gai = byml.loads(d.decompress(build.read(os.path.join(out, "RSDB/GameActorInfo.Product.999.rstbl.byml.zs"))))
        self.assertIn({"__RowId": "CoopPuppet_Child"}, gai)
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            out, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        self.assertGreater(t.get("Pack/Actor/CoopPuppet_Child.pack"), 0)
        # Issue 111: the merged ActorParam gets the player's entry plus Player_Base's.
        self.assertEqual(t.get("Actor/CoopPuppet_Adult.engine__actor__ActorParam.bgyml"), 8000 + 13336)
        self.assertEqual(t.get("Actor/CoopPuppet_Child.engine__actor__ActorParam.bgyml"), 8000 + 13336)
        self.assertEqual(t.get("Actor/Player_Adult.engine__actor__ActorParam.bgyml"), 8000)
        self.assertGreaterEqual(t.get("RSDB/ActorInfo.Product.999.rstbl.byml"), 1000)
        man = json.loads(build.read(os.path.join(mod, "coop_puppet.json")))
        self.assertEqual([a["actor"] for a in man["actors"]], ["CoopPuppet_Child", "CoopPuppet_Adult"])
        self.assertIs(man["sound"], True)
        self.assertEqual(man["footsteps"], "puppet")
        self.assertIsNone(man["sound_user"])
        # The puppet's footstep param has its own entry; the made-up table has none for the
        # player's file, so it is the file plus the margin.
        for a in man["actors"]:
            fs = a["footstep"]
            self.assertEqual(fs["param"], f"Component/SoundFootStepParam/{a['actor']}.game__component__SoundFootStepParam.bgyml")
            self.assertEqual(t.get(fs["param"]), fs["size"])
            self.assertGreater(fs["size"], build.PACK_SIZE_MARGIN)
        self.assertEqual(man["shots"], [])  # the made-up romfs has no shootables
        self.assertEqual(man["impacts"], [])  # nor the Business Scrub's seed
        self.assertEqual([a["instance_heap"] for a in man["actors"]], [5692448, 10245064])
        self.assertEqual([a["param_size"] for a in man["actors"]], [21336, 21336])
        for a in man["actors"]:
            self.assertIn("EquipmentUser_Weapon", a["equipment_keys"])
            self.assertIn("EquipmentUser_Shield", a["equipment_keys"])
        self.assertEqual(man["tool_sha256"], build.tool_hash())  # the kit compares it (pack-hint)
        # Running again gives the same files.
        first = {r: build.read(os.path.join(out, r)) for r in
                 ("RSDB/ActorInfo.Product.999.rstbl.byml.zs", "Pack/Actor/CoopPuppet_Child.pack.zs")}
        build.build(self.romfs, mod, log=lambda *_: None)
        for r, b in first.items():
            self.assertEqual(build.read(os.path.join(out, r)), b)

    def test_build_impacts(self):
        # Issue 166: with the Business Scrub seed's pack in the romfs, the impacts are made.
        from puppet_pack import zs
        d = zs.Dicts.from_romfs(self.romfs)
        scrub = {"Actor/Shootable_ScrubBusinessSees.engine__actor__ActorParam.bgyml":
                 byml.dumps({"Components": {}}), build.IMPACT_TEMPLATE: fake_emit_ainb()}
        self.write("Pack/Actor/Shootable_ScrubBusinessSees.pack.zs", d.compress(sarc.write(scrub), "pack.zsdic"))
        ai = byml.loads(self.ai_bytes) + [{"__RowId": "Shootable_ScrubBusinessSees", "InstanceHeapSize": 129832,
                                           "ELinkUserName": "Shootable_ScrubBusinessSees"}]
        ai.sort(key=lambda r: r["__RowId"])
        self.write("RSDB/ActorInfo.Product.999.rstbl.byml.zs", zstandard.ZstdCompressor().compress(byml.dumps(ai)))
        mod = os.path.join(self.dir, "mod")
        build.build(self.romfs, mod, log=lambda *_: None)
        man = json.loads(build.read(os.path.join(mod, "coop_puppet.json")))
        self.assertEqual([(i["actor"], i["elink"], i["slink"], i["key"]) for i in man["impacts"]],
                         [tuple(i) for i in build.IMPACTS])
        out = os.path.join(mod, "romfs")
        rows = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(out, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        self.assertEqual(rows["CoopHit_Arrow"]["ELinkUserName"], "NormalArrow")
        self.assertEqual(rows["CoopHit_Arrow"]["SLinkUserName"], "NormalArrow")
        self.assertNotIn("ELinkUserName", rows["CoopHit_ArrowSound"])
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            out, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        for i in man["impacts"]:
            files = sarc.read(d.decompress(build.read(os.path.join(out, i["pack"]))))
            self.assertEqual(sorted(files), i["members"])
            for m in i["members"]:
                self.assertGreater(t.get(m), len(files[m]))
            self.assertGreater(t.get(i["pack"][:-3]), 0)

    def test_build_heap_switch(self):
        from puppet_pack import zs
        mod = os.path.join(self.dir, "mod2")
        m = build.build(self.romfs, mod, log=lambda *_: None, heaps={"CoopPuppet_Adult": 20000000})
        self.assertEqual([a["instance_heap"] for a in m["actors"]], [5692448, 20000000])
        rows = byml.loads(zs.Dicts.from_romfs(self.romfs).decompress(build.read(
            os.path.join(mod, "romfs", "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))
        heap = {r["__RowId"]: r.get("InstanceHeapSize") for r in rows}
        self.assertEqual(heap["CoopPuppet_Adult"], 20000000)
        self.assertEqual(heap["Player_Adult"], 10245064)  # the switch never touches the game's row

    def test_missing_parent_size_fails(self):
        t = rstb.Rstb()
        t.set("RSDB/ActorInfo.Product.999.rstbl.byml", 1000)
        t.set("RSDB/GameActorInfo.Product.999.rstbl.byml", 500)
        for v in ("Child", "Adult"):
            t.set(f"Actor/Player_{v}.engine__actor__ActorParam.bgyml", 8000)
        self.write("System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs",
                   zstandard.ZstdCompressor().compress(t.dumps()))
        with self.assertRaises(build.BuildError):
            build.build(self.romfs, os.path.join(self.dir, "mod4"), log=lambda *_: None)

    def test_main_heap_switch(self):
        mod = os.path.join(self.dir, "mod3")
        self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", mod, "--heap", "Bad=1", "--any-version"]), 2)
        self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", mod, "--any-version",
                                     "--heap", "CoopPuppet_Adult=20000000"]), 0)
        man = json.loads(build.read(os.path.join(mod, "coop_puppet.json")))
        self.assertEqual(man["actors"][1]["instance_heap"], 20000000)

    def test_footstep_size_follows_player_entry(self):
        from puppet_pack import zs
        d = zs.Dicts.from_romfs(self.romfs)
        rs = "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(self.romfs, rs))))
        t.set("Component/SoundFootStepParam/Player_Child.game__component__SoundFootStepParam.bgyml", 5584)
        self.write(rs, zstandard.ZstdCompressor().compress(t.dumps()))
        m = build.build(self.romfs, os.path.join(self.dir, "mod6"), log=lambda *_: None)
        child = m["actors"][0]["footstep"]
        self.assertGreaterEqual(child["size"], 5584)
        self.assertLess(child["size"], 5584 + 0x200)

    def test_main_no_seed_trail(self):
        # Issue 198: the stand-ins are model-only by default (the game dropped every create of the
        # seed copy with its Tail); --seed-trail asks for the trail, --no-seed-trail still works.
        from unittest import mock
        mod = os.path.join(self.dir, "mod7")
        for extra, want in (([], False), (["--no-seed-trail"], False), (["--seed-trail"], True),
                            (["--seed-trail", "--no-seed-trail"], False)):
            with mock.patch.object(build, "build", return_value={}) as b:
                build.main(["--romfs", self.romfs, "--mod-dir", mod, "--any-version"] + extra)
            self.assertIs(b.call_args.kwargs["stand_in_effects"], want)
        # The real build takes it too (this romfs has no stand-in sources, so nothing changes).
        m = build.build(self.romfs, os.path.join(self.dir, "mod8"), log=lambda *_: None,
                        stand_in_effects=False)
        self.assertTrue(all(s["effect"] is None for s in m["shots"]))

    def test_main_player_footsteps(self):
        from puppet_pack import zs
        mod = os.path.join(self.dir, "mod5")
        self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", mod, "--any-version",
                                     "--player-footsteps"]), 0)
        man = json.loads(build.read(os.path.join(mod, "coop_puppet.json")))
        self.assertIs(man["sound"], True)
        self.assertEqual(man["footsteps"], "player")
        self.assertEqual([a["footstep"] for a in man["actors"] if not a["tunic"]], [None, None])
        d = zs.Dicts.from_romfs(self.romfs)
        files = sarc.read(d.decompress(build.read(os.path.join(mod, "romfs", "Pack/Actor/CoopPuppet_Child.pack.zs"))))
        self.assertIn("Component/SoundFootStepParam/Player_Child.game__component__SoundFootStepParam.bgyml", files)
        self.assertNotIn("Component/SoundFootStepParam/CoopPuppet_Child.game__component__SoundFootStepParam.bgyml", files)

    def test_main_sound_user(self):
        from puppet_pack import zs
        mod = os.path.join(self.dir, "mod7")
        # Only an SLink user some actor has.
        self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", mod, "--any-version",
                                     "--sound-user", "Nobody"]), 1)
        self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", mod, "--any-version",
                                     "--sound-user", "Player_Adult"]), 0)
        man = json.loads(build.read(os.path.join(mod, "coop_puppet.json")))
        self.assertEqual(man["sound_user"], "Player_Adult")
        d = zs.Dicts.from_romfs(self.romfs)
        out = os.path.join(mod, "romfs")
        rows = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(out, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        self.assertEqual(rows["CoopPuppet_Child"]["SLinkUserName"], "Player_Adult")
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            out, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        self.assertGreater(t.get("Component/SLink/CoopPuppet_Child.engine__component__SLinkParam.bgyml"), 0)

    def test_main_no_sound(self):
        from puppet_pack import zs
        mod = os.path.join(self.dir, "mod4")
        self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", mod, "--any-version", "--no-sound"]), 0)
        man = json.loads(build.read(os.path.join(mod, "coop_puppet.json")))
        self.assertIs(man["sound"], False)
        self.assertEqual(man["footsteps"], "player")
        self.assertEqual([a["footstep"] for a in man["actors"] if not a["tunic"]], [None, None])
        d = zs.Dicts.from_romfs(self.romfs)
        out = os.path.join(mod, "romfs")
        rows = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(out, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        self.assertNotIn("SLinkUserName", rows["CoopPuppet_Adult"])
        files = sarc.read(d.decompress(build.read(os.path.join(out, "Pack/Actor/CoopPuppet_Adult.pack.zs"))))
        self.assertFalse([n for n in files if "SLink" in n or "SoundFootStep" in n])

    def add_perception_and_guards(self):
        """InterestSpotActor (the puppets' perception), a stealth guard placed in Castle and a townsperson
        placed in Town (both made-up packs)."""
        pack_dict = zstandard.ZstdCompressionDict(sarc.read(zstandard.ZstdDecompressor().decompress(
            self.dicts_raw))["pack.zsdic"])
        z = zstandard.ZstdCompressor(dict_data=pack_dict)
        self.write(f"Pack/Actor/{build.PERCEPTION_SOURCE}.pack.zs", z.compress(sarc.write(fake_perception_pack())))
        for name, aff in (("Npc_Guard", build.GUARD_AFFILIATION),
                          ("Npc_Town", "Component/AffiliationParam/NpcCommon.game__component__AffiliationParam.bgyml")):
            self.write(f"Pack/Actor/{name}.pack.zs", z.compress(sarc.write({build.actor_param_path(name): byml.dumps(
                {"Components": {"AffiliationRef": "?" + aff}})})))
        plain = zstandard.ZstdCompressor()
        rs = os.path.join(self.romfs, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs")
        t = rstb.Rstb.loads(zstandard.ZstdDecompressor().decompress(build.read(rs)))
        for path in (build.PERCEPTION_PHYSICS, build.PERCEPTION_CONTROLLERS):
            t.set(path, 800)
        self.write("System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs", plain.compress(t.dumps()))
        for area, scene, actors in (("Castle", "Castle_Dynamic", ["Npc_Guard", "Enemy_X"]),
                                    ("Town", "Town_Static", ["Npc_Town", "Npc_NoPack"])):
            self.write(f"Banc/{area}/{scene}.bcett.byml.zs", plain.compress(byml.dumps(
                {"Actors": [{"Gyaml": a, "Hash": byml.UInt64(i + 1), "Translate": [0.0, 0.0, 0.0]}
                            for i, a in enumerate(actors)], "FilePath": "x"})))

    def test_build_quiet_puppets_for_guard_scenes(self):
        # Issue 150 (review-boss-sync must-fix): with perception (on by default) each puppet has a
        # quiet copy (no perception) and the scenes with a stealth guard are listed for it.
        self.add_perception_and_guards()
        d = zs.Dicts.from_romfs(self.romfs)
        self.assertEqual(build.guard_scenes(self.romfs, d), {"Castle": ["Npc_Guard"]})
        mod = os.path.join(self.dir, "mod_quiet")
        m = build.build(self.romfs, mod, log=lambda *_: None, tunics=("red",), look_slots=0, perception=True)
        self.assertTrue(m["perception"])
        self.assertEqual(m["quiet_scenes"], ["Castle"])
        self.assertEqual(m["guards"], {"Castle": ["Npc_Guard"]})
        made = {a["actor"]: a for a in m["actors"]}
        for loud in ("CoopPuppet_Child", "CoopPuppet_Adult", "CoopPuppet_Child_Red", "CoopPuppet_Adult_Red"):
            quiet = build.quiet_name(loud)
            self.assertIn(quiet, made)
            self.assertIsNotNone(made[loud]["perception"])
            self.assertFalse(made[loud]["quiet"])
            self.assertIsNone(made[quiet]["perception"])
            self.assertTrue(made[quiet]["quiet"])
            self.assertEqual(made[quiet]["tunic"], made[loud]["tunic"])
            files = sarc.read(d.decompress(build.read(os.path.join(mod, "romfs", made[quiet]["pack"]))))
            comps = byml.loads(files[build.actor_param_path(quiet)])["Components"]
            for k in ("AffiliationRef", "PerceptionEmitterRef", "PhysicsRef"):
                self.assertNotIn(k, comps)
            loud_comps = byml.loads(sarc.read(d.decompress(build.read(os.path.join(
                mod, "romfs", made[loud]["pack"]))))[build.actor_param_path(loud)])["Components"]
            self.assertIn("PerceptionEmitterRef", loud_comps)
        rows = {r["__RowId"] for r in byml.loads(d.decompress(build.read(
            os.path.join(mod, "romfs", "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        self.assertTrue({"CoopPuppet_ChildQuiet", "CoopPuppet_AdultQuiet_Red"} <= rows)

    def test_build_no_perception(self):
        self.add_perception_and_guards()
        m = build.build(self.romfs, os.path.join(self.dir, "mod_loud"), log=lambda *_: None, perception=False)
        self.assertFalse(m["perception"])
        # On by default since the lane proof (perception-on-a): the default pack is the perceived one with quiet copies.
        d = build.build(self.romfs, os.path.join(self.dir, "mod_default"), log=lambda *_: None, look_slots=0)
        self.assertTrue(d["perception"])
        self.assertEqual(d["quiet_scenes"], ["Castle"])
        self.assertEqual(m["quiet_scenes"], [])
        self.assertFalse([a for a in m["actors"] if a["quiet"] or a["perception"]])

    def test_build_body(self):
        # --puppet-body: every puppet (and its quiet copy) has its body files, each with a size-table entry.
        zsdic = sarc.read(zstandard.ZstdDecompressor().decompressobj().decompress(self.dicts_raw))
        pack_dict = zstandard.ZstdCompressionDict(zsdic["pack.zsdic"])
        for v in ("Child", "Adult"):
            self.write(f"Pack/Actor/Player_{v}.pack.zs", zstandard.ZstdCompressor(dict_data=pack_dict).compress(
                sarc.write(with_body(fake_player_pack(v)))))
        self.add_perception_and_guards()
        m = build.build(self.romfs, os.path.join(self.dir, "mod_body"), log=lambda *_: None, look_slots=0, body=True)
        self.assertTrue(m["body"])
        self.assertTrue(m["actors"])
        for a in m["actors"]:
            self.assertEqual(sorted(a["body"]), sorted(build.body_paths(a["actor"])), a["actor"])
            self.assertTrue(all(size >= build.PACK_SIZE_MARGIN for size in a["body"].values()))
        d = build.build(self.romfs, os.path.join(self.dir, "mod_nobody"), log=lambda *_: None, look_slots=0)
        self.assertFalse(d["body"])
        self.assertFalse([a for a in d["actors"] if a["body"]])

    def test_build_pvp(self):
        # --puppet-pvp: every puppet (and its quiet copy) has a _Pvp twin, each of its files in the size table.
        zsdic = sarc.read(zstandard.ZstdDecompressor().decompressobj().decompress(self.dicts_raw))
        z = zstandard.ZstdCompressor(dict_data=zstandard.ZstdCompressionDict(zsdic["pack.zsdic"]))
        for v in ("Child", "Adult"):
            self.write(f"Pack/Actor/Player_{v}.pack.zs", z.compress(sarc.write(with_pvp(fake_player_pack(v), v))))
        self.add_perception_and_guards()
        with self.assertRaises(build.BuildError):  # no ATTENTION_SOURCE pack
            build.build(self.romfs, os.path.join(self.dir, "mod_nopvp_src"), log=lambda *_: None, look_slots=0,
                        pvp=True)
        self.write(f"Pack/Actor/{build.ATTENTION_SOURCE}.pack.zs", z.compress(sarc.write(fake_attention_pack())))
        m = build.build(self.romfs, os.path.join(self.dir, "mod_pvp"), log=lambda *_: None, look_slots=0, pvp=True)
        self.assertTrue(m["pvp"])
        names = [a["actor"] for a in m["actors"]]
        plain = [n for n in names if not n.endswith(build.PVP_SUFFIX)]
        self.assertTrue(plain)
        self.assertEqual(sorted(build.pvp_name(n) for n in plain), sorted(set(names) - set(plain)))
        for a in m["actors"]:
            if a["actor"].endswith(build.PVP_SUFFIX):
                sender, clients = build.attention_paths(a["actor"])
                self.assertEqual(sorted(a["pvp"]), sorted([sender, *clients.values()]), a["actor"])
                self.assertTrue(all(size >= build.PACK_SIZE_MARGIN for size in a["pvp"].values()))
                self.assertEqual(sorted(a["body"]), sorted(build.body_paths(a["actor"])))
            else:
                self.assertNotIn("pvp", a)
                self.assertIsNone(a["body"])  # the plain puppet has no body without --puppet-body
        off = build.build(self.romfs, os.path.join(self.dir, "mod_pvp_off"), log=lambda *_: None, look_slots=0)
        self.assertNotIn("pvp", off)
        self.assertEqual([a["actor"] for a in off["actors"]], plain)
        on_plain = {a["actor"]: a for a in m["actors"] if a["actor"] in plain}
        for a in off["actors"]:  # the plain puppets as before
            self.assertEqual(a["pack_bytes"], on_plain[a["actor"]]["pack_bytes"], a["actor"])
            self.assertEqual(build.read(os.path.join(self.dir, "mod_pvp_off", "romfs", a["pack"])),
                             build.read(os.path.join(self.dir, "mod_pvp", "romfs", a["pack"])), a["actor"])

    def test_build_liftable_and_pvp_together(self):
        # --puppet-liftable and --puppet-pvp in one pack: every young puppet and its _Pvp twin are liftable with the
        # carried pose; the adult ones never; every _Pvp twin has its PvP files.
        zsdic = sarc.read(zstandard.ZstdDecompressor().decompressobj().decompress(self.dicts_raw))
        z = zstandard.ZstdCompressor(dict_data=zstandard.ZstdCompressionDict(zsdic["pack.zsdic"]))
        for v in ("Child", "Adult"):
            self.write(f"Pack/Actor/Player_{v}.pack.zs", z.compress(sarc.write(with_pvp(fake_player_pack(v), v))))
        self.add_perception_and_guards()
        self.write(f"Pack/Actor/{build.ATTENTION_SOURCE}.pack.zs", z.compress(sarc.write(fake_attention_pack())))
        self.write(f"Pack/Actor/{build.LIFTABLE_SOURCE}.pack.zs", z.compress(sarc.write(fake_ruto_pack())))
        zd = zstandard.ZstdCompressor(dict_data=zstandard.ZstdCompressionDict(zsdic[build.CARRY_AS_DICT]))
        self.write(f"AS/Player_{asb_carry.CARRY_VARIANTS[0]}.root.asb.zs", zd.compress(fake_asb()))
        m = build.build(self.romfs, os.path.join(self.dir, "mod_lift_pvp"), log=lambda *_: None, look_slots=0,
                        liftable=True, pvp=True)
        self.assertTrue(m["pvp"])
        twins = [a for a in m["actors"] if a["actor"].endswith(build.PVP_SUFFIX)]
        self.assertTrue([a for a in twins if a["from"] == "Player_Child"])
        self.assertTrue([a for a in twins if a["from"] == "Player_Adult"])
        for a in m["actors"]:
            if a["from"] == "Player_Child" and not a["skin"]:
                self.assertEqual(a["liftable"]["carry"]["as"], asb_carry.CARRY_AS, a["actor"])
                pack = sarc.read(build.zs.Dicts.from_romfs(self.romfs).decompress(
                    build.read(os.path.join(self.dir, "mod_lift_pvp", "romfs", a["pack"]))))
                comps = byml.loads(pack[build.actor_param_path(a["actor"])])["Components"]
                self.assertEqual(comps["LiftableRef"], "?" + a["liftable"]["param"], a["actor"])
                self.assertEqual(comps["ASRef"], "?" + asb_carry.CARRY_AS, a["actor"])
            elif a["from"] == "Player_Adult":
                self.assertNotIn("liftable", a, a["actor"])
            if a in twins:
                sender, clients = build.attention_paths(a["actor"])
                self.assertEqual(sorted(a["pvp"]), sorted([sender, *clients.values()]), a["actor"])
            else:
                self.assertNotIn("pvp", a)

    def test_quiet_name(self):
        self.assertEqual(build.quiet_name("CoopPuppet_Child"), "CoopPuppet_ChildQuiet")
        self.assertEqual(build.quiet_name("CoopPuppet_Adult_Green"), "CoopPuppet_AdultQuiet_Green")
        with self.assertRaises(build.BuildError):
            build.quiet_name("Npc_Guard")

    def test_build_writes_the_room_table(self):
        mod = os.path.join(self.dir, "mod_rooms")
        self.write("Banc/Dun/Dun_Static.bcett.byml.zs", zstandard.ZstdCompressor().compress(byml.dumps(
            {"Actors": [{"Gyaml": "A", "Hash": byml.UInt64(1), "Translate": [1.0, 2.0, 3.0],
                         "Dynamic": {"DefaultDungeonRoomIndex": 4}},
                        {"Gyaml": "B", "Hash": byml.UInt64(2), "Translate": [9.0, 9.0, 9.0]}],
             "FilePath": "x"})))
        m = build.build(self.romfs, mod, log=lambda *_: None)
        text = build.read(os.path.join(mod, "coop_rooms.txt")).decode()
        self.assertEqual([line for line in text.splitlines() if not line.startswith("#")], ["Dun 4 1.00 2.00 3.00"])
        self.assertEqual(m["rooms"], {"scenes": 1, "rooms": 1, "placements": 1})

    def test_build_writes_the_map_table(self):
        self.write(map_captures.CAPTURE.replace("*", "MapCaptureInfo.game__MapCaptureInfo"),
                   byml.dumps(MapCapturesTest.CAPTURE))
        self.write(map_captures.MENU.replace("*", "MapMenuInfo.game__ui__MapMenuInfo"),
                   byml.dumps(MapCapturesTest.MENU))
        m = build.build(self.romfs, os.path.join(self.dir, "mod_maps"), log=lambda *_: None)
        text = build.read(os.path.join(self.dir, "mod_maps", "coop_maps.txt")).decode()
        self.assertEqual([line for line in text.splitlines() if not line.startswith("#")], MapCapturesTest.ROWS)
        self.assertEqual(m["maps"], 2)

    def test_place_test(self):
        from puppet_pack import place_test, zs
        mod = os.path.join(self.dir, "mod")
        banc = "Banc/Test/Test_Dynamic.bcett.byml.zs"
        self.write(banc, zstandard.ZstdCompressor().compress(byml.dumps(
            {"Actors": [{"Gyaml": "Animal", "Hash": byml.UInt64(5)}], "FilePath": "x"})))
        with self.assertRaises(build.BuildError):
            place_test.place(self.romfs, mod, banc, [("CoopPuppet_Child", (1, 2, 3))], log=lambda *_: None)
        build.build(self.romfs, mod, log=lambda *_: None)
        place_test.place(self.romfs, mod, banc, [("CoopPuppet_Child", (1, 2, 3))], log=lambda *_: None)
        d = zs.Dicts.from_romfs(self.romfs)
        doc = byml.loads(d.decompress(build.read(os.path.join(mod, "romfs", banc))))
        self.assertEqual([a["Gyaml"] for a in doc["Actors"]], ["Animal", "CoopPuppet_Child"])
        self.assertEqual(doc["Actors"][1]["Translate"], [1.0, 2.0, 3.0])
        self.assertNotIn("Dynamic", doc["Actors"][1])
        # Instance parameters go into the placement's Dynamic table, only for that actor.
        place_test.place(self.romfs, mod, banc, [("CoopPuppet_Child", (1, 2, 3)), ("Animal", (4, 5, 6))],
                         log=lambda *_: None,
                         params={"CoopPuppet_Child": {"EquipmentUser_Weapon": "Weapon_Sword_Kokiri"}})
        doc = byml.loads(d.decompress(build.read(os.path.join(mod, "romfs", banc))))
        self.assertEqual(doc["Actors"][1]["Dynamic"], {"EquipmentUser_Weapon": "Weapon_Sword_Kokiri"})
        self.assertNotIn("Dynamic", doc["Actors"][2])

    def romfs_files(self):
        out = {}
        for root, _, names in os.walk(self.romfs):
            for n in names:
                p = os.path.join(root, n)
                out[os.path.relpath(p, self.romfs).replace("\\", "/")] = build.read(p)
        return out

    def test_build_from_nup_matches_folder(self):
        files = self.romfs_files()
        tid = int(dumpcheck.TITLE_ID, 16)
        # Base holds old tables and stale packs; the update holds the real ones.
        base = {k: (b"stale " + k.encode(), nup.CODEC_RAW) for k in files}
        base["Model/Only_In_Base.bfres.zs"] = (b"x", nup.CODEC_RAW)
        update = {k: (v, nup.CODEC_ZSTD if i % 2 else nup.CODEC_RAW) for i, (k, v) in enumerate(files.items())}
        game = os.path.join(self.dir, "game.nup")
        nup.write_nup(game, [(tid, 0, nup.KIND_BASE, base), (tid | 0x800, 0x20000, nup.KIND_UPDATE, update)])
        a, b = os.path.join(self.dir, "from_dir"), os.path.join(self.dir, "from_nup")
        quiet = io.StringIO()
        with contextlib.redirect_stdout(quiet), contextlib.redirect_stderr(quiet):
            self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", a, "--any-version"]), 0)
            self.assertEqual(build.main(["--romfs", game, "--mod-dir", b, "--any-version"]), 0)
            # Version 999 is not the game's: refused without --any-version.
            self.assertEqual(build.main(["--romfs", game, "--mod-dir", b + "2"]), 1)

        def tree(d):
            return {os.path.relpath(os.path.join(r, n), d): build.read(os.path.join(r, n))
                    for r, _, ns in os.walk(d) for n in ns}
        self.assertEqual(tree(a), tree(b))
        self.assertGreater(len(tree(a)), 4)

    def test_build_body_boots(self):
        """The child's boots (his leather texture, named in his model): a copy per colour, named by
        that colour's body model, marked (look_items.BODY_BOOTS); the adult's are not in this romfs."""
        from puppet_pack import look_items, look_live, zs
        plain = zs.Dicts.from_romfs(self.romfs)
        leather = fake_bntx("Mt_Leather_Player_Child_Alb", blocks=[b"\x55" * 16] * 2, fmt=0x32)
        self.write("Tex/Mt_Leather_Player_Child_Alb.bntx.zs", plain.compress(leather))
        m_rel = "Model/Player_Child.Player_Child.bfres.zs"
        model = plain.decompress(build.read(os.path.join(self.romfs, m_rel)))
        self.write(m_rel, plain.compress(model + b"\x1b\x00Mt_Leather_Player_Child_Alb\x00"))
        said = []
        mod = os.path.join(self.dir, "mod_boots")
        build.build(self.romfs, mod, log=said.append, tunics=["pink", "green"])
        out = os.path.join(mod, "romfs")
        d = zs.Dicts.from_romfs(self.romfs)
        for c, letter in (("pink", "M"), ("green", "G")):
            child = d.decompress(build.read(os.path.join(out, f"Model/CoopTunic_Child_{c.capitalize()}.Player_Child.bfres.zs")))
            self.assertIn(f"P{letter}_Leather_Player_Child_Alb\x00".encode(), child)
            self.assertNotIn(b"Mt_Leather_Player_Child_Alb\x00", child)
            got = d.decompress(build.read(os.path.join(out, f"Tex/P{letter}_Leather_Player_Child_Alb.bntx.zs")))
            renamed = leather.replace(b"Mt_Leather_Player_Child_Alb\x00", f"P{letter}_Leather_Player_Child_Alb\x00".encode())
            self.assertEqual(got, look_live.mark_bntx(renamed, ord(letter)))
        self.assertFalse(os.path.exists(os.path.join(out, "Tex/PM_Boots_Player_Adult_Alb.bntx.zs")))
        self.assertTrue(any("Mt_Boots_Player_Adult_Alb" in line for line in said))
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            out, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        self.assertGreater(t.get("Tex/PM_Leather_Player_Child_Alb.bntx") or 0, 0)

    def test_build_tunics(self):
        from puppet_pack import tunic, zs
        mod = os.path.join(self.dir, "mod_t")
        m = build.build(self.romfs, mod, log=lambda *_: None, tunics=["red", "blue", "green"], test_tunic="red")
        d = zs.Dicts.from_romfs(self.romfs)
        out = os.path.join(mod, "romfs")
        self.assertEqual(sorted(m["tunics"]), ["blue", "green", "red"])
        game_tile = d.decompress(build.read(os.path.join(self.romfs, "Tex/Angin_Tile_A_Alb.bntx.zs")))
        # green: the game's own tile under the green name, not recoloured (#202)
        green = d.decompress(build.read(os.path.join(out, "Tex/Angin_Tile_G_Alb.bntx.zs")))
        # (the puppets' copies are marked: their texels are never your own Link's, tunic.TUNIC_MARK)
        from puppet_pack import look_live as _ll
        unmark = lambda b: _ll.mark_bntx(b, tunic.TUNIC_MARK)
        # (your own Link's copies are marked too, their own way: tunic.SELF_MARK, npc-colour)
        selfmark = lambda b: _ll.mark_bntx(b, tunic.SELF_MARK)
        self.assertEqual(unmark(green), game_tile.replace(b"Angin_Tile_A_Alb\x00", b"Angin_Tile_G_Alb\x00"))
        self.assertNotEqual(_ll.texels(green), _ll.texels(game_tile))
        # your own Link: per colour but green, the tile recoloured under the game's name, beside
        # romfs/ (the kit copies one in); as long as the game's tile, so its size entry stands
        self.assertEqual(m["self_tiles"], {"red": "coop_self/red.bntx.zs", "blue": "coop_self/blue.bntx.zs"})
        self.assertFalse(os.path.exists(os.path.join(mod, "coop_self", "green.bntx.zs")))
        self.assertFalse(os.path.exists(os.path.join(out, "Tex", "Angin_Tile_A_Alb.bntx.zs")))
        for c in ("red", "blue"):
            own = d.decompress(build.read(os.path.join(mod, m["self_tiles"][c])))
            self.assertEqual(len(own), len(game_tile))
            self.assertIn(b"Angin_Tile_A_Alb\x00", own)
            self.assertNotEqual(own, game_tile)
            recoloured = d.decompress(build.read(os.path.join(out, f"Tex/{tunic.tile_name(c)}.bntx.zs")))
            self.assertEqual(own, selfmark(unmark(recoloured)).replace(tunic.tile_name(c).encode() + b"\x00",
                                                                       b"Angin_Tile_A_Alb\x00"))
        for c in ("red", "blue", "green"):
            tile = d.decompress(build.read(os.path.join(out, f"Tex/{tunic.tile_name(c)}.bntx.zs")))
            self.assertIn(tunic.tile_name(c).encode() + b"\x00", tile)
            for v in ("Child", "Adult"):
                model = d.decompress(build.read(os.path.join(out, f"Model/CoopTunic_{v}_{c.capitalize()}.Player_{v}.bfres.zs")))
                self.assertIn(tunic.tile_name(c).encode() + b"\x00", model)
                self.assertIn(f"Player_{v}".encode(), model)
                files = sarc.read(d.decompress(build.read(os.path.join(out, f"Pack/Actor/CoopPuppet_{v}_{c.capitalize()}.pack.zs"))))
                ap = byml.loads(files[f"Actor/CoopPuppet_{v}_{c.capitalize()}.engine__actor__ActorParam.bgyml"])
                mi = ap["Components"]["ModelInfoRef"]
                self.assertEqual(byml.loads(files[mi[1:]]),
                                 {"FmdbName": f"Player_{v}", "ModelProjectName": f"CoopTunic_{v}_{c.capitalize()}"})
        rows = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(out, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        self.assertEqual(rows["CoopPuppet_Adult_Blue"]["ModelProjectName"], "CoopTunic_Adult_Blue")
        # --test-tunic: the plain puppets use the red model too
        self.assertEqual(rows["CoopPuppet_Adult"]["ModelProjectName"], "CoopTunic_Adult_Red")
        self.assertEqual(m["test_tunic"], "red")
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            out, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        self.assertIsNotNone(t.get("Tex/Angin_Tile_B_Alb.bntx"))
        self.assertIsNotNone(t.get("Model/CoopTunic_Child_Red.Player_Child.bfres"))
        # the dark tile (#202): per colour for the puppets, your own beside romfs/, and the child
        # models name the colour's dark tile
        game_dark = d.decompress(build.read(os.path.join(self.romfs, "Tex/Angin_Tile_A_Dark_Alb.bntx.zs")))
        self.assertEqual(m["self_dark_tiles"],
                         {"red": "coop_self/red.dark.bntx.zs", "blue": "coop_self/blue.dark.bntx.zs"})
        for c in ("red", "blue"):
            own = d.decompress(build.read(os.path.join(mod, m["self_dark_tiles"][c])))
            self.assertEqual(own, selfmark(tunic.self_dark_tile(game_tile, game_dark, c)))
            self.assertGreaterEqual(t.get(f"Tex/{tunic.dark_name(c)}.bntx"), len(own))
        self.assertEqual(unmark(d.decompress(build.read(os.path.join(out, "Tex/Angin_Tile_G_Dark_Alb.bntx.zs")))),
                         game_dark.replace(b"Angin_Tile_A_Dark_Alb\x00", b"Angin_Tile_G_Dark_Alb\x00"))
        child = d.decompress(build.read(os.path.join(out, "Model/CoopTunic_Child_Blue.Player_Child.bfres.zs")))
        self.assertIn(b"Angin_Tile_B_Dark_Alb\x00", child)
        self.assertNotIn(b"Angin_Tile_A_Dark_Alb\x00", child)
        # the adult's other tunic textures (#202): per colour for the puppets, the adult model
        # copies name them, and your own in self_files with the tiles
        adult = d.decompress(build.read(os.path.join(out, "Model/CoopTunic_Adult_Red.Player_Adult.bfres.zs")))
        for n in ("Mt_Vest", "Mt_Angin_Acs"):
            self.assertIn(f"{n}_Coop_R_Adult_Alb\x00".encode(), adult)
            self.assertNotIn(f"{n}_Player_Adult_Alb\x00".encode(), adult)
            self.assertTrue(os.path.isfile(os.path.join(out, f"Tex/{n}_Coop_G_Adult_Alb.bntx.zs")))
        # the child's cap knot / headband / collar and back string: the same, in the child copies
        child_red = d.decompress(build.read(os.path.join(out, "Model/CoopTunic_Child_Red.Player_Child.bfres.zs")))
        for n in ("Mt_Cloth_Parts", "Mt_Vest", "Mt_PantsParts"):
            self.assertIn(f"{n}_Coop_R_Child_Alb\x00".encode(), child_red)
            self.assertNotIn(f"{n}_Player_Child_Alb\x00".encode(), child_red)
            self.assertTrue(os.path.isfile(os.path.join(out, f"Tex/{n}_Coop_R_Child_Alb.bntx.zs")))
        self.assertNotIn(b"Mt_Vest_Coop_R_Adult_Alb\x00", child_red)
        self.assertNotIn(b"_Child_Alb\x00", adult)
        # your own Link: its own S copies and model copies naming them (colour-agree), never the
        # puppets' letter copies
        self.assertEqual(m["self_files"]["red"], {
            "Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/red.Angin_Tile_S_Alb.bntx.zs",
            "Tex/Angin_Tile_S_Dark_Alb.bntx.zs": "coop_self/red.Angin_Tile_S_Dark_Alb.bntx.zs",
            "Tex/Mt_Cloth_Parts_Coop_S_Child_Alb.bntx.zs": "coop_self/red.Mt_Cloth_Parts_Coop_S_Child_Alb.bntx.zs",
            "Tex/Mt_Vest_Coop_S_Child_Alb.bntx.zs": "coop_self/red.Mt_Vest_Coop_S_Child_Alb.bntx.zs",
            "Tex/Mt_PantsParts_Coop_S_Child_Alb.bntx.zs": "coop_self/red.Mt_PantsParts_Coop_S_Child_Alb.bntx.zs",
            "Tex/Mt_Vest_Coop_S_Adult_Alb.bntx.zs": "coop_self/red.Mt_Vest_Coop_S_Adult_Alb.bntx.zs",
            "Tex/Mt_Angin_Acs_Coop_S_Adult_Alb.bntx.zs": "coop_self/red.Mt_Angin_Acs_Coop_S_Adult_Alb.bntx.zs",
            "Model/Player_Adult.Player_Adult.bfres.zs": "coop_self/red.Player_Adult.bfres.zs",
            "Model/Player_Child.Player_Child.bfres.zs": "coop_self/red.Player_Child.bfres.zs"})
        for v, names in (("Child", ("Angin_Tile_S_Alb", "Angin_Tile_S_Dark_Alb", "Mt_Cloth_Parts_Coop_S_Child_Alb",
                                    "Mt_Vest_Coop_S_Child_Alb", "Mt_PantsParts_Coop_S_Child_Alb")),
                         ("Adult", ("Angin_Tile_S_Alb", "Mt_Vest_Coop_S_Adult_Alb", "Mt_Angin_Acs_Coop_S_Adult_Alb"))):
            own_model = d.decompress(build.read(os.path.join(mod, f"coop_self/red.Player_{v}.bfres.zs")))
            game_model = d.decompress(build.read(os.path.join(self.romfs, f"Model/Player_{v}.Player_{v}.bfres.zs")))
            self.assertEqual(len(own_model), len(game_model))   # the game's size entry stands
            for n in names:
                self.assertIn(n.encode() + b"\x00", own_model, n)
            self.assertNotIn(b"Angin_Tile_R_Alb\x00", own_model)
            self.assertNotIn(b"_Coop_R_", own_model)
        for rel in m["self_files"]["red"]:
            if rel.startswith("Tex/"):
                self.assertIsNotNone(t.get(rel[:-3]), rel)   # a size entry for every own texture
        # your own textures hold the texels the module finds them by (look files); the puppets' not
        own_tile = d.decompress(build.read(os.path.join(mod, m["self_files"]["red"]["Tex/Angin_Tile_S_Alb.bntx.zs"])))
        pup_tile = d.decompress(build.read(os.path.join(out, "Tex/Angin_Tile_R_Alb.bntx.zs")))
        red_look = dict(_ll.unpack(build.read(os.path.join(mod, "coop_look/red.bin"))))
        self.assertEqual(_ll.texels(own_tile), red_look["Angin_Tile_A_Alb"])
        self.assertNotEqual(_ll.texels(pup_tile), red_look["Angin_Tile_A_Alb"])
        self.assertEqual(_ll.texels(selfmark(unmark(pup_tile))), red_look["Angin_Tile_A_Alb"])
        # green has its own S copies too (npc-colour): the game's tunic textures are on Mido's and
        # Saria's models, so your own Link never wears them; green's are the game's, marked
        self.assertEqual(sorted(m["self_files"]["green"]), sorted(m["self_files"]["red"]))
        own_green = d.decompress(build.read(os.path.join(mod, m["self_files"]["green"]["Tex/Angin_Tile_S_Alb.bntx.zs"])))
        self.assertEqual(own_green, selfmark(game_tile).replace(b"Angin_Tile_A_Alb\x00", b"Angin_Tile_S_Alb\x00"))
        own_green_dark = d.decompress(build.read(os.path.join(
            mod, m["self_files"]["green"]["Tex/Angin_Tile_S_Dark_Alb.bntx.zs"])))
        self.assertEqual(own_green_dark,
                         selfmark(game_dark).replace(b"Angin_Tile_A_Dark_Alb\x00", b"Angin_Tile_S_Dark_Alb\x00"))
        self.assertGreaterEqual(t.get("Tex/Angin_Tile_S_Dark_Alb.bntx"), max(len(own_green_dark), len(game_tile)))
        green_model = d.decompress(build.read(os.path.join(mod, "coop_self/green.Player_Child.bfres.zs")))
        self.assertIn(b"Angin_Tile_S_Alb\x00", green_model)
        self.assertNotIn(b"Angin_Tile_A_Alb\x00", green_model)
        self.assertNotIn(b"Mt_Vest_Player_Child_Alb\x00", green_model)
        self.assertFalse(os.path.exists(os.path.join(mod, "coop_self", "green.dark.bntx.zs")))
        vest = d.decompress(build.read(os.path.join(mod, m["self_files"]["red"]["Tex/Mt_Vest_Coop_S_Adult_Alb.bntx.zs"])))
        game_vest = d.decompress(build.read(os.path.join(self.romfs, "Tex/Mt_Vest_Player_Adult_Alb.bntx.zs")))
        self.assertEqual(len(vest), len(game_vest))
        self.assertNotEqual(vest, game_vest)
        # the game's dark tile's entry holds the BC3 copy the kit puts in its place
        self.assertGreaterEqual(t.get("Tex/Angin_Tile_A_Dark_Alb.bntx"), len(game_tile))
        # your colour changed while the game runs (look_live): per colour, green too, the texels of
        # every tunic texture your Link wears, each as long as the game's own (written over them)
        from puppet_pack import look_live, tunic_extra
        self.assertEqual(m["look_files"], {c: f"coop_look/{c}.bin" for c in ("red", "blue", "green")})
        names = ["Angin_Tile_A_Alb", "Angin_Tile_A_Dark_Alb"] + list(tunic_extra.EXTRA)
        game = {n: look_live.texels(d.decompress(build.read(os.path.join(self.romfs, f"Tex/{n}.bntx.zs"))))
                for n in names}
        looks = {c: look_live.unpack(build.read(os.path.join(mod, m["look_files"][c]))) for c in m["look_files"]}
        # green: the game's own marked as your own green copies are; no colour's texels are ever a
        # texture of the game's (the module finds your own Link's by them: npc-colour)
        self.assertEqual(looks["green"], [(n, look_live.mark(game[n], tunic.SELF_MARK)) for n in names])
        for c in looks:
            for n, data in looks[c]:
                self.assertNotIn(data, game.values(), (c, n))
        for c in ("red", "blue"):
            self.assertEqual([n for n, _ in looks[c]], names)
            for n, data in looks[c]:
                if n != tunic.DARK:   # (the dark tile: the kit's BC3 copy, below)
                    self.assertEqual(len(data), len(game[n]), n)
            self.assertNotEqual(looks[c][0][1], game[names[0]])
        # the tile is your own tile's; the dark tile the BC3 copy the kit puts over the game's for your
        # colour (self_dark_tiles): what the game's memory holds then
        self.assertEqual(looks["red"][0][1], look_live.texels(d.decompress(build.read(
            os.path.join(mod, m["self_tiles"]["red"])))))
        self.assertEqual(looks["red"][1][1], look_live.texels(d.decompress(build.read(
            os.path.join(mod, m["self_dark_tiles"]["red"])))))

    def test_build_look_files_without_green(self):
        from puppet_pack import look_live
        mod = os.path.join(self.dir, "mod_l")
        m = build.build(self.romfs, mod, log=lambda *_: None, tunics=["red"])
        # green is always there (to go back to), marked as your own green copies would be: never the
        # game's own texels, which Mido's model shares (npc-colour)
        from puppet_pack import tunic
        self.assertEqual(sorted(m["look_files"]), ["green", "red"])
        green = look_live.unpack(build.read(os.path.join(mod, "coop_look", "green.bin")))
        self.assertEqual(green[0][0], "Angin_Tile_A_Alb")
        d = zs.Dicts.from_romfs(self.romfs)
        game_tile = look_live.texels(d.decompress(build.read(os.path.join(self.romfs, "Tex/Angin_Tile_A_Alb.bntx.zs"))))
        self.assertEqual(green[0][1], look_live.mark(game_tile, tunic.SELF_MARK))

    def test_build_no_tile_no_tunics(self):
        os.remove(os.path.join(self.romfs, "Tex", "Angin_Tile_A_Alb.bntx.zs"))
        m = build.build(self.romfs, os.path.join(self.dir, "mod_n"), log=lambda *_: None, tunics=["red"])
        self.assertEqual(m["tunics"], {})
        self.assertEqual(m["self_tiles"], {})
        self.assertFalse(m["tunics_off"])  # asked for, none made: the kit says so
        self.assertEqual(sorted(a["actor"] for a in m["actors"]), ["CoopPuppet_Adult", "CoopPuppet_Child"])

    def test_build_from_nup_writes_the_room_table(self):
        # A .nup gave "coop_rooms.txt: 0 placements in 0 rooms of 0 dungeons": only the pack's own
        # sources were taken out of it, not the scene files the room table is made from.
        self.write("Banc/Dun/Dun_Static.bcett.byml.zs", zstandard.ZstdCompressor().compress(byml.dumps(
            {"Actors": [{"Gyaml": "A", "Hash": byml.UInt64(1), "Translate": [1.0, 2.0, 3.0],
                         "Dynamic": {"DefaultDungeonRoomIndex": 4}}], "FilePath": "x"})))
        self.write("Banc/Dun/Dun_Static.other.zs", b"not a scene file")
        tid = int(dumpcheck.TITLE_ID, 16)
        game = os.path.join(self.dir, "game.nup")
        nup.write_nup(game, [(tid | 0x800, 0x20000, nup.KIND_UPDATE,
                              {k: (v, nup.CODEC_RAW) for k, v in self.romfs_files().items()})])
        with nup.Nup(game) as n:
            self.assertEqual(build.nup_scene_files(n), ["Banc/Dun/Dun_Static.bcett.byml.zs"])
        mod = os.path.join(self.dir, "from_nup_rooms")
        quiet = io.StringIO()
        with contextlib.redirect_stdout(quiet), contextlib.redirect_stderr(quiet):
            self.assertEqual(build.main(["--romfs", game, "--mod-dir", mod, "--any-version"]), 0)
        text = build.read(os.path.join(mod, "coop_rooms.txt")).decode()
        self.assertEqual([line for line in text.splitlines() if not line.startswith("#")], ["Dun 4 1.00 2.00 3.00"])
        manifest = json.loads(build.read(os.path.join(mod, "coop_puppet.json")))
        self.assertEqual(manifest["rooms"], {"scenes": 1, "rooms": 1, "placements": 1})

    def make_model_mod(self, name="model_mod", exefs=False, change=True):
        """A made-up character model mod (skin.py): the adult player pointed at a model of its own,
        the child left as the game's, an AS root replaced in place, new files and an in-place one."""
        from puppet_pack import zs
        d = zs.Dicts.from_romfs(self.romfs)
        plain = zstandard.ZstdCompressor()
        root = os.path.join(self.dir, name)
        romfs = os.path.join(root, "romfs")

        def put(rel, data):
            path = os.path.join(romfs, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        pack_dict = zstandard.ZstdCompressionDict(d.by_name["pack.zsdic"].as_bytes())
        adult = fake_player_pack("Adult")
        if change:
            ap_path = "Actor/Player_Adult.engine__actor__ActorParam.bgyml"
            doc = byml.loads(adult[ap_path])
            doc["Components"]["ModelInfoRef"] = "?Component/ModelInfo/Player_Mod_Adult.engine__component__ModelInfo.bgyml"
            doc["Components"]["AnimationRef"] = "?Component/AnimationParam/Player_Mod_Adult.engine__component__AnimationParam.bgyml"
            adult[ap_path] = byml.dumps(doc)
            adult["Component/ModelInfo/Player_Mod_Adult.engine__component__ModelInfo.bgyml"] = byml.dumps(
                {"FmdbName": "Player_Mod_Adult", "ModelProjectName": "Npc_Mod_Adult"})
            adult["Component/AnimationParam/Player_Mod_Adult.engine__component__AnimationParam.bgyml"] = byml.dumps(
                {"AnimationResources": [{"IsRetarget": True, "ModelProjectName": "Mod__Adult_01_Animation",
                                         "RetargetModel": "Skeleton"}]})
        for v, files in (("Adult", adult), ("Child", fake_player_pack("Child"))):
            put(f"Pack/Actor/Player_{v}.pack.zs", zstandard.ZstdCompressor(dict_data=pack_dict).compress(sarc.write(files)))
        put("AS/Player_Adult.root.asb.zs", plain.compress(b"ASB mod root"))
        put("Model/Npc_Mod_Adult.Player_Mod_Adult.bfres.zs", plain.compress(b"FRES mod model"))
        put("Model/Mod__Adult_01_Animation.Skeleton.bfres.zs", plain.compress(b"FRES mod anim"))
        put("Model/Player_Adult.Player_Adult.bfres.zs", plain.compress(b"FRES replaced in place"))
        put("Mals/USen.Product.110.sarc.zs", plain.compress(b"text"))
        # Gear: a Goron tunic made for the mod's body (its own model), and a sword it ships unchanged.
        goron = "Armor_Adult_Goron"
        g_info = "Component/ModelInfo/Armor_Goron.engine__component__ModelInfo.bgyml"
        put(f"Pack/Actor/{goron}.pack.zs", plain.compress(sarc.write({
            f"Actor/{goron}.engine__actor__ActorParam.bgyml": byml.dumps(
                {"Components": {"ModelInfoRef": "?" + g_info, "ArmorRef": "?Component/ArmorParam/G.bgyml"}}),
            g_info: byml.dumps({"FmdbName": "Player_Mod_Adult_Goron", "ModelProjectName": "Npc_Mod_Adult"}),
            "Component/ArmorParam/G.bgyml": byml.dumps({"A": 1})})))
        put("Model/Npc_Mod_Adult.Player_Mod_Adult_Goron.bfres.zs", plain.compress(b"FRES mod goron"))
        sword = "Weapon_Sword_Kokiri"
        s_info = "Component/ModelInfo/Sword.engine__component__ModelInfo.bgyml"
        put(f"Pack/Actor/{sword}.pack.zs", plain.compress(sarc.write({
            f"Actor/{sword}.engine__actor__ActorParam.bgyml": byml.dumps({"Components": {"ModelInfoRef": "?" + s_info}}),
            s_info: byml.dumps({"FmdbName": sword, "ModelProjectName": sword})})))
        ai = byml.loads(d.decompress(build.read(os.path.join(self.romfs, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))
        if not any(r["__RowId"] == goron for r in ai):
            ai.append({"__RowId": goron, "ActorName": goron, "FmdbName": "Armor_Adult_Goron",
                       "ModelProjectName": "Armor_Adult_Goron", "InstanceHeapSize": 1000})
            ai.append({"__RowId": sword, "FmdbName": sword, "ModelProjectName": sword})
            self.write("RSDB/ActorInfo.Product.999.rstbl.byml.zs", plain.compress(byml.dumps(ai)))
            gai = [{"__RowId": r["__RowId"]} for r in ai]
            self.write("RSDB/GameActorInfo.Product.999.rstbl.byml.zs", plain.compress(byml.dumps(gai)))
        t = rstb.Rstb()
        t.set("Model/Npc_Mod_Adult.Player_Mod_Adult.bfres", 777777)
        t.set("AS/Player_Adult.root.asb", 3000000)
        put("System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs", plain.compress(t.dumps()))
        if exefs:
            os.makedirs(os.path.join(root, "exefs"))
        # The game's own table knows the model the mod replaces in place.
        tpath = os.path.join(self.romfs, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs")
        game = rstb.Rstb.loads(d.decompress(build.read(tpath)))
        game.set("Model/Player_Adult.Player_Adult.bfres", 50000)
        game.set("AS/Player_Adult.root.asb", 2000000)
        self.write("System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs", plain.compress(game.dumps()))
        return root

    def test_build_skin_effects_only_on_request(self):
        """A mod's own copy of a game effect (a recoloured sword glow) reaches every sword with that
        effect, so a skin takes it only with skin_effects (--skin-effects), as it is, with a size entry and
        in the skin's hash."""
        mod_src = self.make_model_mod()
        plain = zstandard.ZstdCompressor()
        rel = "Effect/Weapon_Sword_Master.Test.vsetb.byml.zs"
        data = b"VFXB red glow" + bytes(51)
        self.write(rel, plain.compress(b"VFXB game glow" + bytes(50)))
        path = os.path.join(mod_src, "romfs", *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(plain.compress(data))
        d = zs.Dicts.from_romfs(self.romfs)
        plain_mod = os.path.join(self.dir, "mod_no_effects")
        [off] = build.build(self.romfs, plain_mod, log=lambda _l: None, skins=[("Mod", mod_src)], look_slots=0)["skins"]
        self.assertNotIn(rel, off["files"])
        self.assertFalse(os.path.exists(os.path.join(plain_mod, "romfs", *rel.split("/"))))
        fx_mod = os.path.join(self.dir, "mod_effects")
        [on] = build.build(self.romfs, fx_mod, log=lambda _l: None, skins=[("Mod", mod_src)], look_slots=0,
                           skin_effects=True)["skins"]
        self.assertIn(rel, on["files"])
        self.assertEqual(d.decompress(build.read(os.path.join(fx_mod, "romfs", *rel.split("/")))), data)
        self.assertNotEqual(on["sha256"], off["sha256"])
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            fx_mod, "romfs", "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        self.assertGreaterEqual(t.get(rel[:-3]) or 0, len(data))

    def test_build_skin_effects_new_replaced_and_other_version(self):
        """With --skin-effects: a replaced game effect file is taken once; a mod's new effect file is a
        new file (taken once, not again as an effect: one entry in files, one size entry); a link table
        made for another game version is skipped with a line, never taken as a new file."""
        mod_src = self.make_model_mod()
        plain = zstandard.ZstdCompressor()
        replaced = "Effect/Weapon_Sword_Master.Test.vsetb.byml.zs"
        new = "Effect/Mod_Own_Glow.Test.vsetb.byml.zs"
        table = "ELink2/elink2.Product.999.belnk.zs"
        other = "ELink2/elink2.Product.998.belnk.zs"
        self.write(replaced, plain.compress(b"VFXB game glow" + bytes(50)))
        self.write(table, plain.compress(b"ELNK game table" + bytes(49)))
        for rel, body in ((replaced, b"VFXB red"), (new, b"VFXB own"), (table, b"ELNK mod"), (other, b"ELNK old")):
            path = os.path.join(mod_src, "romfs", *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(plain.compress(body + bytes(56)))
        said = []
        mod = os.path.join(self.dir, "mod_effects_kinds")
        [entry] = build.build(self.romfs, mod, log=said.append, skins=[("Mod", mod_src)], look_slots=0,
                              skin_effects=True)["skins"]
        for rel in (replaced, new, table):
            self.assertEqual(entry["files"].count(rel), 1, rel)
            self.assertTrue(os.path.exists(os.path.join(mod, "romfs", *rel.split("/"))), rel)
        self.assertNotIn(other, entry["files"])
        self.assertFalse(os.path.exists(os.path.join(mod, "romfs", *other.split("/"))))
        self.assertTrue(any(other in line and "left out" in line for line in said), said)
        # Without the switch the new effect resource still comes (a new file), the game's copies do not.
        [off] = build.build(self.romfs, os.path.join(self.dir, "mod_effects_off"), log=lambda _l: None,
                            skins=[("Mod", mod_src)], look_slots=0)["skins"]
        self.assertEqual([f for f in off["files"] if f.startswith(("Effect/", "ELink2/"))], [new])

    def make_recolour_sword(self, mod_src, game_model=True):
        """The game's Master Sword on its own model, the dark sword's model in the game (or not),
        and the mod's sword pack naming the dark model with no model file of its own."""
        plain = zstandard.ZstdCompressor()
        sword, dark = "Weapon_Sword_Master", "Weapon_Sword_Master_Dark"
        info = "Component/ModelInfo/Master.engine__component__ModelInfo.bgyml"

        def sword_pack(model):
            return plain.compress(sarc.write({
                f"Actor/{sword}.engine__actor__ActorParam.bgyml": byml.dumps({"Components": {"ModelInfoRef": "?" + info}}),
                info: byml.dumps({"FmdbName": model, "ModelProjectName": model})}))
        self.write(f"Pack/Actor/{sword}.pack.zs", sword_pack(sword))
        if game_model:
            self.write(f"Model/{dark}.{dark}.bfres.zs", plain.compress(b"FRES game dark sword"))
        d = zs.Dicts.from_romfs(self.romfs)
        ai = byml.loads(d.decompress(build.read(os.path.join(self.romfs, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))
        ai.append({"__RowId": sword, "ActorName": sword, "FmdbName": sword, "ModelProjectName": sword})
        self.write("RSDB/ActorInfo.Product.999.rstbl.byml.zs", plain.compress(byml.dumps(ai)))
        self.write("RSDB/GameActorInfo.Product.999.rstbl.byml.zs",
                   plain.compress(byml.dumps([{"__RowId": r["__RowId"]} for r in ai])))
        with open(os.path.join(mod_src, "romfs", "Pack", "Actor", f"{sword}.pack.zs"), "wb") as f:
            f.write(sword_pack(dark))
        return sword, dark, info

    def test_build_skin_gear_on_a_model_the_game_has(self):
        """A mod that points an item at another model the game already has (a recolour mod: the Master
        Sword on the dark sword's model) brings no model file of its own; the skin still gets a copy of
        the item naming the game's model, the game's item stays the game's, and the wearer's own Link
        is not given the mod's item pack (it would replace the game's item for every Link there)."""
        mod_src = self.make_model_mod()
        sword, dark, info = self.make_recolour_sword(mod_src)
        mod = os.path.join(self.dir, "mod_skin_gear")
        m = build.build(self.romfs, mod, log=lambda _l: None, skins=[("Mod", mod_src)], look_slots=0)
        d = zs.Dicts.from_romfs(self.romfs)
        out = os.path.join(mod, "romfs")
        [entry] = m["skins"]
        self.assertEqual(entry["gear"], {"Armor_Adult_Goron": "Armor_Adult_Goron_Skin_Mod",
                                         sword: f"{sword}_Skin_Mod"})
        g = sarc.read(d.decompress(build.read(os.path.join(out, f"Pack/Actor/{sword}_Skin_Mod.pack.zs"))))
        self.assertEqual(byml.loads(g[f"Component/ModelInfo/{sword}_Skin_Mod.engine__component__ModelInfo.bgyml"]),
                         {"FmdbName": dark, "ModelProjectName": dark})
        self.assertFalse(os.path.exists(os.path.join(out, f"Model/{dark}.{dark}.bfres.zs")))  # the game's model
        self.assertFalse(os.path.exists(os.path.join(out, f"Pack/Actor/{sword}.pack.zs")))   # the game's item stays
        self.assertEqual(list(entry["self_files"]), ["Pack/Actor/Player_Adult.pack.zs"])     # the player pack only
        self.assertFalse(os.path.exists(os.path.join(mod, f"coop_skin_self/Mod/{sword}.pack.zs")))
        # A model neither the mod nor the game has: still no copy (the Kokiri sword of make_model_mod).
        self.assertNotIn("Weapon_Sword_Kokiri", entry["gear"])

    def test_build_skin_gear_asks_the_game_not_the_folder(self):
        """From a .nup the romfs folder holds only some of the game's files: the game is asked through
        game_has and game_read (the reader), not the folder. And an item whose model is nowhere is
        left out with a line."""
        mod_src = self.make_model_mod()
        sword, dark, _info = self.make_recolour_sword(mod_src, game_model=False)  # not in the folder
        said = []
        mod = os.path.join(self.dir, "mod_skin_reader")

        def has(path):
            return path == f"Model/{dark}.{dark}.bfres.zs" or os.path.isfile(os.path.join(self.romfs, path))
        m = build.build(self.romfs, mod, log=said.append, skins=[("Mod", mod_src)], look_slots=0,
                        game_has=has, game_read=lambda path: build.read(os.path.join(self.romfs, path)))
        self.assertIn(sword, m["skins"][0]["gear"])
        # The folder alone (no reader): the model is nowhere, so the sword is left out, said in the log.
        said = []
        m = build.build(self.romfs, os.path.join(self.dir, "mod_skin_folder"), log=said.append,
                        skins=[("Mod", mod_src)], look_slots=0)
        self.assertNotIn(sword, m["skins"][0]["gear"])
        self.assertTrue(any(f"{sword} left out" in line and dark in line for line in said), said)

    def test_build_skin(self):
        from puppet_pack import skin
        mod_src = self.make_model_mod()
        mod = os.path.join(self.dir, "mod_skin")
        said = []
        m = build.build(self.romfs, mod, log=said.append, skins=[("Mod", mod_src)], look_slots=0)
        d = zs.Dicts.from_romfs(self.romfs)
        out = os.path.join(mod, "romfs")
        [entry] = m["skins"]
        # Only the age the mod changes; the child is the game's own, left out.
        self.assertEqual(entry["variants"], ["Adult"])
        self.assertEqual(entry["models"], {"Adult": "Npc_Mod_Adult"})
        self.assertEqual(entry["actors"], {"Adult": "CoopPuppet_Adult_Skin_Mod"})
        self.assertEqual(entry["as"], {"Adult": "AS/CoopSkin_Mod_Adult.root.asb"})
        self.assertEqual(len(entry["sha256"]), 64)
        self.assertTrue(any("skin Mod: Adult" in line and "Child unchanged" in line for line in said))
        # The mod's new files come along; what it replaces in place and its text do not.
        self.assertEqual(sorted(entry["files"]), ["AS/CoopSkin_Mod_Adult.root.asb.zs",
                                                  "Model/Mod__Adult_01_Animation.Skeleton.bfres.zs",
                                                  "Model/Npc_Mod_Adult.Player_Mod_Adult.bfres.zs",
                                                  "Model/Npc_Mod_Adult.Player_Mod_Adult_Goron.bfres.zs",
                                                  "Pack/Actor/Armor_Adult_Goron_Skin_Mod.pack.zs"])
        # Gear: only the piece with a model of the mod's own, as a copy of its own; the game's stays.
        self.assertEqual(entry["gear"], {"Armor_Adult_Goron": "Armor_Adult_Goron_Skin_Mod"})
        self.assertFalse(os.path.exists(os.path.join(out, "Pack/Actor/Armor_Adult_Goron.pack.zs")))
        g = sarc.read(d.decompress(build.read(os.path.join(out, "Pack/Actor/Armor_Adult_Goron_Skin_Mod.pack.zs"))))
        g_comps = byml.loads(g["Actor/Armor_Adult_Goron_Skin_Mod.engine__actor__ActorParam.bgyml"])["Components"]
        g_info = "Component/ModelInfo/Armor_Adult_Goron_Skin_Mod.engine__component__ModelInfo.bgyml"
        self.assertEqual(g_comps["ModelInfoRef"], "?" + g_info)
        self.assertEqual(byml.loads(g[g_info]), {"FmdbName": "Player_Mod_Adult_Goron", "ModelProjectName": "Npc_Mod_Adult"})
        self.assertIn("Component/ArmorParam/G.bgyml", g)
        self.assertFalse(os.path.exists(os.path.join(out, "Model/Player_Adult.Player_Adult.bfres.zs")))
        self.assertFalse(os.path.exists(os.path.join(out, "AS/Player_Adult.root.asb.zs")))
        self.assertFalse(os.path.exists(os.path.join(out, "Mals")))
        self.assertEqual(d.decompress(build.read(os.path.join(out, "AS/CoopSkin_Mod_Adult.root.asb.zs"))), b"ASB mod root")
        # The skin puppet: the mod's model and animations, its own AS copy.
        files = sarc.read(d.decompress(build.read(os.path.join(out, "Pack/Actor/CoopPuppet_Adult_Skin_Mod.pack.zs"))))
        comps = byml.loads(files["Actor/CoopPuppet_Adult_Skin_Mod.engine__actor__ActorParam.bgyml"])["Components"]
        self.assertEqual(comps["ASRef"], "?AS/CoopSkin_Mod_Adult.root.asb")
        self.assertEqual(comps["ModelInfoRef"], "?Component/ModelInfo/Player_Mod_Adult.engine__component__ModelInfo.bgyml")
        self.assertIn("Component/AnimationParam/Player_Mod_Adult.engine__component__AnimationParam.bgyml", files)
        # The plain puppet keeps the game's AS and model.
        plain = sarc.read(d.decompress(build.read(os.path.join(out, "Pack/Actor/CoopPuppet_Adult.pack.zs"))))
        pc = byml.loads(plain["Actor/CoopPuppet_Adult.engine__actor__ActorParam.bgyml"])["Components"]
        self.assertEqual(pc["ASRef"], "?AS/Player_Adult.root.asb")
        self.assertEqual(pc["ModelInfoRef"], "?Component/ModelInfo/Adult.engine__component__ModelInfo.bgyml")
        # ActorInfo: the skin's row names the mod's model and adds its animations.
        rows = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(out, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        row = rows["CoopPuppet_Adult_Skin_Mod"]
        self.assertEqual(row["ModelProjectName"], "Npc_Mod_Adult")
        self.assertEqual(row["FmdbName"], "Player_Mod_Adult")
        self.assertIn({"ModelProjectName": "Mod__Adult_01_Animation"}, row["AnimationResources"])
        self.assertNotIn("ActorName", row)
        self.assertNotIn("CoopPuppet_Child_Skin_Mod", rows)
        self.assertEqual(rows["CoopPuppet_Adult"]["FmdbName"], "Player_Adult")
        g_row = rows["Armor_Adult_Goron_Skin_Mod"]
        self.assertEqual((g_row["ActorName"], g_row["ModelProjectName"], g_row["FmdbName"], g_row["InstanceHeapSize"]),
                         ("Armor_Adult_Goron_Skin_Mod", "Npc_Mod_Adult", "Player_Mod_Adult_Goron", 1000))
        self.assertEqual(rows["Armor_Adult_Goron"]["ModelProjectName"], "Armor_Adult_Goron")
        self.assertNotIn("Weapon_Sword_Kokiri_Skin_Mod", rows)
        # Size table: the mod's own entries, the bigger AS entry for the copy.
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            out, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        self.assertEqual(t.get("Model/Npc_Mod_Adult.Player_Mod_Adult.bfres"), 777777)
        self.assertEqual(t.get("AS/CoopSkin_Mod_Adult.root.asb"), 3000000)
        self.assertEqual(t.get("AS/Player_Adult.root.asb"), 2000000)  # the game's own stays
        self.assertGreater(t.get("Model/Mod__Adult_01_Animation.Skeleton.bfres") or 0, 0)  # no mod entry: from the file
        self.assertGreater(t.get("Pack/Actor/CoopPuppet_Adult_Skin_Mod.pack") or 0, 0)
        # Your own Link in the skin: the mod's player pack beside the pack (not in romfs/), its AS on the copy.
        self.assertEqual(entry["self_files"], {"Pack/Actor/Player_Adult.pack.zs": "coop_skin_self/Mod/Player_Adult.pack.zs"})
        self.assertFalse(os.path.exists(os.path.join(out, "Pack/Actor/Player_Adult.pack.zs")))
        own = sarc.read(d.decompress(build.read(os.path.join(mod, "coop_skin_self/Mod/Player_Adult.pack.zs"))))
        base = byml.loads(own["Actor/Player_Base.engine__actor__ActorParam.bgyml"])["Components"]
        self.assertEqual(base["ASRef"], "?AS/CoopSkin_Mod_Adult.root.asb")
        top = byml.loads(own["Actor/Player_Adult.engine__actor__ActorParam.bgyml"])["Components"]
        self.assertIn("Player_Mod_Adult", top["ModelInfoRef"])
        self.assertGreater(t.get("Pack/Actor/Player_Adult.pack") or 0, 0)
        actors = {a["actor"]: a for a in m["actors"]}
        self.assertEqual(actors["CoopPuppet_Adult_Skin_Mod"]["skin"], "Mod")
        self.assertIsNone(actors["CoopPuppet_Adult"]["skin"])
        self.assertIsNone(m["test_skin"])
        # The same mod gives the same hash.
        m2 = build.build(self.romfs, os.path.join(self.dir, "mod_skin2"), log=lambda *_: None,
                         skins=[("Mod", mod_src)], look_slots=0)
        self.assertEqual(m2["skins"][0]["sha256"], entry["sha256"])
        self.assertEqual(skin.actor_name("Adult", "Mod"), "CoopPuppet_Adult_Skin_Mod")

    def test_build_skin_quiet_copy(self):
        # With puppets the game's perception sees, each skin puppet has its quiet copy too.
        self.add_perception_and_guards()
        mod_src = self.make_model_mod()
        m = build.build(self.romfs, os.path.join(self.dir, "mod_skin_q"), log=lambda *_: None,
                        skins=[("Mod", mod_src)], look_slots=0, perception=True)
        actors = {a["actor"]: a for a in m["actors"]}
        self.assertTrue(actors["CoopPuppet_AdultQuiet_Skin_Mod"]["quiet"])
        self.assertEqual(actors["CoopPuppet_AdultQuiet_Skin_Mod"]["skin"], "Mod")
        self.assertFalse(actors["CoopPuppet_Adult_Skin_Mod"]["quiet"])

    def test_build_test_skin(self):
        mod_src = self.make_model_mod()
        mod = os.path.join(self.dir, "mod_test_skin")
        m = build.build(self.romfs, mod, log=lambda *_: None, skins=[("Mod", mod_src)], test_skin="mod",
                        tunics=["red"], look_slots=0)
        d = zs.Dicts.from_romfs(self.romfs)
        out = os.path.join(mod, "romfs")
        self.assertEqual(m["test_skin"], "Mod")
        for actor in ("CoopPuppet_Adult", "CoopPuppet_Adult_Red"):
            files = sarc.read(d.decompress(build.read(os.path.join(out, f"Pack/Actor/{actor}.pack.zs"))))
            comps = byml.loads(files[f"Actor/{actor}.engine__actor__ActorParam.bgyml"])["Components"]
            self.assertEqual(comps["ASRef"], "?AS/CoopSkin_Mod_Adult.root.asb", actor)
            self.assertIn("Player_Mod_Adult", comps["ModelInfoRef"], actor)
        # The child: the mod does not change it, so the child puppets stay Link.
        files = sarc.read(d.decompress(build.read(os.path.join(out, "Pack/Actor/CoopPuppet_Child.pack.zs"))))
        comps = byml.loads(files["Actor/CoopPuppet_Child.engine__actor__ActorParam.bgyml"])["Components"]
        self.assertNotIn("Player_Mod", comps["ModelInfoRef"])
        with self.assertRaises(build.BuildError):
            build.build(self.romfs, os.path.join(self.dir, "mod_x"), log=lambda *_: None,
                        skins=[("Mod", mod_src)], test_skin="Other", look_slots=0)

    def test_build_characters(self):
        # A Character Select mod beside the pack (made-up data): a puppet per character and age it has a
        # model file for, the plain puppet with that model project and the list's heap sizes.
        mods = os.path.join(self.dir, "mods_cs")
        cs = os.path.join(mods, "OoT Character Select")
        os.makedirs(os.path.join(cs, "romfs", "Model"))
        with open(os.path.join(cs, "romfs", "Model", "Player_Adult_C005.Player_Adult.bfres.zs"), "wb") as f:
            f.write(b"made up")
        doc = {"format": 1, "mod": "OoT Character Select",
               "actor_heaps": {v: {"InstanceHeapSize": 0x7000000, "PreActorRenderInfoHeapSize": 0x60000}
                               for v in build.VARIANTS},
               "characters": [{"number": 0, "name": "Link"}, {"number": 7, "name": "Made Up", "file": 5},
                              {"number": 8, "name": "No File", "file": 6}]}
        with open(os.path.join(cs, build.CHARACTERS_FILE), "w", encoding="utf-8") as f:
            json.dump(doc, f)
        mod = os.path.join(mods, "pack")
        found = build.find_characters(mod, log=lambda *_: None)
        self.assertEqual(found["files"], {5: ["Adult"]})
        self.assertEqual(found["numbers"], {5: 7})
        self.assertIsNone(found["colours"])
        m = build.build(self.romfs, mod, log=lambda *_: None, tunics=["red"], look_slots=0, characters=found)
        actors = {a["actor"]: a for a in m["actors"]}
        self.assertEqual(actors["CoopPuppet_Adult_C005"]["character"], 5)
        self.assertEqual(actors["CoopPuppet_Adult_C005"]["model"], "Player_Adult_C005")
        self.assertIsNone(actors["CoopPuppet_Adult_C005"]["tunic"])
        self.assertGreaterEqual(actors["CoopPuppet_Adult_C005"]["instance_heap"], 0x7000000)
        self.assertNotIn("CoopPuppet_Child_C005", actors)   # no model file for that age
        self.assertNotIn("CoopPuppet_Adult_C006", actors)   # listed, no file
        self.assertIsNone(actors["CoopPuppet_Adult"]["character"])
        self.assertIsNone(actors["CoopPuppet_Adult_Red"]["character"])
        self.assertEqual(m["characters"]["numbers"], {"5": 7})
        # The puppet is the game's player pack under its own name: nothing of the mod's is copied.
        self.assertFalse(os.path.exists(os.path.join(mod, "romfs", "Model", "Player_Adult_C005.Player_Adult.bfres.zs")))
        # The mod's colour routine for a puppet goes into the manifest as the list names it; one that does not read
        # leaves the characters out (said in the log).
        doc["puppet_colours"] = {"offset": 0x2E1A000, "head": "FD7BBFA9" * 4, "bits": 4}
        with open(os.path.join(cs, build.CHARACTERS_FILE), "w", encoding="utf-8") as f:
            json.dump(doc, f)
        found = build.find_characters(mod, log=lambda *_: None)
        self.assertEqual(found["colours"], {"offset": 0x2E1A000, "head": "fd7bbfa9" * 4})
        m = build.build(self.romfs, os.path.join(mods, "pack3"), log=lambda *_: None, look_slots=0, characters=found)
        self.assertEqual(m["characters"]["colours"], {"offset": 0x2E1A000, "head": "fd7bbfa9" * 4})
        doc["puppet_colours"]["head"] = "xyz"
        with open(os.path.join(cs, build.CHARACTERS_FILE), "w", encoding="utf-8") as f:
            json.dump(doc, f)
        self.assertIsNone(build.find_characters(mod, log=lambda *_: None))
        del doc["puppet_colours"]
        # Nothing of the mod's tables without copies of them in its romfs: the game's rows, its Player rows raised.
        d = zs.Dicts.from_romfs(self.romfs)
        out_ai = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(mod, "romfs", "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        self.assertEqual(build._num(out_ai["Player_Adult"]["InstanceHeapSize"]), 0x7000000)
        self.assertEqual(build._num(out_ai["Player_Child"]["PreActorRenderInfoHeapSize"]), 0x60000)
        self.assertEqual(out_ai["Player_Adult"]["FmdbName"], "Player_Adult")   # the rest of the row as the game's
        self.assertEqual(out_ai["Animal"], {"__RowId": "Animal", "CalcRadius": 1.0})
        # Another mod's list, or another format, is not ours.
        doc["mod"] = "Something Else"
        with open(os.path.join(cs, build.CHARACTERS_FILE), "w", encoding="utf-8") as f:
            json.dump(doc, f)
        self.assertIsNone(build.find_characters(mod, log=lambda *_: None))
        m = build.build(self.romfs, os.path.join(mods, "pack2"), log=lambda *_: None, look_slots=0)
        self.assertIsNone(m["characters"])

    def test_build_characters_keeps_the_mods_tables(self):
        # Review of #299 (2, 3): the pack's ActorInfo, GameActorInfo and size table replace the mod's own copies, so
        # the mod's rows and entries go into the pack's (solo too: the player's own Link keeps the mod's heaps).
        plain = zstandard.ZstdCompressor()
        mods = os.path.join(self.dir, "mods_cs2")
        cs = os.path.join(mods, "OoT Character Select")
        romfs = os.path.join(cs, "romfs")
        os.makedirs(os.path.join(romfs, "Model"))
        os.makedirs(os.path.join(romfs, "RSDB"))
        os.makedirs(os.path.join(romfs, "System", "Resource"))
        for name, data in (("Player_Adult_C005.Player_Adult.bfres", b"x" * 300),
                           ("Player_Child_C009.Player_Child.bfres", b"y" * 5000)):
            with open(os.path.join(romfs, "Model", name + ".zs"), "wb") as f:
                f.write(plain.compress(data))
        doc = {"format": 1, "mod": "OoT Character Select",
               "actor_heaps": {"Adult": {"InstanceHeapSize": 0x1000000, "PreActorRenderInfoHeapSize": 0x60000},
                               "Child": {"InstanceHeapSize": 0x100, "PreActorRenderInfoHeapSize": 0x60000}},
               "characters": [{"number": 7, "file": 5}, {"number": 20, "file": 9}]}
        with open(os.path.join(cs, build.CHARACTERS_FILE), "w", encoding="utf-8") as f:
            json.dump(doc, f)
        d = zs.Dicts.from_romfs(self.romfs)
        ai = byml.loads(d.decompress(build.read(os.path.join(self.romfs, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))
        mod_ai = [dict(r) for r in ai]
        for r in mod_ai:
            if r["__RowId"] == "Player_Adult":
                r["InstanceHeapSize"] = 0x2000000      # larger than characters.json's: kept
            if r["__RowId"] == "Player_Base":
                r["InstanceHeapSize"] = 7000000        # any row the mod changed
        mod_ai.append({"__RowId": "Cs_MadeUp", "InstanceHeapSize": 5})   # a row the mod adds
        with open(os.path.join(romfs, "RSDB", "ActorInfo.Product.999.rstbl.byml.zs"), "wb") as f:
            f.write(plain.compress(byml.dumps(mod_ai)))
        t = rstb.Rstb()
        t.set("Model/Player_Adult_C005.Player_Adult.bfres", 123456)   # the mod's own entry for its model
        t.set("Layout/MadeUp.bflyt", 999)                             # and for any file of its own
        t.set("Actor/Player_Base.engine__actor__ActorParam.bgyml", 5)  # smaller than the game's: the game's stays
        with open(os.path.join(romfs, "System", "Resource", "ResourceSizeTable.Product.999.Test.rsizetable.zs"), "wb") as f:
            f.write(plain.compress(t.dumps()))
        mod = os.path.join(mods, "pack")
        found = build.find_characters(mod, log=lambda *_: None)
        self.assertEqual(found["numbers"], {5: 7, 9: 20})
        said = []
        m = build.build(self.romfs, mod, log=said.append, look_slots=0, characters=found)
        self.assertEqual(m["characters"]["numbers"], {"5": 7, "9": 20})
        self.assertIn("CoopPuppet_Child_C009", {a["actor"] for a in m["actors"]})
        out_ai = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(mod, "romfs", "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        self.assertEqual(build._num(out_ai["Player_Adult"]["InstanceHeapSize"]), 0x2000000)
        self.assertEqual(build._num(out_ai["Player_Child"]["InstanceHeapSize"]), 5692448)   # the game's was larger
        self.assertEqual(build._num(out_ai["Player_Child"]["PreActorRenderInfoHeapSize"]), 0x60000)
        self.assertEqual(build._num(out_ai["Player_Base"]["InstanceHeapSize"]), 7000000)
        self.assertEqual(out_ai["Cs_MadeUp"], {"__RowId": "Cs_MadeUp", "InstanceHeapSize": 5})
        self.assertIn("CoopPuppet_Adult_C005", out_ai)                 # the pack's own rows too
        self.assertIn("CoopPuppet_Adult", out_ai)
        table = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            mod, "romfs", "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        self.assertEqual(table.get("Model/Player_Adult_C005.Player_Adult.bfres"), 123456)
        self.assertEqual(table.get("Model/Player_Child_C009.Player_Child.bfres"), 5024 + build.PACK_SIZE_MARGIN)
        self.assertEqual(table.get("Layout/MadeUp.bflyt"), 999)
        self.assertEqual(table.get("Actor/Player_Base.engine__actor__ActorParam.bgyml"), 13336)
        self.assertTrue(any("size-table entries from OoT Character Select" in line for line in said), said)
        # A size table for another game version: only the model files are sized (from the files).
        os.rename(os.path.join(romfs, "System", "Resource", "ResourceSizeTable.Product.999.Test.rsizetable.zs"),
                  os.path.join(romfs, "System", "Resource", "ResourceSizeTable.Product.998.Test.rsizetable.zs"))
        mod2 = os.path.join(mods, "pack2")
        build.build(self.romfs, mod2, log=lambda *_: None, look_slots=0, characters=build.find_characters(mod2))
        table = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            mod2, "romfs", "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        self.assertEqual(table.get("Model/Player_Adult_C005.Player_Adult.bfres"), 320 + build.PACK_SIZE_MARGIN)
        self.assertIsNone(table.get("Layout/MadeUp.bflyt"))
        # Without the mod (--no-characters, or none installed): the game's Player rows, as before.
        mod3 = os.path.join(self.dir, "plain_pack")
        build.build(self.romfs, mod3, log=lambda *_: None, look_slots=0)
        out_ai = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(mod3, "romfs", "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        self.assertEqual(build._num(out_ai["Player_Adult"]["InstanceHeapSize"]), 10245064)
        self.assertNotIn("PreActorRenderInfoHeapSize", out_ai["Player_Child"])
        self.assertNotIn("Cs_MadeUp", out_ai)

    def test_skin_refusals(self):
        cases = [
            (("Mod", self.make_model_mod("with_code", exefs=True)), "exefs"),
            (("Mod", self.make_model_mod("unchanged", change=False)), "does not change the player's model"),
            (("Bad-Id", self.make_model_mod("bad_id")), "skin id"),
            (("Mod", os.path.join(self.dir, "nothing_here")), "no romfs folder"),
        ]
        # A skin that cannot be built is left out with a line; the pack is built without it.
        for i, (sk, words) in enumerate(cases):
            said = []
            m = build.build(self.romfs, os.path.join(self.dir, f"mod_r{i}"), log=said.append,
                            skins=[sk], look_slots=0)
            self.assertEqual(m["skins"], [])
            self.assertTrue(any(f"skin {sk[0]}: left out" in s and words in s for s in said), said)
        good = self.make_model_mod("twice")
        said = []
        m = build.build(self.romfs, os.path.join(self.dir, "mod_r9"), log=said.append,
                        skins=[("Mod", good), ("mod", good)], look_slots=0)
        self.assertEqual([e["id"] for e in m["skins"]], ["Mod"])
        self.assertTrue(any("skin mod: left out" in s for s in said), said)

    def test_broken_skin_costs_only_itself(self):
        # One broken model (here a size table that does not decode) is left out; the good one is built
        # whole, and nothing of the broken one is in the pack.
        good = self.make_model_mod("good")
        bad = self.make_model_mod("bad")
        table = os.path.join(bad, "romfs", "System", "Resource")
        os.makedirs(table, exist_ok=True)
        with open(os.path.join(table, "ResourceSizeTable.Product.999.Test.rsizetable.zs"), "wb") as f:
            f.write(b"not a table")
        mod = os.path.join(self.dir, "mod_broken")
        said = []
        m = build.build(self.romfs, mod, log=said.append, skins=[("Bad", bad), ("Good", good)], look_slots=0)
        self.assertEqual([e["id"] for e in m["skins"]], ["Good"])
        self.assertTrue(any("skin Bad: left out" in s and "size table" in s for s in said), said)
        self.assertFalse(os.path.exists(os.path.join(mod, "romfs", "AS", "CoopSkin_Bad_Adult.root.asb.zs")))
        # --test-skin naming the skin that was left out says so (not "no --skin with that id").
        with self.assertRaisesRegex(build.BuildError, "left out"):
            build.build(self.romfs, os.path.join(self.dir, "mod_broken2"), log=lambda *_: None,
                        skins=[("Bad", bad), ("Good", good)], test_skin="bad", look_slots=0)

    def test_reader_has_is_exact(self):
        # "Does the game have this file": an exact name, never a glob pattern. Through the real readers the
        # build is given (a stand-in reader once hid that the .nup one has no listdir, and every skin was left out).
        from puppet_pack import hud

        class Nup:
            def listdir(self, d):
                if d not in ("Model",):
                    raise KeyError(d)
                return {"A[1].bfres.zs": ("f", 0), "Sub": ("d", 1)}
        root = os.path.join(self.dir, "reader_romfs")
        os.makedirs(os.path.join(root, "Model", "Sub"))
        with open(os.path.join(root, "Model", "A[1].bfres.zs"), "wb") as f:
            f.write(b"x")
        for r in (hud.NupReader(Nup()), hud.DirReader(root)):
            self.assertTrue(build._reader_has(r, "Model/A[1].bfres.zs"), r)
            self.assertFalse(build._reader_has(r, "Model/A*.bfres.zs"), r)
            self.assertFalse(build._reader_has(r, "Model/Sub"), r)
            self.assertFalse(build._reader_has(r, "Nope/A.bfres.zs"), r)

    def test_main_builds_a_skin(self):
        # Review of #276: from the command line every --skin was "left out: AttributeError: 'DirReader'
        # object has no attribute 'listdir'" (the tests called build.build directly). main, as players run it.
        mod_src = self.make_model_mod()
        mod = os.path.join(self.dir, "mod_main_skin")
        self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", mod, "--any-version",
                                     "--skin", f"Mod={mod_src}"]), 0)
        with open(os.path.join(mod, "coop_puppet.json"), encoding="utf-8") as f:
            manifest = json.load(f)
        self.assertEqual([s["id"] for s in manifest["skins"]], ["Mod"])
        self.assertTrue(os.path.exists(os.path.join(mod, "romfs", "Pack", "Actor", "CoopPuppet_Adult_Skin_Mod.pack.zs")))

    def test_skin_table_and_files(self):
        from puppet_pack import skin
        # The size table named like the game's, else the newest version's.
        game = "System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs"
        older = "System/Resource/ResourceSizeTable.Product.110.Nin_Ounce_NVN2.rsizetable.zs"
        newer = "System/Resource/ResourceSizeTable.Product.120.Nin_Ounce_NVN2.rsizetable.zs"
        self.assertEqual(skin.pick_table([older, game, newer], game), game)
        self.assertEqual(skin.pick_table([older, newer], game), newer)
        self.assertEqual(skin.pick_table([older, newer]), newer)
        self.assertIsNone(skin.pick_table([]))
        # A file the game has is a replacement even when its size table does not list it.
        sk = skin.Skin("S", self.dir, {}, None, ["Model/New.bfres.zs", "Model/Replaced.bfres.zs"])

        class NoTable:
            def get(self, _):
                return None
        have = {"Model/Replaced.bfres.zs"}
        self.assertEqual(skin.new_files(sk, NoTable(), game_has=lambda p: p in have), ["Model/New.bfres.zs"])
        # Symlinks in a mod folder are skipped.
        root = os.path.join(self.dir, "links")
        os.makedirs(os.path.join(root, "Model"))
        with open(os.path.join(root, "Model", "a.bfres.zs"), "wb") as f:
            f.write(b"x")
        try:
            os.symlink(os.path.join(root, "Model", "a.bfres.zs"), os.path.join(root, "Model", "b.bfres.zs"))
        except (OSError, NotImplementedError):
            self.skipTest("this system makes no symlinks")
        self.assertEqual(skin._walk(root), ["Model/a.bfres.zs"])

    def test_two_versions_of_one_mod(self):
        # Outfit variants of one mod name their models the same: identical files are shared, and the skin
        # with a model file of other contents under the same path gets its model project under its own name.
        a = self.make_model_mod("version_a")
        b = self.make_model_mod("version_b")
        m = build.build(self.romfs, os.path.join(self.dir, "mod_v1"), log=lambda *_: None,
                        skins=[("A", a), ("B", b)], look_slots=0)
        self.assertEqual([e["id"] for e in m["skins"]], ["A", "B"])
        self.assertIn("Model/Npc_Mod_Adult.Player_Mod_Adult.bfres.zs", m["skins"][1]["files"])
        with open(os.path.join(b, "romfs", "Model", "Npc_Mod_Adult.Player_Mod_Adult.bfres.zs"), "wb") as f:
            f.write(zstandard.ZstdCompressor().compress(b"FRES another outfit"))
        # Both add a model under the game's own Player_Adult project, of other contents: that project is not
        # renamed (it names the game's own files too); B uses A's copy.
        for mod_dir, body in ((a, b"FRES npc link a"), (b, b"FRES npc link b")):
            with open(os.path.join(mod_dir, "romfs", "Model", "Player_Adult.Npc_Link_Adult.bfres.zs"), "wb") as f:
                f.write(zstandard.ZstdCompressor().compress(body))
        said = []
        m = build.build(self.romfs, os.path.join(self.dir, "mod_v2"), log=said.append,
                        skins=[("A", a), ("B", b)], look_slots=0)
        # Both are built: B's model project gets a name of its own, and every name of it in B's packs follows.
        self.assertEqual([e["id"] for e in m["skins"]], ["A", "B"])
        self.assertEqual(m["skins"][1]["renamed"], {"Npc_Mod_Adult": "Npc_Mod_Adult_B"})
        self.assertEqual(m["skins"][0]["renamed"], {})
        self.assertIn("Model/Npc_Mod_Adult_B.Player_Mod_Adult.bfres.zs", m["skins"][1]["files"])
        self.assertIn("Model/Npc_Mod_Adult.Player_Mod_Adult.bfres.zs", m["skins"][0]["files"])
        self.assertEqual(m["skins"][1]["models"], {"Adult": "Npc_Mod_Adult_B"})
        self.assertTrue(any("skin B: model project Npc_Mod_Adult -> Npc_Mod_Adult_B" in s for s in said), said)
        self.assertTrue(any("skin B: Model/Player_Adult.Npc_Link_Adult.bfres.zs differ" in s for s in said), said)
        self.assertIn("Model/Player_Adult.Npc_Link_Adult.bfres.zs", m["skins"][0]["files"])
        self.assertNotIn("Model/Player_Adult.Npc_Link_Adult.bfres.zs", m["skins"][1]["files"])
        out = os.path.join(self.dir, "mod_v2", "romfs")
        d = zs.Dicts.from_romfs(self.romfs)
        self.assertEqual(d.decompress(build.read(os.path.join(out, "Model/Npc_Mod_Adult_B.Player_Mod_Adult.bfres.zs"))),
                         b"FRES another outfit")
        rows = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(out, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        self.assertEqual(rows["CoopPuppet_Adult_Skin_B"]["ModelProjectName"], "Npc_Mod_Adult_B")
        self.assertEqual(rows["CoopPuppet_Adult_Skin_A"]["ModelProjectName"], "Npc_Mod_Adult")
        self.assertEqual(rows["Armor_Adult_Goron_Skin_B"]["ModelProjectName"], "Npc_Mod_Adult_B")
        g = sarc.read(d.decompress(build.read(os.path.join(out, "Pack/Actor/Armor_Adult_Goron_Skin_B.pack.zs"))))
        info = byml.loads(g["Component/ModelInfo/Armor_Adult_Goron_Skin_B.engine__component__ModelInfo.bgyml"])
        self.assertEqual(info["ModelProjectName"], "Npc_Mod_Adult_B")

    def test_skin_cli(self):
        mod_src = self.make_model_mod()
        quiet = io.StringIO()
        with contextlib.redirect_stdout(quiet), contextlib.redirect_stderr(quiet):
            self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", os.path.join(self.dir, "cli"),
                                         "--any-version", "--no-hud", "--skin", "noequals"]), 2)

    def test_not_a_romfs(self):
        with self.assertRaises(build.BuildError):
            build.build(self.dir, os.path.join(self.dir, "mod"), log=lambda *_: None)


class LoadRootTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.user = os.path.join(self.dir, "user")
        os.makedirs(os.path.join(self.user, "config"))

    def tearDown(self):
        shutil.rmtree(self.dir)

    def write_ini(self, load_dir):
        with open(os.path.join(self.user, "config", "qt-config.ini"), "w", encoding="utf-8") as f:
            f.write("[Data%20Storage]\nload_directory\\default=false\n")
            f.write(f"load_directory={load_dir}\n")

    def test_no_config(self):
        self.assertEqual(build.load_root(self.user), os.path.join(self.user, "load"))

    def test_configured_elsewhere(self):
        other = os.path.join(self.dir, "other", "load")
        os.makedirs(other)
        self.write_ini(other.replace("\\", "/"))
        self.assertEqual(os.path.normcase(build.load_root(self.user)), os.path.normcase(other))

    def test_configured_missing_folder(self):
        self.write_ini(os.path.join(self.dir, "gone").replace("\\", "/"))
        self.assertEqual(build.load_root(self.user), os.path.join(self.user, "load"))


@unittest.skipIf(zstandard is None, "needs the zstandard package")
class NupTest(unittest.TestCase):
    """A tiny made-up .nup: base plus update, raw and zstd files."""

    TID = int(dumpcheck.TITLE_ID, 16)

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "game.nup")
        self.base = {"Pack/ZsDic.pack.zs": (b"dict v1", nup.CODEC_RAW),
                     "Pack/Actor/Player_Child.pack.zs": (b"child v1" * 100, nup.CODEC_ZSTD),
                     "RSDB/ActorInfo.Product.100.rstbl.byml.zs": (b"old", nup.CODEC_RAW),
                     "Model/Base.bin": (b"", nup.CODEC_RAW)}
        self.update = {"Pack/Actor/Player_Child.pack.zs": (b"child v2" * 100, nup.CODEC_ZSTD),
                       "RSDB/ActorInfo.Product.111.rstbl.byml.zs": (b"new", nup.CODEC_ZSTD)}
        nup.write_nup(self.path, [(self.TID, 0, nup.KIND_BASE, self.base),
                                  (self.TID | 0x800, 0x10000, nup.KIND_UPDATE, {}),
                                  (self.TID | 0x800, 0x20000, nup.KIND_UPDATE, self.update)])

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_merge_update_wins(self):
        self.assertTrue(nup.is_nup(self.path))
        with nup.Nup(self.path) as n:
            self.assertEqual(n.best_update()["version"], 0x20000)
            self.assertEqual(n.read("Pack/Actor/Player_Child.pack.zs"), b"child v2" * 100)
            self.assertEqual(n.read("Pack/ZsDic.pack.zs"), b"dict v1")
            self.assertEqual(n.read("Model/Base.bin"), b"")
            self.assertEqual(n.glob("RSDB/ActorInfo.Product.*.rstbl.byml.zs"),
                             ["RSDB/ActorInfo.Product.100.rstbl.byml.zs",
                              "RSDB/ActorInfo.Product.111.rstbl.byml.zs"])
            self.assertEqual(dumpcheck.nup_files(n)["RSDB/ActorInfo.Product.*.rstbl.byml.zs"],
                             "RSDB/ActorInfo.Product.111.rstbl.byml.zs")
            with self.assertRaises(nup.NupError):
                n.read("Pack/Nope.zs")

    def test_nup_files_has_the_stand_in_sources(self):
        # The pack built from a .nup (before this fix) had "shots": []: only REQUIRED was extracted, so
        # every STAND_INS source was "left out". They are read from the .nup when it has them.
        self.assertEqual({f"Pack/Actor/{src}.pack.zs" for src, _ in build.STAND_INS} |
                         {f"Pack/Actor/{build.IMPACT_SOURCE}.pack.zs"} |
                         {"Tex/Angin_Tile_A_Alb.bntx.zs", "Tex/Angin_Tile_A_Dark_Alb.bntx.zs",
                          "Tex/Mt_Vest_Player_Adult_Alb.bntx.zs", "Tex/Mt_Angin_Acs_Player_Adult_Alb.bntx.zs",
                          "Tex/Mt_Cloth_Parts_Player_Child_Alb.bntx.zs", "Tex/Mt_Vest_Player_Child_Alb.bntx.zs",
                          "Tex/Mt_PantsParts_Player_Child_Alb.bntx.zs", "Tex/Mt_Boots_Player_Adult_Alb.bntx.zs",
                          "Tex/Mt_Leather_Player_Child_Alb.bntx.zs"} |
                         {f"Model/Player_{v}.Player_{v}.bfres.zs" for v in build.VARIANTS},
                         set(dumpcheck.OPTIONAL))
        seed = "Pack/Actor/Shootable_DekuSeed.pack.zs"
        nup.write_nup(self.path, [(self.TID, 0, nup.KIND_BASE, dict(self.base, **{seed: (b"seed", nup.CODEC_RAW)}))])
        with nup.Nup(self.path) as n:
            files = dumpcheck.nup_files(n)
        self.assertEqual(files[seed], seed)
        self.assertNotIn("Pack/Actor/NormalArrow.pack.zs", files)  # optional: not there, no error

    def test_standalone_update(self):
        nup.write_nup(self.path, [(self.TID, 0x20000, nup.KIND_UPDATE, self.update)])
        with nup.Nup(self.path) as n:
            self.assertEqual(n.read("RSDB/ActorInfo.Product.111.rstbl.byml.zs"), b"new")

    def test_damaged_file_and_cut_short(self):
        data = bytearray(build.read(self.path))
        with nup.Nup(self.path) as n:
            _name, _size, eoff, _esize, _sha, _codec = n.files[n.listdir("Pack")["ZsDic.pack.zs"][1]]
        data[eoff] ^= 0xFF
        with open(self.path, "wb") as f:
            f.write(data)
        with nup.Nup(self.path) as n:
            with self.assertRaises(nup.NupError):
                n.read("Pack/ZsDic.pack.zs")
        with open(self.path, "wb") as f:
            f.write(data[:len(data) // 2])
        with self.assertRaises(nup.NupError):
            nup.Nup(self.path)
        self.assertEqual([p.code for p in dumpcheck.check_nup(self.path)], ["nup-bad"])

    def patch_toc(self, change):
        """Changes the TOC bytes in place and fixes the TOC hash, as a crafted file would."""
        data = bytearray(build.read(self.path))
        sup = nup.SUPER.unpack_from(data, 0)
        toc_off, toc_size = sup[3], sup[4]
        toc = bytearray(data[toc_off:toc_off + toc_size])
        change(toc)
        data[toc_off:toc_off + toc_size] = toc
        data[24:56] = hashlib.sha256(bytes(toc)).digest()
        with open(self.path, "wb") as f:
            f.write(data)

    def test_unsafe_names_refused(self):
        for bad in ("..", ".", "..\\..\\evil.zs", "a\\b", "C:evil", "a\0b"):
            nup.write_nup(self.path, [(self.TID, 0, nup.KIND_BASE, {f"Pack/{bad}": (b"x", nup.CODEC_RAW)})])
            with self.assertRaises(nup.NupError, msg=repr(bad)):
                nup.Nup(self.path)
        nup.write_nup(self.path, [(self.TID, 0, nup.KIND_BASE, {"Pack//x": (b"x", nup.CODEC_RAW)})])
        with self.assertRaises(nup.NupError):
            nup.Nup(self.path)

    def test_write_under_stays_inside(self):
        root = os.path.join(self.dir, "out")
        os.makedirs(root)
        for rel in ("../evil", "a/../../evil", "..\\evil", "C:/evil", "/evil", "a//b", ""):
            with self.assertRaises(nup.NupError, msg=repr(rel)):
                nup.write_under(root, rel, b"x")
        self.assertFalse(os.path.exists(os.path.join(self.dir, "evil")))
        dst = nup.write_under(root, "Pack/ok.zs", b"ok")
        self.assertEqual(build.read(dst), b"ok")
        self.assertTrue(os.path.realpath(dst).startswith(os.path.realpath(root) + os.sep))

    def test_bad_child_index(self):
        def change(toc):
            pool_off = nup.TOC.unpack_from(toc, 0)[4]
            struct.pack_into("<I", toc, pool_off - 4, 0xFFFF)
        self.patch_toc(change)
        with self.assertRaises(nup.NupError):
            nup.Nup(self.path)

    def test_zstd_larger_than_stored_size(self):
        # The child pack's stored size says 10 bytes; the frame holds 800.
        with nup.Nup(self.path) as n:
            index = n.listdir("Pack/Actor")["Player_Child.pack.zs"][1]

        def change(toc):
            _m, npk, ndirs, _nf, _po, _ps, _r = nup.TOC.unpack_from(toc, 0)
            at = nup.TOC.size + npk * nup.PACKAGE.size + ndirs * nup.DIR.size + index * nup.FILE.size + 8
            struct.pack_into("<Q", toc, at, 10)
        self.patch_toc(change)
        with nup.Nup(self.path) as n:
            with self.assertRaises(nup.NupError):
                n.read("Pack/Actor/Player_Child.pack.zs")

    def test_check_nup(self):
        # Missing most of what the pack needs.
        self.assertEqual([p.code for p in dumpcheck.check_nup(self.path)], ["nup-partial"])
        full = {pat.replace("*", "111"): (dumpcheck.ZSTD_MAGIC, nup.CODEC_RAW) for pat in dumpcheck.REQUIRED}
        nup.write_nup(self.path, [(self.TID | 0x800, 1, nup.KIND_UPDATE, full)])
        self.assertEqual(dumpcheck.check_nup(self.path), [])
        nup.write_nup(self.path, [(0x0100000000010000, 1, nup.KIND_BASE, full)])
        self.assertEqual([p.code for p in dumpcheck.check_nup(self.path)], ["nup-other"])


class DumpCheckTest(unittest.TestCase):
    """What the emulator's Dump RomFS can leave behind, and what the check says."""

    FILES = (
        "Pack/ZsDic.pack.zs",
        "Pack/Actor/Player_Child.pack.zs",
        "Pack/Actor/Player_Adult.pack.zs",
        "RSDB/ActorInfo.Product.111.rstbl.byml.zs",
        "RSDB/GameActorInfo.Product.111.rstbl.byml.zs",
        "System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs",
        "Model/Some.bfres.zs",
    )

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.user = os.path.join(self.dir, "user")
        self.romfs = os.path.join(self.user, "dump", dumpcheck.TITLE_ID, "romfs")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def put(self, rel, data=dumpcheck.ZSTD_MAGIC + b"rest"):
        path = os.path.join(self.romfs, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    def full_dump(self):
        for rel in self.FILES:
            self.put(rel)

    def codes(self, **kw):
        return [p.code for p in dumpcheck.check_romfs(self.romfs, **kw)]

    def run_main(self, *extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = dumpcheck.main(["--user-dir", self.user, *extra])
        return code, out.getvalue()

    def test_complete_dump_is_found(self):
        self.full_dump()
        self.assertEqual(self.codes(), [])
        code, out = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn(f"romfs={self.romfs}", out)

    def test_no_dump(self):
        code, out = self.run_main()
        self.assertEqual(code, 2)
        self.assertIn("no RomFS dump", out)
        self.assertIn("prod.keys", out)

    def write_user(self, rel, text):
        p = os.path.join(self.user, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)

    def test_keys_too_old(self):
        # The player's case: master keys up to 15, the update needs gen 0x17.
        self.write_user("keys/prod.keys", "".join(f"master_key_{i:02x} = {'0' * 32}\n" for i in range(0x16)))
        self.write_user("log/eden_log.txt",
                        "[ 1.0] Loader <Error> core\\file_sys\\content_archive.cpp:NCA:48: "
                        "Cannot decrypt abc.nca: keys for gen 0x17+ required.\n")
        probs = dumpcheck.check_keys(self.user)
        self.assertEqual([p.code for p in probs], ["keys-old"])
        self.assertIn("master_key_16", probs[0].message)
        self.assertIn("master_key_15", probs[0].message)
        code, out = self.run_main()
        self.assertEqual(code, 2)
        self.assertIn("too old", out)
        # With master_key_16 the keys are new enough.
        self.write_user("keys/prod.keys", "master_key_16 = " + "0" * 32 + "\n")
        self.assertEqual(dumpcheck.check_keys(self.user), [])

    @unittest.skipIf(zstandard is None, "needs the zstandard package")
    def test_nup_in_game_list_wins(self):
        games = os.path.join(self.dir, "games")
        os.makedirs(games)
        game = os.path.join(games, "Game [x].nup")
        full = {pat.replace("*", "111"): (dumpcheck.ZSTD_MAGIC, nup.CODEC_RAW) for pat in dumpcheck.REQUIRED}
        nup.write_nup(game, [(int(dumpcheck.TITLE_ID, 16), 0x20000, nup.KIND_UPDATE, full)])
        self.write_user("config/qt-config.ini",
                        "[UI]\nPaths\\gamedirs\\size=2\nPaths\\gamedirs\\1\\path=SDMC\n"
                        f"Paths\\gamedirs\\2\\path={games.replace(os.sep, '/')}\n")
        os.makedirs(os.path.join(self.romfs, "Pack"))  # a broken dump is ignored
        code, out = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn(f"romfs={os.path.normpath(game)}", out)
        code, out = self.run_main("--romfs", game)
        self.assertEqual(code, 0)

    def test_keys_one_folder_too_deep(self):
        deep = os.path.join(self.user, "keys", "keys")
        os.makedirs(deep)
        open(os.path.join(deep, "prod.keys"), "w").close()
        codes = [p.code for p in dumpcheck.check_setup(self.user)]
        self.assertIn("keys-deep", codes)
        self.assertNotIn("keys", codes)

    def test_skeleton(self):
        os.makedirs(os.path.join(self.romfs, "Pack", "Actor"))
        self.assertEqual(self.codes(), ["skeleton"])
        self.assertEqual(self.run_main()[0], 1)

    def test_partial(self):
        self.full_dump()
        os.remove(os.path.join(self.romfs, "Pack", "Actor", "Player_Adult.pack.zs"))
        self.assertEqual(self.codes(), ["partial"])

    def test_stopped_mid_file(self):
        self.full_dump()
        self.put("Pack/Actor/Player_Child.pack.zs", b"\0" * 64)
        self.assertEqual(self.codes(), ["damaged"])

    def test_manual_dumped_instead(self):
        self.put("html-document/index.html", b"<html>")
        self.assertEqual(self.codes(), ["not-game"])

    def test_base_game_without_update(self):
        for rel in self.FILES:
            self.put(rel.replace(".111.", ".100."))
        self.assertEqual(self.codes(), ["version"])
        self.assertEqual(self.codes(any_version=True), [])

    def test_two_versions(self):
        self.full_dump()
        self.put("RSDB/ActorInfo.Product.100.rstbl.byml.zs")
        self.assertEqual(self.codes(), ["mixed"])

    def test_dump_root_setting(self):
        other = os.path.join(self.dir, "elsewhere")
        self.romfs = os.path.join(other, dumpcheck.TITLE_ID, "romfs")
        self.full_dump()
        os.makedirs(os.path.join(self.user, "config"))
        with open(os.path.join(self.user, "config", "qt-config.ini"), "w", encoding="utf-8") as f:
            f.write(f"dump_directory={other.replace(os.sep, '/')}\n")
        self.assertEqual([os.path.normcase(p) for p in dumpcheck.find_dumps(self.user)],
                         [os.path.normcase(self.romfs)])

    def test_parent_folder_given(self):
        self.full_dump()
        for given in (os.path.join(self.user, "dump"), os.path.dirname(self.romfs), self.romfs):
            self.assertEqual(dumpcheck.resolve(given), self.romfs)
        code, out = self.run_main("--romfs", os.path.join(self.user, "dump"))
        self.assertEqual(code, 0)

    def test_build_main_refuses_bad_dump(self):
        os.makedirs(os.path.join(self.romfs, "Pack"))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = build.main(["--romfs", self.romfs, "--mod-dir", os.path.join(self.dir, "mod")])
        self.assertEqual(code, 1)
        self.assertIn("Skeleton", err.getvalue())


class MapCapturesTest(unittest.TestCase):
    # Made-up numbers in the game's shape: entry i of the captures is the map of entry i of the menu.
    CAPTURE = {"MapCaptureParamArray": [
        {"CameraPos": {"X": -5.0, "Y": 120.0, "Z": -12.0}, "Height": 300.0,
         "CaptureParamData": {"01": {"MapCaptureParam": {"Near": -20.0}}, "00": {"MapCaptureParam": {"Near": -95.0}}}},
        {"CameraPos": {"X": 0.0, "Y": 200.0, "Z": 3.0}, "Height": 214.0, "IsDungeon": True,
         "CaptureParamData": {"00": {"MapCaptureParam": {}}, "01": {"MapCaptureParam": {"Far": 105.0, "Near": 93.5}}}},
        {"CaptureParamData": {"00": {"MapCaptureParam": {}}}},   # no camera: left out
        {"CameraPos": {"X": 1.0, "Y": 1.0, "Z": 1.0}, "Height": 9.0},  # past the menu's entries: left out
    ]}
    MENU = {"MapMenuParamArray": [
        {"LocationName": "Forest", "BancName": ["Forest_Child", "Forest_Adult"],
         "ChangeBancInfo": [{"BancName": "Forest_Adult_Alternative"}]},
        {"LocationName": "Temple", "BancName": ["Temple", "Temple"]},
        {"LocationName": "Nowhere", "BancName": ["Nowhere_Child", "Nowhere_Adult"]},
    ]}
    ROWS = ["map Forest -5 -12 120 300 0 Forest_Child,Forest_Adult,Forest_Adult_Alternative -95:- -20:-",
            "map Temple 0 3 200 214 1 Temple -:- 93.5:105"]

    def test_rows_in_order_with_floors(self):
        rows = map_captures.render(map_captures.captures(self.CAPTURE, self.MENU)).splitlines()
        self.assertEqual([r for r in rows if not r.startswith("#")], self.ROWS)

    def test_floors_in_their_number_order(self):
        cap = {"MapCaptureParamArray": [{"CameraPos": {"X": 0.0, "Y": 200.0, "Z": 0.0}, "Height": 10.0,
                                         "CaptureParamData": {str(i).zfill(1 if i > 9 else 2): {"MapCaptureParam": {"Near": float(i)}}
                                                              for i in (10, 2, 0, 1)}}]}
        menu = {"MapMenuParamArray": [{"LocationName": "Tower", "BancName": ["Tower", "Tower"]}]}
        c = map_captures.captures(cap, menu)[0]
        self.assertEqual([n for n, _f in c["floors"]], [0.0, 1.0, 2.0, 10.0])

    def test_missing_files_give_an_empty_table(self):
        d = tempfile.mkdtemp()
        try:
            out = os.path.join(d, "coop_maps.txt")
            self.assertEqual(map_captures.write_table(d, out), 0)
            self.assertTrue(all(line.startswith("#") for line in build.read(out).decode().splitlines()))
            self.assertEqual(map_captures.main(["--romfs", d, "--out", out]), 1)
        finally:
            shutil.rmtree(d)

    @unittest.skipUnless(os.environ.get("COOP_TEST_ROMFS"), "set COOP_TEST_ROMFS to the game's romfs")
    def test_real_romfs_the_field_is_the_biggest_area(self):
        # Only with the player's own game: checks the order rule (entry i is the map of entry i).
        d = tempfile.mkdtemp()
        try:
            out = os.path.join(d, "coop_maps.txt")
            self.assertGreater(map_captures.write_table(os.environ["COOP_TEST_ROMFS"], out), 20)
            caps = map_captures.captures(map_captures._load(os.environ["COOP_TEST_ROMFS"], map_captures.CAPTURE),
                                         map_captures._load(os.environ["COOP_TEST_ROMFS"], map_captures.MENU))
            areas = [c for c in caps if not c["dungeon"]]
            self.assertEqual(max(areas, key=lambda c: c["height"])["location"], "HyruleField")
            self.assertTrue(all(c["dungeon"] for c in caps if c["location"] in ("InsideTheDekuTree", "FireTemple")))
        finally:
            shutil.rmtree(d)


class DungeonRoomsTest(unittest.TestCase):
    def test_thin_keeps_points_a_step_apart_in_order(self):
        pts = [[0, 0, 0], [0.5, 0, 0], [1.0, 0, 0], [3, 0, 0], [3, 0.2, 0]]
        self.assertEqual(dungeon_rooms.thin(pts), [[0, 0, 0], [1.0, 0, 0], [3, 0, 0]])

    def test_render_sorted_rows(self):
        text = dungeon_rooms.render({"B": {2: [[1, 2, 3]]}, "A": {1: [[0, 0, 0], [0.1, 0, 0]], 0: [[-1.234, 5, 6]]}})
        rows = [line for line in text.splitlines() if not line.startswith("#")]
        self.assertEqual(rows, ["A 0 -1.23 5.00 6.00", "A 1 0.00 0.00 0.00", "B 2 1.00 2.00 3.00"])

    @unittest.skipUnless(os.environ.get("COOP_TEST_ROMFS"), "set COOP_TEST_ROMFS to the game's romfs")
    def test_real_romfs_each_placement_in_its_own_room(self):
        # Only with the player's own game; nothing of it is kept in the repo.
        rooms = dungeon_rooms.scene_rooms(os.environ["COOP_TEST_ROMFS"])
        self.assertTrue(rooms)
        for scene, by_room in rooms.items():
            pts = [(r, p) for r, ps in by_room.items() for p in dungeon_rooms.thin(ps)]
            for room, p in pts:
                best = min(pts, key=lambda q: (q[1][0] - p[0]) ** 2 + (2 * (q[1][1] - p[1])) ** 2
                           + (q[1][2] - p[2]) ** 2)
                self.assertEqual(best[0], room, f"{scene} {p}")


class PlaceRoomsTest(unittest.TestCase):
    """deku-ladder: a live place's own room is its room graph's vote, kept only when it is not a tie and our Link
    near the place is guessed in that room (review round 5)."""

    def test_a_tie_is_no_room(self):
        self.assertIsNone(place_rooms.choose({3: 1, 2: 1}))  # Jabu's GreenTentacle asker
        self.assertIsNone(place_rooms.choose({}))
        self.assertEqual(place_rooms.choose({17: 3, 5: 1}), 17)

    def test_a_room_link_is_never_in_near_the_place_is_not_kept(self):
        # Rooms 5 and 6 around the place; room 7's only placement 45 m away (Ganon's BarrierRemove_Water).
        points = [(5, 0.0, 0.0, 0.0), (5, 6.0, 0.0, 0.0), (6, 0.0, 0.0, 8.0), (7, 45.0, 0.0, 0.0)]
        room, why = place_rooms.place_room({7: 1}, points, (0.0, 0.0, 0.0))
        self.assertEqual(room, -1)
        self.assertIn("voted", why)
        room, _ = place_rooms.place_room({5: 2, 6: 1}, points, (0.0, 0.0, 0.0))
        self.assertEqual(room, 5)
        room, why = place_rooms.place_room({5: 1, 6: 1}, points, (0.0, 0.0, 0.0))
        self.assertEqual((room, why[:5]), (-1, "a tie"))

    def test_the_guess_is_coop_rooms(self):
        points = [(1, 0.0, 0.0, 0.0), (2, 0.0, 4.0, 0.0)]
        self.assertEqual(place_rooms.guess(points, (0.0, 1.5, 0.0)), 1)  # height counts twice
        self.assertEqual(place_rooms.guess(points, (0.0, 2.5, 0.0)), 2)
        self.assertIsNone(place_rooms.guess(points, (30.0, 0.0, 0.0)))  # beyond kFar


if __name__ == "__main__":
    unittest.main()
