"""hud-fresh-0121's in-game proof: the other player's party HUD block (name line + hearts) and name tag stay on a game
stepped a frame every HOLD_S seconds by the bridge, on each game (testkit-0120's kit-in-play shots, about 1.5 s a frame,
showed neither: PartyStep and the hook's markers dropped every reading older than a second by the wall clock).

How it is judged (worker-rules TEST SYSTEM: state first, a shot for what a player sees):
- state: the bridge's `slow_steps` (docs/playtest.md; hookshot/src/coop_hookshot.h SlowStep): the last party HUD step
  and the last name-tag step that ran a second or more after the one before, and what each showed then. After the
  held frame each game must have a NEW slow step of each (count up), its gap >= 1000 ms, showing >= 1 block and >= 1 tag.
  Before hud-fresh-0121 such a step showed 0 of both (lichii/hook/coop_fresh.h's header).
- shot: the bridge shot of that same held frame on each game (refs/stepped-hud-<x>.png; NO-REF until a person keeps
  one): the block under the game's own hearts and the tag over the other Link.

Sources of every value:
- one game held at a time, the other running (hold_running_other): the real slow PC (its partner's game keeps sending);
  both held makes the other player fade by the presence rule (see hold_running_other), not the HUD/tags' stale check.
- HOLD_S 1.5: testkit-0120's frame time (smoke-pause-A.png 16:15:55.15 -> kit-in-play-A.png 16:16:11.85 over presents
  12361 -> 12372, game-out-A.txt:7499/:7587); the module's window is 1000 ms (coop_hookshot.cpp kSlowStepMs, PartyStep's
  CoopFresh::Stale(.., 1000, ..)). The hold is the scenario's input (the game held by the bridge, frames not running),
  not a wait for anything (the other game runs frames meanwhile, CHUNK at a time).
- the shot runs the held frame: the bridge's `shot` lets 1 frame at a time run until the picture comes (at most 5;
  docs/playtest.md `shot`, coop_playtest.cpp), so its first frame is the one after the hold.
- HUD_ON: coop_hookshot.cpp PartyStep ("coop: party hud: on the game's HUD"); the tags' lines and the view checks:
  test_regress_ui.py (TAGS_ON, TAGS_NO_MEMORY, TAGS_DIST, camera_behind_each, other_in_view, seen_not).
"""
import time

import pytest

from test_regress_ui import (CRASH, TAGS_DIST, TAGS_NO_MEMORY, TAGS_ON, camera_behind_each, other_in_view,
                             seen_not, tags_open, tags_seen)
from pair import CHUNK
from test_rows import need_child_in_play, place_in_front

GAMES = ("A", "B")
HUD_ON = r"coop: party hud: on the game's HUD"
HOLD_S = 1.5
SLOW_MS = 1000


def slow(pair, x):
    """Game x's `slow_steps` from its state: {'party_hud': {...}, 'name_tags': {...}} (count, gap_ms, shown)."""
    st = pair.state(x)
    s = st.get("slow_steps")
    assert isinstance(s, dict), "%s: no slow_steps in the bridge's state (a module without hud-stepped-0121)" % x
    return s


def hold_running_other(pair, x, other):
    """Game x held (no frame) for HOLD_S while `other` runs CHUNK frames at a time: x's next frame is a slow step, and
    the other player is still fresh on x. Holding BOTH games stops the other's samples too: on x the other player then
    fades out 500 ms after its last sample (coop/presence presence.hpp Settings show_delay 100 + extrapolate 150 +
    fade 250 ms; coop/overlay overlay.cpp skip(Why::faded): no marker, no tag), which is that rule, not hud-fresh-0121
    (stepped-hud-53d40ab3-2 FAIL 00:19 11-28: both held 4967 ms, A party_hud shown 1, name_tags shown 0)."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < HOLD_S:
        pair.call(other, "step", frames=CHUNK)


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("hud-fresh-0121: party HUD + name tags stay on a game stepped a frame every 1.5 s (testkit-0120 kit-in-play)")
def test_stepped_hud_and_tags(pair):
    with pair.setup():
        need_child_in_play(pair)
        place_in_front(pair, "B", of="A", dist=TAGS_DIST)       # B in front of A, facing A: each sees the other
        camera_behind_each(pair)                                 # (each game's own view, proven on its state)
        for x in GAMES:
            seen_not(pair, x, TAGS_ON, TAGS_NO_MEMORY, max_frames=120)
            if not pair.log[x].find(HUD_ON, pos=0):
                pair.expect_log(x, HUD_ON, max_frames=120)
        pair.step_both(30)                                       # (both running at full speed: no slow step here)
        before = {x: slow(pair, x) for x in GAMES}
    for x, other in (("A", "B"), ("B", "A")):
        ok, why = other_in_view(pair, x, other)
        assert ok, "%s: the other player is not in view before the held frame: %s" % (x, why)
    # (nametags-300-2, stepped-hud-c889af41-2 08:33: A's shot had B in view and no tag while name_tags said shown 1: on
    # 182cd53b5, without row 300's fix 58e1b85d8, the tag's balloon part was never opened, so the game drew none. shown
    # >= 1 alone let 0.12.0 pass; row 300's check is the part open + the balloon screen's root shown (test_regress_ui
    # tags_open), here before the held frame and AT it, with no frames stepped in between)
    for x in GAMES:
        tags_open(pair, x)
    for x, other in (("A", "B"), ("B", "A")):
        hold_running_other(pair, x, other)                       # x held HOLD_S while the other runs (sends samples)
        res = pair.check_shot(x, "stepped-hud", tolerance=0.80)  # (runs the held frame; 0.80: Kakariko's people move)
        t = tags_seen(pair, x)                                   # (the held frame's state: nothing stepped since)
        assert t.get("open", 0) >= 1 and t.get("screen_root") is True and t.get("shown", 0) >= 1, (
            "%s: at the held frame no tag drawn (row 300: its balloon part open and the screen root shown): %s" % (x, t))
        pair.notes.append("%s shot: %s; tags %s" % (x, res.get("why"), t))
    for x in GAMES:
        after = slow(pair, x)
        for part, need in (("party_hud", "block"), ("name_tags", "tag")):
            a, b = after[part], before[x][part]
            assert a["count"] > b["count"], "%s: no %s step %d ms or more after the one before (count %d -> %d)" % (
                x, part, SLOW_MS, b["count"], a["count"])
            assert a["gap_ms"] >= SLOW_MS, "%s: %s's last slow step only %d ms after the one before" % (x, part, a["gap_ms"])
            assert a["shown"] >= 1, "%s: the held frame (%d ms after the one before) showed no %s: %s" % (
                x, a["gap_ms"], need, after)
        pair.notes.append("%s slow_steps: %s" % (x, after))
    for x in GAMES:
        pair.never_log(x, CRASH)
