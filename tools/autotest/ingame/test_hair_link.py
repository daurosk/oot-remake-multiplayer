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
- Colour: huecheck.solid_count (ingame/huecheck.py, hair-check-0121): the pixels of the colour in SOLID patches inside
  the Link's own screen box (bandcheck.box from the same frame's view.clip and the Link's pos: own Link on its game,
  test_tunic.puppet_at's puppet on the other). The whole-frame changed_hue judged the camera, the windmill, the flames
  and brown hair (hair-link-309d7180: 6 FAILs, all test bugs).
- No black shots: before each shot set the games are stepped until both pictures are lit (huecheck.lit; the adult
  shots of hair-link-309d7180 were a load's black frames).
- Mask: the Skull Mask lines of test_rows.test_masks_on_puppet (give / wear head / dress head, equips.head).
- Adult: the bridge's `warp KakarikoVillage_Adult` (test_look.py's proven adult warp; the boot's Kakariko by day, where
  KokiriForest_Adult gave dark shots) on a child Link sets Sequence_IsChildhood first (docs/playtest.md
  `warp`) and goes by the map change event (not Longshot's teleport, so no 8 s gap); WARP_GAP frames run between.
- No Character Select: the ONE no-CS lane game, ingame/no_cs.py (MARK + prove()).
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bandcheck    # noqa: E402  (a Link's screen box from view.clip)
import game_names   # noqa: E402  (the ONE item name table)
import huecheck     # noqa: E402  (vest-link's colour check)
import no_cs        # noqa: E402  (the ONE no-Character-Select game)
try:
    import solo     # noqa: E402  (the ONE solo game: harness-c133 item 7, once it lands)
except ImportError:
    solo = None
from test_gear_link import face_each_other   # noqa: E402  (the shared set-up helpers)
from test_rows import need_child_in_play     # noqa: E402
from test_tunic import puppet_at             # noqa: E402  (the puppet's place in a game's world list)

RED, BLUE = 0xDD4527, 0x4F8AE1
SKULL = game_names.SKULL_MASK   # (test_rows.test_masks_on_puppet; one table)

OWN_HAIR = r"coop: look: your Link's gear is gear\S*\.hair-%06X now"
PUP_HAIR = r"coop: look: the puppets' boots are boots\S*\.[A-Z]-hair-%06X\b\S* now"
PUP_NO_HAIR = r"coop: look: the puppets' boots are (?!\S*-hair-)\S+ now"

# The colour check's thresholds (huecheck.solid_count, counts scaled to a 1280-wide shot). From the STORED
# hair-link-309d7180 lane-1 shots with hand-drawn boxes (huecheck.py's MEASURED block; the hair-link-9 report):
#   GAIN: the least a real wear added was B's masked puppet 31 (red 91, blue in the patch 25); a shot pair with no
#   wear added at most 27 (A unset -> unset again). GAIN[RED] 30, GAIN[BLUE] 15 (no-wear blue in the patch: 0-9).
#   LEFT_HAIR: of the red a wear added, what the unset left: A 0.046, B 0.0; the same red 6 frames later (the
#   still-red control): A 1.0, B 1.03. 0.3 sits between (6x the most left, a third of the least kept).
# Those boxes were drawn by hand; the test's own boxes come from view.clip. Until MEASURED cites the huecheck lines of a
# lane run of THIS check, a run whose checks all hold ends NO-REF (pair.unproven), never PASS, and a count under GAIN is
# noted, not failed. Always a FAIL: a wear that added nothing (the hair was not repainted), red left at/over LEFT_HAIR.
GAIN = {RED: 30, BLUE: 15}
LEFT_HAIR = 0.3
SAME_SSIM = 0.90
MEASURED = None   # e.g. "run <name>: red gain A .. B .., blue .., unset left .., still-red control .."
WARP_GAP = 30     # frames between the two age warps (an age warp goes by the map change event, not Longshot's 8 s teleport gap: docs/playtest.md warp; review-rows-c130-2 nit)
LIT_FRAMES = 300  # frames the games are stepped at most for a lit picture (a load's fade is well under it; else SETUP-FAIL-like AssertionError)
ADULT = "KakarikoVillage_Adult"   # (test_look.py / test_gear.py's adult warp, in the boot's Kakariko)


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


def games(pair):
    return tuple(x for x in ("A", "B") if x in pair.c)


def wait_lit(pair):
    """Steps the games until each one's picture is lit (huecheck.lit on a plain bridge shot): never judge a load's black
    frame (hair-link-309d7180 adult-red-A: every pixel black)."""
    pair.until("a lit picture on %s (not a load's black frame)" % "+".join(games(pair)),
               lambda: all(huecheck.lit(pair.call(x, "shot", name="hair-link-lit-%s" % x)["path"]) for x in games(pair)),
               LIT_FRAMES)


def link_box(pair, x, who, path):
    """`who`'s Link on game x's shot at `path`: the box of its own Link (x == who) or of its puppet (test_tunic.puppet_at
    near who's place), from x's view.clip of the same frame (no frame passes between the state and the shot)."""
    from PIL import Image
    s = pair.state(x)
    at = pair.state(who)["pos"]
    pos = at if x == who else puppet_at(pair, x, at)
    assert pos, "%s: no puppet of %s near %s in the world list" % (x, who, at)
    assert isinstance(s.get("view"), dict), "%s: no camera (view) in the state" % x
    with Image.open(path) as im:
        w, h = im.size
    b = bandcheck.box(s["view"], pos, w, h)
    assert b, "%s: %s's Link is off %s's shot (pos %s)" % (x, who, x, pos)
    return b


def shots(pair, name, who="A", twice=False):
    """Each game's shot (pictures lit first) and `who`'s Link's box on it: {x: {'path', 'box'}}; `twice`: also a
    second shot 6 frames later ('path2', 'box2': the puppet animated, and the still-colour control)."""
    wait_lit(pair)
    out = {}
    for x in games(pair):
        if x != who:   # (the puppet in x's world list first: the state and shot then come from the same frame)
            at = pair.state(who)["pos"]
            pair.until("%s's puppet of %s" % (x, who), lambda: puppet_at(pair, x, at), max_frames=120)
        path = pair.check_shot(x, name)["path"]
        out[x] = {"path": path, "box": link_box(pair, x, who, path)}
    if twice:
        pair.step_both(6)
        for x in games(pair):
            path = pair.check_shot(x, name + "-2")["path"]
            out[x].update(path2=path, box2=link_box(pair, x, who, path))
    return out


def note(pair, line):
    print(line)
    pair.notes.append(line)


def count(shot, colour, second=False, within=None):
    k = "2" if second else ""
    return huecheck.solid_count(shot["path" + k], shot["box" + k], colour, within)


def gained(pair, before, after, colour, what, low, games_=None, within=None):
    """Each game's solid `colour` count in the Link's box, before -> after: nothing added is a FAIL (the hair was not
    repainted); less than GAIN goes to `low` (judged only once MEASURED). `within`: {x: mask} (the hair patch)."""
    out = {}
    for x in games_ or tuple(before):
        m = (within or {}).get(x)
        n0, n1 = count(before[x], colour, within=m), count(after[x], colour, within=m)
        note(pair, "huecheck %s %s #%06X before=%d after=%d gain=%d box=%s" % (
            x, what, colour, n0, n1, n1 - n0, list(after[x]["box"])))
        assert n1 - n0 > 0, "%s %s: no #%06X added on the Link: the hair was not repainted (before %d, after %d)" % (
            x, what, colour, n0, n1)
        if n1 - n0 < GAIN[colour]:
            low.append("%s %s: #%06X gained %d (want %d)" % (x, what, colour, n1 - n0, GAIN[colour]))
        out[x] = (n0, n1)
    return out


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
    the game's own equip call). An empty slot reads "" or None (hair-link-6: the module gives "")."""
    pair.act("A", "wear head %s" % (SKULL if on else "none"))
    pair.act("A", "dress head")
    pair.until("A's own equips.head is %s" % (SKULL if on else "empty"),
               lambda: (pair.state("A")["equips"]["head"] or None) == (SKULL if on else None), 120)
    pair.step_both(20)


# Budgets: ESTIMATED (not measured), from the waits: set-up < 30 s, each wear() at most 240 + 600 + 30 frames
# (~29 s at 30 fps), the lit wait (at most LIT_FRAMES) and two shots a step and the put-back; the adult test also two
# warps per game and WARP_GAP.
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
        n = gained(pair, before, red, RED, "unset->red", low)
        patch = {x: huecheck.hair_patch(red[x]["path"], red[x]["box"], RED) for x in red}
        wear(pair, "A", BLUE)
        blue = shots(pair, "hair-link-blue")
        gained(pair, red, blue, BLUE, "red->blue", low, within=patch)   # (in the red hair's patch: Navi is not hair)
        for x in blue:
            gone = count(blue[x], RED)
            note(pair, "huecheck %s red->blue red left=%d of %d" % (x, gone, n[x][1]))
            assert gone < LEFT_HAIR * max(1, n[x][1]), "%s: the red stayed on the hair after blue (%d of %d)" % (
                x, gone, n[x][1])
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
        coloured = shots(pair, "hair-link-unset-red", twice=True)
        n = gained(pair, before, coloured, RED, "unset->red", low)
        wear(pair, "A", None)
        back = shots(pair, "hair-link-unset-again")
        blind = []
        for x in back:
            n0, n1 = n[x]
            added = max(1, n1 - n0)
            left = (count(back[x], RED) - n0) / float(added)
            still = (count(coloured[x], RED, second=True) - n0) / float(added)   # (the control: red 6 frames on)
            score, err = imagecheck_ssim(before[x]["path"], back[x]["path"])
            note(pair, "huecheck %s unset-again #%06X added=%d left=%.3f still-red=%.3f ssim=%s" % (
                x, RED, n1 - n0, left, still, score))
            assert left < LEFT_HAIR, "%s: red left on the hair after unset (%.3f of the red it added, want < %.2f)" % (
                x, left, LEFT_HAIR)
            if still < LEFT_HAIR:
                blind.append("%s: the check did not see the red 6 frames on either (%.3f)" % (x, still))
            if err or score < SAME_SSIM:
                low.append("%s: unset again vs before SSIM %s %s (want >= %.2f)" % (x, score, err or "", SAME_SSIM))
        done = True
    finally:
        put_back(pair, done)
    if blind:   # (a check that cannot see a stuck colour proves no unset: never PASS)
        pair.unproven("; ".join(blind))
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
        gained(pair, before, red, RED, "mask unset->red", low)
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
        gained(pair, before, a_red, RED, "A unset->red", low)
        wear(pair, "A", None)
        unset = shots(pair, "hair-link-no-cs-unset-again", who="B")
        wear(pair, "B", BLUE)   # B's own Link without the mod -> its puppet in A (same places: A faces B already)
        b_blue = shots(pair, "hair-link-no-cs-b-blue", who="B")
        gained(pair, unset, b_blue, BLUE, "B unset->blue", low)
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
    """Last in the file: its put-back warps both games back to the boot's scene as children (pair.boot_scene: where
    every default-boot test starts; the old put-back went to KokiriForest_Child, not the boot's place)."""
    with pair.setup():
        need_child_in_play(pair)
        unset_both(pair)
        home = pair.boot_scene
        assert home and home.endswith("_Child"), "the boot's scene %r is no child scene to come back to" % home
    done, low = False, []

    def child_again():
        pair.step_both(WARP_GAP)
        to_age(pair, home, "child")

    try:
        to_age(pair, ADULT, "adult")   # (in the test, not the set-up: two warps outrun its 30 s)
        face_each_other(pair)
        before = shots(pair, "hair-link-adult-unset")
        wear(pair, "A", RED)
        red = shots(pair, "hair-link-adult-red", twice=True)
        gained(pair, before, red, RED, "adult unset->red", low)
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
        before = shots(pair, "hair-link-solo-unset")   # (games(pair) = ('A',) in a --solo run)
        mark = pair.log["A"].mark()
        set_hair(pair, "A", "%06X" % RED)
        wait_since(pair, "A", OWN_HAIR % RED, mark, 240)
        pair.step_both(30)
        red = shots(pair, "hair-link-solo-red")
        gained(pair, before, red, RED, "solo unset->red", low)
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
