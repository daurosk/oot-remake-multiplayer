"""Rows 230 / 231 / 232 (enemies shared by their owner) as bridge tests: ports of coord offline/enemies-proof-230-run.ps1's
scenarios hs-redead-scream-230, hs-meg-decoys-231, hs-deadhand-grab-232 and hs-deadhand-grab-232b (branch
scream-share-230-run 4dc5e7324; the module code they prove is on this base: fd6092f1, a3975b6d, 1b349024).

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_redead_scream | hs-redead-scream-230 (row 230) | default (child Kakariko) | scream lines on both games + Link reaction line + shots |
| test_meg_decoys | hs-meg-decoys-231 (row 231 + enemies-dying e8184823) | live-meg-room | decoy seats from both world lists + death lines + shots |
| test_deadhand_grab | hs-deadhand-grab-232 (row 232) | live-deadhand-room | grab lines + Dead Hand's owner in both world lists + shots |
| test_deadhand_grab_other_way | hs-deadhand-grab-232b (row 232, B held while A owns him) | live-deadhand-room-b | as above |

Boots: the Redead row loads the kit save (the default boot) and makes both Links adult in the ruined Market; the way
back leaves both child in Kakariko for the lane's later tests. Meg and Dead Hand live in boss rush stages that a free-play
warp cannot reach (docs/playtest.md `warp`: a map outside Longshot's list answers not_in_event) and that need the rush's
arrival event (`event DreamBossStage_Rematch_Arrive`, not an `act` kind): the old scenarios' own boot lines are
scenarios/live-meg-room.*, live-deadhand-room.* and live-deadhand-room-b.* (run with --boot NAME; needs_boot keeps these
tests off any other boot). They change the stage for good (Meg killed, Dead Hand's owner moved), so nothing is put back,
and each Dead Hand test has its own boot; its set-up also checks the game to be held does not own him yet (NO-REF).

Every log pattern is the module's own line (cited at its writer); every place is the old scenario's (cited). Judged by
game state (module log lines, each game's world list: owner, alive, pos: docs/playtest.md "world"), with shots on each
game for what a player sees (look rows: the frozen Links, Meg's seats, Dead Hand at the held Link). No reference shots
yet: the shots are kept and the result is NO-REF until the owner approves references (pair.check_shot).
"""
import math
import re

import pytest

from test_gear import AGE_FLAG, GATE, SCENE_CHILD, shots   # noqa: E402  (the same way back, same shot rule)
from test_rows import need_child_in_play                   # noqa: E402

# ---- row 230: the Redead's scream -----------------------------------------------------------------------------------
MARKET, MARKET_ENTRANCE = "Market_Adult", "Market_Entrance"   # hs-redead-scream-230.A.txt 'warp Market_Adult Market_Entrance'
REDEAD = "Enemy_ReDead"
RD = (-3.17, 100.15, 13.13)        # hs-redead-scream-230.A.txt header: the standing Redead (Banc Market_Adult_Dynamic)
A_FAR = (0.67, 100.2, 8.57)        # 'place 0.67 100.2 8.57 # 6 m in front of the Redead'
A_NEAR = (-1.51, 11.15)            # 'goto -1.51 11.15 0.5 300 # walk in to 2.6 m'
WALK_TOL, WALK_FRAMES = 0.5, 300   # the goto's tolerance and frames
B_SPOT = (-0.24, 100.2, 11.45)     # hs-redead-scream-230.B.txt 'place -0.24 100.2 11.45 # 3.4 m in front, 20 deg'
A_AWAY, B_AWAY = (-1.7, 102.2, 60.0), (-3.0, 102.2, 60.0)   # 'place ... # away, back by the entrance'
AWAIT_SCREAM = 2400                # 'await 2400 scream: ...'
REACH_M = 5.5                      # kScreamReachM (lichii/hook/coop_objects_sync.h:441; InScreamReach, coop_objects_sync.cpp:863)
FROZEN = "Paralysis"               # the AS node of a frozen Link (lichii/hook/coop_objects.cpp:2379-2385 comment)
LINE_FRAMES = 120                  # a peer's line after the owner's: one sample (sent at once, coop_objects_sync.cpp:3401)
#                                    + the reaction look 0.5 s on (kScreamLookUs = 500'000, coop_objects.cpp:6871); the rest
#                                    of the 2 s is an ESTIMATED margin (not measured)
SCREAM_ENV = ("LICHII_COOP_DEBUG_OBJECTS=1",)   # hs-redead-scream-230.A.txt '#! switches' (CHEATS=1 is the default boot's)

