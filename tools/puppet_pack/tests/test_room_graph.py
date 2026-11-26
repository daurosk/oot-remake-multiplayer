"""Tests for eye_switches.read_graph and switch_events on a tiny synthetic room (row 98(2)): an unwired UpdateOnce
input of a gate means the game reads that gate once, when the room loads, so a flag behind it is "flag-once": it
deletes our asker only at a load, never live, and is no guard. No game files.

    python tools/puppet_pack/tests/test_room_graph.py   (CI runs it through test_puppet_pack.py)
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import byml, eye_switches  # noqa: E402

TYPES = eye_switches.TYPES


def ainb(nodes, multi=()):
    """The bytes of an AI file as read_graph reads it. nodes: [{"type", "imm": {name: value}, "bool": [(name, src,
    out, value)], "string": [(name, default)], "out": [bool output names]}]; multi: [(node, output)]."""
    pool, at = bytearray(b"\0"), {"": 0}

    def s(t):
        if t not in at:
            at[t] = len(pool)
            pool.extend(t.encode() + b"\0")
        return at[t]

    head = 0x74
    bodies = head + 0x3C * len(nodes)
    body_size = 0x30 + 16 * len(TYPES)
    imm = bodies + body_size * len(nodes)
    str_imm = imm + 0x10
    n_imm = sum(len(n.get("imm", {})) for n in nodes)
    io = str_imm + 0x0C * n_imm
    lists = io + 4 * 12
    entries = {t: [] for t in TYPES}  # (input entries bytes, output entries bytes)
    outs = {t: [] for t in TYPES}
    node_bytes, body_bytes, imm_bytes = bytearray(), bytearray(), bytearray()
    k = 0
    for n in nodes:
        node = bytearray(0x3C)
        struct.pack_into("<II", node, 8, s(n.get("type", "")), 0)
        struct.pack_into("<I", node, 0x14, bodies + len(body_bytes))
        node_bytes += node
        body = bytearray(body_size)
        struct.pack_into("<II", body, 0x18, k, len(n.get("imm", {})))
        for name, value in n.get("imm", {}).items():
            imm_bytes += struct.pack("<III", s(name), 0, s(value))
            k += 1
        for t, name in enumerate(TYPES):
            ins = []
            if name == "bool":
                ins = [struct.pack("<IhhII", s(nm), src, o, 0, v) for nm, src, o, v in n.get("bool", [])]
            elif name == "string":
                ins = [struct.pack("<IhhII", s(nm), -1, 0, 0, s(d)) for nm, d in n.get("string", [])]
            o_names = [struct.pack("<I", s(x)) for x in (n.get("out", []) if name == "bool" else [])]
            struct.pack_into("<IIII", body, 0x30 + 16 * t, len(entries[name]), len(ins), len(outs[name]),
                             len(o_names))
            entries[name] += ins
            outs[name] += o_names
        body_bytes += body
    starts, o = [], lists
    blob = bytearray()
    for name in TYPES:
        starts.append(o)
        for e in entries[name]:
            blob += e
        o += sum(len(e) for e in entries[name])
        starts.append(o)
        for e in outs[name]:
            blob += e
        o += sum(len(e) for e in outs[name])
    multi_at = o
    for node, output in multi:
        blob += struct.pack("<HHI", node, output, 0)
    pool_at = multi_at + 8 * len(multi)
    out = bytearray(head)
    struct.pack_into("<II", out, 0x0C, 0, len(nodes))
    struct.pack_into("<I", out, 0x24, pool_at)
    struct.pack_into("<I", out, 0x2C, imm)
    struct.pack_into("<II", out, 0x34, io, multi_at)
    out += node_bytes + body_bytes
    out += struct.pack("<IIII", 0, 0, 0, str_imm) + imm_bytes
    out += struct.pack("<12I", *starts) + blob + pool
    return bytes(out)


def room(once):
    """An asker started by a step switch; deleted by Room_Flag through an AND whose UpdateOnce is `once`."""
    return [
        {"type": "ActorLogicEventRequestActor", "imm": {"InstanceName": "Asker"},
         "bool": [("EventRequestTrigger", 1, 1, 0), ("Logic_Delete", 3, 1, 0), ("Logic_Create", -1, 0, 1)],
         "out": ["Dead", "EventBool"]},
        {"type": "ActorLogicBootUpAndForceTrunOffControlActor", "imm": {"InstanceName": "Switch"},
         "out": ["OffState", "OnState"]},
        {"type": "HoldGameDataGetBoolSimple", "string": [("GameDataNameBool", "Room_Flag")], "out": ["Bool"]},
        {"type": "OperateAND", "bool": [("BoolMulti", 2, 0, 0), ("UpdateOnce", -1, 0, once)], "out": ["False", "True"]},
    ]


class _Plain:
    @staticmethod
    def decompress(raw):
        return raw


class ReadGraphTest(unittest.TestCase):
    def test_unwired_bool_inputs_keep_their_constant(self):
        nodes, _ = eye_switches.read_graph(ainb(room(1)))
        self.assertEqual(nodes[3]["type"], "OperateAND")
        self.assertEqual(nodes[3]["bdef"], {"UpdateOnce": 1})
        self.assertEqual(nodes[0]["bdef"], {"Logic_Create": 1})  # wired inputs have no constant
        self.assertEqual(nodes[2]["sdef"], {"GameDataNameBool": "Room_Flag"})
        self.assertEqual(eye_switches.read_graph(ainb(room(0)))[0][3]["bdef"], {"UpdateOnce": 0})

    def test_multi_input_list(self):
        nodes, multi = eye_switches.read_graph(ainb(room(0), multi=[(2, 0), (1, 1)]))
        self.assertEqual(multi(0, 2), [(2, 0), (1, 1)])


class SwitchEventsTest(unittest.TestCase):
    def run_room(self, once):
        with tempfile.TemporaryDirectory() as romfs:
            os.makedirs(os.path.join(romfs, "Logic"))
            os.makedirs(os.path.join(romfs, "Banc", "Scene"))
            with open(os.path.join(romfs, "Logic", "Room.logic.module.ainb"), "wb") as f:
                f.write(ainb(room(once)))
            actors = [{"Hash": 1, "Gyaml": "EventRequestActor", "Dynamic": {"RequestEventName": "Ev"},
                       "Translate": [1.0, 0.0, 0.0]},
                      {"Hash": 2, "Gyaml": "Obj_StepSwitch_A_01", "Translate": [0.0, 0.0, 0.0]}]
            groups = [{"Logic": "Work/Logic/Room.logic.module.ain",
                       "References": [{"Path": "Asker", "Id": "Asker", "Reference": 1},
                                      {"Path": "Switch", "Id": "Switch", "Reference": 2}]}]
            with open(os.path.join(romfs, "Banc", "Scene", "Room.bcett.byml.zs"), "wb") as f:
                f.write(byml.dumps({"Actors": actors, "AiGroups": groups}))
            wiring = {}
            lines, guards, _, _ = eye_switches.switch_events(romfs, dicts=_Plain(), wiring=wiring)
            return lines, guards, wiring

    def test_a_live_delete_flag_is_a_guard(self):
        lines, guards, wiring = self.run_room(0)
        self.assertEqual(len(lines), 1)
        self.assertEqual(lines[0][:3], ("Ev", "Scene", "Obj_StepSwitch_A_01"))
        self.assertEqual(guards, [("Room_Flag", "Scene", "Ev", "deletes the asker")])
        self.assertEqual(wiring[lines[0]]["delete"], ["Room_Flag"])
        self.assertEqual(wiring[lines[0]]["guards"], ["Room_Flag"])  # g3-objects: a live guard

    def test_a_delete_flag_read_once_at_load_is_no_guard(self):
        lines, guards, wiring = self.run_room(1)
        self.assertEqual(len(lines), 1)
        self.assertEqual(guards, [])
        self.assertEqual(wiring[lines[0]]["delete"], [])
        # objects-next-a-6: for g3-objects' hold it is a LOAD guard (on at our room's load: our asker is gone)
        self.assertEqual(wiring[lines[0]]["guards"], ["load:Room_Flag"])


def lift_room(flag_alone):
    """A lift whose IsOn ORs the asker's EventBool with Room_Flag: alone (flag_alone) or ANDed with the switch (the
    Deku Tree compass lifts' OR(asker, AND(RoomD_StepSwitch_On_01, plate)))."""
    nodes = [
        {"type": "ActorLogicEventRequestActor", "imm": {"InstanceName": "Asker"},
         "bool": [("EventRequestTrigger", 1, 1, 0)], "out": ["Dead", "EventBool"]},
        {"type": "ActorLogicBootUpAndForceTrunOffControlActor", "imm": {"InstanceName": "Switch"},
         "out": ["OffState", "OnState"]},
        {"type": "HoldGameDataGetBoolSimple", "string": [("GameDataNameBool", "Room_Flag")], "out": ["Bool"]},
        {"type": "OperateAND", "bool": [("BoolMulti", -100, 2, 0), ("UpdateOnce", -1, 0, 0)], "out": ["False", "True"]},
        {"type": "OperateOR", "bool": [("BoolMulti", -102, 2, 0), ("UpdateOnce", -1, 0, 0)], "out": ["False", "True"]},
        {"type": "ActorLogicLift", "imm": {"InstanceName": "Lift"}, "bool": [("IsOn", 4, 1, 0)], "out": ["Dead"]},
    ]
    multi = [(2, 0), (1, 1), (0, 1), (2, 0) if flag_alone else (3, 1)]
    return nodes, multi


