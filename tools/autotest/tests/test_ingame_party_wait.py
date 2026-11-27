"""Selftests of row 298 (ingame/test_party_wait.py, fakes/party_wait.py) on the fake games, no game.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_ingame as T   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results
DOOR, GIVE_UP, OFF = "test_party_wait_boss_door", "test_party_wait_gives_up", "test_party_wait_off"


class PartyWait(unittest.TestCase):
    """Each passes on a good fake game and FAILS when the thing it guards is broken."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_file(self, env=None, refs=None, only=None):
        out = os.path.join(self.tmp, "o%d" % len(os.listdir(self.tmp)))
        e = dict(env or {})
        e["BT_FAKE_REFS"] = refs or os.path.join(self.tmp, "norefs")
        args = ["test_party_wait.py", "--fake", "--no-rerun", "--out", out, "-q"]
        if only:
            args += ["-k", only]
        p = pytest_run(args, env=e)
        return p, results(summary(out)), out

    def test_good_game(self):
        """The boss door and give-up tests are NO-REF with no stored shots (never PASS), PASS with them; off PASSes."""
        p, r, out = self.run_file()
        self.assertEqual(r, {DOOR: "NO-REF", GIVE_UP: "NO-REF", OFF: "PASS"}, p.stdout[-3000:])
        refs = os.path.join(self.tmp, "refs")
        os.makedirs(refs)
        new = os.path.join(out, "lane1", "new")
        names = sorted(os.listdir(new))
        self.assertEqual(len(names), 5, names)        # A waiting, B's chat, A everyone here, A and B at the give-up
        for f in names:
            shutil.copyfile(os.path.join(new, f), os.path.join(refs, f))
        p, r, _ = self.run_file(refs=refs)
        self.assertEqual(r, {DOOR: "PASS", GIVE_UP: "PASS", OFF: "PASS"}, p.stdout[-3000:])

    def test_no_wait_line_fails(self):
        p, r, _ = self.run_file(env={"BT_FAKE_PARTY_WAIT_BUG": "nowait"})
        self.assertEqual(r[DOOR], "FAIL", p.stdout[-3000:])
        self.assertEqual(r[GIVE_UP], "FAIL", p.stdout[-3000:])
        self.assertIn("1/2 here", p.stdout, p.stdout[-3000:])

    def test_never_giving_up_fails(self):
        p, r, _ = self.run_file(env={"BT_FAKE_PARTY_WAIT_BUG": "nogiveup"}, only=GIVE_UP)
        self.assertEqual(r, {GIVE_UP: "FAIL"}, p.stdout[-3000:])
        self.assertIn("going on", p.stdout, p.stdout[-3000:])

    def test_a_wait_with_the_setting_off_fails(self):
        p, r, _ = self.run_file(env={"BT_FAKE_PARTY_WAIT_BUG": "always"}, only=OFF)
        self.assertEqual(r, {OFF: "FAIL"}, p.stdout[-3000:])
        self.assertIn("with the setting off", p.stdout, p.stdout[-3000:])

    def test_a_wait_inside_the_room_fails(self):
        p, r, _ = self.run_file(env={"BT_FAKE_PARTY_WAIT_BUG": "room"}, only=DOOR)
        self.assertEqual(r, {DOOR: "FAIL"}, p.stdout[-3000:])
        self.assertIn("not a moment", p.stdout, p.stdout[-3000:])

    def test_numbers_are_the_modules(self):
        """The room and ring numbers are coop_boss.cpp's and coop_boss_scenes.h's (one source: the module)."""
        sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ingame"))
        import test_party_wait as PW   # noqa: E402
        root = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
        with open(os.path.join(root, "lichii", "hook", "coop_boss.cpp"), encoding="utf-8") as f:
            boss = f.read()
        with open(os.path.join(root, "lichii", "hook", "coop_boss_scenes.h"), encoding="utf-8") as f:
            scenes = f.read()
        with open(os.path.join(root, "lichii", "hook", "coop_party_wait.h"), encoding="utf-8") as f:
            wait = f.read()
        self.assertIn('{0, "InsideTheDekuTree", "Enemy_DungeonBoss_Gohma", Era::child, 10, %.1ff, %.1ff, %.1ff, %.1ff, '
                      '%.1ff,' % (PW.CX, PW.CZ, PW.RADIUS, PW.Y_LOW, PW.Y_HIGH), boss)
        self.assertIn("kAnteGrow = %.1ff;" % PW.ANTE_GROW, scenes)
        self.assertIn("kAnteHigh = %.1ff;" % PW.ANTE_HIGH, scenes)
        self.assertIn("kGiveUpUs = %d'000'000;" % PW.GIVE_UP_S, wait)
        self.assertFalse(PW.ring(PW.CORRIDOR))           # the corridor is inside the room: not a moment
        self.assertTrue(PW.ring((PW.RING_X, PW.CORRIDOR[1], PW.CORRIDOR[2])))


if __name__ == "__main__":
    unittest.main()