# 'coop: objects: share <note>' (coop_objects.cpp:2660) of coop_objects_sync.cpp's notes:
OWNER = r"scream: our Enemy_ReDead froze our Link \('([^']*)', \d+ quarters\): shared for the Links in its reach"   # :3403
RECV_IN = r"scream: the owner's Enemy_ReDead froze its Link; ours is in its reach \(([\d.]+) m\): frozen here too"  # :4418
RECV_OUT = r"scream: the owner's Enemy_ReDead froze its Link; ours (?:is out of its reach \(([\d.]+) m\)|not seen)"  # :4415, :4421
REACTION = r"scream: our Link plays '([^']*)' 0\.5 s after the shared scream's hit"   # coop_objects.cpp:2384

# ---- row 231: Meg's decoys --------------------------------------------------------------------------------------------
MEG_BOOT, MEG_STAGE = "live-meg-room", "MegStage"
MEG, DUMMY = "Enemy_MiddleBoss_Poe_Meg", "Enemy_MiddleBoss_Poe_Meg_Dummy"
SPLIT = r"coop: objects: \+ Enemy_MiddleBoss_Poe_Meg_Dummy "   # hs-meg-decoys-231.A.txt 'await ... coop: objects: \+ ..._Dummy'
SPLIT_FIRST, SPLIT_NEXT, SPLITS, SPLITS_MIN = 3600, 2400, 3, 2   # its awaits; PASS needs >= 2 splits seen (its header)
SPLIT_SETTLE, SPLIT_B = 20, 18     # 'mark a-splitN; wait 20; mark a-splitNb' (+ the 2 frames of a shot pair)
# a copy within SEAT_M of an owner's decoy stands at that seat: half the smallest gap between two of the owner's seats
# (4.25 m: A's decoys at -1.54 1.21 / -2.54 5.34 / 2.59 2.22, run shots-lane1/ep230-hs-meg-decoys-231/20261123-090930
# hookshot_log-A.txt 'coop: objects: + Enemy_MiddleBoss_Poe_Meg_Dummy ... at')
SEAT_M = 2.0
LOOP = r"minions: our copy of Enemy_MiddleBoss_Poe_Meg made"   # coop_objects_sync.cpp:3088 (the FAIL line of its header)
DEATH = r"coop: bosses: Enemy_MiddleBoss_Poe_Meg: its (?:owner's died|death has begun)"   # coop_boss_native.cpp:614, :1066
NOT_TAKEN = r"'Enemy_MiddleBoss_Poe_Meg[^']*': its Life step did not take"   # coop_objects.cpp:4171, :4186
KILL_WAIT, DEATH_WAIT = 120, 300   # 'kill ...; wait 120; mark a-kill; wait 300; mark a-death'
BOSS_ENV = ("LICHII_COOP_DEBUG_BOSS=1", "LICHII_COOP_DEBUG_OBJECTS=1")   # hs-meg/deadhand '#! switches' (+ CHEATS=1)

