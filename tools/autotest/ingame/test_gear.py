"""Row 148 (puppet gear vs age) as bridge tests: ports of hs-gear-age and hs-gear-adult (offline/world-misc-0115-probe.ps1,
reports/port-sweep-c131.md G1a/G1b). Owner: port-sweep-c131.

| Test | Old row | Proves |
|---|---|---|
| test_child_gear_flags_not_drawn | hs-gear-age (row 148, bladeedge02 0.11.4) | a child's adult gear FLAGS never reach the other game's puppet |
| test_adult_gear_shown | hs-gear-adult (row 148 regression) | an adult's real gear does reach it, and is not lost at a rewarp |

The rule both judge (coop/host/src/player_reader.cpp, the comment above drawn_hand and read_equipment): for a CHILD the
hand / tunic / boots pieces are decided by the armor links (what the game draws), so a flag naming
Armor_Adult_Gauntlet_Gold is sent as '' (or the bracelet); for an ADULT a named flag stands. The sender sends
PLAYER_EQUIP only when the merged equipment changes (coop/host/src/sampler.cpp, `now != equipment_sent_`), and the other
game logs each one it gets as `P<n> PLAYER_EQUIP: weapon '...', ..., hand '<hand>'` (lichii/hook/coop_puppet.cpp, the
player_equip branch). The tunic and boots are not in that line: the shots carry them (LOOK, each game, twice).
"""
import pytest

import game_names
from test_rows import need_child_in_play, place_in_front

GOLD, GORON, HOVER = game_names.GOLD_GAUNTLETS, game_names.GORON_TUNIC, game_names.HOVER_BOOTS   # (one table)
SLOTS = (("hand", GOLD), ("armor", GORON), ("boots", HOVER))
FLAG = {"hand": "PlayerStatus.EquipHandArmor", "armor": "PlayerStatus.EquipArmor",   # coop_testinput.cpp WearFlag
        "boots": "PlayerStatus.EquipBoot"}
EQUIP = r"P1 PLAYER_EQUIP: "                          # coop_puppet.cpp player_equip Note (B's puppet of A = P1)
GOLD_SENT = EQUIP + r".*hand 'Armor_Adult_Gauntlet_"  # the row 148 bug: a child's puppet sent the adult's gauntlets
HAND_EMPTY = EQUIP + r".*hand ''"
SCENE_CHILD, SCENE_ADULT, GATE = "KakarikoVillage_Child", "KakarikoVillage_Adult", "Kakariko_WellSide"  # the rows' warp
AGE_FLAG = "Sequence_IsChildhood"                   # hs-gear-adult.A.txt: set after the load, then the warp


def face_each_other(pair):
    """hs-gear-*.A.txt: A steps aside to face B; B.txt: B faces A, 'press ZL' = camera behind B (so B's shots hold A's
    puppet). Here by place/face (docs/ingame-tests.md step 3: never walk to a spot)."""
    place_in_front(pair, "A", of="B")
    a = pair.state("A")["pos"]
    pair.act("B", "face %.2f %.2f" % (a[0], a[2]))
    pair.press("B", ["ZL"], frames=8)
    pair.step_both(20)


def shots(pair, name):
    """B's puppet of A and A's own Link, one shot on each game. One, not two a few frames apart: this row judges which
    pieces are drawn (still gear), not an animation (docs/ingame-tests.md step 6). Tolerance 0.80: an outdoor scene
    with two Links that breathe and sway."""
    for x in ("A", "B"):
        pair.check_shot(x, name, tolerance=0.80)


def wear(pair, slot, name):
    pair.act("A", "wear %s %s" % (slot, name))
    pair.expect_log("A", r"coop: test wear %s %s: set" % (slot, name), max_frames=60)   # coop_testinput.cpp Wear


def saved_gear(pair):
    """A's own gear flags before the test (put back at its end: later tests share this boot)."""
    out = {}
    for slot, _ in SLOTS:
        v = pair.flag_read("A", FLAG[slot]).get("value")
        out[slot] = v if isinstance(v, str) and v else "none"
    return out


def put_back(pair, gear):
    for slot, name in gear.items():
        wear(pair, slot, name)                    # (its 'set' line: a refused put-back is seen, not left on)


def way_back(pair, gear, done, warp_child):
    """The boot as it was for the lane's later tests (review-rows-c130, as kokiri-band's finally): A's gear flags back,
    then (warp_child) both games child again. A failure here is raised only when the test body passed (`done`); after
    a real FAIL it goes into the notes, so the test's own message stays. Then both games must be child in play."""
    try:
        put_back(pair, gear)
        if warp_child:
            for x in ("A", "B"):
                pair.flag(x, AGE_FLAG, 1)
            pair.step_both(60)                    # the flag before the warp, as on the way out
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


