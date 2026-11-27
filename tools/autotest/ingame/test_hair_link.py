"""hair-link (report 293, Snail parity P1, C #133): Link's Hair colour (part 19), painted by co-op on Link's own hair
texture, on your own Link and on your puppet in the other game (its own adult + child copy per tunic letter); unset it
is the game's own hair (0.11.12 exactly).

| Test | What it proves | Kind |
|---|---|---|
| test_hair_own_and_puppet | a Hair colour on A's own Link and on A's (child) puppet in B, then another colour | log + LOOK (hue + shots on each game) |
| test_hair_unset_is_game | Hair back to unset: the game's hair again, the picture as before | log + LOOK |
| test_hair_under_mask | the colour kept with the Skull Mask on (the mask moves the hair's UV, not its texture) | log + LOOK |
| test_hair_without_character_select | the same with B booted without the Character Select mod (both ways) | log + LOOK; needs_env |
| test_hair_adult | adult Link: own Link + the adult puppet copy (P<L>_Hairs_Player_Adult_Alb) | log + LOOK (warps; last) |

| test_hair_solo | one game, no co-op session (no Host/Join): your own Link's hair coloured | log + LOOK; needs the SOLO boot |

Solo (co-op idle): the SOLO lane boot is harness-c133's (item 7, C #133: one lane game, no co-op session, the needs_solo
marker) and ingame/solo.py is its ONE place, as no_cs.py is the no-CS game's. Until solo.py is in the harness the test
carries an impossible needs_env placeholder: always DESELECTED (named in summary.json 'deselected'): NOT PROVEN, never
PASS and never a SETUP-FAIL. (`menu leave` is no way there: the lane's next test needs the session back.)

Set-up and judging, each cited where used:
- `menu look hair=RRGGBB|none`: test only, the module's MENU verb (lichii/hook/coop_testinput.cpp MENU "look ",
  hookshot coop_pause.cpp TestMenu "look "): key `hair` is Part::hair's key (coop_look.cpp Parts), chosen as the
  Customize page chooses it (ChooseLookParts) and worn as when the menu closes (WearChosenLook), so it goes on the wire
  as a player's look does. The colour is read by CoopLook::ReadParts -> CoopColors::Parse (coop_colors.cpp: a leading
  '#' is optional, either case). Sent WITHOUT the '#': the bridge refuses an act line with '#' or ';'
  (hookshot/src/coop_playtest_core.cpp ParseRequest "act"; hair-link-4217f901 16:48 bad_request on all 4 tests).
- Own Link: the gear wardrobe (host.what "gear", coop_look_live.cpp) logs `coop: look: your Link's gear is <painting>
  now (...)` when it draws a painting (coop_look_tunic.cpp Step, drawn != 0); the painting is CoopLookParts::Name:
  `gear...` ending `.hair-RRGGBB` (upper-case hex) when the hair is set (coop_look_parts.cpp Name: the hair piece last).
  Unset, the name is `gear` (kBase); that line is not waited for (whether the way back draws is not proven): the
  puppet's line and the picture judge the way back.
- Puppets: `coop: look: the puppets' boots are <name> now` (coop_look_live.cpp FollowBoots, on a change only); the name
  is CoopLookParts::BootsPaintingName: `boots[.<L>RRGGBB...]` then `.<L>-hair-RRGGBB` per tunic letter L.
- A is player 1, B player 2: tunics 0x3070FF (blue) / 0xFF3030 (red) (coop/protocol messages.hpp slot_color); only the
  pixels that change are judged (huecheck), so a red hair beside B's red tunic is still told apart.
- Colours: RED 0xDD4527 and BLUE 0x4F8AE1 (reports/hair-link.md test plan: Character Select's Red and Blue hair as
  sRGB), far from the game's blond hair and from each other on the colour wheel.
- Colour: huecheck.changed_hue (vest-link's, ingame/huecheck.py, imported, not copied).
- Mask: the Skull Mask lines of test_rows.test_masks_on_puppet (give / wear head / dress head, equips.head).
- Adult: the bridge's `warp KokiriForest_Adult` on a child Link sets Sequence_IsChildhood first (docs/playtest.md
  `warp`) and goes by the map change event (not Longshot's teleport, so no 8 s gap); WARP_GAP frames run between.
- No Character Select: the ONE no-CS lane game, ingame/no_cs.py (MARK + prove()).
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import huecheck     # noqa: E402  (vest-link's colour check)
import no_cs        # noqa: E402  (the ONE no-Character-Select game)
try:
    import solo     # noqa: E402  (the ONE solo game: harness-c133 item 7, once it lands)
except ImportError:
    solo = None
from test_gear_link import face_each_other   # noqa: E402  (the shared set-up helpers)
from test_rows import need_child_in_play     # noqa: E402

RED, BLUE = 0xDD4527, 0x4F8AE1
SKULL = "Armor_Child_Mask_Skull"   # (test_rows.test_masks_on_puppet)

OWN_HAIR = r"coop: look: your Link's gear is gear\S*\.hair-%06X now"
PUP_HAIR = r"coop: look: the puppets' boots are boots\S*\.[A-Z]-hair-%06X\b\S* now"
PUP_NO_HAIR = r"coop: look: the puppets' boots are (?!\S*-hair-)\S+ now"

# The colour check's thresholds are starting values, NOT MEASURED (as test_vest_link.py; worker-rules: no guessed
# values): MIN_CHANGED is the floor under which the hair was clearly not repainted (a FAIL), MIN_SHARE the share of the
# changed pixels in the hair's hue, SAME_SSIM the unset-again vs before picture. Until MEASURED cites a lane run's
# huecheck lines (red, blue, and an unchanged pair for the idle-sway floor), a run whose checks all hold ends NO-REF
# (pair.unproven), never PASS, and a share under MIN_SHARE is noted, not failed. The hair is smaller on screen than
# the vest: these may well need to go down once measured.
MIN_CHANGED, MIN_SHARE, SAME_SSIM = 20, 0.5, 0.90
MEASURED = None   # e.g. "run <name>: red changed .. share .., blue .., unchanged pair changed .."
WARP_GAP = 30     # frames between the two age warps (an age warp goes by the map change event, not Longshot's 8 s teleport gap: docs/playtest.md warp; review-rows-c130-2 nit)


def set_hair(pair, x, value):
    r = pair.act(x, "menu look hair=%s" % value)
    assert r.get("ok", True), "%s refused 'menu look hair=%s': %s" % (x, value, r)


def wait_since(pair, x, pattern, mark, max_frames):
    return pair.until("%s log /%s/" % (x, pattern), lambda: pair.log[x].find(pattern, pos=mark), max_frames)


def wear(pair, who, colour):
    """`who` wears the Hair `colour` (None: unset); waits for its own Link's line (set only) and the puppet's line on
    the other game."""
    other = "B" if who == "A" else "A"
    marks = {x: pair.log[x].mark() for x in ("A", "B")}
    set_hair(pair, who, "none" if colour is None else "%06X" % colour)   # (no '#': the bridge refuses it in an act line)
    if colour is not None:
        wait_since(pair, who, OWN_HAIR % colour, marks[who], 240)
    pup = PUP_NO_HAIR if colour is None else PUP_HAIR % colour
    wait_since(pair, other, pup, marks[other], 600)   # (the look over the wire, then the puppets' boots' round)
    pair.step_both(30)


def shots(pair, name, twice=False):
    """Each game's shot; `twice`: a second one 6 frames later (the puppet animated, not a frozen picture), on each
    test's first coloured step only (the suite keeps a test to a few shots: test_ingame Suite, at most 8)."""
    first = {x: pair.check_shot(x, name)["path"] for x in ("A", "B")}
    if twice:
        pair.step_both(6)
        for x in ("A", "B"):
            pair.check_shot(x, name + "-2")
    return first