# ---- row 232: Dead Hand's grab ------------------------------------------------------------------------------------
DH_BOOT, DH_STAGE, DEADHAND = "live-deadhand-room", "DeadHandStage", "Enemy_DeadHand"
# 232b gets its own boot (= its own lane job): 232 moves Dead Hand's owner for good, so on 232's boot B already owns
# him and 232b's hand-over would hold with no grab (review-rows-c130-2 BLOCKER). Same steps as live-deadhand-room.
DH_BOOT_B = "live-deadhand-room-b"
BY_FIRST = (0.0, 92.4, -107.2)     # hs-deadhand-grab-232.A.txt 'place 0.0 92.4 -107.2 # by the first hand (0, -108.43)'
BY_SECOND = (0.0, 92.4, -99.6)     # hs-deadhand-grab-232.B.txt 'place 0.0 92.4 -99.6 # by the order-2 hand'
OUT_OF_REACH = (0.0, 92.4, -112.0)   # hs-deadhand-grab-232.A.txt 'place 0.0 92.4 -112.0 # back, out of the hands' reach'
# a copy of Dead Hand more than DH_SAME_M from the owner's stands at another hand: half the 10 m between the first hand
# (0, -108.43) and the order-2 hand (0, -98.43) (hs-deadhand-grab-232.A.txt header, DeadHandStage Banc)
DH_SAME_M = 5.0
GRAB_ON = r"grab: our Enemy_DeadHand's piece holds our Link: Enemy_DeadHand wanted here"   # coop_objects_sync.cpp:3555
GRAB_OFF = r"grab: no piece of Enemy_DeadHand holds our Link now"                         # coop_objects_sync.cpp:3560
GRAB_FIRST, GRAB_FREE, GRAB_SECOND = 3600, 3600, 7200   # the scenarios' awaits (232.A 3600 + 3600, 232.B 7200)
# the hand-over waits for the owner's attack to end (server/session.cpp:3215-3232, ask_handover); the old row judged it on
# its b-grab-3s and b-grab-6s shots: 360 frames
HANDOVER = 360


# ---- shared helpers -----------------------------------------------------------------------------------------------
def flat(p, q):
    return math.hypot(p[0] - q[0], p[2] - q[2])


def me(pair, x):
    """Game x's own player number (state coop.players, the entry with self true: docs/playtest.md state)."""
    for p in (pair.state(x).get("coop") or {}).get("players") or []:
        if p.get("self"):
            return p.get("number")
    raise AssertionError("%s: no own entry (self) in state coop.players" % x)


def actors(pair, x, name):
    """Game x's world-list actors called `name` (the whole list, playtest_client.Client.world)."""
    w = pair.c[x].world().get("world") or {}
    return [a for a in w.get("actors", []) if a.get("name") == name]


def alive(acts):
    return [a for a in acts if a.get("alive") is not False and a.get("pos")]


def other(x):
    return "B" if x == "A" else "A"


def place(pair, x, p):
    pair.act(x, "place %.2f %.2f %.2f" % tuple(p))


def face(pair, x, p):
    pair.act(x, "face %.2f %.2f" % (p[0], p[2]))


def walk_to(pair, x, tx, tz, tol=WALK_TOL, max_frames=WALK_FRAMES):
    """A scenario's 'goto X Z TOL FRAMES': the bridge's `move` (world yaw, 0 = +z, 90 = +x: docs/playtest.md move) 8
    frames at a time toward the point, the other game stepped the same frames. True on arrival."""
    ran = 0
    while ran < max_frames:
        p = pair.state(x)["pos"]
        if math.hypot(tx - p[0], tz - p[2]) <= tol:
            return True
        pair.call(x, "move", yaw=math.degrees(math.atan2(tx - p[0], tz - p[2])) % 360.0, strength=1.0, frames=8)
        pair.step_other(other(x), 8)
        ran += 8
    p = pair.state(x)["pos"]
    return math.hypot(tx - p[0], tz - p[2]) <= tol


def in_stage(pair, stage):
    for x in ("A", "B"):
        s = pair.state(x)
        assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
        assert (s.get("scene") or "").lower() == stage.lower(), "%s is in %s, not %s (the boot's stage)" % (
            x, s.get("scene"), stage)


