"""Row 294 (player report: a guest won Epona; the host then could never win her; the next day, on the host's save,
neither game had a horse: stuck before Gerudo Valley and horseback archery). State checks through the bridge.

Root cause (lichii/hook/coop_quests.h, "Row 294"): owning Epona is the save's OwnedHorseList (Struct 0x4920b64f,
`OwnedHorseList.ActorName`, a String64Array of 10), written only by Animal_Epona_Adult.EventCopyAndRegisterOwnedHorse
in Get_Epona.bfevfl's escape subflows. Co-op shared Get_Epona's Complete without it: a game at Complete has no field
Epona and Ingo no longer races, so it could never win her. The fix: the hook registers her first (EnsureEpona,
coop_hook.cpp: ActorName[first empty] = Animal_Epona_Adult through the game's String64Array queue, the trace the
game's own registration leaves in a won save), then writes Complete; the quest catch-up (in a session) mends a save
already at Complete with no horse the same way.

Values (each cited):
- `Step_Get_Epona`: the quest's Enum, hash 0x92431c55 (coop_quests.cpp Table; the old scenarios' `setenum
  0x92431c55 Complete`, epona-own.B.txt:44). `Complete` hashes to 0x4c0a63f4 (coop_hook.cpp ride getter before row
  294; tests/lichii/quests_test.cpp "row 294" checks flag_hash("Complete")).
- `OwnedHorseList.ActorName[0]`: element 0 (bridge `flag_read` / `flag` NAME[i], docs/playtest.md); won saves hold
  `Animal_Epona_Adult` there (coord-multiplayer test-saves owner-worlds-1125 wrld_03 and saves-incoming
  oot-defeated-ganon slot_00, read from their progress.sav).
- The log lines: coop_hook.cpp EnsureEpona ("coop: story: Epona registered as this game's own horse
  (OwnedHorseList.ActorName[0] = Animal_Epona_Adult): <why> (row 294)") and the quests' raise line
  ("coop: quests: Get_Epona <from> -> Complete (shared Threads of Time; issue 193)", coop_hook.cpp take_raised).
- `Market_Child`: a warp the bridge took on 11-23 (test_row53.py SCENE), for the scene load in the mend test.
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import ocarina_notes as ON                 # noqa: E402  (the ONE key -> note table, read from the game)
from test_rows import need_child_in_play   # noqa: E402  (the pilot rows' start-state check)

QUEST = "Step_Get_Epona"
COMPLETE = "0x4c0a63f4"                    # flag_read's enum value: the hash, 0x and 8 hex digits
HORSE = "OwnedHorseList.ActorName[0]"
EPONA = "Animal_Epona_Adult"
REGISTERED = r"coop: story: Epona registered as this game's own horse \(OwnedHorseList\.ActorName\[0\] = " \
             r"Animal_Epona_Adult\): %s"
SHARED_WHY = r"another player won her"
MENDED_WHY = r"Get_Epona was Complete here with no owned horse \(mended\)"
RAISED = r"coop: quests: Get_Epona \S+ -> Complete"
SHARE_FRAMES = 600     # the watcher sends a change within frames; the end may be held one try (EnsureEpona queued)
MEND_FRAMES = 300      # the catch-up runs once a second (CoopEvery::kSecondUs) while no event runs
APPLY_FRAMES = 30      # the game's applier empties the queue at its next GameData pass


def quest(pair, x):
    r = pair.flag_read(x, QUEST)
    assert r.get("kind") == "enum", "%s: %s reads as %s (not the quest's Enum)" % (x, QUEST, r.get("kind"))
    return r.get("value")


def horse(pair, x):
    r = pair.flag_read(x, HORSE)
    assert r.get("kind") == "text", ("%s: %s reads as %s: this module has no String64Array read (row 294's "
                                     "flag_read NAME[i])" % (x, HORSE, r.get("kind")))
    return r.get("value")


def set_horse(pair, x, text):
    r = pair.flag(x, HORSE, text)
    assert r.get("value") == text, "%s: %s not set to %r: %s" % (x, HORSE, text, r)   # (an error raises BridgeError)


@pytest.mark.budget(180)
@pytest.mark.row("#294 a won Epona reaches the other game with her horse (fresh win, both games)")
def test_epona_won_reaches_the_other_with_the_horse(pair):
    with pair.setup():
        need_child_in_play(pair)
        for x in ("A", "B"):
            q = quest(pair, x)
            if q == COMPLETE:
                raise AssertionError("%s: %s is already Complete: this boot is not fresh (the group keeps the end; "
                                     "run this test first on a lane's boot)" % (x, QUEST))
            set_horse(pair, x, "")
        pair.step_both(APPLY_FRAMES)
        for x in ("A", "B"):
            assert horse(pair, x) == "", "%s: %s did not empty" % (x, HORSE)
        mark = {x: pair.log[x].mark() for x in ("A", "B")}
    # B wins her: what its own escape flow leaves, the horse registered (the game's EventCopyAndRegisterOwnedHorse),
    # then Complete (Get_Epona.bfevfl: event 149 then 147).
    set_horse(pair, "B", EPONA)
    pair.step_both(APPLY_FRAMES)
    pair.flag("B", QUEST, "Complete")
    pair.until("A's %s is Complete" % QUEST, lambda: quest(pair, "A") == COMPLETE, SHARE_FRAMES)
    pair.step_both(APPLY_FRAMES)
    assert horse(pair, "A") == EPONA, "A took Complete without the horse (the row 294 bug): %s = %r" % (
        HORSE, horse(pair, "A"))
    reg = pair.expect_log("A", REGISTERED % SHARED_WHY, max_frames=60)
    raised = pair.expect_log("A", RAISED, max_frames=60)
    # the horse first, the end after (the log's order)
    lines = pair.log["A"].since(mark["A"])
    assert lines.index(reg) < lines.index(raised), "A: Complete came before the horse:\n%s\n%s" % (raised, reg)
    # B won her itself: nothing registered for it by co-op
    assert horse(pair, "B") == EPONA, "B lost its own horse: %r" % horse(pair, "B")
    assert pair.log["B"].find(r"coop: story: Epona registered", pos=mark["B"]) is None, "B: co-op registered a horse " \
        "for the game that won her"
    pair.notes.append("A: %s = %s, %s Complete" % (HORSE, EPONA, QUEST))


@pytest.mark.budget(240)
@pytest.mark.row("#294 a save at Get_Epona Complete with no owned horse gets her back (mend)")
def test_epona_complete_without_horse_is_mended(pair):
    with pair.setup():
        need_child_in_play(pair)
        if quest(pair, "A") != COMPLETE:
            # (her first, so the set-up's own Complete is never a bare one the catch-up mends around the mark)
            set_horse(pair, "A", EPONA)
            pair.step_both(APPLY_FRAMES)
            pair.flag("A", QUEST, "Complete")
            pair.until("A's %s is Complete" % QUEST, lambda: quest(pair, "A") == COMPLETE, SHARE_FRAMES)
        mark = pair.log["A"].mark()
    # The player report's save: Complete, and no horse (a shared end before 0.12.0).
    set_horse(pair, "A", "")
    pair.expect_log("A", r'coop: test settext OwnedHorseList\.ActorName\[0\] "": set', max_frames=30)
    pair.expect_log("A", REGISTERED % MENDED_WHY, max_frames=MEND_FRAMES)
    pair.step_both(APPLY_FRAMES)
    assert horse(pair, "A") == EPONA, "A: not mended: %s = %r" % (HORSE, horse(pair, "A"))
    assert quest(pair, "A") == COMPLETE, "A: the mend moved %s" % QUEST
    # A scene load: a live-only write goes back at a load (issue 57); the applier's saved copy stays.
    st = pair.warp_one("A", "Market_Child")
    assert (st.get("scene") or "").lower() == "market_child", "A is in %s after the warp" % st.get("scene")
    pair.step_both(APPLY_FRAMES)
    assert horse(pair, "A") == EPONA, "A: the horse went at the scene load: %r" % horse(pair, "A")
    regs = [ln for ln in pair.log["A"].since(mark) if "coop: story: Epona registered" in ln]
    assert len(regs) == 1, "A: registered %d times (once is the mend): %s" % (len(regs), regs)
    pair.notes.append("A: mended and kept through a scene load")


# ---- Test 3: Epona's Song on each game after the share and the mend (B #144, 2026-11-26) ----
# The player-facing end of row 294: a game that took the end from another player (B's win reaching A, then A's mend)
# can call HER with Epona's Song. No reload: it runs after tests 1 and 2 on the same boot (file order), so A's horse
# is the one co-op gave and B's the one its own win left. Each game plays the song in Hyrule Field, its own Epona
# comes (the ride module's line on that game), the other game sees her (its line), and a bridge shot on EACH game
# (twice, 6 frames apart: she moves) is compared to refs/<name>-<A|B>.png (Pair.check_shot; no reference yet: NO-REF,
# the candidate kept under <out>/new/ for the owner, never a pass by itself).
# No Epona after the song is a FAIL, not NO-REF as in test_epona_seen: there the won state was a bare flag (no
# registered horse, the row 294 bug itself); here both games hold her in OwnedHorseList, which is what the row fixes.
# Values: the steps of the hs-epona-seen port (port-sweep-c131 4e4016f78 tools/autotest/ingame/test_epona.py
# test_epona_seen), each cited at its constant.
SONG_ITEM = "Item_Melody_EponaSong"     # IsGet.Item_Melody_EponaSong (epona-seen-run.ps1 header, docs/items.md)
AGE_FLAG = "Sequence_IsChildhood"       # 1 child / 0 adult, then the warp (test_dbno_crawl.to_place; docs/playtest.md)
FIELD = "HyruleField_Adult"             # hs-epona-sync.A/B.txt 'warp HyruleField_Adult'
BOOT = ("KakarikoVillage_Child", "Kakariko_WellSide")   # the default boot (scenarios/live-kakariko-child.A.txt:7)
WATCH = 10.0                            # hs-epona-sync.A.txt 'place ... # 10 m south'
# the rows' '#! switches' (hs-epona-seen.A/B.txt), as test_epona.ROW_ENV, + the ocarina probe: the ocarina-out line
# (pair.OCARINA_PLAYS) is written only with LICHII_COOP_OCARINA_PROBE=1 (coop_objects.cpp:7012 EnvFlag, coop_ocarina.cpp:320; test_songs.ROW_ENV)
ROW_ENV = ("LICHII_COOP_PUPPET=1", "LICHII_COOP_DEBUG_ANIM=1", "LICHII_COOP_OBJECTS=1", "LICHII_COOP_OCARINA_PROBE=1")
NOTE_SENT = r"coop: ocarina note \d+ \(Ocarina_%s\) sent"            # test_rows.test_ocarina_notes (coop_ocarina_sound.cpp)
OUT_FREE = r"coop: ride: our Epona is out with nobody on her"       # coop_ride.cpp:488-491 (hs-epona-seen.B.txt b-free)
SEEN_HERE = r"coop: ride: P\d+'s Animal_Epona_Adult is here, nobody on her"   # coop_ride.cpp:326-330 (hs-epona-seen.A)
CALL_FRAMES = 900      # hs-epona-seen.B.txt 'await 900 ... out with nobody on her'
COME_FRAMES = 480      # hs-epona-seen.B.txt 'wait 480': she runs up and stops near the player
SEEN_FRAMES = 600      # hs-epona-seen.A.txt 'await 600' for the other game's line


def other(x):
    return "B" if x == "A" else "A"


def own_horse_pos(pair, x):
    """Game x's own Epona (the nearest Animal_Epona_Adult in its world list), or None (no world list)."""
    w = (pair.c[x].world() or {}).get("world") or {}
    for a in w.get("actors", []):
        if a.get("name") == EPONA and a.get("pos"):
            return a["pos"]
    return None


