"""Row 203 (mag, 0.11.8): a Gerudo Fortress small key "did not spawn / was not received" in co-op.

Player report row 203 (coord reports/player-reports.md); cause in coord reports/gerudo-warp-0119.md "Row 203": the
count sharing worked in mag's log (SmallKeyNum is a count, the IsGet_SmallKey flags are shared); the missing key was a
side effect of row 200 (a copy's death ran the owner-only defeat event GerudoThiefDropKey in the other game), fixed by
gerudo-warp-0119 (7583ecec1, in integrate-0120). No old .ps1 row: this is the first proof of it in a game.

What this proves, by flags only (no shot: a count and a flag, nothing a player looks at on its own):

| Check | Source |
|---|---|
| A takes cell 1's key: B's DungeonInfo.GerudoFortress.SmallKeyNum rises by the same 1 | coop/host/src/game_items.cpp:93 (R::count: shared as a delta, flag_watcher.cpp FlagWatcher::add) |
| ... and B has IsGet_SmallKey_GerudoFortress.SmallKey01 | lichii/hook/coop_story.cpp:272 (Kind::world, report 99) |
| A uses it at a lock: B's count goes down by 1 too (mag's log: count deltas at 95929/96674) | the same count rule |
| the shared flags agree after (checkpoint + desync_check --flags-only) | as test_rows.py test_story_flag_shared |

The guard fight half of row 200/203 (a copy's death never runs the defeat event) needs an adult save in the fortress
and a guard fight: not in this lane's boot (child, Kakariko). The report says so; that half stays with row 200.
"""
import os
import subprocess
import sys

import pytest

from test_rows import need_child_in_play

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import playtest_client as pc   # noqa: E402

COUNT = "DungeonInfo.GerudoFortress.SmallKeyNum"        # coop/host/src/game_items.cpp:93 (int32, R::count)
KEY01 = "IsGet_SmallKey_GerudoFortress.SmallKey01"      # lichii/hook/coop_story.cpp:272 (0xdc0010f6, Kind::world)
SHARE_FRAMES = 300   # as test_story_flag_shared's wait for a shared flag (one server round trip and a frame's diff)


def count(pair, x):
    r = pair.flag_read(x, COUNT)
    v = r.get("value")
    assert isinstance(v, int) and not isinstance(v, bool), "%s: %s reads %r (kind %s)" % (x, COUNT, v, r.get("kind"))
    return v


def is_set(pair, x, name):
    return pair.flag_read(x, name).get("value") in (1, True)


@pytest.mark.budget(150)
@pytest.mark.row("row 203 a Gerudo Fortress small key taken in one game is in the other's count (gerudo-warp-0119)")
def test_gerudo_small_key_shared(pair):
    logs = os.path.join(pair.out_dir or ".", "checkpoints")
    with pair.setup():
        need_child_in_play(pair)
        for x in ("A", "B"):          # back to 0 in each game (a world flag is shared only 0 -> 1: coop_story.cpp)
            pair.flag(x, KEY01, 0)
        pair.step_both(30)
        for x in ("A", "B"):
            assert not is_set(pair, x, KEY01), "%s: %s did not go back to 0" % (x, KEY01)
        start = {x: count(pair, x) for x in ("A", "B")}
        assert start["A"] == start["B"], "the counts differ before the test: %s" % start
    c0 = start["A"]
    pair.flag("A", COUNT, c0 + 1)     # A picks cell 1's key up: the game's own count +1 and the key's flag
    pair.flag("A", KEY01, 1)
    pair.until("B's %s = %d (A took a key)" % (COUNT, c0 + 1), lambda: count(pair, "B") == c0 + 1, SHARE_FRAMES)
    pair.until("B has %s" % KEY01, lambda: is_set(pair, "B", KEY01), SHARE_FRAMES)
    pair.flag("A", COUNT, c0)         # A uses it at a lock: -1
    pair.until("B's %s back to %d (A used the key)" % (COUNT, c0), lambda: count(pair, "B") == c0, SHARE_FRAMES)
    pair.step_both(30)
    got = {x: count(pair, x) for x in ("A", "B")}
    pair.notes.append("count %s: start %d, after take %d, after use A %d B %d" % (COUNT, c0, c0 + 1, got["A"], got["B"]))
    assert got == {"A": c0, "B": c0}, "the counts after the use: %s (want %d each: a delta lost or doubled)" % (got, c0)
    tag = "keys-%d" % (pair.c["A"].last_frame or 0)
    for x, p in (("A", "p1"), ("B", "p2")):
        pc.checkpoint(pair.c[x], logs, p, tag)
    r = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(HERE), "desync_check.py"), logs, "--clients",
                        "--flags-only"], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, "desync_check exit %d: %s" % (r.returncode, (r.stdout + r.stderr)[-400:])
