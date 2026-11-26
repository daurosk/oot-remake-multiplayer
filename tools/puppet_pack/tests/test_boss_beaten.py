"""Tests for puppet_pack.boss_beaten (G2). No romfs here: the walk's rules on made-up actions, and the committed
coop_boss_beaten.inc and the tool's lists checked against the C++ they mirror."""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))

from puppet_pack import boss_beaten as bb  # noqa: E402
from puppet_pack import evfl  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
HOOK = os.path.join(REPO, "lichii", "hook")


def read(name):
    with open(os.path.join(HOOK, name), encoding="utf-8") as f:
        return f.read()


GOHMA = bb.BOSSES[0]
STEPS = ["NotReady", "Ready", "GoInside", "MeetBoss", "GoToBossRoom", "DefeatBoss", "GoToWarpPoint", "Complete"]
QUESTS = {"Get_StoneOfForest": (0x7d776b61, STEPS, [5])}


def saved(*names, reset=bb.SAVED):
    return {bb.murmur3(n): reset for n in names}


def act(action, **params):
    return ("Actor", action, params)  # as evfl.actions_of gives them


class Murmur(unittest.TestCase):
    def test_known_hashes(self):
        # coop_dungeon.cpp's table and the game's GameDataList
        self.assertEqual(bb.murmur3("InsideTheDekuTree_Heart_Container_Obtained"), 0x98ca7aab)
        self.assertEqual(bb.murmur3("ForestTemple_SageOfForest_BossAppearance"), 0xd99e718a)


class BeatenSet(unittest.TestCase):
    def actions(self):
        return [
            act("EventTriggerSetGameDataBool", GameDataName="Gohma_DefeatBoss", Value=True, Index=-1),
            act("EventTriggerSetGameDataBool", GameDataName="Gohma_Temporary", Value=True, Index=-1),
            act("EventTriggerSetGameDataBool", GameDataName="InsideTheDekuTree_BossArea_ShutIn", Value=False),
            act("EventTransition", EventName="Dm_Deku_BossOut"),
            act("EventExecuteProgressChallengeStep", ChallengeName="Get_StoneOfForest", StepName="GoToWarpPoint"),
        ]

    def flags(self):
        f = saved("Gohma_DefeatBoss", "InsideTheDekuTree_BossArea_ShutIn", "HavePlayedEvent.InsideTheDekuTree_BossDie",
                  "HavePlayedEvent.Dm_Deku_BossIn", "HavePlayedEvent.Dm_Deku_BossOut")
        f.update(saved("Gohma_Temporary", reset=1))
        return f

    def test_saved_writes_played_events_and_the_step(self):
        s = bb.beaten_set(GOHMA, self.actions(), self.flags(), QUESTS)
        self.assertEqual(s["bools"], [("Gohma_DefeatBoss", True), ("HavePlayedEvent.InsideTheDekuTree_BossDie", True),
                                      ("HavePlayedEvent.Dm_Deku_BossIn", True),
                                      ("HavePlayedEvent.Dm_Deku_BossOut", True)])
        self.assertEqual((s["quest"], s["step"]), ("Get_StoneOfForest", "GoToWarpPoint"))
        self.assertEqual(s["below"], STEPS[:6])  # never Complete: a finished dungeon is not stepped back

    def test_no_boss_out_or_no_step_or_a_wrong_step_is_an_error(self):
        acts = [a for a in self.actions() if a[1] != "EventTransition"]
        with self.assertRaises(ValueError):
            bb.beaten_set(GOHMA, acts, self.flags(), QUESTS)
        acts = [a for a in self.actions() if a[1] != "EventExecuteProgressChallengeStep"]
        with self.assertRaises(ValueError):
            bb.beaten_set(GOHMA, acts, self.flags(), QUESTS)
        acts = self.actions()[:-1] + [act("EventExecuteProgressChallengeStep", ChallengeName="Get_StoneOfForest",
                                          StepName="DefeatBoss")]
        with self.assertRaises(ValueError):
            bb.beaten_set(GOHMA, acts, self.flags(), QUESTS)

    def test_a_step_after_an_earlier_boss_step(self):
        # Defeat_Ganon: Ganondorf's death moves it to Step06 (after the boss step Step05), Ganon's to Complete (after
        # Step09); Step07 follows no boss step.
        steps = ["NotReady", "Ready", "Step01", "Step02", "Step03", "Step04", "Step05", "Step06", "Step07", "Step08",
                 "Step09", "Complete"]
        quests = {"Defeat_Ganon": (0x1234, steps, [6, 9, 10])}
        boss = (8, "Ganondorf", "Die_1", "Dm_In", "Dm_Out")

        def death(step):
            return [act("EventTransition", EventName="Dm_Out"),
                    act("EventExecuteProgressChallengeStep", ChallengeName="Defeat_Ganon", StepName=step)]

        flags = saved("HavePlayedEvent.Die_1", "HavePlayedEvent.Dm_In", "HavePlayedEvent.Dm_Out")
        s = bb.beaten_set(boss, death("Step06"), flags, quests)
        self.assertEqual((s["step"], s["below"]), ("Step06", steps[:7]))
        self.assertEqual(bb.beaten_set(boss, death("Complete"), flags, quests)["below"], steps[:11])
        with self.assertRaises(ValueError):
            bb.beaten_set(boss, death("Step07"), flags, quests)

    def test_normal_world_answers(self):
        self.assertEqual(bb.normal_world("GameSystemActor", "EventQueryIsDreamWorld", {}), 1)
        self.assertEqual(bb.normal_world("GameDataActor", "EventQueryIsGameDataBool",
                                         {"GameDataName": "DreamWorld_BossRush"}), 1)
        self.assertIsNone(bb.normal_world("Player_Event", "EventQueryPlayerIsChild", {}))