# ---- row 230 --------------------------------------------------------------------------------------------------------
def scream_round(pair, r):
    """One round of hs-redead-scream-230: B 3.4 m in front of the Redead, A walks in from 6 m to 2.6 m; either game may
    own it. Returns None (no scream this round) or (owner game, owner's anim, receiver line). FAILs on a scream that
    did not reach a Link standing in its reach, or a receiver Link that did not freeze."""
    start = {x: pair.log[x].mark() for x in ("A", "B")}
    place(pair, "B", B_SPOT)
    face(pair, "B", RD)
    place(pair, "A", A_FAR)
    face(pair, "A", RD)
    pair.step_both(2)
    if not walk_to(pair, "A", *A_NEAR):
        pair.notes.append("round %d: A's walk in ended %.1f m from the goto point" % (
            r, math.hypot(A_NEAR[0] - pair.state("A")["pos"][0], A_NEAR[1] - pair.state("A")["pos"][2])))

    def owner():
        for x in ("A", "B"):
            ln = pair.log[x].find(OWNER, pos=start[x])
            if ln:
                return x, ln
        return None
    try:
        own, line = pair.until("round %d: a Redead's scream on either game" % r, owner, max_frames=AWAIT_SCREAM)
    except AssertionError:
        pair.notes.append("round %d: no scream in %d frames" % (r, AWAIT_SCREAM))
        return None
    rx = other(own)
    shots(pair, "redead-scream%d" % r)            # both Links in the paralysed pose, the Redead in its scream pose
    pair.step_both(60)                            # 'wait 60'
    shots(pair, "redead-frozen%d" % r)
    got = pair.until("round %d: %s's line for %s's scream" % (r, rx, own), lambda: (
        pair.log[rx].find(RECV_IN, pos=start[rx]) or pair.log[rx].find(RECV_OUT, pos=start[rx])), max_frames=LINE_FRAMES)
    if not re.search(RECV_IN, got):
        d = flat(pair.state(rx)["pos"], RD)
        assert d > REACH_M, ("round %d: %s's Link stands %.1f m in front of the Redead (inside its %.1f m reach) but "
                             "the scream did not reach it: %s" % (r, rx, d, REACH_M, got))
        pair.notes.append("round %d: %s's Link was %.1f m off (outside the reach): %s" % (r, rx, d, got))
        return None
    react = pair.until("round %d: %s's Link reaction line" % (r, rx), lambda: pair.log[rx].find(REACTION, pos=start[rx]),
                       max_frames=LINE_FRAMES)
    anim = re.search(REACTION, react).group(1)
    assert anim == FROZEN, "round %d: %s's Link plays '%s', not '%s', after %s's shared scream (%s)" % (
        r, rx, anim, FROZEN, own, react)
    pair.step_both(300)                           # 'wait 300'
    place(pair, "A", A_AWAY)
    place(pair, "B", B_AWAY)
    pair.step_both(900)                           # 'wait 900': away, the Redead calms down
    return own, re.search(OWNER, line).group(1), got


def market_back(pair, done):
    """The boot as it was (as test_gear.way_back): both child in Kakariko. A failure is raised only when the test body
    passed (`done`); else noted, so the test's own message stays."""
    try:
        for x in ("A", "B"):
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


@pytest.mark.budget(600)   # ESTIMATED: ~8'200 game frames (2 rounds x 3'900 of the waits above + the warp's 60 + walk) + shots
@pytest.mark.look
@pytest.mark.needs_env(*SCREAM_ENV)
@pytest.mark.row("#230 a Redead's scream freezes every Link in its reach, not only its owner's (hs-redead-scream-230)")
def test_redead_scream(pair):
    done = False
    try:
        with pair.setup():
            need_child_in_play(pair)
            # a name of the other age goes by the module's map change event, which takes the entrance
            # (docs/playtest.md warp; the scenario's 'setflag Sequence_IsChildhood 0' + 'warp Market_Adult Market_Entrance')
            pair.warp_both(MARKET, entrance=MARKET_ENTRANCE)
            pair.step_both(60)
            for x in ("A", "B"):
                s = pair.state(x)
                assert pair.in_play(s) and s.get("age") == "adult", "%s not an adult in play in %s (%s, %s)" % (
                    x, MARKET, s.get("scene"), s.get("age"))
            # 'at the scenario's spot' = within 4 goto tolerances (2 m): an ESTIMATED margin, not measured
            rd = [a for a in alive(actors(pair, "B", REDEAD)) if flat(a["pos"], RD) < WALK_TOL * 4]
            assert rd, "no standing %s at %.2f %.2f %.2f in B's world list (the scenario's Redead)" % ((REDEAD,) + RD)
        seen = [scream_round(pair, r) for r in (1, 2)]
        shared = [s for s in seen if s]
        if not shared:
            pair.unproven("no shared scream in either round (hs-redead-scream-230 VOID: no 'scream:' line, the "
                          "Redead never woke)")
        for own, anim, got in shared:
            pair.notes.append("%s owned the scream ('%s'); %s" % (own, anim, got.split("scream: ")[-1]))
        done = True
    finally:
        market_back(pair, done)