def ocarina_out(pair, x):
    """The ocarina out through the ONE shared way (Pair.ocarina_out, as test_epona.test_epona_seen): the re-equip after
    the last load, D-pad Left, the first-use box closed with B; ends with Link playing and no note sent, so the song's
    keys are the first notes."""
    pair.ocarina_out(x)


def song_shots(pair, player, name):
    """The other game faces the player (camera behind it: 'face' + 'press ZL', test_epona.face_rider), the player
    faces its own Epona; a bridge shot on each game, again 6 frames later (at most 8 shots a test: 2 songs x 2 x 2)."""
    o = other(player)
    p = pair.state(player)["pos"]
    pair.act(o, "face %.2f %.2f" % (p[0], p[2]))
    pair.press(o, ["ZL"], frames=8)
    h = own_horse_pos(pair, player)
    if h:
        pair.act(player, "face %.2f %.2f" % (h[0], h[2]))
        pair.press(player, ["ZL"], frames=8)
    for i in (1, 2):
        if i == 2:
            pair.step_both(6)
        for x in ("A", "B"):
            pair.check_shot(x, "%s-%d" % (name, i), tolerance=0.80)   # a moving horse in an open field (test_dbno_crawl)


def way_back_child(pair, done):
    """Both games child at the boot's place for the lane's later tests (Get_Epona and the horses stay: co-op never
    lowers the end, docs/flags.md). A failure here is raised only when the test body passed."""
    try:
        for x in ("A", "B"):
            pair.flag(x, AGE_FLAG, 1)
        pair.step_both(30)
        pair.warp_both(BOOT[0], entrance=BOOT[1])
        pair.step_both(30)
    except Exception as e:
        if done:
            raise
        pair.notes.append("way back after the failure: %s: %s" % (type(e).__name__, e))
        return
    if done:
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s) and s.get("age") == "child", "%s stays %s after the test" % (x, s.get("age"))


