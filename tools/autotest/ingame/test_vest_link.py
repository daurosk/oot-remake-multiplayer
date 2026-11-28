"""vest-link (report 293, Snail parity P2, C #132): Link's Vest colour (part 27), painted by co-op on Link's own vest
textures, on your own Link and on your puppet in the other game; unset it is the tunic's colour (0.11.12 exactly).

| Test | What it proves | Kind |
|---|---|---|
| test_vest_own_and_puppet | a Vest colour on A's own Link and on A's puppet in B, then another colour | log + LOOK (hue + shots on each game) |
| test_vest_unset_is_tunic | Vest back to unset: the tunic's painting again, the picture as before | log + LOOK |
| test_vest_without_character_select | the same with B booted without the Character Select mod (both ways) | log + LOOK; needs_env |

Set-up and judging, each cited where used:
- `menu look link_vest=RRGGBB|none`: test only, the module's MENU verb (lichii/hook/coop_testinput.cpp MENU "look ",
  hookshot coop_pause.cpp TestMenu "look "): the part chosen as the Customize page chooses it (ChooseLookParts) and worn
  as when the menu closes (WearChosenLook), so it goes on the wire as a player's look does. The bridge lower-cases the
  line; the colour is read by CoopColors::Parse (lichii/hook/coop_colors.cpp, HexDigit takes either case).
- Own Link: the tunic wardrobe (host.what "tunic", hookshot/src/coop_look_tunic.h) logs `coop: look: your Link's tunic
  is <painting> now (...)` when it draws a new painting (coop_look_tunic.cpp Step); the painting name is
  CoopLookParts::TunicPainting: `tunic-<tunic|green>-vest-RRGGBB[-cap-RRGGBB]` with a vest, upper-case hex (Hex).
- Puppets: `coop: look: the puppets' tunics are <name> now` (coop_look_live.cpp FollowTunics, on a change only); the
  name is CoopLookParts::PuppetTunicsName: `ptunics|<colour>=RRGGBB[+capRRGGBB][+vestRRGGBB]...`.
- A is player 1, B player 2 (the lanes' host and joiner): A's tunic is its slot colour 0x3070FF (blue) and B's 0xFF3030
  (red) (coop/protocol messages.hpp slot_color), so yellow and green vests are far from both on the colour wheel.
- Colour: huecheck.solid_count (hair-check-0121's check, shared with test_hair_link.py): the pixels of the vest's
  colour class in SOLID patches inside the wearer's own screen box (test_hair_link.shots: bandcheck.box from the same
  frame's view.clip + that Link's pos, own Link on its game, its puppet on the other), before -> after. The old
  whole-frame changed_hue share judged the scene (gate-look 309d7180c: green 9-10% though the vest was green in every
  shot; the camera, Navi, torches and the warm ground changed too). Shots also go to refs (NO-REF candidates).
- No Character Select: the ONE no-CS lane game, tools/autotest/ingame/no_cs.py (gear-link 812219312, C #132): MARK
  (needs_env B:COOP_TEST_NO_CHARACTER_SELECT=1, harness-c132's switch) and prove() (B has no mod, the module's
  ModPresent read through flag_read).
- Fake reactions: ingame/fakes/vest_link.py; selftests: tests/test_ingame_vest_link.py.
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import huecheck     # noqa: E402  (the shared colour check: solid_count)
import no_cs        # noqa: E402  (the ONE no-Character-Select game)
from test_gear_link import face_each_other   # noqa: E402  (the shared set-up helpers)
from test_hair_link import shots             # noqa: E402  (lit shots + each wearer's screen box: one helper)
from test_rows import need_child_in_play   # noqa: E402

YELLOW, GREEN = 0xFFD000, 0x20C020

OWN_VEST = r"coop: look: your Link's tunic is tunic-(?:green|[0-9A-F]{6})-vest-%06X\b\S* now"
OWN_NO_VEST = r"coop: look: your Link's tunic is (?!\S*-vest-)\S+ now"
PUP_VEST = r"coop: look: the puppets' tunics are \S*\+vest%06X\S* now"
PUP_NO_VEST = r"coop: look: the puppets' tunics are (?!\S*\+vest)\S+ now"

# The colour check's thresholds (huecheck.solid_count in the wearer's box, counts scaled to a 1280-wide shot). From the
# STORED gate-look 309d7180c lane-1 shots (coord-multiplayer/offline/gate-look-309d7180/lane1/shots, vest-link-*) with
# hand-drawn boxes (A's own Link 570,370-720,640 on A; A's puppet 665,415-750,565 on B; huecheck.py's vest classes):
#   GAIN: unset->yellow A +2197, B +238; yellow->green A +2037, B +282; a shot pair with no wear of that colour moved
#   the yellow count at most 61 (A: unset vs unset-again / green; the yellow boots and blond hair are in the box) and
#   the green count 0. GAIN[YELLOW] 120 (2x that noise, half the least real gain), GAIN[GREEN] 60 (a fifth of 282).
#   LEFT_VEST: of the yellow a wear added, what was left after green or after the unset: A -0.03, B 0.03 (the count
#   back at the unset shot's); a vest still yellow keeps 1.0. 0.3 as test_hair_link's LEFT_HAIR (10x the most left).
# Those boxes were drawn by hand; the test's own boxes come from view.clip. Until MEASURED cites the huecheck lines of a
# lane run of THIS check, a run whose checks all hold ends NO-REF (pair.unproven), never PASS, and a gain under GAIN is
# noted, not failed. Always a FAIL: a wear that added nothing (the vest was not repainted), yellow left at/over
# LEFT_VEST. The 0.90 SSIM of unset-again vs before is noted only (a starting value).
GAIN = {YELLOW: 120, GREEN: 60}
LEFT_VEST = 0.3
SAME_SSIM = 0.90
MEASURED = None   # e.g. "run <name>: yellow gain A .. B .., green gain .., yellow left after green .. after unset .."


def set_vest(pair, x, value):
    r = pair.act(x, "menu look link_vest=%s" % value)
    assert r.get("ok", True), "%s refused 'menu look link_vest=%s': %s" % (x, value, r)


def wait_since(pair, x, pattern, mark, max_frames):
    return pair.until("%s log /%s/" % (x, pattern), lambda: pair.log[x].find(pattern, pos=mark), max_frames)


def wear(pair, who, colour):
    """`who` wears the Vest `colour` (None: unset); waits for its own Link's line and the puppet's line on the other."""
    other = "B" if who == "A" else "A"
    marks = {x: pair.log[x].mark() for x in ("A", "B")}
    set_vest(pair, who, "none" if colour is None else "%06X" % colour)   # (no '#': the bridge refuses it in an act line, coop_playtest_core.cpp ParseRequest)
    own = OWN_NO_VEST if colour is None else OWN_VEST % colour
    pup = PUP_NO_VEST if colour is None else PUP_VEST % colour
    wait_since(pair, who, own, marks[who], 240)
    wait_since(pair, other, pup, marks[other], 600)   # (the look over the wire, then the puppets' tunics' round)
    pair.step_both(30)


def note(pair, line):
    print(line)
    pair.notes.append(line)


def count(shot, colour):
    return huecheck.solid_count(shot["path"], shot["box"], colour)


def gained(pair, before, after, colour, what, low):
    """Each game's solid `colour` count in the wearer's box, before -> after: nothing added is a FAIL (the vest was not
    repainted); less than GAIN goes to `low` (judged only once MEASURED). Returns {x: gain}."""
    out = {}
    for x in ("A", "B"):
        n0, n1 = count(before[x], colour), count(after[x], colour)
        note(pair, "huecheck %s %s #%06X before=%d after=%d gain=%d box=%s" % (
            x, what, colour, n0, n1, n1 - n0, list(after[x]["box"])))
        assert n1 - n0 > 0, "%s %s: no #%06X added on the Link: the vest was not repainted (before %d, after %d)" % (
            x, what, colour, n0, n1)
        if n1 - n0 < GAIN[colour]:
            low.append("%s %s: #%06X gained %d (want %d)" % (x, what, colour, n1 - n0, GAIN[colour]))
        out[x] = n1 - n0
    return out


def left(pair, before, after, colour, gain, what):
    """Of the `colour` a wear added (`gain`, from `before`), how much is still in the wearer's box at `after`: at or over
    LEFT_VEST is a FAIL (the vest kept the colour)."""
    for x in ("A", "B"):
        n0, n1 = count(before[x], colour), count(after[x], colour)
        share = (n1 - n0) / float(max(1, gain[x]))
        note(pair, "huecheck %s %s #%06X left=%d of %d share=%.3f" % (x, what, colour, n1 - n0, gain[x], share))
        assert share < LEFT_VEST, "%s: yellow left on the vest %s (%d of %d)" % (x, what, n1 - n0, gain[x])


def judged(pair, low):
    if not MEASURED:
        pair.unproven("the vest colour thresholds are not measured yet (MEASURED: the huecheck lines of a lane run)%s"
                      % ("; low: " + "; ".join(low) if low else ""))
    assert not low, "; ".join(low)


def put_back(pair, done):
    """Both vests unset for the lane's next test; a failure here is raised only when the test itself passed (else it
    would hide the test's own FAIL), and noted otherwise."""
    try:
        unset_both(pair)
    except Exception as e:   # noqa: BLE001
        if done:
            raise
        pair.notes.append("put back: %s" % e)


