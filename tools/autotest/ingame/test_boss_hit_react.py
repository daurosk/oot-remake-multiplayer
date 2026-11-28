"""Row 138 (2)(7) in the bridge form (boss-hit-react-138, B #148: port of scenarios/hs-boss-hit-react-dodongo.{A,B}.txt,
enemy-owner-0115 / #238): a non-owner's hits that TAKE LIFE leave the reaction to the boss's owner.

A owns King Dodongo (A arrives first); B's copy takes A's verbs. A bombs his mouth (the stun, A's own); while he lies
stunned B slashes his head five times (B's own hits on a copy). Before (0.11.4) each slash ran B's copy's own brain
('our own hit: the copy runs its own reaction'): his recovery and roll went another way on B's screen and snapped back.
Fixed: a hit that took life logs 'our own hit took life: its owner's game reacts' and no own reaction; a hit that took
NO life (his armour, no stun) still runs B's own reaction (allowed), as does a bomb stun of B's own.

| Test | Proves |
|---|---|
| test_copy_hit_that_took_life_leaves_the_reaction_to_the_owner | no slash of B's that took life (B's world-list life of the boss fell) ran the copy's own reaction; B logged the life hit; the counts line has K > 0 hits left to the owner; shots of both games right after the slashes (NO-REF until owner-approved refs) |

Results: FAIL = a slash that took life ran B's own reaction (the 0.11.4 bug), or the counts line stayed at K = 0 after
B's life line. NO-REF (VOID, pair.unproven) = no slash took life (run ef002962 had K = 0: the slashes missed the stun),
or own-reaction lines came for slashes whose life change B's world list did not show (cannot be judged per slash).

Sources (worker-rules "no guessed values"):
- No needs_env: native boss sync is on by default (hookshot/src/coop_hookshot.cpp SetBossNative, BOSS_NATIVE != "0");
  the scenario's LICHII_COOP_DEBUG_BOSS / DEBUG_OBJECTS only add info lines, none judged here (as test_boss_respawn).
- Arrival and set-up: test_boss_respawn.start_check / boss_arrival / back_after (imported, never copied); the stage
  start {"DodongoStage", "DodongoStage", ...} of lichii/hook/coop_start_points.inc (the scenarios' 'warp DodongoStage
  DodongoStage').
- Notes are logged 'coop: bosses: <note>' (lichii/hook/coop_objects.cpp, bosses.TakeNotes). From
  lichii/hook/coop_boss_native.cpp: 'NAME: native boss sync on (BSA 0x.., ours|a copy)' (Table, first sight);
  Table::NoteOwnHit 'NAME: our own hit: the copy runs its own reaction' (a hit that took no life, outside the copy's own
  time); Table::NoteOwnDamage 'NAME: our own hit took life: its owner's game reacts ...' (ONCE per session:
  copy_hits_to_owner++ == 0); Table::NoteOwnStun 'its own stun in our game' (B's own bomb: allowed).
  coop_objects.cpp NewOwnHitAt: took_life -> NoteOwnDamage, else NoteOwnHit (the fix this row proves).
- Counts line: coop_hookshot.cpp LogBossNative, every 10 s of WALL clock and only when a count changed:
  '... {} own stuns; {} hits that took life left to the owner), ...'.
- Inputs and frames: scenarios/hs-boss-hit-react-dodongo.A/B.txt: 'give bombs' (docs/playtest.md `give`) + 'assign
  Bomb' (an `act` kind) on both, 240 frames; A 'wait 1800' (the intro, then he walks), 'press ZL 10', 'wait 30', 'press
  ZR', 'wait 120', 'hold ZL+LSTICK_UP 60', 'press A' (bomb 1); B's slashes start 614 frames after A's bomb (B's title
  wait is 300 frames longer and its 'wait 2350': 300 + 2350 - 2036), 'hold ZL+LSTICK_UP 90', then 5x 'hold ZL+Y 8' +
  'wait 40' (the sword is Y); w01..w16 every 60 frames; B's own bomb after its w16, A's bomb 2 600 frames after its w16;
  the last 'wait 1800'. LSTICK_UP = stick [0, 1] (test_boss_respawn's 'hold LSTICK_UP 60'); a 'press' is the bridge's
  8-frame input (Pair.press frames=8).
- Life: B's world list (test_enemies.actors: the bridge `dump`), the boss's `life` (docs/playtest.md world list), read
  before each slash and after its 40 frames.
- Shots: tolerance 0.80 (a moving boss, as volvagia-rocks); four 60 frames apart per game right after the slashes
  (w01..w04, his recovery: where the twitch showed), on both games at the same frame (the pair steps together).
"""
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pair as P                   # noqa: E402
import test_boss_respawn as BR     # noqa: E402  (boss-respawn-289-4's steps; never edited here)
import test_enemies as TE          # noqa: E402  (actors: one world-list read)
from test_boss_respawn import back_after   # noqa: E402,F401  (fixture: the way back gets its own deadline)