@pytest.mark.budget(240)
@pytest.mark.look
@pytest.mark.row("#148 a child's adult gear flags are not drawn on its puppet (hs-gear-age)")
def test_child_gear_flags_not_drawn(pair):
    with pair.setup():
        need_child_in_play(pair)
        gear = saved_gear(pair)
        face_each_other(pair)
        # the channel is live: B got A's equipment at least once this session (else every never_log below is empty)
        assert pair.log["B"].find(EQUIP, pos=0), "B: no P1 PLAYER_EQUIP line this session (B never got A's gear)"
    done = False
    try:
        wear(pair, "hand", GOLD)                  # hs-gear-age.A.txt 'wear hand ...' + 'wait 300' -> gold-flag
        pair.step_both(300)
        pair.never_log("B", GOLD_SENT)
        shots(pair, "gear-child-gold")
        wear(pair, "armor", GORON)                # 'wear armor' + 'wear boots' + 'wait 300' -> adult-flags
        wear(pair, "boots", HOVER)
        pair.step_both(300)
        pair.never_log("B", GOLD_SENT)
        shots(pair, "gear-child-flags")
        # rewarp: the game makes A's Link again from those flags (its GameData path, as after a reload). The entrance
        # makes it the game's map change, not a teleport (pair.warp_both docstring; hs-gear-age's 'warp ... WellSide').
        # Both games warp here (old row: A only); B's new puppet of A is made from the same sent equipment.
        pair.warp_both(SCENE_CHILD, entrance=GATE)
        need_child_in_play(pair)
        face_each_other(pair)
        pair.step_both(120)
        # player_reader.cpp: seen in game (hs-gear-adult run 20261118-161054) a child made again with these flags
        # draws none of them, only the bracelet: so B must never get the gauntlets, before or after the rewarp
        pair.never_log("B", GOLD_SENT)
        shots(pair, "gear-child-rewarp")
        done = True
    finally:
        way_back(pair, gear, done, warp_child=False)


@pytest.mark.budget(300)
@pytest.mark.look
# 'give' needs LICHII_COOP_CHEATS=1 (hs-gear-adult.A.txt '#! switches'): the default boot's own header turns it on
# (scenarios/live-kakariko-child.A.txt; bt_plugin.boot_identity), as for test_rows.test_masks_on_puppet: no needs_env.
@pytest.mark.row("#148 regression: an adult's real gear is drawn on its puppet (hs-gear-adult)")
def test_adult_gear_shown(pair):
    with pair.setup():
        need_child_in_play(pair)
        gear = saved_gear(pair)
        for x in ("A", "B"):                      # hs-gear-adult A/B: age flag after the load, then the warp
            pair.flag(x, AGE_FLAG, 0)
        pair.step_both(60)                        # 'wait 60': the flag takes 30+ frames before the warp
        pair.warp_both(SCENE_ADULT, entrance=GATE)
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s) and s.get("age") == "adult", "%s not an adult in play (%s)" % (x, s.get("age"))
        for _, name in SLOTS:                     # the save owns none of them: give first (hs-gear-adult.A.txt)
            pair.give("A", "PouchData.%s.StockNum" % name)
        face_each_other(pair)
    done = False
    try:
        pair.log["B"].mark()
        wear(pair, "hand", GOLD)
        pair.step_both(30)
        pair.act("A", "dress hand")               # the game's own equip call 0x2df6c80: A's own Link wears them
        pair.expect_log("B", EQUIP + r".*hand '%s'" % GOLD, max_frames=300)      # the old row's B log line
        shots(pair, "gear-adult-gold")
        wear(pair, "armor", GORON)
        wear(pair, "boots", HOVER)
        pair.step_both(30)
        pair.act("A", "dress all")
        pair.step_both(300)
        shots(pair, "gear-adult-all")             # red tunic + Hover Boots on A's own Link and B's puppet of A
        pair.log["B"].mark()
        pair.warp_both(SCENE_ADULT, entrance=GATE)
        face_each_other(pair)
        pair.step_both(120)
        pair.never_log("B", HAND_EMPTY)           # FAIL: the links rule lost a real piece at the rewarp
        shots(pair, "gear-adult-rewarp")
        done = True
    finally:
        way_back(pair, gear, done, warp_child=True)
