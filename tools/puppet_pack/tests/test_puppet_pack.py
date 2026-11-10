"""Tests for the puppet pack builder. Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_puppet_pack.py
"""
import json
import os
import random
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import build, byml, rstb, sarc  # noqa: E402

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
        }
    }
    child = {
        "$parent": "Work/Actor/Player_Base.engine__actor__ActorParam.gyml",
        "Components": {
            "ModelInfoRef": f"?Component/ModelInfo/{variant}.engine__component__ModelInfo.bgyml",
            "EquipmentUserRef": f"?Component/EquipmentUserParam/{variant}.game__component__EquipmentUserParam.bgyml",
        },
    }
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

    def test_ref_to_member(self):
        self.assertEqual(build.ref_to_member("?Component/X.bgyml"), "Component/X.bgyml")
        self.assertEqual(build.ref_to_member("Work/Component/X.gyml"), "Component/X.bgyml")
        self.assertIsNone(build.ref_to_member(3))

    def test_info_rows(self):
        rows = [{"__RowId": "B"}, {"__RowId": "D"}]
        player = {"__RowId": "Player_Child", "ActorName": "Player", "ELinkUserName": "Player",
                  "CalcRadius": 20.0, "FmdbName": "Player_Child"}
        new = build.info_row(player, "C")
        self.assertEqual(new["FmdbName"], "Player_Child")
        self.assertNotIn("ActorName", new)
        self.assertEqual(new["CalcRadius"], 100000.0)
        out = build.upsert_rows(rows, [new, {"__RowId": "A"}])
        self.assertEqual([r["__RowId"] for r in out], ["A", "B", "C", "D"])
        again = build.upsert_rows(out, [new])
        self.assertEqual(len(again), 4)

    def test_info_row_heap_cap(self):
        # Issue 111: the adult puppet asked Player_Adult's 10 MB instance heap and was never made.
        adult = {"__RowId": "Player_Adult", "InstanceHeapSize": 10245064}
        child = {"__RowId": "Player_Child", "InstanceHeapSize": 5692448}
        self.assertEqual(build.info_row(adult, "A", 6012792)["InstanceHeapSize"], 6012792)
        self.assertEqual(build.info_row(child, "C", 6012792)["InstanceHeapSize"], 5692448)
        self.assertEqual(build.info_row(adult, "A")["InstanceHeapSize"], 10245064)
        self.assertEqual(adult["InstanceHeapSize"], 10245064)  # the game's row is not changed
        big = {"__RowId": "P", "InstanceHeapSize": byml.UInt(9000000)}
        capped = build.info_row(big, "A", byml.UInt(6000000))["InstanceHeapSize"]
        self.assertIsInstance(capped, byml.UInt)
        self.assertEqual(capped.v, 6000000)
        self.assertNotIn("InstanceHeapSize", build.info_row({"__RowId": "X"}, "A", 6012792))


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
               "InstanceHeapSize": 10245064},
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
        # The adult puppet's heap is capped at Player_Base's; the child keeps its own; the game's rows stay.
        self.assertEqual(rows["CoopPuppet_Adult"]["InstanceHeapSize"], 6012792)
        self.assertEqual(rows["CoopPuppet_Child"]["InstanceHeapSize"], 5692448)
        self.assertEqual(rows["Player_Adult"]["InstanceHeapSize"], 10245064)
        gai = byml.loads(d.decompress(build.read(os.path.join(out, "RSDB/GameActorInfo.Product.999.rstbl.byml.zs"))))
        self.assertIn({"__RowId": "CoopPuppet_Child"}, gai)
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            out, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        self.assertGreater(t.get("Pack/Actor/CoopPuppet_Child.pack"), 0)
        self.assertEqual(t.get("Actor/CoopPuppet_Adult.engine__actor__ActorParam.bgyml"), 8000)
        self.assertGreaterEqual(t.get("RSDB/ActorInfo.Product.999.rstbl.byml"), 1000)
        man = json.loads(build.read(os.path.join(mod, "coop_puppet.json")))
        self.assertEqual([a["actor"] for a in man["actors"]], ["CoopPuppet_Child", "CoopPuppet_Adult"])
        self.assertEqual([a["instance_heap"] for a in man["actors"]], [5692448, 6012792])
        for a in man["actors"]:
            self.assertIn("EquipmentUser_Weapon", a["equipment_keys"])
            self.assertIn("EquipmentUser_Shield", a["equipment_keys"])
        # Running again gives the same files.
        first = {r: build.read(os.path.join(out, r)) for r in
                 ("RSDB/ActorInfo.Product.999.rstbl.byml.zs", "Pack/Actor/CoopPuppet_Child.pack.zs")}
        build.build(self.romfs, mod, log=lambda *_: None)
        for r, b in first.items():
            self.assertEqual(build.read(os.path.join(out, r)), b)

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


if __name__ == "__main__":
    unittest.main()
