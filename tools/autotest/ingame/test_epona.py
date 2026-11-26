"""Row 150 (another player's Epona seen) as bridge tests: ports of hs-epona-sync and hs-epona-seen
(coord offline/epona-sync-run.ps1 and epona-seen-run.ps1; scenarios/hs-epona-sync.*.txt, hs-epona-seen.*.txt).

| Test | Old row | Kind |
|---|---|---|
| test_epona_sync | hs-epona-sync (row 150 / #199, PLAYER_RIDE free) | ride log lines on both games + LOOK (shots on each game) |
| test_epona_seen | hs-epona-seen (row 150: B calls her with Epona's Song) | song keys + ride log lines + LOOK |

Both run on the default kit boot (child Kakariko); each makes both games adult in Hyrule Field the rows' way (age flag
after the load, then the warp: hs-epona-sync.A/B.txt) and leaves both child again in Kakariko for the lane's later tests.

What a won Epona is: the quest enum `Get_Epona` (0x92431c55, docs/flags.md "Shared by its end only") at `Complete`
(its hash 0x4c0a63f4, the game's own line 'quest Get_Epona ... -> 0x4c0a63f4': offline/epona-seen-run.ps1 header).
The ride module asks the game's own getter once a second and logs 'coop: ride: Epona is ours (Get_Epona Complete)'
(lichii/hook/coop_ride.cpp:468-474). Co-op shares Get_Epona only at its end, so once B is Complete, A is too.
A field Epona stands at -100.47 150.60 264.55 only while Get_Epona is not Complete (hs-epona.B.txt header:
HyruleField_Adult_Static), so test_epona_sync needs a boot where nobody has won her yet and runs first (file order);
test_epona_seen does not need her there (the song brings her).

hs-epona-seen used the owner's beaten-Ganon save as slot_00. Here the save is not needed: B gets the song by `give`
(IsGet.Item_Melody_EponaSong, docs/playtest.md `give`) and the won quest by `flag` (the enum value's name), the way
hs-epona-sync.B.txt's own `setenum 0x92431c55 Complete` does. Whether the GAME's song call answers a won state made
this way is not proven by any run yet: if the song gives no free Epona, the test ends NO-REF with that reason (the old
row's VOID 'b-free 0'), never PASS and never FAIL.

Every log pattern is the old rows' own pass line (their headers), each cited at its constant. Positions are the rows'
(hs-epona.B.txt / hs-epona-sync.*.txt), and B's mount spots are taken from Epona's own place in B's world list.
"""
import math
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import ocarina_notes as ON     # noqa: E402  (the ONE key -> note table, read from the game)

from test_gear import AGE_FLAG, GATE, SCENE_CHILD, shots   # noqa: E402  (the same way out and back, same shot rule)
from test_rows import need_child_in_play                   # noqa: E402

FIELD = "HyruleField_Adult"                       # hs-epona-sync.A/B.txt 'warp HyruleField_Adult ...'
EPONA = "Animal_Epona_Adult"                      # the horse actor's name (every ride line below)
EPONA_FIELD = (-100.47, 150.60, 264.55)           # hs-epona.B.txt header: the field Epona of a save that has not won her
BESIDE = 1.5                                      # hs-epona.B.txt 'place -99.0 151.0 264.5 # beside Epona (1.5 m east)'
WATCH = 10.0                                      # hs-epona-sync.A.txt 'place -100.5 151.0 254.5 # 10 m south'
GET_EPONA = "Get_Epona"                           # 0x92431c55 (docs/flags.md; hs-epona-sync.B.txt setenum)
COMPLETE, COMPLETE_HASH = "Complete", "0x4c0a63f4"   # epona-seen-run.ps1 header (the game's own quest line)
SONG_ITEM = "Item_Melody_EponaSong"              # IsGet.Item_Melody_EponaSong (epona-seen-run.ps1 header, docs/items.md)
# the rows' '#! switches' (hs-epona-sync.A/B.txt, hs-epona-seen.A/B.txt); LICHII_COOP_CHEATS=1 is the default boot's
# own (scenarios/live-kakariko-child.A.txt), as test_gear.test_adult_gear_shown says.
ROW_ENV = ("LICHII_COOP_PUPPET=1", "LICHII_COOP_DEBUG_ANIM=1", "LICHII_COOP_OBJECTS=1")

