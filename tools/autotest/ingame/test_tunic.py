"""Rows #202 (own colour) and 198 (a puppet's tunic copy drawn in your own Link's texels) as one bridge test: the port of
hs-tunic's own-colour line (tools/autotest/groups/hookshot-parity-wave1.txt:37) with row 198's check from
offline/row-198-gear-run.ps1 run F1 (reports/port-sweep-c131.md G1c). Owner: port-sweep-c131(-2).

Not ported here (said in the report): row-198-gear-run's C0 control (the old module, before the fix: a build, not a
test) and F2 gear-parts (look_boots / look_hylian_frame = gold: coop-options.ini keys with no switch,
lichii/hook/coop_settings.cpp kVariables; a bridge test cannot set them).
"""
import colorsys
import math

import pytest

import pair as P
from test_rows import need_child_in_play

BLUE = "A:LICHII_COOP_COLOR=blue"                  # wave1:37 switches (LICHII_COOP_PUPPET=1 is in every boot: BASE_ENV)
# The lane's own look kept out (gate-tunic-309d7180, 0.12.0: the first run of this test FAILED on the lanes' files, not
# on the module). The pause menu saves a look to the game's coop-options.ini (CoopTitle::SaveLook) and a boot reads it
# back; the save baseline does not put that file back, so lane1-A started with look_cap_trim = blue (and 10 parts more),
# said 'tunic-3070FF-cap-3070FF', and lane1-B with color = pink + look_cap_trim = orange: no green Link for the colour
# check. LICHII_COOP_LOOK (hookshot/src/coop_look.h EnvParts) gives the parts instead of the file's; one key with no
# value is no part at all (ReadParts passes an empty value over), so both games wear their tunic and nothing else.
# B: green, the game's own (preset 30D040 = slot_color(3): TunicPainting's own green, the pack's green puppet), as
# hookshot-B wore for wave1:37; without it a lane's color line (or none: B's slot red FF3030) decides.
NO_PARTS = "LICHII_COOP_LOOK=cap_trim:"
B_GREEN = "B:LICHII_COOP_COLOR=green"
PARTS = r"coop: look: the parts are LICHII_COOP_LOOK's \(cap_trim:\), not the settings': 0 part\(s\) set"  # coop_look_live.cpp
# wave1:37 expects, A's log. The 'wears' line names LICHII_COOP_SELF_TUNIC when set, else green
# (lichii/hook/coop_puppet_config.cpp:55-59): run.ps1 put green's own files in, a live.py boot may not, so \S+.
STARTS = r"coop: your Link wears \S+ at the start; green players get CoopPuppet_<Age>_Green"
OWN_BLUE = r"coop: look: your Link.s tunic is tunic-3070FF now"        # coop_look_tunic.cpp:505
PACK_WARN = r"the puppet pack's tunic files are"                       # wave1:37 never= (coop_look_live.cpp:728/733)
CROSS = r"row 198 cross guard"                                         # coop_look_live.cpp:591 (row-198-gear-run F1)

# The colour check (review-rows-c130 10:38: a stored-look compare alone is no proof of a colour). On each game's shot:
# tunic-blue and tunic-green pixels in the box of each Link (bandcheck.box: the shot's camera view.clip, feet to
# bandcheck.HEADS by age), each less the same-size box beside it (bandcheck.side_box: the background's own). Blue = 3070FF
# (lichii/hook/coop_colors.cpp:21, hue 222), A's colour; green = the game's own Kokiri tunic (B wears no colour). The
# Link wearing blue must hold more blue than green and at least COLOUR_MIN blue, the green one the other way round.
# Each game's own Link is the control (its colour is the module's own line above), the other's puppet is the row.
# bandcheck.HEADS["child"] is measured (1.25 m; the adult 1.9 m made the boxes overlap: run tunic-look-53d40ab3-2);
# each box takes its Link's age from that game's state.
# NOT MEASURED: the hue/saturation/value ranges, COLOUR_MIN and BEARING. Until MEASURED cites a lane run's
# colourcheck lines, a run whose colour checks all hold ends NO-REF, never PASS (worker-rules: no guessed values).
COLOUR_MIN = 4
MEASURED = None   # e.g. "run <name>: A own blue .. green .., B's puppet of A blue .. green .., ..."
# A stands BEARING degrees off B's facing, and A faces BEARING beside B, so from the camera behind each Link (ZL) the
# other is beside it on the shot, not behind it (as test_look.py's kokiri band). Every shot proves it: the 4 boxes
# must be apart, else SETUP-FAIL.
BEARING = 35.0
DIST = 2.5        # m: hs-gear-*.A.txt "A steps aside to face B" (test_rows.place_in_front's default)