class AskerClassTest(unittest.TestCase):
    def run_room(self, flag_alone):
        with tempfile.TemporaryDirectory() as romfs:
            os.makedirs(os.path.join(romfs, "Logic"))
            os.makedirs(os.path.join(romfs, "Banc", "Scene"))
            nodes, multi = lift_room(flag_alone)
            with open(os.path.join(romfs, "Logic", "Room.logic.module.ainb"), "wb") as f:
                f.write(ainb(nodes, multi))
            actors = [{"Hash": 1, "Gyaml": "EventRequestActor", "Dynamic": {"RequestEventName": "Ev"},
                       "Translate": [1.0, 0.0, 0.0]},
                      {"Hash": 2, "Gyaml": "Obj_StepSwitch_A_01", "Translate": [0.0, 0.0, 0.0]},
                      {"Hash": 3, "Gyaml": "Obj_Lift", "Translate": [5.0, 0.0, 0.0]}]
            groups = [{"Logic": "Work/Logic/Room.logic.module.ain",
                       "References": [{"Path": n, "Id": n, "Reference": h}
                                      for n, h in (("Asker", 1), ("Switch", 2), ("Lift", 3))]}]
            with open(os.path.join(romfs, "Banc", "Scene", "Room.bcett.byml.zs"), "wb") as f:
                f.write(byml.dumps({"Actors": actors, "AiGroups": groups}))
            askers = {}
            eye_switches.switch_events(romfs, dicts=_Plain(), askers=askers)
            return askers

    def test_a_shared_flag_alone_in_the_or_is_live(self):
        askers = self.run_room(True)
        key = ("Ev", "Scene", (1.0, 0.0, 0.0))
        self.assertEqual(list(askers), [key])
        self.assertEqual(list(askers[key]["outputs"]), ["EventBool"])
        self.assertEqual(eye_switches.asker_class(key, askers[key], {"Room_Flag"})[0], "live")
        self.assertEqual(eye_switches.asker_class(key, askers[key], set())[0], "asker")  # not shared

    def test_a_shared_flag_anded_in_the_or_is_no_live_result(self):
        askers = self.run_room(False)
        key = ("Ev", "Scene", (1.0, 0.0, 0.0))
        cls, why = eye_switches.asker_class(key, askers[key], {"Room_Flag"})
        self.assertEqual(cls, "asker")
        self.assertIn("Obj_Lift.IsOn", why)
        (_, _, (_, also)), = askers[key]["outputs"]["EventBool"]
        self.assertIn(("Room_Flag", False, 2), also)  # in an OR, behind an AND
        self.assertIn(("EventRequestActor", False, 1), also)  # alone in the OR


