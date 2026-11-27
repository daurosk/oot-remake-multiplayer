"""Row 286 (row-286-wake-3): Twinrova, "sometimes the other players do not see the witches' attacks"
(reports/player-reports.md row 286). This test judges the WAKE: once the fight is on, every game's sisters are awake in
the same fight, run by one owner. The shots themselves (bullets) are row 222's test (test_boss_shots.py, owner
twinrova-shots-222); nothing here repeats them.

| Test | How the games come in | Old probe |
|---|---|---|
| test_twinrova_wake_together | both games warp into TwinrovaStage at the same frame | row-286-wake 61e8a6794 (offline/row-286-probe/drive.py), never run |
| test_twinrova_wake_late | A plays the intro and fights, B comes in after A's intro ended | (new: the real play path, below) |

Why a late case: in play the players do not reach the room together. docs/dungeons-boss.md "How it works" 1-3: the
others are moved in at the END of the leader's intro, and a game whose intro still runs once another player's has
finished skips the rest of it (SeqOneShotEventRequestForceSkip). So a copy game whose sisters must wake from a skipped
intro, into a fight whose owner's verbs and battle orders are already running, is the normal case, not a corner.

Checks (all from the game's own state or the module's own lines; each pattern cites its source):
  per sister (Enemy_DungeonBoss_Koume, Enemy_DungeonBoss_Kotake) and per game, after the warp:
  - 'native boss sync on (BSA .., ours|a copy)' (coop_boss_native.cpp:527) in both games; exactly one game says ours;
  - in the copy game: "the owner's verbs came (brain N)" (coop_boss_native.cpp:1512) and
    "battle orders: the owner's came" (coop_boss_orders.cpp:188): the copy's sisters run the owner's fight;
  - the state's world list (docs/playtest.md "world"): the sister is listed in both games, alive, life > 0, and both
    games name the same owner (a player number);
  - the boss counts line (coop_hookshot.cpp:601 'coop: bosses: ours sent N brain + N action verbs ...; copies: N own
    skipped, N brain + N action verbs put in'): over the fight the owner game sent action verbs and the copy game put
    action verbs in (the fight state moved in both, not just a first verb).
  - looks (user-facing: the sisters awake, flying, in each game): paired shots on each game, compared to refs/
    (tolerance 0.80, a scene with moving things, docs/ingame-tests.md step 6, as test_boss_shots.py).
Judge order: log + state first, looks last (a look miss is never reported in place of a state FAIL).

Waits: 30 frames a second (pair.FPS). The intro + first attack wait, 1800 ticks = 900 frames, is the old row-222 A
script's 'wait 1800  # the intro, then the fight' (row-222-shots-twinrova.A.txt:27, cited in test_boss_shots.py).
"""
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import pair as P               # noqa: E402

# test_boss_shots.py SWITCHES (the old rows' '#! switches' line): boss notes, the object walk's names, cheats (kill).
SWITCHES = ("LICHII_COOP_DEBUG_BOSS=1", "LICHII_COOP_DEBUG_OBJECTS=1", "LICHII_COOP_CHEATS=1")
STAGE = "TwinrovaStage"          # coop_start_points.inc:121 {"TwinrovaStage", "TwinrovaStage", ...}
AWAY = ("SpiritTemple", "SpiritTemple_Entrance")   # coop_start_points.inc:116: B waits in the dungeon, not the stage
BOOT = "live-twinrova-room"      # scenarios/live-twinrova-room.A/B.txt: child save, both games in TwinrovaStage
SISTERS = ("Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Kotake")   # docs/dungeons-boss.md "One boss": Twinrova's parts
CRASH = r"exception 0x|FATAL"    # test_boss_shots.py CRASH (no-crash-A/B max=0)

INTRO_FIGHT = 900                # frames: 'wait 1800' ticks // 2 (module docstring)
FIGHT_LOOKS = 4                  # paired looks in the fight, one every LOOK_GAP frames
LOOK_GAP = 300                   # frames (10 s: the boss counts line's own period, coop_hookshot.cpp 'every 10 s')
LATE_GAP = 900                   # frames A fights alone before B comes in (past A's intro: INTRO_FIGHT)
SHOT_TOLERANCE = 0.80            # test_boss_shots.py Shots.TOLERANCE (moving scene)