def unset_both(pair):
    for x in ("A", "B"):
        set_vest(pair, x, "none")
    pair.step_both(60)


# Budgets: ESTIMATED (not measured), from the waits: set-up < 30 s, each wear() at most 240 + 600 + 30 frames
# (~29 s at 30 fps), shots and the put-back; 2-3 wear()s per test, the no-CS test 3 + the other game's.
@pytest.mark.budget(240)   # ESTIMATED
@pytest.mark.look
@pytest.mark.row("#293 P2 Link's Vest colour (own Link + puppet)")
def test_vest_own_and_puppet(pair):
    with pair.setup():
        need_child_in_play(pair)
        face_each_other(pair)
        unset_both(pair)
    done, low = False, []
    try:
        before = shots(pair, "vest-link-unset")
        wear(pair, "A", YELLOW)
        yellow = shots(pair, "vest-link-yellow")
        gain = gained(pair, before, yellow, YELLOW, "unset->yellow", low)
        wear(pair, "A", GREEN)
        green = shots(pair, "vest-link-green")
        gained(pair, yellow, green, GREEN, "yellow->green", low)
        left(pair, before, green, YELLOW, gain, "after green")
        done = True
    finally:
        put_back(pair, done)
    judged(pair, low)


@pytest.mark.budget(240)   # ESTIMATED
@pytest.mark.look
@pytest.mark.row("#293 P2 Link's Vest unset = the tunic's colour (0.11.12)")
def test_vest_unset_is_tunic(pair):
    with pair.setup():
        need_child_in_play(pair)
        face_each_other(pair)
        unset_both(pair)
    done, low = False, []
    try:
        before = shots(pair, "vest-link-unset-before")   # (own name: test_vest_own_and_puppet's -unset is another ref)
        wear(pair, "A", YELLOW)
        coloured = shots(pair, "vest-link-unset-yellow")
        gain = gained(pair, before, coloured, YELLOW, "unset->yellow", low)
        wear(pair, "A", None)
        back = shots(pair, "vest-link-unset-again")
        # (only the yellow in the wearer's box: the whole-frame count took the torch flames as yellow left, B #148)
        left(pair, before, back, YELLOW, gain, "after unset")
        for x in ("A", "B"):
            score, err = imagecheck_ssim(before[x]["path"], back[x]["path"])
            note(pair, "huecheck %s unset-again ssim=%s" % (x, score))
            if err or score < SAME_SSIM:
                low.append("%s: unset again vs before SSIM %s %s (want >= %.2f)" % (x, score, err or "", SAME_SSIM))
        done = True
    finally:
        put_back(pair, done)
    judged(pair, low)


