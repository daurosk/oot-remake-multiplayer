import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import kit_setup as ks  # noqa: E402
import playtest_client as pc  # noqa: E402


class Fake:
    def state(self):
        return {"max_life": 40}

    def __init__(self, fail_on=None):
        self.log, self.fail_on = [], fail_on

    def _do(self, *call):
        if self.fail_on == call[0]:
            raise RuntimeError("refused")
        self.log.append(call)
        return {"ok": True}

    def give(self, item):
        return self._do("give", item)

    def flag(self, name, value=1):
        return self._do("flag", name, value)

    def warp(self, scene, entrance=None, timeout=60):
        return self._do("warp", scene, entrance)

    def act(self, line):
        return self._do("act", line)

    def step(self, frames=1):
        return self._do("step", frames)


class Plan(unittest.TestCase):
    def test_life_is_quarter_hearts_and_the_warp_comes_last(self):
        calls = ks.plan({"warp": {"scene": "GohmaStage", "entrance": "GohmaStage"}, "hearts": 5, "max_hearts": 5})
        self.assertEqual(calls, [("flag", "PlayerStatus.MaxLife", 20), ("flag", "PlayerStatus.Life", 20),
                                 ("warp", "GohmaStage", "GohmaStage")])

    def test_place_comes_after_the_warp_and_is_checked(self):
        calls = ks.plan({"place": {"x": -12, "y": 48.5, "z": 66.5}, "warp": {"scene": "InsideTheDekuTree", "entrance": "Entrance"},
                         "hearts": 3})
        self.assertEqual(calls[-2:], [("warp", "InsideTheDekuTree", "Entrance"), ("place", -12.0, 48.5, 66.5)])
        for bad in ({"x": 1, "y": 2}, {"x": 1, "y": 2, "z": "3"}, {"x": 1, "y": 2, "z": float("nan")},
                    {"x": True, "y": 2, "z": 3}, {"x": 1, "y": 2, "z": 3, "w": 4}, [1, 2, 3], {"x": 1e9, "y": 0, "z": 0}):
            with self.assertRaises(ValueError, msg=bad):
                ks.plan({"place": bad})

    def test_items_first_then_magic_rupees_keys(self):
        calls = ks.plan({"magic": 2, "rupees": 99, "items": ["bow"], "small_keys": {"ForestTemple": 3},
                         "boss_keys": ["JabuJabu"]})
        self.assertEqual(calls, [("give", "bow"), ("give", "Item_FairyBless_MP"), ("give", "Item_FairyBless_MPDouble"),
                                 ("flag", "PlayerStatus.MPMax", 480), ("flag", "PlayerStatus.MP", 480),
                                 ("flag", "PlayerStatus.CurrentRupee", 99),
                                 ("flag", "DungeonInfo.ForestTemple.SmallKeyNum", 3),
                                 ("flag", "DungeonInfo.InsideJabuJabuBelly.IsGetBossKey", 1)])

    def test_no_magic_clears_the_meter(self):
        calls = ks.plan({"magic": 0})
        self.assertIn(("flag", "IsGet.Item_FairyBless_MP", 0), calls)
        self.assertIn(("flag", "PouchData.Item_FairyBless_MPDouble.StockNum", 0), calls)
        self.assertEqual(calls[-2:], [("flag", "PlayerStatus.MPMax", 0), ("flag", "PlayerStatus.MP", 0)])

    def test_single_magic_takes_double_away(self):
        calls = ks.plan({"magic": 1})
        self.assertEqual(calls[0], ("give", "Item_FairyBless_MP"))
        self.assertIn(("flag", "IsGet.Item_FairyBless_MPDouble", 0), calls)
        self.assertEqual(calls[-2:], [("flag", "PlayerStatus.MPMax", 240), ("flag", "PlayerStatus.MP", 240)])

    def test_hearts_above_the_maximum_are_refused(self):
        with self.assertRaises(ValueError):
            ks.plan({"hearts": 5, "max_hearts": 3})
        c = Fake()
        c.state = lambda: {"max_life": 12}
        with self.assertRaises(ValueError):
            ks.apply(c, {"hearts": 5})
        self.assertEqual(c.log, [])
        ks.apply(c, {"hearts": 3})

    def test_a_warp_without_an_entrance(self):
        self.assertEqual(ks.plan({"warp": {"scene": "KokiriForest_Child"}}), [("warp", "KokiriForest_Child", None)])

    def test_bad_specs_are_refused(self):
        for bad in ({"hart": 3}, {"hearts": 0}, {"hearts": True}, {"hearts": 21}, {"magic": 3}, {"rupees": -1},
                    {"small_keys": {"Nowhere": 1}}, {"small_keys": {"ForestTemple": 10}}, {"boss_keys": "ForestTemple"},
                    {"items": ["a b"]}, {"flags": {"x y": 1}}, {"flags": {"X": 1.5}}, {"warp": {"scene": "A", "x": 1}},
                    {"warp": {}}, [], {"max_hearts": 2}):
            with self.assertRaises(ValueError, msg=repr(bad)):
                ks.plan(bad)

    def test_flags_pass_through_in_order(self):
        calls = ks.plan({"flags": {"Gohma_DefeatBoss": 0, "IsGet.Item_Stone_Forest": True}})
        self.assertEqual(calls, [("flag", "Gohma_DefeatBoss", 0), ("flag", "IsGet.Item_Stone_Forest", True)])