def is_blue(rgb):
    hh, s, v = colorsys.rgb_to_hsv(*(c / 255.0 for c in rgb[:3]))
    return 205 <= hh * 360 <= 240 and s >= 0.45 and v >= 0.25


def is_green(rgb):
    hh, s, v = colorsys.rgb_to_hsv(*(c / 255.0 for c in rgb[:3]))
    return 95 <= hh * 360 <= 150 and s >= 0.40 and v >= 0.20


def count(px, b, test):
    x0, y0, x1, y1 = b
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if test(px[x, y]))


def colours(path, view, pos, other, age, other_age):
    """{box, side, blue, green} of the Link of `age` at `pos` on the shot (net of the side box away from `other`, a Link
    of `other_age`), or None when it is off the shot."""
    import bandcheck
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    b = bandcheck.box(view, pos, w, h, age)
    if not b:
        return None
    ob = bandcheck.box(view, other, w, h, other_age)
    side = bandcheck.side_box(b, w, (ob[0] + ob[2]) / 2 if ob else None)
    px = im.load()
    out = {"box": b, "side": side}
    for name, test in (("blue", is_blue), ("green", is_green)):
        out[name] = max(0, count(px, b, test) - (count(px, side, test) if side else 0))
    return out


def puppet_at(pair, x, at, within=2.0):
    """Game x's world list: the position of a CoopPuppet_ within `within` m of `at` (any age or colour), or None."""
    w = pair.c[x].world().get("world") or {}
    for a in w.get("actors", []):
        p = a.get("pos")
        if (a.get("name") or "").startswith("CoopPuppet_") and p and math.hypot(p[0] - at[0], p[2] - at[2]) < within:
            return p
    return None


def stand_apart(pair):
    """A DIST m from B at BEARING off B's facing, then each faces BEARING beside the other; ZL = camera behind each."""
    s = pair.state("B")
    bx, by, bz = s["pos"]
    yaw = math.radians(s["facing"] + BEARING)
    ax, az = bx + DIST * math.sin(yaw), bz + DIST * math.cos(yaw)
    pair.act("A", "place %.2f %.2f %.2f" % (ax, by, az))
    to_b = math.atan2(bx - ax, bz - az) + math.radians(BEARING)
    pair.act("A", "face %.2f %.2f" % (ax + math.sin(to_b), az + math.cos(to_b)))
    pair.press("A", ["ZL"], frames=8)
    pair.press("B", ["ZL"], frames=8)
    pair.step_both(20)


def prove_apart(pair, viewers, name):
    """After stand_apart, outside the set-up block (two shots per proof do not fit SETUP_LIMIT_S; a SetupFail is still
    SETUP-FAIL): each viewer whose shot of the other player's puppet is judged sees it IN SIGHT, not only inside its
    projected box (sight.prove; kokiri-band-c124-7, run kokiri-look-182cd53b: a place by distance put the other Link
    behind a wall)."""
    import sight
    for x in viewers:
        sight.prove(pair, x, "B" if x == "A" else "A", "%s-%s" % (name, x))


