"""kit_look: the one-call look (no game)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import kit_look  # noqa: E402


def state(**kw):
    s = {"scene": "KokiriForest_Child", "room": None, "pos": [0.0, 10.0, 0.0], "facing": 90.0, "life": 12,
         "max_life": 12, "rupees": 5, "age": "child", "event": False, "loading": False, "prompt": None,
         "equips": {"weapon": "Sword_Kokiri", "shield": None}, "inventory": {"Item_Nuts": 5, "Item_Zero": 0},
         "world": {"why": None, "actors": []}}
    s.update(kw)
    return s


def actor(i, name, kind, pos, life=None, alive=None):
    return {"id": i, "name": name, "kind": kind, "pos": pos, "dist": abs(pos[0]) + abs(pos[2]), "life": life,
            "max_life": life, "alive": alive}


class LookTest(unittest.TestCase):
    def test_bearing_and_side(self):
        self.assertEqual(kit_look.bearing([0, 0, 0], [0, 0, 5]), 0)
        self.assertEqual(kit_look.bearing([0, 0, 0], [5, 0, 0]), 90)
        self.assertEqual(kit_look.bearing([0, 0, 0], [-5, 0, 0]), 270)
        self.assertEqual(kit_look.side(90, 100), "ahead")
        self.assertEqual(kit_look.side(90, 180), "right")
        self.assertEqual(kit_look.side(90, 0), "left")
        self.assertEqual(kit_look.side(90, 270), "behind")

    def test_short_and_complete(self):
        w = {"why": None, "actors": [actor(3, "En_Skb", "enemy", [4, 10, 0], 4, True),
                                     actor(4, "Door_Warp1", "door", [0, 10, 6]),
                                     actor(5, "Obj_TBox", "chest", [-3, 10, 0]),
                                     actor(6, "Far", "enemy", [99, 0, 0], 4, True)]}
        text, summ = kit_look.look(state(world=w, prompt="Open"))
        self.assertLess(len(text.splitlines()), 40)
        for want in ("KokiriForest_Child", "child", "12/12", "Sword_Kokiri", "Item_Nuts 5", "A: Open",
                     "enemy: En_Skb #3 life 4/4 4m yaw 90 ahead", "door: Door_Warp1 #4 6m yaw 0 left",
                     "chest: Obj_TBox #5", "first look"):
            self.assertIn(want, text)
        self.assertNotIn("Far", text)
        self.assertNotIn("Item_Zero", text)
        self.assertEqual(set(summ["seen"]), {3, 4, 5})

    def test_changes(self):
        a = actor(3, "En_Skb", "enemy", [4, 10, 0], 4, True)
        _, first = kit_look.look(state(world={"why": None, "actors": [a]}))
        dead = dict(a, alive=False, life=0)
        new = actor(9, "En_Item00", "item", [1, 10, 1])
        text, _ = kit_look.look(state(life=8, pos=[3.0, 10.0, 0.0], world={"why": None, "actors": [dead, new]},
                                      event=True), first)
        line = text.splitlines()[-1]
        for want in ("life 12 -> 8", "moved 3.0 m", "event started", "enemy En_Skb #3 is dead", "new item En_Item00 #9"):
            self.assertIn(want, line)
        self.assertIn("enemies dead: 1", text)
        again, _ = kit_look.look(state(life=8, pos=[3.0, 10.0, 0.0], world={"why": None, "actors": [dead, new]},
                                       event=True), kit_look.look(state(life=8, pos=[3.0, 10.0, 0.0],
                                                                        world={"why": None, "actors": [dead, new]},
                                                                        event=True))[1])
        self.assertTrue(again.endswith("changed: nothing"))

    def test_scene_change_and_age(self):
        _, first = kit_look.look(state())
        text, _ = kit_look.look(state(scene="LostWoods", age="adult"), first)
        self.assertIn("scene KokiriForest_Child -> LostWoods", text)
        self.assertIn("age child -> adult", text)

    def test_body_wins_over_age(self):
        # after an area change Longshot's `age` can lag; the module's `body` (our Link actor's name) does not
        text, first = kit_look.look(state(age="child", body="adult"))
        self.assertIn("Link adult,", text)
        self.assertEqual(first["age"], "adult")
        text, _ = kit_look.look(state(age="child", body=None))
        self.assertIn("Link child,", text)

    def test_no_world_says_why_and_stale_says_so(self):
        text, _ = kit_look.look(state(world={"why": "solo: no co-op session", "actors": []}))
        self.assertIn("none listed (solo: no co-op session)", text)
        text, _ = kit_look.look(state(snapshot_stale=True, pos=None, not_ready="old sample"))
        self.assertIn("stale or missing (old sample)", text)

    def test_event_is_named_and_text_marked_unreadable(self):
        text, _ = kit_look.look(state(event=True))
        self.assertIn("EVENT running", text)
        self.assertIn("C: not readable", text)

    def test_more_than_the_cap_is_counted(self):
        w = {"why": None, "actors": [actor(i, "E", "enemy", [i, 0, 0], 2, True) for i in range(1, 11)]}
        text, _ = kit_look.look(state(world=w))
        self.assertIn("enemy: 4 more", text)


if __name__ == "__main__":
    unittest.main()
