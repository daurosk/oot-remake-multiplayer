"""Rows 289 and 234 in the bridge form (boss-respawn-289-4, B #130: port of coord-multiplayer offline
boss-respawn-289-run.ps1, preflight CLEAN review-c123-4 06:57 UTC 2026-11-26, never run: parked, then retired).

| Test | Old pass (boss-respawn-289-run.ps1) | Proves |
|---|---|---|
| test_respawn_shows_the_stage[<Stage>] | volvagia, dodongo, lizalfos, ganon | row 289 fixed: after a bleed-out in a boss stage the respawn "to the entrance" shows the stage and gives control back |
| test_respawn_black_with_starter_kept | volvkeep | the cause (report boss-respawn-289.md point 7): the same steps with the game's reservation kept (LICHII_COOP_ARRIVAL_STARTER=keep) stay BLACK |
| test_volvagia_rocks_each_game | rocks234 (hs-volvagia-rocks) | row 234: each game makes its own falling rocks in the Volvagia fight, both Links alive (ten heart gives) |

volvkeep BLACK and the fixed test's picture on the same steps = cause point 7 proven (only the switch differs).

Sources (worker-rules "no guessed values"):
- Stages: lichii/hook/coop_start_points.inc, each {"<S>", "<S>", x, y, z, false} (start of the stage's own name, no
  entrance actor), the old run's pass list.
- Warp: the bridge `warp` with an entrance goes by the game's map change event in free play
  (hookshot/src/coop_playtest_core.cpp PlanWarp: entrance_given -> map_event; coop_playtest.cpp ask_map_event), as the old
  scenarios' 'warp <S> <S>' (scenarios/hs-volvagia-rocks.A.txt; lane2 run 20261125-114351 "test warp to VolvagiaStage
  (start VolvagiaStage): scene set").
- nofairy: lichii/hook/coop_testinput.cpp NOFAIRY = setenum of the five BottleItem hashes to Empty (copied below as the
  module has them, taken from hookshot.BOTTLE_ITEMS: `act` refuses 'nofairy', it is not an act kind, docs/playtest.md `act`).
- '/dbno time 10' (A, the host), 'hurt 80' (B): hs-gohma-bleedout (row 227) and br289-volvagia.{A,B}.txt; the chat's
  answer line: coop_testinput.cpp:1758 "coop: test chat '{}': {}".
- Log lines (the old .ps1's own patterns, :344-:383): "coop: dbno: our Link: event N " (coop_dbno_game.cpp:523; enum
  Event 0 down, 2 bled out, 7 respawned); "coop: teleport: respawn at the entrance <S> of <S>" (coop_teleport_game.cpp:484);
  "asking the game's map change event Dm_SY_BancChangeGo for <S> at <S>" (:600); "live event Dm_SY_BancChangeGo asked of
  the game (result 1)"; "control: state loading -> play"; "coop: event: our Link's event began" (coop_objects.cpp
  EventLog); fix ":433 <S> has no entrance actor named <S>: the arrival event's starter emptied"; keep ":612 ... the
  starter is kept (LICHII_COOP_ARRIVAL_STARTER=keep)"; rocks "coop: objects: rocks: N new falling rocks" (coop_objects.cpp:6896).
- Frame waits: br289-volvagia.{A,B}.txt (a-in -> b-before 1800, b-before -> b-down 600, b-down -> b-arrived 1500,
  b-arrived -> b-later 1200, 'hold LSTICK_UP 60' + 'wait 600' -> b-end) and hs-volvagia-rocks.{A,B}.txt.
- Black / picture: the mean grey of the shot (0-255, RGB averaged), as workers-a/volvagia-289-tools/mean.py on the black
  run 20261125-114351 B (picture 66.9-74.8, black 0.0): < 5 black, > 15 picture, between = not decided (a FAIL).
- Switches: the old A header '#! switches LICHII_COOP_DEBUG_BOSS=1,LICHII_COOP_DEBUG_OBJECTS=1,...'. Only
  LICHII_COOP_DEBUG_OBJECTS is needed for a judged line (rocks; event log, same file); DEBUG_BOSS / SCENE_PROBE gave
  info lines the old row never judged, so they are not asked for. LICHII_COOP_CHEATS: the bridge runner sets it always
  (docs/playtest.md "The verb gate").
"""
import math
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bt_plugin   # noqa: E402  (boot_env / env_has: which switches this boot has)
import pair as P   # noqa: E402
sys.path.insert(0, os.path.dirname(HERE))
import hookshot    # noqa: E402  (BOTTLE_ITEMS: the ONE nofairy list, the scripts' own)