def play_song(pair, player):
    """Epona's Song on game `player`: X Y A X Y A, each ON.SONG_HOLD frames, ON.SONG_GAP apart (run fec750ad), each
    note's own sent line (since its own press) before the next."""
    for key in ON.keys("EponaSong"):
        at = pair.log[player].mark()
        pair.press(player, [key], frames=ON.SONG_HOLD)
        pat = NOTE_SENT % ON.KEY_NOTE[key]
        pair.until("%s log /%s/" % (player, pat), lambda: pair.log[player].find(pat, pos=at), ON.SONG_GAP)
        pair.step_both(ON.SONG_GAP)


@pytest.mark.budget(480)
@pytest.mark.look
@pytest.mark.needs_env(*ROW_ENV)
@pytest.mark.row("#294 after the share and the mend, Epona's Song calls each game's own Epona (both games, shots)")
def test_epona_song_calls_her_on_each_game(pair):
    done = False
    try:
        with pair.setup():
            need_child_in_play(pair)
            for x in ("A", "B"):
                if horse(pair, x) != EPONA or quest(pair, x) != COMPLETE:
                    # tests 1-2 did not leave it (deselected or failed): the won state as B's own win leaves it (the
                    # horse, then Complete: test 1), noted so the result says the horse was not co-op's
                    set_horse(pair, x, EPONA)
                    pair.step_both(APPLY_FRAMES)
                    pair.flag(x, QUEST, "Complete")
                    pair.notes.append("%s: won state set by this test's set-up (tests 1-2 did not leave it)" % x)
            pair.step_both(APPLY_FRAMES)
            for x in ("A", "B"):
                assert horse(pair, x) == EPONA and quest(pair, x) == COMPLETE, "%s: not won with her: %s = %r, %s %s" % (
                    x, HORSE, horse(pair, x), QUEST, quest(pair, x))
                pair.give(x, SONG_ITEM)
                pair.flag(x, AGE_FLAG, 0)
            pair.step_both(60)
        pair.warp_both(FIELD)                       # (a warp pair is ~31 s: Pair.WARP_PAIR_MEASURED_S, not set-up)
        with pair.setup():
            pair.step_both(30)
            for x in ("A", "B"):
                s = pair.state(x)
                assert pair.in_play(s) and s.get("age") == "adult", "%s not an adult in play in %s (%s, %s)" % (
                    x, FIELD, s.get("scene"), s.get("age"))
            a = pair.state("A")["pos"]
            pair.act("B", "place %.2f %.2f %.2f" % (a[0], a[1], a[2] - WATCH))   # B 10 m south, watching A
            pair.step_both(10)
        for player in ("A", "B"):
            o = other(player)
            with pair.setup():                      # the ocarina is set-up too (its own 30 s)
                ocarina_out(pair, player)
            pair.log[o].mark()
            start = pair.log[player].mark()         # (she may come inside the last note's gap: look from the song's start)
            play_song(pair, player)
            try:
                pair.until("%s log /%s/" % (player, OUT_FREE), lambda: pair.log[player].find(OUT_FREE, pos=start),
                           CALL_FRAMES)
            except AssertionError:
                raise AssertionError("%s played Epona's Song and its own Epona did not come (no /%s/ in %d frames): "
                                     "the horse co-op gave or kept does not answer the song" % (
                                         player, OUT_FREE, CALL_FRAMES))
            pair.press(player, ["B"], frames=12)    # the ocarina away
            pair.step_both(COME_FRAMES)
            pair.expect_log(o, SEEN_HERE, max_frames=SEEN_FRAMES)
            song_shots(pair, player, "epona-294-song-%s" % player)
            pair.notes.append("%s: Epona's Song brought her; %s saw her" % (player, o))
        done = True
    finally:
        way_back_child(pair, done)
