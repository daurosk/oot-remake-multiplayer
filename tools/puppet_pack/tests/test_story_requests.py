"""Tests for story_requests.py (which placed event request starts a shared story cutscene). Synthetic data only.

    python tools/puppet_pack/tests/test_story_requests.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import evfl  # noqa: E402
from puppet_pack import story_requests as sr  # noqa: E402


def setting(name, flow, entry="", base="StarterPos", comp="FlowchartEvent"):
    return {"__RowId": name, "FlowchartFileName": f"Work/Event/EventFlow/{flow}.evfc", "EntryPointName": entry,
            "EventBaseSetting": f"Work/Event/EventSetting/X/{base}.game__event__GameEventBaseSetting.gyml",
            "EventSettingComponentList": f"Work/Event/EventSetting/Y/{comp}.engine__event__EventSettingComponentList.gyml"}


class StoryRequestsTest(unittest.TestCase):
    def test_story_names_from_the_cpp_table(self):
        text = 'constexpr std::string_view kNames[] = {\n    "Dm_Kokiri_0070", "Meet_DekuTree",\n};\n'
        self.assertEqual(sr.story_names(text), {"Dm_Kokiri_0070", "Meet_DekuTree"})
        with self.assertRaises(ValueError):
            sr.story_names("nothing here")

    def test_every_named_setting_of_a_story_flow_is_classified(self):
        settings = sr.event_settings([
            setting("KokiriForest_Meet_DekuTree_MeetDekuTree_01", "Meet_DekuTree"),
            setting("Dm_Kokiri_0070", "Dm_Kokiri_0070"),                           # a story name: not in the table
            setting("Dm_FireTemple_0020", "Dm_FireTemple_0020"),
            setting("Get_StoneOfWater_ShowLetter", "Get_StoneOfWater", "StoneOfWater_ShowLetter"),
            setting("Dm_FireTemple_0015", "Dm_FireTemple_0020", "EntryPoint1"),    # an entry of a Dm_ cutscene
            setting("Get_StoneOfWater_DestroyBoss", "Get_StoneOfWater", comp="BossDie"),
            setting("Get_StoneOfWater_TalkWithImpa", "Get_StoneOfWater"),          # runs nothing of the player's
            setting("Get_StoneOfFire_AppearBoss", "Get_StoneOfFire"),
            setting("StoneOfWater_ElevatedFloor", "Get_StoneOfWater"),
            setting("Meet_Darunia_PlayOcarina", "Get_StoneOfFire", comp="NPCTalk"),
            setting("Talk_Deku_01_Attacked", "Get_StoneOfForest", "Talk_Deku_01_Attacked"),
            setting("Get_StoneOfWater_ArriveJabuJabuBelly", "Get_StoneOfWater", base="PlayerPos"),
            setting("Get_StoneOfForest_GetSword", "Get_StoneOfForest", "GetSword"),
            setting("Get_StoneOfWater_RutoGetStone", "Get_StoneOfWater", "Get_StoneOfWater_RutoGetStone"),
            setting("Check_DaruniaDoor", "Get_StoneOfFire"),
            setting("Room_Gimmick_Open", "Room_Gimmick"),                           # not a story flow
            setting("3838149862_1053658113_1523149261_D_T", "Get_StoneOfFire"),    # a talk row id
        ])
        self.assertEqual(settings["Meet_Darunia_PlayOcarina"].components, "NPCTalk")
        self.assertEqual(settings["Get_StoneOfWater_ArriveJabuJabuBelly"].base, "PlayerPos")
        story = {"Meet_DekuTree", "Get_StoneOfFire", "Get_StoneOfWater", "Get_StoneOfForest", "Dm_Kokiri_0070",
                 "Dm_FireTemple_0020", "Meet_GreatFairy"}
        runs = {  # what each entry runs (evfl.actions_of), as the game's flows have it
            "StoneOfWater_ShowLetter": [("Player_Event", "EventCreateShowItemBottle", {}),
                                        ("Npc_Zora_KingZora_child", "EventTalk", {})],
            "GetSword": [("EventSystemActor", "EventTransition", {"EventName": "Dm_SY_Treasurebox"}),
                         ("PouchSystemActor", "EventTriggerSetForGetWindow", {})],
            "EntryPoint1": [("Player_Event", "EventTriggerPlayerRequestCreateDynamicEquipmentNoRelationPouch", {}),
                            ("Player_Event", "EventDeleteShowItem", {})],  # a model and a put-away: not own
            "Get_StoneOfWater_RutoGetStone": [("PouchSystemActor", "EventTriggerSetForGetWindow", {})],  # shared
        }
        rows = {r[0]: r[2] for r in sr.table(settings, story, {"StoneOfWater_ElevatedFloor"},
                                             lambda flow, entry: runs.get(entry, []))}
        self.assertEqual(rows, {
            "KokiriForest_Meet_DekuTree_MeetDekuTree_01": "story",
            "Get_StoneOfWater_TalkWithImpa": "story",
            "Get_StoneOfWater_ShowLetter": "own",
            "Dm_FireTemple_0015": "story",
            "Get_StoneOfWater_RutoGetStone": "story",
            "Get_StoneOfWater_DestroyBoss": "boss",
            "Get_StoneOfFire_AppearBoss": "boss",
            "StoneOfWater_ElevatedFloor": "gimmick",
            "Meet_Darunia_PlayOcarina": "own",
            "Talk_Deku_01_Attacked": "own",
            "Get_StoneOfWater_ArriveJabuJabuBelly": "own",
            "Get_StoneOfForest_GetSword": "own",
            "Check_DaruniaDoor": "never",
        })
        self.assertEqual(sr.no_setting(story, settings),
                         ["Get_StoneOfFire", "Get_StoneOfForest", "Get_StoneOfWater", "Meet_DekuTree",
                          "Meet_GreatFairy"])

    def test_touches_own_only_what_is_the_players_own(self):
        pouch = "EventTriggerAddOrRemovePouchContent"
        # Own: a trade (Num < 0), a consumable added, rupees, a bottle's contents, ocarina input, gear from the
        # pouch, a shop, a chest.
        self.assertIn("Item_AlarmCucco", sr.touches_own([("PouchSystemActor", pouch,
                                                          {"Actor": "Item_AlarmCucco", "Num": -1})]))
        self.assertIn("Item_Arrow", sr.touches_own([("PouchSystemActor", pouch, {"Actor": "Item_Arrow", "Num": 10})]))
        self.assertIn("rupees", sr.touches_own([("PouchSystemActor", "EventTriggerIncreaseRupee", {})]))
        self.assertIn("bottle", sr.touches_own([("Player_Event", "EventCreateShowItemBottle", {})]))
        # A subflow call is no action (G2 review: every item get can reach the get window's text for a bottle,
        # GetPouchContent:OpenGetWindowForBottle_SubProcess; what a subflow runs is listed after it).
        # Every other inventory-changing action of the game's flows (G2 review), and the get's plumbing that is not.
        for action in sr.OWN_ACTIONS:
            self.assertNotEqual(sr.touches_own([("PouchSystemActor", action, {})]), "", action)
        self.assertIn("trade challenge", sr.touches_own([("PouchSystemActor",
                                                         "EventTriggerRemoveSwordBigGoronChallengeActor", {})]))
        self.assertIn("rupees", sr.touches_own([("PouchSystemActor", "EventTriggerAddGameDataIntToRupee", {})]))
        for plumbing in ("EventTriggerAddOwnToPouch", "EventTriggerRecoverByCureItem", "EventTriggerSetForGetWindow"):
            self.assertEqual(sr.touches_own([("PouchSystemActor", plumbing, {})]), "", plumbing)
        call = ("<subflow>", "GetPouchContent:OpenGetWindowForBottle_SubProcess", {})
        self.assertEqual(sr.touches_own([call]), "")
        self.assertIn("ocarina", sr.touches_own([("GameSystemActor", "EventWaitOcarinaInputForSequence", {})]))
        self.assertIn("pouch", sr.touches_own([("Player_Event", "EventTriggerPlayerEquipDynamicEquipmentFromPouch", {})]))
        self.assertIn("shop", sr.touches_own([("Npc_EventStarter", "EventTriggerGetItemAtShop", {})]))
        self.assertIn("chest", sr.touches_own([("EventSystemActor", "EventTransition",
                                                {"EventName": "Dm_SY_Treasurebox"})]))
        # Row 150: a step of a quest whose steps are each player's own (Get_Epona: Dm_LonLon_0040, Ingo beaten);
        # a step of a quest co-op shares or catches up is not.
        self.assertIn("Get_Epona to AfterWonGambleRace", sr.touches_own([(
            "ChallengeSystemActor", "EventExecuteProgressChallengeStep",
            {"ChallengeName": "Get_Epona", "StepName": "AfterWonGambleRace"})]))
        self.assertIn("Get_Epona", sr.touches_own([(
            "ChallengeSystemActor", "EventExecuteRollbackChallengeStep",
            {"ChallengeName": "Get_Epona", "ChallengeStep": "Ready"})]))
        self.assertEqual(sr.touches_own([(
            "ChallengeSystemActor", "EventExecuteProgressChallengeStep",
            {"ChallengeName": "Goto_HyruleCastle", "StepName": "Step04"})]), "")
        # Not own: a +1 of an item co-op shares, a tier, a get window, a shown item, the empty bottle every get
        # passes, rupee UI, hearts, a song subflow by name, a flag.
        self.assertEqual(sr.touches_own([
            ("PouchSystemActor", pouch, {"Actor": "Item_Ocarina_Fairy", "Num": 1}),
            ("PouchSystemActor", pouch, {"Actor": "Item_BulletBag_Large", "Num": 1}),
            ("PouchSystemActor", pouch, {"Num": 1}),
            ("PouchSystemActor", "EventTriggerSetForGetWindow", {}),
            ("PouchSystemActor", "EventTriggerSetHavePlayedGetEvent", {}),
            ("Player_Event", "EventCreateShowItemFromActorFileName", {}),
            ("PouchSystemActor", "EventTriggerPushToEmptyBottle", {}),
            ("UISystemActor", "EventOpenRupeeDisplay", {}),
            ("Player_Event", "EventTriggerHealLifeMax", {}),
            ("<subflow>", "Dm_MelodyGet_Zelda:Main", {}),
            ("GameDataActor", "EventTriggerSetGameDataBool", {"GameDataName": "IsGet.Item_Bottle"}),
        ]), "")

    def test_own_story_names(self):
        settings = sr.event_settings([setting("Dm_HrlCastle_0020", "Dm_HrlCastle_0020"),
                                      setting("Dm_HrlCastle_0050", "Dm_HrlCastle_0050"),
                                      {"__RowId": "Dm_Kokiri_0150"}])  # a movie only: nothing to read
        runs = {"Dm_HrlCastle_0020": [("PouchSystemActor", "EventTriggerAddOrRemovePouchContent",
                                       {"Actor": "Item_AlarmCucco", "Num": -1})],
                "Dm_HrlCastle_0050": [("Player_Event", "EventCreateShowItemFromActorFileName", {}),
                                      ("PouchSystemActor", "EventTriggerSetForGetWindow", {})]}  # a gift: shared
        self.assertEqual(sr.own_story({"Dm_HrlCastle_0020", "Dm_HrlCastle_0050", "Dm_Kokiri_0150"}, settings,
                                      lambda flow, entry: runs.get(flow, [])),
                         [("Dm_HrlCastle_0020", "takes Item_AlarmCucco from the pouch")])

    def test_room_turn_toggles_and_room_actions(self):
        # Dm_ForestTemple_0005 (row 173): asks TwistedCorridor_A_02 and writes it both ways, and turns the corridor.
        ask = (sr.QUERY, "EventQueryIsGameDataBool", {"GameDataName": "A_02"})
        set_a = [("GameDataActor", "EventTriggerSetGameDataBool", {"GameDataName": "A_02", "Value": v})
                 for v in (True, False)]
        self.assertIn("toggles A_02", sr.room_turn([ask] + set_a))
        self.assertIn("EventTriggerRelocateTwistCorridor",
                      sr.room_turn([("GameSystemActor", "EventTriggerRelocateTwistCorridor", {"Is01": True})]))
        # Not a toggle: set both ways but never asked (a mark during the scene), asked but set one way only (a
        # first-time mark), or a level (the Water Temple's WaterLevel ints are set to one value per flow).
        self.assertEqual(sr.room_turn(set_a), "")
        self.assertEqual(sr.room_turn([ask, set_a[0]]), "")
        level = (sr.QUERY, "EventQueryIsGameDataInt", {"GameDataName": "WaterTemple_Room00_WaterLevel"})
        self.assertEqual(sr.room_turn([level, ("GameDataActor", "EventTriggerSetGameDataInt",
                                               {"GameDataName": "WaterTemple_Room00_WaterLevel", "Value": 0})]), "")
        # A question is no action of the player's own (a Bottle query is not a bottle's contents).
        self.assertEqual(sr.touches_own([(sr.QUERY, "EventQueryIsBottleEmpty", {})]), "")

    def test_room_story_names(self):
        settings = sr.event_settings([setting("Dm_ForestTemple_0005", "Dm_ForestTemple_0005"),
                                      setting("Dm_HrlCastle_0020", "Dm_HrlCastle_0020"),
                                      setting("Dm_WaterTemple_0020", "Dm_WaterTemple_0020")])
        turn = ("GameSystemActor", "EventTriggerRelocateTwistCorridor", {})
        runs = {"Dm_ForestTemple_0005": [turn],
                "Dm_HrlCastle_0020": [turn, ("PouchSystemActor", "EventTriggerAddOrRemovePouchContent",
                                             {"Actor": "Item_AlarmCucco", "Num": -1})],  # own first: kOwnStory
                "Dm_WaterTemple_0020": [("GameDataActor", "EventTriggerSetGameDataInt", {"Value": 0})]}
        got = sr.room_story({"Dm_ForestTemple_0005", "Dm_HrlCastle_0020", "Dm_WaterTemple_0020"}, settings,
                            lambda flow, entry: runs.get(flow, []))
        self.assertEqual([n for n, _ in got], ["Dm_ForestTemple_0005"])
        # A setting of a story flow under another name is classified "room" (not shared) the same way.
        s = sr.Setting("Dm_ForestTemple_0005", "", "StarterPos", "TwistCorridorEvent")
        self.assertEqual(sr.classify("Dm_ForestTemple_0005_X", s, set(), [turn])[0], "room")

    def test_gimmick_names_from_the_hook(self):
        dungeon = '{0xfd1a276a, false, "HavePlayedEvent.StoneOfWater_ElevatedFloor"}, {1, true, "Other.Flag"},'
        places = '{"Dm_FireTemple_0029", "FireTemple", -46.9f, 186.5f, -22.4f},'
        self.assertEqual(sr.gimmick_names(dungeon, places), {"StoneOfWater_ElevatedFloor", "Dm_FireTemple_0029"})

    def test_carries_whole_parts(self):
        self.assertTrue(sr.carries("KokiriForest_Meet_DekuTree_MeetDekuTree_01", "Meet_DekuTree"))
        self.assertTrue(sr.carries("Get_StoneOfFire_MeetDarnia", "Get_StoneOfFire"))
        self.assertFalse(sr.carries("Get_StoneOfFireX_Meet", "Get_StoneOfFire"))
        self.assertFalse(sr.carries("GoronCity_SecretHole_AppearTreasureBox", "Get_StoneOfFire"))

    def test_render(self):
        text = sr.render([("KokiriForest_Meet_DekuTree_MeetDekuTree_01", "Meet_DekuTree", "story", ""),
                          ("Get_StoneOfForest_GetSword", "Get_StoneOfForest", "own", "an item each player gets")],
                         ["Meet_GreatFairy"], [("Dm_HrlCastle_0050", "runs X Y")],
                         [("Dm_ForestTemple_0005", "toggles A_02")])
        self.assertIn("constexpr StoryOwn kRoomStory[] = {", text)
        self.assertIn('    {"Dm_ForestTemple_0005", "toggles A_02"},', text)
        self.assertIn('{"KokiriForest_Meet_DekuTree_MeetDekuTree_01", "Meet_DekuTree"},', text)
        self.assertIn("constexpr StorySetting kStorySettings[] = {", text)
        self.assertIn("//   own     Get_StoneOfForest_GetSword (Get_StoneOfForest): an item each player gets", text)
        self.assertIn("constexpr std::string_view kNoSetting[] = {", text)
        self.assertIn('    {"Dm_HrlCastle_0050", "runs X Y"},', text)
        self.assertIn('    "Meet_GreatFairy",', text)

    def test_played_flag_is_the_setting_asked(self):
        # The game's own hash of a flag name (coop_shared_scenes.cpp has HavePlayedEvent.Dm_Kokiri_0140 so).
        self.assertEqual(sr.murmur3("HavePlayedEvent.Dm_Kokiri_0140"), 0xEBAEC07F)
        self.assertEqual(sr.played_flag("Get_StoneOfFire_MeetDarnia"), "HavePlayedEvent.Get_StoneOfFire_MeetDarnia")
        names = ["Get_StoneOfFire_MeetDarnia", "Meet_DekuTree"]
        gamedata = bytes(8) + sr.murmur3(sr.played_flag(names[0])).to_bytes(4, "big") + bytes(8)
        self.assertEqual(sr.missing_played_flags(names, gamedata), ["Meet_DekuTree"])
        self.assertEqual(sr.missing_played_flags(names[:1], gamedata), [])


class EvflTest(unittest.TestCase):
    @staticmethod
    def ev(kind, nxt=evfl.NONE, actor=evfl.NONE, action=evfl.NONE, cases=(), forks=(), join=evfl.NONE,
           flowchart="", entry="", params=None):
        return evfl.Event(kind, nxt, actor, action, cases, forks, join, flowchart, entry, params or {})

    def test_walk_reaches_every_branch_subflow_and_transition(self):
        ev = self.ev
        actors = [("Player_Event", "", ["EventA", "EventB", "EventC"]), ("EventSystemActor", "", ["EventTransition"])]
        main = evfl.Flowchart("Main", actors, [
            ev(1, cases=(1, 2)),                                    # 0: a switch
            ev(0, nxt=3, actor=0, action=0),                        # 1
            ev(2, forks=(4,), join=5),                              # 2: a fork
            ev(4, flowchart="Other", entry="Start"),                # 3: a subflow into another file
            ev(0, actor=0, action=1),                               # 4
            ev(3, nxt=6),                                           # 5: the join
            ev(0, actor=1, action=0, params={"EventName": "Next_Setting"}),  # 6
        ], {"EntryPoint0": 0, "Unused": 6})
        other = evfl.Flowchart("Other", actors, [ev(0, actor=0, action=2)], {"Start": 0})
        flows = {"Main": main}
        got = evfl.actions_of(flows, "Main", "", load=lambda f: other if f == "Other" else None,
                              transition=lambda e: ("Third", "X") if e == "Next_Setting" else None)
        names = {(a, b) for a, b, _ in got}
        self.assertEqual(names, {("Player_Event", "EventA"), ("Player_Event", "EventB"), ("Player_Event", "EventC"),
                                 ("<subflow>", "Other:Start"), ("EventSystemActor", "EventTransition")})
        self.assertIn("Other", flows)                      # loaded once, kept
        self.assertEqual(evfl.actions_of(flows, "Main", "Missing"), [])
        self.assertEqual(evfl.actions_of(flows, "Nowhere", "EntryPoint0"), [])

    def test_parse_refuses_other_files_and_reads_a_timeline_as_empty(self):
        with self.assertRaises(ValueError):
            evfl.parse(b"NOTEVFL" + bytes(0x40))
        timeline = b"BFEVFL\x00\x00" + bytes(0x38)            # 0 flowcharts at 0x20
        self.assertEqual(evfl.parse(timeline), evfl.Flowchart("", [], [], {}))


if __name__ == "__main__":
    unittest.main()