# ---- row 231 --------------------------------------------------------------------------------------------------------
def meg_owner(pair):
    """Meg's owner as a player number, from both world lists (they must agree)."""
    seen = {}
    for x in ("A", "B"):
        m = alive(actors(pair, x, MEG))
        assert m, "%s: no living %s in its world list" % (x, MEG)
        seen[x] = m[0].get("owner")
    assert seen["A"] == seen["B"] and isinstance(seen["A"], int), "Meg's owner differs or is unknown: %s" % seen
    return seen["A"]


def seats_match(pair, n, num):
    """Split n: the owner's living decoys, and the other game's, the same count at the same seats (SEAT_M)."""
    own = "A" if num["A"] == meg_owner(pair) else "B"
    mine = alive(actors(pair, own, DUMMY))
    theirs = alive(actors(pair, other(own), DUMMY))
    assert mine, "split %d: %s (Meg's owner) lists no living %s" % (n, own, DUMMY)
    assert len(theirs) == len(mine), "split %d: %s shows %d Megs' decoys, the owner %s %d (%s vs %s)" % (
        n, other(own), len(theirs), own, len(mine), [a["pos"] for a in theirs], [a["pos"] for a in mine])
    for d in mine:
        near = min(flat(d["pos"], t["pos"]) for t in theirs)
        assert near <= SEAT_M, "split %d: the owner's decoy at %.2f %.2f has no copy at its seat on %s (nearest %.1f m)" % (
            n, d["pos"][0], d["pos"][2], other(own), near)
    return own, len(mine)


@pytest.mark.budget(900)   # ESTIMATED: ~11'300 game frames (SPLIT_FIRST + 2 x SPLIT_NEXT + 3 x 638 + kill/death 1'020) + shots
@pytest.mark.look
@pytest.mark.needs_boot(MEG_BOOT)
@pytest.mark.needs_env(*BOSS_ENV)
@pytest.mark.row("#231 Meg's decoys: only her owner's are kept and shared (same count, same seats); her death once")
def test_meg_decoys(pair):
    if any(pair.log[x].find(DEATH, pos=0) for x in ("A", "B")):
        pytest.skip("Meg died earlier on this boot (an earlier round): the stage has no Meg to split")
    with pair.setup():
        in_stage(pair, MEG_STAGE)
        num = {x: me(pair, x) for x in ("A", "B")}
        meg_owner(pair)
    t0 = {x: pair.log[x].mark() for x in ("A", "B")}   # (the test's window: later marks move each log's own place)
    splits = []
    for n in range(1, SPLITS + 1):
        at = {x: pair.log[x].mark() for x in ("A", "B")}
        try:
            pair.until("split %d: a new decoy on both games" % n, lambda: all(
                pair.log[x].find(SPLIT, pos=at[x]) for x in ("A", "B")), max_frames=SPLIT_FIRST if n == 1 else SPLIT_NEXT)
        except AssertionError as e:
            pair.notes.append(str(e))
            break
        pair.step_both(SPLIT_SETTLE)
        splits.append(seats_match(pair, n, num))
        shots(pair, "meg-split%d" % n)            # the same Megs at the same seats on both screens
        pair.step_both(SPLIT_B)
        shots(pair, "meg-split%db" % n)
        pair.step_both(600)                       # 'wait 600'
    for x in ("A", "B"):
        ln = pair.log[x].find(LOOP, pos=t0[x])
        assert ln is None, "%s: a decoy loop: %s" % (x, ln)
    if len(splits) < SPLITS_MIN:
        pair.unproven("%d split(s) seen, the row needs %d (hs-meg-decoys-231 VOID: no decoy line)" % (len(splits), SPLITS_MIN))
    pair.notes.append("splits (owner, decoys): %s" % splits)
    kill_at = {x: pair.log[x].mark() for x in ("A", "B")}
    for x in ("A", "B"):                          # both kill: a kill on a copy is held at 1, the owner's kills her
        pair.act(x, "kill %s" % MEG)
    pair.step_both(KILL_WAIT)
    pair.until("Meg dead or gone on both games", lambda: not any(alive(actors(pair, x, MEG)) for x in ("A", "B")),
               max_frames=DEATH_WAIT)
    pair.until("a copy's death line", lambda: any(pair.log[x].find(DEATH, pos=kill_at[x]) for x in ("A", "B")),
               max_frames=DEATH_WAIT)
    shots(pair, "meg-death")                      # Meg dying or gone on both
    pair.step_both(DEATH_WAIT)                    # (the rest of 'wait 300' after the kill: a second death line shows)
    for x in ("A", "B"):
        n = sum(1 for ln in pair.log[x].since(kill_at[x]) if re.search(DEATH, ln))
        assert n <= 1, "%s logged Meg's death begun %d times (once: enemies-dying e8184823)" % (x, n)
        ln = pair.log[x].find(NOT_TAKEN, pos=kill_at[x])
        assert ln is None, "%s: Meg's kill did not take: %s" % (x, ln)