STAGE = "DodongoStage"
BOSS = "Enemy_DungeonBoss_Dodongo"
SYNC_ON = r"coop: bosses: %s: native boss sync on \(BSA 0x[0-9a-fA-F]+, %s\)"     # coop_boss_native.cpp
# The owner's stall note (coop_boss_native.cpp Note "...: ours held '<anim>' (brain B, action A, life L) N s with no hit:
# stuck", after kStallSteps = 1500 game steps, coop_boss_native.h). Run boss-hit-react-53d40ab3-2 (2026-11-28): A logged
# it for 'Wait' 3 times and no intro event ran after the warp: the fight never began, so no bomb could stun him.
STALL_WAIT = r"coop: bosses: %s: ours held 'Wait' \(brain \d+, action [^,]+, life \d+\) \d+ s with no hit: stuck" % BOSS
# Why: a warp plays no boot event. The game's own rematch boots DodongoStage with Get_StoneOfFire_AppearBoss, which makes
# him appear (DreamWorld_BossAppear); without it he stays in 'Wait' (docs/boss-multiplayer.md "Boss rush stages reached
# by a warp", boss-route-01112). Asked on both games after the arrival, as scenarios/kd-roll-hold-325.A/B.txt ('event
# Get_StoneOfFire_AppearBoss' + 'wait 1800  # the intro, then the fight'; big-run-01113 PASS, judge-c1 21:46 11-26),
# through the bridge's act (lichii/hook/coop_testinput.cpp ActKinds, Kind::event; logged 'coop: test event NAME: handed').
APPEAR = "Get_StoneOfFire_AppearBoss"
HANDED = r"coop: test event %s: handed to the live-event requester" % APPEAR   # coop_testinput.cpp Kind::event
ANIM = r"coop: objects: anim %s 0x[0-9a-f]+ '([^']+)'" % BOSS                   # coop_objects.cpp (each change, <= 400)
OWN_REACT = r"coop: bosses: %s: our own hit: the copy runs its own reaction" % BOSS   # NoteOwnHit
TOOK_LIFE = r"coop: bosses: %s: our own hit took life: its owner's game reacts" % BOSS   # NoteOwnDamage (once)
OWN_STUN = r"coop: bosses: %s: its own stun in our game" % BOSS                         # NoteOwnStun (allowed)
COUNTS = r"coop: bosses: ours sent .*; (\d+) hits that took life left to the owner"    # LogBossNative
COUNTS_WALL_S = 10.0                          # LogBossNative: at most one line per 10 s of wall clock

UP = [0, 1]                                   # LSTICK_UP (test_boss_respawn 'hold LSTICK_UP 60')
GIVE_FRAMES = 240                             # 'give bombs' + 'wait 120' + 'assign Bomb' + 'wait 120'
INTRO_FRAMES = 1800                           # A 'wait 1800  # the intro, then he walks'
BOMB = (("ZL",), None, 10), (None, None, 30), (("ZR",), None, 8), (None, None, 120), (("ZL",), UP, 60), (("A",), None, 8)
B_BOMB = BOMB[2:]                             # B's own bomb: 'press ZR', 'wait 120', 'hold ZL+LSTICK_UP 60', 'press A'
TO_SLASH = 614                                # B's slashes begin this long after A's bomb (module docstring)
LOCK_FRAMES = 90                              # B 'hold ZL+LSTICK_UP 90'
SLASHES, SLASH_HOLD, SLASH_WAIT = 5, 8, 40    # B 5x 'hold ZL+Y 8' + 'wait 40'
W_EVERY, W_COUNT, SHOTS = 60, 16, 4           # w01..w16 every 60 frames; shots at w01..w04
BOMB2_AFTER = 600                             # A 'wait 600' after w16, then bomb 2
END_FRAMES = 1800                             # the scenarios' last 'wait 1800': the counts line's wait