STAGES = ["VolvagiaStage", "DodongoStage", "LizalfosStage", "GanonStage"]   # coop_start_points.inc, the old pass list
BOTTLES = hookshot.BOTTLE_ITEMS   # nofairy's bottles (the scripts' list; checked == coop_testinput.cpp kBottleItems)
KEEP = "LICHII_COOP_ARRIVAL_STARTER=keep"     # coop_teleport_game.cpp:68-71
OBJECTS = "LICHII_COOP_DEBUG_OBJECTS=1"
BACK_TO = "KakarikoVillage_Child"            # test_smoke.WARP_TO: the lane's boot map, so later tests start in play
BLACK, PICTURE = 5.0, 15.0                    # mean.py thresholds (module docstring)
WARP_S = 31.0          # worst warp of one game on a lane: 26.9/28.8/31.0 s (party-hud-bridge-6947aa3d test_warp r1-r3, as P.recover)
ARRIVAL_EVENT_S = 48.0  # B's boss-stage arrival event, run 20261125-114351
IN_PLAY_FRAMES = 2400   # the old scenarios' 'wait 2400' after the load (br289-volvagia, hs-volvagia-rocks)
BOSS_ARRIVAL_S = 2 * (2 * (WARP_S + ARRIVAL_EVENT_S) + IN_PLAY_FRAMES / P.FPS)   # = 476 s, a CEILING (2x margin)
# B #130 (09:43, via review-rows-c130): the owner's 30 s is the SET-UP CHECK (pair.setup(): in play, no fairy, gives),
# kept first and at the default limit. The boss-stage arrival is a step after it with its own hard limit: a scene load
# plus the stage's own arrival event, A then B, then both in play. A miss there is SETUP-FAIL 'boss arrival in <S>: ...'
# naming the phase it was in and each phase's time (boss_arrival).
# boss-respawn-bridge-e3e6e253 (15:46, lane 2): the old 150 s (fit for 13.5-15.5 s warps) ran out on a lane whose
# warps take 27-31 s; both games were in the stage, its arrival over, Volvagia up (game logs), when the time was gone.
# The bound is a ceiling only (too short = SETUP-FAIL, never a PASS); the phase times each run notes are the measure.


def mean_grey(path):
    from PIL import Image, ImageStat
    with Image.open(path) as im:
        return sum(ImageStat.Stat(im.convert("RGB")).mean) / 3.0


def screen(pair, x, name):
    """A bridge shot of game x (framebuffer), kept with the result, and its mean grey."""
    r = pair.call(x, "shot", name="%s-%s" % (name, x))
    path = r.get("path")
    assert path and os.path.exists(path), "%s: no shot file for %s (%s)" % (x, name, r)
    v = round(mean_grey(path), 1)
    pair.shots.append({"game": x, "name": name, "path": path, "ref": None, "score": v, "ok": True,
                       "why": "mean grey %.1f" % v})
    pair.notes.append("%s %s mean %.1f" % (x, name, v))
    return v


def start_check(pair):
    """The set-up check (inside pair.setup(), <= 30 s): both games in play at the boot's place (Pair.need_boot_place:
    stepped to a fresh in-play sample, never one read; B #139 16:26), no bottled fairy (a fairy revives instead of
    downing)."""
    pair.need_boot_place()
    for x in ("A", "B"):
        for h in BOTTLES:
            pair.act(x, "setenum %s Empty" % h)


