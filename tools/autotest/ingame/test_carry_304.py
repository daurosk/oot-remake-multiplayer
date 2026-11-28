"""Rows 304-306 (carry-sync-304, B #152 0.12.1): what one player carries is seen in that player's hands on the other
game. Probe first (worker-rules "root cause PROVEN in game first"): each test lifts ONE thing with A and records, every
SAMPLE_EVERY frames, where A's own object is and where B's copy of it is, against A's Link; then A walks with it.

| Test | Proves |
|---|---|
| test_carry_cucco | (306) a cucco A carries: B's copy stays in A's hands (not on the ground, not falling through A) |
| test_carry_rock | (305) a liftable rock A carries: B's copy is lifted with A (not left at its rest place) |
| test_carry_ruto | (304) child Ruto A carries in Jabu-Jabu: B has her, in A's hands (each game had its own Ruto) |

No guessed number decides a SETUP-FAIL or a FAIL (review-rows-c130-11 MUST 1). 'A carries it' is judged without
the hands box: A's own object (the same id) rose above its rest height or left its rest place (`carried`). While A
carries it, B's copy missing, or still at its rest place, FAILS (the row's bug). A copy further than MISS_M (no source)
from A's Link is a NO-REF miss, and a copy in the hands box is NO-REF too, until MEASURED cites a probe run (the box
against real samples); never PASS before that. The notes keep every sample for the measurement.

Sources (worker-rules "no guessed values"):
- The lane's boot map is KakarikoVillage_Child (test_boss_respawn.BACK_TO); its catch cucco: tools/playtest/data/
  kakariko-village-child.json "catch-cucco-near-gate" (-96.5 97.4 51.3, "placed 0.8 m from it, A lifted it (the prompt
  then said Have)"). Name prefix "Animal_Cucco" (docs/objects.md "The carry claim": Animal_Cucco, _SuperCucco,
  _FlockCucco).
- The rock: tools/autotest/scenarios/rock.A.txt (Graveyard, Obj_LiftRock_A_03 at x -34.12 z 14.94; Link at -34.35 14.2
  lifted it with 'press A 12' after 'wait 20'); the scene and entrance: coop_start_points.inc {"Graveyard_Child",
  "GraveYard_KakarikoSide", -36.50, 98.01, 8.48}.
- Ruto: Npc_Zora_Ruto_Child_001/_002 (docs/objects.md "Followers", InsideJabuJabuBelly); the scene and entrance:
  coop_start_points.inc {"InsideJabuJabuBelly", "InsideJabuJabuBelly_FromZoraFountain", -0.16, 91.67, -0.78}. Her place:
  romfs Banc/InsideJabuJabuBelly/InsideJabuJabuBelly_Static.bcett.byml.zs (read with tools/puppet_pack byml + zs), the
  placement Npc_Zora_Ruto_Child_001 Name 'Step1', Dynamic.DefaultDungeonRoomIndex 2, no Presence flag, Translate
  (1.63, 91.25, -77.98), its Reference link AnchorAction1 at (2.61, 90.98, -73.41) (RUTO_AT, RUTO_STAND). (_002 is
  Room 15's, Presence Get_StoneOfWater_IsAfter_Step13_01_Exp: after the stone.) Probe carry-304-182cd53b-3: she is not
  in A's world list at the entrance, 77 m from her room, so both Links are placed at RUTO_STAND first and A's world
  list is read every RUTO_POLL frames for up to RUTO_LOAD_FRAMES until she is there (SETUP-FAIL naming the npcs seen).
- The world list: docs/playtest.md "world": a dump lists every actor the objects walk sees (<= 200) with id, name, kind,
  pos [x, y, z] (y = height); the dump's state.pos is our own Link.
- Lift: 'place X Y Z' then 'face X Z' (coop_testinput.cpp PLACE/FACE; test_enemies.py), then A held LIFT_FRAMES (rock.A.txt 'press A 12';
  'wait 20' before it), then HOLD_WAIT frames (rock.A.txt 'wait 50' before the throw).
- Walk with it: the bridge 'move' (yaw, frames; docs/playtest.md), WALK_FRAMES.
"""
import json
import math
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pair as P                   # noqa: E402
import test_boss_respawn as BR     # noqa: E402  (start_check, OBJECTS; never edited here)
from test_boss_respawn import back_after   # noqa: E402,F401  (fixture: back to the boot map after a warp)

