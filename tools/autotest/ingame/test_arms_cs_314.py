"""Row 314 (Discord /bugreport #314, v0.12.0: "in the character select mod while playing with some skins as an adult the
arms start moving super weird while ..." (cut off), no logs) as bridge probe tests: WHERE do the arms go wrong?

| Test | Boot | Kind |
|---|---|---|
| test_arms_adult_pair[skin] | default pair (Kakariko by the well) | A plays the skin as ADULT, B watches A's puppet: shots of A's own character and of B's puppet of A, idle / walk / run (2 frames-apart shots) |
| test_arms_adult_solo[skin] | --solo (game A alone, co-op idle) | the same moves, A's own character only |

The question the report leaves open (reports/arms-cs-314.md): (a) the player's OWN character, (b) other players'
PUPPETS of that skin, (c) both. Read from the code before this run (NOT yet seen in a game):
 - Our module never writes the player's own skeleton: coop_pose.cpp OnCalcFrame READS it (ReadLocal, for the pose
   stream) and writes only to puppets (WriteFor/WritePose) and to fairies (CopySkeleton, coop_fairy.cpp:263). The
   kit changes only Link's own model files of Character Select (merge_tables.py MODELS_KEEP: tunic texture names),
   never a Player_<age>_C<nnn> skin. So (a) on a co-op-idle game (solo) would be Character Select's own rig.
 - A Character Select character's puppet takes no pose copy (coop_puppet.cpp IsCharacterActor in Director's pose
   paths; reports/puppet-run-307.md): it plays the sender's AS command only, with the mod's retargeted
   AnimationParam (lane pack coop_puppet.json characters.animation_params: Adult = Player.engine__component__
   AnimationParam.bgyml, Child = Player_Child's; both IsRetarget in the mod's Player_<age> pack, read 2026-11-27).
This test ends NO-REF (pair.unproven): its shots and notes are the evidence, judged by eye until refs are approved.

Every value is cited:
 - CHAR_FLAG: the mod's pick (characters.json "gamedata" CharacterMenu_SelectedCharacter Int, beside lane1-A's mod;
   test_levitate.CHAR_FLAG). The player is made with the pick at an age change (coop_char_ages.h header: "the game
   makes it at a save load, at an age change ... NOT at a plain scene or area load of the same age", lane run
   2026-11-23), so each skin is put on as a child and the age change to adult makes the adult with it.
   Co-op's "Character by age" is off on the lanes (no coop-character-ages.txt in lane1-A's config: Stored{} on=false,
   coop_char_ages.h Parse/Picks), so nothing rewrites the pick.
 - AGE_FLAG, SCENE_ADULT, SCENE_CHILD, GATE: test_gear's (hs-gear-adult.A.txt: the flag, 60 frames, then the warp).
 - SKINS: characters.json (number, file) and the adult rest angle of Arm_1 vs Link's (-35 deg) read from the lane mod's
   Model/Player_Adult_C<file>.Player_Adult.bfres (scratch reader, report): Malon 11/file 9 (+10 deg, the 74-skin
   45-deg class), Saria 2/file 0 (= Link's), Sheik 9/file 7 (-20 deg), Darunia's son 24/file 22 (10-deg class).
   Every one is in both lane packs (coop_puppet.json characters.numbers / variants Adult, read 2026-11-27 21:0x UTC).
 - PUPPET: B's spawn line, coop_puppet.cpp:431 Say 'P<n> <actor> spawned after' (Director::Say adds 'coop: puppet: ');
   the actor name CoopPuppet_Adult_C<nnn> (coop_puppet.cpp CharacterActor, build.py character_actor), its quiet copy
   CoopPuppet_AdultQuiet_C<nnn> (build.py quiet_name).
 - POSE: LICHII_COOP_DEBUG_POSE=1's once-a-second line (coop_pose.cpp OnCalcFrame: '<model> N bones, ... written/s,
   ... refused/s'), kept in the notes for each game.
 - The moves: bridge `move` (docs/playtest.md: world yaw, strength 0..1); walk 0.5 and run 1.0 as hs-puppet-smooth
   (test_puppet_run.py). The frame counts below are this probe's own choice (how long a move lasts before a shot),
   not a value any set-up check relies on.
"""
import json
import os
import re
import sys

import pytest

import solo
from test_gear import AGE_FLAG, GATE, SCENE_ADULT, SCENE_CHILD
from test_rows import place_in_front

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import hookshot   # noqa: E402  (pack_folders)

