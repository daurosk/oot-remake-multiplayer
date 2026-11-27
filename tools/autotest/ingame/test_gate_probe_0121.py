"""gate-ui-epona-0121 probe (B #148, measures only, never PASS for an rc row) on the S 309d7180c lane pair.

1. test_probe_tags_camera: why B's camera stayed on A's side in test_regress_ui::test_name_tags (S 309d7180c gate-ui:
   'B: A's feet (-0.76, -3.53) ... own Link at depth 3.90, A's at 1.19' after 3 ZL). The same set-up (need_child_in_play,
   place_in_front B of A at TAGS_DIST, A's ZL first), then B's state before and after each ZL: pos, facing, camera yaw,
   other_in_view's numbers, plus a B shot after each ZL. Then the same with B turned again by `face` before its ZL.
2. test_probe_epona: (a) is a field Epona in B's world list in HyruleField_Adult, from the old row's entrance
   HyruleField_FrontOfMarket (hs-epona-sync.B.txt:24) and after B is placed beside her spot (hs-epona-sync.B.txt:27),
   versus the test's warp with no entrance (B landed at HyruleField_FrontOfLostWoodBridge, gate-epona game-out-B 4806);
   (b) after B's `flag Get_Epona Complete`, per 30 frames: Get_Epona, OwnedHorseList.ActorName[0] (row 294's owns
   check, coop_ride.cpp:468) and B's 'coop: story: Epona' / 'coop: ride:' lines (gate-epona: no 'Epona is ours').
Every line goes to the notes and stderr; set-up misses only are FAIL.
"""
import math
import sys

import pytest
from test_epona import EPONA, EPONA_FIELD, FIELD, GET_EPONA, ROW_ENV, horse_pos
from test_gear import AGE_FLAG, GATE, SCENE_CHILD
from test_regress_ui import TAGS_DIST, other_in_view
from test_rows import need_child_in_play, place_in_front

HORSE = "OwnedHorseList.ActorName[0]"       # test_epona_294.py:37 (row 294)
MARKET = "HyruleField_FrontOfMarket"        # hs-epona-sync.A/B.txt 'warp HyruleField_Adult HyruleField_FrontOfMarket'


def note(pair, line):
    pair.notes.append(line)
    print("gate-probe-0121: " + line, file=sys.stderr, flush=True)


def look(pair, what, x="B", other="A"):
    s = pair.state(x)
    ok, why = other_in_view(pair, x, other)
    note(pair, "%s %s: pos %s facing %s camera %s event %s | in view %s: %s" % (
        what, x, s.get("pos"), s.get("facing"), s.get("camera"), s.get("event"), ok, why))


@pytest.mark.budget(150)
@pytest.mark.row("probe gate-ui-epona-0121: B's camera after place_in_front + ZL (test_name_tags) - measures only")
def test_probe_tags_camera(pair):
    with pair.setup():
        need_child_in_play(pair)
        place_in_front(pair, "B", of="A", dist=TAGS_DIST)
    look(pair, "placed", "A", "B")
    look(pair, "placed")
    pair.press("A", ["ZL"], frames=8)
    pair.step_both(20)
    look(pair, "after A's ZL", "A", "B")
    look(pair, "after A's ZL")
    for n in range(3):
        pair.press("B", ["ZL"], frames=8)
        pair.step_both(20)
        look(pair, "after B's ZL %d" % (n + 1))
        pair.check_shot("B", "probe0121-tags-zl%d" % (n + 1), tolerance=0.80)
    a = pair.state("A")["pos"]
    pair.act("B", "face %.2f %.2f" % (a[0], a[2]))
    pair.step_both(10)
    look(pair, "B faced A again")
    pair.press("B", ["ZL"], frames=8)
    pair.step_both(20)
    look(pair, "after face + ZL")
    pair.check_shot("B", "probe0121-tags-face-zl", tolerance=0.80)


def epona_actors(pair, x="B"):
    w = pair.c[x].world().get("world") or {}
    acts = w.get("actors", [])
    out = ["%s %s" % (a.get("name"), a.get("pos")) for a in acts if "Epona" in (a.get("name") or "")
           or "Horse" in (a.get("name") or "")]
    return "%d actors listed (%s); horses: %s" % (len(acts), w.get("why") or "ok", out or "none")


def quest_lines(pair, x, pos):
    keep = [ln.strip()[:160] for ln in pair.log[x].since(pos) if "coop: story: Epona" in ln or "coop: ride:" in ln
            or "Get_Epona" in ln]
    return keep[-4:]


@pytest.mark.budget(420)
@pytest.mark.needs_env(*ROW_ENV)
@pytest.mark.row("probe gate-ui-epona-0121: the field Epona from FrontOfMarket; owns after Get_Epona Complete - measures only")
def test_probe_epona(pair):
    with pair.setup():
        need_child_in_play(pair)
        for x in ("A", "B"):
            note(pair, "start %s: Get_Epona %s, %s %r" % (x, pair.flag_read(x, GET_EPONA).get("value"), HORSE,
                                                          pair.flag_read(x, HORSE).get("value")))
        for x in ("A", "B"):
            pair.flag(x, AGE_FLAG, 0)
        pair.step_both(60)
        pair.warp_both(FIELD, entrance=MARKET)
    try:
        s = pair.state("B")
        note(pair, "B in %s age %s at %s (%.1f m from her spot)" % (s.get("scene"), s.get("age"), s.get("pos"),
                                                                   math.dist(s["pos"][::2], EPONA_FIELD[::2])))
        note(pair, "B world from the entrance: " + epona_actors(pair))
        pair.act("B", "place %.2f %.2f %.2f" % (EPONA_FIELD[0] + 1.5, EPONA_FIELD[1], EPONA_FIELD[2]))
        pair.act("B", "face %.2f %.2f" % (EPONA_FIELD[0], EPONA_FIELD[2]))
        for i in range(4):
            pair.step_both(60)
            note(pair, "B world placed +%d: %s; horse_pos %s" % ((i + 1) * 60, epona_actors(pair), horse_pos(pair)))
        pair.check_shot("B", "probe0121-epona-spot", tolerance=0.80)
        pos = pair.log["B"].mark()
        pair.flag("B", GET_EPONA, "Complete")
        for i in range(8):
            pair.step_both(30)
            note(pair, "B +%d after Complete: Get_Epona %s, %s %r, lines %s" % (
                (i + 1) * 30, pair.flag_read("B", GET_EPONA).get("value"), HORSE, pair.flag_read("B", HORSE).get("value"),
                quest_lines(pair, "B", pos)))
        note(pair, "A after B's Complete: Get_Epona %s, %s %r" % (
            pair.flag_read("A", GET_EPONA).get("value"), HORSE, pair.flag_read("A", HORSE).get("value")))
    finally:   # (both child in Kakariko again, as test_epona.way_back; Get_Epona stays: the job's own boot only)
        try:
            for x in ("A", "B"):
                pair.flag(x, AGE_FLAG, 1)
            pair.step_both(60)
            pair.warp_both(SCENE_CHILD, entrance=GATE)
        except Exception as e:
            note(pair, "way back: %s: %s" % (type(e).__name__, e))
