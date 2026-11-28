"""Row 321 (player report, nosoyyender 11-28, no logs: reports/player-reports.md): "whenever I leave Hyrule Town and head
toward Hyrule Field during the adult era, the game crashes or my PC freezes" (a guest, AMD card). This test crosses
that scene change on our build a few times, solo (co-op idle) and in a co-op pair with the GUEST (game B) crossing,
and judges it by state: each arrival in the scene asked, in play (a fresh Link sample, no load, no event), still in
play after STAY_FRAMES of game there, and no crash line in any game's module log. Each game's commit charge over the
run is the lane's own commit.json (memsample.Sampler, bt_plugin: from the boot start, every 30 s), kept with the result.

Sources of every value (worker-rules "no guessed values"):
- MARKET, MARKET_START: lichii/hook/coop_teleport.cpp start table ('{"Market_Adult", "Market_Entrance",
  "Dm_SY_BancChangeArrive_Market_02"}'): the Market's start at its gate to the field.
- FIELD, FIELD_START: 'warp HyruleField_Adult HyruleField_FrontOfMarket' (scenarios/epona.A.txt:16, epona-own.A.txt:30,
  hs-epona.A.txt:45 and 9 more scenario lines): the field's start in front of the Market, where the Market's exit lands.
- Each warp is given its entrance, so it is the game's map change event (the loading zone's own Dm_SY_BancChangeGo),
  never Longshot's teleport (coop_playtest_core.cpp PlanWarp; test_first_night.py): the same way out as walking
  through the gate, without depending on a walk path.
- AGE_FLAG 0 + 60 frames before the first adult warp: test_first_night.py (test_gear.py: the age flag takes 30+ frames).
- BOOT_SCENE / BOOT_START: the boot's Kakariko (scenarios/live-kakariko-child.A.txt; test_first_night.py way_back).
- CRASH_LINE: bt_plugin.CRASH_LINE (the module's crash handler line).
- MEMORY: the 0.12.0 memory line, once a minute, solo too (hookshot/src/coop_hookshot.cpp Frame, row 271: "coop: memory: "
  + ProcessMemoryText(); "coop: memory: process: private 28448 MB, ..." in the owner's P1 log of row 329). Each crossing
  game must have one since the test began (the crossings take minutes); its values go into the notes.
- STAY_FRAMES 300 (~10 s at pair.FPS 30): the field loads its far terrain and grass after the arrival; a stall or a
  device loss after the arrival shows in that time as a game that no longer answers or leaves play.
No shot: nothing new is drawn (a crash/stall check, judged by state; worker-rules TEST SYSTEM: shots only for the
user-facing things a row changes).
"""
import re

import pytest

import solo
from bt_plugin import CRASH_LINE

MARKET, MARKET_START = "Market_Adult", "Market_Entrance"
FIELD, FIELD_START = "HyruleField_Adult", "HyruleField_FrontOfMarket"
AGE_FLAG = "Sequence_IsChildhood"
BOOT_SCENE, BOOT_START = "KakarikoVillage_Child", "Kakariko_WellSide"
MEMORY = r"coop: memory: process: private (\d+) MB"
CROSSINGS = 3
# The memory line is due once a minute of WALL clock (CoopHostMenu::MemoryLineDue, coop_hookshot.cpp Frame): at most
# 3600 frames (~2 min of game at pair.FPS 30) for the next one, so a run that ended between two lines still sees one.
MEMORY_WAIT_FRAMES = 3600
# A test-only switch (the real module reads no BT_* name; test_dropin_longshot.py's BT_DROPIN=1 the same way): this
# row's tests run only with --env BT_ROW321=1 (needs_env: every other run deselects them), and fakes/market_field_321.py
# reacts only there (B #149: a fake acts only while its own row runs).
ROW_ENV = "BT_ROW321=1"
STAY_FRAMES = 300


def arrive(pair, x, scene, start):
    st = pair.warp_one(x, scene, start)
    assert (st.get("scene") or "").lower() == scene.lower(), "%s: asked %s at %s, is in %s" % (
        x, scene, start, st.get("scene"))
    pair.until_in_play("%s in play in %s" % (x, scene), games=(x,), max_frames=600)
    return st