def imagecheck_ssim(a, b):
    import imagecheck   # (the shared compare's SSIM: the same measure as every look ref)
    return imagecheck.ssim(a, b)


@pytest.mark.budget(300)   # ESTIMATED
@pytest.mark.look
@no_cs.MARK
@pytest.mark.row("#293 P2 Link's Vest without Character Select (C #132)")
def test_vest_without_character_select(pair):
    with pair.setup():
        need_child_in_play(pair)
        no_cs.prove(pair)
        face_each_other(pair)
        unset_both(pair)
    done, low = False, []
    try:
        before = shots(pair, "vest-link-no-cs-unset")
        wear(pair, "A", YELLOW)   # A (with the mod) -> its puppet in the CS-less B
        a_yellow = shots(pair, "vest-link-no-cs-a-yellow")
        gain = gained(pair, before, a_yellow, YELLOW, "A unset->yellow", low)
        wear(pair, "A", None)
        unset = shots(pair, "vest-link-no-cs-unset-again")
        left(pair, before, unset, YELLOW, gain, "after A's unset")
        b_before = shots(pair, "vest-link-no-cs-b-unset", who="B")   # (B's own Link on B, its puppet on A)
        wear(pair, "B", GREEN)    # B's own Link without the mod -> its puppet in A (same places: A faces B already)
        b_green = shots(pair, "vest-link-no-cs-b-green", who="B")
        gained(pair, b_before, b_green, GREEN, "B unset->green", low)
        done = True
    finally:
        put_back(pair, done)
    judged(pair, low)
