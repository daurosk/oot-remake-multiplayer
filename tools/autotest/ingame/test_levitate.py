"""Row 285 (customized characters levitate during cutscenes) as a bridge test: the port of coord
offline/levitate-285-run.ps1's scenario levitate-285 (wt-levitate-285 tools/autotest/scenarios/levitate-285.A/B.txt,
243118f2b). B plays Impa (Character Select number 10: an adult's legs on child Link) and opens the Deku Tree map
chest; A (Link) stands beside it and watches B's puppet play the chest's get scene. The fix it proves is on this base:
puppets take the mod's AnimationParam (retargeted clips, DrHammer79 4be88ad65 + aafef0443; reports/levitate-285.md
root cause A).

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_levitate_impa_chest | levitate-285 (row 285) | any (warps to InsideTheDekuTree) | B = Impa opens the map chest; A's puppet of B is held in B's cutscene (log); shots on each game at stand, open1, open3, open4 |

What the old run judged and how it is judged here:
 1. The SHOTS decide (the old result.txt: "Judge by the SHOTS ... B (Impa) puppet feet on the floor in A"): bridge
    shots on each game at four of the scenario's marks (stand, open1, open3, open4: the harness keeps a test at <= 8
    shots; open2 and after are stepped, not shot), compared to refs; NO-REF until the owner approves them. User-facing (what A sees of B), so shots, never a log line alone.
 2. A's module log says B's puppet is held in its cutscene: 'coop: puppet: P<n> show -> hold (their cutscene)'
    (lichii/hook/coop_puppet.cpp:515 Say via Director::Say :183, why "their cutscene" :2355). Without it the get
    scene was not seen as theirs: the shots would not test the held clip.
 3. B's own game is in the chest's event after its A press (state `event`); none = the chest gave no get scene
    (opened earlier on this boot, the chest is the save's for good): NO-REF, never PASS.
 Set-up: B's CharacterMenu_SelectedCharacter is read back as 10 (the scenario's setflag); A's lane pack
 (coop_puppet.json beside the pack, hookshot.pack_folders) lists Character Select AnimationParams its puppets took for
 B's age (characters.animation_params, written with the log's "the Player_<v> puppets take OoT Character Select's ...
 (retargeted clips)" lines, tools/puppet_pack/build.py): without them A's puppet of B plays the game's clips and the
 fix is not in this lane (SETUP-FAIL). The finally puts B's old character value back and both games back in Kakariko
 by the well (test_gear's warp); a failure there is raised only when the test body passed, else noted.
 Not ported: the old run's levitate-285-link comparison (B = Link, the same chest): the chest opens once per save,
 so the comparison cannot share this boot.
Every place, face and wait is the old scenario's (cited).
"""
import json
import os
import sys

import pytest

from test_enemies import face, place   # noqa: E402  (bridge place / face, as the scenarios' 'place' / 'toward')
from test_gear import GATE, SCENE_CHILD   # noqa: E402  (the boot's place: Kakariko by the well, the rows' warp)

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import hookshot   # noqa: E402  (pack_folders: where a game's co-op pack is, as the kit finds it)

SCENE, ENTRANCE = "InsideTheDekuTree", "Entrance"   # levitate-285.A/B.txt 'warp InsideTheDekuTree Entrance'
CHAR_FLAG, IMPA = "CharacterMenu_SelectedCharacter", 10   # levitate-285.B.txt 'setflag CharacterMenu_SelectedCharacter 10'
CHAR_WAIT = 900                                    # levitate-285.B.txt 'wait 900' after the setflag (A 'wait 900' too)
B_AT = (5.45, 108.9, 6.15)                          # levitate-285.B.txt 'place 5.45 108.9 6.15' (the chest's front)
A_AT = (7.57, 108.9, 8.35)                          # levitate-285.A.txt 'place 7.57 108.9 8.35' (beside it, 1.5 m)
CHEST = (6.51, 108.4, 7.25)                         # B 'toward 6.51 7.25 10 # face the chest' (chest-other.A.txt: 6.51 108.40 7.25)
PLACE_WAIT = 240                                    # both 'wait 240' after the place
# B 'press A' after 'mark stand', then 'wait 60; mark open1; wait 60; mark open2; wait 120; mark open3; wait 240; mark open4'
OPEN_MARKS = [("open1", 60), ("open2", 60), ("open3", 120), ("open4", 240)]
# B after open4: 'wait 240; press A; wait 120; press A; wait 120; press B; wait 600; mark after'
CLOSE = [(None, 240), ("A", 120), ("A", 120), ("B", 600)]
HOLD = r"coop: puppet: P\d+ show -> hold \(their cutscene\)"   # coop_puppet.cpp:515 (why: :2355)
SHOT_MARKS = ("stand", "open1", "open3", "open4")   # (tests/test_ingame.py: <= 8 shots a test)
TOL = 0.80                                          # as test_intro_seen / test_gear.shots (animated puppet)