class Apply(unittest.TestCase):
    def test_place_is_sent_as_one_place_line_after_the_warp(self):
        c = Fake()
        ks.apply(c, {"warp": {"scene": "InsideTheDekuTree", "entrance": "Entrance"}, "place": {"x": -12, "y": 48.5, "z": 66.5}})
        self.assertEqual(c.log, [("warp", "InsideTheDekuTree", "Entrance"), ("act", "place -12.0 48.5 66.5")])

    def test_runs_every_call_in_order(self):
        c = Fake()
        done = ks.apply(c, {"hearts": 3, "items": ["bombs"], "warp": {"scene": "S"}})
        self.assertEqual(c.log, [("give", "bombs"), ("flag", "PlayerStatus.Life", 12), ("warp", "S", None)])
        self.assertEqual(len(done), 3)

    def test_a_refusal_says_which_step_and_what_was_done(self):
        c = Fake(fail_on="flag")
        with self.assertRaises(RuntimeError) as e:
            ks.apply(c, {"items": ["bombs"], "hearts": 3})
        self.assertIn("step 2 of 2", str(e.exception))
        self.assertIn("PlayerStatus.Life", str(e.exception))

    def test_a_bad_spec_runs_nothing(self):
        c = Fake()
        with self.assertRaises(ValueError):
            ks.apply(c, {"items": ["bombs"], "hearts": 0})
        self.assertEqual(c.log, [])


REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))


def _source_text():
    out = []
    for sub in ("coop/host/src", "lichii/hook", "tools/autotest/scenarios", "docs"):
        for dp, _, fs in os.walk(os.path.join(REPO, sub)):
            for f in fs:
                if f.endswith((".cpp", ".h", ".hpp", ".inc", ".txt", ".md")):
                    with open(os.path.join(dp, f), encoding="utf-8", errors="replace") as fh:
                        out.append(fh.read())
    return chr(10).join(out)


