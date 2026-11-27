"""Row 307 (a Character Select character's puppet runs like a fast walk; back to Link it stays so) as a bridge test.

| Test | Boot | Kind |
|---|---|---|
| test_puppet_run_swap | default (Kakariko by the well) | B walks and runs as Link, as Malon, as Link again; A watches B's puppet (probe + shots) |

Owner report (2026-11-27 21:0x UTC, player-reports row 307): with another character (Malon from Character Select) the
puppet has the old running bug, no run animation, a walk played very fast; switching back to Link keeps it.

Read from the code and the game's AS file (not yet run; coop_as_vars.h): a character's puppet takes no pose copy
(coop_puppet.cpp Director::PoseFrame, IsCharacterActor) and plays only the sender's AS command ('Move') at the sender's
rate; 'Move' blends walk and run by AS variables (LeftStickLength, LinearVelXZ, XZSpeed, IsDash in AS/Player_Child.
root.asb's variable table) that the player's own code writes each frame and nothing writes on a puppet.

This test is the probe run first (LICHII_COOP_AS_VARS_PROBE=1: where those values are in the AS controller, and what
each puppet holds there) plus LICHII_COOP_DEBUG_POSE=1 (A's pose copy: 'written/s' and 'refused/s' a second, coop_pose.
cpp OnCalcFrame), for the Link puppet before and after the swap. It ends NO-REF (pair.unproven) with every line it read
in its notes: nothing is judged yet. The fix's run judges A's puppet of B by the same phases.

Every value is cited:
 - Malon: Character Select number 5, file 3 ('Malon (child)', characters.json beside lane1-A's mod; the lane pack's
   coop_puppet.json characters.numbers {"3": 5}, variants {"3": ["Child", "Adult"]}: read 2026-11-27 20:5x UTC).
 - CHAR_FLAG and CHAR_WAIT: test_levitate's (the saved Int the mod reads; 900 frames after the setflag).
 - A's spawn line: coop_puppet.cpp:456 Say 'P<n> <actor> spawned after ...' (Director::Say adds 'coop: puppet: ').
 - The moves: hs-puppet-smooth.A.txt's walk (stick 0.5, 240) and run (stick 1, 150) back and forth, here as the
   bridge's `move` (world yaw, strength; docs/playtest.md move) so B goes along +z and back whatever the camera does.
"""
import json
import os
import re
import sys

import pytest

from test_gear import GATE, SCENE_CHILD   # noqa: E402  (the boot's place: Kakariko by the well)

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import hookshot   # noqa: E402  (pack_folders)

SWITCHES = ("LICHII_COOP_AS_VARS_PROBE=1", "LICHII_COOP_DEBUG_POSE=1")
CHAR_FLAG = "CharacterMenu_SelectedCharacter"   # test_levitate.CHAR_FLAG
LINK, MALON, MALON_FILE = 0, 5, "3"             # characters.json: Link 0 (no file); 'Malon (child)' number 5, file 3
CHAR_WAIT = 900                                  # test_levitate.CHAR_WAIT (frames after the setflag)
SPAWN = r"coop: puppet: P\d+ (CoopPuppet_\w+) spawned after"   # coop_puppet.cpp:456
STAND, WALK, RUN = 60, 240, 150                  # frames: hs-puppet-smooth.A.txt 'stick 0 0.5 240', 'stick 0 1 150'
WALK_S, RUN_S = 0.5, 1.0                         # its stick strengths
CHUNK = 30                                       # frames per bridge move call (B), A stepped the same
PROBE = "as-vars probe: "                        # coop_puppet.cpp AsVarsProbeFrame (Director::Say)
POSE = r"coop: pose: .* written/s, .* refused/s"  # coop_pose.cpp OnCalcFrame debug line
TOL = 0.80                                       # animated puppet (test_levitate.TOL)


def pack_has_malon(pair):
    """A's lane pack makes Malon's child puppet (characters.numbers[file 3] == 5, 'Child' in its variants)."""
    _, pack, _ = hookshot.pack_folders(pair.games["A"]["folder"])
    f = os.path.join(pack, "coop_puppet.json")
    with open(f, encoding="utf-8") as fh:
        chars = json.load(fh).get("characters") or {}
    assert (chars.get("numbers") or {}).get(MALON_FILE) == MALON, (
        "A's pack %s: characters.numbers[%s] is %s, not %d (Malon)" % (f, MALON_FILE,
                                                                       (chars.get("numbers") or {}).get(MALON_FILE), MALON))
    assert "Child" in ((chars.get("variants") or {}).get(MALON_FILE) or []), (
        "A's pack %s has no child puppet for file %s (Malon)" % (f, MALON_FILE))


def go(pair, yaw, strength, frames):
    """B moves along world yaw at `strength` for `frames` frames (bridge move), A stepped the same frames."""
    left = frames
    while left > 0:
        n = min(CHUNK, left)
        pair.call("B", "move", yaw=yaw, strength=strength, frames=n)
        pair.call("A", "step", frames=n)
        left -= n