def colour_check(pair, x, path, view, own_pos, pup_pos, own_colour, pup_colour, own_age, pup_age):
    own = colours(path, view, own_pos, pup_pos, own_age, pup_age)
    pup = colours(path, view, pup_pos, own_pos, pup_age, own_age)
    line = "colourcheck %s: own %s puppet %s" % (x, own, pup)
    print(line)
    pair.notes.append(line)
    if own and pup:
        import bandcheck
        clash = bandcheck.disjoint(own["box"], own["side"], pup["box"], pup["side"])
        if clash:
            raise P.SetupFail("%s: the colour check's boxes overlap (%s, %s): the Links are not side by side"
                              % (x, clash[0], clash[1]))
    for who, got, want in (("own Link", own, own_colour), ("puppet", pup, pup_colour)):
        assert got, "%s: the %s is off the shot" % (x, who)
        other = "green" if want == "blue" else "blue"
        assert got[want] >= COLOUR_MIN and got[want] > got[other], (
            "%s's shot: the %s is not %s: %d %s px, %d %s px in its box %s" % (
                x, who, want, got[want], want, got[other], other, got["box"]))


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.needs_env(BLUE, B_GREEN, NO_PARTS)
@pytest.mark.row("#202 own colour + 198 no cross guard (hs-tunic, row-198-gear F1)")
def test_own_colour_no_cross_guard(pair):
    with pair.setup():
        need_child_in_play(pair)
        stand_apart(pair)             # hs-tunic.A.txt: A steps off and stands by B; B.txt: faces A, camera behind
    prove_apart(pair, ("A", "B"), "tunic")   # (both shots judge the other's puppet: colour_check)
    # the colour is set at the module's start: these lines are from the boot (pos=0), as the old row read them
    for x in ("A", "B"):              # (else the lane file's parts are worn: the lines below would judge them)
        bad = pair.log[x].find(r"LICHII_COOP_LOOK: '", pos=0)
        assert bad is None, "%s: %s" % (x, bad)
        assert pair.log[x].find(PARTS, pos=0), "%s: no /%s/: the module did not take LICHII_COOP_LOOK" % (x, PARTS)
    assert pair.log["A"].find(STARTS, pos=0), "A: no /%s/ (wave1:37)" % STARTS
    pair.until("A log /%s/" % OWN_BLUE, lambda: pair.log["A"].find(OWN_BLUE, pos=0), max_frames=300)
    assert pair.log["A"].find(PACK_WARN, pos=0) is None, "A: %s" % pair.log["A"].find(PACK_WARN, pos=0)
    pair.step_both(120)                # hs-tunic.B.txt b-sees-a: 120 + 170 frames in; the puppets are drawn by now
    # wave1:37 see=hs-tunic-A/B: A's own Link blue + B's puppet green on A; A's puppet blue on B. Still colours: one
    # shot each game (docs/ingame-tests.md step 6), the stored-look record; the colour itself by colour_check.
    # Tolerance 0.80: two Links that breathe, Kakariko's people.
    for x, own_colour, pup_colour in (("A", "blue", "green"), ("B", "green", "blue")):
        o = "B" if x == "A" else "A"
        so = pair.state(o)
        at = so["pos"]
        pup = pair.until("%s's puppet of %s" % (x, o), lambda: puppet_at(pair, x, at), max_frames=120)
        s = pair.state(x)              # (x's camera and Link; the shot runs at most a few frames after it)
        res = pair.check_shot(x, "tunic-blue", tolerance=0.80)
        colour_check(pair, x, res["path"], s["view"], s["pos"], pup, own_colour, pup_colour, s["age"], so["age"])
    pair.step_both(120)
    for x in ("A", "B"):               # row 198: the guard must never fire, in either game, from the boot on
        line = pair.log[x].find(CROSS, pos=0)
        assert line is None, "%s: %s" % (x, line)
    if not MEASURED:
        pair.unproven("the colour check's values are not measured yet (MEASURED: a lane run's colourcheck lines); "
                      "the colourcheck lines are in the notes")
