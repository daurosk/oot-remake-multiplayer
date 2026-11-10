"""Tests for the puppet pack builder. Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_puppet_pack.py
"""
import contextlib
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
              {"__RowId": "Player_Adult", "ActorName": "Player", "FmdbName": "Player_Adult"},
              {"__RowId": "Player_Child", "ActorName": "Player", "FmdbName": "Player_Child"}]
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
        ids = [r["__RowId"] for r in byml.loads(d.decompress(ai_raw))]
        self.assertEqual(ids, ["Animal", "CoopPuppet_Adult", "CoopPuppet_Child", "Player_Adult", "Player_Child"])
        gai = byml.loads(d.decompress(build.read(os.path.join(out, "RSDB/GameActorInfo.Product.999.rstbl.byml.zs"))))
        self.assertIn({"__RowId": "CoopPuppet_Child"}, gai)
        t = rstb.Rstb.loads(d.decompress(build.read(os.path.join(
            out, "System/Resource/ResourceSizeTable.Product.999.Test.rsizetable.zs"))))
        self.assertGreater(t.get("Pack/Actor/CoopPuppet_Child.pack"), 0)
        self.assertEqual(t.get("Actor/CoopPuppet_Adult.engine__actor__ActorParam.bgyml"), 8000)
        self.assertGreaterEqual(t.get("RSDB/ActorInfo.Product.999.rstbl.byml"), 1000)
        man = json.loads(build.read(os.path.join(mod, "coop_puppet.json")))
        self.assertEqual([a["actor"] for a in man["actors"]], ["CoopPuppet_Child", "CoopPuppet_Adult"])
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