# pass lines: hs-epona-sync.B.txt / hs-epona-seen.B.txt headers (written by lichii/hook/coop_ride.cpp:323-330, 472-494)
B_RIDES = r"coop: ride: we ride Animal_Epona_Adult"                  # b-rides
B_OWNS = r"coop: ride: Epona is ours \(Get_Epona Complete\)"         # b-owns
B_FREE = r"coop: ride: our Epona is out with nobody on her"          # b-free
B_GONE = r"coop: ride: our Epona is gone"                            # hs-epona-sync.B.txt:15-16 (INCONCLUSIVE case)
B_AGAIN = r"coop: ride: our Epona has us on her again"               # b-on-again
A_HERE = r"coop: ride: P\d+'s Animal_Epona_Adult is here, nobody on her"   # hs-epona-seen.A.txt 'await' line
A_RIDES = r"coop: ride: P\d+ rides Animal_Epona_Adult"               # a-rides
A_WAITS = r"coop: ride: P\d+'s Animal_Epona_Adult waits, nobody on her \(the same copy\)"   # a-waits
A_SAME = r"coop: ride: P\d+ rides Animal_Epona_Adult \(horse copy CoopRide_Epona, the same copy\)"   # a-same-copy
A_GONE = r"coop: ride: P\d+(?:'s horse gone| off the horse) \(copy deleted\)"   # coop_ride.cpp:334-336
A_COPY = r"coop: spawn: CoopRide_Epona: create copy"                 # a-one-copy (coop_spawn.cpp:2031 Say)
NOTE_SENT = r"coop: ocarina note \d+ \(Ocarina_%s\) sent"             # test_rows.test_ocarina_notes (coop_ocarina_sound.cpp)
OCARINA_OUT = r"ocarina probe: our Link plays the ocarina"           # test_rows.test_ocarina_notes


def stick(pair, x, sx, sy, frames):
    """A scenario's 'stick X Y TICKS' (x right, y up/away from the camera) as the bridge's [angle, strength]
    (playtest_client.Client.input: degrees clockwise from up)."""
    pair.press(x, [], stick=(math.degrees(math.atan2(sx, sy)) % 360.0, min(1.0, math.hypot(sx, sy))), frames=frames)


def horse_pos(pair, x="B"):
    """Epona's place in game x's world list (the nearest Animal_Epona_Adult), or None."""
    w = pair.c[x].world().get("world") or {}
    for a in w.get("actors", []):
        if a.get("name") == EPONA and a.get("pos"):
            return a["pos"]
    return None


def face_rider(pair):
    """A faces B, camera behind A (hs-epona-*.A.txt 'face' + 'press ZL'), so A's shot holds B and the horse."""
    b = pair.state("B")["pos"]
    pair.act("A", "face %.2f %.2f" % (b[0], b[2]))
    pair.press("A", ["ZL"], frames=8)


def watch_from_south(pair, at):
    """A WATCH m south of `at` (hs-epona-sync.A.txt 'place -100.5 151.0 254.5'), facing B."""
    pair.act("A", "place %.2f %.2f %.2f" % (at[0], at[1], at[2] - WATCH))
    pair.step_both(10)
    face_rider(pair)


