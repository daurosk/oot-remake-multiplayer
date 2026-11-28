"""Row 215 (owner's 0.11.8 playtest, reports/player-reports.md row 215): Ganon at life 0 was handed from game to game
during his finish (each copy's stab asked him over to its hitter) and nobody could stab him. Fixed in the rc by
boss-finish-215 a8ba290d: the server's Session::keeps_finish refuses every hand-over of a boss (or a part of its group)
whose owner's last state says life 0 or a death animation, says one line per finish and counts stat boss_finish_kept.
This test confirms it in game on the reachable case (no Ganon boot exists; reports/ux-sweep-0120-3.md HANDOFF):
Twinrova's finish (the merged root at life 0) on the live-twinrova-room boot.

| Step | How | Source |
|---|---|---|
| set-up | both games in play, both warped into TwinrovaStage (its intro starts on arrival) | test_boss_wake.py |
| the fight | both games stepped until in play (past the intro, by state: until_intro_over) | test_boss_wake.py |
| the merge | the owner game kills Koume and Kotake (bridge `kill ID NAME`, cheats on), then Twinrova (merged, phase 2) is in its world list alive | docs/dungeons-boss.md "Twinrova merged ... phase 2", "At the merge both games remove the manager and the sisters" |
| the finish | the owner game kills her (life 0: Session::boss_finishing) | server/session.cpp boss_finishing |
| the stabs | the copy game's Link is placed at her (x, z) and the copy strikes her (`hit ID NAME 1`) STABS times | row 215's cause: a copy's hit asks her over to its hitter (session.cpp Damage, ask_handover) |

Checks (server output and both games' state; each pattern cites its source):
- the server's 'boss finish: Enemy_DungeonBoss_Twinrova in <scene> at life N|in its death stays with player <owner>
  (not to player <copy>): no hand-over until it is gone (row 215)' (session.cpp keeps_finish), exactly once for this
  finish (finish_noted);
- the server's counts line ends '; boss finishes kept N' (host_service.cpp say_counts, row-215-confirm) and N rose;
- no server 'hand-over: <Twinrova or a sister> ... player A -> ...' line (session.cpp change_owner) from the kill on;
- both games' world lists name the same owner (the owner game's player number) for her while she is listed;
- no crash line in either game.
No ask made at all (no keep line, no hand-over: the copy's strikes reached nothing) is NO-REF, never PASS: then
keeps_finish was not exercised. No merge after the sisters' kills is NO-REF too (the finish was not reached).
No screenshot: nothing user-facing is judged here (who owns the boss is state; the fight's looks are rows 222/286).
It leaves Twinrova beaten for the rest of the boot (as test_ux_sweep's final-blow test): it sorts after
test_boss_shots.py / test_boss_wake.py, which need her alive.
"""
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import pair as P               # noqa: E402
from test_boss_wake import (AWAY, BOOT, CRASH, SISTERS, STAGE, SWITCHES,   # noqa: E402
                            need_in_play, own_number, until_intro_over)

ROOT = "Enemy_DungeonBoss_Twinrova"   # docs/dungeons-boss.md:66 (Twinrova's body actors), :718 (merged, phase 2)
NAMES = (ROOT,) + SISTERS
STRIKE_WAIT = 120       # frames for a strike's answer in state.strike (test_ux_sweep.py: max_frames=120)
MERGE_WAIT = 1800       # frames for the merged Twinrova after the sisters' kills (ESTIMATED: the merge event; no run yet)
OWNER_WAIT = 300        # frames for both lists to name one owner after the merge (10 s; ESTIMATED, the share's first owner)
STABS = 3               # the copy's strikes at life 0 (the owner's playtest: several stab attempts)
STAB_GAP = 30           # frames between them
WATCH = 900             # frames from the kill: her death (docs/dungeons-boss.md:49: a death event is taken by 15 s)
CHUNK = 30              # frames per owner read while watching
SERVER_OUT = "server-out.txt"   # the boot's own server output (pair.py server_restart: 'the boot's own is server-out.txt')

LISTENING = r"coop_server: listening on port [0-9]+ \("          # host_service.cpp start: say("listening on port ...")
KEEP = re.compile(r"coop_server: boss finish: %s in \S+ (at life -?[0-9]+|in its death) stays with (player [0-9]+|nobody)"
                  r" \(not to (player [0-9?]+)\): no hand-over until it is gone \(row 215\)" % ROOT)   # keeps_finish
HANDOVER = re.compile(r"coop_server: hand-over: (%s)( \(group [0-9]+\))? player ([0-9]+) -> (player [0-9]+|nobody)"
                      % "|".join(NAMES))                                                           # change_owner
KEPT = re.compile(r"coop_server: enemies: .*; boss finishes kept ([0-9]+)")                         # say_counts


