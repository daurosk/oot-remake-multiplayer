"""Row 305 (carry-boulder-305, B #153 0.12.1): a LARGE boulder one player carries is seen in that player's hands on the
other game. Probe first (worker-rules "root cause PROVEN in game first"): adult A lifts one Obj_LiftRock_B_ (the big
rocks) and walks with it; every SAMPLE_EVERY frames the test records where A's own boulder is and where B's copy of it
is, against A's Link, plus the world sync's own lines on both games (coop_world.cpp WorldSync::Move: the carrier's
'moved here: sending' / 'at rest: place sent', the other's 'moved by another player' / 'put where another player
left it').

| Test | Proves |
|---|---|
| test_carry_boulder | (305) a big boulder A carries: B's copy is lifted with A (not left at its rest place, not missing) |

The judging is test_carry_304's (carry-sync-304, its owner; imported, never edited here): NO-REF until the hands box is
measured, FAIL when B's copy stays at rest or is missing while A carries it.

Sources (worker-rules "no guessed values"):
- The big rocks: Obj_LiftRock_B_01, 17 placements in GoronCity (lichii/hook/coop_dungeon.cpp ~1678-1694, the row-220
  reset table); the world sync shares every Obj_LiftRock_ (coop_world.cpp kBreakables "rocks Link lifts and throws").
- Adult Link: 'Sequence_IsChildhood 0', 30 frames, then the warp (test_dbno_crawl.to_place, hs-dbno-adult A/B.txt:1-2;
  docs/playtest.md `warp`). The places tried, in order (PLACES), are adult starts of the scenes that have
  Obj_LiftRock_B_01 placements (coop_dungeon.cpp kWorldKeys: GoronCity, GerudoValley, DesertColossus), each from
  coop_start_points.inc (lines 45-52, 25). Run carry-boulder-305-182cd53b (04:23 UTC 11-28) SETUP-FAILed at the first
  one, GoronCity_MountainTrailSide: A's world list held 200 actors (the walk's cap, 185 `other`, farthest 63.6 m) and
  no Obj_LiftRock_B_, so a crowded city may push its big rocks out of the list; the next places are tried in turn.
- Strength: PlayerStatus.LiftGrade, enum None/Low/Middle/High (docs/items.md "Strength, scale"; docs/playtest.md
  `flag` takes an enum value's name). High = the most Link can lift, so the grade is never why a lift fails.
- The boulder's place is NOT cited: at each place the test finds the nearest Obj_LiftRock_B_ in A's world list (dump,
  <= 200 actors, docs/playtest.md "world") through the shared pair.find_actor, whose miss says CAP HIT when the list
  is full (B #167); each place's findings (count, cap, big rocks; the miss text) go into the notes, and the SETUP-FAIL
  names every place when none has one.
- Lift: test_carry_304's place/face (coop_testinput.cpp PLACE/FACE), A held LIFT_FRAMES; a big lift's animation length
  is NOT measured, so A's boulder is sampled every LIFT_POLL frames up to LIFT_MAX frames until it is in A's hands
  (SETUP-FAIL with every sample if never).
"""
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pair as P                   # noqa: E402
import test_boss_respawn as BR     # noqa: E402  (start_check, OBJECTS; never edited here)
import test_carry_304 as C         # noqa: E402  (dump/nearest/by_id/in_hands/follow/judge; carry-sync-304's)
import test_dbno_crawl as DC       # noqa: E402  (to_place; never edited here)
from test_boss_respawn import back_after   # noqa: E402,F401  (fixture: back to the boot map after a warp)