def moves(pair, tag):
    """Stand, walk away and back, run away and back; two shots of A's view a few frames apart in the run."""
    pair.step_both(STAND)
    go(pair, 0.0, WALK_S, WALK)
    go(pair, 180.0, WALK_S, WALK)
    go(pair, 0.0, RUN_S, RUN // 2)
    pair.check_shot("A", "puppet-run-307-%s-run1" % tag, tolerance=TOL)
    go(pair, 0.0, RUN_S, 6)
    pair.check_shot("A", "puppet-run-307-%s-run2" % tag, tolerance=TOL)
    go(pair, 0.0, RUN_S, RUN // 2 - 6)
    go(pair, 180.0, RUN_S, RUN)
    pair.step_both(STAND)


def wait_spawn(pair, at, want, what):
    """Steps both games until A's log has a spawn line since `at` whose actor matches `want`; returns the actor."""
    rx, wx = re.compile(SPAWN), re.compile(want)

    def seen():
        for ln in pair.log["A"].since(at):
            m = rx.search(ln)
            if m and wx.fullmatch(m.group(1)):
                return m.group(1)
        return None
    return pair.until("A: B's puppet made again as %s ('%s')" % (what, want), seen, max_frames=CHAR_WAIT)


def collect(pair, tag, a_at, b_at):
    """The probe and pose lines of the phase, into the notes (each game's probe report and A's pose copy)."""
    b_probe = [ln for ln in pair.log["B"].since(b_at) if PROBE in ln]
    a_probe = [ln for ln in pair.log["A"].since(a_at) if PROBE in ln]
    a_pose = [ln for ln in pair.log["A"].since(a_at) if re.search(POSE, ln)]
    pair.notes.append("%s: B probe %d lines, last report: %s" % (tag, len(b_probe), " | ".join(b_probe[-14:])))
    pair.notes.append("%s: A probe (its puppet of B): %s" % (tag, " | ".join([ln for ln in a_probe if " P" in ln][-3:])))
    pair.notes.append("%s: A pose copy: %s" % (tag, " | ".join(a_pose[-4:])))
    return b_probe, a_probe


@pytest.mark.budget(420)   # ESTIMATED: 3 phases x ~1'000 frames + two puppet rebuilds (<= 900 frames each) + 6 shots
@pytest.mark.look
@pytest.mark.needs_env(*SWITCHES)
@pytest.mark.row("#307 a Character Select character's puppet runs (not a fast walk), and Link's after the swap back")
def test_puppet_run_swap(pair):
    was, done = None, False
    try:
        with pair.setup():
            pack_has_malon(pair)
            pair.warp_both(SCENE_CHILD, entrance=GATE)   # (inside the set-up: C #145; the boot's own place)
            for x in ("A", "B"):
                s = pair.state(x)
                assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
                assert (s.get("scene") or "") == SCENE_CHILD, "%s in %s, not %s" % (x, s.get("scene"), SCENE_CHILD)
                assert s.get("age") == "child", "%s is %s, not child (Malon's puppet here is the child one)" % (
                    x, s.get("age"))
            was = pair.flag_read("B", CHAR_FLAG).get("value")
            if was != LINK:
                a_at = pair.log["A"].mark()
                pair.flag("B", CHAR_FLAG, LINK)
                wait_spawn(pair, a_at, r"CoopPuppet_Child(?!_C\d{3})\w*", "Link")
            b = pair.state("B")["pos"]
            pair.act("A", "face %.2f %.2f" % (b[0], b[2]))
        phases = []
        for tag, value, want in (("link", None, None),
                                 ("malon", MALON, r"CoopPuppet_Child(?:Quiet)?_C003"),
                                 ("link-again", LINK, r"CoopPuppet_Child(?!(?:Quiet)?_C\d{3})\w*")):
            a_at, b_at = pair.log["A"].mark(), pair.log["B"].mark()
            if value is not None:
                pair.flag("B", CHAR_FLAG, value)
                got = pair.flag_read("B", CHAR_FLAG).get("value")
                assert got == value, "B: %s is %s after the setflag, not %d" % (CHAR_FLAG, got, value)
                actor = wait_spawn(pair, a_at, want, tag)
                pair.notes.append("%s: A made B's puppet as %s" % (tag, actor))
                b = pair.state("B")["pos"]
                pair.act("A", "face %.2f %.2f" % (b[0], b[2]))
            moves(pair, tag)
            phases.append((tag,) + collect(pair, tag, a_at, b_at))
        for tag, b_probe, _ in phases:
            assert b_probe, "%s: B's log has no '%s' line (the probe did not run: switch missing?)" % (tag, PROBE)
        done = True
        pair.unproven("probe run: where the AS variables are and what A's puppet of B holds there are in the notes; "
                      "judged by the fix's run")
    finally:
        try:
            if was is not None:
                pair.flag("B", CHAR_FLAG, was)
            pair.warp_both(SCENE_CHILD, entrance=GATE)
        except Exception as e:
            if done:
                raise
            pair.notes.append("way back after the failure: %s: %s" % (type(e).__name__, e))