def seq_frames(seq):
    return sum(f for _, _, f in seq)


FRAMES = (GIVE_FRAMES + INTRO_FRAMES + seq_frames(BOMB) + TO_SLASH + LOCK_FRAMES
          + SLASHES * (SLASH_HOLD + SLASH_WAIT) + W_COUNT * W_EVERY + BOMB2_AFTER + seq_frames(B_BOMB) + END_FRAMES)
# The budget from its parts (as test_volvagia_rocks): the arrival's own ceiling + two set-ups + the game frames twice
# (a margin) + 60 s for the 8 shots and 12 world reads.
BUDGET_S = int(BR.BOSS_ARRIVAL_S + 2 * P.SETUP_LIMIT_S + 2 * FRAMES / P.FPS + 60.0) + 1


def play(pair, x, seq):
    """A scenario's press/hold/wait lines on game x (the other game steps the same frames)."""
    for buttons, stick, frames in seq:
        if buttons is None:
            pair.step_both(frames)
        else:
            pair.press(x, list(buttons), stick=stick, frames=frames)


def boss_life(pair, x):
    """Game x's life of the boss in its world list (the first with a life), or None."""
    for a in TE.actors(pair, x, BOSS):
        if a.get("life") is not None:
            return a["life"]
    return None


def arm(pair):
    """Bombs on ZR in both games (the scenarios' 'give bombs' + 'assign Bomb'); any miss is SETUP-FAIL."""
    with pair.setup():
        for x in ("A", "B"):
            r = pair.give(x, "bombs")
            assert "failed" not in str(r.get("result", "")), "%s: give bombs: %s" % (x, r)
            pair.act(x, "assign Bomb")
        pair.step_both(GIVE_FRAMES)


def sync_on(pair, start):
    """After the intro: A's boss sync line says ours, B's a copy (the row's roles); a miss is SETUP-FAIL."""
    for x, role in (("A", "ours"), ("B", "a copy")):
        if not pair.log[x].find(SYNC_ON % (BOSS, role), start[x]):
            raise P.SetupFail("boss sync: %s has no '%s: native boss sync on (..., %s)' after the intro" % (x, BOSS, role))


def appear(pair):
    """Both games asked his appear event (the stage's boot event a warp does not play); a refusal or no 'handed' line
    is SETUP-FAIL. Returns each game's log position before the ask."""
    pos = {x: pair.log[x].size() for x in ("A", "B")}
    with pair.setup():
        for x in ("A", "B"):
            r = pair.act(x, "event %s" % APPEAR)
            assert r.get("ok", True) is not False, "%s: act event %s: %s" % (x, APPEAR, r)
        pair.step_both(10)
        for x in ("A", "B"):
            if not pair.log[x].find(HANDED, pos[x]):
                raise P.SetupFail("%s: no '%s' after its act" % (x, HANDED))
    return pos


def awake(pair, since):
    """After the intro wait: A's boss must not have stalled in 'Wait' since his appear event was asked (the fight never
    began); a stall is SETUP-FAIL. A's anim names since then go in the notes."""
    anims = [m.group(1) for ln in pair.log["A"].since(since["A"]) for m in [re.search(ANIM, ln)] if m]
    pair.notes.append("A's %s anims after the appear event: %s" % (BOSS, anims[:12] or "none logged"))
    stall = pair.log["A"].find(STALL_WAIT, since["A"])
    if stall:
        raise P.SetupFail("%s never woke in A: '%s' after the appear event + %d frames (no fight, so no stun to slash)"
                          % (BOSS, stall, INTRO_FRAMES))


