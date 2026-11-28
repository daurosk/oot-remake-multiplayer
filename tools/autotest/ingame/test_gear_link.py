"""gear-link (report 293, Snail parity P4): Link's Equipment shown or hidden, on your own Link and on your puppet in the
other game; with or without Character Select (C #132: the game's own shown bit, no mod texture).

| Test | What it proves | Kind |
|---|---|---|
| test_gear_hidden_own_and_puppet | part 29 hides A's gear on A's own Link and on A's puppet in B | log + LOOK (shots on each game) |
| test_gear_shown_again | Hidden off: the pieces come back on both | log + LOOK |
| test_gear_hidden_without_character_select | game B without the mod: B's own Link, and A's puppet seen on B | log + LOOK; needs no_cs.ENV |
| test_gear_hidden_solo | co-op idle (no session): Hidden hides and shows again A's own Link | log + LOOK; needs --solo |

Set-up and judging, each cited where used:
- `menu look link_gear=hidden|none`: test only, the module's MENU verb (lichii/hook/coop_testinput.cpp, hookshot
  coop_pause.cpp TestMenu "look "): the part chosen as the Customize page chooses it (ChooseLookParts) and worn as when
  the menu closes (WearChosenLook), so it goes on the wire as a player's look does.
- Log lines: lichii/hook/coop_char_colours.cpp Wardrobe::Frame says `characters: equipment hidden on <your model|a
  puppet> (N piece(s))` and `characters: equipment shown again on ...` (LOG_INFO "coop: {}", hookshot DressCharacters).
- Without Character Select: game B booted by harness-c132's switch (no_cs.py: ENV, MARK, the setup proof). Without
  that switch in --env the test is deselected: NOT-RUN in the rc summary = NOT PROVEN, never PASS (C #132).
- Solo (owner rule: non-multiplayer features work solo, co-op idle): harness-c132's --solo boot (solo.py: MARK, prove()
  = game A only and its log co-op idle). The module dresses own Link while idle: hookshot coop_hookshot.cpp g_on is set
  after the 'co-op idle until Host or Join' line (Start), and the FRAME site calls DressCharacters whenever g_on.
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import no_cs                                               # noqa: E402  (the ONE no-Character-Select game)
import solo                                                # noqa: E402  (the ONE solo boot: game A alone)
from test_rows import face_each_other, need_child_in_play   # noqa: E402  (the shared set-up helpers)

HIDDEN_ON = r"characters: equipment hidden on %s \((\d+) piece\(s\)\)"
SHOWN_ON = r"characters: equipment shown again on %s \((\d+) piece\(s\)\)"
OWN, PUPPET = "your model", "a puppet"


def wears_a_sword(pair, x):
    """A piece in the hidden handles (the sword: handle 0, kGearHandles) so Hidden has something to hide; the state's
    equips.weapon is PlayerStatus.EquipWeapon (docs/playtest.md "State")."""
    e = pair.state(x).get("equips") or {}
    assert e.get("weapon"), "%s wears no sword (equips %s): Hidden would hide nothing" % (x, e)


def set_gear(pair, value, x="A"):
    r = pair.act(x, "menu look link_gear=%s" % value)
    assert r.get("ok", True), "%s refused 'menu look link_gear=%s': %s" % (x, value, r)


def hide_and_check(pair, shot, x="A"):
    """Game x hides its Link's gear: x's own Link and x's puppet on the other game, a shot on each."""
    y = "B" if x == "A" else "A"
    mark_x, mark_y = pair.log[x].mark(), pair.log[y].mark()
    set_gear(pair, "hidden", x)
    pair.expect_log(x, HIDDEN_ON % OWN, max_frames=240)
    pair.expect_log(y, HIDDEN_ON % PUPPET, max_frames=600)   # (the look over the wire, then the puppet's frame)
    pair.step_both(30)
    assert pair.log[x].find(HIDDEN_ON % OWN, pos=mark_x), "%s: own Link's gear not hidden" % x
    assert pair.log[y].find(HIDDEN_ON % PUPPET, pos=mark_y), "%s: %s's puppet's gear not hidden" % (y, x)
    pair.check_shot(x, shot)   # x's own Link, from x's camera
    pair.check_shot(y, shot)   # x's puppet, facing y


def show_and_check(pair, shot):
    set_gear(pair, "none")
    pair.expect_log("A", SHOWN_ON % OWN, max_frames=240)
    pair.expect_log("B", SHOWN_ON % PUPPET, max_frames=600)
    pair.step_both(30)
    pair.check_shot("A", shot)
    pair.check_shot("B", shot)


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("#293 P4 Link's Equipment hidden (own Link + puppet)")
def test_gear_hidden_own_and_puppet(pair):
    with pair.setup():
        need_child_in_play(pair)
        wears_a_sword(pair, "A")
        face_each_other(pair)
    try:
        hide_and_check(pair, "gear-link-hidden")
        # Hidden every frame (the game shows a piece again when it uses it): still hidden later, said once.
        pair.step_both(120)
        pair.check_shot("B", "gear-link-hidden-later")
    finally:
        set_gear(pair, "none")
        pair.step_both(30)


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("#293 P4 Link's Equipment shown again")
def test_gear_shown_again(pair):
    with pair.setup():
        need_child_in_play(pair)
        wears_a_sword(pair, "A")
        face_each_other(pair)
    hide_and_check(pair, "gear-link-hidden")
    show_and_check(pair, "gear-link-shown")


@no_cs.MARK
@pytest.mark.budget(240)
@pytest.mark.look
@pytest.mark.row("#293 P4 Link's Equipment hidden without Character Select (C #132)")
def test_gear_hidden_without_character_select(pair):
    with pair.setup():
        need_child_in_play(pair)
        no_cs.prove(pair)
        wears_a_sword(pair, "A")
        wears_a_sword(pair, "B")
        face_each_other(pair)
    try:
        # B without the mod hides its own Link's gear (and A, with it, sees B's puppet hidden) ...
        hide_and_check(pair, "gear-link-no-cs-own", x="B")
        set_gear(pair, "none", "B")
        pair.step_both(60)
        # ... and B shows A's puppet hidden when A hides its own.
        hide_and_check(pair, "gear-link-no-cs-puppet", x="A")
    finally:
        set_gear(pair, "none", "A")
        set_gear(pair, "none", "B")
        pair.step_both(30)


@solo.MARK
@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("#293 P4 Link's Equipment hidden solo (co-op idle)")
def test_gear_hidden_solo(pair):
    with pair.setup():
        solo.prove(pair)
        # stepped to a fresh in-play sample in the boot's scene, child Link, never one read; on a solo pair it reads
        # game A only (harness-c133-scene 31a934d74: until_in_play defaults to pair.c; review-rows-c130-3 16:47)
        pair.need_boot_place("child")
        wears_a_sword(pair, "A")
    try:
        mark = pair.log["A"].mark()
        set_gear(pair, "hidden")
        pair.expect_log("A", HIDDEN_ON % OWN, max_frames=240)
        pair.step_both(30)
        assert pair.log["A"].find(HIDDEN_ON % OWN, pos=mark), "A: own Link's gear not hidden solo"
        pair.check_shot("A", "gear-link-solo-hidden")
        mark = pair.log["A"].mark()
        set_gear(pair, "none")
        pair.expect_log("A", SHOWN_ON % OWN, max_frames=240)
        pair.step_both(30)
        assert pair.log["A"].find(SHOWN_ON % OWN, pos=mark), "A: own Link's gear not shown again solo"
        pair.check_shot("A", "gear-link-solo-shown")
    finally:
        set_gear(pair, "none")
        pair.step_both(30)
