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
- Colour: huecheck.changed_hue (this row's file), the pixels that changed between two shots of the same place and the
  share of them in the vest's hue (no guessed mask region). Shots also go to refs (NO-REF candidates for the owner).
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
import huecheck     # noqa: E402  (this row's colour check)
import no_cs        # noqa: E402  (the ONE no-Character-Select game)
from test_gear_link import face_each_other   # noqa: E402  (the shared set-up helpers)
from test_rows import need_child_in_play   # noqa: E402

YELLOW, GREEN = 0xFFD000, 0x20C020

OWN_VEST = r"coop: look: your Link's tunic is tunic-(?:green|[0-9A-F]{6})-vest-%06X\b\S* now"
OWN_NO_VEST = r"coop: look: your Link's tunic is (?!\S*-vest-)\S+ now"
PUP_VEST = r"coop: look: the puppets' tunics are \S*\+vest%06X\S* now"
PUP_NO_VEST = r"coop: look: the puppets' tunics are (?!\S*\+vest)\S+ now"

# The colour check's thresholds are starting values, NOT MEASURED (review-rows-c130 11:21 point 2): MIN_CHANGED is the
# floor under which the vest was clearly not repainted (a FAIL), MIN_SHARE the share of the changed pixels in the vest's
# hue, and huecheck's moved/saturated/near. Until MEASURED cites a lane run's huecheck lines (yellow, green, and an
# unchanged pair for the idle-sway floor), a run whose checks all hold ends NO-REF (pair.unproven), never PASS, and a
# share under MIN_SHARE is noted, not failed. The 0.90 SSIM of unset-again vs before is the same: a starting value.
MIN_CHANGED, MIN_SHARE, SAME_SSIM = 20, 0.5, 0.90
MEASURED = None   # e.g. "run <name>: yellow changed .. share .., green .., unchanged pair changed .."


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


def shots(pair, name):
    return {x: pair.check_shot(x, name)["path"] for x in ("A", "B")}


def note(pair, line):
    print(line)
    pair.notes.append(line)


def hue(pair, before, after, colour, what, low):
    """Each game's changed_hue from `before` to `after`: under MIN_CHANGED is a FAIL (the vest was not repainted); a
    share under MIN_SHARE goes to `low` (judged only once MEASURED)."""
    for x in ("A", "B"):
        h = huecheck.changed_hue(before[x], after[x], colour)
        note(pair, "huecheck %s %s #%06X changed=%d coloured=%d share=%.3f" % (
            x, what, colour, h["changed"], h["coloured"], h["share"]))
        assert h["changed"] >= MIN_CHANGED, "%s %s: only %d pixels changed: the vest was not repainted %s" % (
            x, what, h["changed"], h)
        if h["share"] < MIN_SHARE:
            low.append("%s %s: %.0f%% of the changed pixels are #%06X's hue (want %.0f%%)" % (
                x, what, h["share"] * 100, colour, MIN_SHARE * 100))


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
        hue(pair, before, yellow, YELLOW, "unset->yellow", low)
        wear(pair, "A", GREEN)
        green = shots(pair, "vest-link-green")
        hue(pair, yellow, green, GREEN, "yellow->green", low)
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
        before = shots(pair, "vest-link-unset")
        wear(pair, "A", YELLOW)
        coloured = shots(pair, "vest-link-unset-yellow")
        wear(pair, "A", None)
        back = shots(pair, "vest-link-unset-again")
        for x in ("A", "B"):
            score, err = imagecheck_ssim(before[x], back[x])
            # (only the pixels the wear painted: a whole-frame count took the torch flames as yellow left, B #148)
            h = huecheck.left_after_unset(before[x], coloured[x], back[x], YELLOW)
            note(pair, "huecheck %s unset-again #%06X wear=%d left=%d share=%.3f ssim=%s" % (
                x, YELLOW, h["wear"], h["left"], h["share"], score))
            assert h["wear"] >= MIN_CHANGED, "%s: only %d pixels took the yellow: the vest was not repainted %s" % (
                x, h["wear"], h)
            assert h["share"] < huecheck.LEFT_SHARE, "%s: yellow left on the vest after unset %s" % (x, h)
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
        hue(pair, before, a_yellow, YELLOW, "A unset->yellow", low)
        wear(pair, "A", None)
        unset = shots(pair, "vest-link-no-cs-unset-again")
        wear(pair, "B", GREEN)    # B's own Link without the mod -> its puppet in A (same places: A faces B already)
        b_green = shots(pair, "vest-link-no-cs-b-green")
        hue(pair, unset, b_green, GREEN, "B unset->green", low)
        done = True
    finally:
        put_back(pair, done)
    judged(pair, low)
