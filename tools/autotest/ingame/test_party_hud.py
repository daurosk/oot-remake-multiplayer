"""party-hud-crash-c126: the other players' hearts on the game's own HUD, through boot -> intro -> title -> save load
and back to the title -> load again, on both games, with no crash (replaces offline/party-hud-crash-c126-run.ps1 +
party-hud-probe-run.ps1, the hs-party-hud-probe row).

The crash it guards (party-hud-crash-c126, runs row-53-tot-probe-c126 06:58 and party-hud-crash-c126-05b2d8c7 08:25,
both games): at the save's first load from the title the module's party HUD (CoopPartyHud::Hud::Step -> Find /
MakeLive) called into the status screen's layout while that screen was listed but its layout was not the game's (the
scene loader's memory), 0xc0000005. The lane boot (live.py start: boot, intro, title, the save loaded from the title)
IS that window, so the first test judges the boot; the second goes back to the title and loads again.

Sources of every value (worker-rules "no guessed values"):
- switches: tools/autotest/scenarios/hs-party-hud-probe.A.txt '#! switches': COOP_TEST_PARTY_HUD=1 (both: four
  made-up players, coop_party_hud.cpp TestBlocks; the 4th, or all four on B, need a run-time gauge = MakeLive, the
  crash's path) and B:COOP_TEST_PARTY_HUD_NO_PACK=1 (B as without the pack: coop_party_hud.h Hud::IgnorePack).
  LICHII_COOP_TEST_TO_TITLE=1 is a base switch of every boot (bt_plugin).
- "coop: party hud: on the game's HUD": hookshot/src/coop_hookshot.cpp PartyStep (logged when Hud::Step turns true;
  "hidden (the game's HUD has none of ours now..." when it turns false).
- "run-time gauges are off (the game's part clone crashes outside a screen's own set-up)", "test: the pack's panes
  are not looked for (no pack)", "the HUD has no hearts of ours (an older pack): the other players' hearts are hidden":
  coop_party_hud.cpp Hud::Find (kSay "coop: party hud: "). The module makes no run-time clone (party-hud-crash-c126,
  88f63dfb1: the clone crashed every game that ran it): A (the pack, 3 slots) shows three players and "+1"; B (as
  without the pack) shows none of ours, and makes nothing, so it must never crash and never say the HUD is on.
- "CRASH: exception": the game's own crash report (offline/lib/game-crash.ps1, its source lines).
- "coop: party hud probe:": coop_hookshot.cpp PartyProbe (LICHII_COOP_PARTY_HUD_PROBE=1): with it on the HUD makes
  no game call, so a run with it proves nothing here.
- to-title.now and its log line: test_smoke.py test_title_and_load_again (hookshot/src/coop_pause.cpp).
- shots: the HUD's corner where the blocks show is the game's own HUD (worker-rules TEST SYSTEM: HUD UI by picture);
  tolerance 0.80 as test_smoke's pause page. The test players' hearts change every 1.5 s (kTestStepMs), so a shot
  may differ from the reference by one heart's fill: a person judges the NO-REF candidates before any ref is kept.
"""
import os

import pytest

from test_smoke import AT_TITLE, TITLE_FRAMES, TO_TITLE_ASKED, TO_TITLE_GAVE_UP   # (one copy of the title lines)

pytestmark = pytest.mark.needs_env("COOP_TEST_PARTY_HUD=1", "B:COOP_TEST_PARTY_HUD_NO_PACK=1")

GAMES = ("A", "B")
HUD_ON = r"coop: party hud: on the game's HUD"
HUD_HIDDEN = r"coop: party hud: hidden \(the game's HUD has none of ours now"
GAUGES_OFF = r"coop: party hud: run-time gauges are off \(the game's part clone crashes outside a screen's own set-up\)"
NO_PACK = r"coop: party hud: test: the pack's panes are not looked for \(no pack\)"
B_HIDDEN = r"coop: party hud: the HUD has no hearts of ours \(an older pack\): the other players' hearts are hidden"
CRASH = r"CRASH: exception"
PROBE = r"coop: party hud probe:"


def crash_why(pair, x):
    """': CRASH ...' when game x's log has the game's crash report (for a SETUP-FAIL's why), else ''."""
    try:
        line = pair.log[x].find(CRASH, pos=0)
    except OSError:
        return ""
    return ": %s" % line.strip() if line else ""


