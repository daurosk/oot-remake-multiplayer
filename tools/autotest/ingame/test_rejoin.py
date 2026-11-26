"""Regress step 4 (row 249): a guest's own rupees (and life) are kept over a rejoin.

This replaces coord-multiplayer/offline/regress-01113-steps/s4-rupee-rejoin.ps1 (scenario offline/rupee-249/
hs-rupee-rejoin.steps.txt), the regress-vs-0.11.12 step "hearts + rupees on join/rejoin":

| Old check | Here |
|---|---|
| B a real guest (switch B:LICHII_COOP_TEST_OWN_SAVE=0) | the same switch for this boot (needs_env); set-up: B's log says it plays from the co-op folder (coop_save.cpp:407) |
| B's -coop folder made by hand with 137 rupees before the run | B's own value set in play through the bridge (`flag` PlayerStatus.CurrentRupee 0xa77921d7, coop/host/src/game_items.cpp:224, per player), then B's own autosave (a warp = a scene change, docs/testing.md:350) writes it into that folder; proven by reading the newest slot's progress.sav (save_flags.get) |
| close B / start B | Pair.rejoin("B"): live.py quit + relaunch in the same session, A running in real time meanwhile |
| "the host's save written to", "our own N ... values kept in it", never "no values of our own kept yet" (coop_save.cpp:563-569) | the same lines in B's new log |
| "now playing from the co-op save folder" | the same line after the rejoin |
| every slot at 137 rupees; the HUD's rupee counter in b-rejoined shows 137 | B's state `rupees` = its own value after the rejoin, A's unchanged; HUD shots of both games compared to refs (NO-REF until the owner approves one) |

The old run made B's folder by hand as a 0.11.8-0.11.10 guest's; here the first join of this session makes it (the
same write_coop_save path, coop/host/src/save_transfer.cpp:432), and the rejoin is the judged join. Life
(PlayerStatus.Life 0x31ab5580, game_items.cpp:83, per player) is judged too when the bridge's `flag` moves the
state's `life` to the value set (its unit proven in the set-up), else that part is a note, never a guess.

Run on its own boot (B real guest), e.g. on lane 2:
  pytest tools/autotest/ingame --lanes 2 -k test_own_values_kept_over_rejoin --env B:LICHII_COOP_TEST_OWN_SAVE=0 ...
"""
import os
import re
import sys

import pytest

from test_rows import need_child_in_play

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import save_flags   # noqa: E402  (tools/autotest/save_flags.py: the progress.sav reader)

RUPEES = "0xa77921d7"     # PlayerStatus.CurrentRupee (coop/host/src/game_items.cpp:224, int32, per player)
LIFE = "0x31ab5580"       # PlayerStatus.Life (coop/host/src/game_items.cpp:83, int32, per player)
OWN_SAVE_OFF = "B:LICHII_COOP_TEST_OWN_SAVE=0"   # coop_save.cpp:154 (hookshot.py TEST_GAME_DEFAULTS sets 1)
ELSEWHERE = "KokiriForest_Child"                         # scenarios/boulder.A.txt "warp KokiriForest_Child"
HOME, HOME_AT = "KakarikoVillage_Child", "Kakariko_WellSide"   # live-kakariko-child.B.txt's warp (the default boot)

# coop_save.cpp:407 / :559 / :566 / :569 (the old scenario's own patterns)
COOP_FOLDER = re.compile(r"coop: save: now playing from the co-op save folder (.+?)\s*$")
WRITTEN = r"coop: save: the host's save written to"
KEPT = r"coop: save: our own \d+ rupees/ammo/bottles/life/magic values kept in it"
NONE_KEPT = r"no values of our own kept yet"
NO_CRASH = r"unhandled exception|fault at|coop: null read"
CHOICES = (37, 38, 39, 41)   # a count under a child's 99 wallet, unlike both games' own (the old run: 137 in a 200)


def newest_rupees(folder):
    """(slot path, rupees) of the newest slot_NN/progress.sav in a co-op folder, (None, None) if none reads."""
    best = None
    try:
        names = os.listdir(folder)
    except OSError:
        return None, None
    for n in names:
        p = os.path.join(folder, n, "progress.sav")
        if save_flags.SLOT.match(n) and os.path.isfile(p):
            t = os.path.getmtime(p)
            if best is None or t > best[0]:
                best = (t, p)
    if best is None:
        return None, None
    try:
        return best[1], save_flags.get(save_flags.read_file(best[1]), save_flags.TYPES["int"], int(RUPEES, 16))
    except (OSError, save_flags.SaveError):
        return best[1], None


