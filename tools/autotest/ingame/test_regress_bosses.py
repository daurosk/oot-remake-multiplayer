"""Regress vs 0.11.12, step 7 (reports/rc-release-0120.md section 4; old step: coord offline/regress-01113.ps1 step 7 =
regress-01113-steps/s7-boss.ps1): "Barinade boss route + Ganondorf ball" as bridge tests.

| Test | Old step part | Boot | Proves |
|---|---|---|---|
| test_barinade_native_sync | hs-barinade-native (s7 MUST 'Enemy_DungeonBoss_Barinade: native boss sync on', NOT 'native sync off') | live-barinade-room | the route reaches Barinade in both games and her copy takes the owner's verbs (one fight) |
| test_ganondorf_ball_sent_back | s7 "Ganondorf ball" (its NotProven; the old step ran hs-phantom-ganon-ball as "the closest existing scenario") | live-ganondorf-room | row 211: Ganondorf's rally ball flies in both games without a turn cutting it, and a ball sent back knocks him down in both |

Why Ganondorf's own fight, not hs-phantom-ganon-ball: the step names Ganondorf's ball, and row 211
(reports/player-reports.md) is that ball (DungeonBossGanondrof_Bullet_*); its scenario hs-ganondorf-rally and checks
(reports/shot-reflect-211-4.md "Scenario + run request" a-c) were written after the old step picked the closest one.
Phantom Ganon's ball (row 177) is its own row (hs-phantom-ganon-ball; not ported here).

Sources (worker-rules "no guessed values"):
- Stages: lichii/hook/coop_start_points.inc:3 BarinadeStage, :39 GanondorfStage. Both have no appear asker of their own:
  after a warp the boss comes only with the rematch's boot event DreamBossStage_Rematch_<Boss>_Start
  (docs/boss-multiplayer.md "Boss rush stages reached by a warp"); the `event` step is a scenario step, not an `act`
  kind (docs/playtest.md act), so each test has its own boot (scenarios/live-barinade-room, live-ganondorf-room: the
  old scenarios' own lines up to 'mark a-in' / 'b-in' + their event line). Ganondorf's boot: A enters 600 ticks
  after B, so B owns him (hs-ganondorf-rally.A.txt 'wait 600 # A enters second').
- Lines (each pattern at its writer; the boot's lines come before the test began, so those are read from the whole log):
  "coop: bosses: native sync on (brain verbs wrapped, action verbs wrapped)" (hookshot/src/coop_hookshot.cpp:456; off =
  "native sync off": the old NOT); "<boss>: native boss sync on (BSA 0x.., ours|a copy)" (coop_boss_native.cpp:527, the
  old MUST); "<boss>: the owner's verbs came (brain N)" (:1512); the boss counts line (coop_hookshot.cpp:601; parsed by
  test_boss_wake.COUNTS); world owner (docs/playtest.md "world"). Row 211: "coop: objects: share <boss>: its own rally
  ball flies here: its attack goes on, no boss turn (row 211)" (coop_objects_sync.cpp:2561 via coop_objects.cpp:2660);
  "coop: shots: enemy shot DungeonBossGanondrof_Bullet_LightBall at ..: kept here (a split shot ...)" (coop_spawn.cpp:
  2173); "coop: objects: anim <boss> 0x.. '<anim>'( (ours))" (coop_objects.cpp:3014) with Parry_Ready / LightBallDamage
  (shot-reflect-211-4.md check (c); coop_objects_sync.cpp:1006 'LightBallDamage_Start/Loop/End'); "coop: objects: share
  stunned here, said to the owner: <boss>" (coop_objects_sync.cpp:3369).
- Inputs: the old scenarios' keys (bridge names: playtest_client.BUTTONS; LSTICK_UP = stick [0, 1], hookshot.STICK_KEYS),
  their waits and holds in frames = ticks // 2 (1 tick = 1/60 s, hookshot.TICKS; 30 frames a second, pair.FPS).
- Switches: the old A headers' '#! switches LICHII_COOP_DEBUG_BOSS=1,LICHII_COOP_DEBUG_OBJECTS=1,LICHII_COOP_CHEATS=1'.
- Shots (user-facing: the boss in the same move on both screens; Ganondorf down after a ball sent back): paired, compared
  to refs/ at 0.80 (moving scene, docs/ingame-tests.md step 6). NO-REF until approved.
Not judged here (NOT proven): the server's 'boss turns' line against a flying ball (shot-reflect-211-4 check (a): the
server log is not one of the pair's logs); row 211's owner-side line stands for it (no turn while his ball flies).
"""
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pair as P                                                         # noqa: E402
from test_boss_wake import COUNTS, own_number, sync_on, verbs_came      # noqa: E402  (the same patterns, one place)
from test_enemy_turn import in_stage, keep_up                                     # noqa: E402