def no_crash_logged(pair):
    """The games' logs only (no bridge call: a crashed game has none, and a call to it would fail the set-up first)."""
    for x in GAMES:
        assert pair.log[x].find(CRASH, pos=0) is None, "%s crashed: %s" % (x, pair.log[x].find(CRASH, pos=0).strip())
        assert pair.log[x].find(PROBE, pos=0) is None, "%s runs the probe (LICHII_COOP_PARTY_HUD_PROBE): no HUD calls" % x


def no_crash(pair):
    no_crash_logged(pair)
    for x in GAMES:
        assert pair.state(x).get("control") is True, "%s: the bridge does not answer" % x


def hud_shots(pair, name):
    pair.step_both(30)                      # (the blocks' first frames: shown, not fading in)
    for x in GAMES:
        pair.check_shot(x, name, tolerance=0.80)


@pytest.mark.budget(120)
@pytest.mark.look
@pytest.mark.row("party-hud-crash-c126: boot -> title -> save load, the party HUD on the game's HUD, no crash (hs-party-hud-probe)")
def test_party_hud_after_boot(pair):
    no_crash_logged(pair)   # (first, before any bridge call: a crash in the boot's load is this test's FAIL)
    with pair.setup():
        for x in GAMES:
            assert pair.in_play(pair.state(x)), "%s not in play after the boot%s" % (x, crash_why(pair, x))
    no_crash(pair)   # (a crash is this test's FAIL, never its set-up's)
    pair.until("A: the party HUD on the game's HUD since the boot",
               lambda: pair.log["A"].find(HUD_ON, pos=0), max_frames=300)
    for x in GAMES:
        assert pair.log[x].find(GAUGES_OFF, pos=0), "%s: no 'run-time gauges are off' line (the clones must be off)" % x
    assert pair.log["B"].find(NO_PACK, pos=0), "B: COOP_TEST_PARTY_HUD_NO_PACK not on"
    assert pair.log["B"].find(B_HIDDEN, pos=0), "B (no pack): not hidden"
    assert pair.log["B"].find(HUD_ON, pos=0) is None, "B (no pack, no clones) says the HUD is on"
    hud_shots(pair, "party-hud-boot")
    no_crash(pair)


@pytest.mark.budget(300)
@pytest.mark.look
@pytest.mark.leaves_play
@pytest.mark.row("party-hud-crash-c126: back to the title, menu -> load again, the party HUD back, no crash")
def test_party_hud_title_and_load_again(pair):
    no_crash_logged(pair)
    with pair.setup():
        for x in GAMES:
            assert pair.in_play(pair.state(x)), "%s not in play before the way back to the title%s" % (x, crash_why(pair, x))
    no_crash(pair)
    asked = {x: pair.log[x].mark() for x in GAMES}
    for x in GAMES:
        pair.trigger(x, "to-title.now")
    for x in GAMES:
        pair.expect_log(x, TO_TITLE_ASKED, max_frames=300)

    def at_title(x):
        return pair.log[x].find(AT_TITLE, pos=asked[x]) is not None

    def both_at_title():
        for x in GAMES:
            gave_up = pair.log[x].find(TO_TITLE_GAVE_UP, pos=asked[x])
            assert gave_up is None, "%s: the way back to the title gave up: %s" % (x, gave_up.strip())
        return all(at_title(x) and not pair.in_play(pair.state(x)) for x in GAMES)
    # (at the title = the title's own line since the ask, as test_smoke's title test: 'not in play' alone also holds
    # on an open pause menu, and load_both presses A only on a game at the title)
    pair.until("both games at the title", both_at_title, max_frames=TITLE_FRAMES)
    pair.expect_log("A", HUD_HIDDEN, max_frames=600)
    no_crash(pair)
    pair.load_both(at_title, max_frames=3600)
    for x in GAMES:
        assert pair.in_play(pair.state(x)), "%s not in play after the load" % x
    pair.expect_log("A", HUD_ON, max_frames=600)
    pair.expect_log("B", NO_PACK, max_frames=600)    # (B's HUD looked in again after the load: still none of ours)
    assert pair.log["B"].find(HUD_ON) is None, "B (no pack, no clones) says the HUD is on after the load"
    hud_shots(pair, "party-hud-reload")
    no_crash(pair)