@pytest.mark.row("regress step 4 / #249: a guest's own rupees and life kept over a rejoin (hs-rupee-rejoin)")
@pytest.mark.needs_env(OWN_SAVE_OFF)
@pytest.mark.budget(900)   # quit (<= 60 s) + relaunch boot (<= 420 s, Pair.REJOIN_*) + the waits around them
def test_own_values_kept_over_rejoin(pair):
    with pair.setup():
        need_child_in_play(pair)
        m = None
        for ln in pair.log["B"].since(0):
            m = COOP_FOLDER.search(ln) or m
        assert m, "B does not play from a co-op folder: not a real guest (%s missing?)" % OWN_SAVE_OFF
        folder = m.group(1)
        assert os.path.isdir(folder), "B's co-op folder %s is not there" % folder
        a0, b0 = pair.state("A"), pair.state("B")
        want = next(v for v in CHOICES if v not in (a0.get("rupees"), b0.get("rupees")))
    set_life = (b0.get("max_life") or 0) >= 8
    try:
        with pair.setup():
            pair.flag("B", RUPEES, want)
            life = None
            if set_life:
                life = b0["max_life"] - 4                     # one heart down (life in the state's own unit)
                pair.flag("B", LIFE, life)
            pair.step_both(30)
            b1 = pair.state("B")
            assert b1.get("rupees") == want, "B's rupees did not follow the flag: %s, set %d" % (b1.get("rupees"), want)
            if life is not None and b1.get("life") != life:
                pair.notes.append("life not judged: B's state life %s after flag %s=%d (another unit)"
                                  % (b1.get("life"), LIFE, life))
                life = None
        # per player: A's own count is not B's (game_items.cpp R::per_player)
        assert pair.state("A").get("rupees") == a0.get("rupees"), "A's rupees moved with B's: %s -> %s" % (
            a0.get("rupees"), pair.state("A").get("rupees"))

        # B's own autosave writes its values into the co-op folder (a scene change: away and back home)
        pair.warp_one("B", ELSEWHERE)
        pair.warp_one("B", HOME, entrance=HOME_AT)
        slot = pair.until("B's autosave in %s with %d rupees" % (folder, want),
                          lambda: newest_rupees(folder)[1] == want and newest_rupees(folder)[0], max_frames=900)
        pair.notes.append("autosave: %s has %d rupees" % (slot, want))

        pair.rejoin("B")   # (the lane's own B boot, Pair.boot, with --no-load)
        pair.expect_log("B", WRITTEN, max_frames=900)
        pair.expect_log("B", KEPT, max_frames=300)
        pair.never_log("B", NONE_KEPT)
        pair.expect_log("B", COOP_FOLDER.pattern, max_frames=900)
        pair.until("B in play after the rejoin", lambda: pair.in_play(pair.state("B")), max_frames=1800)
        pair.step_both(60)
        a2, b2 = pair.state("A"), pair.state("B")
        assert b2.get("rupees") == want, "B's own rupees not kept over the rejoin: %s, had %d (A has %s)" % (
            b2.get("rupees"), want, a2.get("rupees"))
        assert a2.get("rupees") == a0.get("rupees"), "A's rupees changed over B's rejoin: %s -> %s" % (
            a0.get("rupees"), a2.get("rupees"))
        if life is not None:
            assert b2.get("life") == life, "B's own life not kept over the rejoin: %s, had %d" % (b2.get("life"), life)
        for x in ("A", "B"):
            pair.never_log(x, NO_CRASH)
        # user-facing: each game's HUD rupee counter (and hearts) after the rejoin
        for x in ("A", "B"):
            pair.check_shot(x, "rejoin-hud")
    finally:
        # back as the lane had it, also after a FAIL (review-c123-11): B's own count (its next autosave keeps it)
        # (B may be down after a failed rejoin: a restore error is a note, never in place of the test's own result)
        try:
            pair.flag("B", RUPEES, b0.get("rupees") or 0)
            if set_life:
                pair.flag("B", LIFE, b0["life"])
        except Exception as e:
            pair.notes.append("restore of B's rupees/life failed: %s: %s" % (type(e).__name__, e))
