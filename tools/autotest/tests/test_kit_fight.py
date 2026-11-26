"""kit_fight: fight on a scripted game (no game here)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import kit_fight  # noqa: E402
import playtest_client as pc  # noqa: E402


class Game:
    """Link at the origin, one enemy (id 3). A new press of the sword button takes `dmg` life off it when it is in
    reach and the lock-on button is held with it (a sword held from the last input is no new swing); `bite` takes
    Link's life off after each slash or guard."""
    def __init__(self, dist=6.0, life=3, dmg=1, bite=0, link_life=12, hits_work=True, scene_after=None, stale_after=None):
        self.dist, self.life, self.dmg, self.bite, self.link = dist, life, dmg, bite, link_life
        self.hits_work, self.scene_after, self.stale_after = hits_work, scene_after, stale_after
        self.presses, self.frames, self.moves, self.rounds, self.scene = [], [], [], 0, "A"
        self.steps, self.held = 0, []
        self.gone, self.event = False, False

    def state(self):
        return {"scene": self.scene, "life": self.link, "max_life": 12, "pos": [0.0, 0.0, 0.0], "event": self.event,
                "loading": False}

    def world(self):
        if self.stale_after is not None and self.rounds >= self.stale_after:
            return {"frame": 1, "world": {"actors": [], "stale": True, "why": "no walk for 900 ms"}}
        acts = [] if self.gone else [{"id": 3, "name": "Deku Baba", "kind": "enemy", "pos": [self.dist, 0.0, 0.0],
                                      "dist": self.dist, "life": self.life, "alive": self.life > 0}]
        return {"frame": 1, "world": {"actors": acts, "frame": 1}}

    def input(self, stick, buttons, frames, rstick=None):
        self.presses.append(list(buttons))
        self.frames.append(frames)
        new_swing = "Y" in buttons and "Y" not in self.held
        self.held = list(buttons)
        if self.hits_work and new_swing and "ZL" in buttons and self.dist <= kit_fight.REACH_M:
            self.life = max(0, self.life - self.dmg)
        if buttons != ["ZL"] or frames == kit_fight.SHIELD_FRAMES:   # (a swing's let-go is part of its slash)
            self.link = max(0, self.link - self.bite)
            self.rounds += 1
        if self.scene_after is not None and self.rounds >= self.scene_after:
            self.scene = "B"
        return {"ok": True}

    def step(self, n):
        self.steps += 1
        return {"ok": True}

    def move(self, yaw, strength=1.0, frames=1, check=True):
        self.moves.append(yaw)
        self.held = []
        return {"ok": True}


