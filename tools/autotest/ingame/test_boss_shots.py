"""Rows 222 and 286 (twinrova-shots-222): the owner's game decides a boss's shots; a copy's game makes the owner's.

| Test | Old row (scenario + run script) | Kind |
|---|---|---|
| test_twinrova_shots | row-222-shots-twinrova.{steps,A,B}.txt + offline/row-222-shooter-run.ps1 | log (counts, bullets) + LOOK (paired shots on each game) |
| test_phantom_ganon_shots | row-286-shots-phantomganon.{steps,A,B}.txt + the same run script | log (counts, bullets) + LOOK (paired shots on each game) |

Old and new judge the same thing: each old `lines` check of the .steps.txt is an assert with the SAME pattern, and the
run script's judge (offline/row-222-shooter-run.ps1, judge block) is `judge_shots` below:
  - per game the last "boss shots" counts line (coop_hookshot.cpp LogBossShooter);
  - UNPROVEN (= FAIL here, the message says so) when no owner's shot was sent in one game and made in a copy;
  - FAIL when a judged bullet ("coop: objects: + <name> 0x..", coop_objects.cpp, LICHII_COOP_DEBUG_OBJECTS=1) is made
    in one game only; UNPROVEN when no judged bullet was made at all.
The counts are the module's totals since the game started (LogBossShooter's static Counts), and this suite boots the
pair once for every test: each test judges the change from the last counts line before it began.

Waits: the old scripts count TICKS, 60 a second (hookshot.py TICKS); the bridge steps game frames, 30 a second
(pair.FPS), so every old wait / hold is ticks // 2 frames here (`fr`). The old title + save load part (wait 1200,
press A x4, wait 2400 up to mark a-in) is gone: the lane is in play already; each test starts at the old mark a-in.

Shots (B #130 word 3, 09:43): the boss attacks are user-facing, so every paired shot of the old run (a-p1-NN, f-NN,
p2-NN on both games at the same moment, and its 'shot end') is a `pair.check_shot` per game, compared to refs/ by the
harness (tolerance 0.80: a scene with moving things, docs/ingame-tests.md step 6, as boss-respawn's rocks). The first run
gives NO-REF candidates (<out>/lane<L>/new/); only a reviewer who looked at them adopts them into refs/. The log and
state judge comes FIRST: a look miss is collected and asserted after it, so a row FAIL always says which one.
"""
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import playtest_client as pc   # noqa: E402
sys.path.insert(0, HERE)
import pair as P                   # noqa: E402

# The old rows' '#! switches' line (row-222-shots-twinrova.A.txt:12, row-286-shots-phantomganon.A.txt:8).
SWITCHES = ("LICHII_COOP_DEBUG_BOSS=1", "LICHII_COOP_DEBUG_OBJECTS=1", "LICHII_COOP_CHEATS=1")

# .steps.txt checks, the same patterns (row-222-shots-twinrova.steps.txt / row-286-shots-phantomganon.steps.txt).
SHOOTER = r"coop: bosses: boss shots the owner's \(wrapped\)"   # coop_hookshot.cpp SetBossShooter (module fix + patch 0045)
CRASH = r"exception 0x|FATAL"                                    # no-crash-A/B: max=0


def sync_on(boss):
    """coop_boss_native.cpp:527 Note(b.name + ": native boss sync on (BSA " + Hex(b.bsa) + ", ours|a copy)")."""
    return r"%s: native boss sync on \(BSA 0x[0-9a-f]+, (ours|a copy)\)" % boss


# The run script's counts regex (offline/row-222-shooter-run.ps1 $counts_re), the text of coop_hookshot.cpp
# LogBossShooter: groups 1 = ours sent, 5 = owner's made.
COUNTS = re.compile(r"boss shots: ours sent ([0-9]+) \(([0-9]+) refused by the game\); copies: ([0-9]+) own skipped, "
                    r"([0-9]+) own run before the owner's came, ([0-9]+) owner's made \(([0-9]+) refused\), ([0-9]+) no "
                    r"template, ([0-9]+) not prepared, ([0-9]+) dropped, ([0-9]+) started again, ([0-9]+) put back")