SWITCH = "LICHII_COOP_DEBUG_POSE=1"
CHAR_FLAG = "CharacterMenu_SelectedCharacter"
LINK = 0
# (id, Character Select number, file): characters.json beside lane1-A's mod (see the docstring).
SKINS = (("link", LINK, None), ("malon", 11, 9), ("saria", 2, 0), ("sheik", 9, 7), ("darunia-son", 24, 22))
SOLO_SKINS = (("link", LINK, None), ("malon", 11, 9), ("sheik", 9, 7))
SPAWN = r"coop: puppet: P\d+ (CoopPuppet_\w+) spawned after"
POSE = r"coop: pose: .* bones, .* written/s, .* refused/s"
CHAR_WAIT = 900          # frames for B's new puppet of A (test_puppet_run / test_levitate CHAR_WAIT)
AGE_WAIT = 60            # test_gear: the age flag, 60 frames, then the warp
IDLE, WALK, RUN = 90, 60, 24
CHUNK = 30
TOL = 0.80               # animated characters outdoors (test_levitate.TOL)


def pack_has(pair, x, file):
    """Game x's lane pack makes the adult puppet of Character Select file `file` (characters.numbers + variants)."""
    _, pack, _ = hookshot.pack_folders(pair.games[x]["folder"])
    with open(os.path.join(pack, "coop_puppet.json"), encoding="utf-8") as fh:
        chars = json.load(fh).get("characters") or {}
    f = str(file)
    assert f in (chars.get("numbers") or {}), "%s's pack has no Character Select file %s" % (x, f)
    assert "Adult" in ((chars.get("variants") or {}).get(f) or []), "%s's pack has no adult puppet of file %s" % (x, f)


def go(pair, yaw, strength, frames):
    """A moves toward world `yaw` for `frames` frames (bridge move); every other game is stepped the same frames."""
    left = frames
    while left > 0:
        n = min(CHUNK, left)
        pair.call("A", "move", yaw=yaw, strength=strength, frames=n)
        for y in pair.c:
            if y != "A":
                pair.step_other(y, n)
        left -= n


def yaw_to(pair, x, y):
    """World yaw (docs/playtest.md: 0 toward +z, 90 toward +x) from game x's Link to game y's Link."""
    import math
    a, b = pair.state(x)["pos"], pair.state(y)["pos"]
    return math.degrees(math.atan2(b[0] - a[0], b[2] - a[2])) % 360.0


def shots(pair, name):
    for x in sorted(pair.c):
        pair.check_shot(x, name, tolerance=TOL)


def to_adult_as(pair, number):
    """Child -> adult with Character Select pick `number` on A: the pick set and read back as a child, then the age
    flag and the warp on every game (the age change makes A's adult with the pick)."""
    pair.flag("A", CHAR_FLAG, number)
    got = pair.flag_read("A", CHAR_FLAG).get("value")
    assert got == number, "A: %s is %s after the setflag, not %d" % (CHAR_FLAG, got, number)
    for x in pair.c:
        pair.flag(x, AGE_FLAG, 0)
    pair.step_both(AGE_WAIT)
    pair.warp_both(SCENE_ADULT, entrance=GATE)
    for x in pair.c:
        s = pair.state(x)
        # `body` (our Link actor's name) may be null (docs/playtest.md "State": no live world list): never "child"
        assert pair.in_play(s) and s.get("age") == "adult" and s.get("body") != "child", (
            "%s not an adult in play after the warp (age %s, body %s)" % (x, s.get("age"), s.get("body")))
    got = pair.flag_read("A", CHAR_FLAG).get("value")
    assert got == number, "A: %s is %s after the age change, not %d" % (CHAR_FLAG, got, number)


def way_back(pair, was, done):
    """The boot as it was for later tests: A's pick back, both games child at the boot's place (test_gear.way_back's
    rule: a failure here is raised only when the body passed, else noted)."""
    try:
        if was is not None:
            pair.flag("A", CHAR_FLAG, was)
        for x in pair.c:
            pair.flag(x, AGE_FLAG, 1)
        pair.step_both(AGE_WAIT)
        pair.warp_both(SCENE_CHILD, entrance=GATE)
    except Exception as e:
        if done:
            raise
        pair.notes.append("way back after the failure: %s: %s" % (type(e).__name__, e))
        return
    if done:
        for x in pair.c:
            s = pair.state(x)
            assert pair.in_play(s) and s.get("age") == "child", (
                "%s stays %s after the test: the lane's later tests need child Link" % (x, s.get("age")))


