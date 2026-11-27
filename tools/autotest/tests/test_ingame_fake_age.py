"""Selftests of the fake game's age (ingame/fakegame.py FakeGame.age): one source, the game's own Sequence_IsChildhood
flag, with every fake hook loaded. Cause (integrate-0120-4 18:59): fakes/dbno_crawl.py set st['age'] for every fake
boot from that flag (unset = child) and ran after fakes/boss_awake.py, so row 286's adult boot (live-twinrova-room)
read child and its 4 wake selftests SETUP-FAILed 'A is child, this test needs adult Link'."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ingame"))
import fakegame  # noqa: E402


class FakeAge(unittest.TestCase):
    def lane(self, env=(), boot=""):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        lane = fakegame.FakeLane(tmp, env=env, boot=boot)     # (every fake in ingame/fakes, as a fake run loads them)
        self.addCleanup(lane.close)
        return lane

    def ages(self, lane):
        return {x: lane.g[x].state()["age"] for x in ("A", "B")}

    def test_a_plain_boot_is_child(self):
        self.assertEqual(self.ages(self.lane()), {"A": "child", "B": "child"})

    def test_the_adult_twinrova_boot_stays_adult_with_every_fake_loaded(self):
        """boss_awake's boot sets the flag; no other hook (dbno_crawl sorts after it) turns it back to child."""
        self.assertEqual(self.ages(self.lane(env=["LICHII_COOP_DEBUG_BOSS=1"], boot="live-twinrova-room")),
                         {"A": "adult", "B": "adult"})

    def test_the_boss_switch_alone_keeps_a_child_boot_child(self):
        """The switch is on for other boss boots too (live-deku-gohma-reset, a child save: test_boss_reset); only
        live-twinrova-room's save is adult (boss_awake.ADULT_BOOT)."""
        self.assertEqual(self.ages(self.lane(env=["LICHII_COOP_DEBUG_BOSS=1"], boot="live-deku-gohma-reset")),
                         {"A": "child", "B": "child"})

    def test_the_flag_alone_decides_each_game(self):
        lane = self.lane()
        lane.g["A"].flags["Sequence_IsChildhood"] = 0
        self.assertEqual(self.ages(lane), {"A": "adult", "B": "child"})
        lane.g["A"].flags["Sequence_IsChildhood"] = 1
        self.assertEqual(self.ages(lane)["A"], "child")

    def test_a_warp_to_a_scene_of_the_other_age_sets_the_flag_as_the_real_verb(self):
        """docs/playtest.md 'warp': a name ending in the other age first sets Sequence_IsChildhood; no suffix keeps it."""
        lane = self.lane()
        g = lane.g["A"]
        g.answer({"cmd": "warp", "scene": "KokiriForest_Adult"})
        self.assertEqual((g.flags.get("Sequence_IsChildhood"), self.ages(lane)), (0, {"A": "adult", "B": "child"}))
        g.answer({"cmd": "warp", "scene": "TwinrovaStage"})
        self.assertEqual(self.ages(lane)["A"], "adult")
        g.answer({"cmd": "warp", "scene": "KokiriForest_Child"})
        self.assertEqual((g.flags.get("Sequence_IsChildhood"), self.ages(lane)["A"]), (1, "child"))


if __name__ == "__main__":
    unittest.main()
