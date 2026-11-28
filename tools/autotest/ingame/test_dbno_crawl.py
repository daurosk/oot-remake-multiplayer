"""Row 44 / issue #337 (down but not out: a downed player crawls), ported from the hs-dbno-* rows (crawl-337-9).

| Test | Old row (scenario files) | Kind |
|---|---|---|
| test_downed_child_crawls_and_is_revived | hs-dbno-child + offline/crawl-337 crawl-337-revive | log + positions + LOOK |
| test_downed_charsel_puppet_lies | offline/crawl-337 crawl-337-charsel (review-c122 point 4) | log + LOOK |
| test_downed_adult_lies_bleeds_out_respawns | hs-dbno-adult (rows 113 / 190) | log + positions + LOOK |

The fix under test: crawl-337-7 a58eb7720 + 6e35681f4 (lichii/hook/coop_dbno_body.cpp), in integrate-0120.
B is the one downed, A (P1, the host: it joins first) changes the rules and revives. Every log pattern is a line the
module writes (file:line cited where it is used); every key, place and number is cited where it is used.
Each test puts the lane back the way the rows expect it (Down but not out on, 45 s, child, the Kakariko boot's spot)
whatever happens in it (`_restore`), so a FAIL here never turns the next row into a SETUP-FAIL.
"""
import contextlib
import math
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import hookshot   # noqa: E402  (BOTTLE_ITEMS: the ONE list of bottle Enums, as the scripts' `nofairy` uses it)
import playtest_client as pc   # noqa: E402  (REVIVE_BUTTON: the revive button, coop_dbno.h pad::revive)

# lichii/hook/coop_dbno.h:276 enum class Event: went_down 0, revived 1, bled_out 2, ..., respawned 7; logged as
# "coop: dbno: our Link: event N (by PM)" (lichii/hook/coop_dbno_game.cpp:523).
EV_DOWN, EV_REVIVED, EV_BLED_OUT, EV_RESPAWNED = 0, 1, 2, 7
REACH_M = 2.5          # lichii/hook/coop_dbno.h kReviveReachM (level distance, feet to feet)
BESIDE_M = 1.3         # A beside B for the revive: crawl-337-revive.A.txt (hs-dbno-child run 20261125-145208: 1.3 m)
HIT = 4                # 'hurt 4' = one heart (tools/autotest/hookshot.py: hurt QUARTER_HEARTS 1 to 80), the rows' hit
HIT_GAP = 60           # frames between hits: hs-dbno-child.B.txt / hs-dbno-adult.B.txt 'wait 60'
DEFAULT_BLEED_S = 45   # coop/protocol messages.hpp DbnoMode bleed_s default: what the other rows play with
CHARSEL = "CharacterMenu_SelectedCharacter"   # Character Select's Int (char-npc-v022.A.txt:9)
CHARSEL_PICK = 7       # the NPC character char-npc-v022.A.txt / crawl-337-charsel.B.txt pick
BOOT = ("KakarikoVillage_Child", "Kakariko_WellSide")   # the default boot (scenarios/live-kakariko-child.A.txt:7)
RESTORE_S = 120        # the restore's own deadline after a failure (two warps at most)
# The respawn moves B by the /teleport trip (coop_dbno_game.cpp:546-552 CoopTeleport::Beside), so "beside A" is judged
# as test_rows.py test_teleport_to_player judges /teleport's arrival: < 3.0 m, same scene.
BESIDE_AFTER_M = 3.0
# Crawl minimum: the test's own (no old row measured it; hs-dbno-child judged the crawl by shots only): 0.1 m in 120
# frames of full stick only says "the stick moves the downed Link at all"; the first lane run's value is the measure.
CRAWL_MIN_M = 0.1

# The downed Link's own lines (lichii/hook/coop_dbno_body.cpp, logged as "coop: dbno: <note>", coop_dbno_game.cpp:453)
LYING_CHILD = (r"coop: dbno: our downed Link: (the game's own crawl runs \('|.*: the game's AS 'Crouch' \(Tunnel_Wait, "
               r"its lying wait\) is held on slot 0 for the rest of this down)")           # :477 / :207