def pack_anims(pair, x, age):
    """The Character Select AnimationParams game `x`'s co-op pack gave its puppets of that age (characters.
    animation_params in coop_puppet.json); raises AssertionError (in set-up: SETUP-FAIL) when there are none."""
    _, pack, _ = hookshot.pack_folders(pair.games[x]["folder"])
    f = os.path.join(pack, "coop_puppet.json")
    try:
        with open(f, encoding="utf-8") as fh:
            chars = json.load(fh).get("characters") or {}
    except (OSError, ValueError) as e:
        raise AssertionError("%s: no readable co-op pack manifest %s (%s)" % (x, f, e))
    v = "Child" if age == "child" else "Adult"
    got = (chars.get("animation_params") or {}).get(v)
    assert got, ("%s's pack (%s) lists no Character Select AnimationParam for its %s puppets (characters: %s): its "
                 "puppet of B plays the game's clips, the fix is not in this lane's pack" % (
                     x, f, v, "none" if not chars else "animation_params %s" % chars.get("animation_params")))
    return got


def way_back(pair, was, done):
    """B's old character value, then both games in Kakariko by the well (the boot's place for later default-boot
    tests). A failure is raised only when the test body passed (`done`); else noted, so the test's own message stays."""
    try:
        if was is not None and was != IMPA:
            pair.flag("B", CHAR_FLAG, was)
        pair.warp_both(SCENE_CHILD, entrance=GATE)
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s) and (s.get("scene") or "") == SCENE_CHILD, (
                "%s in %s after the test, not %s: the lane's later tests need the boot's place" % (
                    x, s.get("scene"), SCENE_CHILD))
    except Exception as e:
        if done:
            raise
        pair.notes.append("way back after the failure: %s: %s" % (type(e).__name__, e))


def shots(pair, mark):
    for x in ("A", "B"):
        pair.check_shot(x, "levitate-285-%s" % mark, tolerance=TOL)


@pytest.mark.budget(300)   # ESTIMATED: one warp + ~3'500 game frames (the old scenario's waits) + 8 shots
@pytest.mark.look
@pytest.mark.row("#285 a customized character's puppet stands on the floor in the other player's cutscene (Impa, chest)")
def test_levitate_impa_chest(pair):
    was, done = None, False
    try:
        with pair.setup():
            pair.notes.append("A's pack: %s" % ", ".join(pack_anims(pair, "A", pair.state("B").get("age"))))
            pair.warp_both(SCENE, ENTRANCE)
            for x in ("A", "B"):
                s = pair.state(x)
                assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
                assert (s.get("scene") or "") == SCENE, "%s in %s, not %s" % (x, s.get("scene"), SCENE)
            was = pair.flag_read("B", CHAR_FLAG).get("value")
            pair.flag("B", CHAR_FLAG, IMPA)
            got = pair.flag_read("B", CHAR_FLAG).get("value")
            assert got == IMPA, "B: %s is %s after the setflag, not %d" % (CHAR_FLAG, got, IMPA)
        pair.step_both(CHAR_WAIT)
        place(pair, "B", B_AT)
        place(pair, "A", A_AT)
        pair.step_both(PLACE_WAIT)
        face(pair, "B", CHEST)
        face(pair, "A", B_AT)                       # A 'toward 5.45 6.15 10 # face B'
        pair.step_both(10)
        shots(pair, "stand")
        at = pair.log["A"].mark()
        pair.press("B", buttons=["A"], frames=8)
        for i, (mark, wait) in enumerate(OPEN_MARKS):
            pair.step_both(wait)
            if i == 0 and not pair.state("B").get("event"):
                pair.unproven("B's A press at the map chest started no event (the chest was opened earlier on this "
                              "boot: it opens once per save)")
            if mark in SHOT_MARKS:
                shots(pair, mark)
        line = pair.log["A"].find(HOLD, pos=at)
        assert line, "A: no '%s' after B opened the chest (B's get scene not held as theirs)" % HOLD
        pair.notes.append("A: %s" % line)
        for button, wait in CLOSE:
            if button:
                pair.press("B", buttons=[button], frames=8)
            pair.step_both(wait)
        done = True
    finally:
        way_back(pair, was, done)
