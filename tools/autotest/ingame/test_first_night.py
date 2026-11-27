"""C6-2 of rc-01113 (Longshot 3.2's first adult night fix in co-op) as a bridge test: the port of
offline/first-night-c62-run.ps1 and tools/autotest/scenarios/first-night-c62.steps.txt (+ .A.txt / .B.txt),
reports/port-sweep-c131.md G7. Owner: port-g7.

Longshot 3.2 clears the saved Bool HrlField_HasEnteredFirstTime_Adult (0x8592aa82, docs/time.md "Clock gate") in a
game that is outside Hyrule Field (ls32-src/host/hos_main.c oot_first_night_fix: not a HyruleField* map, no event,
flag 1 -> 0, hos_log "oot: cleared the first adult night's time lock ..."). The row: does a guest's clear reach a host
that is in Hyrule Field? It must not: the flag is on no shared list (tests/lichii/story_test.cpp "the clock gate's two
flags are on no shared list"), and A's clock is A's own.

The same checks as first-night-c62.steps.txt, each with its old name:
- set-up (old setup-a/b-setflag, setup-a/b-watch): both adult (Sequence_IsChildhood 0), both in HyruleField_Adult, the
  flag set to 1 on each (coop_testinput.cpp SetFlag "coop: test setflag NAME (bool) 1: set"), each game's watch
  (LICHII_COOP_DEBUG_BOOL, lichii/hook/coop_flags.cpp FlagDebug "coop: flags debug: get_bool(0x8592aa82) = 1").
  Both lane games must be a Longshot with the fix (its log text in longshot.exe, as the old run checked).
- then B goes to KakarikoVillage_Adult (first-night-c62.B.txt): b-cleared (Longshot's line in B), b-watch-cleared
  (B's watch "= 0 (was 1)"); then 60 s of game (the old 'sleep 60') with: a-never-cleared, a-not-cleared-by-longshot,
  a-no-sync-line (no line in A names 0x8592aa82 other than the watch), a-time-sent (A's clock is sent:
  LICHII_COOP_DEBUG_TIME, lichii/hook/coop_time.cpp "coop: time: sent"); and A's flag read back 1 (flag_read, new: the
  state's own value next to the log's).
- shots (old a-field-flag1, b-kakariko-cleared, a-field-60s-after): the sky of A before and 60 s after B's clear (A's
  clock unchanged by B), B in Kakariko. The old run judged them by eye; here each moment is a bridge shot of BOTH
  games against a reference (first-night-field-flag1, -b-cleared, -60s-after). One shot per game per moment.

Differences: every warp takes an entrance (the map change event, not Longshot's teleport, which allows one per 8 s
per game: see FIELD_START). The warps' 'wait 900' (the load) is the bridge warp's own wait for the arrival; B's
Kakariko 'wait 900' becomes the frames until B's clear line (max 900). The old run warped A, then B; here both are put
in the Field in the set-up, then only B moves (the same order of what is tested). The way back puts both flags back
and both games child in the boot's Kakariko (the boot is shared).
"""
import os

import pytest

FLAG = "HrlField_HasEnteredFirstTime_Adult"
HASH = "8592aa82"
AGE_FLAG = "Sequence_IsChildhood"                   # first-night-c62.A/B.txt 'setflag Sequence_IsChildhood 0'
FIELD, KAKARIKO = "HyruleField_Adult", "KakarikoVillage_Adult"    # first-night-c62.A/B.txt warps
# With an entrance each warp is the game's map change event, never Longshot's teleport (coop_playtest_core.cpp
# PlanWarp: "An entrance names a start point, which Longshot's teleport cannot take"), so Longshot's one teleport per
# 8 s per game (docs/playtest.md `warp`) cannot fail B's second warp a few seconds after its first (review-rows-c136 1).
# The entrances: scenarios/color-field.A.txt:15 'warp HyruleField_Adult HyruleField_FrontOfKakarikoVillage',
# live-kakariko-child.A.txt / test_gear.py 'Kakariko_WellSide' (KakarikoVillage_Adult in 23 scenario lines).
FIELD_START, KAKARIKO_START = "HyruleField_FrontOfKakarikoVillage", "Kakariko_WellSide"
BOOT_SCENE, BOOT_START = "KakarikoVillage_Child", "Kakariko_WellSide"   # scenarios/live-kakariko-child.A.txt
FIX_TEXT = b"cleared the first adult night's time lock"           # hos_main.c oot_first_night_fix (in longshot.exe)

