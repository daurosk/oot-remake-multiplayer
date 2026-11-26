"""Rows 138 item 4 and 137 (a guest holds Gohma beaten, then the HOST loads its save from before her) as a bridge test: the
port of coord offline/boss-away-0115-probe.ps1's scenario hs-boss-away-reset (scenarios/hs-boss-away-reset.steps.txt,
-kill.B.txt, -after.B.txt; the module code it proves: lichii/hook/coop_hook.cpp boss_beaten_ revert writes,
coop_boss_game.cpp:642 the host's load, coop_objects.cpp:4788 the max life held lower).

| Test | Old row | Boot + save | Kind |
|---|---|---|---|
| test_boss_reset_host_loads_before_her | hs-boss-away-reset (138 (4) + 137) | live-deku-gohma-reset + --save gohma-alive | B kills her, A (host) loads its save with her alive: B's beaten flags put back once out of her area, her room has no warp, Gohma there; 137: B's container heart taken back |

Run: pytest tools/autotest/ingame --boot live-deku-gohma-reset --save gohma-alive --env LICHII_COOP_DEBUG_BOSS=1
--env LICHII_COOP_DEBUG_OBJECTS=1 (the old steps' '#! switches'; DEBUG_FLAGS gave info lines only, CHEATS is the
runner's own: docs/playtest.md "The verb gate").

The save: the old run wrote Gohma alive into BOTH lanes' save files (Set-Saves 'gohma-alive'); here --save gohma-alive
(testsave.py, saves/gohma-alive.py) does that, and the boot sets none of those flags, so testsave.prove() reading them
back from each game proves the game runs that save. A's reload in the test reads A's FILE, which is why it must hold her
alive (a setflag after landing would be lost by the reload).

The old A stayed on the title from the start while B played; the bridge boot proof needs both games in play, so A goes
to the title inside the test first (to-title.now, the smoke test's way: test_smoke.test_title_and_load_again) before B
starts the fight, and nothing in A's play changed her flags (the boot sets only the four room flags of the old -kill
file, never her own). Then B kills her alone, A loads, B's checks follow the old -after.B.txt.

Every place and wait is the old scenario's (cited); every log pattern is the module's own line (cited at its writer).
Judged by game state (log lines, flag reads, world list); shots on B of what a player sees (the warp up after the kill,
no warp after the put-back) and A after its load. No references yet: NO-REF until the owner approves them.
The run changes the dungeon for good, so the test runs once per boot.
"""
import os

import pytest

import testsave
from test_boss_away import (CORRIDOR, DEATH, DEFEAT, DOOR, GOHMA, KILLS, LOOK_UP, ROOM_E, SCENE, TRIGGER, UNDER,
                            goto)                                     # noqa: E402  (hs-boss-away-reset-kill.B.txt = the same walk)
from test_enemies import actors, alive, place                        # noqa: E402
from test_smoke import TO_TITLE_ASKED, TO_TITLE_ON                   # noqa: E402

BOOT, SAVE = "live-deku-gohma-reset", "gohma-alive"
ENV = ("LICHII_COOP_DEBUG_BOSS=1", "LICHII_COOP_DEBUG_OBJECTS=1")    # hs-boss-away-reset.steps.txt '#! switches'

# waits of hs-boss-away-reset-kill.B.txt (the walk-in is test_boss_away's, same lines)
B_WAIT, INTRO, DEATH_WAIT = 600, 2400, 120 + 2400   # 'wait 600' after place, 'wait 2400 # the door's event and the
#                                                     intro', 'mark b-kill; wait 2400 # her death and its event'
CENTRE = (7.5, 67.2, 1.5, 900)        # 'goto 7.5 67.2 1.5 900 # the room's centre, where the heart container falls'
PICK = (9.0, 67.2, 1.5, 300)          # 'goto 9.0 67.2 1.5 300'
CENTRE_WAIT, PICK_WAIT = 600, 900     # 'wait 600' after CENTRE, 'wait 900' + 'mark b-pickup'
LOADED_WAIT = 30 * 60                 # steps 'load A; sleep 30' then the 'forgotten' line (30 s at 60 frames/s)
# hs-boss-away-reset-after.B.txt
IN_AREA, OUT_WAIT, CORR_WAIT, BACK_WAIT = 300, 600, 240, 1200   # 'wait 300; mark b-in-area', place room E 'wait 600;
#                                                                 mark b-out', corridor 'wait 240', 'wait 1200; mark b-back'
TITLE_FRAMES, LOAD_FRAMES = 900, 3600   # test_smoke.test_title_and_load_again's waits (title, load_both)

FORGOTTEN = r"coop: boss: the host loaded a save: the fights this session knew are forgotten"   # coop_boss_game.cpp:642
REVERTED = r"coop: boss: Queen Gohma is alive in the host's save: Gohma_DefeatBoss = 0 \(written\)"
#           coop_hook.cpp:1707 (w.revert; set->boss "Queen Gohma", w.name of coop_boss_beaten.inc:8)
REVERT_ANY = r"coop: boss: Queen Gohma is alive in the host's save: "                          # (any of her flags)
REVERT_INTRO = r"coop: boss: Queen Gohma is alive in the host's save: HavePlayedEvent\.Dm_Deku_BossIn"   # steps PASS 2:
#                                                                 'never Dm_Deku_BossIn' (her intro kept)
LOWERED = (r"coop: objects: our Link's max life \d+ -> \d+ \(MaxLife is held lower: the host loaded a save with fewer "
           r"hearts")                                                                      # coop_objects.cpp:4788