class ReaderAnswers(unittest.TestCase):
    """evfl.actions_of with `answer`: only the answered case of a switch (the dungeon's path of a boss's death)."""

    def chart(self):
        def ev(kind, nxt=evfl.NONE, actor=evfl.NONE, action=evfl.NONE, cases=(), values=(), query=evfl.NONE):
            return evfl.Event(kind, nxt, actor, action, cases, (), evfl.NONE, "", "", {}, query, values)
        actors = [("GameSystemActor", "", ["Rush", "Dungeon"])]
        return evfl.Flowchart("Death", actors, [
            ev(1, actor=0, query=0, cases=(1, 2), values=(0, 1)),  # IsDreamWorld: 0 is true
            ev(0, actor=0, action=0),
            ev(0, actor=0, action=1),
        ], {"Win": 0}, [["EventQueryIsDreamWorld"]])

    def actions(self, answer):
        return [a[1] for a in evfl.actions_of({"Death": self.chart()}, "Death", "Win", answer=answer)]

    def test_answered_case_only(self):
        self.assertEqual(self.actions(bb.normal_world), ["Dungeon"])

    def test_no_answer_or_an_unknown_value_takes_every_case(self):
        self.assertEqual(sorted(self.actions(None)), ["Dungeon", "Rush"])
        self.assertEqual(sorted(self.actions(lambda *_: 7)), ["Dungeon", "Rush"])


class AgainstTheCode(unittest.TestCase):
    def test_bosses_match_coop_boss_scenes(self):
        text = read("coop_boss_scenes.cpp")
        for room, name, death, boss_in, boss_out in bb.BOSSES:
            m = re.search(r'\{"' + re.escape(name) + r'", "(\w+)", "(\w+)", (-?\d+), \{[^}]*\},\s*\{"(\w+)"', text)
            self.assertIsNotNone(m, name)
            self.assertEqual(m.groups(), (boss_in, boss_out, str(room if room < 8 else -1), death), name)
        self.assertEqual([b[0] for b in bb.BOSSES], list(range(10)))  # BOSS_SCENE's boss index, kBosses' order

    def test_not_shared_matches_coop_dungeon(self):
        text = read("coop_dungeon.cpp")
        block = text[text.index("kNotShared{"):]
        names = set(re.findall(r'"(\w+)"', block[:block.index("};")]))
        self.assertEqual(names, bb.NOT_SHARED)

    def test_the_committed_table_hashes_its_names(self):
        text = read("coop_boss_beaten.inc")
        rows = re.findall(r"\{0x([0-9a-f]{8})u, (?:true|false), \"([\w.]+)\"\}", text)
        self.assertGreater(len(rows), 8 * 3)
        for h, name in rows:
            self.assertEqual(int(h, 16), bb.murmur3(name), name)
        sets = re.findall(r"\{(\d), \"[^\"]+\", 0x[0-9a-f]{8}u, \"\w+\", 0x([0-9a-f]{8})u, \"(\w+)\", kBelow\1, "
                          r"kBools\1\}", text)
        self.assertEqual([int(r) for r, _, _ in sets], list(range(10)))
        for _, h, step in sets:
            self.assertEqual(int(h, 16), bb.murmur3(step), step)


if __name__ == "__main__":
    unittest.main()