PLACES = (                                    # (scene, entrance): coop_start_points.inc, adult starts
    ("GoronCity_Adult", "GoronCity_MountainTrailSide"),        # :50
    ("GoronCity_Adult", "GoronCity_CraterSide"),               # :49 (the city's lowest floor)
    ("GoronCity_Adult", "GoronCity_LostWoodSide_after"),       # :51
    ("GerudoValley_Adult", "GerudoValley_Entrance"),           # :46
    ("GerudoValley_Adult", "GerudoValley_FromGerudoFortress"), # :45
    ("DesertColossus_Adult", "DesertColossus_FromSpiritTempleRightHand"),  # :25
)
BOULDER = "Obj_LiftRock_B_"
FIND_M = 120.0                                # nearest big rock to A's start within this (the whole city)
LIFT_POLL, LIFT_MAX = 10, 240
WORLD_LINE = r"world: [0-9a-f]{16} Obj_LiftRock_B_\S* "


def find_place(pair, tag):
    """Both games (adult) to each PLACES start in turn until A's world list has an Obj_LiftRock_B_ within FIND_M of
    A's Link. Each place's findings go into the notes. Returns (A's Link, the boulder actor)."""
    found = []
    for i, (scene, entrance) in enumerate(PLACES):
        if i == 0:
            DC.to_place(pair, "adult", scene, entrance)
        else:
            pair.warp_both(scene, entrance)
            pair.step_both(30)
            DC.need_at(pair, "adult", scene)
        link, obj, actors = pair.find_actor("A", BOULDER, FIND_M, tag="%s-find-%d" % (tag, i), missing_ok=True)
        big = [a for a in actors if str(a.get("name", "")).startswith(BOULDER) and a.get("pos")]
        line = "%s/%s: A at %s, %d actors%s, %s: %s" % (
            scene, entrance, link, len(actors), " (CAP HIT)" if len(actors) >= P.WALK_CAP else "", BOULDER,
            [(a.get("name"), a.get("id"), a.get("pos")) for a in big][:6] or "none")
        pair.notes.append("%s find: %s" % (tag, line))
        found.append("%s %d%s" % (entrance, len(actors), "cap" if len(actors) >= P.WALK_CAP else ""))
        if obj is not None:
            return link, obj
    raise P.SetupFail("no %s within %.0f m of A's Link at any of %d places (entrance, actors in A's list; each "
                      "place's findings in the notes 'boulder find:'): %s"
                      % (BOULDER, FIND_M, len(PLACES), ", ".join(found)))


def lift_big(pair, tag, link, obj):
    """test_carry_304.lift's set-up (A NEAR_M from the boulder facing it, B B_FROM_M behind A), then A presses A and
    the boulder is sampled until it is in A's hands. Returns (A's id, B's copy id or None, rest place)."""
    o = obj["pos"]
    dx, dz = (link[0] - o[0], link[2] - o[2]) if link else (1.0, 0.0)
    n = (dx * dx + dz * dz) ** 0.5 or 1.0
    a_at = (o[0] + C.NEAR_M * dx / n, o[1], o[2] + C.NEAR_M * dz / n)
    b_at = (o[0] + (C.NEAR_M + C.B_FROM_M) * dx / n, o[1], o[2] + (C.NEAR_M + C.B_FROM_M) * dz / n)
    for x, spot, face in (("A", a_at, o), ("B", b_at, a_at)):
        pair.log[x].mark()
        pair.act(x, "place %.2f %.2f %.2f" % (spot[0], spot[1], spot[2]))  # coop_testinput.cpp PLACE: X Y Z only
        pair.step_both(2)
        pair.expect_log(x, r"coop: test place .*: done", max_frames=30)
        pair.log[x].mark()
        pair.act(x, "face %.2f %.2f" % (face[0], face[2]))  # coop_testinput.cpp FACE: X Z
        pair.step_both(2)
        pair.expect_log(x, r"coop: test face .*: Link at .* turned to yaw", max_frames=30)  # coop_boss_game.cpp:152
    pair.step_both(C.LIFT_SETTLE)
    _, b_actors, _ = C.dump(pair, "B", "%s-b-before" % tag)
    copy = C.nearest(b_actors, BOULDER, o, within=1.5)
    pair.notes.append("%s: A's %s id %s at %s; B's copy %s" % (
        tag, obj.get("name"), obj.get("id"), o, ("id %s at %s" % (copy.get("id"), copy.get("pos"))) if copy else "NONE"))
    _, _, st = C.dump(pair, "A", "%s-a-prompt" % tag)
    pair.notes.append("%s: A's prompt before the lift %r" % (tag, st.get("prompt")))
    pair.press("A", ["A"], frames=C.LIFT_FRAMES)
    seen = []
    for f in range(0, LIFT_MAX, LIFT_POLL):
        pair.step_both(LIFT_POLL)
        link, actors, st = C.dump(pair, "A", "%s-a-lift-%03d" % (tag, f))
        mine = C.by_id(actors, obj.get("id"))
        seen.append((f + LIFT_POLL, mine and mine.get("pos"), link))
        if C.in_hands(mine and mine.get("pos"), link):
            pair.notes.append("%s: in A's hands %d frames after the press" % (tag, f + LIFT_POLL))
            return obj.get("id"), copy.get("id") if copy else None, o
    raise P.SetupFail("A did not lift the %s in %d frames: (frame, its place, A's Link) %s; prompt %r" % (
        obj.get("name"), LIFT_MAX, seen, st.get("prompt")))