def note(pair, line):
    print(line)
    pair.notes.append(line)


def hue(pair, before, after, colour, what, low):
    """Each game's changed_hue from `before` to `after`: under MIN_CHANGED is a FAIL (the hair was not repainted); a
    share under MIN_SHARE goes to `low` (judged only once MEASURED)."""
    for x in ("A", "B"):
        h = huecheck.changed_hue(before[x], after[x], colour)
        note(pair, "huecheck %s %s #%06X changed=%d coloured=%d share=%.3f" % (
            x, what, colour, h["changed"], h["coloured"], h["share"]))
        assert h["changed"] >= MIN_CHANGED, "%s %s: only %d pixels changed: the hair was not repainted %s" % (
            x, what, h["changed"], h)
        if h["share"] < MIN_SHARE:
            low.append("%s %s: %.0f%% of the changed pixels are #%06X's hue (want %.0f%%)" % (
                x, what, h["share"] * 100, colour, MIN_SHARE * 100))


def judged(pair, low):
    if not MEASURED:
        pair.unproven("the hair colour thresholds are not measured yet (MEASURED: the huecheck lines of a lane run)%s"
                      % ("; low: " + "; ".join(low) if low else ""))
    assert not low, "; ".join(low)


def unset_both(pair):
    for x in ("A", "B"):
        set_hair(pair, x, "none")
    pair.step_both(60)