def to_title(pair, x):
    """Game x back to the title (LICHII_COOP_TEST_TO_TITLE, a BASE_ENV switch of every boot), the other stepped."""
    assert pair.log[x].find(TO_TITLE_ON, pos=0), "%s: LICHII_COOP_TEST_TO_TITLE is not on" % x
    with open(os.path.join(pair.games[x]["folder"], "to-title.now"), "w") as f:
        f.write("bt\n")
    pair.expect_log(x, TO_TITLE_ASKED, max_frames=300)
    pair.until("%s left play (title)" % x, lambda: not pair.in_play(pair.state(x)), max_frames=TITLE_FRAMES)


@pytest.mark.budget(1200)   # ESTIMATED: ~16'000 game frames (the old scenario's waits) + a title + a load + shots
@pytest.mark.look
@pytest.mark.needs_boot(BOOT)
@pytest.mark.needs_save(SAVE)
@pytest.mark.needs_env(*ENV)
@pytest.mark.row("#138(4)+#137 the host loads its save from before a boss a guest beat: the guest's beaten flags and "
                 "container go back (no warp, the boss there)")
def test_boss_reset_host_loads_before_her(pair):
    if any(pair.log[x].find(DEATH, pos=0) for x in ("A", "B")):
        pytest.skip("Gohma died earlier on this boot: the dungeon has no Gohma to kill")
    with pair.setup():
        testsave.prove(pair, SAVE)                     # both in play, child, her five flags read from the save
        for x in ("A", "B"):
            s = pair.state(x)
            assert (s.get("scene") or "").lower() == SCENE.lower(), "%s is in %s, not %s" % (x, s.get("scene"), SCENE)
    at = pair.log["B"].mark()
    to_title(pair, "A")                                # the old A: on the title while B fights
    place(pair, "B", CORRIDOR)
    pair.step_both(B_WAIT)
    for g in DOOR:
        goto(pair, "B", g)
    pair.step_both(INTRO)
    goto(pair, "B", UNDER)
    pair.press("B", rstick=LOOK_UP[0], frames=LOOK_UP[1])
    pair.press("B", buttons=["ZL"], frames=LOOK_UP[2])
    pair.step_both(TRIGGER)
    if not alive(actors(pair, "B", GOHMA)):
        pair.unproven("no live %s in B's world list after its walk-in (VOID: the save's set did not make her body)" % GOHMA)
    life0 = pair.state("B").get("max_life")
    for w in KILLS:
        pair.step_both(w)
        pair.act("B", "kill %s" % GOHMA)
    try:
        pair.expect_log("B", DEATH, max_frames=DEATH_WAIT)          # PASS 1 (b-kill)
    except AssertionError as e:
        pair.unproven("the kill did not land (VOID: no death line): %s" % e)
    goto(pair, "B", CENTRE)
    pair.step_both(CENTRE_WAIT)
    goto(pair, "B", PICK)
    pair.step_both(PICK_WAIT)
    life1 = pair.state("B").get("max_life")
    assert pair.flag_read("B", DEFEAT).get("value"), "B: %s not 1 after her death" % DEFEAT
    pair.check_shot("B", "reset-b-pickup")             # PASS 1: the warp up (and her container gone if taken)
    pair.load_both(max_frames=LOAD_FRAMES)             # 'load A': only A is off play
    s = pair.state("A")
    assert pair.in_play(s), "A not in play after its load"
    assert not pair.flag_read("A", DEFEAT).get("value"), "A: %s set after loading its save (the file holds her alive)" % (
        DEFEAT)
    pair.expect_log("B", FORGOTTEN, max_frames=LOADED_WAIT)     # PASS 2, first half
    pair.check_shot("A", "reset-a-loaded")
    pair.step_both(IN_AREA)                            # b-in-area: still in her area, nothing put back yet
    put_back_early = [ln for ln in pair.log["B"].since(at) if "is alive in the host's save" in ln]
    assert not put_back_early, "B put her flags back while still in her area: %s" % put_back_early[0]
    place(pair, "B", ROOM_E)                           # out of her area
    pair.expect_log("B", REVERTED, max_frames=OUT_WAIT)          # PASS 2: only after b-out
    pair.never_log("B", REVERT_INTRO)
    place(pair, "B", CORRIDOR)
    pair.step_both(CORR_WAIT)
    for g in DOOR:
        goto(pair, "B", g)
    pair.step_both(BACK_WAIT)                          # b-back
    assert not pair.flag_read("B", DEFEAT).get("value"), "B: %s still 1 back in her room" % DEFEAT
    g = alive(actors(pair, "B", GOHMA))
    assert g, "B: no live %s in her room after the host's load (A's save has her alive)" % GOHMA   # PASS 3
    pair.check_shot("B", "reset-b-back")               # PASS 3: NO blue warp, Gohma there
    reverts = [ln for ln in pair.log["B"].since(at) if REVERT_ANY in ln]
    pair.notes.append("B max life: %s before the fight, %s at the pickup, %s now; %d put-back line(s)" % (
        life0, life1, pair.state("B").get("max_life"), len(reverts)))
    # row 137 (PASS 4): only if B took her container (max life up at the pickup)
    if life0 is None or life1 is None or life1 <= life0:
        pair.unproven("row 137 not proven: B took no heart container (max life %s -> %s at the pickup); row 138(4) "
                      "checks all passed" % (life0, life1))
    pair.expect_log("B", LOWERED, max_frames=60)
    now = pair.state("B").get("max_life")
    assert now is not None and now < life1, "B: max life %s after the host's load, not below the pickup's %s" % (
        now, life1)
