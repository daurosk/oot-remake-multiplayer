"""Look rows in the new form: what a player sees on another player's puppet (shots on each game).

| Test | Old row | Kind |
|---|---|---|
| test_kokiri_hood_band | kokiri-band-c124 (coord offline/kokiri-band-c124-run.ps1, scenario kokiri-band-c124.A/B.txt) | LOOK |

kokiri-band-c124 (review-c124 section 5 point 2): the adult Kokiri hood's trims on a puppet near and far, beside the own
Link in the same look. The fix (3d91ec10a, tools/puppet_pack/look_cap.py) is in the puppet pack's coop_look/cap.bin: its
smaller mip levels count the cap's texels, so the thin hood band keeps the Cap trim colour at a distance.
"""
import math
import os

import pytest

import pair as P
import sight
from test_rows import need_child_in_play

# The look of both games (the old row wrote both coop-options.ini: `color = blue`, `look_cap_trim = yellow`).
# LICHII_COOP_COLOR: the kit's tunic switch (lichii/hook/coop_settings.cpp ApplyEnvironment, key `color`);
# LICHII_COOP_LOOK: the test's parts instead of the settings' (hookshot/src/coop_look.h EnvParts), so a lane file's
# leftovers never count. Yellow, not red: the Goron tunic is red, so a red trim could not tell OFF from ON on its hood
# (the old row's -Trim rule). "blue" and "yellow" are presets (hookshot/src/coop_look_menu.cpp kPresets).
LOOK_ENV = ("LICHII_COOP_COLOR=blue", "LICHII_COOP_LOOK=cap_trim:yellow")
# The old scenario's `#! switches` line (kokiri-band-c124.A.txt / .B.txt).
ROW_ENV = ("LICHII_COOP_DEBUG_ANIM=1", "LICHII_COOP_CHEATS=1")
# The texture name the look paints: blue 3070FF + trim yellow FFC020. Seen in run cap-trim-c44's game-out-A.txt:3013
# (the old row's gate, kokiri-band-c124-run.ps1 line 18); the form "your Link's tunic is tunic-202020-cap-FFFFFF now"
# in shots-lane1/bj-242d/20261121-220846/game-out-A.txt.
TUNIC = "tunic-3070FF-cap-FFC020"
# The cap.bin of the fix, made from the branch's tools over the romfs dump: "cap.bin: 174220 bytes, 6 textures, 43444
# offsets, smaller_level=1" (coord offline/kokiri-band-c124/setup-only.txt:26). The module logs the sum of the offsets
# (hookshot/src/coop_look_live.cpp StartModule, "the Cap trim paints {} blocks of {} tunic textures ({} of the Goron
# and Zora tunics)"); an older lane pack gave 43228 (run cap-trim-c44); the look_cap.py just before the fix (3d91ec10a^, packs/
# look_cap_old.py) makes 174844 bytes, 6 textures, 43600 offsets over dump/romfs (kokiri-band-c124-5's maker check,
# 11:5x UTC 2026-11-26; the fix's packs/look_cap.py: 174220 bytes, 43444 offsets). Any other number: the lane has another
# pack, and this test would judge nothing.
CAP_BLOCKS = 43444
# The probe (review-rows-c130 09:41 point a): KOKIRI_BAND_PROBE_PACK=43600 (+ --pack look_cap_old) on the runner's side runs the test on a lane
# with the pack BEFORE the fix, prints the same bandcheck lines and ends NO-REF, never PASS or FAIL: the two runs' values
# are what BAND_SHARE / OWN_MIN get measured from.
PROBE_PACK = os.environ.get("KOKIRI_BAND_PROBE_PACK")
# The cap.bin comes in by --pack (packs/look_cap.py: made by the tested sha's tools/puppet_pack/look_cap.py over the
# lane's romfs; packs/look_cap_old.py: the look_cap.py before the fix, for the probe), never from the lane's own pack.
PACK = "look_cap_old" if PROBE_PACK else "look_cap"
CAP_LINE = r"the Cap trim paints %s blocks of 6 tunic textures \(3 of the Goron and Zora tunics\)" % (
    PROBE_PACK or CAP_BLOCKS)
