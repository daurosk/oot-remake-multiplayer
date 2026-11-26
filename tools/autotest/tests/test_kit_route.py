"""kit_route: planning over recorded routes, go_to, recording (no game)."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import kit_library  # noqa: E402
import kit_route  # noqa: E402

DATA = {"kit": 1, "scene": "TestRoom",
        "positions": {"a": {"x": 0, "z": 0}, "b": {"x": 10, "z": 0}, "c": {"x": 10, "z": 10}, "d": {"x": 0, "z": 10},
                      "far": {"x": 100, "z": 100}},
        "routes": {"east": {"start": "a", "steps": ["b", "c"]}, "back": {"start": "d", "steps": ["a"]}},
        "stuck_spots": [{"x": 10, "z": 5, "radius": 2, "note": "A wall.", "fix": "Go round."}]}


class FakeWalk:
    """A client whose walk_to is replaced: a wall stops it at z >= 6 (stuck) unless `free`."""
    def __init__(self, pos, free=True, scene_at=None, life=12):
        self.pos, self.free, self.scene_at, self.scene, self.life = list(pos), free, scene_at, "TestRoom", life
        self.walks = []

    def state(self):
        return {"pos": list(self.pos), "scene": self.scene, "life": self.life, "event": False}


class Walker:
    def __init__(self, client):
        self.c = client

    def __call__(self, client, x, z, tolerance=0.5, max_frames=600, chunk=8):
        c = self.c
        c.walks.append((x, z))
        if c.scene_at == (x, z):
            c.scene = "Other"
            return {"arrived": False, "reason": "scene changed", "frames": 10, "state": c.state()}
        if not c.free and z > 6:
            c.pos = [x, 0, 6]
            return {"arrived": False, "reason": "timed out", "frames": 240, "state": c.state()}
        c.pos = [x, 0, z]
        return {"arrived": True, "reason": "arrived", "frames": 20, "state": c.state()}


class RouteTest(unittest.TestCase):
    def setUp(self):
        self.orig = kit_route.pc.walk_to

    def tearDown(self):
        kit_route.pc.walk_to = self.orig

    def go(self, client, **kw):
        kit_route.pc.walk_to = Walker(client)
        return kit_route.go_to(client, DATA, **kw)

    def test_slug(self):
        self.assertEqual(kit_route.slug("InsideTheDekuTree"), "inside-the-deku-tree")
        self.assertEqual(kit_route.slug("KokiriForest_Child"), "kokiri-forest-child")

    def test_plan_follows_directed_routes(self):
        self.assertEqual([n for n, *_ in kit_route.plan(DATA, [1, 0, 1], "c")], ["a", "b", "c"])
        self.assertEqual([n for n, *_ in kit_route.plan(DATA, [9, 0, 1], "c")], ["b", "c"])
        self.assertIsNone(kit_route.plan(DATA, [9, 0, 1], "a"))       # routes are one way: b -> a is not recorded
        self.assertIsNone(kit_route.plan(DATA, [50, 0, 50], "c"))     # no named place within 6 m
        self.assertIsNone(kit_route.plan(DATA, [0, 0, 0], "nowhere"))

    def test_go_to_walks_each_waypoint(self):
        c = FakeWalk([0, 0, 1])
        out = self.go(c, goal="c")
        self.assertTrue(out["arrived"])
        self.assertEqual(out["plan"], ["a", "b", "c"])
        self.assertEqual(c.walks, [(0, 0), (10, 0), (10, 10)])

    def test_no_route_walks_straight_and_says_so(self):
        c = FakeWalk([50, 0, 50])
        out = self.go(c, goal="far")
        self.assertTrue(out["arrived"])
        self.assertIn("straight", out["note"])

    def test_stuck_names_the_waypoint_and_the_spot_note(self):
        c = FakeWalk([0, 0, 1], free=False)
        out = self.go(c, goal="c")
        self.assertFalse(out["arrived"])
        self.assertEqual((out["at"], out["reason"]), ("c", "timed out"))
        self.assertIn("Go round", out["stuck_note"])

    def test_scene_change_is_reported(self):
        c = FakeWalk([0, 0, 1], scene_at=(10, 0))
        out = self.go(c, goal="c")
        self.assertEqual((out["arrived"], out["reason"], out["scene"]), (False, "scene changed", "Other"))

    def test_hurt_stops_after_the_leg(self):
        c = FakeWalk([0, 0, 1])
        orig = Walker.__call__

        def hurting(self, *a, **k):
            r = orig(self, *a, **k)
            self.c.life = 8
            r["state"] = self.c.state()
            return r
        kit_route.pc.walk_to = lambda *a, **k: hurting(Walker(c), *a, **k)
        out = kit_route.go_to(c, DATA, goal="c")
        self.assertIn("hurt", out["reason"])
        self.assertEqual(len(c.walks), 1)

    def test_a_hit_mid_leg_stops_too(self):
        c = FakeWalk([0, 0, 1], free=False)       # the leg times out, and Link lost life on the way
        c.life = 8
        c2 = FakeWalk([0, 0, 1], free=False)
        orig = Walker.__call__

        def hurt_then_stuck(self, *a, **k):
            r = orig(self, *a, **k)
            self.c.life = 4
            r["state"] = self.c.state()
            return r
        kit_route.pc.walk_to = lambda *a, **k: hurt_then_stuck(Walker(c2), *a, **k)
        out = kit_route.go_to(c2, DATA, goal="c")
        self.assertIn("hurt", out["reason"])

    def test_sampling_keeps_the_corner_of_an_l_shaped_walk(self):
        class Raw:
            def __init__(self):
                self.pos = [0.0, 1.0, 0.0]

            def state(self):
                return {"pos": list(self.pos), "scene": "S"}

            def move(self, yaw, strength=1.0, frames=1, check=True):
                import math
                self.pos[0] += math.sin(math.radians(yaw)) * 1.0
                self.pos[2] += math.cos(math.radians(yaw)) * 1.0
                return {"ok": True, "state": {"pos": list(self.pos), "scene": "S"}}

        raw = Raw()
        rec = kit_route.Recorder("S")
        rec.add(raw.pos, "S")
        c = kit_route.Sampling(raw, rec)
        for _ in range(20):
            c.move(90)             # 20 m east ...
        for _ in range(60):
            c.move(0)              # ... then 60 m north: more than the walk_to trace keeps
        corner = [p for p in rec.points if abs(p[0] - 20) < 1 and abs(p[2]) < 2.5]
        self.assertTrue(corner, rec.points)
        self.assertEqual(c.state()["scene"], "S")     # other calls pass through
        self.assertLessEqual(len(rec.points), kit_route.MAX_POINTS)

    def test_a_failed_check_leaves_no_temp_file(self):
        with tempfile.TemporaryDirectory() as folder:
            rec = kit_route.Recorder("S")
            rec.add([0, 0, 0], "S")
            rec.add([5, 0, 0], "S")
            orig = kit_route.kit_library.check_data
            kit_route.kit_library.check_data = lambda p: (_ for _ in ()).throw(RuntimeError("boom"))
            try:
                with self.assertRaises(RuntimeError):
                    kit_route.save_route(rec, "x-route", "", folder)
            finally:
                kit_route.kit_library.check_data = orig
            self.assertEqual(os.listdir(folder), [])

    def test_unknown_place_and_xz(self):
        c = FakeWalk([0, 0, 1])
        self.assertIn("no position", self.go(c, goal="zzz")["reason"])
        out = self.go(c, x=3, z=4)
        self.assertTrue(out["arrived"])
        self.assertEqual(c.walks[-1], (3, 4))

    def test_record_and_save_a_route_that_passes_the_library_check(self):
        with tempfile.TemporaryDirectory() as folder:
            rec = kit_route.Recorder("Some_Scene")
            for p in ([0, 1, 0], [0.5, 1, 0.5], [3, 1, 0], [6, 1, 1]):
                rec.add(p, "Some_Scene")
            rec.add([9, 1, 9], "Elsewhere")                           # another scene: not kept
            self.assertEqual(len(rec.points), 3)                      # 0.5 m from the last: thinned
            out = kit_route.save_route(rec, "to-the-gate", "Walked from the start to the gate.", folder)
            self.assertEqual(out["points"], 3)
            path = os.path.join(folder, "some-scene.json")
            self.assertEqual(kit_library.check_data(path), [])
            with open(path, encoding="utf-8") as f:
                d = json.load(f)
            self.assertEqual(d["routes"]["to-the-gate"]["steps"], ["to-the-gate-2", "to-the-gate-3"])
            with self.assertRaises(ValueError):                       # the same name twice
                kit_route.save_route(rec, "to-the-gate", "", folder)
            self.assertEqual(kit_route.load("Some_Scene", folder)["scene"], "Some_Scene")

    def test_save_refuses_short_and_badly_named(self):
        with tempfile.TemporaryDirectory() as folder:
            rec = kit_route.Recorder("S")
            rec.add([0, 0, 0], "S")
            with self.assertRaises(ValueError):
                kit_route.save_route(rec, "x", "", folder)
            rec.add([5, 0, 0], "S")
            with self.assertRaises(ValueError):
                kit_route.save_route(rec, "Bad Name", "", folder)
            self.assertEqual(os.listdir(folder), [])


if __name__ == "__main__":
    unittest.main()
