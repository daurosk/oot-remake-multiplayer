"""Row 333 (xnicktendox, 0.12.0): a Gerudo Fortress carpenter freed by a client "does not count" on the host.

Cause and fix: coord reports/carpenter-333.md and lichii/hook/coop_carpenters.h. Each cell's rescue flags are shared
(lichii/hook/coop_story.cpp, Kind::world, report 99) but the fortress's count Ints are not; the module now sets them
from the shared rescues in every game (FollowCarpenters), and a save already stuck (4/4 freed, card scene unplayed)
gets the card scene asked by name (CoopCarpenters::Mender, kStuckUs 90 s).

| Check | Source |
|---|---|
| A frees cell 1 (its two end flags): B's NumOfCarpenterHelped -> 1 and _02 -> 3 | coop_carpenters.h FollowRescues; coop_hook.cpp FollowCarpenters |
| B's log says so | coop_hook.cpp FollowCarpenters: 'carpenters freed: our count -> 1 (the shared rescues; row 333)' |
| back to 0 rescues: both counts back to 0 / 4 (a count above the rescues comes down) | FollowRescues (both ways) |
| stuck save in the fortress: the mender's log line (A or B, whichever fires first), then HavePlayedEvent.Meet_FourCarpenter_ClearEvent = 1 in both | coop_hook.cpp (Row 333 mender) LOG 'card scene never played' |
| the card scene on each game's screen (a shot each) | user-facing: the scene a player sees |
| each game holds the card exactly once after it (both menders may fire) | coop/host/src/game_items.cpp:38 IsGet.Item_Token_Gerudo (R::any), :333 PouchData.Item_Token_Gerudo.StockNum (R::max); the get is Dm_Gerudo_0025's own (docs/items.md:382) |
| every mender line is counted per game (a re-ask after an expired ask: unit test carpenters_test.cpp) | coop_carpenters.h Mender (asks again every kStuckUs while unplayed) |

The split rescue done by hand (case 2 of the report: the 4th rescuer's own +1 plays the card scene) needs four real
cell fights; it is not in this file (report NOT proven).
"""
import os
import sys

import pytest

from test_rows import need_child_in_play

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

HELPED = "GerudoFortress_Meet_FourCarpenter_NumOfCarpenterHelped"       # coop_carpenters.h kHelped 0x290a6222
LEFT = "GerudoFortress_Meet_FourCarpenter_NumOfCarpenterHelped_02"      # coop_carpenters.h kHelpedLeft 0x6a466d6b
CARD_PLAYED = "HavePlayedEvent.Meet_FourCarpenter_ClearEvent"           # coop_carpenters.h kCardScene 0x754f6228
# Each cell's two end flags, by name (coop_carpenters.h kCellFlags; names from coop_story.cpp's shared list).
CELLS = [("GerudoFortress_IsAfterHelpCarpenter_Npc_Hylia_0%d_Adult" % (12 + n), "GerudoFortress_WarpPoint0%d_ClearEvent" % n)
         for n in (1, 2, 3, 4)]
FORT, FORT_START = "GerudoFortress_Adult", "GerudoFortress_FromGerudoValley"   # coop_start_points.inc:40
FOLLOW_FRAMES = 300   # FollowCarpenters runs once a second (quest_catch_up_ Due) + a shared flag's round trip
MENDER_FRAMES = 150 * 30   # kStuckUs 90 s of wall clock after arrival, with margin (frames step slower than real time)
CARD_FRAMES = 60 * 30      # the live requester's ask, then the scene's start
CARD_GOT = "IsGet.Item_Token_Gerudo"                      # coop/host/src/game_items.cpp:38 (bool, R::any)
CARD_NUM = "PouchData.Item_Token_Gerudo.StockNum"         # coop/host/src/game_items.cpp:333 (int32, R::max)
MEND = "card scene never played"                          # coop_hook.cpp, the Row 333 mender's LOG_INFO


def read_int(pair, x, name):
    r = pair.flag_read(x, name)
    v = r.get("value")
    assert isinstance(v, int) and not isinstance(v, bool), "%s: %s reads %r (kind %s)" % (x, name, v, r.get("kind"))
    return v


def is_set(pair, x, name):
    return pair.flag_read(x, name).get("value") in (1, True)


def counts(pair, x):
    return read_int(pair, x, HELPED), read_int(pair, x, LEFT)


def clear_cells(pair):
    """No cell rescued in either game (a world flag is shared only 0 -> 1: each game is set back itself)."""
    for x in ("A", "B"):
        for a, w in CELLS:
            pair.flag(x, a, 0)
            pair.flag(x, w, 0)
    pair.step_both(30)