LYING_ADULT = r"coop: dbno: our downed adult Link: the game's AS 'DamageLStand' held at frame 0"   # :660
CRAWL_REFUSED = r"coop: dbno: our downed Link: Crawl \(234\) refused"                           # :701
UP_LINE = r"coop: dbno: our Link up: "                                                          # :668 :707 :719 :839
UP_REFUSED = r"coop: dbno: our Link up: .* refused"                                             # :712
CRAWL_TAKEN = r"coop: dbno: our downed Link: Crawl \(234\) asked and taken"                   # :686
CRAWL_WAITS = r"coop: dbno: our downed Link: Crawl \(234\) waits: the slot took a request"     # :693
ADULT_UP = r"coop: dbno: our Link up: LDamageStandUp \(85\) asked and taken"                   # :719
ADULT_UP_WAIT = r"coop: dbno: our Link up: LDamageStandUp \(85\) refused \(\d+\): the game's own Wait stands"  # :712


def event(n):
    return r"coop: dbno: our Link: event %d \(by P\d+\)" % n


def count(pair, x, pattern):
    rx = re.compile(pattern)
    return sum(1 for ln in pair.log[x].since() if rx.search(ln))


def same_scene(pair, x, scene, when):
    """hs-dbno-adult.A.txt check: B still in the scene at every mark (no reload)."""
    st = pair.state(x)
    same = (st.get("scene") or "").lower() == scene.lower() and not st.get("loading")
    assert same, "%s left %s %s (scene %s, loading %s): a reload" % (x, scene, when, st.get("scene"), st.get("loading"))


def dist(a, b):
    return math.dist(a[::2], b[::2])


def need_at(pair, age, scene):
    """Both games in play in `scene` as `age` Link after the warp: STEPPED until true (pair.until_in_play, then
    pair.until on scene + age), never judged from one read (B #139 16:31: a one-read first set-up is the
    gifts-237/row53 'not in play' SETUP-FAIL class; harness-c133-scene pair.need_boot_place does the same for the
    boot's place). AssertionError naming each game's scene/age after the frame bound."""
    pair.until_in_play("both games in play in %s" % scene)

    def there():
        return all(pair.in_play(st) and (st.get("scene") or "").lower() == scene.lower() and st.get("age") == age
                   for st in (pair.state(x) for x in ("A", "B")))
    try:
        pair.until("both games in %s as %s Link" % (scene, age), there)
    except AssertionError as e:
        got = "; ".join("%s in %s, %s" % (x, pair.state(x).get("scene"), pair.state(x).get("age")) for x in ("A", "B"))
        raise AssertionError("%s; %s: this row needs %s Link in %s" % (e, got, age, scene)) from None


def to_place(pair, age, scene, entrance):
    """Both games to the old row's place and age: 'setflag Sequence_IsChildhood 1|0' then the warp
    (hs-dbno-child/-adult A/B.txt:1-2; docs/playtest.md `warp`: the lane recipe sets the flag, 30 frames, then warps)."""
    for x in ("A", "B"):
        pair.flag(x, "Sequence_IsChildhood", 1 if age == "child" else 0)
    pair.step_both(30)
    pair.warp_both(scene, entrance)
    pair.step_both(30)
    need_at(pair, age, scene)


def rules(pair, bleed_s):
    """A (the host) sets the bleed-out ('chat /dbno time N': hs-dbno-child.A.txt; coop_dbno.cpp Parse); B hears it
    (coop_dbno_game.cpp:255 'coop: dbno: on (bleed N s, hold T tenths, L quarter hearts; set by P)'). Returns T."""
    pair.act("A", "chat /dbno time %d" % bleed_s)
    line = pair.expect_log("B", r"coop: dbno: on \(bleed %d s, hold \d+ tenths" % bleed_s, max_frames=300)
    return int(re.search(r"hold (\d+) tenths", line).group(1))


def no_fairy(pair, x):
    """The scripts' `nofairy` (hookshot.py BOTTLE_ITEMS: every bottle's item set to Empty): a bottled fairy makes a
    lethal hit the game's own death and revive, never a down (3 VOID runs, 2026-11-20). Proven by the module's own
    note (coop_dbno_game.cpp:600): its LAST fairy note in the whole log must not be 'a fairy in our bottle'."""
    for item in hookshot.BOTTLE_ITEMS:
        pair.act(x, "setenum %s Empty" % item)
    pair.step_both(30)
    notes = [ln for ln in pair.log[x].since(0) if "fairy in our bottle" in ln or "no fairy in our bottles" in ln]
    assert not notes or "no fairy in our bottles" in notes[-1], "%s: still a fairy in a bottle: %s" % (x, notes[-1])