# ---- row 232 --------------------------------------------------------------------------------------------------------
def deadhand_at(pair, x):
    d = alive(actors(pair, x, DEADHAND))
    return d[0] if d else None


def not_owned_yet(pair, held, num):
    """Set-up: the game that is to be held must not own Dead Hand yet, or the hand-over check holds with no grab (a boot
    an earlier Dead Hand test already used: it moves his owner for good). Then the row cannot be proven here: NO-REF."""
    own = {x: deadhand_at(pair, x).get("owner") for x in ("A", "B")}
    if num[held] in own.values():
        pair.unproven("%s (player %s) already owns %s before any grab (owner A: %s / B: %s): this boot was used by an "
                      "earlier Dead Hand test, the hand-over cannot be seen (give the test its own boot)" % (
                          held, num[held], DEADHAND, own["A"], own["B"]))


def handed_to(pair, held, num):
    """After `held`'s grab: Dead Hand's owner is `held`'s number in both world lists, and the copy stands at the owner's
    hand (DH_SAME_M). Returns the two positions."""
    def ok():
        d = {x: deadhand_at(pair, x) for x in ("A", "B")}
        return d if all(d[x] and d[x].get("owner") == num[held] for x in ("A", "B")) else None
    try:
        d = pair.until("Dead Hand handed to %s (player %s) on both games" % (held, num[held]), ok, max_frames=HANDOVER)
    except AssertionError:
        d = {x: deadhand_at(pair, x) for x in ("A", "B")}
        raise AssertionError("%s (player %s) held by a hand, but Dead Hand's owner is A: %s / B: %s %d frames on: no "
                             "hand-over to the held game (row 232)" % (
                                 held, num[held], d["A"] and d["A"].get("owner"), d["B"] and d["B"].get("owner"),
                                 HANDOVER))
    gap = flat(d["A"]["pos"], d["B"]["pos"])
    assert gap <= DH_SAME_M, "Dead Hand stands %.1f m apart on the two games (A %s, B %s): at another hand" % (
        gap, d["A"]["pos"], d["B"]["pos"])
    return d