SWITCHES = ("LICHII_COOP_DEBUG_BOSS=1", "LICHII_COOP_DEBUG_OBJECTS=1", "LICHII_COOP_CHEATS=1")
SYNC_ON = r"coop: bosses: native sync on \(brain verbs wrapped, action verbs wrapped\)"   # coop_hookshot.cpp:456
SYNC_OFF = r"coop: bosses: native sync off"                                              # the old step's NOT
CRASH = r"exception 0x|FATAL"                                                            # test_boss_wake CRASH
SHOT_TOLERANCE = 0.80
UP = [0, 1.0]                                                            # LSTICK_UP (hookshot.STICK_KEYS)

BARINADE_BOOT, BARINADE_STAGE, BARINADE = "live-barinade-room", "BarinadeStage", "Enemy_DungeonBoss_Barinade"
GANON_BOOT, GANON_STAGE, GANONDORF = "live-ganondorf-room", "GanondorfStage", "Enemy_DungeonBoss_Ganondorf"
BALL = "DungeonBossGanondrof_Bullet_LightBall"     # row 211; coop_enemy_shots kShots (shot-reflect-211-4 "Held")
RALLY = r"coop: objects: share %s: its own rally ball flies here: its attack goes on, no boss turn \(row 211\)" % GANONDORF
BALL_HERE = r"coop: shots: enemy shot %s at " % BALL
ANIM = re.compile(r"coop: objects: anim %s 0x[0-9a-f]+ '([^']*)'( \(ours\))?" % GANONDORF)
COPY_STUN = r"coop: objects: share stunned here, said to the owner: %s" % GANONDORF


def whole(pair, x, pattern):
    """A line of game x's module log matching `pattern` since the game started (the boot's lines are before the test)."""
    return pair.log[x].find(pattern, pos=0)


def sync_both(pair):
    for x in ("A", "B"):
        assert whole(pair, x, SYNC_ON), "%s: no '%s' line (needs a hookshot.exe with the brain/action wraps)" % (
            x, SYNC_ON)
        assert whole(pair, x, SYNC_OFF) is None, "%s: %s" % (x, whole(pair, x, SYNC_OFF))


def roles(pair, boss):
    """{game: 'ours'|'a copy'} from each game's newest sync line for `boss` (whole log)."""
    rx = re.compile(sync_on(boss))
    out = {}
    for x in ("A", "B"):
        for ln in pair.log[x].since(0):
            m = rx.search(ln)
            if m:
                out[x] = m.group(1)
    return out


def boss_entry(pair, x, boss):
    for a in (((pair.state(x).get("world") or {}).get("actors")) or []):
        if a.get("name") == boss:
            return a
    return None


def one_owner(pair, boss, tag):
    """Exactly one game says 'ours', the other 'a copy'; both world lists name the owner game's player number. Returns
    (owner game, copy game)."""
    pair.until("%s's sync line in both games" % boss, lambda: len(roles(pair, boss)) == 2, max_frames=600)
    r = roles(pair, boss)
    ours = [x for x, v in r.items() if v == "ours"]
    assert len(ours) == 1, "%s %s: not exactly one owner game (%s)" % (tag, boss, r)
    owner = ours[0]
    copy = "B" if owner == "A" else "A"
    want = own_number(pair.state(owner))
    for x in ("A", "B"):
        e = boss_entry(pair, x, boss)
        assert e is not None, "%s %s: not in %s's world list" % (tag, boss, x)
        assert e.get("alive") is not False, "%s %s: not alive in %s (%s)" % (tag, boss, x, e)
        assert e.get("owner") == want, "%s %s: %s names owner %s, game %s (player %s) says ours" % (
            tag, boss, x, e.get("owner"), owner, want)
    return owner, copy