class Presets(unittest.TestCase):
    def test_the_shipped_presets_are_there(self):
        names = set(ks.presets())
        self.assertTrue({"deku-start", "deku-before-gohma", "before-dodongo", "before-barinade", "before-phantom-ganon",
                         "before-volvagia", "before-morpha", "before-bongo-bongo", "before-twinrova",
                         "before-ganondorf", "before-ganon"} <= names, names)

    def test_every_preset_plans_and_every_name_is_one_the_repository_already_uses(self):
        text = _source_text()
        starts = open(os.path.join(REPO, "lichii/hook/coop_start_points.inc"), encoding="utf-8").read()
        for name in ks.presets():
            calls = ks.plan(ks.load_preset(name))
            kinds = [c[0] for c in calls]
            self.assertEqual(kinds.count("warp"), 1, name)
            self.assertTrue(all(k == "place" for k in kinds[kinds.index("warp") + 1:]), name)   # only a place after it
            scene, start = calls[kinds.index("warp")][1], calls[kinds.index("warp")][2]
            self.assertIn('{"%s", "%s"' % (scene, start), starts, name)
            for c in calls:
                if c[0] == "give":
                    self.assertIn(c[1].split(".")[1] if c[1].startswith("PouchData.") else c[1] if "_" in c[1] else "",
                                  text, (name, c))
                elif c[0] == "flag" and not c[1].startswith(("PlayerStatus.", "DungeonInfo.")):
                    self.assertIn(c[1], text, (name, c))

    def test_the_gohma_preset_walks_in_through_the_door_not_the_rush_room(self):
        # A free-play warp into the boss rush's GohmaStage left both games black, with no control and no camera
        # (kit-fight's lane run). The preset goes into the Deku Tree, clears the boss room's and the intro's flags so
        # the door's own trigger runs, and puts Link in the corridor before the door (as gohma-door).
        calls = ks.plan(ks.load_preset("deku-before-gohma"))
        self.assertIn(("warp", "InsideTheDekuTree", "Entrance"), calls)
        self.assertNotIn("GohmaStage", [c[1] for c in calls if c[0] == "warp"])
        self.assertEqual(calls[-1], ("place", -12.0, 48.5, 66.5))
        flags = {c[1]: c[2] for c in calls if c[0] == "flag"}
        for name in ("HavePlayedEvent.InsideTheDekuTree_BossArea_ShutIn", "HavePlayedEvent.Dm_Deku_BossIn",
                     "HavePlayedEvent.Dm_Deku_BossOut", "HavePlayedEvent.InsideTheDekuTree_BossArea_Warp",
                     "InsideTheDekuTree_BossArea_ShutIn", "InsideTheDekuTree_BossArea_ChangeENVForBoss",
                     "Gohma_IsLightingForBattle", "Gohma_DefeatBoss", "HavePlayedEvent.InsideTheDekuTree_BossDie"):
            self.assertEqual(flags.get(name), 0, name)
        self.assertEqual(flags.get("0x7d776b61"), "DefeatBoss")

    def test_age_sets_the_childhood_flag_just_before_the_warp_and_apply_lets_frames_run(self):
        # A warp loads Link at the age of Sequence_IsChildhood; the lane recipe is setflag, wait, then warp.
        for age, value in (("adult", 0), ("child", 1)):
            calls = ks.plan({"age": age, "hearts": 3, "max_hearts": 3, "warp": {"scene": "ForestTemple", "entrance": "ForestTemple"},
                             "place": {"x": 1, "y": 2, "z": 3}})
            kinds = [c[0] for c in calls]
            self.assertEqual(calls[kinds.index("warp") - 1], ("flag", "Sequence_IsChildhood", value))
        for bad in ("teen", 1, None, True):
            with self.assertRaises(ValueError):
                ks.plan({"age": bad, "warp": {"scene": "ForestTemple"}})
        fake = Fake()
        ks.apply(fake, {"age": "adult", "warp": {"scene": "ForestTemple", "entrance": "ForestTemple"}})
        self.assertEqual(fake.log, [("flag", "Sequence_IsChildhood", 0), ("step", 30), ("warp", "ForestTemple", "ForestTemple")])

    def test_the_adult_boss_presets_set_the_age_and_go_through_their_dungeon(self):
        # name: (scene, start, the intro played flag, the room's arrival point from docs/dungeons-boss.md / the start list)
        want = {
            "before-phantom-ganon": ("ForestTemple", "ForestTemple", "HavePlayedEvent.Dm_ForestTemple_BossIn", (-5.6, 74.67, -65.83)),
            "before-volvagia": ("FireTemple", "FireTemple", "HavePlayedEvent.Dm_FireTemple_BossIn", (-64.51, 102.58, -0.01)),
            "before-morpha": ("WaterTemple", "WaterTemple_FromLakeHylia", "HavePlayedEvent.Dm_WaterTemple_BossIn", (0.17, 131.36, -87.12)),
            "before-bongo-bongo": ("ShadowTemple", "Entrance", "HavePlayedEvent.Dm_ShadowTemple_BossIn", (-138.26, -11.46, 22.01)),
            "before-twinrova": ("SpiritTemple", "SpiritTemple_Entrance", "HavePlayedEvent.Dm_SpiritTemple_BossIn", (-0.73, 142.17, -158.59)),
            "before-ganondorf": ("InsideGanonCastle", "Entrance", "HavePlayedEvent.Dm_InsideGanonCastle_BossIn", (0.01, 171.05, -11.31)),
            "before-ganon": ("FinalBattleField", "FinalBattleField_Entrance", "HavePlayedEvent.Dm_FinalBattle_BossIn", (11.79, 100.05, -1.07)),
        }
        for name, (scene, start, intro, xyz) in want.items():
            calls = ks.plan(ks.load_preset(name))
            kinds = [c[0] for c in calls]
            w = kinds.index("warp")
            self.assertEqual(calls[w], ("warp", scene, start), name)
            self.assertEqual(calls[w - 1], ("flag", "Sequence_IsChildhood", 0), name)   # adult, just before the warp
            self.assertEqual(calls[-1], ("place",) + xyz, name)
            flags = {c[1]: c[2] for c in calls if c[0] == "flag"}
            self.assertEqual(flags.get(intro), 0, name)
            self.assertNotIn("Stage", scene, name)

    def test_the_child_boss_presets_set_the_age_and_place_link_at_the_rooms_arrival(self):
        for name, xyz in (("before-dodongo", (24.0, 73.41, -77.5)), ("before-barinade", (42.41, 102.01, -98.14))):
            calls = ks.plan(ks.load_preset(name))
            w = [c[0] for c in calls].index("warp")
            self.assertEqual(calls[w - 1], ("flag", "Sequence_IsChildhood", 1), name)
            self.assertEqual(calls[-1], ("place",) + xyz, name)

    def test_the_dodongo_and_barinade_presets_load_the_dungeon_not_the_rush_room(self):
        # As the Gohma preset: the rush rooms DodongoStage and BarinadeStage are not the dungeon, and a warp straight
        # into a boss room skips the door's own trigger. Both go into their dungeon at its entrance, keep the boss key,
        # and clear the boss's intro and death flags so the game's own door trigger can run.
        want = {
            "before-dodongo": ("DodongoCavern", "DodongoCavern_MountainTrailSide", "DodongoStage",
                               ("HavePlayedEvent.Dm_DodongoCave_BossIn", "HavePlayedEvent.Dm_DodongoCave_BossOut",
                                "HavePlayedEvent.Get_StoneOfFire_DefeatBoss"),
                               ("flag", "DungeonInfo.DodongoCavern.IsGetBossKey", 1)),
            "before-barinade": ("InsideJabuJabuBelly", "InsideJabuJabuBelly_FromZoraFountain", "BarinadeStage",
                                ("HavePlayedEvent.Dm_InsideJabu_Jabu_BossIn", "HavePlayedEvent.Dm_InsideJabu_Jabu_BossOut",
                                 "HavePlayedEvent.Get_StoneOfWater_DestroyBoss"),
                                ("flag", "DungeonInfo.InsideJabuJabuBelly.IsGetBossKey", 1)),
        }
        for name, (scene, start, rush, cleared, key) in want.items():
            calls = ks.plan(ks.load_preset(name))
            self.assertIn(("warp", scene, start), calls, name)
            self.assertNotIn(rush, [c[1] for c in calls if c[0] == "warp"], name)
            self.assertIn(key, calls, name)
            flags = {c[1]: c[2] for c in calls if c[0] == "flag"}
            for f in cleared:
                self.assertEqual(flags.get(f), 0, (name, f))

    def test_a_bad_preset_name_is_refused_with_the_list(self):
        for bad in ("nope", "../x", "", None, "A"):
            with self.assertRaises(ValueError) as e:
                ks.load_preset(bad)
            self.assertIn("deku-start", str(e.exception))