def fr(ticks):
    """An old script's TICKS (60 a second, hookshot.py TICKS) as game frames (30 a second, pair.FPS)."""
    return max(1, int(ticks) // 2)


# -- driving ----------------------------------------------------------------------------------------------------
def warp_in(pair, stage):
    """Both games into a boss stage at its own start (coop_start_points.inc rows TwinrovaStage:121,
    PhantomGanonStage:112; the old 'warp <Stage> <Stage>'), asserted per game.
    Called AFTER pair.setup(), never inside it: a two-game warp measures 26.9-31.0 s (pair.py WARP_PAIR_MEASURED_S) and
    the set-up limit is 30 s (SETUP_LIMIT_S; docs/ingame-tests.md 'a set-up that warps cannot fit 30 s'). In
    tb-rows-f9f3b9c2-2 the boot-place check + A's boss warp used ~17 s, then B's half of the warp did not fit the 13 s
    left (SETUP-FAIL 'step not sent (13.2 s left, its reply needs 13.6 s)'). The warp is the row's own first step (the old A.txt:15 / B.txt:3), so a warp that does not arrive is a
    FAIL of this row, inside the test's own budget."""
    st = pair.warp_both(stage, entrance=stage, arrive_in_event=True)
    for x, s in st.items():
        assert (s.get("scene") or "").lower() == stage.lower() and not s.get("loading"), "%s after the warp: %s" % (
            x, s.get("scene"))


# The longest boss intro measured, in game frames from its start event to its end, by each game's own log and its
# 'vi: N frames presented' counter (run twinrova-222-a2e69946, lane 1, module 182cd53b5; offline/twinrova-222-8/
# run-a2e69946/lane1/session): Twinrova, 'SageOfSpirit_BossStart' (taken) -> 'event false' + 'the game's play UI is up':
# B game-out-B.txt.1:6267 (frames 4493-4584) -> :9050 (8273-8569) = 3690-4080; A game-out-A.txt.1:6505 (5099-5331) ->
# :9593 (8873-8950) = 3540-3850. Phantom Ganon (same run): arrival :10025 (8550-8804) -> play UI up :11375 (10114-10295)
# = 1310-1745. The old scripts' Twinrova 'wait 1800' (fr(1800) = 900 frames) was far short: that run's SETUP-FAIL.
INTRO_MEASURED_MAX = 4080
INTRO_MARGIN = 1.25                 # 25% over the longest measured (one run of each boss so far)
# The most frames fight_ready waits after a test's fixed intro wait: the margined longest intro minus the shortest
# fixed wait before it (Twinrova's fr(1800)). A ceiling, not a timing: the wait ends at the first read with the play UI
# up and no event, and every frame counts in the test's own budget.
FIGHT_UI_FRAMES = int(INTRO_MEASURED_MAX * INTRO_MARGIN) - fr(1800)       # = 4200
# Budgets from the frames each test steps at the bridge's measured pace: test_phantom_ganon_shots ran ~8700 frames in
# 562.1 s (run twinrova-222-a2e69946 summary.md) = 15.5 frames/s; +25%. Twinrova: ~11400 frames + FIGHT_UI_FRAMES.
STEP_FPS = 15.5
TWINROVA_FRAMES = 11400 + FIGHT_UI_FRAMES
TWINROVA_BUDGET_S = int(TWINROVA_FRAMES / STEP_FPS * 1.25)                # = 1258 (the old row's 900 s did not fit)
PG_FRAMES = 8700 + FIGHT_UI_FRAMES  # (its fight_ready may wait the same ceiling after its own intro wait)
PG_BUDGET_S = int(PG_FRAMES / STEP_FPS * 1.25)                            # = 1040


def fight_ready(pair, stage):
    """Before the first ZL: both games out of the boss intro, with the play UI up (review-tests-153-3, B #161 00:3x).
    The warp is arrive_in_event (warp_in), and since 7f23bf2d1 an arrive_in_event warp no longer waits for the play UI
    (the intro holds it down: row-215-confirm-4, probe-intro-53d40ab3-2), so after the fixed intro wait a game can still
    be in its intro and a ZL there is not the fight's lock-on. Pair.until_in_play steps both games until each has a
    fresh Link sample, no load, no event and its play UI not down; then each game's last play UI line must be 'up'
    (pair.PLAY_UI, the module's own line: none at all = the intro's end not proven). Either one missing within
    FIGHT_UI_FRAMES is a SETUP-FAIL naming the game, never a FAIL of the shots."""
    try:
        pair.until_in_play("%s: the boss intro over (play UI up) before the first ZL" % stage, max_frames=FIGHT_UI_FRAMES)
    except P.TestTimeout:
        raise
    except AssertionError as e:
        raise P.SetupFail("%s: the boss intro not over %d frames after the intro wait: %s" % (stage, FIGHT_UI_FRAMES, e)
                          ) from None
    for x in ("A", "B"):
        ui = pair.play_ui(x)
        if ui != "up":
            raise P.SetupFail("%s: %s: no 'the game's play UI is up' line after the intro (last: %s); a ZL now may land in "
                              "the intro" % (stage, x, ui))


def hold_both(pair, buttons, frames):
    """Both games hold `buttons` for `frames` frames (the old A and B scripts' 'hold KEYS TICKS' at the same time)."""
    for x in ("A", "B"):
        pair.call(x, "input", buttons=list(buttons), frames=int(frames))


def act_note(pair, x, line):
    """An old script's kill / hit line. In the old run a strike the game refused was a log line, not a failure (the
    checks after it judge); here the same: the refusal goes into the test's notes."""
    try:
        pair.act(x, line)
    except pc.BridgeError as e:
        pair.notes.append("%s '%s' refused: %s %s" % (x, line, e.code, e))


def act_both(pair, line):
    for x in ("A", "B"):
        act_note(pair, x, line)


class Shots:
    """The row's paired shots through Pair.check_shot (A's marks shot both games in the old run: hookshot.py shooter()).
    A shot runs its game up to 5 frames (docs/playtest.md `shot`): the game that ran fewer is stepped to the same
    count. A look miss does not stop the test: it is kept (Pair.shots, ok false) and asserted by check() at the end."""
    TOLERANCE = 0.80   # moving scene (docs/ingame-tests.md "Porting" step 6; boss-respawn-bridge rocks 0.80)

    def __init__(self, pair, test):
        self.pair, self.test, self.missed = pair, test, []

    def both(self, name):
        before = {x: self.pair.c[x].last_frame or 0 for x in ("A", "B")}
        for x in ("A", "B"):
            try:
                self.pair.check_shot(x, "%s-%s" % (self.test, name), tolerance=self.TOLERANCE)
            except AssertionError as e:
                self.missed.append(str(e))
        ran = {x: (self.pair.c[x].last_frame or 0) - before[x] for x in ("A", "B")}
        top = max(ran.values())
        for x, n in ran.items():
            if top > n:
                self.pair.step_other(x, top - n)

    def check(self):
        assert not self.missed, "%d look miss(es): %s" % (len(self.missed), "; ".join(self.missed[:4]))


# -- judging ----------------------------------------------------------------------------------------------------
def last_counts(lines):
    last = None
    for ln in lines:
        m = COUNTS.search(ln)
        if m:
            last = m
    return (int(last.group(1)), int(last.group(5))) if last else (0, 0)


def counts_before(pair):
    """Each game's (ours sent, owner's made) on the last counts line before this test began (0, 0: none yet)."""
    out = {}
    for x in ("A", "B"):
        lg = pair.log[x]
        with open(lg.path, "rb") as f:
            head = f.read(lg.pos).decode("utf-8", "replace").splitlines()
        out[x] = last_counts(head)
    return out


def judge_shots(pair, base, judged, info):
    """offline/row-222-shooter-run.ps1's judge, on this test's lines. `judged` = the bug's own symptom bullets."""
    pair.step_both(fr(600))            # the old scripts' last 'wait 600' before 'mark end'
    try:                               # LogBossShooter says its counts every 10 s while they change
        pair.until("a boss shots counts line in either game",
                   lambda: any(pair.log[x].find(COUNTS.pattern) for x in ("A", "B")), max_frames=600)
    except AssertionError:
        pass                           # (judged below: no line = nothing sent or made)
    sent, made, bullets, said = {}, {}, {}, []
    rx = re.compile(r"coop: objects: \+ (%s) 0x" % info)
    for x in ("A", "B"):
        lines = pair.log[x].since()
        s, m = last_counts(lines)
        if not any(COUNTS.search(ln) for ln in lines):
            s, m = base[x]
        sent[x], made[x] = s - base[x][0], m - base[x][1]
        names = {}
        for ln in lines:
            g = rx.search(ln)
            if g:
                names[g.group(1)] = names.get(g.group(1), 0) + 1
        bullets[x] = names
        said.append("%s: ours sent %d, owner's made %d, bullets %s" % (
            x, sent[x], made[x], ", ".join("%s x%d" % kv for kv in sorted(names.items())) or "none"))
    pair.notes.extend(said)
    assert sum(sent.values()) > 0 and sum(made.values()) > 0, (
        "UNPROVEN: no owner's shot sent and made in a copy (%s)" % "; ".join(said))
    one_sided = ["%s (A %d, B %d)" % (n, bullets["A"].get(n, 0), bullets["B"].get(n, 0)) for n in judged
                 if (bullets["A"].get(n, 0) == 0) != (bullets["B"].get(n, 0) == 0)]
    assert not one_sided, "made in one game only: %s (%s)" % (", ".join(one_sided), "; ".join(said))
    assert any(bullets["A"].get(n, 0) + bullets["B"].get(n, 0) for n in judged), (
        "UNPROVEN: none of the judged bullets was made (%s): %s" % (", ".join(judged), "; ".join(said)))


def need_in_play(pair):
    """The shared default-boot precondition: both games STEPPED until in play, in the boot's scene (Pair.need_boot_place,
    harness-c133-scene). Was one state read per game: right after the lane boot (tb-rows has no smoke tier first) a
    game with no Link sample yet failed as 'not in play' (gifts-237 / row53 SETUP-FAIL class, port-g5-237 16:25)."""
    pair.need_boot_place()


def shooter_on(pair):
    """shooter-A/B: logged once at the world hello (coop_hookshot.cpp:2149 SetBossShooter), so since the game started."""
    for x in ("A", "B"):
        assert pair.log[x].find(SHOOTER, pos=0), "%s: no '%s' since the start (module fix or patch 0045 missing)" % (
            x, SHOOTER.replace("\\", ""))


# -- row 222 ----------------------------------------------------------------------------------------------------
# Judged bullets: the run script's row-222 list (Twinrova_Bullet_FireBullet/FireField/IceBullet/IceField, the bug's
# own symptom lines); the info pattern 'Twinrova_Bullet_\w+'.
TWINROVA_JUDGED = ("Twinrova_Bullet_FireBullet", "Twinrova_Bullet_FireField", "Twinrova_Bullet_IceBullet",
                   "Twinrova_Bullet_IceField")


@pytest.mark.budget(TWINROVA_BUDGET_S)   # (was the old row's '#! timeout 900', row-222-shots-twinrova.A.txt:13: too short)
@pytest.mark.look
@pytest.mark.needs_env(*SWITCHES)
@pytest.mark.row("#222 Twinrova's shots are the owner's in both games (row-222-shots-twinrova)")
def test_twinrova_shots(pair):
    base = counts_before(pair)
    shots = Shots(pair, "twinrova")
    with pair.setup():
        need_in_play(pair)
        shooter_on(pair)
    # 'warp TwinrovaStage TwinrovaStage' (A.txt:15, B.txt:3); start point coop_start_points.inc:121
    # the shared Pair.warp_both: entrance = the start -> the map change event (a boss stage in free play), and
    # arrive_in_event: the boss intro starts on arrival, so a 'timeout' reply with Link in the scene is the arrival
    # (replaces the old 'coop: test warp done' check, warp-A/B). Outside the set-up: see warp_in.
    warp_in(pair, "TwinrovaStage")
    # mark a-in -> 'wait 1800  # the intro, then the fight' (A.txt:27, B.txt:15); sync-A/B on the way
    pair.step_both(fr(1800))
    for x in ("A", "B"):
        pair.expect_log(x, sync_on("Enemy_DungeonBoss_Koume"), max_frames=fr(1200))
    fight_ready(pair, "TwinrovaStage")               # (the intro over, play UI up: no ZL into the intro)
    hold_both(pair, ["ZL"], fr(240))                  # 'hold ZL 240'
    for i in range(1, 11):                            # 'hold ZL 90 a-p1-NN' + 'wait 510' (none after the 10th)
        hold_both(pair, ["ZL"], fr(90))
        shots.both("a-p1-%02d" % i)
        if i < 10:
            pair.step_both(fr(510))
    act_both(pair, "kill Enemy_DungeonBoss_Koume")    # 'kill Enemy_DungeonBoss_Koume', 'wait 60'
    pair.step_both(fr(60))
    act_both(pair, "kill Enemy_DungeonBoss_Kotake")   # 'kill Enemy_DungeonBoss_Kotake', 'wait 600', mark a-k
    pair.step_both(fr(600))
    pair.step_both(fr(2400))                          # 'wait 2400  # the merge cutscene'; fused-A/B on the way
    for x in ("A", "B"):
        pair.expect_log(x, sync_on("Enemy_DungeonBoss_Twinrova"), max_frames=fr(1200))
    hold_both(pair, ["ZL"], fr(240))                  # 'hold ZL 240'
    for i in range(1, 25):                            # 'hold ZL 90 f-NN' + 'wait 60' (A.txt: 24 of them)
        hold_both(pair, ["ZL"], fr(90))
        shots.both("f-%02d" % i)
        pair.step_both(fr(60))
    judge_shots(pair, base, TWINROVA_JUDGED, r"Twinrova_Bullet_\w+")
    shots.both("end")                                 # the .steps.txt 'shot end'
    for x in ("A", "B"):                              # (a crash is never reported as a look miss)
        pair.never_log(x, CRASH)
    shots.check()


# -- row 286 ----------------------------------------------------------------------------------------------------
# Judged bullet: the run script's row-286 list (DungeonBossPhantomGanon_Bullet_LightBall: owner A 2, copy B 0 in
# shots-lane1\hs-phantom-ganon-ball\20261120-135931); the info pattern '\w*PhantomGanon_Bullet_\w+'.
PG = "Enemy_DungeonBoss_PhantomGanon"
PG_JUDGED = ("DungeonBossPhantomGanon_Bullet_LightBall",)


@pytest.mark.budget(PG_BUDGET_S)         # (was the old row's '#! timeout 900', row-286-shots-phantomganon.A.txt:9)
@pytest.mark.look
@pytest.mark.needs_env(*SWITCHES)
@pytest.mark.row("#286 Phantom Ganon's light balls are the owner's in both games (row-286-shots-phantomganon)")
def test_phantom_ganon_shots(pair):
    base = counts_before(pair)
    shots = Shots(pair, "phantomganon")
    with pair.setup():
        need_in_play(pair)
        shooter_on(pair)
    # 'warp PhantomGanonStage PhantomGanonStage' (A.txt:11, B.txt:2); start point coop_start_points.inc:112
    warp_in(pair, "PhantomGanonStage")                # (outside the set-up: see warp_in)
    # mark a-in -> 'wait 3900  # the intro, then the fight (run 040054: no boss at 2400, lane1 save)'; sync-A/B
    pair.step_both(fr(3900))
    for x in ("A", "B"):
        pair.expect_log(x, sync_on(PG), max_frames=fr(1200))
    fight_ready(pair, "PhantomGanonStage")            # (the intro over, play UI up: no ZL into the intro)
    for x in ("A", "B"):                              # 'press ZL 10  # lock on' (both), 'wait 600', mark t1
        pair.press(x, ["ZL"], frames=fr(10))
    pair.step_both(fr(600))
    # Phase 1, both scripts from mark t1 in ticks (hs-phantom-ganon-ball.A/B, unchanged): A 'hit .. 6' at 0, 600,
    # 1200, 1800 (mark t5 at 2400); B 'hit .. 2' at 300, 900, 1500, then 'hit .. 1' every 150 from 1800 to 2550
    # (mark t4b at 2700). Played here in that order on one clock.
    plan = [(0, "A", 6), (300, "B", 2), (600, "A", 6), (900, "B", 2), (1200, "A", 6), (1500, "B", 2), (1800, "A", 6)]
    plan += [(1800 + 150 * k, "B", 1) for k in range(6)]
    plan.sort(key=lambda p: (p[0], p[1]))
    t = 0
    for tick, x, n in plan:
        if tick > t:
            pair.step_both(fr(tick - t))
            t = tick
        act_note(pair, x, "hit %s %d" % (PG, n))
    pair.step_both(fr(2700 - t))                      # to B's mark t4b
    # B at t4b: 'goto -3.9 -65.7 0.4 300' (goto X Z RADIUS TICKS, hookshot.py GOTO) + 'toward 0 -63 10' -> place +
    # face (porting rule: a walk only to get somewhere is a place; Y = B's own height). Phase 2: A from mark t5 (2400)
    # and B from t4b (2700) each do 24 x 'hold ZL 90' + 'wait 210' (A's with marks p2-NN): 300 ticks a round, so the
    # old A's hold k+1 fell with B's hold k; here both start at 2700 and hold together (A one round later, same rounds).
    y = pair.state("B")["pos"][1]
    pair.act("B", "place -3.90 %.2f -65.70" % y)
    pair.act("B", "face 0.00 -63.00")
    for i in range(1, 25):
        hold_both(pair, ["ZL"], fr(90))
        shots.both("p2-%02d" % i)
        pair.step_both(fr(210))
    judge_shots(pair, base, PG_JUDGED, r"\w*PhantomGanon_Bullet_\w+")
    shots.both("end")                                 # the .steps.txt 'shot end'
    for x in ("A", "B"):                              # (a crash is never reported as a look miss)
        pair.never_log(x, CRASH)
    shots.check()
