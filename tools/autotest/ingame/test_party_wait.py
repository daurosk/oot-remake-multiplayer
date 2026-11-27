"""Row 298 (wait for the party, a host option, off by default) as bridge tests: the in-game checks of docs/party-wait.md
"Not proven" (1), (3) and (4). The module code is on this branch: lichii/hook/coop_party_wait.{h,cpp} (the rules) and
coop_party_wait_game.cpp (the frame: the boss door moment, who is here).

| Test | docs/party-wait.md | Kind |
|---|---|---|
| test_party_wait_boss_door | Not proven (1) | A before Gohma's door, B elsewhere: A's wait line (1/2 here) + its chat line; B beside A: 'Everyone is here.' (shots) |
| test_party_wait_gives_up | Not proven (3) | nobody comes: 60 s later 'Not everyone is here: going on.' (shot); A walks meanwhile (state, log) |
| test_party_wait_off | Not proven (4) | the setting off (the default): no party wait line at all (log) |

Solo and co-op idle (Not proven (4), second half) are not a pair's case: the frame returns before reading anything
with no session (coop_party_wait_game.cpp Frame: 'session == nullptr || !g.on'), unit-tested in
tests/lichii/party_wait_test.cpp (off / solo / title silent).

The boss door place. The room is Gohma's circle in coop_boss.cpp kRooms[0] (centre x 7.5 z 67.2, radius 37, height
43..63); its area ('the antechamber') is that circle grown by CoopBossScenes kAnteGrow 14 and its band by kAnteHigh 6
(coop_boss_scenes.h:88-89, InArea/InRoom coop_boss_scenes.cpp:98-122). The corridor before Gohma's door,
test_boss_away.CORRIDOR (-12.0 48.5 66.5, hs-boss-away-dungeon.A.txt), is 19.5 m from the centre: INSIDE the room's
circle, so it is not a moment (the test checks that too). No place of any old row is known to stand in the ring
(37..51 m from the centre): A walks from the corridor away from the door (-x, the corridor's own axis: the old row's
door gotos run along +x, -4.0 -> 6.7) to the ring's middle, and the test reads where A got to. Not in the ring: VOID
(UNPROVEN, naming A's place: the probe result, never a PASS or a FAIL), never a guessed place.

Every log pattern is the module's own line, cited at its writer. Shots for what a player sees: the banner lines.
Afterwards the setting is put back off ('/wait off', the default every other row plays with).
"""
import math

import pytest

from test_dbno_crawl import BOOT, to_place     # noqa: E402  (the default boot's place; the age flag + warp + need_at)
from test_enemies import place, walk_to        # noqa: E402

SCENE, ENTRANCE = "InsideTheDekuTree", "Entrance"      # test_levitate / test_map_arrow: 'warp InsideTheDekuTree Entrance'
CORRIDOR = (-12.0, 48.5, 66.5)     # test_boss_away.CORRIDOR: 'place -12.0 48.5 66.5 # the corridor before Gohma's door'
ROOM_E = (-14.5, 79.6, -6.5)       # test_boss_away.ROOM_E: 'room E ... far from Gohma's area' (B, not here)
# Gohma's room, coop_boss.cpp kRooms[0]: {0, "InsideTheDekuTree", ..., 10, 7.5f, 67.2f, 37.0f, 43.0f, 63.0f, ...}
CX, CZ, RADIUS, Y_LOW, Y_HIGH = 7.5, 67.2, 37.0, 43.0, 63.0
ANTE_GROW, ANTE_HIGH = 14.0, 6.0   # coop_boss_scenes.h:88-89 kAnteGrow, kAnteHigh
RING_X = CX - RADIUS - ANTE_GROW / 2.0   # -36.5: the ring's middle on the corridor's line (z of CORRIDOR)
WALK_TOL, WALK_FRAMES = 2.0, 900   # 7 m of ring each side; 24.5 m at a walk (test_enemies.walk_to, 8 frames a turn)
BESIDE_M = 1.0                     # B put 1 m from A: one room (CoopTeleport::CheckMove; kNearMove 10 with no table)

FPS = 30                           # pair.FPS
GIVE_UP_S = 60                     # coop_party_wait.h:46 kGiveUpUs = 60'000'000 (server clock: wall time)
GIVE_UP_FRAMES = 3 * GIVE_UP_S * FPS   # the clock is the server's, not frames: 3x room for a slow-stepped game
ARRIVE_FRAMES = 300                # 10 s: B's pose reaches A (presence) and A's next frame counts it

