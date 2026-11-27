"""Selftests of the 'host starved' reading of a set-up/boot frame timeout (pair.starved_note, Pair.starved): no game.
Lines are the game's own vi lines (Longshot 'vi: N frames presented (F fps over the last S s, interval 1)'), values from
downed-move-53d40ab3-2 (the WSL CI VM building, both games slow) and tunic-look-53d40ab3-2 (no CI, one load dip in A)."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, INGAME)
import pair as P          # noqa: E402


def vi(*samples):
    """[(fps, seconds), ...] -> the game's vi lines, with other log lines between them."""
    out, n = [], 0
    for fps, s in samples:
        n += int(fps * s)
        out += ["[hos host] coop: objects: something else",
                "[hos Presentation Thread] vi: %d frames presented (%.1f fps over the last %.1f s, interval 1)" % (n, fps, s)]
    return out


FAST = vi(*[(58.0, 5.0)] * 30)
# downed-move-53d40ab3-2, the last samples of each game (game-out-A/B.txt)
A_STARVED = FAST + vi((7.0, 8.0), (6.3, 5.0), (10.3, 5.0), (3.4, 5.0), (0.2, 33.0), (12.2, 5.0), (15.2, 5.0))
B_STARVED = FAST + vi((0.1, 8.0), (0.1, 16.0), (0.2, 6.0), (0.4, 5.0), (6.0, 5.0), (9.5, 5.7), (4.3, 9.8))
# tunic-look-53d40ab3-2: A dipped twice alone (a load), B never
A_DIP = FAST + vi((1.8, 5.0), (1.3, 6.0), (40.0, 5.0))


class StarvedNote(unittest.TestCase):
    def test_both_games_slow_at_once_is_host_starved(self):
        note = P.starved_note({"A": A_STARVED, "B": B_STARVED})
        self.assertTrue(note.startswith("host starved: "), note)
        self.assertIn("A 3.4/0.2 fps", note)
        self.assertIn("B 0.1/0.1/0.2/0.4/4.3 fps", note)
        self.assertIn("not the test", note)

    def test_one_game_slow_alone_is_that_game(self):
        note = P.starved_note({"A": A_DIP, "B": FAST})
        self.assertEqual(note, "A alone slow (A 1.8/1.3 fps in its last 90 s): that game stalled, not the host")

    def test_fast_games_one_dip_or_no_vi_lines_say_nothing(self):
        self.assertIsNone(P.starved_note({"A": FAST, "B": FAST}))
        self.assertIsNone(P.starved_note({"A": FAST + vi((0.5, 5.0)), "B": FAST + vi((0.5, 5.0))}))   # one dip each
        self.assertIsNone(P.starved_note({"A": ["no vi lines"], "B": []}))

    def test_only_the_last_window_counts(self):
        # slow long ago, fast for the last 90 s: not starved now
        self.assertIsNone(P.starved_note({"A": A_STARVED + FAST, "B": B_STARVED + FAST}))
        self.assertEqual(P.slow_samples(vi((1.0, 50.0), (1.0, 50.0), (1.0, 50.0))), ([1.0, 1.0], 2))


class PairStarved(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.pair = P.Pair.__new__(P.Pair)      # (no lane: only the logs Pair.starved reads)
        self.pair.log = {}
        for x, lines in (("A", A_STARVED), ("B", B_STARVED)):
            path = os.path.join(self.tmp.name, "game-out-%s.txt" % x)
            with open(path, "w", encoding="utf-8") as f:
                f.write("\n".join(lines) + "\n")
            self.pair.log[x] = P.Log(path)          # (its mark is the file's end: starved reads from 0)

    def tearDown(self):
        self.tmp.cleanup()

    def test_a_frame_timeout_gets_the_reading(self):
        why = self.pair.starved("set-up: BridgeError: timeout: the frames did not run in time")
        self.assertTrue(why.startswith("set-up: BridgeError: timeout: the frames did not run in time; host starved: "), why)
        self.assertIn("boot: TimeoutError: timed out; host starved: ", self.pair.starved("boot: TimeoutError: timed out"))

    def test_other_failures_are_left_as_they_are(self):
        self.assertEqual(self.pair.starved("set-up: AssertionError: A: no sword"), "set-up: AssertionError: A: no sword")
        self.assertIn("host starved", self.pair.starved("set-up took 61.0 s (limit 30 s)", slow=True))

    def test_no_logs_or_a_read_error_never_hides_the_failure(self):
        self.pair.log = {}
        self.assertEqual(self.pair.starved("boot: TimeoutError: timed out"), "boot: TimeoutError: timed out")

        class Broken:
            def since(self, pos=None):
                raise OSError("gone")
        self.pair.log = {"A": Broken(), "B": Broken()}
        self.assertEqual(self.pair.starved("boot: TimeoutError: timed out"), "boot: TimeoutError: timed out")

    def test_setup_wraps_a_frame_timeout_with_the_reading(self):
        import playtest_client as pc
        self.pair.deadline = P.Deadline(600)
        self.pair.notes = []
        with self.assertRaises(P.SetupFail) as cm:
            with self.pair.setup():
                raise pc.BridgeError({"error": "timeout", "message": "the frames did not run in time"})
        self.assertIn("did not run in time; host starved: ", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