def server_log(pair):
    """The boot's server output as a pair.Log from its start. A live.py session: <session>/server-out.txt; the fake
    lane (no session): next to the fake games' folders (fakes/row215.py). Neither: SETUP-FAIL."""
    if pair.session:
        return pair.server_log(SERVER_OUT)
    if (pair.info or {}).get("fake"):
        lg = P.Log(os.path.join(os.path.dirname(pair.games["A"]["folder"]), SERVER_OUT))
        lg.pos = 0
        return lg
    raise P.SetupFail("no live.py session: no server output to read")


def listed(st, name):
    """Every entry of `name` in a state's world list (docs/playtest.md 'world')."""
    return [a for a in ((st.get("world") or {}).get("actors") or []) if a.get("name") == name]


def living(st, name):
    return [a for a in listed(st, name) if a.get("alive") is not False and (a.get("life") or 0) > 0]


def strike(pair, x, line, name):
    """One `act` strike line (docs/playtest.md act: kill/hit [ID] NAME; its reply's strike_seq), stepped until
    state.strike answers that seq (test_ux_sweep.py's way). Returns the result string."""
    seq = (pair.act(x, line) or {}).get("strike_seq")

    def answered():
        pair.step_both(2)
        s = pair.state(x).get("strike") or {}
        return s if s.get("name") == name and (seq is None or s.get("seq") == seq) else None
    s = pair.until("%s: the answer to '%s'" % (x, line), answered, max_frames=STRIKE_WAIT)
    pair.notes.append("%s %s -> %s" % (x, line, s.get("result")))
    return s.get("result")


def owner_game(pair, st, name):
    """(owner game, its player number, the copy game) from the world lists' owner of `name`: both games must name
    the same player number, and it must be one game's own number (coop.players self)."""
    own = {x: own_number(st[x]) for x in ("A", "B")}
    named = {x: [a.get("owner") for a in listed(st[x], name)] for x in ("A", "B")}
    nums = {o for v in named.values() for o in v}
    assert len(nums) == 1 and isinstance(next(iter(nums)), int), "%s: the games do not name one owner: %s (own %s)" % (
        name, named, own)
    n = next(iter(nums))
    games = [x for x in ("A", "B") if own[x] == n]
    assert len(games) == 1, "%s: owner player %s is not one game's own number (%s)" % (name, n, own)
    return games[0], n, "B" if games[0] == "A" else "A"


@pytest.fixture
def back_after(pair):
    """test_boss_wake.py's back_after: both games out of the stage after the test; a failed way back is a note."""
    yield pair
    pair.deadline = P.Deadline(120)
    try:
        pair.warp_both(AWAY[0], entrance=AWAY[1])
    except Exception as e:                    # noqa: BLE001
        pair.notes.append("way back to %s %s failed: %s" % (AWAY[0], AWAY[1], e))