def world_lines(pair, x, pos):
    """The world sync's own lines about a big rock in game x since log offset pos (read only)."""
    rx = re.compile(WORLD_LINE)
    return [ln.strip()[-200:] for ln in pair.log[x].since(pos) if rx.search(ln)]


LIFT = "PlayerStatus.LiftGrade"     # docs/items.md "Strength, scale"


@pytest.mark.budget(int(2 * P.SETUP_LIMIT_S + 2 * len(PLACES) * P.SETUP_WARP_S + 360))
@pytest.mark.look
@pytest.mark.needs_env(BR.OBJECTS)
@pytest.mark.row("#305 a big boulder another player carries is lifted with them on our screen")
def test_carry_boulder(pair, back_after):
    grade = {}      # each game's LiftGrade before the test (flag_read's enum hash: `flag` takes it back, docs/playtest.md)
    try:
        with pair.setup():
            BR.start_check(pair)
            for x in ("A", "B"):
                grade[x] = pair.flag_read(x, LIFT).get("value")
                pair.flag(x, LIFT, "High")
            pair.step_both(2)
            link, obj = find_place(pair, "boulder")
            pos = {x: pair.log[x].size() for x in ("A", "B")}
            a_id, b_id, rest = lift_big(pair, "boulder", link, obj)
        try:
            rows = C.follow(pair, a_id, b_id, BOULDER, rest, "boulder")
        finally:
            for x in ("A", "B"):
                lines = world_lines(pair, x, pos[x])
                pair.notes.append("boulder: %s world lines (%d): %s" % (x, len(lines), lines[:12]))
        C.judge(pair, rows, "boulder")
    except P.NoRef:
        _restore_grade(pair, grade, failed=False)   # (the probe's normal end: a failed restore is the result)
        raise
    except BaseException:
        _restore_grade(pair, grade, failed=True)
        raise
    _restore_grade(pair, grade, failed=False)


def _restore_grade(pair, grade, failed):
    """Puts each game's LiftGrade back (preflight review-tests-153-2: a later test in the boot or a saved game must not
    inherit High). After a failure a failed restore is only noted (the test's own error stays the result, as
    test_dbno_crawl.restored); after a pass or a NO-REF (the probe's normal end) it is the test's failure."""
    for x, was in grade.items():
        if was is None:
            pair.notes.append("boulder: %s LiftGrade was unreadable before the test: not restored" % x)
            continue
        try:
            r = pair.flag(x, LIFT, was)
            pair.notes.append("boulder: %s LiftGrade back to %s" % (x, r.get("value")))
        except Exception as e:
            if not failed:
                raise
            pair.notes.append("boulder: %s LiftGrade restore after the failure failed: %s: %s" % (x, type(e).__name__, e))
