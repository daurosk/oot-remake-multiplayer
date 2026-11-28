"""Selftests of pair.State / StaleState / Pair.pos (no game).

gate-epona-182cd53b (2026-11-28 05:03 UTC) SETUP-FAILed with "TypeError: 'NoneType' object is not subscriptable":
test_epona.face_rider read pair.state("B")["pos"] right after a place while B's Link snapshot was old (the state's
Link members are null while `not_ready` says why, docs/playtest.md). A null Link member must give a clear message.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ingame"))
import pair as P  # noqa: E402

STALE = {"scene": "HyruleField_Adult", "pos": None, "facing": None, "loading": None, "room": None,
         "not_ready": "Link sample older than the frame"}
FRESH = {"scene": "HyruleField_Adult", "pos": [1.0, 2.0, 3.0], "facing": 90.0, "loading": False, "room": None,
         "prompt": None, "not_ready": None}


class StubPair(P.Pair):
    """A Pair with no games: state replies from a script, steps counted."""

    def __init__(self, replies):
        self.replies = list(replies)
        self.steps = 0

    def call(self, x, cmd, **args):
        assert cmd == "state"
        return dict(self.replies.pop(0) if len(self.replies) > 1 else self.replies[0])

    def step_both(self, frames):
        self.steps += frames


class StateTest(unittest.TestCase):
    def test_stale_member_raises_clear(self):
        st = P.State("B", STALE)
        with self.assertRaises(P.StaleState) as c:
            st["pos"][0]
        msg = str(c.exception)
        self.assertIn("B: state['pos'] is null", msg)
        self.assertIn("Link sample older than the frame", msg)
        self.assertIn("pair.pos('B')", msg)

    def test_get_and_other_keys_unchanged(self):
        st = P.State("B", STALE)
        self.assertIsNone(st.get("pos"))
        self.assertIsNone(st["room"])               # (not a Link member: null is its own meaning)
        self.assertEqual(st["scene"], "HyruleField_Adult")
        with self.assertRaises(KeyError):
            st["nope"]
        self.assertIsInstance(st, dict)

    def test_fresh_null_prompt_is_fine(self):
        st = P.State("A", FRESH)
        self.assertIsNone(st["prompt"])             # (fresh sample: null = no prompt, not stale)
        self.assertEqual(st["pos"], [1.0, 2.0, 3.0])

    def test_stale_is_setup_fail_inside_setup_text(self):
        self.assertTrue(issubclass(P.StaleState, AssertionError))


class PosTest(unittest.TestCase):
    def test_pos_steps_until_fresh(self):
        p = StubPair([STALE, STALE, FRESH])
        self.assertEqual(p.pos("B"), [1.0, 2.0, 3.0])
        self.assertEqual(p.steps, 2 * P.CHUNK)

    def test_pos_fresh_costs_no_frame(self):
        p = StubPair([FRESH])
        self.assertEqual(p.pos("A"), [1.0, 2.0, 3.0])
        self.assertEqual(p.steps, 0)

    def test_pos_never_fresh_raises_stale(self):
        p = StubPair([STALE])
        with self.assertRaises(P.StaleState) as c:
            p.pos("B", max_frames=30)
        self.assertIn("B: pos still null", str(c.exception))
        self.assertIn("Link sample older than the frame", str(c.exception))
        self.assertEqual(p.steps, 30)

    def test_pos_keeps_test_timeout(self):
        p = StubPair([STALE])

        def over(frames):
            raise P.TestTimeout("the test ran past its deadline")
        p.step_both = over
        with self.assertRaises(P.TestTimeout) as c:
            p.pos("B")
        self.assertNotIsInstance(c.exception, P.StaleState)
        self.assertIn("past its deadline", str(c.exception))

    def test_state_wraps(self):
        p = StubPair([STALE])
        st = p.state("B")
        self.assertIsInstance(st, P.State)
        with self.assertRaises(P.StaleState):
            st["pos"]


class FlagGuardTest(unittest.TestCase):
    """Pair.flag refuses a name the game has no flag for (probe0121e-16ac9701 05:07: 'coop: test setenum Get_Epona
    Complete: unknown (was -)', test_epona's GET_EPONA was not the quest's Enum name)."""

    def pair(self, kind):
        p = StubPair([FRESH])
        p.writes = []
        p.flag_read = lambda x, name: {"kind": kind, "value": None}
        p._kept = lambda x, cmd, **a: p.writes.append((x, cmd, a)) or {"ok": True}
        return p

    def test_unknown_name_is_setup_fail(self):
        p = self.pair("unknown")
        with self.assertRaises(P.SetupFail) as c:
            p.flag("B", "Get_Epona", "Complete")
        self.assertIn("B: flag 'Get_Epona' is unknown to the game", str(c.exception))
        self.assertEqual(p.writes, [])

    def test_known_name_is_written(self):
        p = self.pair("enum")
        p.flag("B", "Step_Get_Epona", "Complete")
        self.assertEqual(p.writes, [("B", "flag", {"name": "Step_Get_Epona", "value": "Complete"})])


if __name__ == "__main__":
    unittest.main()
