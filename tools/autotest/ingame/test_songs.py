"""Regress vs 0.11.12 step 11 (coord offline/regress-01113.ps1 step 11, s11-songs.ps1; reports/rc-release-0120.md
section 4): the songs Zelda's Lullaby, the Song of Storms and Saria's Song, as bridge tests. Epona's Song is
test_epona.test_epona_seen; the five notes alone are test_rows.test_ocarina_notes.

| Test | Old row | Kind |
|---|---|---|
| test_frog_songs | hs-u5-gifts B (the frogs: Zelda's Lullaby, then the Song of Storms) | song keys + the frogs' payout Bools on both games (state) + B shots |
| test_ocarina_given_saria | hs-ocarina-given (#190: an ocarina from another player comes out; Saria's Song) | item sync + mend (state, log) + notes + shots on each game |

Every song is played from the ONE key table tools/autotest/ocarina_notes.py (ON.keys / ON.KEY_NOTE / ON.SONG_HOLD /
ON.SONG_GAP, read from the game by run epona-seen-keys-fec750ad), never a copy, and each note is judged by the module's
own 'coop: ocarina note N (Ocarina_<note>) sent' line (coop_ocarina_sound.cpp:133), as test_epona.test_epona_seen does.

What proves a song was the song (not only six notes): the frogs pay each song once and write it in their own Bool
(MiniGame_FrogTenors.bfevfl; lichii/hook/coop_shop_sale.h kOnceGifts: MiniGame_FrogTenors_ZeldaLullaby 0x6e3948e7,
MiniGame_FrogTenors_StormSong 0xd578df95). Both are shared once for the group since row 237 (kOnceGifts), so the other
game gets them too. Saria's Song has no such Bool outside the frogs: its effect is judged by the shots (NO-REF until
references are stored), as the old step did ("shots decide").
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import ocarina_notes as ON     # noqa: E402  (the ONE key -> note table, read from the game)
import testsave                # noqa: E402

from test_gear import GATE, SCENE_CHILD   # noqa: E402  (the lane's Kakariko boot place: the way back)
from test_rows import need_child_in_play, place_in_front   # noqa: E402

# -- shared patterns (each the module's own line) ------------------------------------------------------------------
NOTE_SENT = r"coop: ocarina note \d+ \(Ocarina_%s\) sent"            # coop_ocarina_sound.cpp:133 (test_epona.NOTE_SENT)
OCARINA_OUT = r"ocarina probe: our Link plays the ocarina"          # coop_ocarina.cpp:320 (LICHII_COOP_OCARINA_PROBE=1)
PUPPET_SOUND = r"plays sound 'Ocarina_%s'"                          # test_rows.test_ocarina_notes (the other game hears it)
GIFTS_ON = r"coop: gifts: sharing 22 one-time gift flags"           # coop_hook.cpp:572; 22 = kOnceGifts' size (coop_shop_sale.h)

# -- the frogs (hs-u5-gifts.B.txt; positions from its header: romfs Banc ZoraRiver_Child_Static) ---------------------
RIVER, RIVER_IN = "ZoraRiver_Child", "ZoraRiver_LostWoodSide"     # hs-u5-gifts.B.txt 'warp ZoraRiver_Child ZoraRiver_LostWoodSide'
FROG_SPOT = (8.90, 102.40, -50.90)     # 'place 8.90 102.40 -50.90   # TurnToTarget, the frog scene's player anchor'
FROGS = (9.19, -54.83)                 # 'face 9.19 -54.83           # OcarinaPoint_MiniGame_FrogTenors / Animal_Frog_Blue'
LULLABY_PAID = "0x6e3948e7"            # MiniGame_FrogTenors_ZeldaLullaby (coop_shop_sale.h:188, kOnceGifts)
STORM_PAID = "0xd578df95"              # MiniGame_FrogTenors_StormSong (coop_shop_sale.h, kOnceGifts; coop_room_objects.cpp:205)
SONG_ITEMS = {"ZeldaLullaby": "IsGet.Item_Melody_ZeldaLullaby",   # 0x01c20002 (docs/items.md "Song hashes")
              "StormSong": "IsGet.Item_Melody_StormSong"}         # 0x2b39dd4f (docs/items.md "Song hashes")
TALK_PRESSES = 16      # hs-u5-gifts.B.txt presses A 6 times (Lullaby) / 4 (Storms) 200-240 frames apart, then B: here
TALK_GAP = 200         # A every TALK_GAP frames until the frogs' talk is over, at most TALK_PRESSES (a bound, not a wait)
SHARE_FRAMES = 600     # the other game's copy of a shared Bool (test_intro_seen / test_boss_away wait the same order)

# -- the given ocarina (hs-ocarina-given.A/B.txt; issue 190; docs/items.md "The ocarina") ----------------------------
NO_OCARINA = "no-ocarina"                                   # saves/no-ocarina.py (save_flags.NO_OCARINA + EquipOcarina)
FAIRY = "Item_Ocarina_Fairy"                                # the bridge give's item (docs/playtest.md `give`)
FAIRY_COUNT = "0xb7df60f1"                                  # PouchData.Item_Ocarina_Fairy.StockNum (ocarina::kFairy)
EQUIP = "0xb72cd3fc"                                        # PlayerStatus.EquipOcarina (ocarina::kEquip), kind text
# the Bools a real get sets, raised by MendOcarina for a held Fairy Ocarina (coop/host/src/game_items.cpp:758
# ocarina_bools_mend: kIsGetFairy, kGotFairy, kSlotA, kSlotB, kPrepared; hashes game_items.hpp namespace ocarina)
MENDED_BOOLS = ("0x42bd296f", "0x137df1ac", "0xaacdef1f", "0xda41879d", "0x171de71f")
EQUIPPED = r"coop: items: ocarina '' -> 'Item_Ocarina_Fairy' for the held ocarina"   # coop_hook.cpp:3502 (the #190 pass line)

ROW_ENV = ("LICHII_COOP_CHEATS=1", "LICHII_COOP_OCARINA_PROBE=1")   # give/flag (docs/playtest.md) + the probe line
GIVEN_ENV = ROW_ENV + ("LICHII_COOP_PUPPET=1",)                     # hs-ocarina-given.A.txt '#! switches'


READY_FRAMES = 600     # harness-c133-scene's until_in_play bound (pair.py), the same steps until it is in this base


def ready_child(pair):
    """Both games in play with a FRESH Link sample, stepped (never one read right after the boot: gifts-237 16:25
    SETUP-FAIL 'no Link sample has been taken yet', B #139 16:26), then child Link (test_rows.need_child_in_play).
    Once harness-c133-scene 82f9a7a5a is in the base need_child_in_play steps itself; this stays harmless."""
    pair.until("both games in play (a fresh Link sample, no load, no event)",
               lambda: all(pair.in_play(pair.state(x)) for x in ("A", "B")), max_frames=READY_FRAMES)
    need_child_in_play(pair)


def value(pair, x, name):
    r = pair.flag_read(x, name)
    assert "value" in r, "%s: flag_read %s gave no value: %s" % (x, name, r)
    return r["value"]


def ocarina_out(pair, x, again=False):
    """D-pad Left (the kit save has the ocarina there: hs-ocarina.A.txt 'press DLEFT 12' + 'wait 240'), then the probe
    line. The first time its first-use "Ocarina Audio Input" note is closed with A and the ocarina put away (B) and out
    again, so no note but the song's is played after it (test_epona.test_epona_seen's own steps)."""
    pair.need_ocarina(x)                              # its re-equip after the last load first (songs-frogs-733c4d8f)
    pair.log[x].mark()
    pair.press(x, ["LEFT"], frames=12)
    pair.step_both(240)
    assert pair.log[x].find(OCARINA_OUT), "%s: no '%s' 240 frames after LEFT" % (x, OCARINA_OUT)
    if again:
        return
    pair.press(x, ["A"], frames=12)
    pair.step_both(120)
    ocarina_away(pair, x)
    ocarina_out(pair, x, again=True)


def ocarina_away(pair, x):
    pair.press(x, ["B"], frames=8)
    pair.step_both(60)


def play(pair, x, song, hears=None):
    """Song `song` from the ONE table: each key held ON.SONG_HOLD, its note's 'sent' line within ON.SONG_GAP, then
    ON.SONG_GAP frames (run fec750ad's timing). `hears`: the other game that must play each note's sound too."""
    for key in ON.keys(song):
        pair.log[x].mark()
        if hears:
            pair.log[hears].mark()
        pair.press(x, [key], frames=ON.SONG_HOLD)
        pair.expect_log(x, NOTE_SENT % ON.KEY_NOTE[key], max_frames=ON.SONG_GAP)
        if hears:
            pair.expect_log(hears, PUPPET_SOUND % ON.KEY_NOTE[key], max_frames=ON.SONG_GAP)
        pair.step_both(ON.SONG_GAP)


# -- the frogs -----------------------------------------------------------------------------------------------------
def frogs_talk(pair, paid, song):
    """The frogs' talk after a song: A every TALK_GAP frames while B is in an event (at most TALK_PRESSES), then B
    (hs-u5-gifts.B.txt 'press B' at the end). The song counts once B's own payout Bool is 1."""
    for _ in range(TALK_PRESSES):
        if value(pair, "B", paid) and not pair.state("B").get("event"):
            pair.step_both(120)                       # (a get scene can start right after the talk: twice no event)
            if not pair.state("B").get("event"):
                break
        pair.press("B", ["A"], frames=12)
        pair.step_both(TALK_GAP)
    assert value(pair, "B", paid), (
        "B: the frogs did not pay %s (%s still 0 after the talk): the song was not taken" % (song, paid))
    assert not pair.state("B").get("event"), "B: still in an event after %d A presses" % TALK_PRESSES


def frog_song(pair, song, paid, shot):
    pair.press("B", ["LEFT"], frames=12)             # 'press DLEFT 12' + 'wait 240': the frogs' intro text
    pair.step_both(240)
    # (whether Longshot's 'event' is true in the frogs' talk is not proven by any run: noted, not judged)
    pair.notes.append("B %s: event %s 240 frames after LEFT at the frogs" % (song, pair.state("B").get("event")))
    pair.press("B", ["A"], frames=12)                # 'press A 12' + 'wait 240': closes it, the note prompt opens
    pair.step_both(240)
    play(pair, "B", song)
    pair.step_both(120)                               # 'wait 120' (mark b-lullaby / b-storm)
    pair.check_shot("B", shot, tolerance=0.80)        # the frogs answer the song (B's own screen; A is in Kakariko)
    frogs_talk(pair, paid, song)
    pair.until("A gets the frogs' %s Bool %s (shared once, kOnceGifts)" % (song, paid),
               lambda: value(pair, "A", paid), max_frames=SHARE_FRAMES)


@pytest.mark.budget(900)
@pytest.mark.look
@pytest.mark.needs_env(*ROW_ENV)
@pytest.mark.row("regress 11: Zelda's Lullaby and the Song of Storms played to the frogs, each paid once and "
                 "shared (hs-u5-gifts B)")
def test_frog_songs(pair):
    with pair.setup():
        ready_child(pair)
        assert pair.log["A"].find(GIFTS_ON, pos=0) and pair.log["B"].find(GIFTS_ON, pos=0), (
            "no '%s' at this boot's session start (the frogs' Bools are not shared)" % GIFTS_ON)
        paid = {"%s %s" % (x, f): value(pair, x, f) for x in ("A", "B") for f in (LULLABY_PAID, STORM_PAID)}
    if any(paid.values()):                            # a once-only payout, never put back: an earlier round or test
        pytest.skip("the frogs paid already on this boot (%s): nothing to judge" % paid)
    done = False
    try:
        with pair.setup():
            for item in SONG_ITEMS.values():          # B knows both songs, so an unlearned song is never why it is not taken
                pair.give("B", item)
                assert value(pair, "B", item), "B: %s not 1 after give" % item
        with pair.setup():                            # the ocarina is set-up too (its own 30 s, as test_epona_seen)
            ocarina_out(pair, "B")                    # away from the frogs: the first-use note is not their talk
            ocarina_away(pair, "B")
        with pair.setup():
            pair.warp_one("B", RIVER, RIVER_IN)
            pair.act("B", "place %.2f %.2f %.2f" % FROG_SPOT)
            pair.step_both(120)
            pair.act("B", "face %.2f %.2f" % FROGS)
            pair.press("B", ["ZL"], frames=8)        # camera behind Link (hs-u5-gifts.B.txt 'press ZL')
            pair.step_both(60)
            at = pair.state("B").get("scene") or ""
            assert at.lower() == RIVER.lower(), "B in %s, not %s" % (at, RIVER)
            pair.need_ocarina("B")                    # the warp's load: re-equipped before frog_song's LEFT
        frog_song(pair, "ZeldaLullaby", LULLABY_PAID, "songs-frogs-lullaby")
        frog_song(pair, "StormSong", STORM_PAID, "songs-frogs-storms")
        done = True
    finally:
        way_back(pair, done)


def way_back(pair, done):
    """B back to the lane's Kakariko boot place (test_gear.way_back's warp), for the lane's later tests."""
    try:
        if (pair.state("B").get("scene") or "").lower() != SCENE_CHILD.lower():
            pair.warp_one("B", SCENE_CHILD, GATE)
    except Exception as e:
        if done:
            raise
        pair.notes.append("way back after the failure: %s: %s" % (type(e).__name__, e))
        return
    if done:
        ready_child(pair)


# -- the given ocarina (#190) and Saria's Song -----------------------------------------------------------------------
@pytest.mark.budget(600)
@pytest.mark.look
@pytest.mark.needs_save(NO_OCARINA)
@pytest.mark.needs_env(*GIVEN_ENV)
@pytest.mark.row("#190 an ocarina from another player comes out and plays, then Saria's Song (hs-ocarina-given)")
def test_ocarina_given_saria(pair):
    with pair.setup():
        ready_child(pair)
        testsave.prove(pair, NO_OCARINA)              # neither game holds an ocarina, EquipOcarina "" (SETUP-FAIL else)
        place_in_front(pair, "A", of="B")             # A sees B's Link (its puppet) with the ocarina out
        pair.log["B"].mark()
        r = pair.give("A", FAIRY)                     # A gets it (hs-ocarina-given.A: 'A has it from Saria')
        assert int(value(pair, "A", FAIRY_COUNT) or 0) >= 1, "A: no Fairy Ocarina after give: %s" % r
    # B never had one: it comes only through the item sync, and the mend makes it B's own (the #190 fix)
    pair.until("B gets A's Fairy Ocarina through the item sync (%s >= 1)" % FAIRY_COUNT,
               lambda: int(value(pair, "B", FAIRY_COUNT) or 0) >= 1, max_frames=SHARE_FRAMES)
    pair.expect_log("B", EQUIPPED, max_frames=SHARE_FRAMES)
    pair.until("B's ocarina Bools raised for the held ocarina (%s)" % ", ".join(MENDED_BOOLS),
               lambda: all(value(pair, "B", h) for h in MENDED_BOOLS), max_frames=300)
    assert value(pair, "B", EQUIP) == FAIRY, "B: EquipOcarina is %r, not %r" % (value(pair, "B", EQUIP), FAIRY)
    ocarina_out(pair, "B")                            # the #190 bug: D-pad Left took nothing out
    for x in ("A", "B"):
        pair.check_shot(x, "ocarina-given-out", tolerance=0.80)
    for key, note in ON.KEY_NOTE.items():             # hs-ocarina-given.B.txt: the five notes, then Saria's Song
        pair.log["B"].mark()
        pair.log["A"].mark()
        pair.press("B", [key], frames=ON.SONG_HOLD)
        pair.expect_log("B", NOTE_SENT % note, max_frames=ON.SONG_GAP)
        pair.expect_log("A", PUPPET_SOUND % note, max_frames=ON.SONG_GAP)
        pair.step_both(ON.SONG_GAP)
    play(pair, "B", "SariaSong", hears="A")
    pair.step_both(60)
    for x in ("A", "B"):                              # the song taken: what each player sees (no state shows it)
        pair.check_shot(x, "ocarina-given-saria-1", tolerance=0.80)
    pair.step_both(240)                               # 'wait 300' in all (mark after-song)
    for x in ("A", "B"):
        pair.check_shot(x, "ocarina-given-saria-2", tolerance=0.80)
    pair.notes.append("B after Saria's Song: event %s" % pair.state("B").get("event"))
    for _ in range(2):                                # 'press B 12' twice: the song's text, then the ocarina away
        pair.press("B", ["B"], frames=12)
        pair.step_both(120)