def down(pair, x="B"):
    """The game's own hits ('hurt 4', 60 frames apart: the rows' B.txt) until x is downed (the guard holds the lethal
    one while A is up, coop_dbno.h LifeGuard). At most life/4 + 2 hits, the life read from the state."""
    life = pair.state(x).get("life") or 0
    hits = life // HIT + 2
    for _ in range(hits):
        pair.act(x, "hurt %d" % HIT)
        pair.step_both(HIT_GAP)
        if pair.log[x].find(event(EV_DOWN)):
            break
    assert pair.log[x].find(event(EV_DOWN)), "%s: not downed after %d hits of %d (life was %d)" % (x, hits, HIT, life)
    pair.expect_log("A", r"coop: dbno: P2 is down", max_frames=300)        # coop_dbno_game.cpp:272


def stick_move(pair, x, angle, frames):
    """x's left stick held (bridge `input` stick [angle, strength]: 0 = up/forward, 90 = right; the rows' 'stick 0 1 N'
    / 'stick 1 0 N'), returns the metres moved on the ground."""
    a = pair.state(x)["pos"]
    pair.press(x, stick=[angle, 1.0], frames=frames)
    b = pair.state(x)["pos"]
    return dist(a, b)


def shots(pair, name, twice=True):
    """A bridge shot on each game; with twice, again 6 frames later (the crawl is an animation). Tolerance 0.80:
    moving people in an open field (docs/ingame-tests.md step 6). At most 8 shots per test (tests/test_ingame.py)."""
    for x in ("A", "B"):
        pair.check_shot(x, "%s-1" % name, tolerance=0.80)
    if twice:
        pair.step_both(6)
        for x in ("A", "B"):
            pair.check_shot(x, "%s-2" % name, tolerance=0.80)


def revive(pair, hold_tenths, max_frames=1800):
    """A next to the lying B (BESIDE_M, inside REACH_M) holds the revive button (pc.REVIVE_BUTTON, B; coop_dbno_game.cpp 'Beside a downed teammate:
    hold the revive button'; the Reviver sends a hold every 200 ms, the downed game counts it 700 ms: coop_dbno.h kHoldSendUs /
    kHoldLiveUs). Held in 10-frame turns of each game (press runs A, then B the same frames) so neither game stops
    hearing the other for long. The revive takes hold_tenths of B's clock (coop_dbno.cpp Machine: held_us_)."""
    b = pair.state("B")["pos"]
    pair.act("A", "place %.2f %.2f %.2f" % (b[0] + BESIDE_M, b[1], b[2]))
    pair.act("A", "face %.2f %.2f" % (b[0], b[2]))
    pair.step_both(10)
    a = pair.state("A")["pos"]
    assert dist(a, pair.state("B")["pos"]) < REACH_M, "A not beside B (%.2f m)" % dist(a, pair.state("B")["pos"])
    ran = 0
    while not pair.log["B"].find(event(EV_REVIVED)):
        assert ran < max_frames, "B not revived after A held A %d frames (hold is %d tenths)" % (ran, hold_tenths)
        pair.press("A", [pc.REVIVE_BUTTON], frames=10)
        ran += 10
    pair.expect_log("A", r"coop: dbno: P2 is up", max_frames=300)          # coop_dbno_game.cpp:272
    return ran


def _restore(pair, charsel_was=None):
    """Back to what the next rows assume, whatever happened: nobody downed, Down but not out on at 45 s, Character
    Select back, child at the Kakariko boot's spot. '/dbno off' stands a downed or watching Link up at once
    (coop_dbno.cpp Machine: connected && !rules.on -> up, event got_up)."""
    pair.act("A", "chat /dbno off")
    pair.step_both(60)
    pair.act("A", "chat /dbno on")
    pair.step_both(30)
    pair.act("A", "chat /dbno time %d" % DEFAULT_BLEED_S)
    pair.step_both(30)
    if charsel_was is not None:
        pair.flag("B", CHARSEL, charsel_was)
    if pair.state("A").get("age") != "child" or pair.state("B").get("age") != "child" or \
            (pair.state("A").get("scene") or "").lower() != BOOT[0].lower():
        to_place(pair, "child", *BOOT)


