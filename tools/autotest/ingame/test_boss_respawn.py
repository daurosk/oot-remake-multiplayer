"""Rows 289 and 234 in the bridge form (boss-respawn-289-4, B #130: port of coord-multiplayer offline
boss-respawn-289-run.ps1, preflight CLEAN review-c123-4 06:57 UTC 2026-11-26, never run: parked, then retired).

| Test | Old pass (boss-respawn-289-run.ps1) | Proves |
|---|---|---|
| test_respawn_shows_the_stage[<Stage>] | volvagia, dodongo, lizalfos, ganon | row 289 fixed: after a bleed-out in a boss stage the respawn "to the entrance" shows the stage and gives control back |
| test_respawn_black_with_starter_kept | volvkeep | the cause (report boss-respawn-289.md point 7): the same steps with the game's reservation kept (LICHII_COOP_ARRIVAL_STARTER=keep) stay BLACK |
| test_volvagia_rocks_each_game | rocks234 (hs-volvagia-rocks) | row 234: each game makes its own falling rocks in the Volvagia fight, both Links alive (ten heart gives) |
| test_respawn_in_a_boss_fight | (new, B #168 pick 3) | the same respawn in the real boss room (FireTemple room 33) in a live boss fight: B's beside move is refused ('not during your boss fight'), the entrance shows, control back |

Why A is put in an event (boss-respawn-289-7, br289-6-keep-182cd53b-2 NO-REF 05:05 UTC 2026-11-27): with A idle B's
respawn goes BESIDE A, never to the entrance. The entrance comes only when the beside move fails (coop_dbno.cpp:418-424,
Moved::failed or late): A answers busy while its game runs an event (coop_teleport.cpp:208 AnswerFor, local.event) or
cannot_warp in a boss fight (:213; CoopBoss::Fighting(), coop_boss_game.cpp:585 = the boss sync phase, set only in the 8
dungeons' own boss rooms, coop_boss.cpp kRooms, never in a <Boss>Stage). So in a <Boss>Stage A is put in the game's own
event the way a player is: right after B's bleed-out A warps into the stage again (the bridge warp with an entrance =
the game's map change event Dm_SY_BancChangeGo, its arrival event on landing: arrive_in_event, as boss_arrival), and B's
beside ask (>= kSpectateMinUs 3 s + kQuietUs 3 s after the bleed-out, coop_dbno.cpp:430, coop_dbno.h:217-219) meets it:
'teleport: <A> is busy (loading, a cutscene, a talk or in the air)' (coop_teleport.cpp:232) or no answer in
kRespawnWaitUs (coop_dbno.h:231, 'no answer from', :536), then 'dbno: respawn: not beside them: to the entrance'
(coop_dbno_game.cpp:557).

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
# boss-respawn-289-6 (brief: "check every boss room with a same-scene respawn for the same cause"): the other main boss
# rooms, each a coop_start_points.inc row {"<S>", "<S>", x, y, z, false} (no entrance actor: the row 289 cause, the fix's
# one code path; :4 Barinade, :5 BigOcto, :6 BongoBongo, :39 Ganondorf, :48 Gohma, :110 Morpha, :112 PhantomGanon,
# :121 Twinrova; the selftest checks each against the table). Their own test (test_respawn_shows_the_boss_room) so a
# job can run the old pass list and these apart.
BOSS_ROOMS = ["GohmaStage", "BigOctoStage", "BarinadeStage", "PhantomGanonStage", "MorphaStage", "BongoBongoStage",
              "TwinrovaStage", "GanondorfStage"]
# B #168 pick (3): the real boss room where CoopBoss::Fighting() works. kRooms row 4 (coop_boss.cpp): FireTemple, room 33,
# circle centre (-82, 0) radius 38, height 88-125, arrive (-64.5, 102.6, 0.0). StartPos_SageOfFire is NOT a
# coop_start_points.inc row (only {"FireTemple", "FireTemple", 0.05, 100.00, 24.68, true}, :34), so the bridge warp goes to
# the dungeon's own start and a 'place' puts Link in the room (tools/playtest/data/presets/before-volvagia.json: the same
# point, -64.51 102.58 -0.01). A place with no event: after kNoIntroUs (10 s, coop_boss.h:192) the room's phase is fight
# ('no intro: the fight has begun here', coop_boss.cpp Phase::waiting). The respawn's entrance there is NearestStart =
# the dungeon's only start 'FireTemple' (coop_teleport_game.cpp:471).
FIGHT_ROOM = "FireTemple"
FIGHT_AT = {"A": (-64.51, 102.58, -0.01), "B": (-64.51, 102.58, 1.49)}   # the preset's point; B kArrivalSpread 1.5 m off
# (written by the bridge's `flag` (Pair.flag, read back), never a bare 'act setflag': a wrong name must not pass silently,
# review-tests-153-6 05:4x, the Get_Epona trap)
FIGHT_FLAGS = [("0xeea87aec", "DefeatBoss"),                      # before-volvagia.json 'flags' (the boss alive: SageOfFire
               ("Volvagia_TemporaryDelete", 0),                   # quest back to DefeatBoss, the room's played flags off)
               ("HavePlayedEvent.SageOfFire_DefeatBoss", 0),
               ("HavePlayedEvent.Dm_FireTemple_BossOut", 0),
               ("HavePlayedEvent.Dm_FireTemple_BossIn", 0)]
AGE_FRAMES = 30         # Sequence_IsChildhood 0, then 30 frames before the warp (before-volvagia.json notes; playtest.md warp)
IN_ROOM = r"coop: boss: in the boss room of FireTemple: Enemy_DungeonBoss_Volvagia"   # coop_boss.cpp:469, logged :390
FIGHT_ON = r"coop: boss: (no intro: the fight has begun here|intro started \(Dm_FireTemple_BossIn\))"   # coop_boss.cpp
FIGHT_FRAMES = 1800     # kNoIntroUs 10 s = 600 frames at 60, x3 (a ceiling: a miss is SETUP-FAIL 'no boss fight')
FIGHT_REFUSED = r"coop: teleport: not during your boss fight"   # coop_teleport.cpp:425 (B's own trip, refused at once)
A_BUSY_REFUSED = r"coop: teleport: (\S.* is busy \(loading, a cutscene, a talk or in the air\)|no answer from )"   # :232/:536
AT_ENTRANCE_M = 10.0    # a BOUND (not a measure): B's place after the respawn within this of the start; the run notes it
START_POINTS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), "lichii", "hook",
                            "coop_start_points.inc")
BOTTLES = hookshot.BOTTLE_ITEMS  # nofairy's bottles (the scripts' list; checked == coop_testinput.cpp kBottleItems)
KEEP = "LICHII_COOP_ARRIVAL_STARTER=keep"     # coop_teleport_game.cpp:68-71
OBJECTS = "LICHII_COOP_DEBUG_OBJECTS=1"
BACK_TO = "KakarikoVillage_Child"            # test_smoke.WARP_TO: the lane's boot map, so later tests start in play
BLACK, PICTURE = 5.0, 15.0                    # mean.py thresholds (module docstring)
WARP_S = 31.0          # worst warp of one game on a lane: 26.9/28.8/31.0 s (party-hud-bridge-6947aa3d test_warp r1-r3, as P.recover)
ARRIVAL_EVENT_S = 48.0  # B's boss-stage arrival event, run 20261125-114351
IN_PLAY_FRAMES = 2400   # the old scenarios' 'wait 2400' after the load (br289-volvagia, hs-volvagia-rocks)
BOSS_ARRIVAL_S = 2 * (2 * (WARP_S + ARRIVAL_EVENT_S) + IN_PLAY_FRAMES / P.FPS)   # = 476 s, a CEILING (2x margin)
# A boss room's intro can outlast IN_PLAY_FRAMES stepped (pair.py BOOT_EVENT_FRAMES: Twinrova's intro, row215-309d7180):
# a game still IN AN EVENT at IN_PLAY_FRAMES is waited on up to P.BOOT_EVENT_FRAMES, the arrival bound grown once by those
# frames at P.BOOT_STEP_FPS (the boot proof's own rule, Pair._until_boot_play). A load or no event keeps the 2400.
EVENT_EXTRA_S = (P.BOOT_EVENT_FRAMES - IN_PLAY_FRAMES) / P.BOOT_STEP_FPS   # = 302.5 s
ROOM_BUDGET_S = int(900 + EVENT_EXTRA_S)      # the old '#! timeout 900' + that one growth = 1202 s
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
            return all(pair.ready(x, v) and (v.get("scene") or "").lower() == stage.lower() for x, v in sts.items())
        phase = "both in play"
        what = "both games in play in %s after its arrival event" % stage

        def in_play():
            try:
                return pair.until(what, both_in_play, max_frames=IN_PLAY_FRAMES)
            except P.TestTimeout:
                raise
            except AssertionError:
                in_event = [x for x in ("A", "B") if pair.state(x).get("event")]
                if not in_event:
                    raise
            # (once: the boot proof's growth, one deadline, one wait rule; capped by the test's own deadline as
            # Pair._until_boot_play's _boot_grow_cap, so the arrival never runs past the test's end)
            extra = max(0.0, min(EVENT_EXTRA_S, outer.end - pair.deadline.end))
            pair.deadline.end += extra
            pair.deadline.seconds += extra
            took.append("%s in an event at %d frames: waited on up to %d (+%.0f s%s)" % (
                "+".join(in_event), IN_PLAY_FRAMES, P.BOOT_EVENT_FRAMES, extra,
                ", capped by the test's deadline" if extra < EVENT_EXTRA_S else ""))
            return pair.until(what + " (a game in an event at %d frames: the event ceiling)" % IN_PLAY_FRAMES,
                              both_in_play, max_frames=P.BOOT_EVENT_FRAMES - IN_PLAY_FRAMES)
        timed(phase, in_play)
        if then:
            phase = "arrival check"
            then()                            # (a check of the arrival itself, e.g. the hearts still in)
    except Exception as e:                    # noqa: BLE001  (a timeout, a refused warp, a wrong scene, a SetupFail)
        why = str(e) if isinstance(e, P.SetupFail) else "%s: %s" % (type(e).__name__, e)
        raise P.SetupFail("boss arrival in %s: in '%s' after %.1f of %.0f s (%s): %s" % (
            stage, phase, outer.clock() - t0, pair.deadline.seconds, "; ".join(took) or "nothing done", why)) from e
    finally:
        pair.deadline = outer
    pair.notes.append("boss arrival in %s %.1f s (%s)" % (stage, outer.clock() - t0, "; ".join(took)))


def start_point(scene):
    """The module's start of `scene` at its own name: (x, y, z, has an entrance actor), read from coop_start_points.inc
    (the ONE table; never copied here)."""
    with open(START_POINTS, encoding="utf-8") as f:
        for s, e, x, y, z, actor in re.findall(
                r'\{"(\w+)", "(\w+)", (-?[\d.]+)f, (-?[\d.]+)f, (-?[\d.]+)f, (true|false)\}', f.read()):
            if s == scene and e == scene:
                return float(x), float(y), float(z), actor == "true"
    raise AssertionError("%s: no {\"%s\", \"%s\", ...} row in %s" % (scene, scene, scene, START_POINTS))


def a_in_event(pair, stage):
    """A put in the game's own event while B is spectating (module docstring 'Why A is put in an event'): A warps into
    `stage` again (the map change event, its arrival event on landing). A not in an event after it is SETUP-FAIL: the
    row's entrance path would not be certain."""
    try:
        st = pair.warp_one("A", stage, entrance=stage, arrive_in_event=True)
    except Exception as e:                    # noqa: BLE001  (a refused warp, a timeout)
        raise P.SetupFail("A in an event for B's respawn: A's warp into %s again failed: %s: %s"
                          % (stage, type(e).__name__, e)) from e
    if not st.get("event"):
        raise P.SetupFail("A in an event for B's respawn: A is not in an event after its warp into %s again (%s)"
                          % (stage, pair.why_not("A", st)))
    pair.notes.append("A in its arrival event in %s for B's beside ask" % stage)


