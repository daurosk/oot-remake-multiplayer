"""Row 308 (owner WANT, B #152 20:36 UTC 2026-11-27): a player picks up ANOTHER player's bomb and puts it away: the bomb
leaves the world on every game, the picker's bomb count goes up by 1, the thrower's count is unchanged by it.

| test | what |
|---|---|
| test_bomb_pocket | B takes a bomb out and puts it down; A lifts B's bomb and puts it away: A's count +1, B's count only B's own -1, no Item_Bomb left near A on either game, shots on both while A holds it and after |

PROBE first (root cause unknown in a game): docs/native-puppets.md "Phase 2 status": the hand-over (BOMB_TAKE) and the
stash ('a taken unlit bomb gone as PouchData.Item_Bomb.StockNum rose by one is removed as picked up') are wired behind
LICHII_COOP_BOMB_SHARE=1 and never ran in a game. UNKNOWN there and here, so this test MEASURES them (notes):
(1) that B's 'press A' while holding puts the bomb down (docs/testing.md 'A actions'), (2) that A's 'press B' while
holding puts it away (docs/testing.md bindings 'B put away'; docs/native-puppets.md: 'whether the game lets Link put a
held unlit bomb back (a button)'), (3) that the fuse leaves time for it (the frames below are the shortest the bridge
gives). Each one is a SETUP-FAIL / NO-REF naming what the state showed, never a guess turned PASS or FAIL
(review-rows-c130-11 MUST 2): while A still has the bomb after 'press B', or it is gone with A's count unchanged (it
went off, or 'press B' is not put-away), the test ends NO-REF; it FAILS only when the bomb left A's hands into the
pocket (A's count +1) and the world or B's count is wrong (the row's bug). No hands box decides anything here
(MUST 1): B's bomb out = an Item_Bomb in B's world list that was not there before ZR; lying = not over B's Link by
more than test_carry_304.RISE_M; A lifted it = none left at its lying place (REST_M) and one within MISS_M of A's Link.

Sources:
- Bombs on ZR: 'give bombs' + 'assign Bomb' + 240 frames (test_boss_hit_react.arm, hs-boss-hit-react-dodongo); ZR uses
  the item (docs/testing.md bindings). Lift grade: a bomb is lifted only at PlayerStatus.LiftGrade Low or more
  (docs/items.md 'Bombs from the cheat', enum None/Low/Middle/High, items.md:22).
- The count: PouchData.Item_Bomb.StockNum (docs/items.md table; coop_bomb_share.h stash), read by the bridge flag_read.
- The switch line: coop_spawn.cpp:2381 'coop: bombs: LICHII_COOP_BOMB_SHARE=1: a lifted bomb changes hands'.
- World list, place/face/lift and the hands box: test_carry_304 (its sources).
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pair as P                   # noqa: E402
import test_boss_respawn as BR     # noqa: E402  (start_check, OBJECTS)
import test_carry_304 as C         # noqa: E402  (dump, nearest, in_hands, xz, NEAR_M; never edited here)

SHARE = "LICHII_COOP_BOMB_SHARE=1"
SHARE_LINE = r"coop: bombs: LICHII_COOP_BOMB_SHARE=1: a lifted bomb changes hands"   # coop_spawn.cpp:2381
COUNT = "PouchData.Item_Bomb.StockNum"
BOMB = "Item_Bomb"
GIVE_FRAMES = 240                  # test_boss_hit_react.GIVE_FRAMES
PRESS, SETTLE = 8, 20              # the bridge's press (Pair.press frames=8); frames after each step
APART_M = 3.0                      # A stands this far from B at the start (out of reach of B's bomb)
GONE_M = 3.0                       # no Item_Bomb within this of A's Link once put away
REST_M = 0.5                       # an Item_Bomb this near the lying place is still there (test_carry_304.follow's b_at_rest)


def count(pair, x):
    r = pair.flag_read(x, COUNT)
    return r.get("value")


def bombs(pair, x, tag):
    link, actors, st = C.dump(pair, x, tag)
    return link, [a for a in actors if str(a.get("name", "")) == BOMB and a.get("pos")], st


@pytest.mark.budget(int(2 * P.SETUP_LIMIT_S + 240))
@pytest.mark.look
@pytest.mark.needs_env(BR.OBJECTS, SHARE)
@pytest.mark.row("#308 another player's bomb picked up and put away: +1 to the picker's count, gone on every game")
def test_bomb_pocket(pair):
    with pair.setup():
        BR.start_check(pair)
        for x in ("A", "B"):
            if not pair.log[x].find(SHARE_LINE, 0):
                raise P.SetupFail("%s: no '%s' line: the bomb share is off in this boot" % (x, SHARE_LINE))
            r = pair.give(x, "bombs")
            if "failed" in str(r.get("result", "")):
                raise P.SetupFail("%s: give bombs: %s" % (x, r))
            pair.act(x, "assign Bomb")
            pair.flag(x, "PlayerStatus.LiftGrade", "Low")
        pair.step_both(GIVE_FRAMES)
        n0 = {x: count(pair, x) for x in ("A", "B")}
        if not all(isinstance(n0[x], int) and n0[x] > 0 for x in n0):
            raise P.SetupFail("no bombs after give: %s %s" % (COUNT, n0))
        b_link, _, _ = C.dump(pair, "B", "bomb-b-start")
        a_at = (b_link[0] + APART_M, b_link[1], b_link[2])
        pair.act("A", "place %.2f %.2f %.2f" % a_at)
        pair.step_both(2)
        # B takes a bomb out (ZR) and puts it down (A, standing): MEASURED (UNKNOWN 1)
        _, before, _ = bombs(pair, "B", "bomb-b-before")
        pair.press("B", ["ZR"], frames=PRESS)
        pair.step_both(SETTLE)
        b_link, held, _ = bombs(pair, "B", "bomb-b-out")
        new = [b for b in held if b.get("id") not in {o.get("id") for o in before}]
        pair.notes.append("B out: B's new Item_Bombs %s, B's Link %s" % ([b["pos"] for b in new], b_link))
        if not new:
            raise P.SetupFail("B took no bomb out: no new Item_Bomb in B's world list (B's %s, before %s), B's Link %s, "
                              "count %s" % ([b["pos"] for b in held], [b["pos"] for b in before], b_link,
                                            count(pair, "B")))
        pair.press("B", ["A"], frames=PRESS)
        pair.step_both(SETTLE)
        b_link, down, _ = bombs(pair, "B", "bomb-b-down")
        pair.notes.append("B put down: B's Item_Bombs %s, B's Link %s" % ([b["pos"] for b in down], b_link))
        a_link, a_bombs, _ = bombs(pair, "A", "bomb-a-find")
        mine = C.nearest(a_bombs, BOMB, b_link, within=4.0)
        if mine is None or mine["pos"][1] - b_link[1] > C.RISE_M:
            raise P.SetupFail("B's bomb not lying near B on A's game: A's Item_Bombs %s (B's Link %s)" % (
                [b["pos"] for b in a_bombs], b_link))
        o = mine["pos"]
        dx, dz = a_link[0] - o[0], a_link[2] - o[2]
        n = (dx * dx + dz * dz) ** 0.5 or 1.0
        pair.act("A", "place %.2f %.2f %.2f" % (o[0] + C.NEAR_M * dx / n, o[1], o[2] + C.NEAR_M * dz / n))
        pair.step_both(2)
        pair.act("A", "face %.2f %.2f" % (o[0], o[2]))
        pair.step_both(2)
        n1 = {x: count(pair, x) for x in ("A", "B")}
    # A lifts B's bomb (the row: another player's bomb)
    # the cited lift (test_carry_304 LIFT_SETTLE + LIFT_FRAMES = rock.A.txt 'wait 20', 'press A 12'); probe
    # carry-304-182cd53b-3's 8-frame press left the bomb lying (UNKNOWN (a) press too short vs (b) not liftable on A)
    pair.step_both(C.LIFT_SETTLE)
    _, _, st0 = bombs(pair, "A", "bomb-a-at")
    pair.press("A", ["A"], frames=C.LIFT_FRAMES)
    pair.step_both(SETTLE)
    a_link, a_bombs, st = bombs(pair, "A", "bomb-a-held")
    _, b_bombs, _ = bombs(pair, "B", "bomb-b-sees")
    pair.notes.append("A lift: A's prompt at the bomb %r, after %r; A's Item_Bombs %s, A's Link %s; B's %s" % (
        st0.get("prompt"), st.get("prompt"), [b["pos"] for b in a_bombs], a_link, [b["pos"] for b in b_bombs]))
    still = [b["pos"] for b in a_bombs if C.xz(b["pos"], o) <= REST_M]
    with_a = [b["pos"] for b in a_bombs if C.xz(b["pos"], a_link) <= C.MISS_M]
    assert with_a and not still, "A did not lift B's bomb: A's Item_Bombs %s (lying place %s, A's Link %s)" % (
        [b["pos"] for b in a_bombs], o, a_link)
    for x in ("A", "B"):
        pair.check_shot(x, "bomb-308-held", tolerance=0.80)
    # A puts it away (B: UNKNOWN 2)
    pair.press("A", ["B"], frames=PRESS)
    pair.step_both(SETTLE)
    n2 = {x: count(pair, x) for x in ("A", "B")}
    near = {}
    for x in ("A", "B"):
        _, bs, _ = bombs(pair, x, "bomb-%s-after" % x.lower())
        near[x] = [b["pos"] for b in bs if C.xz(b["pos"], a_link) <= GONE_M]
    pair.notes.append("counts: start %s, before A's lift %s, after A's B %s; Item_Bombs near A: %s" % (n0, n1, n2, near))
    for x in ("A", "B"):
        pair.check_shot(x, "bomb-308-away", tolerance=0.80)
    if n2["A"] != n1["A"] + 1:
        _, a_now, _ = bombs(pair, "A", "bomb-a-unknown")
        held = [b["pos"] for b in a_now if C.xz(b["pos"], a_link) <= C.MISS_M]
        if held:
            pair.unproven("A still has the bomb after 'press B' (UNKNOWN 2: put-away input): A's Item_Bombs near A's Link "
                          "%s, A's count %s -> %s" % (held, n1["A"], n2["A"]))
        if not a_now:
            pair.unproven("the bomb is gone with A's count %s -> %s (UNKNOWN 2/3: it went off, or 'press B' is not "
                          "put-away)" % (n1["A"], n2["A"]))
        pair.unproven("the bomb left A's hands but not into A's pocket (UNKNOWN 2: 'press B' put it down?): A's Item_Bombs "
                      "%s, A's count %s -> %s" % ([b["pos"] for b in a_now], n1["A"], n2["A"]))
    assert n2["B"] == n1["B"], "B's count changed by A's pocket: %s -> %s" % (n1["B"], n2["B"])
    assert n1["B"] == n0["B"] - 1, "B's own take-out: %s -> %s (not -1)" % (n0["B"], n1["B"])
    assert not near["A"] and not near["B"], "a bomb is still in the world after A put it away: %s" % near