def counts(lines):
    last = None
    for ln in lines:
        m = COUNTS.search(ln)
        if m:
            last = m
    return tuple(int(last.group(i)) for i in (1, 2, 4, 5)) if last else (0, 0, 0, 0)


def press_both(pair, buttons, frames, stick=None):
    """Both games hold the same pad for `frames` frames (A's input, then B's: each runs those frames)."""
    for x in ("A", "B"):
        args = {"buttons": list(buttons), "frames": int(frames)}
        if stick is not None:
            args["stick"] = list(stick)
        pair.call(x, "input", **args)


def look_both(pair, name, missed):
    for x in ("A", "B"):
        try:
            pair.check_shot(x, name, tolerance=SHOT_TOLERANCE)
        except AssertionError as e:
            missed.append(str(e))


# -- Barinade ----------------------------------------------------------------------------------------------------
INTRO_LEFT = 600        # frames: hs-barinade-native 'wait 1800' after the event (ticks // 2 = 900) less the boot's 'wait 600' (300)


def swing_round(pair, x):
    """hs-barinade-native: 'hold ZL+LSTICK_UP 60; hold ZL+Y 8; wait 12; hold ZL+Y 8' (ticks) on game x."""
    pair.press(x, ["ZL"], stick=UP, frames=30)
    pair.press(x, ["ZL", "Y"], frames=4)
    pair.step_both(6)
    pair.press(x, ["ZL", "Y"], frames=4)


@pytest.mark.needs_boot(BARINADE_BOOT)
@pytest.mark.budget(600)        # hs-barinade-native '#! timeout 600'; ~3000 frames + reads and shots
@pytest.mark.look
@pytest.mark.needs_env(*SWITCHES)
@pytest.mark.row("regress-01113 step 7: Barinade boss route, native boss sync in both games (hs-barinade-native, s7-boss)")
def test_barinade_native_sync(pair):
    missed = []
    with pair.setup():
        in_stage(pair, BARINADE_STAGE)
        sync_both(pair)
        keep_up(pair)
    base = {x: counts(pair.log[x].since(0)) for x in ("A", "B")}
    pair.step_both(INTRO_LEFT)                      # the intro, then the fight
    owner, copy = one_owner(pair, BARINADE, "barinade")
    pair.until("copy %s: %s's owner's verbs came" % (copy, BARINADE),
               lambda: whole(pair, copy, verbs_came(BARINADE)), max_frames=600)   # (it may come in the boot's frames)
    for x in ("A", "B"):
        pair.press(x, ["ZL"], frames=5)             # 'press ZL 10': lock on
    for i in (1, 2, 3):
        look_both(pair, "barinade-%d" % i, missed)  # a-1..3 / b-1..3: the same phase and move on both screens
        if i < 3:
            swing_round(pair, "A")
            pair.step_both(350)                     # B swings between A's swings ('wait 700' ticks)
            swing_round(pair, "B")
            pair.step_both(400)                     # 'wait 800'
            keep_up(pair)
    pair.step_both(300)                             # the counts line comes every 10 s (coop_hookshot.cpp LogBossNative)
    now = {x: counts(pair.log[x].since(0)) for x in ("A", "B")}
    sent, put = now[owner][1] - base[owner][1], now[copy][3] - base[copy][3]
    pair.notes.append("barinade: owner %s sent %d action verbs; copy %s put %d in" % (owner, sent, copy, put))
    assert sent > 0, "the owner game %s sent no action verb in the fight" % owner
    assert put > 0, "the copy game %s put none of the owner's action verbs in" % copy
    one_owner(pair, BARINADE, "barinade end")
    for x in ("A", "B"):
        pair.never_log(x, CRASH)
    assert not missed, "%d look miss(es): %s" % (len(missed), "; ".join(missed[:4]))