ON = r"coop: party wait: on \(set by \d+\)"            # coop_party_wait_game.cpp:142 OnEvent (PARTY_WAIT_MODE)
OFF = r"coop: party wait: off \(set by \d+\)"          # the same line, off
ANY = r"coop: party wait: (Waiting|Everyone|Not everyone)"   # coop_party_wait_game.cpp:191 (every notice)
WAIT_1_2 = r"coop: party wait: Waiting for the other players to sync\.\.\. \(1/2 here\)"   # :191, WaitLine
#           (coop_party_wait.cpp:10: here_with_us 1 = A alone of 2)
SAID = r"coop: party wait: said to the others: Waiting for the party at the boss door \(Queen Gohma\)"
#        coop_hook.cpp:2076 (out.chat, coop_party_wait.cpp:95 + WhereText :15; 'Queen Gohma' coop_boss_scenes.cpp:21)
ALL_HERE = r"coop: party wait: Everyone is here\."     # :191 (coop_party_wait.cpp:68)
GAVE_UP = r"coop: party wait: Not everyone is here: going on\."   # :191 (coop_party_wait.cpp:72)
SHOT_AFTER = 15                    # frames after the line: the notice is on the banner (chat_.Add(line, true))


def rules(pair, on):
    """A (the host, test_dbno_crawl.rules) says '/wait on|off' (coop_party_wait.cpp Parse; host only); both games
    log the server's PARTY_WAIT_MODE."""
    at = {x: pair.log[x].mark() for x in ("A", "B")}
    pair.act("A", "chat /wait %s" % ("on" if on else "off"))
    for x in ("A", "B"):
        pair.until("%s log /%s/" % (x, ON if on else OFF),
                   lambda x=x: pair.log[x].find(ON if on else OFF, pos=at[x]), max_frames=300)


def ring(p):
    """(in the area, not in the room) for position p, by the module's own sums (InArea / InRoom)."""
    d = math.hypot(p[0] - CX, p[2] - CZ)
    area = d <= RADIUS + ANTE_GROW and Y_LOW - ANTE_HIGH <= p[1] <= Y_HIGH + ANTE_HIGH
    room = d <= RADIUS and Y_LOW <= p[1] <= Y_HIGH
    return area and not room


def fresh_visit(pair):
    """Both games to Kakariko and back into the Deku Tree as child Link: a new visit (coop_party_wait.cpp Frame: a
    scene change clears the moments already waited), whatever an earlier test left. Inside pair.setup()."""
    to_place(pair, "child", *BOOT)
    to_place(pair, "child", SCENE, ENTRANCE)


def to_door(pair):
    """A from the corridor (inside the room: no moment) to the ring's middle. Returns A's place; VOID when not there."""
    if not walk_to(pair, "A", RING_X, CORRIDOR[2], tol=WALK_TOL, max_frames=WALK_FRAMES) or \
            not ring(pair.state("A")["pos"]):
        p = pair.state("A")["pos"]
        pair.unproven("A did not reach the ring before Gohma's room (37..51 m from %.1f %.1f, y %.0f..%.0f): A at "
                      "%.2f %.2f %.2f, %.1f m from the centre (the probe result: no place known there)" % (
                          CX, CZ, Y_LOW - ANTE_HIGH, Y_HIGH + ANTE_HIGH, p[0], p[1], p[2],
                          math.hypot(p[0] - CX, p[2] - CZ)))
    p = pair.state("A")["pos"]
    pair.notes.append("A in the ring at %.2f %.2f %.2f (%.1f m from the room's centre)" % (
        p[0], p[1], p[2], math.hypot(p[0] - CX, p[2] - CZ)))
    return p


def set_up(pair):
    """A fresh visit, B in room E (not here), A in the corridor (in the room), THEN the rules on: both Links are at
    known places when the setting comes on (a moment met while turning it on is waited once a visit)."""
    with pair.setup():
        fresh_visit(pair)
        place(pair, "B", ROOM_E)
        place(pair, "A", CORRIDOR)
        pair.step_both(30)
        assert not ring(pair.state("A")["pos"]), "A's corridor place is in the ring (a moment): %s" % pair.state("A")["pos"]
        rules(pair, True)


def put_back(pair):
    rules(pair, False)


