"""Tests for eye_switches.classify (deku-ladder, A #39). Synthetic wiring only: no game files.

    python tools/puppet_pack/tests/test_eye_switches.py   (CI runs it through test_puppet_pack.py)
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import eye_switches  # noqa: E402

LINE = ("Ev", "Scene", "Obj_StepSwitch_A_01", (0.0, 0.0, 0.0), "OnState", (1.0, 0.0, 0.0))
SHARED = {"Room_Done", "Room_Other"}


def wire(on=(), results=(), delete=()):
    return {"on": list(on), "results": list(results), "delete": list(delete)}


def asker_or(flag, neg=False):
    """An actor input that ORs the asker's output with `flag` (same polarity)."""
    return (("EventRequestActor", neg, True), (flag, neg, True))


class ClassifyTest(unittest.TestCase):
    def classify(self, w):
        return eye_switches.classify(LINE, w, SHARED)

    def test_shared_flags_only_is_a(self):
        w = wire(on=[("BootUp", [("flag", "Room_Done", False)])],
                 results=[("flag", "Room_Done", "set"), ("flag", "Room_NaviHint_01", "set")],
                 delete=["Room_Done"])
        self.assertEqual(self.classify(w)[0], "A")

    def test_actor_or_shared_flag_is_a_and_and_is_not(self):
        covered = ("actor", "Obj_Shutter", ("Open", asker_or("Room_Done")))
        self.assertEqual(self.classify(wire(results=[covered]))[0], "A")
        # a False output (negated) on both sides still covers
        negated = ("actor", "Obj_Shutter", ("Lock", asker_or("Room_Done", True)))
        self.assertEqual(self.classify(wire(results=[negated]))[0], "A")
        # an AND with the flag (not an OR) does not: the actor waits for our asker
        anded = ("actor", "Obj_Fire", ("StartBurnSign", (("EventRequestActor", False, False), ("Room_Done", False, False))))
        self.assertEqual(self.classify(wire(results=[anded]))[:2], ("B", "none"))
        # an unshared flag does not either
        own = ("actor", "Obj_Shutter", ("Open", asker_or("Room_Own")))
        self.assertEqual(self.classify(wire(results=[own]))[0], "B")

    def test_the_events_moment_only_is_a(self):
        w = wire(results=[
            ("actor", "Obj_TBox_A_01_NoBake", ("isSpawnAct", (("EventRequestActor", False, False),))),
            ("actor", "PushAirWall_Box", ("BootUp", (("EventRequestActor", False, False),))),
            ("actor", "Area", ("Logic_Delete", (("EventRequestActor", False, False),))),
            ("module", "Gimmick_TrapTreasureBox_Set.logic.module", ("IsSpawnAct", ())),
            ("node", "TriggerRequestAutoSave", "IsDrive"),
            ("node", "TriggerDungeonRoomStepForward", "IsDrive"),
        ])
        self.assertEqual(self.classify(w)[0], "A")

    def test_unshared_flag_or_node_is_b(self):
        self.assertEqual(self.classify(wire(results=[("flag", "Gimmick_Water", "set")]))[0], "B")
        self.assertEqual(self.classify(wire(results=[("flag", "Gimmick_Water", "clear")]))[0], "A")
        self.assertEqual(self.classify(wire(results=[("node", "OperateGameDataAllocAutogenGameData", "IsWrite")]))[0], "B")

    def test_b_fixes(self):
        miss = [("flag", "Gimmick_Water", "set")]
        # an unwired forcing input: set it (BootUp and Active are not ON inputs to force)
        w = wire(on=[("BootUp", []), ("FirstOn", [])], results=miss)
        self.assertEqual(self.classify(w)[:2], ("B", "input FirstOn"))
        self.assertEqual(self.classify(wire(on=[("BootUp", []), ("Active", [])], results=miss))[:2], ("B", "none"))
        # the ON input reads a shared flag that does not delete the asker: hold only the delete flag
        w = wire(on=[("ForceControl", [("flag", "Room_Done", False)])], results=miss, delete=["Room_Other"])
        self.assertEqual(self.classify(w)[:2], ("B", "split"))
        # ... the same flag deletes the asker: no way
        w = wire(on=[("ForceControl", [("flag", "Room_Done", False)])], results=miss, delete=["Room_Done"])
        self.assertEqual(self.classify(w)[:2], ("B", "none"))

    def test_the_switch_itself_covers_when_its_on_input_is_a_shared_flag(self):
        reads_switch = ("actor", "Obj_Lift", ("IsOn", (("EventRequestActor", False, True), (LINE[2], False, True))))
        self.assertEqual(self.classify(wire(on=[("BootUp", [("flag", "Room_Done", False)])], results=[reads_switch]))[0], "A")
        self.assertEqual(self.classify(wire(on=[("BootUp", [])], results=[reads_switch]))[0], "B")

    def test_a_shared_flag_the_asker_drives_the_same_way_covers(self):
        # g3-objects, Jabu room 14 upper: the switch re-arms from `asker OR not Updown`, and the event clears Updown
        rearm = ("actor", "Obj_Switch", ("BootUp", (("EventRequestActor", False, True), ("Room_Done", True, True))))
        self.assertEqual(self.classify(wire(results=[rearm, ("flag", "Room_Done", "clear")]))[0], "A")
        # ... the event sets it instead (the OR's negated term goes false): not covered
        self.assertEqual(self.classify(wire(results=[rearm, ("flag", "Room_Done", "set")]))[0], "B")
        # ... an unshared flag driven the same way does not cover either
        own = ("actor", "Obj_Switch", ("BootUp", (("EventRequestActor", False, True), ("Room_Own", True, True))))
        self.assertEqual(self.classify(wire(results=[own, ("flag", "Room_Own", "clear")]))[0], "B")

    def test_no_drive_of_an_asker_that_writes_a_shared_two_way_flag(self):
        # g3-objects: our asker run late would write the flag again and could undo the other player's newer write
        miss = [("flag", "Gimmick_Water", "set"), ("flag", "Room_Done", "clear")]
        on = [("BootUp", []), ("FirstOn", [])]
        self.assertEqual(eye_switches.classify(LINE, wire(on=on, results=miss), SHARED)[:2], ("B", "input FirstOn"))
        cls, fix, why = eye_switches.classify(LINE, wire(on=on, results=miss), SHARED, {"Room_Done"})
        self.assertEqual((cls, fix), ("B", "none"))
        self.assertIn("two-way Room_Done", why)
        # a two-way flag that is not shared is each game's own: our asker may still run
        self.assertEqual(eye_switches.classify(LINE, wire(on=on, results=miss), SHARED, {"Gimmick_Water"})[:2],
                         ("B", "input FirstOn"))

    def test_two_way_flags(self):
        wiring = {1: wire(results=[("flag", "W", "set"), ("flag", "X", "set")]),
                  2: wire(results=[("flag", "W", "clear"), ("actor", "X", ("BootUp", ()))])}
        self.assertEqual(eye_switches.two_way_flags(wiring), {"W"})

    def test_asker_guards(self):
        # g3-objects: the saved flags that stop ONE asker: (kind, what, plug, negated, under an OR)
        sw = ("actor", {"Gyaml": "Obj_StepSwitch_A_01"}, "OnState", False, False)
        # negated outside an OR, and a delete flag; a positive trigger flag and HavePlayedEvent.* are not guards
        trig = [sw, ("flag", "Done", "Bool", True, False), ("flag", "Lit", "Bool", False, False)]
        dele = [("flag", "Gone", "Bool", False, False), ("flag", "HavePlayedEvent.Ev", "Bool", False, False)]
        self.assertEqual(eye_switches.asker_guards(trig, dele), ["Done", "Gone"])
        # Jabu-Jabu room 1: the negated flag is the trigger's one leaf under an OR (the OR's other inputs unwired)
        self.assertEqual(eye_switches.asker_guards([sw, ("flag", "First", "Bool", True, True)], []), ["First"])
        # two leaves under ORs: which OR holds which is unknown, none counts (no hold)
        self.assertEqual(eye_switches.asker_guards(
            [sw, ("flag", "A", "Bool", True, True), ("flag", "B", "Bool", False, True)], []), [])
        # a negated delete flag deletes nothing once on
        self.assertEqual(eye_switches.asker_guards([sw], [("flag", "Gone", "Bool", True, False)]), [])
        # g1-events 9fce0c8f: under an OR AND an AND (2) the negated flag is not alone: no guard
        self.assertEqual(eye_switches.asker_guards([sw, ("flag", "First", "Bool", True, 2)], []), [])

    def test_asker_guards_read_once_at_load_are_load_guards(self):
        # objects-next-a-6: a flag behind a gate with UpdateOnce 1 ("flag-once") stops the asker only if it was on at
        # the room's load: a LOAD guard, marked "load:" (the Deku Tree room D compass asker, RoomD_StepSwitch_On_01)
        sw = ("actor", {"Gyaml": "Obj_StepSwitch_A_02"}, "OnState", False, False)
        dele = [("flag-once", "RoomD_On", "Bool", False, False)]
        self.assertEqual(eye_switches.asker_guards([sw], dele), ["load:RoomD_On"])
        self.assertEqual(eye_switches.load_guard("load:RoomD_On"), ("RoomD_On", True))
        self.assertEqual(eye_switches.load_guard("RoomD_On"), ("RoomD_On", False))
        # a negated trigger flag read once: the same (the trigger is false for the visit only if it was on at load)
        self.assertEqual(eye_switches.asker_guards([sw, ("flag-once", "Done", "Bool", True, False)], []),
                         ["load:Done"])
        # negated once at load deletes nothing; HavePlayedEvent.* stays out as for a live flag
        self.assertEqual(eye_switches.asker_guards([sw], [("flag-once", "Gone", "Bool", True, False),
                                                          ("flag-once", "HavePlayedEvent.Ev", "Bool", False, False)]), [])
        # the same flag read live too: the live read stops the asker at any time, so it is a live guard only
        self.assertEqual(eye_switches.asker_guards([sw], dele + [("flag", "RoomD_On", "Bool", False, False)]),
                         ["RoomD_On"])
        # a live and a load guard of different flags: both, each marked
        self.assertEqual(eye_switches.asker_guards([sw, ("flag", "Done", "Bool", True, False)], dele),
                         ["Done", "load:RoomD_On"])