def slashes(pair):
    """B's five slashes; per slash: B's boss life before and after its 40 frames and B's log lines in its window."""
    rows = []
    for i in range(1, SLASHES + 1):
        before = boss_life(pair, "B")
        pos = pair.log["B"].size()
        pair.press("B", ["ZL", "Y"], frames=SLASH_HOLD)
        pair.step_both(SLASH_WAIT)
        after = boss_life(pair, "B")
        lines = pair.log["B"].since(pos)
        rows.append({"i": i, "before": before, "after": after,
                     "own": any(re.search(OWN_REACT, ln) for ln in lines),
                     "took": any(re.search(TOOK_LIFE, ln) for ln in lines)})
        pair.notes.append("slash %d: B's %s life %s -> %s; own reaction %s; took-life line %s" % (
            i, BOSS, before, after, rows[-1]["own"], rows[-1]["took"]))
    return rows


def fell(r):
    return r["before"] is not None and r["after"] is not None and r["after"] < r["before"]


def counts_k(pair, start):
    """The highest K of B's counts lines since `start` (None: no counts line)."""
    ks = [int(m.group(1)) for ln in pair.log["B"].since(start) for m in [re.search(COUNTS, ln)] if m]
    return max(ks) if ks else None


@pytest.mark.budget(BUDGET_S)                 # from its parts (BUDGET_S above)
@pytest.mark.look
@pytest.mark.row("#138 (2)(7) a non-owner's boss hit that took life leaves the reaction to the owner "
                 "(hs-boss-hit-react-dodongo)")
def test_copy_hit_that_took_life_leaves_the_reaction_to_the_owner(pair, back_after):
    clock = pair.deadline.clock
    with pair.setup():
        BR.start_check(pair)
    start = {x: pair.log[x].size() for x in ("A", "B")}   # (every line judged below is this test's own)
    BR.boss_arrival(pair, STAGE)
    arm(pair)
    asked = appear(pair)
    pair.step_both(INTRO_FRAMES)
    sync_on(pair, start)
    awake(pair, asked)
    play(pair, "A", BOMB)                    # A's bomb 1: the stun, A's own
    pair.step_both(TO_SLASH)
    pair.press("B", ["ZL"], stick=UP, frames=LOCK_FRAMES)
    rows = slashes(pair)
    for w in range(1, W_COUNT + 1):
        pair.step_both(W_EVERY)
        if w <= SHOTS:                        # his recovery on both screens, the same frame
            for x in ("A", "B"):
                pair.check_shot(x, "boss-hit-react-138-w%02d" % w, tolerance=0.80)
    play(pair, "B", B_BOMB)                   # B's own bomb (a stun of its own: its own reaction, allowed)
    pair.step_both(BOMB2_AFTER - seq_frames(B_BOMB))
    play(pair, "A", BOMB[2:])                 # A's bomb 2 ('press ZR' .. 'press A')
    pair.notes.append("B's own stun line: %s" % bool(pair.log["B"].find(OWN_STUN, start["B"])))

    # FAIL first: a slash that took life ran B's own reaction (the 0.11.4 bug)
    bad = [r["i"] for r in rows if r["own"] and (fell(r) or r["took"])]
    assert not bad, "B: slash(es) %s took life but ran the copy's own reaction ('%s'): the owner's reaction was not " \
                    "left to it" % (bad, OWN_REACT)
    took = pair.log["B"].find(TOOK_LIFE, start["B"])
    if not took:
        pair.unproven("B never logged '%s: our own hit took life' (no slash took life: lives %s; the slashes missed A's "
                      "stun)" % (BOSS, [(r["before"], r["after"]) for r in rows]))
    unjudged = [r["i"] for r in rows if r["own"] and not fell(r) and (r["before"] is None or r["after"] is None)]
    if unjudged:
        pair.unproven("own-reaction lines for slash(es) %s whose life B's world list did not show" % unjudged)
    t = clock()
    try:
        pair.until("B's counts line with K > 0 hits left to the owner", lambda: (counts_k(pair, start["B"]) or 0) > 0,
                   max_frames=END_FRAMES)
    except AssertionError:
        if clock() - t < COUNTS_WALL_S:
            pair.unproven("no counts line with K > 0 in %.1f s of wall clock (it comes at most every %.0f s)" % (
                clock() - t, COUNTS_WALL_S))
        raise AssertionError("B logged '%s' but its counts lines stayed at K = %s hits left to the owner after %d "
                             "frames (%.1f s)" % (took, counts_k(pair, start["B"]), END_FRAMES, clock() - t))
    pair.notes.append("B counts: K = %d hits that took life left to the owner" % counts_k(pair, start["B"]))