class FightTest(unittest.TestCase):
    def setUp(self):
        self.orig = pc.walk_to
        self.game = None

        def walk(client, x, z, tolerance=0.5, max_frames=600, chunk=8):
            client.dist = tolerance
            return {"arrived": True, "frames": 8, "state": client.state()}
        pc.walk_to = walk

    def tearDown(self):
        pc.walk_to = self.orig

    def test_walks_up_then_slashes_until_dead(self):
        g = Game(dist=6.0, life=3)
        out = kit_fight.fight(g, 3, "Deku Baba")
        self.assertEqual((out["result"], out["life_before"], out["life_after"]), ("dead", 3, 0))
        self.assertEqual((out["hits"], out["slashes"]), (3, 3))
        # each slash: lock-on and sword, then the sword let go with the lock-on still held
        self.assertEqual(g.presses, [["ZL", "Y"], ["ZL"]] * 3)
        self.assertEqual(g.frames, [kit_fight.SWING_FRAMES, kit_fight.RECOVER_FRAMES] * 3)

    def test_no_step_after_an_input_so_the_lock_on_is_never_let_go(self):
        g = Game(dist=1.0, life=5, bite=1, link_life=40)   # (hurt, so it guards too; never low)
        out = kit_fight.fight(g, 3)
        self.assertEqual(g.steps, 0)
        self.assertGreaterEqual(out["guards"], 1)
        self.assertTrue(all("ZL" in p for p in g.presses))
        self.assertEqual(out["frames"], sum(g.frames))

    def test_the_default_buttons_are_longshots_map(self):
        self.assertEqual((kit_fight.LOCK, kit_fight.SWORD, kit_fight.SHIELD), ("ZL", "Y", "ZL"))

    def test_hurt_makes_link_shield_once_then_slash(self):
        g = Game(dist=1.0, life=4, bite=1)
        out = kit_fight.fight(g, 3)
        self.assertGreaterEqual((out["hurt"], out["guards"]), (1, 1))
        guards = [i for i, f in enumerate(g.frames) if f == kit_fight.SHIELD_FRAMES]
        self.assertTrue(guards)
        for i in guards:
            self.assertEqual(g.presses[i], ["ZL"])                       # the shield is ZL held without the sword
            self.assertEqual(g.presses[i + 1], ["ZL", "Y"])              # never two guards in a row

    def test_low_life_backs_off_and_stops(self):
        g = Game(dist=1.0, life=50, bite=3, link_life=9)
        out = kit_fight.fight(g, 3)
        self.assertEqual(out["result"], "low")
        self.assertEqual(len(g.moves), 1)
        self.assertAlmostEqual(g.moves[0], 270.0)                       # the enemy is at +x: away is -x (yaw 270)
        self.assertLessEqual(out["link_after"], kit_fight.MIN_LIFE)

    def test_a_scene_change_stops_the_fight_not_a_death(self):
        g = Game(dist=1.0, life=50, scene_after=2)
        out = kit_fight.fight(g, 3)
        self.assertEqual((out["result"], out["stopped"]), ("stopped", "scene changed"))

    def test_a_stale_list_is_not_a_death(self):
        g = Game(dist=1.0, life=50, stale_after=2)
        out = kit_fight.fight(g, 3)
        self.assertEqual(out["result"], "stopped")
        self.assertIn("stale", out["stopped"])

    def test_gone_is_not_called_dead(self):
        g = Game(dist=1.0, life=50)
        orig = g.input

        def vanish(stick, buttons, frames, rstick=None):
            g.gone = True
            return orig(stick, buttons, frames, rstick)
        g.input = vanish
        self.assertEqual(kit_fight.fight(g, 3)["result"], "gone")

    def test_life_that_never_moves_says_the_buttons_may_be_wrong(self):
        g = Game(dist=1.0, life=50, hits_work=False)
        out = kit_fight.fight(g, 3, max_frames=60)
        self.assertEqual(out["result"], "alive")
        self.assertIn("lock-on ZL, sword Y", out["hint"])
        self.assertIn("Gohma", out["hint"])

    def test_other_buttons_can_be_given(self):
        g = Game(dist=1.0, life=1)
        kit_fight.fight(g, 3, lock="L", sword="B", shield="R", max_frames=24)
        self.assertEqual(g.presses[:2], [["L", "B"], ["L"]])

    def test_a_shield_button_apart_from_the_lock_on_is_held_with_it(self):
        g = Game(dist=1.0, life=50, bite=1)
        kit_fight.fight(g, 3, shield="R", max_frames=60)
        self.assertIn(["ZL", "R"], g.presses)

    def test_bad_frame_limit_is_refused(self):
        with self.assertRaises(ValueError):
            kit_fight.fight(Game(), 3, max_frames=0)


    def test_an_event_stops_the_fight_and_nothing_is_pressed(self):
        g = Game(dist=1.0, life=3)
        g.event = True                                                   # a cutscene (Gohma's entrance, a title card)
        out = kit_fight.fight(g, 3)
        self.assertEqual((out["result"], out["stopped"]), ("stopped", "event"))
        self.assertEqual((g.presses, out["life_after"]), ([], 3))

    def test_a_walk_up_that_stops_for_an_event_ends_the_fight(self):
        g = Game(dist=6.0, life=3)
        calls = []

        def walk(client, x, z, tolerance=0.5, max_frames=600, chunk=8):
            calls.append(max_frames)
            return {"arrived": False, "reason": "event", "frames": 0, "distance": 6.0, "state": client.state()}
        pc.walk_to = walk
        out = kit_fight.fight(g, 3)
        self.assertEqual((out["result"], out["stopped"], len(calls), out["rounds"]), ("stopped", "event", 1, 0))

    def test_a_refused_walk_up_ends_the_fight(self):
        g = Game(dist=6.0, life=3)

        def walk(client, x, z, tolerance=0.5, max_frames=600, chunk=8):
            return {"arrived": False, "reason": "move refused: no_camera: no camera", "frames": 0, "distance": 6.0}
        pc.walk_to = walk
        out = kit_fight.fight(g, 3)
        self.assertEqual(out["result"], "stopped")
        self.assertTrue(out["stopped"].startswith("move refused"))

    def test_walk_ups_that_get_no_nearer_end_as_stuck(self):
        g = Game(dist=6.0, life=3)
        calls = []

        def walk(client, x, z, tolerance=0.5, max_frames=600, chunk=8):
            calls.append(1)
            return {"arrived": False, "reason": "timed out", "frames": max_frames, "distance": 5.9}
        pc.walk_to = walk
        out = kit_fight.fight(g, 3)
        self.assertEqual((out["result"], out["stopped"]), ("stopped", "stuck"))
        self.assertEqual(len(calls), kit_fight.STUCK_WALKS)
        self.assertEqual(out["frames"], kit_fight.STUCK_WALKS * kit_fight.APPROACH_FRAMES)

    def test_the_frame_limit_is_never_passed(self):
        for limit in (1, 11, 12, 25, 100):
            g = Game(dist=1.0, life=500, hits_work=False)
            out = kit_fight.fight(g, 3, max_frames=limit)
            self.assertEqual(out["result"], "alive")
            self.assertLessEqual(out["frames"], limit)
            self.assertEqual(out["frames"], sum(g.frames))

    def test_a_hit_in_the_last_round_is_counted(self):
        g = Game(dist=1.0, life=500, bite=1, link_life=40, hits_work=False)
        out = kit_fight.fight(g, 3, max_frames=kit_fight.SWING_FRAMES + kit_fight.RECOVER_FRAMES)
        self.assertEqual((out["rounds"], out["hurt"]), (1, 1))


if __name__ == "__main__":
    unittest.main()