@pytest.mark.budget(150)
@pytest.mark.row("row 333 a carpenter freed in one game counts in the other's fortress count (carpenter-333)")
def test_carpenter_count_follows(pair):
    with pair.setup():
        need_child_in_play(pair)
        clear_cells(pair)
        for x in ("A", "B"):
            for a, w in CELLS:
                assert not is_set(pair, x, a) and not is_set(pair, x, w), "%s: %s/%s did not go back to 0" % (x, a, w)
        pair.until("both counts at 0 / 4 with no cell freed",
                   lambda: all(counts(pair, x) == (0, 4) for x in ("A", "B")), FOLLOW_FRAMES)
    a, w = CELLS[0]
    pair.flag("A", a, 1)       # A frees cell 1: its carpenter's end flags (Dm_Gerudo_0020 sets them)
    pair.flag("A", w, 1)
    pair.until("B has cell 1's flags", lambda: is_set(pair, "B", a) and is_set(pair, "B", w), FOLLOW_FRAMES)
    pair.until("B's counts 1 / 3", lambda: counts(pair, "B") == (1, 3), FOLLOW_FRAMES)
    pair.expect_log("B", "carpenters freed: our count -> 1", FOLLOW_FRAMES)
    got = {x: counts(pair, x) for x in ("A", "B")}
    pair.notes.append("after cell 1 in A: %s" % got)
    assert got == {"A": (1, 3), "B": (1, 3)}, "counts after one rescue: %s (want 1/3 in each game)" % got
    clear_cells(pair)          # back down: a count above the rescues follows them down
    pair.until("both counts back at 0 / 4", lambda: all(counts(pair, x) == (0, 4) for x in ("A", "B")), FOLLOW_FRAMES)


@pytest.mark.budget(420)
@pytest.mark.look
@pytest.mark.row("row 333 a save with all four carpenters freed and no card scene gets the card scene (carpenter-333)")
def test_stuck_save_gets_card_scene(pair):
    with pair.setup():
        need_child_in_play(pair)
        for x in ("A", "B"):
            for a, w in CELLS:
                pair.flag(x, a, 1)
                pair.flag(x, w, 1)
            pair.flag(x, CARD_PLAYED, 0)
            pair.flag(x, CARD_GOT, 0)     # no card yet (a stuck save before its card; the get is the scene's)
            pair.flag(x, CARD_NUM, 0)
        pair.step_both(30)
        for x in ("A", "B"):
            assert not is_set(pair, x, CARD_PLAYED), "%s: %s did not go back to 0" % (x, CARD_PLAYED)
            assert not is_set(pair, x, CARD_GOT) and read_int(pair, x, CARD_NUM) == 0, "%s: the card is still held" % x
        # adult and into the fortress (docs/playtest.md warp: Sequence_IsChildhood, 30 frames, then the warp)
        for x in ("A", "B"):
            pair.flag(x, "Sequence_IsChildhood", 0)
        pair.step_both(30)
        pair.warp_both(FORT, FORT_START)
    pair.until("both counts 4 / 0", lambda: all(counts(pair, x) == (4, 0) for x in ("A", "B")), FOLLOW_FRAMES)
    # Both games are stuck, so either mender may fire first; the other game may then join the shared scene instead
    # of asking its own (review-tests-153-8: accept the line from A or B).
    first = pair.until("a mender line in A or B (/%s/)" % MEND,
                       lambda: next((x for x in ("A", "B") if pair.log[x].find(MEND)), None), MENDER_FRAMES)
    pair.until("%s's card scene running or played" % first,
               lambda: is_set(pair, first, CARD_PLAYED) or pair.state(first).get("event"), CARD_FRAMES)
    pair.step_both(60)
    for x in ("A", "B"):        # (each game: the screenshot rule; the other game joins the shared scene or plays its own)
        pair.check_shot(x, "carpenter-card-scene")
    for x in ("A", "B"):
        pair.until("%s has %s" % (x, CARD_PLAYED), lambda x=x: is_set(pair, x, CARD_PLAYED), CARD_FRAMES)
    pair.step_both(120)   # the get's count settled and shared (FOLLOW_FRAMES covers one round trip)
    held = {x: (is_set(pair, x, CARD_GOT), read_int(pair, x, CARD_NUM)) for x in ("A", "B")}
    asks = {x: sum(1 for ln in pair.log[x].since() if MEND in ln) for x in ("A", "B")}
    pair.notes.append("mender: first line in %s; lines per game %s; card held %s" % (first, asks, held))
    assert held == {"A": (True, 1), "B": (True, 1)},         "the card after the mender: %s (want IsGet true and StockNum 1 in each game: not given, or given twice)" % held