def put_back(pair, done, also=()):
    """Both hairs unset (and `also`, more put-backs) for the lane's next test; a failure here is raised only when the
    test itself passed (else it would hide the test's own FAIL), and noted otherwise."""
    for fn in (lambda: unset_both(pair),) + tuple(also):
        try:
            fn()
        except Exception as e:   # noqa: BLE001
            if done:
                raise
            pair.notes.append("put back: %s" % e)


def mask(pair, on):
    """A's Skull Mask on / off (test_rows.test_masks_on_puppet's lines: 'wear' writes EquipHeadArmor, 'dress head' is
    the game's own equip call)."""
    pair.act("A", "wear head %s" % (SKULL if on else "none"))
    pair.act("A", "dress head")
    want = SKULL if on else None
    pair.until("A's own equips.head is %s" % want, lambda: pair.state("A")["equips"]["head"] == want, 120)
    pair.step_both(20)


# Budgets: ESTIMATED (not measured), from the waits: set-up < 30 s, each wear() at most 240 + 600 + 30 frames
# (~29 s at 30 fps), two shots a step and the put-back; the adult test also two warps per game and WARP_GAP.
@pytest.mark.budget(240)   # ESTIMATED
@pytest.mark.look
@pytest.mark.row("#293 P1 Link's Hair colour (own Link + puppet)")
def test_hair_own_and_puppet(pair):
    with pair.setup():
        need_child_in_play(pair)
        face_each_other(pair)
        unset_both(pair)
    done, low = False, []
    try:
        before = shots(pair, "hair-link-unset")
        wear(pair, "A", RED)
        red = shots(pair, "hair-link-red", twice=True)
        hue(pair, before, red, RED, "unset->red", low)
        wear(pair, "A", BLUE)
        blue = shots(pair, "hair-link-blue")
        hue(pair, red, blue, BLUE, "red->blue", low)
        done = True
    finally:
        put_back(pair, done)
    judged(pair, low)


@pytest.mark.budget(240)   # ESTIMATED
@pytest.mark.look
@pytest.mark.row("#293 P1 Link's Hair unset = the game's hair (0.11.12)")
def test_hair_unset_is_game(pair):
    with pair.setup():
        need_child_in_play(pair)
        face_each_other(pair)
        unset_both(pair)
    done, low = False, []
    try:
        before = shots(pair, "hair-link-unset")
        wear(pair, "A", RED)
        coloured = shots(pair, "hair-link-unset-red")
        wear(pair, "A", None)
        back = shots(pair, "hair-link-unset-again")
        for x in ("A", "B"):
            score, err = imagecheck_ssim(before[x], back[x])
            # (only the pixels the wear painted: a whole-frame count took the torch flames as red left, B #148)
            h = huecheck.left_after_unset(before[x], coloured[x], back[x], RED)
            note(pair, "huecheck %s unset-again #%06X wear=%d left=%d share=%.3f ssim=%s" % (
                x, RED, h["wear"], h["left"], h["share"], score))
            assert h["wear"] >= MIN_CHANGED, "%s: only %d pixels took the red: the hair was not repainted %s" % (
                x, h["wear"], h)
            assert h["share"] < huecheck.LEFT_SHARE, "%s: red left on the hair after unset %s" % (x, h)
            if err or score < SAME_SSIM:
                low.append("%s: unset again vs before SSIM %s %s (want >= %.2f)" % (x, score, err or "", SAME_SSIM))
        done = True
    finally:
        put_back(pair, done)
    judged(pair, low)


def imagecheck_ssim(a, b):
    import imagecheck   # (the shared compare's SSIM: the same measure as every look ref)
    return imagecheck.ssim(a, b)


@pytest.mark.budget(240)   # ESTIMATED
@pytest.mark.look
@pytest.mark.row("#293 P1 Link's Hair colour under the Skull Mask")
def test_hair_under_mask(pair):
    with pair.setup():
        need_child_in_play(pair)
        pair.give("A", "PouchData.%s.StockNum" % SKULL)
        face_each_other(pair)
        unset_both(pair)
    done, low = False, []
    try:
        mask(pair, True)
        pair.expect_log("B", r"P1 wears head '%s'" % SKULL, max_frames=300)   # (test_masks_on_puppet's expect)
        before = shots(pair, "hair-link-mask-unset")
        wear(pair, "A", RED)
        red = shots(pair, "hair-link-mask-red", twice=True)
        hue(pair, before, red, RED, "mask unset->red", low)
        done = True
    finally:
        put_back(pair, done, also=(lambda: mask(pair, False),))
    judged(pair, low)


