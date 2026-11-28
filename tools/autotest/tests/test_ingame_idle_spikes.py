"""Selftests of row 322 (tools/autotest/ingame/test_idle_spikes.py): no game, log lines only."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
AUTOTEST = os.path.dirname(HERE)
INGAME = os.path.join(AUTOTEST, "ingame")
sys.path.insert(0, INGAME)
sys.path.insert(0, AUTOTEST)

# Longshot's own lines (lane log fps-spike-182cd53b game-out-A.txt:4255-4256, 4408)
FPS = "[hos Presentation Thread] vi: 8208 frames presented (%s fps over the last 5.0 s, interval 1)"
BUCK = ("[hos Presentation Thread] vi: present intervals: <15.5 ms 20, 15.5-17.8 136, 17.8-22 61, 22-30 34, "
        "30-40 4, over 40 %d")
STALL = ("[hos AudioFrameworkThread] vcores: AudioFrameworkThread (priority 4) waited 173.6 ms for a core: given by "
         "Presentation Thread (priority 17), which held it 175.1 ms")
NOISE = "[hos host] vk: a frame: mirrors 101 (735 MB): 5.40 MB synced in 1017.5 copies, 19.73 MB written back"


def log(overs, between=None, fps="59.9"):
    out = []
    for i, o in enumerate(overs):
        out += [NOISE] + (between or {}).get(i, []) + [FPS % fps, BUCK % o]
    return out


class IdleSpikes(unittest.TestCase):
    def setUp(self):
        import test_idle_spikes as T
        self.T = T

    def test_windows_pair_with_the_lines_before_them(self):
        got = self.T.windows_with_lines(log([0, 2], {1: [STALL, "[hos x] coop: hud: something ran"]}))
        self.assertEqual(len(got), 2)
        self.assertEqual(got[0][1], [])
        self.assertEqual(got[1][0][2][5], 2)
        self.assertEqual(len(got[1][1]), 2)            # the stall and the module line, not the vk counter line

    def test_clean_idle_passes(self):
        bad, note, facts = self.T.judge_idle(self.T.windows_with_lines(log([0] * 36)))
        self.assertEqual(bad, [])
        self.assertIn("36 windows", note)
        self.assertEqual(facts["spiky"], [])

    def test_the_players_10s_signature_fails_with_its_period(self):
        bad, note, facts = self.T.judge_idle(self.T.windows_with_lines(log([1 if i % 2 else 0 for i in range(36)])))
        self.assertTrue(bad and "freeze" in bad[0])
        self.assertEqual(set(facts["gaps_s"]), {10})

    def test_one_freeze_fails(self):
        bad, _, facts = self.T.judge_idle(self.T.windows_with_lines(log([0] * 10 + [1] + [0] * 25, {10: [STALL]})))
        self.assertTrue(bad)
        self.assertEqual(facts["spiky"][0]["i"], 10)
        self.assertIn("waited 173.6 ms", facts["spiky"][0]["next_to"][0])
        self.assertIn("vcores waits max 173.6 ms", bad[0])

    def test_low_fps_fails(self):
        bad, _, _ = self.T.judge_idle(self.T.windows_with_lines(log([0] * 36, fps="50.0")))
        self.assertTrue(bad and "mean" in bad[0])

    def test_judges_at_least_three_minutes(self):
        self.assertGreaterEqual(self.T.WINDOWS * 5, 180)

    def test_solo_only(self):
        self.assertTrue(any(m.name == "needs_solo" for m in self.T.test_solo_idle_no_spikes.pytestmark))


if __name__ == "__main__":
    unittest.main()