def boss_arrival(pair, stage, then=None):
    """Both games to `stage` at its own start, in play: a bounded step after the set-up check (BOSS_ARRIVAL_S); any
    miss is SETUP-FAIL 'boss arrival in <stage>: ...' (the row's own check never ran).
    The warp counts Link in the stage during its arrival event (arrive_in_event: a boss stage's intro can outlast the
    bridge's 60 s 'no event' wait), then both games run until both are in play there: 2400 frames at most, the old
    scenarios' wait after the load ('wait 2400' before a-in / b-in, br289-volvagia and hs-volvagia-rocks)."""
    outer = pair.deadline
    limit = max(0.0, min(BOSS_ARRIVAL_S, outer.left()))
    pair.deadline = P.Deadline(limit, clock=outer.clock)
    t0 = outer.clock()
    took = []                                 # (phase, s): the measure, in the notes and in any SETUP-FAIL

    def timed(phase, fn):
        t = outer.clock()
        try:
            return fn()
        finally:
            took.append("%s %.1f s" % (phase, outer.clock() - t))

    phase = "warp"
    try:
        for x in ("A", "B"):                  # (Pair.warp_both's order and rules, one game at a time so each is timed)
            phase = "warp %s" % x
            s = timed(phase, lambda: pair.warp_one(x, stage, entrance=stage, arrive_in_event=True))
            assert (s.get("scene") or "").lower() == stage.lower(), "%s is in %s, not %s" % (x, s.get("scene"), stage)

        def both_in_play():
            sts = {x: pair.state(x) for x in ("A", "B")}
            return all(pair.in_play(v) and (v.get("scene") or "").lower() == stage.lower() for v in sts.values())
        phase = "both in play"
        timed(phase, lambda: pair.until("both games in play in %s after its arrival event" % stage, both_in_play,
                                        max_frames=IN_PLAY_FRAMES))
        if then:
            phase = "arrival check"
            then()                            # (a check of the arrival itself, e.g. the hearts still in)
    except Exception as e:                    # noqa: BLE001  (a timeout, a refused warp, a wrong scene, a SetupFail)
        why = str(e) if isinstance(e, P.SetupFail) else "%s: %s" % (type(e).__name__, e)
        raise P.SetupFail("boss arrival in %s: in '%s' after %.1f of %.0f s (%s): %s" % (
            stage, phase, outer.clock() - t0, limit, "; ".join(took) or "nothing done", why)) from e
    finally:
        pair.deadline = outer
    pair.notes.append("boss arrival in %s %.1f s (%s)" % (stage, outer.clock() - t0, "; ".join(took)))


def rest_from(pair, x, pattern):
    """Game x's log lines from the first one matching `pattern` (that line first) since the test began; [] if none."""
    lines = pair.log[x].since()
    rx = re.compile(pattern)
    for i, ln in enumerate(lines):
        if rx.search(ln):
            return lines[i:]
    return []


def has(lines, pattern):
    rx = re.compile(pattern)
    return any(rx.search(ln) for ln in lines)


@pytest.fixture
def back_after(pair):
    yield pair
    pair.deadline = P.Deadline(120)           # (the test's own deadline may be spent: the way back has its own)
    try:
        pair.warp_both(BACK_TO)
    except Exception as e:                    # noqa: BLE001  (a failed way back is said, never a result of this test)
        pair.notes.append("way back to %s failed: %s" % (BACK_TO, e))


def bleed_out_and_respawn(pair, stage):
    """The old row's steps after both are in the stage: A (host) sets the bleed-out to 10 s, B is downed, bleeds out,
    respawns; returns B's log lines from 'respawn at the entrance' (the row 289 path) on, and the load's end in them."""
    S = re.escape(stage)
    pair.act("A", "chat /dbno time 10")
    pair.expect_log("A", r"coop: test chat '/dbno time 10': sent", max_frames=300)
    pair.step_both(1800)                      # br289 a-in -> b-before: the stage's fight under way (A busy, :2)
    pair.act("B", "hurt 80")                  # br289-volvagia.B.txt: downed (16 quarters on the lane save)
    pair.expect_log("B", r"coop: dbno: our Link: event 0 ", max_frames=600)       # down
    pair.expect_log("B", r"coop: dbno: our Link: event 2 ", max_frames=1500)      # bled out (10 s + spectate)
    pair.expect_log("B", r"coop: dbno: our Link: event 7 ", max_frames=1500)      # respawned
    way = pair.until("B's respawn route line", lambda: pair.log["B"].find(
        r"coop: teleport: (respawn at the entrance %s of %s|next to \w+|move to )" % (S, S)), max_frames=300)
    if "respawn at the entrance" not in way:
        pair.unproven("B's respawn did not go to the entrance (A took the beside move: %r); the row 289 path "
                      "was not taken" % way[-160:])     # (NO-REF, never PASS: old row 'UNPROVEN ... not taken')
    pair.expect_log("B", r"asking the game's map change event Dm_SY_BancChangeGo for %s at %s" % (S, S), max_frames=300)
    pair.expect_log("B", r"live event Dm_SY_BancChangeGo asked of the game \(result 1\)", max_frames=300)
    pair.until("B's load after the respawn ended ('control: state loading -> play' after the ask)",
               lambda: has(rest_from(pair, "B", r"respawn at the entrance %s of %s" % (S, S)),
                           r"control: state loading -> play"), max_frames=1500)
    return rest_from(pair, "B", r"respawn at the entrance %s of %s" % (S, S))