# -- Ganondorf's ball ---------------------------------------------------------------------------------------------
RALLY_FRAMES = 4500     # hs-ganondorf-rally: swings for 5 min from the walk-in (18000 ticks); ESTIMATED as half that
#                         (2.5 min of fight, ~6 of his volleys at the shot-reflect-211-4 run's rate): the first lane run decides


def walk_in(pair):
    """hs-ganondorf-rally A/B: 'hold LSTICK_UP 120; wait 60; hold LSTICK_UP 120; wait 600' (ticks) on both games."""
    press_both(pair, [], 60, stick=UP)
    pair.step_both(30)
    press_both(pair, [], 60, stick=UP)
    pair.step_both(300)


@pytest.mark.needs_boot(GANON_BOOT)
@pytest.mark.budget(900)        # hs-ganondorf-rally '#! timeout' none; 4500 frames + reads; the first lane run's time replaces it
@pytest.mark.look
@pytest.mark.needs_env(*SWITCHES)
@pytest.mark.row("#211 regress-01113 step 7: Ganondorf's ball sent back in co-op (hs-ganondorf-rally, s7-boss 'Ganondorf ball')")
def test_ganondorf_ball_sent_back(pair):
    missed = []
    with pair.setup():
        in_stage(pair, GANON_STAGE)
        sync_both(pair)
        keep_up(pair)
    walk_in(pair)
    owner, copy = one_owner(pair, GANONDORF, "ganondorf")
    ran, parry, stun = 0, None, None
    while ran < RALLY_FRAMES:                       # both locked on (ZL), a swing every 15 ticks (ZL+Y 6, ZL 9)
        press_both(pair, ["ZL", "Y"], 3)
        press_both(pair, ["ZL"], 5)
        ran += 8
        if ran % 240 == 0:
            keep_up(pair)
            seq = [(m.group(1), bool(m.group(2))) for m in map(ANIM.search, pair.log[owner].since()) if m]
            if parry is None:
                parry = next((i for i, (a, ours) in enumerate(seq) if ours and "Parry_Ready" in a), None)
            if parry is not None and any(ours and "LightBallDamage_Start" in a for a, ours in seq[parry:]):
                stun = ran
                break
    pair.notes.append("ganondorf: owner %s, copy %s; %d frames of swings; parry %s; owner stun at %s" % (
        owner, copy, ran, "seen" if parry is not None else "none", stun))
    assert pair.log[owner].find(RALLY), "owner %s: no '%s' line: no rally ball flew, or a turn cut it" % (owner, RALLY)
    assert pair.log[copy].find(BALL_HERE), "copy %s: no %s seen while a copy" % (copy, BALL)
    if parry is None:
        pair.unproven("no ball was sent back in %d frames (no Parry_Ready of the owner's Ganondorf): the ball flew in "
                      "both games, its return not reached by these swings" % ran)
    assert stun is not None, "owner %s: Parry_Ready (a ball sent back) but no LightBallDamage_Start after it" % owner
    pair.until("copy %s: Ganondorf down too (LightBallDamage_Start or 'stunned here')" % copy,
               lambda: any("LightBallDamage_Start" in m.group(1) for m in map(ANIM.search, pair.log[copy].since()) if m)
               or pair.log[copy].find(COPY_STUN), max_frames=150)
    one_owner(pair, GANONDORF, "ganondorf down")
    look_both(pair, "ganondorf-down-1", missed)     # shot-reflect-211-4 (c): Ganondorf knocked down in BOTH windows
    pair.step_both(6)
    look_both(pair, "ganondorf-down-2", missed)
    for x in ("A", "B"):
        pair.never_log(x, CRASH)
    assert not missed, "%d look miss(es): %s" % (len(missed), "; ".join(missed[:4]))