class PerformerPlaysTest(unittest.TestCase):
    """Row 165: an event's EventPlayAS on a room actor whose Logic_Delete reads the event's played state."""

    PERFORMERS = {("Cave", "Ev"): {("Obj_Stair", (1.0, 2.0, 3.0))}, ("Other", "Ev"): {("Obj_Stair", (0.0, 0.0, 0.0))}}
    PLAYS = {"Ev": {("Obj_Stair", "Stair_Demo_Anim"), ("Player_Event", "DemoWait"), ("Obj_Shutter", "Open")}}

    def test_only_a_performer_the_played_state_deletes(self):
        got = eye_switches.performer_plays("Ev", "Cave", self.PERFORMERS, self.PLAYS)
        self.assertEqual(got, [("Obj_Stair", (1.0, 2.0, 3.0), "Stair_Demo_Anim")])
        self.assertEqual(eye_switches.performer_plays("Ev2", "Cave", self.PERFORMERS, self.PLAYS), [])
        self.assertEqual(eye_switches.performer_plays("Ev", "Nowhere", self.PERFORMERS, self.PLAYS), [])

    def test_plays_make_a_live_asker_class_asker(self):
        key = ("Ev", "Cave", (0.0, 0.0, 0.0))
        w = {"results": [("flag", "Room_Done", "set")], "delete": ["Room_Done"]}
        self.assertEqual(eye_switches.asker_class(key, w, SHARED)[0], "live")
        plays = eye_switches.performer_plays("Ev", "Cave", self.PERFORMERS, self.PLAYS)
        cls, why = eye_switches.asker_class(key, w, SHARED, plays)
        self.assertEqual(cls, "asker")
        self.assertIn("Obj_Stair plays Stair_Demo_Anim", why)


if __name__ == "__main__":
    unittest.main()