class ResultsOutputTest(unittest.TestCase):
    """--results (row 98(2)): a class-asker line names its outputs; a shared live flag that deletes its asker is a
    result guard (coop_event_result.cpp kGuards); a flag read once at load is none."""

    def run_main(self, once):
        from unittest import mock
        import contextlib
        import io
        with tempfile.TemporaryDirectory() as romfs:
            os.makedirs(os.path.join(romfs, "Logic"))
            os.makedirs(os.path.join(romfs, "Banc", "Scene"))
            nodes = room(once) + [{"type": "ActorLogicLift", "imm": {"InstanceName": "Lift"},
                                   "bool": [("IsOn", 0, 1, 0)], "out": ["Dead"]}]
            with open(os.path.join(romfs, "Logic", "Room.logic.module.ainb"), "wb") as f:
                f.write(ainb(nodes))
            actors = [{"Hash": 1, "Gyaml": "EventRequestActor", "Dynamic": {"RequestEventName": "Ev"},
                       "Translate": [1.0, 0.0, 0.0]},
                      {"Hash": 2, "Gyaml": "Obj_StepSwitch_A_01", "Translate": [0.0, 0.0, 0.0]},
                      {"Hash": 3, "Gyaml": "Obj_Lift", "Translate": [5.0, 0.0, 0.0]}]
            groups = [{"Logic": "Work/Logic/Room.logic.module.ain",
                       "References": [{"Path": n, "Id": n, "Reference": h}
                                      for n, h in (("Asker", 1), ("Switch", 2), ("Lift", 3))]}]
            with open(os.path.join(romfs, "Banc", "Scene", "Room.bcett.byml.zs"), "wb") as f:
                f.write(byml.dumps({"Actors": actors, "AiGroups": groups}))
            dungeon = os.path.join(romfs, "coop_dungeon.cpp")
            with open(dungeon, "w", encoding="utf-8") as f:
                f.write('    {0x12345678, false, "Room_Flag"},\n')
            out = io.StringIO()
            with mock.patch.object(eye_switches.zs.Dicts, "from_romfs", return_value=_Plain()), \
                    contextlib.redirect_stdout(out):
                self.assertEqual(eye_switches.main(["--romfs", romfs, "--dungeon", dungeon, "--results"]), 0)
            return out.getvalue()

    def test_a_live_delete_of_a_class_asker_asker_is_a_result_guard(self):
        text = self.run_main(0)
        self.assertIn('{"Ev", "Scene", {1.0f, 0.0f, 0.0f}, "EventBool"},  // asker', text)
        guards = text.split("// kResultGuards", 1)[1]
        self.assertIn('{"Room_Flag", "Scene", "Ev"},', guards)

    def test_a_delete_read_once_at_load_is_no_result_guard(self):
        text = self.run_main(1)
        guards = text.split("// kResultGuards", 1)[1].split("// kTriggers", 1)[0]
        self.assertNotIn("Room_Flag", guards)


