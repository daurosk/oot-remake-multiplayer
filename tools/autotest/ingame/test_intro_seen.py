"""Row 244 (a) (a player whose save saw a boss's intro before must not own the boss while its game still plays that
intro) as a bridge test: the port of coord offline/gohma-244-rerun.ps1's scenario hs-intro-seen-gohma
(scenarios/hs-intro-seen-gohma.A/B.txt; the module code it proves is on this base: lichii/hook/coop_boss_game.cpp's
readiness, never ready while our game plays the intro, Sync::InIntro).

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_intro_seen_gohma | hs-intro-seen-gohma (row 244 (a)) | live-deku-gohma-seen | A walks in and owns Gohma; B (its save saw the intro) walks in: not ready while its intro plays, A stays the owner, both Links move after (log + state + shots) |

Boot: scenarios/live-deku-gohma-seen.A/B.txt (run with --boot live-deku-gohma-seen): live-deku-gohma with B's
HavePlayedEvent.Dm_Deku_BossIn 1 (hs-intro-seen-gohma.B.txt's setflag), read back in the set-up. Its own boot: the
test needs a live Gohma that no other test killed, and it changes the boss room for good (runs once per boot).

What the old run judged and how it is judged here:
 1. B while its intro plays logs 'not ready to own the boss: our game plays its intro now (Dm_Deku_BossIn)' and no
    'ready to own the boss' line until its event is over (state `event`, read at the same frame as the log: the bridge
    steps both games and they wait between calls). No such line at all = B's game played no intro: NO-REF (the old
    run's INCONCLUSIVE), never PASS.
 2. A owns her through B's intro: A 'native boss sync on (BSA ..., ours)', B '... a copy', B 'the owner's verbs came';
    B never 'Enemy_DungeonBoss_Gohma: now ours'.
 3. Both Links have control after the intro: each one's goto arrives (A's after its own intro, B's after its), and a
    live Gohma is in both world lists; shots on each game (what a player sees: Gohma in the same place on both
    screens, not where an intro left her). No references yet: NO-REF until the owner approves them.
 0. VOID (NO-REF) as the old run's: no 'native boss sync on (..., ours)' in A after A's trigger.
Every log pattern is the module's own line (cited at its writer); every place and wait is the old scenario's (cited).
"""
import pytest

from test_enemies import actors, alive, place, walk_to   # noqa: E402  (the same world list, place and goto)
import boss_lines as BL   # noqa: E402  (the native boss table's lines: one table)

BOOT, SCENE = "live-deku-gohma-seen", "InsideTheDekuTree"   # the boot's 'warp InsideTheDekuTree Entrance'
GOHMA = "Enemy_DungeonBoss_Gohma"                         # hs-intro-seen-gohma.A.txt 'kill Enemy_DungeonBoss_Gohma'
SEEN = "HavePlayedEvent.Dm_Deku_BossIn"                   # live-deku-gohma-seen.B.txt 'setflag ... 1' (A's: 0)
ENV = ("LICHII_COOP_DEBUG_BOSS=1", "LICHII_COOP_DEBUG_OBJECTS=1")   # hs-intro-seen-gohma.A.txt '#! switches' (+ CHEATS=1)

# places (x y z) and gotos (x z tol frames: the scenario's 'goto X Z TOL FRAMES', test_enemies.walk_to) of hs-intro-seen-gohma.A/B.txt
CORRIDOR = (-12.0, 48.5, 66.5)        # A and B 'place -12.0 48.5 66.5 # the corridor, 11 m before the door'
DOOR = [(-4.0, 67.5, 1.0, 600), (6.7, 67.6, 1.5, 600)]   # 'goto -4.0 67.5 1.0 600', 'goto 6.7 67.6 1.5 600 # through the door'
UNDER = (17.0, 67.8, 1.5, 600)              # A 'goto 17.0 67.8 1.5 600 # CONTROL + her drop: under her ceiling spot'
A_MOVED = (12.0, 67.5, 1.5, 600)            # A 'goto 12.0 67.5 1.5 600 # inside the room, 6.5 m west of her' + 'mark a-moved'
B_MOVED = (15.0, 67.5, 1.5, 600)            # B 'goto 15.0 67.5 1.5 600 # CONTROL: 3.5 m west of her' + 'mark b-moved'
CORRIDOR_WAIT, INTRO, TRIGGER = 240, 2400, 900   # 'wait 240' after the place; 'wait 2400 # the door's event, the intro,
#                                                  Gohma's own appear event'; A 'wait 900' + 'mark a-trigger'
LOOK_UP = ([0.0, 1.0], 60, 10)        # A 'rstick 0 1 60' (bridge rstick [angle, strength]), 'press ZL 10'
WATCH = 600                           # A 'wait 600' + 'mark a-watch1' (B's copy after its intro)

