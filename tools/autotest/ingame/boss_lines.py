"""The native boss table's log lines, ONE table for the bridge tests and their fakes (row 303 VOID 2026-11-28).

The table's notes (lichii/hook/coop_boss_native.cpp Note) are logged by coop_objects.cpp as 'coop: bosses: {}'
(its bosses.TakeNotes() loop). 'coop: boss: {}' (coop_boss_game.cpp) is another writer: the boss rooms' Sync lines,
never these. test_intro_seen.py and the fakes boss_stuck.py / intro_seen.py had 'coop: boss:' here, so the live
runs never found the line (boss-stuck3-182cd53b NO-REF 'A does not own Gohma after its walk-in' although
game-out-A.txt :7296 has 'coop: bosses: Enemy_DungeonBoss_Gohma: native boss sync on (BSA 0x10523de398, ours)')
while the fakes, written from the same wrong text, passed. tests/test_ingame_boss_lines.py checks this table
against the module source.
"""
import re

PREFIX = "coop: bosses: "          # coop_objects.cpp: LOG_INFO(Core, "coop: bosses: {}", note)

# Each note's text after '<boss>: ' (coop_boss_native.cpp): {bsa} 0x.. hex, {role} 'ours' | 'a copy'.
SYNC_ON = "native boss sync on (BSA {bsa}, {role})"             # Note(b.name + ": native boss sync on (BSA " ...
VERBS_CAME = "the owner's verbs came (brain {brain}): the copy's own brain is skipped from now"
NOW_OURS = "now ours: our game runs it (sends its verbs)"       # Note(b.name + ": now " + ... "ours: ...")


def line(boss, text):
    """A note's whole line as the module logs it."""
    return PREFIX + boss + ": " + text


def sync_line(boss, role, bsa=0x7f3a20):
    return line(boss, SYNC_ON.format(bsa="%#x" % bsa, role=role))


def verbs_line(boss, brain=3):
    return line(boss, VERBS_CAME.format(brain=brain))


def now_ours_line(boss):
    return line(boss, NOW_OURS)


def sync_re(boss, role):
    """'native boss sync on' of `boss` with `role` ('ours' / 'a copy'), any BSA."""
    return re.escape(PREFIX + boss + ": native boss sync on (BSA ") + r"0x[0-9a-f]+" + re.escape(", %s)" % role)


def verbs_re(boss):
    return re.escape(PREFIX + boss + ": the owner's verbs came")


def now_ours_re(boss):
    return re.escape(PREFIX + boss + ": now ours")