def mount(pair, line):
    """B gets on: placed BESIDE m from Epona's own place (east, north, west, south: the old rows' rings round her),
    facing her, A pressed ('press A 12' + 'wait 150' each round, hs-epona-seen.B.txt). True once `line` is in B's log."""
    start = pair.log["B"].mark()
    for dx, dz in ((1, 0), (0, 1), (-1, 0), (0, -1)):
        h = horse_pos(pair) or EPONA_FIELD
        pair.act("B", "place %.2f %.2f %.2f" % (h[0] + BESIDE * dx, h[1], h[2] + BESIDE * dz))
        pair.act("B", "face %.2f %.2f" % (h[0], h[2]))
        pair.step_both(10)
        pair.press("B", ["A"], frames=12)
        pair.step_both(150)
        if pair.log["B"].find(line, pos=start):
            return True
    return False


def to_field(pair):
    """Both games adult in Hyrule Field: the age flag after the load, 'wait 60', then the warp (hs-epona-sync.A/B.txt)."""
    for x in ("A", "B"):
        pair.flag(x, AGE_FLAG, 0)
    pair.step_both(60)
    pair.warp_both(FIELD)
    for x in ("A", "B"):
        s = pair.state(x)
        assert pair.in_play(s) and s.get("age") == "adult", "%s not an adult in play in %s (%s, %s)" % (
            x, FIELD, s.get("scene"), s.get("age"))


def quest(pair, x):
    return pair.flag_read(x, GET_EPONA).get("value")


B_ON = r"coop: ride: (?:we ride Animal_Epona_Adult|our Epona has us on her again)"   # coop_ride.cpp:493, 500-503
B_OFF = r"coop: ride: (?:we got off|our Epona is out with nobody|our Epona is gone)"


def riding(pair, start):
    """Whether B's last ride line since `start` (a B log position) says it is on her."""
    on = False
    for ln in pair.log["B"].since(start):
        if re.search(B_ON, ln):
            on = True
        elif re.search(B_OFF, ln):
            on = False
    return on


def way_back(pair, was, done, b_start):
    """The boot as it was for the lane's later tests (as test_gear.way_back; also after a SETUP-FAIL): B off the horse,
    Get_Epona back to what each game had (a refused write is noted: co-op never lowers it, docs/flags.md), both
    games child in Kakariko.
    A failure here is raised only when the test body passed (`done`); else noted, so the test's own message stays."""
    try:
        if riding(pair, b_start):
            pair.press("B", ["A"], frames=12)     # get off (else the warp keeps a rider)
            pair.step_both(120)
        for x in ("A", "B"):
            if was.get(x) is not None and was[x] != COMPLETE_HASH:
                try:
                    pair.flag(x, GET_EPONA, was[x])
                except Exception as e:            # (a refusal is not the row: noted)
                    pair.notes.append("%s: Get_Epona not put back to %s: %s: %s" % (x, was[x], type(e).__name__, e))
            pair.flag(x, AGE_FLAG, 1)
        pair.step_both(60)
        pair.warp_both(SCENE_CHILD, entrance=GATE)
    except Exception as e:
        if done:
            raise
        pair.notes.append("way back after the failure: %s: %s" % (type(e).__name__, e))
        return
    if done:
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s) and s.get("age") == "child", (
                "%s stays %s after the test: the lane's later tests need child Link" % (x, s.get("age")))


def off_walk_on(pair, prefix):
    """B gets off, walks away and back, gets on again; A keeps the same horse copy (both rows' end)."""
    pair.log["B"].mark()
    pair.log["A"].mark()
    pair.press("B", ["A"], frames=12)             # get off: she stays, sent free
    pair.step_both(300)
    if pair.log["B"].find(B_GONE) and not pair.log["B"].find(B_FREE):
        pair.unproven("B's Epona went away when B got off (hs-epona-sync.B.txt:15-16 INCONCLUSIVE)")
    pair.expect_log("B", B_FREE, max_frames=120)
    pair.expect_log("A", A_WAITS, max_frames=300)
    face_rider(pair)
    shots(pair, prefix + "-dismounted")           # the horse alone in A's window, B's puppet beside her
    stick(pair, "B", 1.0, 0, 90)                  # 'stick 1.0 0 90': walk away from her on foot
    pair.step_both(120)
    face_rider(pair)
    shots(pair, prefix + "-walked-away")          # B off, the horse still there
    pair.never_log("A", A_GONE)                   # FAIL: A dropped B's waiting horse
    assert mount(pair, B_AGAIN), "B could not get on its own waiting Epona again (no /%s/)" % B_AGAIN
    pair.expect_log("A", A_SAME, max_frames=300)  # the others keep the same copy
    stick(pair, "B", 0, 0.8, 120)                 # 'stick 0 0.8 120'
    face_rider(pair)
    shots(pair, prefix + "-mounted-again")