def pose_notes(pair, tag, marks):
    for x in sorted(pair.c):
        lines = [ln for ln in pair.log[x].since(marks[x]) if re.search(POSE, ln)]
        pair.notes.append("%s %s pose: %s" % (tag, x, " | ".join(lines[-3:]) or "no pose line"))


def moves(pair, tag, toward):
    """Idle, walk toward `toward` (yaw), run toward it (two shots 6 frames apart), each with shots on every game."""
    pair.step_both(IDLE)
    shots(pair, "arms-314-%s-idle" % tag)
    go(pair, toward, 0.5, WALK)
    shots(pair, "arms-314-%s-walk" % tag)
    go(pair, toward, 1.0, RUN)
    shots(pair, "arms-314-%s-run1" % tag)
    go(pair, toward, 1.0, 6)
    shots(pair, "arms-314-%s-run2" % tag)


@pytest.mark.budget(360)   # ESTIMATED: two age-change warps + one puppet rebuild (<= 900 frames) + ~300 frames + 8 shots
@pytest.mark.look
@pytest.mark.needs_env(SWITCH)
@pytest.mark.row("#314 probe: a Character Select skin as adult, own character (A) and its puppet (B)")
@pytest.mark.parametrize("tag,number,file", SKINS, ids=[s[0] for s in SKINS])
def test_arms_adult_pair(pair, tag, number, file):
    was, done = None, False
    try:
        with pair.setup():
            pair.need_boot_place("child")
            if file is not None:
                for x in ("A", "B"):
                    pack_has(pair, x, file)
            was = pair.flag_read("A", CHAR_FLAG).get("value")
            b_at = pair.log["B"].mark()
            to_adult_as(pair, number)
            want = (r"CoopPuppet_Adult(?:Quiet)?_C%03d" % file) if file is not None else \
                r"CoopPuppet_Adult(?!(?:Quiet)?_C\d{3})\w*"

            def seen():
                for ln in pair.log["B"].since(b_at):
                    m = re.search(SPAWN, ln)
                    if m and re.fullmatch(want, m.group(1)):
                        return m.group(1)
                return None
            actor = pair.until("B: A's puppet made as %s ('%s')" % (tag, want), seen, max_frames=CHAR_WAIT)
            pair.notes.append("%s: B made A's puppet as %s" % (tag, actor))
            place_in_front(pair, "A", of="B", dist=8.0)   # A 8 m in front of B, facing B
            a = pair.state("A")["pos"]
            pair.act("B", "face %.2f %.2f" % (a[0], a[2]))
            pair.press("B", ["ZL"], frames=8)              # B's camera behind B (test_gear.face_each_other)
            pair.step_both(20)
        marks = {x: pair.log[x].mark() for x in pair.c}
        moves(pair, "pair-" + tag, yaw_to(pair, "A", "B"))   # A comes toward B: both views keep A in frame
        pose_notes(pair, tag, marks)
        done = True
    finally:
        way_back(pair, was, done)
    pair.unproven("row 314 probe: judge the arms in the shots (own A, puppet B) before any fix")


@pytest.mark.budget(300)   # ESTIMATED: two age-change warps + ~300 frames + 4 shots, one game
@pytest.mark.look
@solo.MARK
@pytest.mark.needs_env(SWITCH)
@pytest.mark.row("#314 probe: a Character Select skin as adult on a co-op-idle game (own character only)")
@pytest.mark.parametrize("tag,number,file", SOLO_SKINS, ids=[s[0] for s in SOLO_SKINS])
def test_arms_adult_solo(pair, tag, number, file):
    was, done = None, False
    try:
        with pair.setup():
            solo.prove(pair)
            pair.need_boot_place("child")
            was = pair.flag_read("A", CHAR_FLAG).get("value")
            to_adult_as(pair, number)
        marks = {"A": pair.log["A"].mark()}
        moves(pair, "solo-" + tag, (pair.state("A").get("facing") or 0.0))
        pose_notes(pair, tag, marks)
        done = True
    finally:
        way_back(pair, was, done)
    pair.unproven("row 314 probe: judge the own character's arms in the shots (co-op idle) before any fix")