SETFLAG_1 = r"coop: test setflag %s \(bool\) 1: set" % FLAG       # the old setup-*-setflag
WATCH_1 = r"get_bool\(0x%s\) = 1" % HASH                          # the old setup-*-watch
CLEARED = r"cleared the first adult night's time lock"            # the old b-cleared / a-not-cleared-by-longshot
# (Longshot's own hos_log line, in pair.log: the game's stdout+stderr IS game-out-<x>.txt, tools/autotest/hookshot.py:1670
# Popen(stdout=go, stderr=STDOUT) and :1790 "stderr is game-out-<x>.txt, so that is the log"; seen there as
# "[hos host] boot: 128530 MB of memory, 18432 MB of swap", hos_main.c:2109's hos_log, shots-lane1/hs-party-hud-probe/
# 20261125-233954/game-out-A.txt:11; review-rows-c136 3)
WATCH_CLEARED = r"get_bool\(0x%s\) = 0 \(was 1\)" % HASH           # the old b-watch-cleared / a-never-cleared
OTHER_LINE = r"^(?!.*(get_bool|watching bool)).*%s" % HASH        # the old a-no-sync-line
TIME_SENT = r"time: sent"                                         # the old a-time-sent
AFTER_FRAMES = 60 * 30                                            # the old 'sleep 60', in frames (FPS 30)
TOL = 0.80                                                        # outdoor scenes with a moving sky and grass


def has_fix(folder):
    try:
        with open(os.path.join(folder, "longshot.exe"), "rb") as f:
            return FIX_TEXT in f.read()
    except OSError:
        return None


def shots(pair, name):
    """One bridge shot on each game (the screenshot rule): A's sky in Hyrule Field and B where B is."""
    for x in ("A", "B"):
        pair.check_shot(x, name, tolerance=TOL)


def way_back(pair, saved, done):
    """Both flags as they were, then both games child in the boot's Kakariko (test_gear.py's way_back: the age flag,
    60 frames, the warp with the entrance). Raised only when the test body passed (`done`), else noted."""
    try:
        for x in ("A", "B"):
            if saved.get(x) is not None:
                pair.flag(x, FLAG, int(saved[x]))
            pair.flag(x, AGE_FLAG, 1)
        pair.step_both(60)
        pair.warp_both(BOOT_SCENE, entrance=BOOT_START)
    except Exception as e:
        if done:
            raise
        pair.notes.append("way back failed after the test's FAIL: %s: %s" % (type(e).__name__, e))


@pytest.mark.look
@pytest.mark.budget(360)
@pytest.mark.needs_env("LICHII_COOP_DEBUG_TIME=1", "LICHII_COOP_DEBUG_BOOL=0x8592aa82")
@pytest.mark.row("C6-2 rc-01113 Longshot 3.2's first adult night clear in a guest never reaches a host in "
                 "Hyrule Field (first-night-c62)")
def test_first_night_clear_stays_in_its_game(pair):
    saved = {}
    with pair.setup():
        for x in ("A", "B"):
            fix = has_fix(pair.games[x]["folder"])
            assert fix is not None, "%s: no longshot.exe in %s" % (x, pair.games[x]["folder"])
            assert fix, "%s: longshot.exe has no first-night fix (no %r): not Longshot 3.2" % (x, FIX_TEXT.decode())
            assert pair.log[x].find(r"coop: flags debug .*watching bool 0x%s" % HASH, pos=0), (
                "%s: the bool watch is not on (no 'watching bool 0x%s' line: LICHII_COOP_DEBUG_BOOL)" % (x, HASH))
            saved[x] = pair.flag_read(x, FLAG).get("value")
        for x in ("A", "B"):
            pair.flag(x, AGE_FLAG, 0)
        pair.step_both(60)                        # test_gear.py: the age flag takes 30+ frames before the warp
        for x in ("A", "B"):
            st = pair.warp_one(x, FIELD, FIELD_START)
            assert pair.in_play(st) and st.get("age") == "adult", "%s not an adult in %s (%s, %s)" % (
                x, FIELD, st.get("scene"), st.get("age"))
        for x in ("A", "B"):
            pair.flag(x, FLAG, 1)
            pair.expect_log(x, SETFLAG_1, max_frames=30)
        for x in ("A", "B"):                      # first-night-c62.A/B.txt 'wait 300' after the setflag: at most
            pair.expect_log(x, WATCH_1, max_frames=300)   # that, until the watch read it (setup-a/b-watch)
            pair.never_log(x, CLEARED)            # (in Hyrule Field Longshot leaves it)
    done = False
    try:
        shots(pair, "first-night-field-flag1")
        st = pair.warp_one("B", KAKARIKO, KAKARIKO_START)
        assert st.get("scene") == KAKARIKO, "B not in %s: %s" % (KAKARIKO, st.get("scene"))
        pair.expect_log("B", CLEARED, max_frames=900)            # b-cleared
        pair.expect_log("B", WATCH_CLEARED, max_frames=300)      # b-watch-cleared
        shots(pair, "first-night-b-cleared")
        pair.expect_log("A", TIME_SENT, max_frames=AFTER_FRAMES)  # a-time-sent
        pair.step_both(AFTER_FRAMES)                             # the old 'sleep 60'
        pair.never_log("A", WATCH_CLEARED)                       # a-never-cleared
        pair.never_log("A", CLEARED)                             # a-not-cleared-by-longshot
        pair.never_log("A", OTHER_LINE)                          # a-no-sync-line
        got = pair.flag_read("A", FLAG).get("value")
        assert got in (1, True), "A's %s is %r after B's clear, not 1: the clear reached the host" % (FLAG, got)
        shots(pair, "first-night-60s-after")
        done = True
    finally:
        way_back(pair, saved, done)