@pytest.mark.budget(900)                      # the old scenario's '#! timeout 900'
@pytest.mark.look
@pytest.mark.needs_env(OBJECTS)
@pytest.mark.row("#289 respawn to the entrance of a boss stage shows the stage (boss-respawn-289 volvagia/dodongo/lizalfos/ganon)")
@pytest.mark.parametrize("stage", STAGES)
def test_respawn_shows_the_stage(pair, back_after, stage, request):
    if bt_plugin.env_has(bt_plugin.boot_env(request.config), KEEP):
        pytest.skip("this boot keeps the starter (%s): the fixed path is not on it" % KEEP)
    S = re.escape(stage)
    with pair.setup():
        start_check(pair)
    boss_arrival(pair, stage)
    rest = bleed_out_and_respawn(pair, stage)
    pair.step_both(1200)                      # br289 b-arrived -> b-later: the arrival event begun and over
    rest = rest_from(pair, "B", r"respawn at the entrance %s of %s" % (S, S))
    load = [i for i, ln in enumerate(rest) if "control: state loading -> play" in ln][0]
    assert has(rest, r"coop: teleport: %s has no entrance actor named %s: the arrival event's starter emptied" % (S, S)), \
        "fix: no '%s has no entrance actor named %s: the arrival event's starter emptied' line" % (stage, stage)
    assert has(rest[load:], r"coop: event: our Link's event began"), "fix: our Link's arrival event never began after the load"
    later = screen(pair, "B", "br289-%s-b-later" % stage)
    p0 = pair.state("B")["pos"]
    pair.press("B", stick=[0, 1], frames=60)  # 'hold LSTICK_UP 60' (STICK_KEYS LSTICK_UP = up: angle 0, full)
    pair.step_both(600)
    end = screen(pair, "B", "br289-%s-b-end" % stage)
    screen(pair, "A", "br289-%s-a-end" % stage)   # (A's screen, kept for the owner; not judged, as the old a-after)
    assert later > PICTURE and end > PICTURE, "fix: B's screen after the respawn is not the stage (mean b-later %.1f, " \
        "b-end %.1f; > %.0f)" % (later, end, PICTURE)
    p1 = pair.state("B")["pos"]
    assert p0 and p1, "B has no position after the arrival (%s, %s)" % (p0, p1)
    moved = math.dist(p0[::2], p1[::2])
    assert moved >= 0.5, "fix: B has no control after the arrival (%.2f m with the stick 1 s; >= 0.5)" % moved


