"""Tests for the world compare of desync_check.py and for sync_rows.py (it reads docs/sync-coverage.md).
Run: python -I tools/autotest/tests/test_desync_check.py"""
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import desync_check as dc  # noqa: E402
import sync_rows as sr  # noqa: E402

PAGE = """# Page

| Object | Actors (romfs) | Path | Live | Proven | Notes | P |
|---|---|---|---|---|---|---|
| Chests (live) | `Obj_TBox_*` | flags | live | game | n | P1 |
| Chests that appear | `Obj_TBox_*`, `...TBoxAppear` | flags | ? | - | n | P2 |
| Pots | `Obj_Pot_*` (12), `Obj_Pot_Big` | objects | reload | - | n | P2 |
| Doors | `Obj_Door_*` | objects | live (4.6 s late) | game | n | P2 |
| Rubble | `Obj_Rubble*` | none | live | - | n | P2 |
| Fairies | `Obj_Fairy` | none | - | - | n | P3 |
| Enemies | `Enemy_*` | objects | ? | - | n | P2 |
| Bosses | `Enemy_Goma`, `Enemy_Boss*` | boss | live | game | n | P1 |
| Odd | `Obj_Odd` | flags | sometimes | - | n | P2 |
| Text only | Obj_Plain and words | flags | live | - | n | P2 |

| State | Path | Live |
|---|---|---|
| A flag | flags | live |
"""


def page_rows():
    d = tempfile.mkdtemp()
    p = os.path.join(d, "page.md")
    with io.open(p, "w", encoding="utf-8") as f:
        f.write(PAGE)
    return sr.load(p)


def actor(name, pos=(0, 0, 0), kind="enemy", life=None, owner="server", alive=True, id=None, dist=1.0, i=[0]):
    i[0] += 1
    n = id if id is not None else i[0]
    return {"id": n, "addr": "0x%x" % n, "name": name, "kind": kind, "pos": None if pos is None else list(pos),
            "dist": dist, "life": life, "max_life": life, "owner": owner, "alive": alive}


def cp(actors, frame=1000, scene="Forest", pos=(0, 0, 0), world_extra=None):
    w = {"frame": frame, "age_ms": 5, "stale": False, "behind": 0, "total": len(actors), "other_count": 0,
         "count": len(actors), "actors": actors}
    w.update(world_extra or {})
    return {"tag": "t", "frame": frame, "scene": scene, "state": {"scene": scene, "pos": list(pos), "world": w},
            "flags": {"F": 1}}


class Rows(unittest.TestCase):
    def setUp(self):
        self.rows, self.tables = page_rows()

    def test_only_tables_with_path_live_and_actors_are_read(self):
        self.assertEqual(self.tables, 1)
        self.assertNotIn("A flag", [r["name"] for r in self.rows])

    def test_only_backticked_names_count(self):
        by = {r["name"]: r for r in self.rows}
        self.assertEqual(by["Chests that appear"]["globs"], ["Obj_TBox_*"])      # `...TBoxAppear` is no name
        self.assertEqual(by["Pots"]["globs"], ["Obj_Pot_*", "Obj_Pot_Big"])
        self.assertEqual(by["Text only"]["globs"], [])
        self.assertIsNone(sr.match(self.rows, "Obj_Plain"))

    def test_labels(self):
        lab = {r["name"]: r["label"] for r in self.rows}
        self.assertEqual(lab["Chests (live)"], "live")
        self.assertEqual(lab["Chests that appear"], "not proven")
        self.assertEqual(lab["Pots"], "reload")
        self.assertEqual(lab["Doors"], "live")                  # "live (4.6 s late)" is live
        self.assertEqual(lab["Rubble"], "not shared")           # Path none beats Live live
        self.assertEqual(lab["Fairies"], "not shared")

    def test_unknown_live_value_is_a_problem(self):
        self.assertEqual(len(sr.problems(self.rows)), 1)
        self.assertIn("sometimes", sr.problems(self.rows)[0])

    def test_a_tie_takes_the_weaker_label_and_names_both_rows(self):
        m = sr.match(self.rows, "Obj_TBox_Basic")
        self.assertTrue(m["tied"])
        self.assertEqual(m["label"], "not proven")
        self.assertEqual([r["name"] for r in m["rows"]], ["Chests (live)", "Chests that appear"])
        self.assertIn("Chests (live)", m["text"])
        self.assertIn("Chests that appear", m["text"])

    def test_an_exact_name_beats_a_wildcard(self):
        m = sr.match(self.rows, "Enemy_Goma")
        self.assertEqual(m["rows"][0]["name"], "Bosses")
        self.assertEqual(m["label"], "live")                     # not the Enemy_* row
        self.assertEqual(sr.match(self.rows, "Enemy_Other")["rows"][0]["name"], "Enemies")

    def test_the_longer_wildcard_beats_the_shorter(self):
        m = sr.match(self.rows, "Enemy_BossFoo")
        self.assertEqual(m["rows"][0]["name"], "Bosses")

    def test_no_row(self):
        self.assertIsNone(sr.match(self.rows, "Npc_Unknown"))

    def test_ties_lists_a_glob_on_rows_with_different_labels(self):
        self.assertEqual(sr.ties(self.rows), {"Obj_TBox_*": ["Chests", "Chests that appear"]})