def one_copy(pair, start):
    n = sum(1 for ln in pair.log["A"].since(start) if A_COPY in ln)
    assert n == 1, "A made the horse copy %d times (a-one-copy: exactly once; a second is a blink)" % n


@pytest.mark.budget(480)
@pytest.mark.look
@pytest.mark.needs_env(*ROW_ENV)
@pytest.mark.row("#150/#199 another player's own Epona seen while ridden and while she waits (hs-epona-sync)")
def test_epona_sync(pair):
    b_start = pair.log["B"].mark()
    with pair.setup():
        need_child_in_play(pair)
        was = {x: quest(pair, x) for x in ("A", "B")}
    if was["B"] == COMPLETE_HASH:
        # review-rows-c130: co-op never lowers Get_Epona, so after test_epona_seen (an earlier round of --repeat, or an
        # earlier test on this boot) no field Epona stands: SKIP (nothing to judge here), not SETUP-FAIL
        pytest.skip("the boot's Epona is won (Get_Epona %s, an earlier test or round): the field Epona does not stand"
                    % was["B"])
    done = False
    try:
        with pair.setup():
            to_field(pair)
            h = horse_pos(pair)
            assert h and math.dist(h[::2], EPONA_FIELD[::2]) < 5, "no %s standing at the field spot in B's world list (%s)" % (
                EPONA, h)
            watch_from_south(pair, h)
        start = pair.log["A"].mark()
        if not mount(pair, B_RIDES):
            pair.unproven("B never got on the field's Epona (hs-epona-sync.B.txt:15 INCONCLUSIVE: b-rides 0)")
        pair.expect_log("A", A_RIDES, max_frames=300)
        face_rider(pair)
        shots(pair, "epona-sync-mounted")
        pair.flag("B", GET_EPONA, COMPLETE)       # 'setenum 0x92431c55 Complete': won while B sits on her
        pair.expect_log("B", B_OWNS, max_frames=180)   # asked once a second (coop_ride.cpp:468)
        stick(pair, "B", 0, 1.0, 180)             # 'stick 0 1.0 180': gallop
        face_rider(pair)
        shots(pair, "epona-sync-gallop-1")        # two shots a few frames apart: her legs move on each game
        pair.step_both(6)
        shots(pair, "epona-sync-gallop-2")
        pair.step_both(240)                       # 'wait 240': let her stop
        off_walk_on(pair, "epona-sync")
        one_copy(pair, start)
        done = True
    finally:
        way_back(pair, was, done, b_start)