# The native boss table's notes (coop_boss_native.cpp:527/1512/393), logged 'coop: bosses: <note>' by coop_objects.cpp:
# the one table boss_lines.py (row 303 VOID 11-28: 'coop: boss:' here never matched a live line).
SYNC = BL.sync_re(GOHMA, "%s")
VERBS = BL.verbs_re(GOHMA)
NOW_OURS = BL.now_ours_re(GOHMA)
NOT_READY = r"coop: boss: not ready to own the boss: our game plays its intro now \(Dm_Deku_BossIn\)"  # coop_boss_game.cpp:367
DEATH = r"Dm_Deku_BossOut: our game started the death"   # coop_boss_scenes.cpp:458 (as test_boss_away.DEATH)
READY = r"coop: boss: ready to own the boss \(Dm_Deku_BossIn\)"                # coop_boss_game.cpp:366 (room->intro, coop_boss.cpp:52)


def goto(pair, x, g):
    return walk_to(pair, x, g[0], g[1], tol=g[2], max_frames=g[3])


@pytest.mark.budget(600)   # ESTIMATED: ~8'000 game frames (the old scenario's waits up to b-moved + a-watch1) + shots
@pytest.mark.look
@pytest.mark.needs_boot(BOOT)
@pytest.mark.needs_env(*ENV)
@pytest.mark.row("#244 (a) a player whose save saw the intro is not the boss's owner while its game plays the intro")
def test_intro_seen_gohma(pair):
    if any(pair.log[x].find(DEATH, pos=0) for x in ("A", "B")):
        pytest.skip("Gohma died earlier on this boot: the test needs her alive")
    with pair.setup():
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
            assert (s.get("scene") or "").lower() == SCENE.lower(), "%s is in %s, not %s" % (x, s.get("scene"), SCENE)
        a, b = pair.flag_read("A", SEEN).get("value"), pair.flag_read("B", SEEN).get("value")
        assert not a, "A: %s is %s, not 0 (the boot's setflag)" % (SEEN, a)
        assert b, "B: %s is %s, not 1 (live-deku-gohma-seen.B.txt's setflag)" % (SEEN, b)
        place(pair, "A", CORRIDOR)
    # A walks in first and owns her (hs-boss-fix-gohma's A)
    pair.step_both(CORRIDOR_WAIT)
    for g in DOOR:
        goto(pair, "A", g)
    pair.step_both(INTRO)
    goto(pair, "A", UNDER)
    pair.press("A", rstick=LOOK_UP[0], frames=LOOK_UP[1])
    pair.press("A", buttons=["ZL"], frames=LOOK_UP[2])
    pair.step_both(TRIGGER)
    if not pair.log["A"].find(SYNC % "ours", pos=0):   # (whole log: the walk may see her before the test)
        pair.unproven("A does not own Gohma after its trigger (hs-intro-seen-gohma VOID: no 'native boss sync on "
                      "(..., ours)' in A)")
    assert goto(pair, "A", A_MOVED), "A has no control after its intro (its goto to %s did not arrive)" % (A_MOVED,)
    # B (its save saw the intro) walks in: its own intro, with a copy Gohma
    place(pair, "B", CORRIDOR)
    pair.step_both(CORRIDOR_WAIT)
    for g in DOOR:
        goto(pair, "B", g)
    try:
        pair.expect_log("B", NOT_READY, max_frames=INTRO)
    except AssertionError as e:
        pair.unproven("B's game played no intro (its flag skipped it; the old run's INCONCLUSIVE): %s" % e)
    at = pair.log["B"].mark()       # the not-ready line is before this point

    def ready_after_intro():
        line = pair.log["B"].find(READY, pos=at)
        st = pair.state("B")
        assert not (line and st.get("event")), "B logged '%s' while its intro still plays (event on)" % line
        return line
    pair.until("B ready to own the boss after its intro", ready_after_intro, max_frames=INTRO)
    assert goto(pair, "B", B_MOVED), "B has no control after its intro (its goto to %s did not arrive)" % (B_MOVED,)
    assert pair.log["B"].find(SYNC % "a copy", pos=0), "B: no 'native boss sync on (..., a copy)' for Gohma"
    pair.expect_log("B", VERBS, max_frames=WATCH)
    pair.step_both(WATCH)
    pair.never_log("B", NOW_OURS)
    for x in ("A", "B"):
        g = alive(actors(pair, x, GOHMA))
        assert g, "%s: no live Gohma in the boss room after B's intro" % x
        pair.notes.append("%s: Gohma at %s" % (x, g[0].get("pos")))
    for x in ("A", "B"):
        pair.check_shot(x, "gohma-intro-seen-moved", tolerance=0.80)   # a-watch1 / b-moved: tolerance as test_gear.shots
