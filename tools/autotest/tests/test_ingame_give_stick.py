"""Selftests of the give / flag read-back (give-life-fix, 2026-11-26; volvagia-rocks-234-2 17:22 VOID): the game keeps
PlayerStatus.Life and MaxLife in our Link's Life component, the flags being only its mirror, so a flag-only write is
put back by the next frame. The fixed module writes the component first and the bridge reads every give and number
flag back (did_not_stick); Pair.give / Pair.flag make that a SETUP-FAIL at once, naming the flag. No game: the fake
lane, whose BT_FAKE_LIFE_FLAG_ONLY=1 plays the module before the fix.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")

TESTS = '''
def test_hearts_given_and_filled(pair):
    with pair.setup():
        for x in pair.c:
            for _ in range(10):
                pair.give(x, "PlayerStatus.MaxLife")
            r = pair.flag(x, "PlayerStatus.Life", 56)
            assert r["value"] == 56, r


def test_twenty_gives_stop_at_the_most_hearts(pair):
    # (volvagia-rocks-309d7180: MaxLife shared by the max took B past 20 hearts; the give stops there, no change)
    import fakegame
    with pair.setup():
        for _ in range(20):
            r = pair.give("B", "PlayerStatus.MaxLife")
        assert r["result"] == "no change: PlayerStatus.MaxLife already %d" % fakegame.MOST_MAX_LIFE, r


def test_a_flag_past_the_most_hearts_is_setup_fail(pair):
    with pair.setup():
        pair.flag("A", "PlayerStatus.MaxLife", 84)
'''


class GiveStick(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        d = os.path.join(self.tmp, "t")
        os.makedirs(d)
        with open(os.path.join(d, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(d, "test_hearts.py"), "w", encoding="utf-8") as f:
            f.write(TESTS)
        self.dir, self.out = d, os.path.join(self.tmp, "o")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, flag_only):
        env = dict(os.environ)
        env.pop("BT_FAKE_LIFE_FLAG_ONLY", None)
        if flag_only:
            env["BT_FAKE_LIFE_FLAG_ONLY"] = "1"
        p = subprocess.run([sys.executable, "-m", "pytest", ".", "--fake", "--out", self.out, "-q", "-p",
                            "no:cacheprovider", "-p", "no:randomly"], cwd=self.dir, capture_output=True, text=True,
                           timeout=600, env=env)
        with open(os.path.join(self.out, "summary.json"), encoding="utf-8") as f:
            rows = {r["test"].split("::")[-1]: r for r in json.load(f)["tests"]}
        self.rows = rows
        return rows["test_hearts_given_and_filled"], p.stdout[-3000:]

    def test_the_fixed_module_keeps_the_hearts(self):
        row, out = self.run_fake(False)
        self.assertEqual(row["result"], "PASS", out)

    def test_the_game_s_most_hearts_stop_a_give_and_a_flag_past_it_is_setup_fail(self):
        self.run_fake(False)
        self.assertEqual(self.rows["test_twenty_gives_stop_at_the_most_hearts"]["result"], "PASS")
        row = self.rows["test_a_flag_past_the_most_hearts_is_setup_fail"]
        self.assertEqual(row["result"], "SETUP-FAIL")
        self.assertIn("flag PlayerStatus.MaxLife did not stick: did not stick: PlayerStatus.MaxLife written 84, the game put back", json.dumps(row))

    def test_a_flag_only_give_is_setup_fail_at_once_naming_the_flag(self):
        row, out = self.run_fake(True)
        self.assertEqual(row["result"], "SETUP-FAIL", out)
        why = json.dumps(row)
        self.assertIn("give PlayerStatus.MaxLife did not stick", why, out)
        self.assertIn("written 20, the game put back 16", why, out)


if __name__ == "__main__":
    unittest.main()