@pytest.mark.needs_boot(BOOT)
@pytest.mark.budget(960)        # 6000 max (until_intro_over) + 1800 max + 900 frames at the boot proof's 11.9 a second (pair.BOOT_STEP_FPS) = 807 s + warps, reads
@pytest.mark.needs_env(*SWITCHES)
@pytest.mark.row("#215 a boss in its finish (Twinrova at life 0) stays with its owner's game: no hand-over")
def test_twinrova_finish_stays_with_its_owner(pair, back_after):
    with pair.setup():
        need_in_play(pair)
        srv = server_log(pair)
        assert srv.find(LISTENING), "SETUP: %s has no 'listening on port' line (not this boot's server output)" % (
            srv.path)
        st = pair.warp_both(STAGE, entrance=STAGE, arrive_in_event=True)   # test_boss_wake.py
        for x, s in st.items():
            assert (s.get("scene") or "").lower() == STAGE.lower() and not s.get("loading"), "%s after the warp: %s" % (
                x, s.get("scene"))
    until_intro_over(pair, "row 215")    # (was INTRO_FIGHT 900 fixed frames first, then this same wait: one wait, by state)

    # The merge: the sisters' owner kills both (one strike at a time: state.strike keeps only the last).
    st = {x: pair.state(x) for x in ("A", "B")}
    if not living(st["A"], SISTERS[0]) and not living(st["A"], ROOT):
        pair.unproven("no Twinrova sister or Twinrova alive in A's world list after the intro (beaten in this boot?)")
    if not living(st["A"], ROOT):
        own, _, _ = owner_game(pair, st, SISTERS[0])
        for name in SISTERS:
            for a in living(pair.state(own), name):
                r = strike(pair, own, "kill %s %s" % (a.get("id"), name), name)
                if r != "done":
                    pair.unproven("%s: the kill of %s answered %r: the merge was not reached" % (own, name, r))
        try:
            pair.until("merged Twinrova alive in both games' world lists",
                       lambda: all(living(pair.state(x), ROOT) for x in ("A", "B")), max_frames=MERGE_WAIT)
        except AssertionError as e:
            pair.unproven("no merged %s after the sisters' kills: %s" % (ROOT, e))

    # The finish: her owner kills her; from then on no rule may hand her over.
    def agreed():        # (review-rows-c136-4: right after the merge the lists may not yet name one owner)
        try:
            return owner_game(pair, {x: pair.state(x) for x in ("A", "B")}, ROOT)
        except AssertionError:
            return None
    try:
        own, num, copy = pair.until("both games name one owner for %s" % ROOT, agreed, max_frames=OWNER_WAIT)
    except AssertionError:
        owner_game(pair, {x: pair.state(x) for x in ("A", "B")}, ROOT)   # (FAIL naming what each game says)
        raise
    st = {x: pair.state(x) for x in ("A", "B")}
    pair.notes.append("owner game %s (player %d), copy %s" % (own, num, copy))
    srv_mark = srv.mark()
    kept_before = [int(m.group(1)) for m in map(KEPT.search, srv.since(0)) if m]
    kept_before = kept_before[-1] if kept_before else 0
    tw = living(st[own], ROOT)[0]
    r = strike(pair, own, "kill %s %s" % (tw.get("id"), ROOT), ROOT)
    if r != "done":
        pair.unproven("%s: the kill of %s answered %r: no finish" % (own, ROOT, r))
    try:                 # (docs/playtest.md kill: a far actor may not die until Link is near: then no finish, NO-REF)
        pair.until("%s at life 0 in %s's world list" % (ROOT, own),
                   lambda: not living(pair.state(own), ROOT), max_frames=STRIKE_WAIT)
    except AssertionError as e:
        pair.unproven("%s struck but not at life 0: %s" % (ROOT, e))

    # The stabs: the copy's Link at her (x, z), its strikes on her (row 215's ask: a copy's hit to its hitter).
    cs = pair.state(copy)
    at = (listed(cs, ROOT) or [tw])[0].get("pos") or tw.get("pos")
    if cs.get("pos") and at:
        pair.act(copy, "place %.2f %.2f %.2f" % (at[0], cs["pos"][1], at[2] + 1.0))
    for i in range(STABS):
        e = listed(pair.state(copy), ROOT)
        if not e:
            pair.notes.append("stab %d: %s no longer in %s's list" % (i + 1, ROOT, copy))
            break
        strike(pair, copy, "hit %s %s 1" % (e[0].get("id"), ROOT), ROOT)
        pair.step_both(STAB_GAP)

    # Watch her death: both games name the same owner while she is listed.
    seen_owner = []
    ran = 0
    while ran < WATCH:
        pair.step_both(CHUNK)
        ran += CHUNK
        for x in ("A", "B"):
            for a in listed(pair.state(x), ROOT):
                if a.get("owner") != num:
                    seen_owner.append("%s at +%d frames: owner %s (life %s)" % (x, ran, a.get("owner"), a.get("life")))

    lines = srv.since(srv_mark)
    keeps = [m for m in map(KEEP.search, lines) if m]
    moves = [ln for ln in lines if HANDOVER.search(ln)]
    kept_now = [int(m.group(1)) for m in map(KEPT.search, lines) if m]
    pair.notes.append("server: %d keep line(s), %d hand-over line(s), boss finishes kept %d -> %s" % (
        len(keeps), len(moves), kept_before, kept_now[-1] if kept_now else "no counts line"))
    for x in ("A", "B"):
        pair.never_log(x, CRASH)
    assert not moves, "%s handed over in its finish (row 215): %s" % (ROOT, " | ".join(moves[:3]))
    assert not seen_owner, "%s's owner changed in its finish (row 215): %s" % (ROOT, "; ".join(seen_owner[:4]))
    if not keeps:
        pair.unproven("no hand-over was asked in the finish (no 'boss finish:' line): keeps_finish not exercised; "
                      "the copy's strikes: %s" % [n for n in pair.notes if n.startswith(copy + " hit")])
    assert len(keeps) == 1, "%d 'boss finish:' lines for one finish (want 1): %s" % (
        len(keeps), [m.group(0) for m in keeps])
    k = keeps[0]
    assert k.group(2) == "player %d" % num, "keep line names %s, the owner is player %d: %s" % (
        k.group(2), num, k.group(0))
    want_copy = own_number(pair.state(copy))
    assert k.group(3) == "player %s" % want_copy, "keep line refused %s, the copy is player %s: %s" % (
        k.group(3), want_copy, k.group(0))
    assert kept_now, "no server counts line with 'boss finishes kept' after the keep line (server build without it?)"
    assert kept_now[-1] > kept_before, "boss finishes kept did not rise (%d -> %d)" % (kept_before, kept_now[-1])
