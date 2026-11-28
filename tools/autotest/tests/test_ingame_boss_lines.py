"""ingame/boss_lines.py (the native boss table's log lines) against the module source and a real game line.

Row 303 VOID (boss-stuck3-182cd53b, 2026-11-28): test_intro_seen.SYNC and the fakes boss_stuck.py / intro_seen.py said
'coop: boss: <boss>: native boss sync on ...', the module logs 'coop: bosses: ...'; the fakes were written from the
same wrong text, so every selftest passed and every live run missed the line. Here the table is read against the
writers themselves, and every test / fake that names these notes must take them from the table.
"""
import glob
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
HOOK = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), "lichii", "hook")
sys.path.insert(0, INGAME)
import boss_lines as BL   # noqa: E402

GOHMA = "Enemy_DungeonBoss_Gohma"
# The live line the VOID run had (offline/boss-stuck-303/run3-182cd53b lane1/session/game-out-A.txt :7296).
REAL = "[hos Presentation Thread] coop: bosses: Enemy_DungeonBoss_Gohma: native boss sync on (BSA 0x10523de398, ours)"


def source(name):
    with open(os.path.join(HOOK, name), encoding="utf-8") as f:
        return f.read()


class BossLinesTest(unittest.TestCase):
    def test_the_prefix_is_the_one_the_table_notes_are_logged_with(self):
        objects = source("coop_objects.cpp")
        # the loop that logs the native boss table's notes (CoopBossNative::Table::TakeNotes)
        loop = re.search(r"for \(const std::string& note : bosses\.TakeNotes\(\)\) \{\s*LOG_INFO\(Core, \"([^\"]*)\{\}\"",
                         objects)
        self.assertIsNotNone(loop, "coop_objects.cpp: the bosses.TakeNotes() log loop is gone")
        self.assertEqual(loop.group(1), BL.PREFIX)
        # 'coop: boss: ' is coop_boss_game.cpp's (the rooms' Sync lines), not this table's
        self.assertNotEqual(BL.PREFIX, "coop: boss: ")

    def test_each_note_text_is_the_modules(self):
        native = source("coop_boss_native.cpp")
        self.assertIn('Note(b.name + ": native boss sync on (BSA " + Hex(b.bsa) + ", " +', native)
        self.assertIn('(b.role == Role::owner ? "ours" : "a copy") + ")");', native)
        self.assertIn('Note(e->name + ": the owner\'s verbs came (brain " + std::to_string(state.brain_verb) +', native)
        self.assertIn('"): the copy\'s own brain is skipped from now");', native)
        self.assertIn('Note(b.name + ": now " + (b.role == Role::owner ? "ours: our game runs it (sends its verbs)"',
                      native)

    def test_the_real_line_is_found_and_the_fake_lines_are_the_real_form(self):
        self.assertTrue(re.search(BL.sync_re(GOHMA, "ours"), REAL))
        self.assertFalse(re.search(BL.sync_re(GOHMA, "a copy"), REAL))
        self.assertTrue(re.search(BL.sync_re(GOHMA, "ours"), BL.sync_line(GOHMA, "ours", 0x10523de398)))
        self.assertEqual(BL.sync_line(GOHMA, "ours", 0x10523de398), REAL.split("] ", 1)[1])
        self.assertTrue(re.search(BL.verbs_re(GOHMA), BL.verbs_line(GOHMA)))
        self.assertTrue(re.search(BL.now_ours_re(GOHMA), BL.now_ours_line(GOHMA)))
        import test_intro_seen as IS   # (the pattern test_boss_stuck.py imports)
        self.assertTrue(re.search(IS.SYNC % "ours", REAL))
        self.assertTrue(re.search(IS.VERBS, BL.verbs_line(GOHMA)))
        self.assertTrue(re.search(IS.NOW_OURS, BL.now_ours_line(GOHMA)))

    def test_her_stuck_and_owner_lines_reach_test_boss_stuck(self):
        """test_boss_stuck kept only lines with 'coop: boss: <boss>', so its stuck lines could never be seen."""
        native = source("coop_boss_native.cpp")
        for text in ('": ours held \'" + b.animation + "\' (brain "', '" s with no hit: stuck, its AI chooses again ("',
                     'Note(b.name + ": fell " + std::to_string(static_cast<int>(below)) +',
                     '" m below its fight start (row 178): put back at its fight start, its AI chooses again");',
                     'Note(b.name + ": its AI did not run its body as a copy: it chooses again");',
                     '"a copy: it takes the owner\'s verbs"'):
            self.assertIn(text, native)
        import test_boss_stuck as BS
        lines = [BL.line(GOHMA, "ours held 'Wait' (brain 3, action 1, life 21) 25 s with no hit: stuck, its AI chooses "
                                "again (1 of 3)"),
                 BL.line(GOHMA, "fell 12 m below its fight start (row 178): put back at its fight start, its AI "
                                "chooses again"),
                 BL.line(GOHMA, "its AI did not run its body as a copy: it chooses again")]
        for ln in lines:
            self.assertTrue(any(k in ln for k in BS.BOSS_LINES), ln)
            self.assertTrue(any(re.search(p, ln) for p in BS.STUCK_LINES), ln)
        for role in ("ours: our game runs it (sends its verbs)", "a copy: it takes the owner's verbs"):
            ln = BL.line(GOHMA, "now " + role)
            self.assertTrue(any(k in ln for k in BS.BOSS_LINES) and re.search(BS.OWNER_LINE, ln), ln)
        probe = BL.PREFIX + "probe 323: Gohma 0x104fd96a00 role owner owner 1 (changed)"
        self.assertTrue(any(k in probe for k in BS.BOSS_LINES))

    def test_no_test_or_fake_writes_these_notes_with_the_rooms_prefix(self):
        # 'coop: boss: <Actor>: native boss sync on' / ': the owner's verbs came' / ': now ours' anywhere in ingame/
        bad = re.compile(r"coop: boss: [A-Za-z_%{]")
        notes = ("native boss sync on", "the owner's verbs came", ": now ours")
        hits = []
        for f in glob.glob(os.path.join(INGAME, "**", "*.py"), recursive=True):
            with open(f, encoding="utf-8") as fh:
                for n, text in enumerate(fh, 1):
                    if bad.search(text) and any(k in text for k in notes):
                        hits.append("%s:%d" % (os.path.relpath(f, INGAME), n))
        self.assertEqual(hits, [])


if __name__ == "__main__":
    unittest.main()
