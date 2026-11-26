"""kit_wait: wait_until on a scripted client (no game)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import kit_wait  # noqa: E402


class Client:
    """Each state() answers the next scripted state (the last one repeats); step() counts frames."""
    def __init__(self, states):
        self.states, self.i, self.frames = states, 0, 0

    def state(self):
        s = self.states[min(self.i, len(self.states) - 1)]
        self.i += 1
        return s

    def step(self, n):
        self.frames += n


def st(**kw):
    s = {"scene": "A", "life": 12, "event": False, "loading": False, "prompt": None, "pos": [0, 5.0, 0],
         "world": {"actors": []}}
    s.update(kw)
    return s


def enemy(alive=True):
    return {"world": {"actors": [{"id": 3, "kind": "enemy", "alive": alive, "life": 4 if alive else 0, "dist": 3.0}]}}


def stale(**kw):
    return {"world": dict({"actors": [], "stale": True, "why": "no walk for 900 ms (a load, or the session ended)"}, **kw)}


def no_session():
    return {"world": {"actors": [], "why": "no co-op session"}}


class WaitTest(unittest.TestCase):
    def test_a_stale_list_is_not_an_empty_room(self):
        c = Client([st(**enemy()), st(**stale())])
        out, _ = kit_wait.wait_until(c, "enemies_clear", max_frames=16)
        self.assertFalse(out["met"])
        self.assertIn("no walk for", out["why"])

    def test_no_session_is_not_an_empty_room(self):
        out, _ = kit_wait.wait_until(Client([st(**no_session())]), "enemies_clear", max_frames=8)
        self.assertFalse(out["met"])
        self.assertIn("no co-op session", out["why"])

    def test_a_stale_list_is_not_a_death(self):
        c = Client([st(**enemy()), st(**stale())])
        out, _ = kit_wait.wait_until(c, "enemy_dead", ident=3, max_frames=16)
        self.assertFalse(out["met"])

    def test_gone_after_seen_is_met_but_says_gone(self):
        c = Client([st(**enemy()), st()])
        out, _ = kit_wait.wait_until(c, "enemy_dead", ident=3, max_frames=16)
        self.assertTrue(out["met"])
        self.assertIn("gone", out["note"])

    def test_a_string_id_is_a_number(self):
        c = Client([st(**enemy()), st(**enemy(False))])
        out, _ = kit_wait.wait_until(c, "enemy_dead", ident="3", max_frames=16)
        self.assertTrue(out["met"])
        with self.assertRaises(ValueError):
            kit_wait.wait_until(c, "enemy_dead", ident="x")

    def test_already_true_costs_no_frame(self):
        c = Client([st(event=False)])
        out, _ = kit_wait.wait_until(c, "event_over")
        self.assertTrue(out["met"])
        self.assertEqual(c.frames, 0)

    def test_event_over_after_some_chunks(self):
        c = Client([st(event=True), st(event=True), st(event=False)])
        out, _ = kit_wait.wait_until(c, "event_over", chunk=10)
        self.assertEqual((out["met"], out["frames_run"], c.frames), (True, 20, 20))

    def test_enemy_dead_listed_dead_or_gone(self):
        out, _ = kit_wait.wait_until(Client([st(**enemy()), st(**enemy(False))]), "enemy_dead", ident=3)
        self.assertTrue(out["met"])
        out, _ = kit_wait.wait_until(Client([st(**enemy()), st()]), "enemy_dead", ident=3)
        self.assertTrue(out["met"])
        out, _ = kit_wait.wait_until(Client([st()]), "enemy_dead", max_frames=8, ident=3)   # never listed
        self.assertFalse(out["met"])
        self.assertIn("never listed", out["why"])
        with self.assertRaises(ValueError):
            kit_wait.wait_until(Client([st()]), "enemy_dead")

    def test_no_world_list_is_unknown_not_dead(self):
        s = st()
        s["world"] = None
        out, _ = kit_wait.wait_until(Client([s]), "enemy_dead", max_frames=16, ident=3)
        self.assertFalse(out["met"])
        self.assertIn("no world list", out["why"])

    def test_stops_when_hurt_or_dead_or_event_or_scene(self):
        out, _ = kit_wait.wait_until(Client([st(**enemy()), st(life=8, **enemy())]), "enemy_dead", ident=3)
        self.assertIn("hurt", out["stopped"])
        out, _ = kit_wait.wait_until(Client([st(), st(life=0)]), "prompt")
        self.assertEqual(out["stopped"], "dead")
        out, _ = kit_wait.wait_until(Client([st(), st(event=True)]), "prompt")
        self.assertEqual(out["stopped"], "event")
        out, _ = kit_wait.wait_until(Client([st(), st(scene="B")]), "prompt")
        self.assertEqual(out["stopped"], "scene_changed")
        out, _ = kit_wait.wait_until(Client([st(), st(scene="B")]), "scene_changed")
        self.assertTrue(out["met"])

    def test_on_ground_needs_a_steady_height(self):
        ys = [10, 8, 6, 5.0, 5.0, 5.01]
        c = Client([st(pos=[0, y, 0]) for y in ys])
        out, _ = kit_wait.wait_until(c, "on_ground")
        self.assertTrue(out["met"])
        self.assertEqual(out["frames_run"], 5 * kit_wait.CHUNK)

    def test_timeout_and_bad_args(self):
        out, _ = kit_wait.wait_until(Client([st()]), "prompt", max_frames=20, chunk=8)
        self.assertFalse(out["met"])
        self.assertEqual(out["frames_run"], 20)
        for bad in (dict(what="nope"), dict(what="prompt", max_frames=0), dict(what="prompt", chunk=999)):
            with self.assertRaises(ValueError):
                kit_wait.wait_until(Client([st()]), **bad)

    def test_condition(self):
        out, _ = kit_wait.wait_until(Client([st(life=4), st(life=12)]), "condition", condition="life >= 12")
        self.assertTrue(out["met"])


if __name__ == "__main__":
    unittest.main()