def sync_on(boss):
    """coop_boss_native.cpp:527 Note(b.name + ": native boss sync on (BSA " + Hex(b.bsa) + ", ours|a copy)")."""
    return r"%s: native boss sync on \(BSA 0x[0-9a-f]+, (ours|a copy)\)" % boss


def verbs_came(boss):
    """coop_boss_native.cpp:1512 Note(e->name + ": the owner's verbs came (brain " + N + "): the copy's own brain ...")."""
    return r"%s: the owner's verbs came \(brain [0-9]+\)" % boss


def orders_came(boss):
    """coop_boss_orders.cpp:188 Note(e.member.name + ": battle orders: the owner's came: our own manager's orders ...")."""
    return r"%s: battle orders: the owner's came" % boss


# coop_hookshot.cpp:601 (LogBossNative), its first two clauses: groups 1 brain sent, 2 action sent, 4 brain put in,
# 5 action put in.
COUNTS = re.compile(r"coop: bosses: ours sent ([0-9]+) brain \+ ([0-9]+) action verbs \([0-9]+ by plug\); copies: "
                    r"([0-9]+) own skipped, ([0-9]+) brain \+ ([0-9]+) action verbs put in")


# -- driving ----------------------------------------------------------------------------------------------------
def warp_one(pair, x, scene, entrance=None, in_event=True):
    """ONE game to `scene` at `entrance` (default: the scene's own start) by the shared Pair.warp_one (pair.py,
    test-bridge-c127-identity 3d268dd6a: warp_both's rules; the other game is stepped to the same frame count), then
    asserted in the scene and not loading. in_event: a boss stage's intro starts on arrival (arrive_in_event)."""
    s = pair.warp_one(x, scene, entrance=entrance or scene, arrive_in_event=in_event)
    assert (s.get("scene") or "").lower() == scene.lower() and not s.get("loading"), "%s after the warp: %s" % (
        x, s.get("scene"))


def need_in_play(pair):
    """The first set-up: the shared pair.need_boot_place (harness-c133-scene 82f9a7a5a): both games in play STEPPED
    (never one read: the gifts-237/row53 'not in play' SETUP-FAIL class, B #139 16:31), in the boot's scene
    (TwinrovaStage, scenarios/live-twinrova-room.A/B.txt) and child: the boot's save is the lane's child baseline
    (row215-309d7180 game-out-A/B.txt: 'test warp to TwinrovaStage ... childhood - NOT set', 'simulated turf: TwinrovaStage
    (child)'); 'adult' here was SETUP-FAIL on every real run of this boot."""
    return pair.need_boot_place("child")


def look_both(pair, name, missed):
    """A paired look (pair.check_shot on each game, at the same frame count): a miss is kept, asserted last."""
    before = {x: pair.c[x].last_frame or 0 for x in ("A", "B")}
    for x in ("A", "B"):
        try:
            pair.check_shot(x, name, tolerance=SHOT_TOLERANCE)
        except AssertionError as e:
            missed.append(str(e))
    ran = {x: (pair.c[x].last_frame or 0) - before[x] for x in ("A", "B")}
    top = max(ran.values())
    for x, n in ran.items():
        if top > n:
            pair.step_other(x, top - n)


# -- judging ----------------------------------------------------------------------------------------------------
def roles(pair, boss):
    """{game: 'ours'|'a copy'} from the sync line since this test began (the newest one: a hand-over says it again)."""
    rx = re.compile(sync_on(boss))
    out = {}
    for x in ("A", "B"):
        for ln in pair.log[x].since():
            m = rx.search(ln)
            if m:
                out[x] = m.group(1)
    return out


def sister(st, boss):
    """The sister's entry in a state's world list (docs/playtest.md 'world'), or None."""
    for a in ((st.get("world") or {}).get("actors") or []):
        if a.get("name") == boss:
            return a
    return None