# The old row's crash patterns (kokiri-band-c124-run.ps1, the pass's checks).
CRASH = r"exception 0x|crash window: .*caught|FATAL|CRASH:"
# A's distances in front of B (metres), far to near: the test's own choice in place of the old row's walk (stick 0
# -0.4 for 15 frames, 6 times, from the spot of run cap-trim-c44 where B's puppet of A was ~150 px tall in 1280x720).
# 14 m makes the puppet smaller than that (the smaller mip levels the fix is about); 2.5 m is as near as the old last step.
DISTANCES = (14.0, 11.0, 8.5, 6.5, 5.0, 3.5, 2.5)
# A stands off B's line of sight (review-rows-c130 09:41 point b): BEARING degrees to the side of B's facing, so from
# the camera behind B its puppet is beside B's own Link, not behind it. The test's own choice; every shot proves it:
# the 4 boxes of the band check (both Links and their background boxes) must be apart, else SETUP-FAIL.
BEARING = 35.0
SETTLE = 30   # frames after a place: the puppet follows A's samples (the old row waited 90 after each step)


# B's turns from its arrival facing, tried in order until A at the far spot is in sight (sight.prove): the arrival
# facing first (the old row's view), then right, back, left. The test's own choice; each try's numbers go to the notes.
HEADINGS = (0, 90, 180, 270)


def look_along(pair, x, facing):
    """Game x's Link faces `facing` degrees (the bridge's 'face X Z': a point 1 m that way; facing 0 = +z, 90 = +x)."""
    p = pair.state(x)["pos"]
    f = math.radians(facing)
    pair.act(x, "face %.2f %.2f" % (p[0] + math.sin(f), p[2] + math.cos(f)))
    pair.step_both(2)


def put_in_front(pair, x, of, dist):
    """Game x's Link `dist` m from game `of`'s Link, BEARING degrees to the side of its facing, facing it
    ('place X Y Z' / 'face X Z': tools/autotest/hookshot.py:357-366; facing 0 = +z, 90 = +x: docs/playtest.md
    "State"); returns the distance it stands at after SETTLE frames (a wall or a slope can stop it short)."""
    s = pair.state(of)
    px, py, pz = s["pos"]
    yaw = math.radians(s["facing"] + BEARING)
    pair.act(x, "place %.2f %.2f %.2f" % (px + dist * math.sin(yaw), py, pz + dist * math.cos(yaw)))
    pair.act(x, "face %.2f %.2f" % (px, pz))
    pair.step_both(SETTLE)
    got = pair.state(x).get("pos")
    if not got:
        raise P.SetupFail("%s has no place after 'place' (not in play: %s)" % (x, pair.state(x).get("not_ready")))
    ax, _, az = got
    return math.hypot(ax - px, az - pz)


def puppet_near(pair, x, at, within=2.0):
    """Game x's world list has an adult puppet within `within` m of `at` (Client.world(), as test_puppets_spawn)."""
    w = pair.c[x].world().get("world") or {}
    for a in w.get("actors", []):
        p = a.get("pos")
        if (a.get("name") or "").startswith("CoopPuppet_Adult") and p and math.hypot(p[0] - at[0], p[2] - at[2]) < within:
            return p
    return None


# The band check (review-rows-c130 09:36 point 1: a whole-shot SSIM cannot see a thin band turn blue). On B's shot,
# trim-yellow pixels in the box of B's own Link and in the box of A's puppet (bandcheck.py), each less the same-size
# box beside it. B's own Link must show the trim (else the measure itself is wrong); the puppet's trim per pixel of
# its box must be at least BAND_SHARE of the own Link's: with the bug its band is the tunic's blue (about 0). The
# share is a starting value, NOT MEASURED: until MEASURED cites the probe runs (the fixed pack and the old 43600 pack,
# the bandcheck lines of each), a run whose band checks all hold ends NO-REF, never PASS (worker-rules: no guessed
# values). The same goes for is_trim's hue/saturation/value range, bandcheck.HEAD and the box's half-width.
OWN_MIN = 4
BAND_SHARE = 0.35
MEASURED = None   # e.g. "run <name> (43444) + run <name> (43600): own net .., puppet share .. vs .."


def boxes_apart(own, pup, d):
    """A box over the other Link would measure that Link: the set-up is wrong, not the band (SETUP-FAIL)."""
    import bandcheck
    if own and pup:
        clash = bandcheck.disjoint(own["box"], own["side"], pup["box"], pup["side"])
        if clash:
            raise P.SetupFail("%.1f m: the band check's boxes overlap (%s, %s): A's puppet is not beside B's own Link"
                              % (d, clash[0], clash[1]))