@pytest.mark.budget(300)   # set-up (4 warps) + 900 walk + 300 arrive + shots
@pytest.mark.look
@pytest.mark.row("#298 wait for the party: the boss door's wait line, the others' chat line, 'Everyone is here.'")
def test_party_wait_boss_door(pair):
    set_up(pair)
    try:
        at = {x: pair.log[x].mark() for x in ("A", "B")}
        pair.step_both(60)
        line = pair.log["A"].find(ANY, pos=at["A"])
        assert line is None, "A waited in the corridor (inside Gohma's room: not a moment): %s" % line
        to_door(pair)
        pair.until("A log /%s/" % WAIT_1_2, lambda: pair.log["A"].find(WAIT_1_2, pos=at["A"]), max_frames=120)
        said = pair.until("A log /%s/" % SAID, lambda: pair.log["A"].find(SAID, pos=at["A"]), max_frames=120)
        assert "(not sent)" not in said, "A's chat line to the others was not sent: %s" % said
        pair.step_both(SHOT_AFTER)
        pair.check_shot("A", "party-wait-waiting-1", tolerance=0.80)    # A's banner: the wait line (moving Links: 0.80)
        pair.check_shot("B", "party-wait-chat-1", tolerance=0.80)       # B's banner: A's chat line
        a = pair.state("A")["pos"]
        pair.act("B", "place %.2f %.2f %.2f" % (a[0] + BESIDE_M, a[1], a[2]))
        pair.until("A log /%s/" % ALL_HERE, lambda: pair.log["A"].find(ALL_HERE, pos=at["A"]), max_frames=ARRIVE_FRAMES)
        line = pair.log["A"].find(GAVE_UP, pos=at["A"])
        assert line is None, "A gave up although B came: %s" % line
        pair.step_both(SHOT_AFTER)
        pair.check_shot("A", "party-wait-all-here-1", tolerance=0.80)   # A's banner: 'Everyone is here.'
    finally:
        put_back(pair)


@pytest.mark.budget(420)   # set-up + 900 walk + 5400 frames for the 60 s + the walk meanwhile + a shot
@pytest.mark.look
@pytest.mark.row("#298 wait for the party: nobody comes: 60 s, then 'Not everyone is here: going on.', never blocked")
def test_party_wait_gives_up(pair):
    set_up(pair)
    try:
        at = pair.log["A"].mark()
        p0 = to_door(pair)
        pair.until("A log /%s/" % WAIT_1_2, lambda: pair.log["A"].find(WAIT_1_2, pos=at), max_frames=120)
        # never blocked: A walks 2 m along the ring meanwhile (stays in it: the same moment, so the wait goes on)
        to = (p0[0], p0[2] + 2.0)
        moved = walk_to(pair, "A", to[0], to[1], tol=0.5, max_frames=240)
        p1 = pair.state("A")["pos"]
        assert moved and math.hypot(p1[0] - p0[0], p1[2] - p0[2]) >= 1.5, (
            "A could not walk while waiting: %s -> %s" % (p0, p1))
        if not ring(p1):
            pair.unproven("A's walk left the ring (%s): the wait ended by leaving, not by the time" % p1)
        pair.until("A log /%s/" % GAVE_UP, lambda: pair.log["A"].find(GAVE_UP, pos=at), max_frames=GIVE_UP_FRAMES)
        line = pair.log["A"].find(ALL_HERE, pos=at)
        assert line is None, "'Everyone is here.' with B in room E: %s" % line
        pair.step_both(SHOT_AFTER)
        pair.check_shot("A", "party-wait-gave-up-1", tolerance=0.80)    # A's banner: 'Not everyone is here: going on.'
        pair.check_shot("B", "party-wait-gave-up-1", tolerance=0.80)    # B's screen: no notice of A's (A's line only)
    finally:
        put_back(pair)


@pytest.mark.budget(240)   # set-up + 900 walk + 120
@pytest.mark.row("#298 wait for the party: off (the default): no party wait line at the boss door")
def test_party_wait_off(pair):
    with pair.setup():
        fresh_visit(pair)
        place(pair, "B", ROOM_E)
        place(pair, "A", CORRIDOR)
        pair.step_both(30)
        rules(pair, True)                       # on, then off: the off line proves the server's value reached both
        rules(pair, False)
    at = {x: pair.log[x].mark() for x in ("A", "B")}
    to_door(pair)
    pair.step_both(120)
    for x in ("A", "B"):
        line = pair.log[x].find(ANY, pos=at[x]) or pair.log[x].find(r"coop: party wait: said to the others", pos=at[x])
        assert line is None, "%s: a party wait line with the setting off: %s" % (x, line)