def own_number(st):
    """This game's own player number: the state's coop.players entry with self true (docs/playtest.md state example,
    'coop':{'players':[{'number':1,...,'self':true,...}]}), or None."""
    for p in ((st.get("coop") or {}).get("players") or []):
        if p.get("self") is True:
            return p.get("number")
    return None


def judge_world(pair, tag, owner_game):
    """Both games list each sister, alive, life > 0, and both name as its owner the player number of the game whose
    sync line says 'ours' (owner_game). The world 'owner' is OwnerJson (coop_world_snapshot.h:72): null = not known to
    the sharing, the string "server" = the server, else a player number; each of those is said as what it is."""
    st = {x: pair.state(x) for x in ("A", "B")}
    want = own_number(st[owner_game])
    assert isinstance(want, int), "%s: game %s gives no own player number (coop.players self): %s" % (
        tag, owner_game, (st[owner_game].get("coop") or {}).get("players"))
    bad = []
    for boss in SISTERS:
        e = {x: sister(st[x], boss) for x in ("A", "B")}
        said = "; ".join("%s %s" % (x, "absent" if e[x] is None else "owner %s alive %s life %s at %s" % (
            e[x].get("owner"), e[x].get("alive"), e[x].get("life"), e[x].get("pos"))) for x in ("A", "B"))
        pair.notes.append("%s %s: %s" % (tag, boss, said))
        if any(e[x] is None for x in ("A", "B")):
            bad.append("%s not in both games' world lists (%s)" % (boss, said))
            continue
        if not all(e[x].get("alive") is True and (e[x].get("life") or 0) > 0 for x in ("A", "B")):
            bad.append("%s not alive in both games (%s)" % (boss, said))
        for x in ("A", "B"):
            o = e[x].get("owner")
            if o is None:
                bad.append("%s: game %s's sharing does not know it (owner null; the owner game %s is player %d)" % (
                    boss, x, owner_game, want))
            elif o == "server":
                bad.append("%s: game %s says the server owns it, not player %d (game %s says ours)" % (
                    boss, x, want, owner_game))
            elif o != want:
                bad.append("%s: game %s names player %s as owner, game %s (player %d) says ours" % (
                    boss, x, o, owner_game, want))
    assert not bad, "%s: %s" % (tag, " | ".join(bad))


def counts(lines):
    last = None
    for ln in lines:
        m = COUNTS.search(ln)
        if m:
            last = m
    return tuple(int(last.group(i)) for i in (1, 2, 4, 5)) if last else None



def counts_at_begin(pair):
    """Each game's counts on the last counts line before this test began (zeros: none yet). One boot runs every test."""
    out = {}
    for x in ("A", "B"):
        lg = pair.log[x]
        with open(lg.path, "rb") as f:
            head = f.read(lg.pos).decode("utf-8", "replace").splitlines()
        out[x] = counts(head) or (0, 0, 0, 0)
    return out


def judge_wake(pair, base, tag):
    """Sync, owner's verbs and orders on the copy, one owner, and the fight state moving on both sides. Returns the
    owner game."""
    owner = None
    for boss in SISTERS:
        for x in ("A", "B"):
            pair.expect_log(x, sync_on(boss), max_frames=600)
        r = roles(pair, boss)
        ours = [x for x, v in r.items() if v == "ours"]
        assert len(ours) == 1, "%s %s: not exactly one owner game (%s)" % (tag, boss, r)
        owner = ours[0]
        copy = "B" if owner == "A" else "A"
        pair.expect_log(copy, verbs_came(boss), max_frames=600)
        pair.expect_log(copy, orders_came(boss), max_frames=600)
    judge_world(pair, tag, owner)
    copy = "B" if owner == "A" else "A"
    try:                               # LogBossNative says its counts every 10 s while they change
        pair.until("a boss counts line in both games since the test began",
                   lambda: all(counts(pair.log[x].since()) for x in ("A", "B")), max_frames=600)
    except AssertionError:
        pass                           # (judged below: no line = nothing sent or put in)
    now = {x: counts(pair.log[x].since()) or base[x] for x in ("A", "B")}
    sent = now[owner][1] - base[owner][1]
    put = now[copy][3] - base[copy][3]
    pair.notes.append("%s: owner %s sent %d action verbs; copy %s put %d in" % (tag, owner, sent, copy, put))
    assert sent > 0, "%s: the owner game %s sent no action verb in the fight" % (tag, owner)
    assert put > 0, "%s: the copy game %s put none of the owner's action verbs in (its sisters did not take the fight)" % (
        tag, copy)
    return owner