@pytest.mark.budget(900)
@pytest.mark.look
@pytest.mark.needs_env(OBJECTS, KEEP)
@pytest.mark.row("#289 cause: the starter kept -> black after the respawn (boss-respawn-289 volvkeep)")
def test_respawn_black_with_starter_kept(pair, back_after):
    stage = "VolvagiaStage"
    S = re.escape(stage)
    with pair.setup():
        start_check(pair)
    boss_arrival(pair, stage)
    rest = bleed_out_and_respawn(pair, stage)
    pair.step_both(1200)
    rest = rest_from(pair, "B", r"respawn at the entrance %s of %s" % (S, S))
    load = [i for i, ln in enumerate(rest) if "control: state loading -> play" in ln][0]
    assert has(rest, r"coop: teleport: %s has no entrance actor named %s: the starter is kept \(%s\)" % (S, S, KEEP)), \
        "keep: no 'the starter is kept (%s)' line: the switch did not reach the module" % KEEP
    assert not has(rest, r"the arrival event's starter emptied"), "keep: a 'starter emptied' line with the starter kept"
    later = screen(pair, "B", "br289-keep-b-later")
    pair.press("B", stick=[0, 1], frames=60)
    pair.step_both(600)
    end = screen(pair, "B", "br289-keep-b-end")
    began = has(rest[load:], r"coop: event: our Link's event began")
    assert 0 <= later < BLACK and 0 <= end < BLACK and not began, "keep: not the old black screen (mean b-later %.1f, " \
        "b-end %.1f, < %.0f; arrival event began %s): the cause is not shown on these steps" % (later, end, BLACK, began)


def give_hearts(pair):
    """hs-volvagia-rocks: ten heart containers (the cheats window's give raises the max only: coop_cheat_give.cpp
    Wanted +4), then life filled to 56 quarters (16 + 40), so each Link lives through the fight; read back. Inside
    pair.setup(). Shared: the rows 234 tests (this file, test_volvagia_rocks.py) use this one, never a copy."""
    for x in ("A", "B"):
        for _ in range(10):
            pair.give(x, "PlayerStatus.MaxLife")
            pair.step_both(4)
        pair.flag(x, "PlayerStatus.Life", 56)
    pair.step_both(30)
    hearts_in(pair, "after ten gives")


def hearts_in(pair, when):
    for x in ("A", "B"):
        mx = pair.flag_read(x, "PlayerStatus.MaxLife").get("value")
        lf = pair.flag_read(x, "PlayerStatus.Life").get("value")
        assert isinstance(mx, int) and mx >= 56, "%s: MaxLife %r %s (want >= 56)" % (x, mx, when)
        assert lf == 56, "%s: Life %r %s (want 56: 'setflag PlayerStatus.Life 56')" % (x, lf, when)


@pytest.mark.budget(900)                      # hs-volvagia-rocks '#! timeout 600' + its 'await 10800' (6 min of game)
@pytest.mark.look
@pytest.mark.needs_env(OBJECTS)
@pytest.mark.row("#234 Volvagia's falling rocks in each game (hs-volvagia-rocks, boss-respawn-289 rocks234)")
def test_volvagia_rocks_each_game(pair, back_after):
    stage = "VolvagiaStage"
    with pair.setup():
        start_check(pair)
        give_hearts(pair)
    boss_arrival(pair, stage, then=lambda: hearts_in(pair, "after the arrival"))   # (the load must not undo them)
    for x in ("A", "B"):
        pair.log[x].mark()
    # hs-volvagia-rocks.{A,B}: lock on ('press ZL 10'), then twice 'hold ZL+LSTICK_UP 60', 'hold ZL+Y 8', 'wait 12',
    # 'hold ZL+Y 8' (the sword is Y) and the wait after (A 1500, B 800 + 700 before).
    for x in ("A", "B"):
        pair.press(x, ["ZL"], frames=10)
    for _ in range(2):
        for x in ("A", "B"):
            pair.press(x, ["ZL"], stick=[0, 1], frames=60)
            pair.press(x, ["ZL", "Y"], frames=8)
            pair.step_both(12)
            pair.press(x, ["ZL", "Y"], frames=8)
        pair.step_both(800)
    # 'await 10800 objects: rocks:' in each game: each game makes its own rocks (a FAIL: rocks only on one side)
    pair.expect_log("A", r"coop: objects: rocks: \d+ new falling rocks", max_frames=10800)
    pair.expect_log("B", r"coop: objects: rocks: \d+ new falling rocks", max_frames=10800)
    for x in ("A", "B"):                      # rocks at each screen's own Link (the old row's shot check)
        pair.check_shot(x, "volvagia-rocks-1", tolerance=0.80)   # 0.80: a fight scene, moving fire and rocks
    pair.step_both(6)
    for x in ("A", "B"):
        pair.check_shot(x, "volvagia-rocks-2", tolerance=0.80)