class TriggerGuardsTest(unittest.TestCase):
    """--results' kTriggers (review-a38-5): every leaf of an asker's trigger that another game's shared state can
    raise is a guard (a shared flag, a silver rupee counter's key and its tokens, a kill); every other leaf is a proof
    line saying why it cannot rise."""

    def guards(self, trigger, shared=(), keys=(), one_way=(), plugs=None, world=None):
        askers = {("Ev", "Scene", (1.0, 0.0, 0.0)): {"trigger": trigger}}
        return eye_switches.trigger_guards(askers, None, set(shared), set(keys), set(one_way), plugs or {},
                                           {"Scene": world} if world else None)

    def test_followed_world_objects_are_follow_guards(self):
        # Review of 70ba0763: coop_world follows another game's blocks to their rest place, so their push can raise
        # our trigger: a block in a press switch's room (each waits), a puzzle manager's linked blocks (group = the
        # manager: the hook keeps the largest), a followed trigger actor itself (a hammer switch's press).
        world = {
            50: ("Obj_StepSwitch_A_01", 3, (0.0, 0.0, 0.0), ()),
            51: ("Obj_MovableBox_A_01", 3, (5.0, 0.0, 0.0), ()),
            52: ("Obj_AmosStatue_A_01", 3, (9.0, 0.0, 0.0), ()),
            53: ("Obj_MovableBox_A_01", 4, (1.0, 0.0, 0.0), ()),        # another room: cannot rest on it
            54: ("Obj_MovableBox_BasePosAnchor", 3, (5.0, 0.0, 0.0), ()),  # an anchor: not followed on its own
            60: ("ForestTemple_MovableBox_Puzzle_Manager", None, (0.0, 0.0, 0.0), (61, 62, 54)),
            61: ("Obj_ForestTemple_MovableBox_A_01", 25, (0.0, 0.0, 0.0), ()),
            62: ("Obj_ForestTemple_MovableBox_A_02", 25, (0.0, 0.0, 0.0), ()),
            70: ("Obj_HammerSwitch_A_01", 3, (0.0, 0.0, 0.0), ()),
            80: ("Obj_EyeSwitch_A_02", 3, (0.0, 0.0, 0.0), ()),
        }
        guards, proofs = self.guards([("actor", "Obj_StepSwitch_A_01", 50, "FirstOn", False, 0, (0.0, 0.0, 0.0), ()),
                                      ("actor", "ForestTemple_MovableBox_Puzzle_Manager", 60, "CorrectPos", False, 0,
                                       (0.0, 0.0, 0.0), ()),
                                      ("actor", "Obj_HammerSwitch_A_01", 70, "OnState", False, 0, (0.0, 0.0, 0.0), ()),
                                      ("actor", "Obj_EyeSwitch_A_02", 80, "OnState", False, 0, (0.0, 0.0, 0.0), ())],
                                     world=world)
        follow = sorted((g[4], g[5]) for g in guards if g[0] == "follow")
        self.assertEqual(follow, [(51, 0), (52, 0), (61, 60), (62, 60), (70, 0)])
        self.assertEqual(len(guards), 5)
        self.assertEqual(len(proofs), 1)
        self.assertIn("a followed block in its room cannot drive it", proofs[0][2])

    def test_a_press_switch_with_no_room_takes_the_blocks_in_reach(self):
        world = {50: ("Obj_StepSwitch_A_01", None, (0.0, 0.0, 0.0), ()),
                 51: ("Obj_MovableBox_A_01", None, (10.0, 0.0, 0.0), ()),
                 52: ("Obj_MovableBox_A_01", None, (30.0, 0.0, 0.0), ())}
        guards, _ = self.guards([("actor", "Obj_StepSwitch_A_01", 50, "OnState", False, 0, (0.0, 0.0, 0.0), ())],
                                world=world)
        self.assertEqual([g[4] for g in guards], [51])

    def test_is_followed_matches_coop_world(self):
        for name in ("Obj_MovableBox_A_01", "Obj_SliderBox_A_01", "Obj_AmosStatue_A_01", "Obj_RollingRock_A_01",
                     "Obj_HammerSwitch_A_01", "Obj_FireTemple_HammerDropPillar_A_01", "Obj_Deku_MoveFloor_A_01",
                     "Obj_InsideTheDekuTree_FallingLadder_A_01"):
            self.assertTrue(eye_switches.is_followed(name), name)
        for name in ("Obj_MovableBox_BasePosAnchor", "Obj_StepSwitch_A_01", "Obj_RollingRockArea_A_01",
                     "Enemy_Wolfos", "Obj_Pot_A_01"):
            self.assertFalse(eye_switches.is_followed(name), name)

    def test_a_shared_flag_waits_and_an_unshared_or_negated_one_is_proved(self):
        guards, proofs = self.guards([("flag", "Shared_On", 0, "Bool", False, 0, (), ()),
                                      ("flag", "Shared_Done", 0, "Bool", True, 0, (), ()),
                                      ("flag", "Own", 0, "Bool", False, 0, (), ()),
                                      ("flag-once", "Shared_Load", 0, "Bool", False, 0, (), ())],
                                     shared=("Shared_On", "Shared_Done", "Shared_Load"), one_way=("Shared_Done",))
        self.assertEqual(guards, [("flag", "Scene", "Ev", "Shared_On", 0, 0, "a shared flag")])
        text = " ".join(t for _, _, t in proofs)
        self.assertIn("Shared_Done: read negated", text)
        self.assertIn("its 0 is never sent (one way)", text)
        self.assertIn("Own: not shared", text)
        self.assertIn("Shared_Load: read once", text)

    def test_a_silver_rupee_counter_guards_its_key_and_its_shared_tokens(self):
        guards, _ = self.guards([("actor", "Obj_silverRupeeCounter_03", 100, "AllGet", False, 0, (0.0, 0.0, 0.0),
                                  (7, 8, 9))], keys=(100, 7, 8))
        self.assertIn(("key", "Scene", "Ev", "", 100, 0, "Obj_silverRupeeCounter_03's own key"), guards)
        self.assertEqual(sorted(g[4] for g in guards if g[0] == "token"), [7, 8])
        self.assertTrue(all(g[5] == 100 for g in guards if g[0] == "token"))

    def test_an_unshared_counter_is_proved(self):
        guards, proofs = self.guards([("actor", "Obj_silverRupeeCounter", 100, "AllGet", False, 0, (0.0, 0.0, 0.0),
                                       (7,))], keys=(7,))
        self.assertEqual(guards, [])
        self.assertIn("not shared", proofs[0][2])

    def test_kills_are_guards_and_a_live_plug_only_lowers(self):
        guards, proofs = self.guards([("actor", "Enemy_TorchSlug", 5, "Dead", False, 0, (0.0, 0.0, 0.0), ()),
                                      ("actor", "Enemy_Poe", 6, "Logic_Alive", True, 0, (0.0, 0.0, 0.0), ()),
                                      ("actor", "Enemy_Poe", 6, "Logic_Alive", False, 2, (0.0, 0.0, 0.0), ())])
        self.assertEqual(sorted(g[4] for g in guards if g[0] == "kill"), [5, 6])
        self.assertIn("only lowers it", proofs[0][2])

    def test_a_keyed_actor_is_a_key_and_a_switch_share_plug_is_flagged(self):
        guards, proofs = self.guards([("actor", "Obj_Ice_A_01", 40, "isArrow", False, 0, (0.0, 0.0, 0.0), ()),
                                      ("actor", "Obj_StepSwitch_A_01", 41, "OnState", False, 0, (2.0, 0.0, 0.0), ()),
                                      ("actor", "Obj_EyeSwitch_A_02", 42, "OnState", False, 0, (9.0, 0.0, 0.0), ()),
                                      ("node", "HoldPlayerState", 0, "Out", False, 0, (), ())],
                                     keys=(40,), plugs={("Scene", "Obj_StepSwitch_A_01"): [((2.5, 0.0, 0.0), "OnState", "")]})
        self.assertEqual(guards, [("key", "Scene", "Ev", "", 40, 0, "Obj_Ice_A_01's key")])
        text = " ".join(t for _, _, t in proofs)
        self.assertIn("Obj_StepSwitch_A_01.OnState: HELD BY THE SWITCH SHARE", text)
        self.assertIn("Obj_EyeSwitch_A_02.OnState: each game's own", text)
        self.assertIn("our own game's state", text)

    def test_switch_plugs_reads_both_line_forms_and_a_guarded_line_is_proved(self):
        # Lines as in g3-objects 59a488df's coop_room_objects.cpp kSwitchPlugs (5 fields) and the 4-field form.
        lines = ['    {"DodongoCavern", "Obj_EyeSwitch_A_05", {65.7f, 121.8f, -23.0f}, "OnState", '
                 '"DodongoCavern_NaviHelp_17_00"},  // Obj_DodongoCave_FireWall_A_01; asks FocusOnGimmick_Basic',
                 '    {"DodongoCavern", "Obj_StepSwitch_A_01", {-49.0f, 114.2f, -8.0f}, "OnState", ""},  // shutter',
                 '    {"Scene", "Obj_SwitchHit_A_01", {1.0f, 2.0f, 3.0f}, "OnState"},',
                 '    {"Scene", "Obj_SwitchHit_A_02", {1.0f, 2.0f, 3.0f}, "OnState", "load:Room_Flag+Other_Flag"},']
        text = "\n".join(lines) + "\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "coop_room_objects.cpp")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            plugs = eye_switches.switch_plugs(path)
        self.assertEqual(plugs[("DodongoCavern", "Obj_EyeSwitch_A_05")],
                         [((65.7, 121.8, -23.0), "OnState", "DodongoCavern_NaviHelp_17_00")])
        self.assertEqual(plugs[("DodongoCavern", "Obj_StepSwitch_A_01")], [((-49.0, 114.2, -8.0), "OnState", "")])
        self.assertEqual(plugs[("Scene", "Obj_SwitchHit_A_01")], [((1.0, 2.0, 3.0), "OnState", "")])
        self.assertEqual(plugs[("Scene", "Obj_SwitchHit_A_02")][0][2], "load:Room_Flag+Other_Flag")
        _, proofs = self.guards([("actor", "Obj_EyeSwitch_A_05", 9, "OnState", False, 0, (65.7, 121.8, -23.0), ()),
                                 ("actor", "Obj_StepSwitch_A_01", 8, "OnState", False, 0, (-49.0, 114.2, -8.0), ())],
                                plugs={("Scene", k[1]): v for k, v in plugs.items() if k[0] == "DodongoCavern"})
        text = " ".join(t for _, _, t in proofs)
        self.assertIn("Obj_EyeSwitch_A_05.OnState: held by the switch share only behind its guard "
                      "DodongoCavern_NaviHelp_17_00", text)
        self.assertIn("Obj_StepSwitch_A_01.OnState: HELD BY THE SWITCH SHARE (not covered)", text)

    def test_the_results_output_prints_the_trigger_table(self):
        text = ResultsOutputTest().run_main(0)
        self.assertIn("// kTriggers", text)
        self.assertIn("// a trigger leaf shared state cannot raise, and why", text)


if __name__ == "__main__":
    unittest.main()