def boss_room_fight(pair):
    """Both games adult in FireTemple room 33 in a live boss fight (B #168 pick 3): the boss made alive
    (FIGHT_FLAGS), adult (Sequence_IsChildhood 0), the bridge warp to the dungeon's start, placed in the room, then
    each game's own 'in the boss room' and fight lines. Bounded (BOSS_ARRIVAL_S); a miss is SETUP-FAIL
    'boss room fight in FireTemple: ...' ('no boss fight' when the room's phase never became the fight)."""
    outer = pair.deadline
    pair.deadline = P.Deadline(max(0.0, min(BOSS_ARRIVAL_S, outer.left())), clock=outer.clock)
    phase = "flags"
    try:
        marks = {x: pair.log[x].size() for x in ("A", "B")}   # (size, not mark: the test's own base stays)
        for x in ("A", "B"):
            for name, value in FIGHT_FLAGS:
                pair.flag(x, name, value)     # (a refused name raises; a bridge error is this phase's SETUP-FAIL)
                got = pair.flag_read(x, name).get("value")    # (read back: an unknown name reads no value)
                if str(got).lower() != str(value).lower() and not (value == 0 and got is False):
                    raise P.SetupFail("%s: flag %s reads %r after writing %r (unknown name or not kept)"
                                      % (x, name, got, value))
        phase = "adult"
        for x in ("A", "B"):
            pair.flag(x, "Sequence_IsChildhood", 0)   # (the bridge's flag write, read back: did_not_stick = SETUP-FAIL)
        pair.step_both(AGE_FRAMES)
        for x in ("A", "B"):
            phase = "warp %s" % x
            s = pair.warp_one(x, FIGHT_ROOM, entrance=FIGHT_ROOM)
            assert (s.get("scene") or "").lower() == FIGHT_ROOM.lower(), "%s is in %s" % (x, s.get("scene"))
            assert s.get("age") == "adult", "%s is %s, not adult (Sequence_IsChildhood 0)" % (x, s.get("age"))
        phase = "place"
        for x in ("A", "B"):
            pair.act(x, "place %.2f %.2f %.2f" % FIGHT_AT[x])
        for x in ("A", "B"):
            phase = "in the boss room %s" % x
            pair.until("%s: '%s'" % (x, IN_ROOM), lambda x=x: pair.log[x].find(IN_ROOM, pos=marks[x]),
                       max_frames=FIGHT_FRAMES)
        for x in ("A", "B"):
            phase = "no boss fight %s" % x
            pair.until("%s: the room's fight line" % x, lambda x=x: pair.log[x].find(FIGHT_ON, pos=marks[x]),
                       max_frames=FIGHT_FRAMES)
        phase = "both in play"
        pair.until("both games in play in the boss room", lambda: all(pair.ready(x) for x in ("A", "B")),
                   max_frames=FIGHT_FRAMES)
    except Exception as e:                    # noqa: BLE001
        why = str(e) if isinstance(e, P.SetupFail) else "%s: %s" % (type(e).__name__, e)
        raise P.SetupFail("boss room fight in %s: in '%s': %s" % (FIGHT_ROOM, phase, why)) from e
    finally:
        pair.deadline = outer
    pair.notes.append("boss room fight in %s: %s" % (FIGHT_ROOM, "; ".join(
        "%s %r" % (x, pair.log[x].find(FIGHT_ON, pos=marks[x])[-90:]) for x in ("A", "B"))))


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