CUCCO_SPOT = (-96.5, 97.4, 51.3)              # kakariko-village-child.json catch-cucco-near-gate
CUCCO = "Animal_Cucco"
ROCK_SCENE, ROCK_ENTRANCE = "Graveyard_Child", "GraveYard_KakarikoSide"
ROCK_AT = (-34.12, 14.94)                     # rock.A.txt (x, z)
ROCK = "Obj_LiftRock_"
RUTO_SCENE, RUTO_ENTRANCE = "InsideJabuJabuBelly", "InsideJabuJabuBelly_FromZoraFountain"
RUTO = "Npc_Zora_Ruto_Child"
RUTO_AT = (1.63, 91.25, -77.98)               # Banc Jabu _Static: Npc_Zora_Ruto_Child_001 'Step1', room 2 (header)
RUTO_STAND = (2.61, 90.98, -73.41)            # the same placement's AnchorAction1 link (header)
RUTO_POLL, RUTO_LOAD_FRAMES = 30, 300         # read A's world list every 30 frames, at most 300 (5 s at 60 fps)
NEAR_M = 0.8                                  # A placed this far from the object (catch-cucco-near-gate)
B_FROM_M = 4.0                                # B stands this far from A, looking at A (in view, not in reach)
LIFT_SETTLE, LIFT_FRAMES, HOLD_WAIT = 20, 12, 50   # rock.A.txt
WALK_FRAMES = 60
SAMPLE_EVERY = 6
FIND_M = 30.0                                 # the object A lifts: the nearest of its name within this of the spot
# The hands box (NOT measured: MEASURED below): the carried object over its carrier's Link.
HANDS_XZ, HANDS_DY = (1.2, (0.2, 2.6))
MISS_M = 2.5                                  # a copy further than this from A's Link (xz) is not in A's hands
AWAY_M = 1.0                                  # A's own object this far (xz) from its rest place: it left it
RISE_M = 0.1                                  # A's own object this much over its rest height: it rose (a resting
#                                               object keeps its height; not the hands box, which is far larger)
MISS_SHARE = 0.5                              # FAIL when more than this share of the samples miss
MEASURED = None