class RealPage(unittest.TestCase):
    """The page in the repository."""

    KNOWN_TIES = {"FallingFloor_A_03", "Obj_Rupee_*", "Obj_Heart", "Obj_TBox_*"}

    @classmethod
    def setUpClass(cls):
        cls.rows, cls.tables = sr.load()

    def test_it_reads_the_tables_and_every_live_value(self):
        self.assertGreaterEqual(self.tables, 3)
        self.assertEqual(sr.problems(self.rows), [])

    def test_no_new_tie(self):
        self.assertEqual(set(sr.ties(self.rows)), self.KNOWN_TIES)

    def test_enemies_and_rolled_spawns_rows(self):
        enemies = sr.match(self.rows, "Enemy_Dekubaba")
        self.assertEqual(enemies["rows"][0]["name"], "Enemies")
        self.assertEqual(enemies["label"], "not proven")
        rolled = sr.match(self.rows, "Enemy_Stalchild")
        self.assertEqual(rolled["rows"][0]["name"].split(" (")[0], "Rolled spawns")

    def test_the_overlaps_and_their_winners(self):
        def win(n):
            m = sr.match(self.rows, n)
            return m["glob"], m["label"], [r["name"].split(" (")[0] for r in m["rows"]]
        self.assertEqual(win("Obj_Fairy"), ("Obj_Fairy", "not proven", ["Drops"]))
        self.assertEqual(win("Obj_FairyX"), ("Obj_Fairy*", "not shared", ["Fairies"]))
        self.assertEqual(win("Item_Token_Gold")[0], "Item_Token_Gold")
        self.assertEqual(win("Item_Dungeon_Map")[0], "Item_Dungeon_*")
        self.assertEqual(win("Item_Other")[0], "Item_*")
        self.assertEqual(win("Obj_Rupee_Silver"), ("Obj_Rupee_Silver", "not proven", ["Silver rupees"]))
        self.assertEqual(win("Obj_CandleStick_TimeExtinguish")[:2], ("Obj_CandleStick_TimeExtinguish", "not shared"))
        self.assertEqual(win("Obj_CandleStick_A")[0], "Obj_CandleStick_*")
        self.assertEqual(win("Obj_WaterTemple_WaterPlane")[0], "Obj_WaterTemple_WaterPlane*")
        self.assertEqual(win("Obj_Foo_WaterPlane2")[0], "Obj_*_WaterPlane*")

    def test_each_known_tie_prints_both_rows_and_the_weaker_label(self):
        m = sr.match(self.rows, "Obj_Rupee_Green")
        self.assertTrue(m["tied"])
        self.assertEqual(m["label"], "not proven")
        self.assertEqual(len(m["rows"]), 2)
        self.assertEqual(sr.match(self.rows, "Obj_Heart")["label"], "not proven")
        t = sr.match(self.rows, "Obj_TBox_Basic")
        self.assertEqual((t["tied"], len(t["rows"])), (True, 2))
        ff = sr.match(self.rows, "FallingFloor_A_03")
        self.assertTrue(ff["tied"])
        self.assertEqual(len(ff["rows"]), 3)

    def test_a_stalchild_prints_live_not_proven(self):
        m = sr.match(self.rows, "Enemy_Stalchild")
        self.assertEqual(m["label"], "not proven")
        self.assertIn("live not proven", m["text"])
        self.assertNotIn("not shared", m["text"])

    def test_the_names_that_are_not_actor_prefixes_are_the_known_fragments(self):
        """`unprefixed` lists the backticked tokens with no actor prefix: names of other kinds (a few are real actor
        names) and fragments of a note. A new one is a page change to look at: it is listed here, then added or fixed."""
        names = sorted({g for _, g in sr.unprefixed(self.rows)})
        sys.stderr.write("\nunprefixed tokens in sync-coverage.md: %s\n" % ", ".join(names))
        self.assertEqual(names, ["*ElevatorLift*", "*InteractiveSpiderWebParts*", "FallingFloor_A_03",
                                 "FallingLadder_A_01", "HammerDropFloor", "HammerDropPillar", "HammerStair",
                                 "Land_kokiri_Plant_*", "WallCrack", "WaterTemple_Room00_WaterLevel", "_Cleared"])

    def test_row_83_is_split(self):
        self.assertEqual(sr.match(self.rows, "Obj_SolarSwitch_A")["rows"][0]["name"], "Sun switches")
        self.assertEqual(sr.match(self.rows, "Obj_SolarSwitch_A")["label"], "not proven")
        self.assertEqual(sr.match(self.rows, "Obj_SpiritTemple_Mirror")["label"], "not shared")
        self.assertEqual(sr.match(self.rows, "Obj_SpiritTemple_RayShaft_A")["label"], "not shared")


