"""Tests for room_objects.py (which moving room objects their own logic drives). Synthetic data only.

    python tools/puppet_pack/tests/test_room_objects.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import room_objects as ro  # noqa: E402


def node(**inputs):
    """A logic node whose bool inputs are name -> source node (-1: a constant)."""
    empty = {t: [] for t in ("int", "bool", "float", "string", "vec3", "udt")}
    n = {"type": "ActorLogicX", "imm": {}, "in": dict(empty), "out": dict(empty)}
    n["in"]["bool"] = [(name, src, 0) for name, src in inputs.items()]
    return n


class RoomObjectsTest(unittest.TestCase):
    def test_platform_names_as_coop_platforms(self):
        self.assertTrue(ro.is_driver("GroundControlActor_WaveLoop"))
        self.assertTrue(ro.is_driver("GrouondControlActor_DiffentSpeedMoveTo"))
        self.assertTrue(ro.is_driver("Obj_InsideTheDekuTree_ElevatorLift_A_01"))
        self.assertTrue(ro.is_driver("Obj_ForestTemple_Elevator_A_01"))
        self.assertFalse(ro.is_driver("Npc_ElevatorLift_A_01"))
        self.assertFalse(ro.is_driver("Obj_MovableBox_A_01"))

    def test_wired_inputs_leave_out_constants_and_resets(self):
        n = node(IsOn=4, Logic_Create=2, RespawnTrigger=3, ForceWarp=5, IisForceWarp=6, Move=-1)
        self.assertEqual(ro.wired_inputs(n), {"IsOn"})
        self.assertEqual(ro.wired_inputs(node(IsInPlayer=-100)), {"IsInPlayer"})  # the multi-input list
        self.assertEqual(ro.wired_inputs(node(Move=-1)), set())

    def test_delivery_kinds(self):
        place = (17.2, 112.3, -23.0)
        step = ("actor", "DodongoCavern", "Obj_StepSwitch_A_01", place, "OnState", "ActorLogicX")
        self.assertIn("SWITCH_STATE", ro.leaf_kind(step, frozenset()))
        eye = {("DodongoCavern", "Obj_StepSwitch_A_01", place, "OnState")}
        self.assertIn("CoopEyeSwitch", ro.leaf_kind(step, eye))
        asker = ("actor", "X", "EventRequestActor", place, "EventBool", "ActorLogicEventRequestActor")
        self.assertIn("asker", ro.leaf_kind(asker, frozenset()))
        self.assertIn("flag", ro.leaf_kind(("node", "HoldGameDataGetBoolSimple", {}), frozenset()))
        self.assertIn("contact", ro.leaf_kind(("node", "HoldActorObserverBySensorHitInfo", {}), frozenset()))
        self.assertIn("blackboard", ro.leaf_kind(("node", "HoldBlackboardGetBool", {}), frozenset()))

    def test_riders_by_logic_not_by_name(self):
        n = node()
        n["out"]["bool"] = ["Dead", "Moving", "RideOnPlayer"]
        self.assertFalse(ro.is_driver("Obj_FireTemple_RiseFloor_A_02"))  # no platform name part
        self.assertTrue(ro.is_rider(n, "Obj_FireTemple_RiseFloor_A_02"))
        self.assertTrue(ro.is_rider(n, "Obj_ForestTemple_Elevator_A_01"))
        self.assertFalse(ro.is_rider(n, "Npc_Something"))
        self.assertFalse(ro.is_rider(node(), "Obj_InsideTheDekuTree_ElevatorLift_A_01"))  # OnPlayer only

    def test_placement_groups(self):
        def actor(name, x, guid=None):
            a = {"Gyaml": name, "Hash": 1, "Translate": [x, 2.0, 3.0]}
            if guid is not None:
                a["Phive"] = {"Placement": {"GroupGUID": guid, "ID": 9}}
            return a
        actors = [actor("GroundControlActor_ResidentMoveTo", 1.0, 70), actor("Obj_WaterPlaneDynamic_A_01", 5.0, 70),
                  actor("FldObj_AirWall_DedicatedEnemy", 6.0, 70), actor("FldObj_AirWall_DedicatedEnemy", 7.0, 70),
                  actor("GroundControlActor_WaveLoop", 8.0, 71), actor("Obj_Deku_MoveFloor_A_01", 9.0, 71),
                  actor("Obj_Pot_A_01", 10.0)]
        table = [{"PlgGuid": 70, "GameDataPath": "Work/GameData/Entity/Flag_A.gmdentity.json?Flag_A",
                  "GameDataType": "Bool"}]
        got = ro.groups_of(actors, table)
        self.assertEqual(len(got), 1)  # the wave's group has no flag: its own driver moves it
        flag, kind, place, names = got[0]
        self.assertEqual((flag, kind), ("Flag_A", "Bool"))
        self.assertEqual(place, (1.0, 2.0, 3.0))  # the controller's
        self.assertEqual(names, ("FldObj_AirWall_DedicatedEnemy", "FldObj_AirWall_DedicatedEnemy",
                                 "Obj_WaterPlaneDynamic_A_01"))
        self.assertEqual(ro.groups_of(actors, []), [])

    def test_shared_names_reads_the_flag_list_only(self):
        import tempfile
        text = ('constexpr std::array kFlags = std::to_array<Flag>({\n'
                '    {0x00000001, false, "A_Flag"},\n    {0x00000002, true, "An_Int"},\n});\n'
                'constexpr std::array kKeys = std::to_array<Key>({\n    {0x00000003, false, "Not_A_Flag"},\n});\n')
        with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False, encoding="utf-8") as f:
            f.write(text)
        try:
            self.assertEqual(ro.shared_names(f.name), {"A_Flag", "An_Int"})
        finally:
            os.unlink(f.name)

    def test_eye_switch_lines_parse(self):
        import tempfile
        line = ('    {"Ev", "DodongoCavern", "Obj_StepSwitch_A_01", {17.2f, 112.3f, -23.0f}, "FirstOn", '
                '{19.0f, 118.0f, -23.0f}},\n')
        with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False, encoding="utf-8") as f:
            f.write(line)
        try:
            self.assertEqual(ro.eye_switch_lines(f.name),
                             {("DodongoCavern", "Obj_StepSwitch_A_01", (17.2, 112.3, -23.0), "FirstOn")})
        finally:
            os.unlink(f.name)

    def test_eye_switch_lines_only_the_driven_ones(self):
        # g3-objects: a step/hit/sun switch's OnState is not driven by CoopEyeSwitch (its AI rewrites it), so it is no
        # longer left out of the switch plugs as covered; an eye's OnState, a FirstOn plug and a kInputs line are.
        import tempfile
        text = ('constexpr Switch kSwitches[] = {\n'
                '    {"E1", "S", "Obj_EyeSwitch_A_02", {1.0f, 0.0f, 0.0f}, "OnState", {0.0f, 0.0f, 0.0f}},\n'
                '    {"E2", "S", "Obj_StepSwitch_A_01", {2.0f, 0.0f, 0.0f}, "FirstOn", {0.0f, 0.0f, 0.0f}},\n'
                '    {"E3", "S", "Obj_StepSwitch_A_03", {3.0f, 0.0f, 0.0f}, "OnState", {0.0f, 0.0f, 0.0f}},\n'
                '    {"E4", "S", "Obj_StepSwitch_A_04", {4.0f, 0.0f, 0.0f}, "OnState", {0.0f, 0.0f, 0.0f}},\n'
                '};\n'
                'constexpr Input kInputs[] = {\n'
                '    {"E4", "S", "Obj_StepSwitch_A_04", {4.0f, 0.0f, 0.0f}, "FirstOn"},\n'
                '};\n')
        with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False, encoding="utf-8") as f:
            f.write(text)
        try:
            driven = ro.eye_switch_lines(f.name)
            self.assertEqual({line[1] for line in driven},
                             {"Obj_EyeSwitch_A_02", "Obj_StepSwitch_A_01", "Obj_StepSwitch_A_04"})
            every = ro.eye_switch_lines(f.name, driven_only=False)
            self.assertEqual(every - driven, {("S", "Obj_StepSwitch_A_03", (3.0, 0.0, 0.0), "OnState")})
            # an empty generated input table (std::array<Input, 0> kInputs{}) drives nothing extra
            with open(f.name, "w", encoding="utf-8") as g:
                g.write(text[: text.index("constexpr Input")] + "constexpr std::array<Input, 0> kInputs{};\n")
            self.assertEqual({line[1] for line in ro.eye_switch_lines(f.name)},
                             {"Obj_EyeSwitch_A_02", "Obj_StepSwitch_A_01"})
        finally:
            os.unlink(f.name)

    def test_asking_lines_take_every_romfs_asker(self):
        # review-a38-3: an asker with no live place (absent from coop_eye_switch.cpp, Dodongo 17 / Forest 11) still
        # makes its plug an asking one; only the lines CoopEyeSwitch drives are left out
        dodongo = ("FocusOnGimmick_Basic", "DodongoCavern", "Obj_EyeSwitch_A_05", (65.7, 121.8, -23.0), "OnState",
                   (70.4, 127.8, -23.0))
        driven = ("E", "S", "Obj_EyeSwitch_A_01", (1.0, 2.0, 3.0), "OnState", (0.0, 0.0, 0.0))
        wiring = {dodongo: {}, driven: {}}
        self.assertEqual(ro.asking_lines(wiring, {driven[1:5]}),
                         {("DodongoCavern", "Obj_EyeSwitch_A_05", (65.7, 121.8, -23.0), "OnState")})
        self.assertEqual(len(ro.asking_lines(wiring)), 2)

    def test_asking_plug_held_only_when_every_asker_has_a_guard(self):
        # g3-objects: wiring lines are (event, scene, switch, place, plug, asker place)
        key = ("S", "Obj_StepSwitch_A_02", (1.0, 0.0, 0.0), "OnState")

        def line(event, asker):
            return (event, *key, asker)

        def w(guards, on=(), sets=()):
            return {"guards": list(guards), "on": list(on), "results": [("flag", f, "set") for f in sets]}

        shared = {"Done", "Unlocked"}
        # one asker, stopped by a shared flag: held behind it
        self.assertEqual(ro.asking_plug(key, {line("E", (0, 0, 0)): w(["Done"])}, shared), (["Done"], ["E"], []))
        # an asker with only its own (unshared) guard: held behind that (Dodongo 17's NaviHelp_17_0x)
        self.assertEqual(ro.asking_plug(key, {line("E", (0, 0, 0)): w(["Own"])}, shared)[0], ["Own"])
        # a shared guard wins over the asker's own ones (each alone stops it; the shared one arrives from the presser)
        self.assertEqual(ro.asking_plug(key, {line("E", (0, 0, 0)): w(["Own", "Done"])}, shared)[0], ["Done"])
        # two askers (Spirit room 19): one has no guard -> never held, however the other is guarded
        two = {line("E", (0, 0, 0)): w(["Done"]), line("E", (5, 0, 0)): w([])}
        self.assertIsNone(ro.asking_plug(key, two, shared)[0])
        # not held: the native path is the switch's own ON input reading a shared flag the event sets
        hammer = {line("H", (0, 0, 0)): w([], on=[("ForceControl", [("flag", "Unlocked", False)]),
                                                   ("BootUp", [("flag", "Unlocked", False)])], sets=["Unlocked"])}
        self.assertEqual(ro.asking_plug(key, hammer, shared),
                         (None, ["H"], [("BootUp", "Unlocked"), ("ForceControl", "Unlocked")]))
        # a flag the event does not set, or a negated read, is no native path (OPEN)
        other = {line("H", (0, 0, 0)): w([], on=[("ForceControl", [("flag", "Unlocked", True)])], sets=["Unlocked"])}
        self.assertEqual(ro.asking_plug(key, other, shared)[2], [])
        # objects-next-a-6: a load guard keeps its mark; it counts as shared by its flag's name (Deku Tree room D)
        self.assertEqual(ro.asking_plug(key, {line("E", (0, 0, 0)): w(["Own", "load:Done"])}, shared)[0],
                         ["load:Done"])
        self.assertEqual(ro.asking_plug(key, {line("E", (0, 0, 0)): w(["load:Own"])}, shared)[0], ["load:Own"])
        # no line at all for the plug: nothing to hold
        self.assertEqual(ro.asking_plug(key, {}, shared), (None, [], []))


if __name__ == "__main__":
    unittest.main()
