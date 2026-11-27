"""Rows 304-306 (carry-sync-304, B #152 0.12.1): what one player carries is seen in that player's hands on the other
game. Probe first (worker-rules "root cause PROVEN in game first"): each test lifts ONE thing with A and records, every
SAMPLE_EVERY frames, where A's own object is and where B's copy of it is, against A's Link; then A walks with it.

| Test | Proves |
|---|---|
| test_carry_cucco | (306) a cucco A carries: B's copy stays in A's hands (not on the ground, not falling through A) |
| test_carry_rock | (305) a liftable rock A carries: B's copy is lifted with A (not left at its rest place) |
| test_carry_ruto | (304) child Ruto A carries in Jabu-Jabu: B has her, in A's hands (each game had its own Ruto) |

Until MEASURED cites a probe run (the hands box against real samples), a run whose copy IS in the hands ends
pair.unproven (NO-REF, never PASS); a copy that is clearly NOT there (absent, or > MISS_M from A's Link in most samples
while A's own object is in A's hands) FAILS: that is the row's bug.

Sources (worker-rules "no guessed values"):
- The lane's boot map is KakarikoVillage_Child (test_boss_respawn.BACK_TO); its catch cucco: tools/playtest/data/
  kakariko-village-child.json "catch-cucco-near-gate" (-96.5 97.4 51.3, "placed 0.8 m from it, A lifted it (the prompt
  then said Have)"). Name prefix "Animal_Cucco" (docs/objects.md "The carry claim": Animal_Cucco, _SuperCucco,
  _FlockCucco).
- The rock: tools/autotest/scenarios/rock.A.txt (Graveyard, Obj_LiftRock_A_03 at x -34.12 z 14.94; Link at -34.35 14.2
  lifted it with 'press A 12' after 'wait 20'); the scene and entrance: coop_start_points.inc {"Graveyard_Child",
  "GraveYard_KakarikoSide", -36.50, 98.01, 8.48}.
- Ruto: Npc_Zora_Ruto_Child_001/_002 (docs/objects.md "Followers", InsideJabuJabuBelly); the scene and entrance:
  coop_start_points.inc {"InsideJabuJabuBelly", "InsideJabuJabuBelly_FromZoraFountain", -0.16, 91.67, -0.78}. Her place
  is NOT cited anywhere: the test finds her in A's world list (dump) and SETUP-FAILs naming the npcs it saw if she is not
  there (that answers where she is for the next run).
- The world list: docs/playtest.md "world": a dump lists every actor the objects walk sees (<= 200) with id, name, kind,
  pos [x, y, z] (y = height); the dump's state.pos is our own Link.
- Lift: 'place x y z face_x face_z' (docs/playtest.md act place), then A held LIFT_FRAMES (rock.A.txt 'press A 12';
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
NEAR_M = 0.8                                  # A placed this far from the object (catch-cucco-near-gate)
B_FROM_M = 4.0                                # B stands this far from A, looking at A (in view, not in reach)
LIFT_SETTLE, LIFT_FRAMES, HOLD_WAIT = 20, 12, 50   # rock.A.txt
WALK_FRAMES = 60
SAMPLE_EVERY = 6
FIND_M = 30.0                                 # the object A lifts: the nearest of its name within this of the spot
# The hands box (NOT measured: MEASURED below): the carried object over its carrier's Link.
HANDS_XZ, HANDS_DY = (1.2, (0.2, 2.6))
MISS_M = 2.5                                  # a copy further than this from A's Link (xz) is not in A's hands
MISS_SHARE = 0.5                              # FAIL when more than this share of the samples miss
MEASURED = None


def xz(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def in_hands(obj, link):
    return obj is not None and link is not None and xz(obj, link) <= HANDS_XZ and \
        HANDS_DY[0] <= obj[1] - link[1] <= HANDS_DY[1]


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
    looking at A; A presses A. SETUP-FAIL unless A's own object is then in A's hands (A lifted it). Returns
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
        pair.act(x, "place %.2f %.2f %.2f %.2f %.2f" % (spot[0], spot[1], spot[2], face[0], face[2]))
        pair.step_both(2)
        pair.expect_log(x, r"coop: test place .*: done", max_frames=30)
    pair.step_both(LIFT_SETTLE)
    _, b_actors, _ = dump(pair, "B", "%s-b-before" % tag)
    copy = nearest(b_actors, prefix, o, within=1.5)
    pair.notes.append("%s: A's %s id %s at %s; B's copy %s" % (
        tag, obj.get("name"), obj.get("id"), o, ("id %s at %s" % (copy.get("id"), copy.get("pos"))) if copy else "NONE"))
    pair.press("A", ["A"], frames=LIFT_FRAMES)
    pair.step_both(HOLD_WAIT)
    link, actors, st = dump(pair, "A", "%s-a-held" % tag)
    mine = by_id(actors, obj.get("id"))
    if not in_hands(mine and mine.get("pos"), link):
        raise P.SetupFail("A did not lift the %s: its place %s, A's Link %s, prompt %r" % (
            obj.get("name"), mine and mine.get("pos"), link, st.get("prompt")))
    return obj.get("id"), copy.get("id") if copy else None, o


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
                     "b_at_rest": nearest(b_actors, prefix, rest, within=0.5) is not None})
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


def judge(pair, rows, tag):
    a_ok = [r for r in rows if in_hands(r["a_obj"], r["a_link"])]
    miss = [r for r in a_ok if r["b_copy"] is None or xz(r["b_copy"], r["a_link"]) > MISS_M]
    for r in rows:
        pair.notes.append("%s %02d %s A link %s A obj %s | B copy %s (id %s)%s" % (
            tag, r["i"], "walk" if r["walking"] else "still", r["a_link"], r["a_obj"], r["b_copy"], r["b_copy_id"],
            " B still has one at rest" if r["b_at_rest"] else ""))
    if not a_ok:
        pair.unproven("%s: A's own object left A's hands in every sample (A dropped it?)" % tag)
    assert len(miss) <= MISS_SHARE * len(a_ok), (
        "%s: B's copy NOT in A's hands in %d of %d samples while A carries it (absent or > %.1f m from A's Link)" % (
            tag, len(miss), len(a_ok), MISS_M))
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
    judge(pair, follow(pair, a_id, b_id, CUCCO, rest, "cucco"), "cucco")


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
    judge(pair, follow(pair, a_id, b_id, ROCK, rest, "rock"), "rock")


@pytest.mark.budget(int(2 * P.SETUP_LIMIT_S + 2 * P.SETUP_WARP_S + 300))
@pytest.mark.look
@pytest.mark.needs_env(BR.OBJECTS)
@pytest.mark.row("#304 child Ruto another player carries in Jabu-Jabu is there, in their hands, on our screen")
def test_carry_ruto(pair, back_after):
    with pair.setup():
        BR.start_check(pair)
        start = pair.warp_both(RUTO_SCENE, RUTO_ENTRANCE)
        pair.until_in_play("both in %s" % RUTO_SCENE)
        a_id, b_id, rest = lift(pair, RUTO, start["A"]["pos"], "ruto", within=500.0)
    judge(pair, follow(pair, a_id, b_id, RUTO, rest, "ruto"), "ruto")