class Points(unittest.TestCase):
    class C:
        """A game whose flag `Stuck` ignores writes: the restore must say it is not exact."""
        def __init__(self):
            self.flags, self.log, self.pos, self.moves = {"A": 1, "Stuck": 5}, [], [1.0, 2.0, 3.0], True

        def dump(self, tag):
            return {"flags": {k: v for k, v in self.flags.items() if not k.startswith("0x")},   # the game's dump has no quest hashes
                    "state": {"scene": "S", "pos": list(self.pos), "life": 12}, "frame": 1}

        def state(self):
            return {"scene": "S"}

        def flag(self, name, value=1):
            self.log.append(("flag", name, value))
            if isinstance(value, str) and value.startswith("0x") and name not in self.enum_read:
                raise pc.BridgeError({"error": "bad_request", "message": "not a value flag_read read"})
            if name not in ("Stuck", "0xstuck"):
                self.flags[name] = value

        enum_read = ()

        def flag_read(self, name):
            self.log.append(("flag_read", name))
            if name not in self.flags:
                return {"ok": True, "name": name, "kind": "unknown", "value": None}
            return {"ok": True, "name": name, "kind": "int", "value": self.flags[name]}

        def act(self, line):
            self.log.append(("act", line))

        def warp(self, *a):
            self.log.append(("warp",) + a)
            if self.moves:   # a load puts Link at the scene's start
                self.pos = [9.0, 9.0, 9.0]

    def test_exact_only_when_the_second_dump_equals_the_saved_one(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            ks.save_point(c, d, "p1", "t")
            c.flags.update({"A": 9, "Stuck": 6})
            r = ks.load_point(c, d, "p1", "t")
            self.assertEqual((r["restored"], r["exact"], r["left"]), (2, False, ["Stuck"]))
            self.assertIn(("act", "place 1.0 2.0 3.0"), c.log)
            self.assertIn(("flag", "PlayerStatus.Life", 12), c.log)
            kinds = [x[0] for x in c.log]
            self.assertLess(max(i for i, k in enumerate(kinds) if k == "flag" and c.log[i][1] != "PlayerStatus.Life"),
                            kinds.index("warp"))   # the flags go in first, then the scene loads again
            self.assertTrue(r["reloaded"])

    def test_the_hidden_flags_come_back_at_their_save_time_values_and_are_checked(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            ks.apply(c, {"flags": {"0xabc": 0, "0xkey": 1}}, log_dir=d, player="p1")   # boss alive, key in
            c.flags["0xkey"] = 0                         # the key is used before the save: the save keeps 0, not 1
            out = ks.save_point(c, d, "p1", "t")
            self.assertEqual((out["setup_flags"], out["setup_flags_unread"]), (2, []))
            c.flags.update({"0xabc": 1, "0xkey": 1})     # the boss beaten, a key got after the save
            c.log.clear()
            r = ks.load_point(c, d, "p1", "t")
            self.assertEqual((c.flags["0xabc"], c.flags["0xkey"]), (0, 0))
            self.assertEqual(r["setup_flags_written"], 2)
            self.assertTrue(r["setup_flags_checked"])
            self.assertEqual((r["left"], r["exact"]), ([], True))
            kinds = [x[0] for x in c.log]
            self.assertLess(c.log.index(("flag", "0xabc", 0)), kinds.index("warp"))   # written before the room loads
            self.assertGreater(len(kinds) - 1 - kinds[::-1].index("flag_read"), kinds.index("warp"))   # read after it

    def test_a_hidden_flag_that_does_not_come_back_is_a_mismatch(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            c.flags.pop("Stuck")
            c.flags["0xstuck"] = 0                       # a hidden flag that ignores writes
            ks.apply(c, {"flags": {"0xstuck": 0}}, log_dir=d, player="p1")
            ks.save_point(c, d, "p1", "t")
            c.flags["0xstuck"] = 1
            r = ks.load_point(c, d, "p1", "t")
            self.assertFalse(r["setup_flags_checked"])
            self.assertFalse(r["exact"])
            self.assertEqual(r["left"], ["0xstuck"])
            self.assertEqual(r["setup_flags_left"], {"0xstuck": {"want": 0, "now": 1}})

    def test_an_enum_hash_the_bridge_refuses_is_not_checked(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            c.flags["0xq"] = "0x00000abc"                # an enum, read as its value's hash
            ks.apply(c, {"flags": {"0xq": "Step05"}}, log_dir=d, player="p1")
            c.flags["0xq"] = "0x00000abc"
            ks.save_point(c, d, "p1", "t")
            c.flags["0xq"] = "0x00000def"
            r = ks.load_point(c, d, "p1", "t")           # (this fake game run never read it: the bridge refuses)
            self.assertEqual(len(r["setup_flags_refused"]), 1)
            self.assertFalse(r["setup_flags_checked"])
            self.assertFalse(r["exact"])

    def test_a_flag_the_game_does_not_have_is_named_unread(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            ks.apply(c, {"flags": {"0xnone": 1}}, log_dir=d, player="p1")
            c.flags.pop("0xnone")
            out = ks.save_point(c, d, "p1", "t")
            self.assertEqual((out["setup_flags"], out["setup_flags_unread"]), (0, ["0xnone"]))

    def test_a_second_setup_replaces_the_first_ones_flags(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            ks.apply(c, {"flags": {"0xabc": 1}}, log_dir=d, player="p1")
            ks.apply(c, {"flags": {"0xdef": 2}}, log_dir=d, player="p1")
            ks.save_point(c, d, "p1", "t")
            c.log.clear()
            r = ks.load_point(c, d, "p1", "t")
            self.assertEqual(r["setup_flags_written"], 1)
            self.assertNotIn(("flag", "0xabc", 1), c.log)
            self.assertIn(("flag", "0xdef", 2), c.log)
            ks.forget_setup(d, "p1")   # a new server: nothing to put back
            ks.save_point(c, d, "p1", "u")
            self.assertEqual(ks.load_point(c, d, "p1", "u")["setup_flags_written"], 0)

    def test_a_name_the_dump_lists_ends_at_the_saved_value_not_the_setups(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            ks.apply(c, {"flags": {"A": 5}}, log_dir=d, player="p1")   # the setup wrote A=5 ...
            c.flags["A"] = 1
            ks.save_point(c, d, "p1", "t")                              # ... but the save has A=1
            c.log.clear()
            r = ks.load_point(c, d, "p1", "t")
            self.assertEqual(r["setup_flags_written"], 0)               # A is the dump's, not the setup's
            self.assertNotIn(("flag", "A", 5), c.log)
            self.assertEqual(c.flags["A"], 1)

    def test_a_same_scene_load_that_cannot_be_told_is_not_exact(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            c = self.C()
            c.moves = False
            ks.save_point(c, d, "p1", "t")
            r = ks.load_point(c, d, "p1", "t")
            self.assertEqual((r["reloaded"], r["exact"], r["left"]), (False, False, ["Stuck"][:0]))
            self.assertTrue([x for x in c.log if x[0] == "warp"])

    def test_the_second_game_leaves_the_delta_shared_keys_out(self):
        import tempfile
        c = Fake()
        ks.apply(c, {"small_keys": {"ForestTemple": 2}, "rupees": 5}, shared=False)
        self.assertEqual([x for x in c.log if "SmallKeyNum" in str(x)], [])
        self.assertTrue([x for x in c.log if "CurrentRupee" in str(x)])
        with tempfile.TemporaryDirectory() as d:
            p = self.C()
            p.flags["DungeonInfo.ForestTemple.SmallKeyNum"] = 2
            ks.save_point(p, d, "p1", "t")
            p.flags["DungeonInfo.ForestTemple.SmallKeyNum"] = 0
            r = ks.load_point(p, d, "p1", "t", shared=False)
            self.assertEqual(r["restored"], 0)
            self.assertEqual(r["left"], [])

    def test_a_missing_tag_is_refused(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d, self.assertRaises(ValueError):
            ks.load_point(self.C(), d, "p1", "nope")


if __name__ == "__main__":
    unittest.main()