class World(unittest.TestCase):
    def setUp(self):
        self.rows, _ = page_rows()

    def test_same_actors_agree(self):
        a = [actor("Enemy_Dekubaba", (1, 0, 0), life=3), actor("Obj_TBox_X", (5, 0, 0), kind="chest")]
        b = [actor("Enemy_Dekubaba", (9, 0, 0), life=3), actor("Obj_TBox_X", (5, 0, 0), kind="chest")]
        r = dc.compare_world(cp(a), cp(b), self.rows)
        self.assertIsNone(r["note"])
        self.assertEqual(r["diffs"], [])
        self.assertEqual(r["compared"], 2)

    def test_a_moving_enemys_place_is_not_compared(self):
        a, b = [actor("Enemy_Dekubaba", (1, 0, 0))], [actor("Enemy_Dekubaba", (30, 0, 0))]
        self.assertEqual(dc.compare_world(cp(a), cp(b), self.rows)["diffs"], [])

    def test_a_chests_place_is_compared_to_0_2_m(self):
        a, b = [actor("Obj_TBox_X", (5, 0, 0), kind="chest")], [actor("Obj_TBox_X", (5.5, 0, 0), kind="chest")]
        d = dc.compare_world(cp(a), cp(b), self.rows)["diffs"]
        self.assertEqual([x["field"] for x in d], ["pos"])
        b = [actor("Obj_TBox_X", (5.1, 0, 0), kind="chest")]
        self.assertEqual(dc.compare_world(cp(a), cp(b), self.rows)["diffs"], [])

    def test_a_bosss_place_is_compared(self):
        a, b = [actor("Enemy_BossGoma", (0, 0, 0))], [actor("Enemy_BossGoma", (2, 0, 0))]
        d = dc.compare_world(cp(a), cp(b), self.rows)["diffs"]
        self.assertEqual([x["field"] for x in d], ["pos"])
        self.assertEqual(d[0]["label"], "live")

    def test_alive_owner_and_life(self):
        a = [actor("Enemy_Goma", life=3)]
        b = [actor("Enemy_Goma", life=2, owner=2, alive=False)]
        d = dc.compare_world(cp(a), cp(b), self.rows)["diffs"]
        self.assertEqual(sorted(x["field"] for x in d), ["alive", "life", "owner"])

    def test_a_count_difference_names_the_actor(self):
        a = [actor("Enemy_Goma", (0, 0, 0)), actor("Enemy_Goma", (4, 0, 0))]
        b = [actor("Enemy_Goma", (0, 0, 0))]
        d = dc.compare_world(cp(a), cp(b), self.rows)["diffs"]
        self.assertEqual([(x["name"], x["field"], x["a"], x["b"]) for x in d], [("Enemy_Goma", "count", 2, 1)])

    def test_pairs_by_nearest_place_not_by_order(self):
        a = [actor("Enemy_Goma", (0, 0, 0), life=3), actor("Enemy_Goma", (50, 0, 0), life=1)]
        b = [actor("Enemy_Goma", (50, 0, 0), life=1), actor("Enemy_Goma", (0, 0, 0), life=3)]
        self.assertEqual(dc.compare_world(cp(a), cp(b), self.rows)["diffs"], [])

    def test_labels_follow_the_page(self):
        a, b = [actor("Enemy_Dekubaba", life=3)], [actor("Enemy_Dekubaba", life=1)]
        d = dc.compare_world(cp(a), cp(b), self.rows)["diffs"]
        self.assertEqual(d[0]["label"], "not proven")
        self.assertIn("live not proven", d[0]["text"])
        a, b = [actor("Obj_Pot_Big", alive=True)], [actor("Obj_Pot_Big", alive=False)]
        self.assertEqual(dc.compare_world(cp(a), cp(b), self.rows)["diffs"][0]["label"], "reload")

    def test_not_shared_is_left_out_and_counted(self):
        a, b = [actor("Obj_Fairy", alive=True)], [actor("Obj_Fairy", alive=False)]
        r = dc.compare_world(cp(a), cp(b), self.rows)
        self.assertEqual((r["diffs"], r["skipped"], r["compared"]), ([], 2, 0))

    def test_no_row_is_not_compared_and_named(self):
        a, b = [actor("Npc_Who", alive=True)], [actor("Npc_Who", alive=False)]
        r = dc.compare_world(cp(a), cp(b), self.rows)
        self.assertEqual((r["diffs"], r["no_row"]), ([], {"Npc_Who": 2}))

    def test_link_is_never_compared(self):
        a, b = [actor("Player_Link", kind="player", life=3)], [actor("Player_Link", kind="player", life=1)]
        r = dc.compare_world(cp(a), cp(b), self.rows)
        self.assertEqual((r["diffs"], r["no_row"], r["compared"]), ([], {}, 0))

    def test_a_diff_names_the_actor_and_the_row(self):
        a, b = [actor("Enemy_Goma", (1, 2, 3), life=3, id=7)], [actor("Enemy_Goma", (1, 2, 4), life=1, id=19)]
        d = dc.compare_world(cp(a), cp(b), self.rows)["diffs"][0]["text"]
        self.assertIn("A id 7 at (1.0, 2.0, 3.0)", d)
        self.assertIn("B id 19 at (1.0, 2.0, 4.0)", d)
        self.assertIn("docs/sync-coverage.md:", d)
        self.assertIn("Path: boss", d)                  # the Bosses row
        self.assertIn("Live: live", d)

    def test_a_count_difference_names_the_unmatched_actors(self):
        a = [actor("Enemy_Goma", (0, 0, 0), id=1), actor("Enemy_Goma", (4, 0, 0), id=2)]
        b = [actor("Enemy_Goma", (0, 0, 0), id=5)]
        d = dc.compare_world(cp(a), cp(b), self.rows)["diffs"][0]["text"]
        self.assertIn("no match in B: A id 2 at (4.0, 0.0, 0.0)", d)

    def test_a_static_actor_with_no_place_is_noted_not_a_difference(self):
        a = [actor("Obj_Door_A", (5, 0, 0), kind="door")]
        b = [actor("Obj_Door_A", None, kind="door")]
        r = dc.compare_world(cp(a), cp(b), self.rows)
        self.assertEqual((r["diffs"], r["no_place"]), ([], {"Obj_Door_A": 1}))
        del b[0]["pos"]
        r = dc.compare_world(cp(a), cp(b), self.rows)               # (no pos key at all: no crash)
        self.assertEqual((r["diffs"], r["no_place"]), ([], {"Obj_Door_A": 1}))

    def test_a_cut_list_is_compared_only_near_both_links(self):
        """Both lists hold 200 actors (the snapshot's cut). A crate only one game kept is beyond the cut, not a desync."""
        cut = {"total": 200}
        # A's Link at the origin kept to 40 m, B's Link at x = 10 kept to 25 m
        far = actor("Obj_Door_Crate", (-20, 0, 0), kind="other", dist=20.0, id=1)      # only A has it: 30 m from B's Link
        near_a = [actor("Obj_Door_Rock", (0, 0, 0), kind="other", dist=40.0, id=2), far]
        near_b = [actor("Obj_Door_Rock", (0, 0, 0), kind="other", dist=25.0, id=3)]
        a, b = cp(near_a, world_extra=cut), cp(near_b, world_extra=cut, pos=(10, 0, 0))
        r = dc.compare_world(a, b, self.rows)
        self.assertEqual(r["cut"], {"a": 40.0, "b": 25.0})
        self.assertEqual(r["diffs"], [])
        self.assertEqual(r["cut_left_out"], 1)                      # A's crate: inside A's 40 m, beyond B's 25 m
        near = actor("Obj_Door_Crate", (5, 0, 0), kind="other", dist=5.0, id=4)        # inside both kept distances
        r = dc.compare_world(cp([near] + near_a, world_extra=cut), b, self.rows)
        self.assertEqual([d["field"] for d in r["diffs"]], ["count"])    # B should have kept it: a real difference

    def test_an_actor_is_judged_against_both_links_95_and_65(self):
        """Links 30 m apart, both lists cut and keep 100 m. A crate 95 m from A's Link and 65 m from B's Link is inside
        both kept distances: B kept it, A (listing from its own place) has it too, nothing differs. A crate 105 m from
        A and 75 m from B is beyond A's cut: A lacks it, B has it, and that is the cut, not a desync."""
        cut = {"total": 200}
        edge = [actor("Obj_Door_Rock", (0, 0, 0), kind="other", dist=100.0, id=1)]    # makes each list's kept distance 100
        in_both = actor("Obj_Door_Crate", (95, 0, 0), kind="other", dist=95.0, id=2)   # 95 from A at 0, 65 from B at 30
        in_b_only = actor("Obj_Door_Crate", (105, 0, 0), kind="other", dist=75.0, id=3)    # 105 from A: beyond A's cut
        a = cp(edge + [in_both], world_extra=cut, pos=(0, 0, 0))
        b_in_both = dict(in_both, id=12, dist=65.0)
        b = cp(edge + [b_in_both, in_b_only], world_extra=cut, pos=(30, 0, 0))
        r = dc.compare_world(a, b, self.rows)
        self.assertEqual(r["cut"], {"a": 100.0, "b": 100.0})
        self.assertEqual(r["diffs"], [])
        self.assertEqual(r["cut_left_out"], 1)                      # the crate 105 m from A
        # the old rule (each copy against its own Link, minus the gap) would have left out the 95 m crate and kept B's
        # 75 m one: a false count difference. Here the count of crates is equal (A 1, B 1 compared).
        self.assertEqual(r["compared"], 2)                          # the rock and the 95/65 crate

    def test_a_cut_list_leaves_out_an_actor_with_no_place(self):
        cut = {"total": 200}
        a = cp([actor("Obj_Door_Rock", (0, 0, 0), kind="other", dist=10.0, id=1)], world_extra=cut)
        b = cp([actor("Obj_Door_Rock", (0, 0, 0), kind="other", dist=10.0, id=2),
                actor("Obj_Door_Crate", None, kind="other", id=3)], world_extra=cut)
        r = dc.compare_world(a, b, self.rows)
        self.assertEqual((r["diffs"], r["cut_left_out"]), ([], 1))
        r = dc.compare_world(cp([actor("Enemy_Goma", None)]), cp([actor("Enemy_Goma", None)]), self.rows)
        self.assertIsNone(r["cut"])                                 # whole lists: no limit, nothing left out
        self.assertEqual(r["cut_left_out"], 0)

    def test_a_whole_list_has_no_cut(self):
        r = dc.compare_world(cp([actor("Enemy_Goma")]), cp([actor("Enemy_Goma")]), self.rows)
        self.assertIsNone(r["cut"])

    def test_not_compared_notes(self):
        a = [actor("Enemy_Goma")]
        self.assertIn("scenes", dc.compare_world(cp(a), cp(a, scene="Water"), self.rows)["note"])
        self.assertIn("apart", dc.compare_world(cp(a), cp(a, pos=(100, 0, 0)), self.rows)["note"])
        self.assertIn("stale", dc.compare_world(cp(a), cp(a, world_extra={"stale": True}), self.rows)["note"])
        self.assertIn("no co-op session",
                      dc.compare_world(cp(a, world_extra={"why": "no co-op session"}), cp(a), self.rows)["note"])
        nowv = cp(a)
        nowv["state"].pop("world")
        self.assertIn("no world list", dc.compare_world(cp(a), nowv, self.rows)["note"])
        self.assertIn("place is unknown", dc.compare_world(cp(a), cp(a, pos=()), self.rows)["note"])

    def test_empty_world_with_a_reason_is_usable(self):
        r = dc.compare_world(cp([], world_extra={"why": "empty: no actor in range"}), cp([]), self.rows)
        self.assertIsNone(r["note"])