@pytest.mark.budget(600)
@pytest.mark.look
@pytest.mark.needs_env(*ROW_ENV)
@pytest.mark.row("#150 B calls its won Epona with Epona's Song, A sees her come, ridden and waiting (hs-epona-seen)")
def test_epona_seen(pair):
    was, b_start = {}, pair.log["B"].mark()
    done = False
    try:
        with pair.setup():
            need_child_in_play(pair)
            was = {x: quest(pair, x) for x in ("A", "B")}
            pair.give("B", SONG_ITEM)
            pair.flag("B", GET_EPONA, COMPLETE)       # the won save's state (epona-seen-run.ps1: Get_Epona Complete)
            to_field(pair)
            if was["B"] != COMPLETE_HASH:
                pair.expect_log("B", B_OWNS, max_frames=180)
            else:                                 # won before this test: the module said it once, at the change
                assert pair.log["B"].find(B_OWNS, pos=0), "B: Get_Epona Complete but never /%s/ this boot" % B_OWNS
            assert quest(pair, "B") == COMPLETE_HASH, "B's Get_Epona is %s, not Complete" % quest(pair, "B")
            pair.act("B", "place %.2f %.2f %.2f" % EPONA_FIELD)   # hs-epona-seen.B.txt 'place -99.0 151.0 264.5'
            pair.act("B", "face %.2f %.2f" % (EPONA_FIELD[0], EPONA_FIELD[2] + 25))   # 'face -99.0 290.0': north, away
            pair.press("B", ["ZL"], frames=8)
            watch_from_south(pair, EPONA_FIELD)
        with pair.setup():                            # the ocarina is set-up too (its own 30 s)
            # the ocarina out as test_rows.test_ocarina_notes (kit save: on d-left = the bridge's LEFT), its first-use note
            # closed with A, then put away (B) and out again, so no note but the song's is played after it
            pair.press("B", ["LEFT"], frames=12)
            pair.step_both(240)
            assert pair.log["B"].find(OCARINA_OUT), "B: no '%s' 240 frames after LEFT" % OCARINA_OUT
            pair.press("B", ["A"], frames=12)
            pair.step_both(120)
            pair.press("B", ["B"], frames=8)
            pair.step_both(60)
            pair.log["B"].mark()
            pair.press("B", ["LEFT"], frames=12)
            pair.step_both(240)
            assert pair.log["B"].find(OCARINA_OUT), "B: the ocarina did not come out again"
        start = pair.log["A"].mark()
        for key in ON.keys("EponaSong"):          # X Y A X Y A, ON.SONG_HOLD presses ON.SONG_GAP apart (run fec750ad)
            pair.log["B"].mark()
            pair.press("B", [key], frames=ON.SONG_HOLD)
            pair.expect_log("B", NOTE_SENT % ON.KEY_NOTE[key], max_frames=ON.SONG_GAP)
            pair.step_both(ON.SONG_GAP)
        pair.log["B"].mark()
        pair.step_both(300)                       # 'wait 300'
        try:
            pair.expect_log("B", B_FREE, max_frames=900)   # 'await 900 ... out with nobody on her'
        except AssertionError:
            pair.unproven("the song brought no free Epona (hs-epona-seen.B.txt VOID 'b-free 0'): the game's call from "
                          "a flag-made won state is not proven by any run yet")
        pair.press("B", ["B"], frames=12)         # the ocarina away
        pair.step_both(480)                       # 'wait 480': she runs up and stops near B
        pair.expect_log("A", A_HERE, max_frames=600)
        face_rider(pair)
        shots(pair, "epona-seen-horse-here")
        if not mount(pair, B_RIDES):
            pair.unproven("no mount round got on (hs-epona-seen.B.txt VOID 'b-rides 0')")
        pair.expect_log("A", A_RIDES, max_frames=300)
        face_rider(pair)
        shots(pair, "epona-seen-mounted")
        for gait, sx, sy in (("walk", 0.15, 0.25), ("trot", 0.35, 0.5), ("gallop", 0.5, 0.85)):   # hs-epona-seen.B.txt
            stick(pair, "B", sx, sy, 300)
            face_rider(pair)
            shots(pair, "epona-seen-%s-1" % gait)
            stick(pair, "B", sx, sy, 6)
            shots(pair, "epona-seen-%s-2" % gait)
        pair.step_both(360)                       # 'wait 360': let go, she stops
        off_walk_on(pair, "epona-seen")
        one_copy(pair, start)
        done = True
    finally:
        way_back(pair, was, done, b_start)