A_DOWN = r"coop: dbno: our Link: event 0 "   # (coop_dbno_game.cpp:523 Event 0 = down; the same line B's down is read by)


def fight_result(pair, stage, step, e):
    """review-tests-153 MUST 1: A stands idle in a live boss fight for the whole row (no one keeps it up). If A went
    down too (a wipe: no one is left to respawn beside), B's bleed-out/respawn lines can miss for a FIGHT result, not
    for row 289: that is NO-REF naming it (never FAIL, never PASS). A not down: the miss is the row's own FAIL."""
    if pair.log["A"].find(A_DOWN):
        pair.unproven("A was downed in %s's fight too (%r) before B's %s: the row 289 path is not judged on a "
                      "fight result (%s)" % (stage, pair.log["A"].find(A_DOWN)[-120:], step, e))
    raise e


def bleed_out_and_respawn(pair, stage, a_busy=True):
    """The old row's steps after both are in the stage: A (host) sets the bleed-out to 10 s, B is downed, bleeds out,
    respawns; returns B's log lines from 'respawn at the entrance' (the row 289 path) on, and the load's end in them.
    a_busy (a <Boss>Stage): right after B's bleed-out A is put in the game's own event (a_in_event), so B's beside ask
    is refused (module docstring); False in the boss room, where the fight refuses it (boss_room_fight). The respawn's
    entrance is the stage's own start (NearestStart: each stage and FireTemple have one row of their own name).
    A miss of B's lines while A is down too is NO-REF (fight_result), not the row's FAIL."""
    S = re.escape(stage)
    pair.act("A", "chat /dbno time 10")
    pair.expect_log("A", r"coop: test chat '/dbno time 10': sent", max_frames=300)
    pair.step_both(1800)                      # br289 a-in -> b-before: the stage's fight under way
    pair.act("B", "hurt 80")                  # br289-volvagia.B.txt: downed (16 quarters on the lane save)
    for step, pat, frames in (("down", r"coop: dbno: our Link: event 0 ", 600),          # down
                              ("bleed-out", r"coop: dbno: our Link: event 2 ", 1500),    # bled out (10 s + spectate)
                              ("respawn", r"coop: dbno: our Link: event 7 ", 1500)):     # respawned
        try:
            pair.expect_log("B", pat, max_frames=frames)
        except AssertionError as e:           # (TestTimeout too)
            fight_result(pair, stage, step, e)
        if step == "bleed-out" and a_busy:
            a_in_event(pair, stage)           # (before B's beside ask: >= 6 s after the bleed-out; B held meanwhile)
    try:
        way = pair.until("B's respawn route line", lambda: pair.log["B"].find(
            r"coop: teleport: (respawn at the entrance %s of %s|next to \w+|move to )" % (S, S)), max_frames=300)
    except AssertionError as e:
        fight_result(pair, stage, "respawn route", e)
    if "respawn at the entrance" not in way:
        pair.unproven("B's respawn did not go to the entrance (A took the beside move: %r); the row 289 path "
                      "was not taken" % way[-160:])     # (NO-REF, never PASS: old row 'UNPROVEN ... not taken')
    refused = pair.log["B"].find(A_BUSY_REFUSED if a_busy else FIGHT_REFUSED)
    pair.notes.append("B's beside move refused: %r" % (refused[-120:] if refused else None))
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
    respawn_shows(pair, stage, request)


