"""Row 136 (a boss beaten by the group while a player is elsewhere in the same dungeon) as a bridge test: the port of coord
offline/boss-away-0115-probe.ps1's scenario hs-boss-away-dungeon (scenarios/hs-boss-away-dungeon.A/B.txt; the module code
it proves is on this base: lichii/hook/coop_boss_beaten.* + coop_hook.cpp's beaten-set writes).

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_boss_away_dungeon | hs-boss-away-dungeon (row 136) | live-deku-gohma | B kills Gohma, A in room E gets her set live (log + flag), A's room: no Gohma, warp (shots) |

Not ported here yet: hs-boss-away-reset (rows 137 and 138 item 4): the HOST loads its save from before Gohma while a guest
holds her beaten, so the host's save FILE must hold her alive (the old run wrote it with save_flags.py, profile
gohma-alive) and A must stay on the title until B has killed her. That needs a harness save-profile option (asked of
harness-c132; reports/port-sweep-c131-5.md).

Boot: scenarios/live-deku-gohma.A/B.txt (run with --boot live-deku-gohma): both games past the title in the Deku Tree with
her set alive (the old scenario's setflag lines, plus the quest step the old run wrote into the save as a setenum after
landing). That setenum-after-landing is new: the set-up reads it back, and the test VOIDs (NO-REF) unless B's walk-in made
her body. The run changes the dungeon for good (Gohma beaten in both games), so the test runs once per boot.

Every log pattern is the module's own line (cited at its writer); every place and wait is the old scenario's (cited).
Judged by game state (module log lines, Gohma_DefeatBoss read back, each world list), with shots on each game of the boss
room (what a player sees: the blue warp and no Gohma). No references yet: NO-REF until the owner approves them.
"""
import pytest

from test_enemies import actors, alive, place, walk_to   # noqa: E402  (the same world list, place and goto)
from test_gear import shots                              # noqa: E402

BOOT, SCENE = "live-deku-gohma", "InsideTheDekuTree"     # the boot's 'warp InsideTheDekuTree Entrance'
GOHMA = "Enemy_DungeonBoss_Gohma"                        # hs-gohma-kill.inc.txt 'kill Enemy_DungeonBoss_Gohma'
QUEST, ALIVE_STEP = "0x7d776b61", "0xb5df43cb"           # Get_StoneOfForest = GoToBossRoom (boss-away-0115-probe.ps1
#                                                          gohma-alive; coop_boss_beaten.inc:5,88: below GoToWarpPoint)
DEFEAT = "Gohma_DefeatBoss"                              # coop_boss_beaten.inc:8 (her room's warp: Create_WarpPrefab)
ENV = ("LICHII_COOP_DEBUG_BOSS=1", "LICHII_COOP_DEBUG_OBJECTS=1")   # hs-boss-away-dungeon.A.txt '#! switches' (+ CHEATS=1)

# places (x y z) and gotos (x z tol frames) of hs-boss-away-dungeon.A/B.txt
ROOM_E = (-14.5, 79.6, -6.5)          # A 'place -14.5 79.6 -6.5 # room E ... far from Gohma's area'
CORRIDOR = (-12.0, 48.5, 66.5)        # 'place -12.0 48.5 66.5 # the corridor before Gohma's door'
DOOR = [(-4.0, 67.5, 1.0, 600), (6.7, 67.6, 1.5, 600)]   # 'goto -4.0 67.5 1.0 600', 'goto 6.7 67.6 1.5 600 # through the door'
UNDER = (17.0, 67.8, 1.5, 600)        # B 'goto 17.0 67.8 1.5 600 # under her ceiling spot'
CENTRE = (7.5, 67.2, 1.5, 600)        # A 'goto 7.5 67.2 1.5 600 # toward the room's centre (the warp)'
B_WAIT, INTRO, TRIGGER = 600, 2400, 900   # B 'wait 600' after its place, 'wait 2400 # the door's event and the intro',
#                                           'wait 900' + 'mark b-trigger'
LOOK_UP = ([0.0, 1.0], 60, 10)        # B 'rstick 0 1 60' (bridge rstick [angle, strength]), 'press ZL 10'
KILLS = (150, 120, 120)               # hs-gohma-kill.inc.txt: 'wait 150; kill', 'wait 120; kill', 'wait 120; kill'
DEATH_WAIT = 120 + 2400               # B 'wait 120; mark b-kill; wait 2400 # her death and its event'
# A's set must be in A's log while A is still in room E: the old A file's 'mark a-kill' (about B's b-kill), 'wait 600;
# mark a-given', 'place' (corridor), 'wait 240; mark a-walk': 840 frames from the kill
GIVEN_FRAMES = 600 + 240
ROOM_WAIT, END_WAIT = 600, 300        # A 'wait 600; mark a-room', 'wait 300; mark a-end'
STILL_M = 0.5                         # A gets no input in room E: the bridge's place is exact; 0.5 m = the gotos' own
#                                       tolerance (test_enemies.WALK_TOL), room for the landing