def write_logs(entries):
    d = tempfile.mkdtemp()
    for player, cps in entries.items():
        with io.open(os.path.join(d, "%s.checkpoints.jsonl" % player), "w", encoding="utf-8") as f:
            for c in cps:
                f.write(json.dumps(c) + "\n")
    return d


def run(entries, **kw):
    out = []
    kw.setdefault("rows", page_rows()[0])
    code = dc.check_clients(write_logs(entries), out=out.append, **kw)
    return code, out


def tagged(tag, a, **kw):
    c = cp(a, **kw)
    c["tag"] = tag
    return c


class Settle(unittest.TestCase):
    def test_no_world_data_keeps_the_old_output(self):
        e = {"p1": [{"tag": "t", "frame": 10, "scene": "S", "state": {}, "flags": {"F": 1}}],
             "p2": [{"tag": "t", "frame": 12, "scene": "S", "state": {}, "flags": {"F": 1}}]}
        code, out = run(e)
        self.assertEqual((code, out), (0, ["t: agree (p1 frame 10 S, p2 frame 12 S)"]))

    def test_a_live_difference_must_be_seen_twice_60_frames_apart(self):
        g, h = [actor("Enemy_Goma", life=3)], [actor("Enemy_Goma", life=1)]
        e = {"p1": [tagged("a", g, frame=1000), tagged("b", g, frame=1100)],
             "p2": [tagged("a", h, frame=1005), tagged("b", h, frame=1110)]}
        code, out = run(e)
        self.assertEqual(code, 1)
        self.assertTrue(any("recheck after both games step 60 frames" in ln for ln in out))
        self.assertTrue(any("DESYNC" in ln and "Enemy_Goma" in ln for ln in out))

    def test_a_desync_line_carries_its_evidence(self):
        g, h = [actor("Enemy_Goma", (1, 0, 0), life=3, id=7)], [actor("Enemy_Goma", (1, 0, 0), life=1, id=19)]
        e = {"p1": [tagged("a", g, frame=1000), tagged("b", g, frame=1100)],
             "p2": [tagged("a", h, frame=1005), tagged("b", h, frame=1110)]}
        line = [ln for ln in run(e)[1] if "DESYNC" in ln][0]
        for part in ("Enemy_Goma", "A id 7", "B id 19", "first seen at a (p1 frame 1000, p2 frame 1005)",
                     "confirmed at b (p1 frame 1100, p2 frame 1110)", "docs/sync-coverage.md:", "Path: boss"):
            self.assertIn(part, line)

    def test_the_first_seen_tag_is_kept_across_close_sightings(self):
        g, h = [actor("Enemy_Goma", life=3, id=7)], [actor("Enemy_Goma", life=1, id=19)]
        e = {"p1": [tagged("a", g, frame=1000), tagged("b", g, frame=1030), tagged("c", g, frame=1100)],
             "p2": [tagged("a", h, frame=1000), tagged("b", h, frame=1030), tagged("c", h, frame=1100)]}
        line = [ln for ln in run(e)[1] if "DESYNC" in ln][0]
        self.assertIn("first seen at a (p1 frame 1000, p2 frame 1000), confirmed at c", line)

    def test_another_actor_of_the_same_name_does_not_confirm(self):
        g1, h1 = [actor("Enemy_Goma", life=3, id=7)], [actor("Enemy_Goma", life=1, id=19)]
        g2, h2 = [actor("Enemy_Goma", life=3, id=8)], [actor("Enemy_Goma", life=1, id=20)]
        e = {"p1": [tagged("a", g1, frame=1000), tagged("b", g2, frame=1100)],
             "p2": [tagged("a", h1, frame=1000), tagged("b", h2, frame=1100)]}
        code, out = run(e)
        self.assertEqual(code, 3)
        self.assertFalse(any("DESYNC" in ln for ln in out))

    def test_a_cut_list_gives_no_false_desync_over_two_checkpoints(self):
        cut = {"total": 200}
        far = actor("Obj_Door_Crate", (-20, 0, 0), kind="other", dist=20.0, id=1)      # 30 m from B's Link: beyond B's cut
        a = [actor("Obj_Door_Rock", kind="other", dist=40.0, id=2), far]
        b = [actor("Obj_Door_Rock", kind="other", dist=25.0, id=3)]
        e = {"p1": [tagged("a", a, frame=1000, world_extra=cut), tagged("b", a, frame=1100, world_extra=cut)],
             "p2": [tagged("a", b, frame=1000, world_extra=cut, pos=(10, 0, 0)),
                    tagged("b", b, frame=1100, world_extra=cut, pos=(10, 0, 0))]}
        code, out = run(e)
        self.assertEqual(code, 0)
        self.assertTrue(any("a list was cut at 200 actors" in ln for ln in out))

    def test_the_first_sighting_alone_is_incomplete_never_agree(self):
        g, h = [actor("Enemy_Goma", life=3)], [actor("Enemy_Goma", life=1)]
        code, out = run({"p1": [tagged("a", g)], "p2": [tagged("a", h)]})
        self.assertEqual(code, 3)
        self.assertIn("incomplete", out[-1])
        self.assertTrue(any("recheck" in ln for ln in out))

    def test_two_sightings_closer_than_60_frames_do_not_confirm(self):
        g, h = [actor("Enemy_Goma", life=3)], [actor("Enemy_Goma", life=1)]
        e = {"p1": [tagged("a", g, frame=1000), tagged("b", g, frame=1030)],
             "p2": [tagged("a", h, frame=1000), tagged("b", h, frame=1030)]}
        self.assertEqual(run(e)[0], 3)

    def test_a_difference_that_goes_away_is_not_a_desync(self):
        g, h = [actor("Enemy_Goma", life=3)], [actor("Enemy_Goma", life=1)]
        e = {"p1": [tagged("a", g, frame=1000), tagged("b", g, frame=1100)],
             "p2": [tagged("a", h, frame=1000), tagged("b", g, frame=1100)]}
        self.assertEqual(run(e)[0], 0)

    def test_a_not_proven_difference_never_gives_exit_1(self):
        g, h = [actor("Enemy_Dekubaba", life=3)], [actor("Enemy_Dekubaba", life=1)]
        e = {"p1": [tagged("a", g, frame=1000), tagged("b", g, frame=1100)],
             "p2": [tagged("a", h, frame=1000), tagged("b", h, frame=1100)]}
        code, out = run(e)
        self.assertEqual(code, 0)
        self.assertTrue(any("live not proven" in ln for ln in out))

    def test_flags_only_leaves_the_world_out(self):
        g, h = [actor("Enemy_Goma", life=3)], [actor("Enemy_Goma", life=1)]
        code, out = run({"p1": [tagged("a", g)], "p2": [tagged("a", h)]}, flags_only=True)
        self.assertEqual((code, len(out)), (0, 1))

    def test_a_flag_difference_still_is_exit_1(self):
        g = [actor("Enemy_Goma")]
        a, b = tagged("a", g), tagged("a", g)
        b["flags"] = {"F": 2}
        code, out = run({"p1": [a], "p2": [b]})
        self.assertEqual(code, 1)
        self.assertIn("<- first desync", out[0])

    def test_a_missing_doc_says_so(self):
        g = [actor("Enemy_Goma")]
        out = []
        d = write_logs({"p1": [tagged("a", g)], "p2": [tagged("a", g)]})
        orig = sr.DOC
        sr.DOC = os.path.join(d, "none.md")
        try:
            code = dc.check_clients(d, out=out.append)
        finally:
            sr.DOC = orig
        self.assertEqual(code, 0)
        self.assertTrue(any("sync-coverage.md was not found" in ln for ln in out))


if __name__ == "__main__":
    unittest.main()