@contextlib.contextmanager
def restored(pair, keep):
    """Runs the test, then _restore. After a failure a failed restore is only noted (the test's own error stays the
    result); after a pass it is the test's failure. keep: a dict the test fills ('charsel': the value it found)."""
    try:
        yield
    except BaseException:
        pair.deadline = type(pair.deadline)(RESTORE_S, clock=pair.deadline.clock)   # (a timed-out test still restores)
        try:
            _restore(pair, keep.get("charsel"))
        except Exception as e:
            pair.notes.append("restore after the failure failed: %s: %s" % (type(e).__name__, e))
        raise
    _restore(pair, keep.get("charsel"))


@pytest.mark.budget(420)
@pytest.mark.look
@pytest.mark.row("#337 row 44 the downed child crawls, revived by A (hs-dbno-child + crawl-337-revive)")
def test_downed_child_crawls_and_is_revived(pair):
    with restored(pair, {}):
        with pair.setup():
            pair.need_boot_place("child")   # the default boot's place, stepped (harness-c133-scene; boot = BOOT, child)
            to_place(pair, "child", "KokiriForest_Child", "FrontDekuTree")   # hs-dbno-child: open ground, no crawlspace
            hold = rules(pair, 240)     # long: the crawl and the revive come before any bleed-out (240 = kDbnoMaxBleedS)
            no_fairy(pair, "B")
        down(pair)
        pair.expect_log("B", LYING_CHILD, max_frames=600)          # the game's crawl, or its lying wait held
        pair.step_both(30)
        shots(pair, "dbno-child-lying")
        crawl_m = stick_move(pair, "B", 0, 120)                     # hs-dbno-child.B.txt 'stick 0 1 120'
        shots(pair, "dbno-child-crawl", twice=False)
        side_m = stick_move(pair, "B", 90, 90)                      # 'stick 1 0 90'
        pair.notes.append("crawl %.2f m in 120 frames, side %.2f m in 90" % (crawl_m, side_m))
        pair.never_log("B", CRAWL_REFUSED)
        pair.never_log("B", event(EV_BLED_OUT))
        assert crawl_m > CRAWL_MIN_M, "B did not crawl: %.2f m in 120 frames of full stick (downed)" % crawl_m
        held = revive(pair, hold)
        pair.notes.append("revived after A held A %d frames (hold %d tenths)" % (held, hold))
        pair.expect_log("B", UP_LINE, max_frames=300)
        pair.never_log("B", UP_REFUSED)
        pair.never_log("B", event(EV_BLED_OUT))
        pair.step_both(60)
        shots(pair, "dbno-child-revived", twice=False)
        walk_m = stick_move(pair, "B", 0, 90)                       # crawl-337-revive.B.txt 'stick 0 1 90'
        pair.notes.append("walk %.2f m in 90 frames after the revive" % walk_m)
        assert walk_m >= 1.0, "B has no control after the revive: %.2f m in 90 frames (crawl-337-revive: >= 1 m)" % walk_m
        assert crawl_m / 120 < walk_m / 90, "downed B moved as fast as a walk: crawl %.3f m/frame, walk %.3f" % (
            crawl_m / 120, walk_m / 90)


@pytest.mark.budget(420)
@pytest.mark.look
@pytest.mark.row("#337 row 44 a Character Select puppet goes down the same (crawl-337-charsel, review-c122 point 4)")
def test_downed_charsel_puppet_lies(pair):
    keep = {}
    with restored(pair, keep):
        with pair.setup():
            pair.need_boot_place("child")   # the default boot's place, stepped (harness-c133-scene; boot = BOOT, child)
            r = pair.flag_read("B", CHARSEL)
            assert r.get("kind") == "int", "no Character Select on this lane (%s reads %s)" % (CHARSEL, r.get("kind"))
            keep["charsel"] = r.get("value") or 0
            to_place(pair, "child", "KokiriForest_Child", "FrontDekuTree")   # crawl-337-charsel = hs-dbno-child's place
            hold = rules(pair, 239)     # (not 240: a rules line is logged on each change; the test before set 240)
            no_fairy(pair, "B")
            pair.flag("B", CHARSEL, CHARSEL_PICK)
            # coop_look_share.cpp:334: the other game's puppet wears that character (crawl-337-charsel live gate)
            pair.expect_log("A", r"coop: look: P2 wears \d+ part colour\(s\), character %d" % CHARSEL_PICK, 600)
        down(pair)
        pair.expect_log("B", LYING_CHILD, max_frames=600)
        pair.step_both(30)
        shots(pair, "dbno-charsel-lying")                           # A: B's puppet as that character, lying
        crawl_m = stick_move(pair, "B", 0, 120)
        shots(pair, "dbno-charsel-crawl", twice=False)
        pair.notes.append("charsel crawl %.2f m in 120 frames" % crawl_m)
        pair.never_log("B", CRAWL_REFUSED)
        revive(pair, hold)
        pair.expect_log("B", UP_LINE, max_frames=300)
        pair.never_log("B", UP_REFUSED)