DEATH = r"Dm_Deku_BossOut: our game started the death"   # coop_boss_scenes.cpp:458 (label = the flow, :392)
GIVEN = r"coop: boss: Queen Gohma was beaten by the group, not in this game: Gohma_DefeatBoss = 1 \(written\)"
#        coop_hook.cpp:1712 (set->boss "Queen Gohma", coop_boss_beaten.inc:88; w.name/value of :8)


def goto(pair, x, g):
    return walk_to(pair, x, g[0], g[1], tol=g[2], max_frames=g[3])


@pytest.mark.budget(900)   # ESTIMATED: ~11'000 game frames (the old scenario's waits) + shots
@pytest.mark.look
@pytest.mark.needs_boot(BOOT)
@pytest.mark.needs_env(*ENV)
@pytest.mark.row("#136 a boss beaten while a player is elsewhere in her dungeon: that game gets her set live (warp, no boss)")
def test_boss_away_dungeon(pair):
    if any(pair.log[x].find(DEATH, pos=0) for x in ("A", "B")):
        pytest.skip("Gohma died earlier on this boot: the dungeon has no Gohma to kill")
    with pair.setup():
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
            assert (s.get("scene") or "").lower() == SCENE.lower(), "%s is in %s, not %s" % (x, s.get("scene"), SCENE)
            q = pair.flag_read(x, QUEST).get("value")
            assert str(q).lower() == ALIVE_STEP, "%s: Get_StoneOfForest is %s, not GoToBossRoom %s (the boot's setenum)" % (
                x, q, ALIVE_STEP)
            d = pair.flag_read(x, DEFEAT).get("value")
            assert not d, "%s: %s is %s before the fight (the boot's setflag 0)" % (x, DEFEAT, d)
        place(pair, "A", ROOM_E)
        place(pair, "B", CORRIDOR)
    at = {x: pair.log[x].mark() for x in ("A", "B")}
    pair.step_both(B_WAIT)
    for g in DOOR:
        goto(pair, "B", g)
    pair.step_both(INTRO)
    goto(pair, "B", UNDER)
    pair.press("B", rstick=LOOK_UP[0], frames=LOOK_UP[1])
    pair.press("B", buttons=["ZL"], frames=LOOK_UP[2])
    pair.step_both(TRIGGER)
    if not alive(actors(pair, "B", GOHMA)):
        pair.unproven("no live %s in B's world list after its walk-in (VOID: the boot's set did not make her body)" % GOHMA)
    for w in KILLS:
        pair.step_both(w)
        pair.act("B", "kill %s" % GOHMA)
    try:
        pair.expect_log("B", DEATH, max_frames=DEATH_WAIT)
    except AssertionError as e:
        pair.unproven("the kill did not land (hs-boss-away-dungeon VOID: no death line): %s" % e)
    pair.expect_log("A", GIVEN, max_frames=GIVEN_FRAMES)
    s = pair.state("A")
    assert walk_dist(s["pos"], ROOM_E) <= STILL_M, "A left room E before its set came (A at %s)" % s["pos"]
    assert pair.flag_read("A", DEFEAT).get("value"), "A: %s not 1 after the 'written' line" % DEFEAT
    place(pair, "A", CORRIDOR)
    pair.step_both(240)                       # 'wait 240; mark a-walk'
    for g in DOOR:
        goto(pair, "A", g)
    pair.step_both(ROOM_WAIT)
    for x in ("A", "B"):
        g = alive(actors(pair, x, GOHMA))
        assert not g, "%s: Gohma still alive in the boss room: %s" % (x, g)
    shots(pair, "gohma-away-room")            # A's a-room / B's b-end: her room, the blue warp up, no Gohma
    goto(pair, "A", CENTRE)
    pair.step_both(END_WAIT)
    shots(pair, "gohma-away-end")             # A at the warp
    for x in ("A", "B"):
        pair.notes.append("%s: %d boss lines" % (x, sum(1 for ln in pair.log[x].since(at[x]) if "coop: boss" in ln)))


def walk_dist(p, q):
    return ((p[0] - q[0]) ** 2 + (p[2] - q[2]) ** 2) ** 0.5