@pytest.mark.budget(ROOM_BUDGET_S)
@pytest.mark.look
@pytest.mark.needs_env(OBJECTS)
@pytest.mark.row("#289 every main boss room: the respawn to its entrance shows the room (same cause, boss-respawn-289-6)")
@pytest.mark.parametrize("stage", BOSS_ROOMS)
def test_respawn_shows_the_boss_room(pair, back_after, stage, request):
    respawn_shows(pair, stage, request)


def respawn_shows(pair, stage, request):
    """The fixed path in one stage (row 289): bleed-out, respawn to the entrance, the stage shown, control back."""
    if bt_plugin.env_has(bt_plugin.boot_env(request.config), KEEP):
        pytest.skip("this boot keeps the starter (%s): the fixed path is not on it" % KEEP)
    with pair.setup():
        start_check(pair)
    boss_arrival(pair, stage)
    bleed_out_and_respawn(pair, stage)
    respawn_judged(pair, stage)


def respawn_judged(pair, stage):
    """After the respawn's load: the fix's lines (a stage with no entrance actor: the row 289 cause), B's screen the
    stage (no black), B in play there near its start (start_point, within AT_ENTRANCE_M) and the stick moving B."""
    S = re.escape(stage)
    pair.step_both(1200)                      # br289 b-arrived -> b-later: the arrival event begun and over
    rest = rest_from(pair, "B", r"respawn at the entrance %s of %s" % (S, S))
    load = [i for i, ln in enumerate(rest) if "control: state loading -> play" in ln][0]
    sx, sy, sz, actor = start_point(stage)
    if not actor:
        assert has(rest, r"coop: teleport: %s has no entrance actor named %s: the arrival event's starter emptied"
                   % (S, S)), "fix: no '%s has no entrance actor named %s: the arrival event's starter emptied' line" % (
                       stage, stage)
        assert has(rest[load:], r"coop: event: our Link's event began"), \
            "fix: our Link's arrival event never began after the load"
    later = screen(pair, "B", "br289-%s-b-later" % stage)
    st = pair.state("B")
    p0 = st.get("pos")
    assert pair.ready("B", st) and (st.get("scene") or "").lower() == stage.lower(), \
        "fix: B is not in play in %s after the respawn (%s)" % (stage, pair.why_not("B", st))
    assert p0, "B has no position after the arrival (%s)" % p0
    off = math.dist((p0[0], p0[2]), (sx, sz))
    pair.notes.append("B %.1f m from the start %s of %s after the respawn" % (off, stage, stage))
    assert off <= AT_ENTRANCE_M, "fix: B is %.1f m from the entrance %s (%.2f %.2f %.2f) after the respawn (<= %.0f)" % (
        off, stage, sx, sy, sz, AT_ENTRANCE_M)
    pair.press("B", stick=[0, 1], frames=60)  # 'hold LSTICK_UP 60' (STICK_KEYS LSTICK_UP = up: angle 0, full)
    pair.step_both(600)
    end = screen(pair, "B", "br289-%s-b-end" % stage)
    screen(pair, "A", "br289-%s-a-end" % stage)   # (A's screen, kept for the owner; not judged, as the old a-after)
    assert later > PICTURE and end > PICTURE, "fix: B's screen after the respawn is not the stage (mean b-later %.1f, " \
        "b-end %.1f; > %.0f)" % (later, end, PICTURE)
    p1 = pair.state("B").get("pos")
    assert p1, "B has no position after the stick (%s)" % p1
    moved = math.dist(p0[::2], p1[::2])
    assert moved >= 0.5, "fix: B has no control after the arrival (%.2f m with the stick 1 s; >= 0.5)" % moved


@pytest.mark.budget(900)
@pytest.mark.look
@pytest.mark.needs_env(OBJECTS)
@pytest.mark.row("#289 in a live boss fight (FireTemple room 33): beside refused, the respawn to the entrance shows (B #168)")
def test_respawn_in_a_boss_fight(pair, back_after, request):
    if bt_plugin.env_has(bt_plugin.boot_env(request.config), KEEP):
        pytest.skip("this boot keeps the starter (%s): the fixed path is not on it" % KEEP)
    with pair.setup():
        start_check(pair)
    boss_room_fight(pair)
    bleed_out_and_respawn(pair, FIGHT_ROOM, a_busy=False)
    assert pair.log["B"].find(FIGHT_REFUSED), "fight: B's beside move was not refused by the boss fight (%r)" % (
        FIGHT_REFUSED)
    respawn_judged(pair, FIGHT_ROOM)


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