@pytest.mark.budget(480)
@pytest.mark.look
@pytest.mark.row("#337 rows 113/190 the downed adult lies, bleeds out, respawns beside A (hs-dbno-adult)")
def test_downed_adult_lies_bleeds_out_respawns(pair):
    with restored(pair, {}):
        with pair.setup():
            pair.need_boot_place("child")   # the default boot's place, stepped (harness-c133-scene; boot = BOOT, child)
            to_place(pair, "adult", "HyruleField_Adult", "HyruleField_FrontOfKakarikoVillage")   # hs-dbno-adult
            rules(pair, 10)             # hs-dbno-adult.A.txt 'chat /dbno time 10' (kDbnoMinBleedS)
            no_fairy(pair, "B")
        field = "HyruleField_Adult"
        down(pair)
        same_scene(pair, "B", field, "after the down")
        pair.expect_log("B", CRAWL_TAKEN, max_frames=300)
        pair.expect_log("B", LYING_ADULT, max_frames=300)
        pair.never_log("B", CRAWL_REFUSED)
        shots(pair, "dbno-adult-lying")
        same_scene(pair, "B", field, "lying")
        crawl_m = stick_move(pair, "B", 0, 90)     # hs-dbno-adult.B.txt 'stick 0 1 180', cut to 90: the 10 s bleed-out
        pair.notes.append("adult downed: %.2f m in 90 frames of full stick" % crawl_m)
        same_scene(pair, "B", field, "after the stick")
        # bleed-out (10 s of B's clock), then spectating, then up beside A once no enemy is near (coop_dbno.h kQuietUs)
        pair.expect_log("B", event(EV_BLED_OUT), max_frames=1800)
        pair.expect_log("A", r"coop: dbno: P2 is spectating", max_frames=300)
        same_scene(pair, "B", field, "spectating")
        pair.expect_log("B", event(EV_RESPAWNED), max_frames=2400)
        pair.expect_log("B", r"coop: dbno: our Link up: the game's AS 'DamageLStand' played on", max_frames=300)  # :668
        def stood():
            if pair.log["B"].find(ADULT_UP_WAIT):
                raise AssertionError("stand-up FAIL only: LDamageStandUp (85) refused, the game's own Wait stood B up")
            return pair.log["B"].find(ADULT_UP)
        pair.until("B log /%s/" % ADULT_UP, stood, max_frames=300)
        pair.never_log("B", UP_REFUSED)
        # hs-dbno-adult.A.txt checks: 'Crawl (234) asked and taken' once, at most one 'waits: the slot took a request'
        n_taken, n_waits = count(pair, "B", CRAWL_TAKEN), count(pair, "B", CRAWL_WAITS)
        assert n_taken == 1, "B: 'Crawl (234) asked and taken' %d times in one down (want once)" % n_taken
        assert n_waits <= 1, "B: 'Crawl (234) waits: the slot took a request' %d times (want at most one)" % n_waits
        pair.step_both(60)
        same_scene(pair, "B", field, "after the respawn")
        a, b = pair.state("A")["pos"], pair.state("B")["pos"]
        pair.notes.append("after the respawn B is %.2f m from A" % dist(a, b))
        assert dist(a, b) < BESIDE_AFTER_M, "B not beside A after the respawn: %.2f m (< %.1f m)" % (
            dist(a, b), BESIDE_AFTER_M)
        shots(pair, "dbno-adult-respawned", twice=False)
        walk_m = stick_move(pair, "B", 0, 90)
        pair.notes.append("adult walk %.2f m in 90 frames after the respawn" % walk_m)
        assert walk_m >= 1.0, "B has no control after the respawn: %.2f m in 90 frames" % walk_m
        assert crawl_m < walk_m, "downed adult B moved as far as a walk: %.2f m vs %.2f m" % (crawl_m, walk_m)