@pytest.mark.budget(300)   # ESTIMATED
@pytest.mark.look
@no_cs.MARK
@pytest.mark.row("#293 P1 Link's Hair without Character Select (C #133)")
def test_hair_without_character_select(pair):
    with pair.setup():
        need_child_in_play(pair)
        no_cs.prove(pair)
        face_each_other(pair)
        unset_both(pair)
    done, low = False, []
    try:
        before = shots(pair, "hair-link-no-cs-unset")
        wear(pair, "A", RED)    # A (with the mod) -> its puppet in the CS-less B
        a_red = shots(pair, "hair-link-no-cs-a-red")
        hue(pair, before, a_red, RED, "A unset->red", low)
        wear(pair, "A", None)
        unset = shots(pair, "hair-link-no-cs-unset-again")
        wear(pair, "B", BLUE)   # B's own Link without the mod -> its puppet in A (same places: A faces B already)
        b_blue = shots(pair, "hair-link-no-cs-b-blue")
        hue(pair, unset, b_blue, BLUE, "B unset->blue", low)
        done = True
    finally:
        put_back(pair, done)
    judged(pair, low)


def to_age(pair, scene, age):
    pair.warp_both(scene)
    pair.until_in_play("both games in play after warp %s" % scene)   # (a stale sample right after the load)
    for x in pair.c:
        s = pair.state(x)
        assert s.get("age") == age, "%s is %s after warp %s (want %s)" % (x, s.get("age"), scene, age)


@pytest.mark.budget(360)   # ESTIMATED (two warps per game + WARP_GAP)
@pytest.mark.look
@pytest.mark.row("#293 P1 Link's Hair colour on adult Link (own Link + adult puppet copy)")
def test_hair_adult(pair):
    """Last in the file: its put-back warps both games back to the kit save's child scene."""
    with pair.setup():
        need_child_in_play(pair)
        unset_both(pair)
    done, low = False, []

    def child_again():
        pair.step_both(WARP_GAP)
        to_age(pair, "KokiriForest_Child", "child")

    try:
        to_age(pair, "KokiriForest_Adult", "adult")   # (in the test, not the set-up: two warps outrun its 30 s)
        face_each_other(pair)
        before = shots(pair, "hair-link-adult-unset")
        wear(pair, "A", RED)
        red = shots(pair, "hair-link-adult-red", twice=True)
        hue(pair, before, red, RED, "adult unset->red", low)
        done = True
    finally:
        put_back(pair, done, also=(child_again,))
    judged(pair, low)


# Until harness-c133's solo.py lands: a switch no run gives (no game knows it), so the test is always deselected.
SOLO_PENDING = "A:COOP_TEST_SOLO_BOOT_NOT_IN_THE_HARNESS_YET=1"
SOLO_MARK = solo.MARK if solo else pytest.mark.needs_env(SOLO_PENDING)


@pytest.mark.budget(180)   # ESTIMATED (one wear on one game, two shot steps)
@pytest.mark.look
@SOLO_MARK
@pytest.mark.row("#293 P1 Link's Hair colour solo (no co-op session)")
def test_hair_solo(pair):
    """Owner rule: a non-multiplayer feature works solo. Game A alone, co-op idle: its own Link's hair is painted
    (coop_look_live.cpp Wear -> g_gear->Wear, which does not look at the session). Only A's own line and A's shots."""
    with pair.setup():
        solo.prove(pair)
        pair.need_boot_place()   # (stepped until in play, never one read; pair.c = {'A'} in a --solo run)
        set_hair(pair, "A", "none")
        pair.step_both(60)
    done, low = False, []
    try:
        before = pair.check_shot("A", "hair-link-solo-unset")["path"]
        mark = pair.log["A"].mark()
        set_hair(pair, "A", "%06X" % RED)
        wait_since(pair, "A", OWN_HAIR % RED, mark, 240)
        pair.step_both(30)
        red = pair.check_shot("A", "hair-link-solo-red")["path"]
        h = huecheck.changed_hue(before, red, RED)
        note(pair, "huecheck A solo unset->red #%06X changed=%d coloured=%d share=%.3f" % (
            RED, h["changed"], h["coloured"], h["share"]))
        assert h["changed"] >= MIN_CHANGED, "A solo: only %d pixels changed: the hair was not repainted %s" % (
            h["changed"], h)
        if h["share"] < MIN_SHARE:
            low.append("A solo: %.0f%% of the changed pixels are #%06X's hue" % (h["share"] * 100, RED))
        done = True
    finally:
        try:
            set_hair(pair, "A", "none")
            pair.step_both(60)
        except Exception as e:   # noqa: BLE001
            if done:
                raise
            pair.notes.append("put back: %s" % e)
    judged(pair, low)
