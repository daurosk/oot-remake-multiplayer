"""Tests for the pure checks of tools/autotest/bridge_proof.py (no game, no socket)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import bridge_proof as bp  # noqa: E402


def coop(*states):
    return {"coop": {"players": [{"state": s} for s in states]}}


class PlayersConnected(unittest.TestCase):
    def test_connected_and_quiet_count(self):
        self.assertTrue(bp.players_connected(coop("connected", "connected")))
        self.assertTrue(bp.players_connected(coop("connected", "quiet")))     # S8: the other game, held 30 s

    def test_others_and_a_lone_player_do_not(self):
        self.assertFalse(bp.players_connected(coop("connected", "disconnected")))
        self.assertFalse(bp.players_connected(coop("connected")))
        self.assertFalse(bp.players_connected({}))


class WarpVerdict(unittest.TestCase):
    def test_a_refusal_fails_with_its_message_in_the_seen(self):
        ok, seen = bp.warp_verdict({"ok": False, "error": "not_in_event", "message": "not in the list"},
                                   "KokiriForest_Child")
        self.assertFalse(ok)
        self.assertEqual((seen["error"], seen["message"]), ("not_in_event", "not in the list"))

    def test_an_arrival_in_the_scene_while_held_passes(self):
        r = {"ok": True, "arrived": True, "paused": True, "frame": 9, "state": {"scene": "KokiriForest_Child"}}
        self.assertTrue(bp.warp_verdict(r, "KokiriForest_Child")[0])

    def test_the_scene_matches_as_the_module_matches_it(self):   # same rule as WarpArrived: case, and no age ending
        def arrived(scene_in_state, asked, **extra):
            r = dict({"ok": True, "arrived": True, "paused": True, "state": {"scene": scene_in_state}}, **extra)
            return bp.warp_verdict(r, asked)[0]
        self.assertTrue(arrived("KokiriForest", "KokiriForest_Child"))     # the map may not carry the age
        self.assertTrue(arrived("kokiriforest_child", "KokiriForest_Child"))
        self.assertFalse(arrived("Market", "KokiriForest_Child"))
        # Longshot's teleport picks the map for Link's age: the reply's `map` is what to compare with
        self.assertTrue(arrived("KokiriForest_Child", "KokiriForest_Adult", map="KokiriForest_Child"))
        self.assertFalse(arrived("KokiriForest_Adult", "KokiriForest_Adult", map="KokiriForest_Child"))

    def test_wrong_scene_not_held_timeout_or_other_errors_fail(self):
        r = {"ok": True, "arrived": True, "paused": True, "state": {"scene": "DekuTree"}}
        self.assertFalse(bp.warp_verdict(r, "KokiriForest_Child")[0])
        r = {"ok": True, "arrived": True, "paused": False, "state": {"scene": "KokiriForest_Child"}}
        self.assertFalse(bp.warp_verdict(r, "KokiriForest_Child")[0])
        self.assertFalse(bp.warp_verdict({"ok": False, "error": "timeout"}, "KokiriForest_Child")[0])
        self.assertFalse(bp.warp_verdict({"ok": False, "error": "failed"}, "KokiriForest_Child")[0])


class MovedVerdict(unittest.TestCase):
    REPLY = {"ok": True, "from": 100, "frame": 130}

    def test_moved_and_30_frames(self):
        ok, _ = bp.moved_verdict({"pos": [0, 0, 0]}, {"pos": [0, 0, 2.0]}, self.REPLY)
        self.assertTrue(ok)

    def test_did_not_move_or_wrong_frames(self):
        self.assertFalse(bp.moved_verdict({"pos": [0, 0, 0]}, {"pos": [0, 0, 0.1]}, self.REPLY)[0])
        self.assertFalse(bp.moved_verdict({"pos": [0, 0, 0]}, {"pos": [0, 0, 2.0]}, dict(self.REPLY, frame=125))[0])

    def test_a_null_place_is_called_stale_not_unmoved(self):
        ok, seen = bp.moved_verdict({"pos": None, "snapshot_stale": True}, {"pos": [0, 0, 2.0]}, self.REPLY)
        self.assertFalse(ok)
        self.assertIn("stale", seen["why"])


if __name__ == "__main__":
    unittest.main()