def fight_and_look(pair, tag, owner, missed):
    for i in range(1, FIGHT_LOOKS + 1):
        pair.step_both(LOOK_GAP)
        judge_world(pair, "%s fight-%02d" % (tag, i), owner)
        look_both(pair, "%s-fight-%02d" % (tag, i), missed)


def end_checks(pair, missed):
    for x in ("A", "B"):              # (a crash is never reported as a look miss)
        pair.never_log(x, CRASH)
    assert not missed, "%d look miss(es): %s" % (len(missed), "; ".join(missed[:4]))


# -- tests ------------------------------------------------------------------------------------------------------
@pytest.fixture
def back_after(pair):
    """After the test (pass or not): both games out of the fight to AWAY, so the next test of the boot starts out of a
    fight (as boss-respawn-bridge's back_after). A failed way back is a note, never a result of this test."""
    yield pair
    pair.deadline = P.Deadline(120)           # (the test's own deadline may be spent: the way back has its own)
    try:
        pair.warp_both(AWAY[0], entrance=AWAY[1])
    except Exception as e:                    # noqa: BLE001
        pair.notes.append("way back to %s %s failed: %s" % (AWAY[0], AWAY[1], e))


@pytest.mark.needs_boot(BOOT)
@pytest.mark.budget(600)        # ESTIMATED, no old run: ~2100 frames (70 s) + warps, shots, reads; the first lane run's time replaces it
@pytest.mark.look
@pytest.mark.needs_env(*SWITCHES)
@pytest.mark.row("#286 Twinrova's sisters wake in both games, one fight (entering together)")
def test_twinrova_wake_together(pair, back_after):
    base = counts_at_begin(pair)
    missed = []
    with pair.setup():
        need_in_play(pair)
        st = pair.warp_both(STAGE, entrance=STAGE, arrive_in_event=True)   # test_boss_shots.py warp_in
        for x, s in st.items():
            assert (s.get("scene") or "").lower() == STAGE.lower() and not s.get("loading"), "%s after the warp: %s" % (
                x, s.get("scene"))
    pair.step_both(INTRO_FIGHT)
    owner = judge_wake(pair, base, "together")
    look_both(pair, "wake-together-awake", missed)
    fight_and_look(pair, "wake-together", owner, missed)
    end_checks(pair, missed)


@pytest.mark.needs_boot(BOOT)
@pytest.mark.budget(700)        # ESTIMATED, no old run: ~3000 frames (100 s) + warps, shots, reads; the first lane run's time replaces it
@pytest.mark.look
@pytest.mark.needs_env(*SWITCHES)
@pytest.mark.row("#286 Twinrova's sisters wake in a game that comes in after the intro (the play path)")
def test_twinrova_wake_late(pair, back_after):
    base = counts_at_begin(pair)
    missed = []
    with pair.setup():
        need_in_play(pair)
        warp_one(pair, "B", AWAY[0], entrance=AWAY[1], in_event=False)   # (the boot left B in the stage too)
        warp_one(pair, "A", STAGE)
    pair.step_both(LATE_GAP)        # A: its intro, then the fight alone (B elsewhere)
    with pair.setup():
        warp_one(pair, "B", STAGE)  # B in after A's intro ended: its own intro is force-skipped (module docstring)
    look_both(pair, "wake-late-arrive", missed)
    pair.step_both(INTRO_FIGHT)
    owner = judge_wake(pair, base, "late")
    look_both(pair, "wake-late-awake", missed)
    fight_and_look(pair, "wake-late", owner, missed)
    end_checks(pair, missed)
