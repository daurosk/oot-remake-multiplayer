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

from puppet_pack import build, byml, dumpcheck, nup, rstb, sarc  # noqa: E402

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
            byml.loads(b"BY\x00\x07" + b"\0" * 12)


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
        self.assertEqual(comps["XLinkRef"], build.FIXED_COMPONENTS["XLinkRef"])
        self.assertNotIn("ELinkRef", comps)
        for p in ("Component/SLink/Player_Child.engine__component__SLinkParam.bgyml",
                  "Component/SoundFootStepParam/Player_Child.game__component__SoundFootStepParam.bgyml",
                  "Component/SoundFootStepParam/Player.game__component__SoundFootStepParam.bgyml"):
            self.assertIn(p, out)
        for p in ("Component/SLink/Player.engine__component__SLinkParam.bgyml",
                  "Component/ELink/Player.engine__component__ELinkParam.bgyml",
                  "Component/XLink/Player.engine__component__XLinkParam.bgyml"):
            self.assertNotIn(p, out)

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
                          "CoopShot_Boomerang"])

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

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def write(self, rel, data):
        p = os.path.join(self.romfs, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

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

    def test_main_player_footsteps(self):
        from puppet_pack import zs
        mod = os.path.join(self.dir, "mod5")
        self.assertEqual(build.main(["--romfs", self.romfs, "--mod-dir", mod, "--any-version",
                                     "--player-footsteps"]), 0)
        man = json.loads(build.read(os.path.join(mod, "coop_puppet.json")))
        self.assertIs(man["sound"], True)
        self.assertEqual(man["footsteps"], "player")
        self.assertEqual([a["footstep"] for a in man["actors"]], [None, None])
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
        self.assertEqual([a["footstep"] for a in man["actors"]], [None, None])
        d = zs.Dicts.from_romfs(self.romfs)
        out = os.path.join(mod, "romfs")
        rows = {r["__RowId"]: r for r in byml.loads(d.decompress(build.read(
            os.path.join(out, "RSDB/ActorInfo.Product.999.rstbl.byml.zs"))))}
        self.assertNotIn("SLinkUserName", rows["CoopPuppet_Adult"])
        files = sarc.read(d.decompress(build.read(os.path.join(out, "Pack/Actor/CoopPuppet_Adult.pack.zs"))))
        self.assertFalse([n for n in files if "SLink" in n or "SoundFootStep" in n])

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
        self.assertEqual({f"Pack/Actor/{src}.pack.zs" for src, _ in build.STAND_INS},
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


if __name__ == "__main__":
    unittest.main()
