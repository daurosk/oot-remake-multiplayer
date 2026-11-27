"""Pair.warp_one sizes the bridge's warp wait under the deadline with a margin (row-215-confirm, CI 45885309573
windows 3.10: test_twinrova_wake_late SETUP-FAIL 'warp not sent (30.0 s left, its reply needs 30.0 s)'). No game.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ingame"))
import pair as P   # noqa: E402

TICK = 0.0156     # Windows' monotonic clock: about 15.6 ms a tick


class Sent(Exception):
    pass


class Coarse:
    """A clock that stands still until advanced (Windows' coarse tick: reads in one tick are equal)."""
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


class Client:
    last_frame = 0


class WarpMargin(unittest.TestCase):
    def pair(self, seconds):
        """A Pair with no games: its deadline reads exactly `seconds` until the clock ticks, and the clock ticks once
        just before _wait reads it (the CI failure's order: warp_one's sizing read, then _wait's, one tick apart)."""
        clock = Coarse()
        p = P.Pair.__new__(P.Pair)
        p.deadline = P.Deadline(seconds, clock=clock)
        p.c = {"A": Client(), "B": Client()}
        p.notes = []
        got = {}
        wait = p._wait

        def ticked_wait(cmd="call", need=P.REPLY_MIN_S):
            clock.now += TICK
            return wait(cmd, need)
        p._wait = ticked_wait

        def request(x, cmd, w, check=True, **kw):
            got.update(x=x, cmd=cmd, wait=w, **kw)
            raise Sent()
        p._request = request
        return p, got

    def test_a_whole_second_deadline_sends_the_warp(self):
        for seconds in (30.0, 29.0, 12.0, 120.0):
            p, got = self.pair(seconds)
            with self.assertRaises(Sent, msg="%s s: the warp was not sent" % seconds):
                p.warp_one("A", "TwinrovaStage", entrance="TwinrovaStage", arrive_in_event=False)
            self.assertEqual(got["cmd"], "warp")
            self.assertLessEqual(got["timeout"] + P.REPLY_MIN_S, seconds - p.WARP_MARGIN_S)

    def test_a_warp_inside_a_set_up_is_sent(self):
        """In a set-up (30 s, plus a whole warp allowance when the harness gives one) the deadline also reads whole."""
        p, got = self.pair(600.0)
        with self.assertRaises(P.SetupFail) as e:
            with p.setup():
                p.warp_one("A", "TwinrovaStage", entrance="TwinrovaStage", arrive_in_event=False)
        self.assertIn("Sent", str(e.exception))
        self.assertEqual(got["cmd"], "warp")

    def test_the_arrival_wait_keeps_its_own_bound(self):
        p, got = self.pair(120.0)
        with self.assertRaises(Sent):
            p.warp_one("A", "TwinrovaStage", entrance="TwinrovaStage", arrive_in_event=True)
        self.assertEqual(got["timeout"], p.ARRIVE_IN_EVENT_WAIT)

    def test_too_little_left_is_a_timeout_not_a_short_warp(self):
        p, got = self.pair(2.5)
        with self.assertRaises(P.TestTimeout):
            p.warp_one("A", "TwinrovaStage", arrive_in_event=False)
        self.assertEqual(got, {})


if __name__ == "__main__":
    unittest.main()