def grab_run(pair, first, second, tag):
    """hs-deadhand-grab-232(b): `first` by the first hand, `second` by the order-2 hand... both placed, `first` held and
    let go, then `second` held: Dead Hand goes to `second` (judged only after `first` is free: the known gap of its
    header, two held at once keep him with his owner)."""
    with pair.setup():
        in_stage(pair, DH_STAGE)
        num = {x: me(pair, x) for x in ("A", "B")}
        assert deadhand_at(pair, "A") and deadhand_at(pair, "B"), "no living %s in a world list (the rush's event placed " \
            "no boss: hs-deadhand-grab-232 run 1 VOID)" % DEADHAND
    not_owned_yet(pair, second, num)                # (after set-up: a NO-REF inside it is a SETUP-FAIL)
    start = {x: pair.log[x].mark() for x in ("A", "B")}
    place(pair, first, BY_FIRST)
    place(pair, second, BY_SECOND)
    pair.step_both(60)
    try:
        pair.until("%s held by a hand" % first, lambda: pair.log[first].find(GRAB_ON, pos=start[first]),
                   max_frames=GRAB_FIRST)
        shots(pair, "%s-grab-%s" % (tag, first))
        pair.step_both(180)
        shots(pair, "%s-grab-%s-3s" % (tag, first))
        pair.until("%s let go" % first, lambda: pair.log[first].find(GRAB_OFF, pos=start[first]), max_frames=GRAB_FREE)
    except AssertionError as e:
        pair.notes.append("%s: %s" % (first, e))  # (not the row: the second's grab is)
    place(pair, first, OUT_OF_REACH)
    free_at = pair.log[second].mark()
    try:
        pair.until("%s held by a hand" % second, lambda: (pair.log[second].find(GRAB_ON, pos=start[second])),
                   max_frames=GRAB_SECOND)
    except AssertionError:
        pair.unproven("no hand took %s (hs-deadhand-grab-232 VOID: no 'grab:' line in %s)" % (second, second))
    if pair.log[first].find(GRAB_ON, pos=free_at) and not pair.log[first].find(GRAB_OFF, pos=free_at):
        pair.unproven("both held at once: he stays with his owner (the header's known gap, review-01111-b)")
    d = handed_to(pair, second, num)
    pair.notes.append("Dead Hand at A %s / B %s, owner %s" % (d["A"]["pos"], d["B"]["pos"], num[second]))
    shots(pair, "%s-grab-%s-3s" % (tag, second))  # held, Dead Hand risen and at the held Link on both screens
    pair.step_both(180)
    shots(pair, "%s-grab-%s-6s" % (tag, second))


@pytest.mark.budget(900)   # ESTIMATED: ~15'200 game frames (60 + GRAB_FIRST + 180 + GRAB_FREE + GRAB_SECOND + HANDOVER + 180) + shots
@pytest.mark.look
@pytest.mark.needs_boot(DH_BOOT)
@pytest.mark.needs_env(*BOSS_ENV)
@pytest.mark.row("#232 Dead Hand's hand holds B: he goes to B's game and rises there, on both screens (hs-deadhand-grab-232)")
def test_deadhand_grab(pair):
    grab_run(pair, "A", "B", "deadhand")


@pytest.mark.budget(900)   # ESTIMATED: ~7'800 game frames (60 + GRAB_SECOND + HANDOVER + 180) + shots
@pytest.mark.look
@pytest.mark.needs_boot(DH_BOOT_B)
@pytest.mark.needs_env(*BOSS_ENV)
@pytest.mark.row("#232 the other way: B held by the first hand while A owns him (hs-deadhand-grab-232b)")
def test_deadhand_grab_other_way(pair):
    """232b: A by the order-2 hand (the one he chased in 232, likely his owner), B 1.2 m from the first hand; B's grab
    may come at once: only B's grab is the row, A's is not awaited (hs-deadhand-grab-232b.A.txt: 'A stays')."""
    with pair.setup():
        in_stage(pair, DH_STAGE)
        num = {x: me(pair, x) for x in ("A", "B")}
        assert deadhand_at(pair, "A") and deadhand_at(pair, "B"), "no living %s in a world list" % DEADHAND
    not_owned_yet(pair, "B", num)                # (after set-up: a NO-REF inside it is a SETUP-FAIL)
    start = pair.log["B"].mark()
    place(pair, "A", BY_SECOND)                   # hs-deadhand-grab-232b.A.txt 'place 0.0 92.4 -99.6'
    place(pair, "B", BY_FIRST)                    # hs-deadhand-grab-232b.B.txt 'place 0.0 92.4 -107.2'
    pair.step_both(60)
    try:
        pair.until("B held by a hand", lambda: pair.log["B"].find(GRAB_ON, pos=start), max_frames=GRAB_SECOND)
    except AssertionError:
        pair.unproven("no hand took B (hs-deadhand-grab-232b VOID: rerun 1 at 2.4 m was never grabbed)")
    if pair.log["A"].find(GRAB_ON, pos=start) and not pair.log["A"].find(GRAB_OFF, pos=start):
        pair.unproven("A held too: two held keep him with his owner (the known gap, review-01111-b)")
    d = handed_to(pair, "B", num)
    pair.notes.append("Dead Hand at A %s / B %s, owner %s" % (d["A"]["pos"], d["B"]["pos"], num["B"]))
    shots(pair, "deadhand-b-grab-B-3s")
    pair.step_both(180)
    shots(pair, "deadhand-b-grab-B-6s")
