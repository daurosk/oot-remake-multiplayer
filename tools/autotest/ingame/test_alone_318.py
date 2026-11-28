"""Rows 318 + 334 (down but not out only with a teammate nearby; dbno-alone-318's file).

| Test | Row | Kind |
|---|---|---|
| test_lethal_hit_with_nobody_nearby_goes_to_the_entrance | 334: A in another place, B's lethal hit: no downed wait | log + scene + life + LOOK |
| test_downed_teammate_leaves_goes_to_the_entrance | 334 control (A beside: downed), then A leaves: the entrance early | log + scene + LOOK |
| test_watched_teammate_leaves_and_the_spectator_goes_to_the_entrance | 318: B watches A, A warps away | log + scene + LOOK |

The fix under test: lichii/hook/coop_dbno.cpp Machine (Notice / Nearby / ToEntranceAlone, kNearbyGraceUs) and
coop_dbno_game.cpp ShownHere (the spectating camera's rule: same place and, in a dungeon, a room CheckMove allows).
B is the one hit, A (P1, the host) sets the rules and is the teammate who is or is not nearby. The helpers (places,
rules, hits, the restore) are test_dbno_crawl.py's, imported, never copied; every log pattern is a module line, cited.
Test names avoid the word 'dbno': tests/test_ingame_dbno_crawl.py selects its rows with '-k dbno'.
"""
import contextlib
import os
import sys
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_dbno_crawl as crawl   # noqa: E402  (to_place, rules, no_fairy, down, shots, event, _restore, BOOT)

# lichii/hook/coop_dbno.h enum class Event: went_down 0, ..., bled_out 2, ..., respawned 7, alone 8; logged as
# "coop: dbno: our Link: event N (by PM)" (coop_dbno_game.cpp, the events loop).
EV_ALONE = 8
FIELD = ("KokiriForest_Child", "FrontDekuTree")   # test_dbno_crawl.py's child place (hs-dbno-child: open ground)
AWAY = crawl.BOOT                                 # A's other place: the default boot's Kakariko spot (another scene)
# The bleed-out this row plays with: kDbnoMinBleedS is 10 (test_dbno_crawl.py adult row); 12 is this row's own (no
# other row sets it, so the fake games know this row by it: fakes/alone_318.py).
BLEED_S = 12
SPECTATE_BACK_S = 6       # coop_dbno.h kQuietUs 3 s + kSpectateMinUs 3 s: the soonest a watcher goes beside
EARLY_FRAMES = 150         # 5 s of game: the 1 s grace + a margin, well under BLEED_S (12 s = 360 frames)
RESPAWN_LIFE = 12          # coop_dbno.h kRespawnLife (capped at the player's own max)
ENTRANCE = r"coop: teleport: respawn at the entrance \S+ of %s"   # coop_teleport_game.cpp, the want_entrance branch
BESIDE = r"coop: dbno: respawn: moving beside P"                  # coop_dbno_game.cpp (move_beside)
RESPAWNED_ALONE = r"coop: dbno: our Link: event 7 \(by P0\)"      # respawned with nobody watched: the entrance


def scene_of(pair, x):
    return (pair.state(x).get("scene") or "").lower()


def apart(pair):
    """A to the other place, B stays: stepped until A is in play there and B still in FIELD."""
    pair.warp_one("A", *AWAY)

    def there():
        return (pair.in_play(pair.state("A")) and scene_of(pair, "A") == AWAY[0].lower()
                and pair.in_play(pair.state("B")) and scene_of(pair, "B") == FIELD[0].lower())
    pair.until("A in %s, B in %s" % (AWAY[0], FIELD[0]), there)


def lethal(pair, x="B"):
    """The game's own hits ('hurt 4', test_dbno_crawl.py down()'s) until x is downed (event 0) or sent to the entrance
    with nobody nearby (event 8). Returns which came first."""
    life = pair.state(x).get("life") or 0
    hits = life // crawl.HIT + 2
    for _ in range(hits):
        pair.act(x, "hurt %d" % crawl.HIT)
        pair.step_both(crawl.HIT_GAP)
        got = [n for n in (crawl.EV_DOWN, EV_ALONE) if pair.log[x].find(crawl.event(n))]
        if got:
            return got[0]
    raise AssertionError("%s: neither downed nor alone after %d hits of %d (life was %d)" % (x, hits, crawl.HIT, life))


def back_at_our_entrance(pair, why):
    """B came back at its own place's entrance (not beside A): the entrance line of FIELD, the respawn with nobody
    watched, B up in FIELD with the continue's hearts. No 'moving beside', no 'bled_out' watching of a far player."""
    pair.expect_log("B", ENTRANCE % FIELD[0], max_frames=600)
    pair.expect_log("B", RESPAWNED_ALONE, max_frames=900)
    pair.expect_log("A", r"coop: dbno: P2 is up", max_frames=300)      # coop_dbno_game.cpp 'P{} is {}'
    pair.never_log("B", BESIDE)

    def up_here():
        st = pair.state("B")
        return pair.in_play(st) and scene_of(pair, "B") == FIELD[0].lower()
    pair.until("B up again in %s (%s)" % (FIELD[0], why), up_here, max_frames=900)
    st = pair.state("B")
    want = min(RESPAWN_LIFE, st.get("max_life") or RESPAWN_LIFE)
    pair.notes.append("%s: B back in %s at %s, life %s" % (why, st.get("scene"), st.get("pos"), st.get("life")))
    assert st.get("life") == want, "B came back with life %s (want %d, kRespawnLife capped)" % (st.get("life"), want)