def xz(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def in_hands(obj, link):
    return obj is not None and link is not None and xz(obj, link) <= HANDS_XZ and \
        HANDS_DY[0] <= obj[1] - link[1] <= HANDS_DY[1]


def carried(obj, rest):
    """A's own object is off its rest place: risen over its rest height, or moved AWAY_M from it (no hands box)."""
    return obj is not None and (obj[1] - rest[1] > RISE_M or xz(obj, rest) > AWAY_M)


def dump(pair, x, tag):
    r = pair.call(x, "dump", tag=tag)
    with open(r["path"], encoding="utf-8") as f:
        d = json.load(f)
    st = d.get("state") or {}
    return st.get("pos"), (st.get("world") or {}).get("actors") or [], st


def nearest(actors, prefix, at, within=FIND_M):
    best = None
    for a in actors:
        if str(a.get("name", "")).startswith(prefix) and a.get("pos"):
            d = math.hypot(a["pos"][0] - at[0], a["pos"][-1] - at[-1])
            if d <= within and (best is None or d < best[0]):
                best = (d, a)
    return best[1] if best else None


def by_id(actors, aid):
    for a in actors:
        if a.get("id") == aid:
            return a
    return None


def lift(pair, prefix, at, tag, within=FIND_M):
    """A placed NEAR_M from the nearest `prefix` actor to `at` (A's world list), facing it; B placed B_FROM_M behind A
    looking at A; A presses A. SETUP-FAIL unless A's own object then left its rest place (`carried`). Returns
    (A's object id, B's copy id or None, the object's rest place)."""
    link, actors, _ = dump(pair, "A", "%s-find" % tag)
    obj = nearest(actors, prefix, at, within)
    if obj is None:
        seen = sorted({str(a.get("name")) for a in actors if a.get("kind") in ("npc", "other", "enemy")})[:40]
        raise P.SetupFail("no %s within %.0f m of %s in A's world list (saw: %s)" % (prefix, within, at, seen))
    o = obj["pos"]
    dx, dz = (link[0] - o[0], link[2] - o[2]) if link else (1.0, 0.0)
    n = math.hypot(dx, dz) or 1.0
    a_at = (o[0] + NEAR_M * dx / n, o[1], o[2] + NEAR_M * dz / n)
    b_at = (o[0] + (NEAR_M + B_FROM_M) * dx / n, o[1], o[2] + (NEAR_M + B_FROM_M) * dz / n)
    for x, spot, face in (("A", a_at, o), ("B", b_at, a_at)):
        pair.log[x].mark()
        pair.act(x, "place %.2f %.2f %.2f" % (spot[0], spot[1], spot[2]))  # coop_testinput.cpp PLACE: X Y Z only
        pair.step_both(2)
        pair.expect_log(x, r"coop: test place .*: done", max_frames=30)
        pair.log[x].mark()
        pair.act(x, "face %.2f %.2f" % (face[0], face[2]))  # coop_testinput.cpp FACE: X Z
        pair.step_both(2)
        pair.expect_log(x, r"coop: test face .*: Link at .* turned to yaw", max_frames=30)  # coop_boss_game.cpp:152
    pair.step_both(LIFT_SETTLE)
    _, b_actors, _ = dump(pair, "B", "%s-b-before" % tag)
    copy = nearest(b_actors, prefix, o, within=1.5)
    pair.notes.append("%s: A's %s id %s at %s; B's copy %s" % (
        tag, obj.get("name"), obj.get("id"), o, ("id %s at %s" % (copy.get("id"), copy.get("pos"))) if copy else "NONE"))
    pair.press("A", ["A"], frames=LIFT_FRAMES)
    pair.step_both(HOLD_WAIT)
    link, actors, st = dump(pair, "A", "%s-a-held" % tag)
    mine = by_id(actors, obj.get("id"))
    pair.notes.append("%s: A held: A's own at %s (rest %s), A's Link %s, prompt %r" % (
        tag, mine and mine.get("pos"), o, link, st.get("prompt")))
    if not carried(mine and mine.get("pos"), o):
        raise P.SetupFail("A did not lift the %s: its place %s (rest %s), A's Link %s, prompt %r" % (
            obj.get("name"), mine and mine.get("pos"), o, link, st.get("prompt")))
    return obj.get("id"), copy.get("id") if copy else None, o


def to_ruto(pair):
    """Both Links placed at RUTO_STAND (her room, header); A's world list read every RUTO_POLL frames until she is in it,
    at most RUTO_LOAD_FRAMES. SETUP-FAIL naming the npcs A saw if she never is."""
    for x in ("A", "B"):
        pair.log[x].mark()
        pair.act(x, "place %.2f %.2f %.2f" % RUTO_STAND)   # coop_testinput.cpp PLACE: X Y Z only
        pair.step_both(2)
        pair.expect_log(x, r"coop: test place .*: done", max_frames=30)
    waited, actors = 0, []
    while waited <= RUTO_LOAD_FRAMES:
        _, actors, _ = dump(pair, "A", "ruto-load")
        if nearest(actors, RUTO, RUTO_AT) is not None:
            pair.notes.append("ruto: in A's world list %d frames after the place at %s" % (waited, RUTO_STAND))
            return
        pair.step_both(RUTO_POLL)
        waited += RUTO_POLL
    seen = sorted({str(a.get("name")) for a in actors if a.get("kind") in ("npc", "other", "enemy")})[:40]
    raise P.SetupFail("no %s within %.0f m of her banc place %s %d frames after A was placed in her room at %s "
                      "(saw: %s)" % (RUTO, FIND_M, RUTO_AT, RUTO_LOAD_FRAMES, RUTO_STAND, seen))


def follow(pair, a_id, b_id, prefix, rest, tag):
    """A holds it still, then walks WALK_FRAMES; every SAMPLE_EVERY frames: A's object vs A's Link, B's copy vs A's
    Link (A's own place: the puppet shows it ~100 ms late, inside HANDS_XZ when standing). Returns the rows."""
    rows = []
    shot = False
    for i, walking in enumerate([False] * 5 + [True] * (WALK_FRAMES // SAMPLE_EVERY)):
        a_link, a_actors, _ = dump(pair, "A", "%s-a-%02d" % (tag, i))
        _, b_actors, _ = dump(pair, "B", "%s-b-%02d" % (tag, i))
        mine = by_id(a_actors, a_id)
        copy = by_id(b_actors, b_id) if b_id is not None else None
        if copy is None:              # the copy may be a new actor once carried (a stand-in): the nearest to A's Link
            copy = nearest(b_actors, prefix, a_link, within=MISS_M * 2) if a_link else None
        rows.append({"i": i, "walking": walking, "a_link": a_link, "a_obj": mine and mine.get("pos"),
                     "b_copy": copy and copy.get("pos"), "b_copy_id": copy and copy.get("id"),
                     "b_at_rest": nearest(b_actors, prefix, rest, within=0.5) is not None,
                     "a_away": bool(mine and mine.get("pos") and xz(mine["pos"], rest) > AWAY_M)})
        if i == 2 and not shot:        # user-facing: the carried thing on both screens
            for x in ("A", "B"):
                pair.check_shot(x, "carry-304-%s-held" % tag, tolerance=0.80)
            shot = True
        if walking:
            pair.call("A", "move", yaw=0, strength=0.6, frames=SAMPLE_EVERY)
            pair.call("B", "step", frames=SAMPLE_EVERY)
        else:
            pair.step_both(SAMPLE_EVERY)
    for x in ("A", "B"):
        pair.check_shot(x, "carry-304-%s-walked" % tag, tolerance=0.80)
    return rows


def judge(pair, rows, tag, rest):
    a_ok = [r for r in rows if carried(r["a_obj"], rest)]
    gone = [r for r in a_ok if r["b_copy"] is None]
    far = [r for r in a_ok if r["b_copy"] is not None and xz(r["b_copy"], r["a_link"]) > MISS_M]
    # B still shows it at the rest place after A's own left it: the row's bug itself, in any one sample (the rest place
    # starts NEAR_M from A's Link, inside MISS_M, so the miss share alone let a copy left behind pass; fake 'rest')
    left = [r["i"] for r in a_ok if r["b_at_rest"] and r["a_away"]]
    for r in rows:
        pair.notes.append("%s %02d %s A link %s A obj %s | B copy %s (id %s)%s" % (
            tag, r["i"], "walk" if r["walking"] else "still", r["a_link"], r["a_obj"], r["b_copy"], r["b_copy_id"],
            " B still has one at rest" if r["b_at_rest"] else ""))
    if not a_ok:
        pair.unproven("%s: A's own object back at its rest place in every sample (A dropped it?)" % tag)
    assert not left, "%s: B still shows it at its rest place while A carries it away (samples %s)" % (tag, left)
    assert len(gone) <= MISS_SHARE * len(a_ok), (
        "%s: B has NO copy in %d of %d samples while A carries it" % (tag, len(gone), len(a_ok)))
    if len(gone) + len(far) > MISS_SHARE * len(a_ok):
        pair.unproven("%s: B's copy > %.1f m from A's Link (MISS_M, not measured) in %d of %d samples, absent in %d" % (
            tag, MISS_M, len(far), len(a_ok), len(gone)))
    hands = [r for r in a_ok if in_hands(r["b_copy"], r["a_link"])]
    if MEASURED is None:
        pair.unproven("%s: hands box not measured yet: B's copy in A's hands %d of %d samples" % (
            tag, len(hands), len(a_ok)))
    assert len(hands) == len(a_ok), "%s: B's copy in A's hands only %d of %d samples" % (tag, len(hands), len(a_ok))


@pytest.mark.budget(int(2 * P.SETUP_LIMIT_S + 240))
@pytest.mark.look
@pytest.mark.needs_env(BR.OBJECTS)
@pytest.mark.row("#306 a cucco another player carries is in their hands on our screen")
def test_carry_cucco(pair):
    with pair.setup():
        BR.start_check(pair)
        a_id, b_id, rest = lift(pair, CUCCO, CUCCO_SPOT, "cucco")
    judge(pair, follow(pair, a_id, b_id, CUCCO, rest, "cucco"), "cucco", rest)


@pytest.mark.budget(int(2 * P.SETUP_LIMIT_S + 2 * P.SETUP_WARP_S + 300))
@pytest.mark.look
@pytest.mark.needs_env(BR.OBJECTS)
@pytest.mark.row("#305 a rock another player carries is lifted with them on our screen")
def test_carry_rock(pair, back_after):
    with pair.setup():
        BR.start_check(pair)
        pair.warp_both(ROCK_SCENE, ROCK_ENTRANCE)
        pair.until_in_play("both in %s" % ROCK_SCENE)
        a_id, b_id, rest = lift(pair, ROCK, (ROCK_AT[0], 0.0, ROCK_AT[1]), "rock")
    judge(pair, follow(pair, a_id, b_id, ROCK, rest, "rock"), "rock", rest)


@pytest.mark.budget(int(2 * P.SETUP_LIMIT_S + 2 * P.SETUP_WARP_S + 300))
@pytest.mark.look
@pytest.mark.needs_env(BR.OBJECTS)
@pytest.mark.row("#304 child Ruto another player carries in Jabu-Jabu is there, in their hands, on our screen")
def test_carry_ruto(pair, back_after):
    with pair.setup():
        BR.start_check(pair)
        pair.warp_both(RUTO_SCENE, RUTO_ENTRANCE)
        pair.until_in_play("both in %s" % RUTO_SCENE)
        to_ruto(pair)
        a_id, b_id, rest = lift(pair, RUTO, RUTO_AT, "ruto")
    judge(pair, follow(pair, a_id, b_id, RUTO, rest, "ruto"), "ruto", rest)