def to_adult_market(pair, games):
    """Set-up: the games adult, in the Market at its gate (inside pair.setup(): C #145, warps in the set-up)."""
    for x in games:
        pair.flag(x, AGE_FLAG, 0)
    pair.step_both(60)
    for x in games:
        st = arrive(pair, x, MARKET, MARKET_START)
        assert pair.state(x).get("age") == "adult", "%s not adult in %s (%s)" % (x, MARKET, st.get("age"))


def cross(pair, x, watch):
    """Game x: Market -> Field -> Market, CROSSINGS times, each arrival in play and still in play STAY_FRAMES later;
    no crash line in any of `watch`."""
    for i in range(CROSSINGS):
        for scene, start in ((FIELD, FIELD_START), (MARKET, MARKET_START)):
            arrive(pair, x, scene, start)
            pair.step_both(STAY_FRAMES)
            s = pair.state(x)
            assert pair.in_play(s) and (s.get("scene") or "").lower() == scene.lower(), (
                "%s: crossing %d: not in play in %s %d frames after the arrival (%s, loading %s, event %s, not_ready %s)"
                % (x, i + 1, scene, STAY_FRAMES, s.get("scene"), s.get("loading"), s.get("event"), s.get("not_ready")))
            for y in watch:
                pair.never_log(y, CRASH_LINE)
            pair.notes.append("%s: crossing %d: in play in %s" % (x, i + 1, scene))


def memory_seen(pair, x, since):
    """Game x's memory lines since the test began: waited for (the line is wall-clock, once a minute: a short run may
    end before its next one), at most MEMORY_WAIT_FRAMES; the private MB of each into the notes."""
    rx = re.compile(MEMORY)

    def lines():
        return [int(m.group(1)) for m in (rx.search(ln) for ln in pair.log[x].since(since)) if m]
    got = pair.until("%s: a memory line (/%s/) since the test began (the 0.12.0 memory line active)" % (x, MEMORY),
                     lines, max_frames=MEMORY_WAIT_FRAMES)
    pair.notes.append("%s: memory private MB over the crossings: %s" % (x, ", ".join(str(v) for v in got)))


def way_back(pair, games, done):
    """Both games child in the boot's Kakariko (the boot is shared). Raised only when the test body passed."""
    try:
        for x in games:
            pair.flag(x, AGE_FLAG, 1)
        pair.step_both(60)
        for x in games:
            pair.warp_one(x, BOOT_SCENE, BOOT_START)
    except Exception as e:
        if done:
            raise
        pair.notes.append("way back failed after the test's FAIL: %s: %s" % (type(e).__name__, e))


@pytest.mark.budget(540)
@pytest.mark.needs_env(ROW_ENV)
@pytest.mark.row("321 adult Link Market -> Hyrule Field, a co-op guest crossing it (and the host), no crash or stall")
def test_market_field_guest_crosses(pair):
    with pair.setup():
        to_adult_market(pair, ("A", "B"))
    since = pair.log["B"].size()
    done = False
    try:
        cross(pair, "B", ("A", "B"))          # the reporter's case: the guest crosses, the host stays in the Market
        for x in ("A", "B"):                  # then both cross together once
            arrive(pair, x, FIELD, FIELD_START)
        pair.step_both(STAY_FRAMES)
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s) and (s.get("scene") or "").lower() == FIELD.lower(), (
                "%s: both in the field: not in play (%s, not_ready %s)" % (x, s.get("scene"), s.get("not_ready")))
            pair.never_log(x, CRASH_LINE)
        memory_seen(pair, "B", since)
        done = True
    finally:
        way_back(pair, ("A", "B"), done)


@solo.MARK
@pytest.mark.budget(420)
@pytest.mark.needs_env(ROW_ENV)
@pytest.mark.row("321 adult Link Market -> Hyrule Field solo (co-op idle), no crash or stall")
def test_market_field_solo(pair):
    with pair.setup():
        solo.prove(pair)
        to_adult_market(pair, ("A",))
    since = pair.log["A"].size()
    done = False
    try:
        cross(pair, "A", ("A",))
        memory_seen(pair, "A", since)
        done = True
    finally:
        way_back(pair, ("A",), done)