def _restore(pair):
    """test_dbno_crawl.py's restore (Down but not out on at 45 s, nobody downed), then B too at the boot's spot: its
    own restore warps only when A is away from the boot, and here A IS at the boot while B is in FIELD."""
    crawl._restore(pair)
    if scene_of(pair, "B") != crawl.BOOT[0].lower():
        crawl.to_place(pair, "child", *crawl.BOOT)


@contextlib.contextmanager
def restored(pair):
    try:
        yield
    except BaseException:
        pair.deadline = type(pair.deadline)(crawl.RESTORE_S, clock=pair.deadline.clock)
        try:
            _restore(pair)
        except Exception as e:
            pair.notes.append("restore after the failure failed: %s: %s" % (type(e).__name__, e))
        raise
    _restore(pair)


def set_up(pair):
    pair.need_boot_place("child")
    crawl.to_place(pair, "child", *FIELD)
    crawl.rules(pair, BLEED_S)
    crawl.no_fairy(pair, "B")


@pytest.mark.budget(420)
@pytest.mark.look
@pytest.mark.row("334 a lethal hit with no teammate nearby: no downed wait, the entrance at once")
def test_lethal_hit_with_nobody_nearby_goes_to_the_entrance(pair):
    with restored(pair):
        with pair.setup():
            set_up(pair)
            apart(pair)
        first = lethal(pair)
        assert first == EV_ALONE, "B was downed (event %d) with A in %s: row 334 wants the entrance at once" % (
            first, AWAY[0])
        pair.never_log("B", crawl.event(crawl.EV_DOWN))
        back_at_our_entrance(pair, "lethal hit alone")
        pair.never_log("B", crawl.event(crawl.EV_BLED_OUT))
        crawl.shots(pair, "alone-334-respawned", twice=False)


@pytest.mark.budget(480)
@pytest.mark.look
@pytest.mark.row("334 control + 318/334 downed beside A, A leaves: the entrance at once, not at the bleed-out")
def test_downed_teammate_leaves_goes_to_the_entrance(pair):
    with restored(pair):
        with pair.setup():
            set_up(pair)
        crawl.down(pair)       # A beside (same place): downed as before (event 0, A says 'P2 is down')
        pair.never_log("B", crawl.event(EV_ALONE))
        apart(pair)
        # Downed only with someone nearby (B #171 07:4x): once A is gone (kNearbyGraceUs 1 s, coop_dbno.h) B goes to
        # the entrance, long before its BLEED_S clock ends (EARLY_FRAMES < BLEED_S * pair.FPS 30).
        pair.expect_log("B", crawl.event(EV_ALONE), max_frames=EARLY_FRAMES)
        pair.never_log("B", crawl.event(crawl.EV_BLED_OUT))
        back_at_our_entrance(pair, "downed, A left")
        crawl.shots(pair, "alone-318-bled-out", twice=False)


@pytest.mark.budget(480)
@pytest.mark.look
@pytest.mark.row("318 B watches A, A warps to another place: B to its own entrance, not beside A")
def test_watched_teammate_leaves_and_the_spectator_goes_to_the_entrance(pair):
    with restored(pair):
        with pair.setup():
            set_up(pair)
        crawl.down(pair)
        pair.expect_log("B", crawl.event(crawl.EV_BLED_OUT), max_frames=BLEED_S * 30 + 900)
        pair.expect_log("A", r"coop: dbno: P2 is spectating", max_frames=300)
        # At once: the respawn beside A waits kQuietUs + kSpectateMinUs (3 s each, coop_dbno.h); A leaves before it.
        # (review-tests-153-7: B's Machine runs on the real clock; a lane warp slower than that lets B go beside A
        # before A is away: then the run proves nothing, NO-REF, never a FAIL on BESIDE.)
        t0 = time.monotonic()
        apart(pair)
        took = time.monotonic() - t0
        pair.notes.append("bled_out seen -> A in play away: %.1f s" % took)
        if pair.log["B"].find(BESIDE) and took > SPECTATE_BACK_S:
            pair.unproven("A's warp took %.1f s (> %d s: kQuietUs + kSpectateMinUs), B went beside A first" % (
                took, SPECTATE_BACK_S))
        pair.expect_log("B", crawl.event(EV_ALONE), max_frames=600)
        back_at_our_entrance(pair, "watched one left")
        crawl.shots(pair, "alone-318-watched-left", twice=False)