def band_check(pair, path, view, own_pos, puppet_pos, d):
    import bandcheck
    own = bandcheck.measure(path, view, own_pos, other=puppet_pos)
    pup = bandcheck.measure(path, view, puppet_pos, other=own_pos)
    line = "bandcheck %.1f m: own %s puppet %s" % (d, own, pup)
    print(line)
    pair.notes.append(line)
    boxes_apart(own, pup, d)
    if PROBE_PACK:
        return
    assert own and own["net"] >= OWN_MIN, "%.1f m: B's own Link shows no trim on the shot (%s): the measure is wrong" % (
        d, own)
    assert pup, "%.1f m: A's puppet is off B's shot" % d
    share = (pup["net"] / pup["area"]) / (own["net"] / own["area"])
    assert pup["net"] >= 1 and share >= BAND_SHARE, (
        "%.1f m: the puppet's hood band is not the trim colour: %d trim px in %d (own Link %d in %d, share %.2f < %.2f)"
        % (d, pup["net"], pup["area"], own["net"], own["area"], share, BAND_SHARE))


@pytest.mark.budget(480)   # (300 + sight proofs: up to 4 headings and 6 nearer spots, 2 shots each)
@pytest.mark.look
@pytest.mark.needs_env(*(LOOK_ENV + ROW_ENV))
@pytest.mark.needs_pack(PACK)
@pytest.mark.row("review-c124 5.2 the adult Kokiri hood's trims on a puppet near and far (kokiri-band-c124)")
def test_kokiri_hood_band(pair):
    # One boot serves every test of the lane, and the others need child Link in Kakariko (need_child_in_play):
    # this test makes both games adult, so it puts them back as the boot left them, pass or fail (a failed way back
    # after a failure does not hide the first one: the lane's next test then says so in its own set-up).
    done = False
    try:
        with pair.setup():
            need_child_in_play(pair)
            # the look took, on both games, from the switches (the old row's live gate): A sent 2 parts (tunic + Cap
            # trim), B wears them (lichii/hook/coop_look_share.cpp "{} part colour(s) sent" / "P{} wears {} part
            # colour(s)"), both own Links wear TUNIC, and the pack's cap.bin is the fix's
            for x in ("A", "B"):
                bad = pair.log[x].find(r"LICHII_COOP_LOOK: '", pos=0)   # (EnvParts: a pair it could not take)
                assert bad is None, "%s: %s" % (x, bad)
                assert pair.log[x].find(r"the parts are LICHII_COOP_LOOK's \(cap_trim:yellow\)", pos=0), \
                    "%s: the module did not take LICHII_COOP_LOOK (a module before kokiri-band-bridge)" % x
                assert pair.log[x].find(CAP_LINE, pos=0), "%s: the lane's puppet pack is not the fix's cap.bin (%s)" % (
                    x, pair.log[x].find(r"the Cap trim paints \d+ blocks[^\n]*|no coop_look/cap\.bin", pos=0))
            assert pair.log["A"].find(r"\b2 part colour\(s\) sent \(PLAYER_LOOK\)", pos=0), "A did not send its 2 parts"
            assert pair.log["B"].find(r"P\d+ wears 2 part colour\(s\)", pos=0), "B did not get A's 2 parts"
            # adult: the bridge warp to an _Adult name sets Sequence_IsChildhood first (docs/playtest.md "warp"); the old
            # scenario: setflag Sequence_IsChildhood 0, then warp KakarikoVillage_Adult
            pair.warp_both("KakarikoVillage_Adult")
            # stepped, never one read (B #139 16:31, the gifts-237 class): the bridge's warp answers on arrival
            # (coop_playtest_core.cpp WarpArrived), but the state read right after can hold a sample older than the frame
            pair.until_in_play("both games in play after the warp to KakarikoVillage_Adult")
            for x in ("A", "B"):
                s = pair.state(x)
                assert pair.in_play(s) and s.get("age") == "adult", "%s is not adult in play (%s, %s)" % (
                    x, s.get("age"), s.get("not_ready"))
            for x in ("A", "B"):   # (the old gate read the whole log: since the session start)
                pair.until("%s's own Link wears %s" % (x, TUNIC),
                           lambda: pair.log[x].find(r"your Link's tunic is %s" % TUNIC, pos=0), max_frames=600)
        # kokiri-band-c124-7 (run kokiri-look-182cd53b): the distance alone put A behind the Kakariko gate wall, out
        # of B's sight, and the band check called the wall a blue band. B turns (HEADINGS, from its arrival facing)
        # until A at the far spot is IN SIGHT on B's shot (sight.prove); none = SETUP-FAIL, never a FAIL of the band.
        # (after the set-up block: two shots per try do not fit SETUP_LIMIT_S; a SetupFail here is still SETUP-FAIL)
        face0 = pair.state("B")["facing"]
        tried = []
        for turn in HEADINGS:
            look_along(pair, "B", face0 + turn)
            got = put_in_front(pair, "A", of="B", dist=DISTANCES[0])
            if abs(got - DISTANCES[0]) >= 1.0:
                tried.append("turn %d: A stands %.1f m, not %.1f" % (turn, got, DISTANCES[0]))
                continue
            # the camera behind B, along its facing (the old B script: `press ZL`; pair.press's 8 frames): B's own
            # Link near the camera, A's puppet beyond it and to the side (BEARING)
            pair.press("B", ["ZL"], frames=8)
            pair.step_both(30)
            try:
                sight.prove(pair, "B", "A", "kokiri-band-turn%03d" % turn)
                pair.notes.append("B turned %d deg from its arrival facing: A in sight at %.1f m" % (turn, got))
                break
            except sight.LeftPlay as e:   # (A is gone: no other heading can run)
                raise P.SetupFail("turn %d: %s; tried before: %s" % (turn, e, "; ".join(tried) or "none"))
            except P.SetupFail as e:
                tried.append("turn %d: %s" % (turn, e))
        else:
            raise P.SetupFail("A's puppet in sight at %.1f m on no heading: %s" % (DISTANCES[0], "; ".join(tried)))
        # each distance: the puppet is there (B's world list), then B's shot; A's own Link at the far and the near spot
        for i, d in enumerate(DISTANCES):
            if i:
                got = put_in_front(pair, "A", of="B", dist=d)
                assert abs(got - d) < 1.0, "A stands %.1f m from B, not %.1f" % (got, d)
            a = pair.state("A")["pos"]
            pup = pair.until("B's puppet of A at %.1f m" % d, lambda: puppet_near(pair, "B", a), max_frames=120)
            sb = pair.state("B")   # (B's camera and Link for the band check; the shot runs at most 5 frames after it)
            # tolerance 0.80: Kakariko has moving people and birds (docs/ingame-tests.md step 6); the band is a small
            # thing, so the first run's reference candidates (new/) are what a person judges (yellow band on the puppet as on
            # B's own Link; FAIL: the puppet's band blue where B's own is yellow)
            if i:                  # (the far spot was proven in the set-up; each nearer one is proven here)
                sight.prove(pair, "B", "A", "kokiri-band-%03ddm" % round(d * 10))
                a = pair.state("A")["pos"]
                pup = pair.until("B's puppet of A back at %.1f m" % d, lambda: puppet_near(pair, "B", a), max_frames=120)
                sb = pair.state("B")
            res = pair.check_shot("B", "kokiri-band-%03ddm" % round(d * 10), tolerance=0.80)
            band_check(pair, res["path"], sb["view"], sb["pos"], pup, d)
            if i in (0, len(DISTANCES) - 1):
                pair.check_shot("A", "kokiri-band-%03ddm" % round(d * 10), tolerance=0.80)
        for x in ("A", "B"):
            pair.never_log(x, CRASH)
        done = True
        if PROBE_PACK or not MEASURED:
            pair.unproven("%s; the bandcheck lines are in the notes" % (
                "probe run on the %s pack" % PROBE_PACK if PROBE_PACK else
                "the band thresholds are not measured yet (MEASURED: run the probe on the fixed and the old pack)"))
    finally:
        try:
            pair.warp_both("KakarikoVillage_Child")   # (an _Child name sets Sequence_IsChildhood 1 first)
            pair.until_in_play("both games in play after the warp back to KakarikoVillage_Child")   # (stepped, as above)
            back = {x: pair.state(x) for x in ("A", "B")}
        except Exception:
            if done:
                raise
            back = None
        for x in ("A", "B") if done else ():
            assert back[x].get("age") == "child", "%s stays %s after the test: the lane's later tests need child" % (
                x, back[x].get("age"))
