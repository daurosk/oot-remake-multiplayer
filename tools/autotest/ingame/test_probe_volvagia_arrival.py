"""PROBE (not a row): why both Links stay in an event in VolvagiaStage after the arrival (B #174, give-life-fix-4).

volvagia-rocks-182cd53b-3 (lane 2, 09:11 UTC 2026-11-28) SETUP-FAIL on both rounds: boss_arrival 'both in play' not
after 2400 frames. What its game logs show (game-out-A.txt of run-182cd53b-3/lane2/session; B the same):
- the arrival itself ends ('the arrival at VolvagiaStage of VolvagiaStage is over: its starter was emptied 2 time(s)',
  after an 'event ended after 1.5 s'), then a SECOND event begins, 'our Link's event began: ... kind 0 (2 in the
  session); names seen: none' (:7070; '2 in the session' is the game's own running-event count, coop_objects.cpp
  EventLog / CoopEventMember::ReadRunning), with Npc_Navi, EventCamera, UISystemActor, EventRequestActor as members
  and DungeonBossVolvagia_Appear_Sign actors made right after it;
- it is 'still in its event after 30 ... 333 s' and ends only at 363.1 s (:9791), about when the test gave up and
  warped back;
- br289-6-keep-182cd53b-2 (LICHII_COOP_ARRIVAL_STARTER=keep, no hearts given) arrived in the same stage and had no
  second event (its game-out-A.txt :5406 on: one 'event ended after 1.7 s', then free play).
Open: does that event end by itself (and after how many stepped frames: integrate-0121's boss_arrival waits on an
event up to P.BOOT_EVENT_FRAMES), or does it wait for a press (a text box: Navi and the UI are members)?

This probe answers both in one run, on the row's own set-up (BR.start_check + BR.give_hearts inside pair.setup(), the
shared warp Pair.warp_one(arrive_in_event=True) A then B, as BR.boss_arrival):
1. steps both games LOOK frames at a time up to WATCH_FRAMES (the 2400 the run gave up at), a sample per game each
   LOOK and a bridge framebuffer shot of each game every SHOT_EVERY frames (BR.screen: kept with the result);
2. then, for each game still in its event, up to PRESSES bridge presses of A (Pair.press, 'A' as Pair.load_both's own
   input), PRESS_GAP frames apart, a shot after each, noting whether the event ended;
3. then steps on up to PROBE_FRAMES in all (a game whose event ends by itself shows its frame).
Runs only with --env BT_PROBE_VOLVAGIA_ARRIVAL=1 (needs_env; no rc pass carries it). Ends NO-REF (Pair.unproven),
never PASS; its product is <out>/probe-volvagia-arrival.json (samples, ends, presses, the module's event lines).
Reading it: an end only after a press = a waiting text box (the row's set-up then presses through it, cited here);
an end by itself = the frames for the event ceiling; no end at all = held by something else (the lines say what).
"""
import json
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pair as P                   # noqa: E402
import test_boss_respawn as BR     # noqa: E402  (shared set-up steps, never copied)
from test_boss_respawn import back_after   # noqa: E402,F401  (fixture: the way back gets its own deadline)

STAGE = "VolvagiaStage"
PROBE_ENV = ("BT_PROBE_VOLVAGIA_ARRIVAL=1", BR.OBJECTS)
WATCH_FRAMES = BR.IN_PLAY_FRAMES      # 2400: where volvagia-rocks-182cd53b-3 gave up
PROBE_FRAMES = P.BOOT_EVENT_FRAMES    # 6000 in all: integrate-0121's event ceiling for boss_arrival
LOOK = 60                             # frames between samples: an end is known to within LOOK
SHOT_EVERY = 600                      # frames between kept shots while watching (5 per game in WATCH_FRAMES)
PRESSES, PRESS_GAP = 4, 60            # A presses per game still in its event, and frames stepped after each
LINES = re.compile(r"coop: event: (our Link|the running)|coop: teleport: |coop: objects: anim \S+ \S+ '|"
                   r"coop: objects: \+ DungeonBossVolvagia|coop: free play")
STATE_KEYS = ("frame", "scene", "event", "loading", "not_ready", "age")
# 4800 frames above WATCH_FRAMES' run at ~7 a second (volvagia-rocks-182cd53b-3: 2400 in 343.8 s) + 2 warps (~19 s
# each, same run) + the set-up (P.SETUP_LIMIT_S) + 8 presses: 6000/7 + 40 + 30 + 60 = ~990 s; a ceiling, not a PASS.
BUDGET_S = 1200


def _keep(pair, x, st, ran):
    k = {key: st.get(key) for key in STATE_KEYS}
    k["stepped"] = ran
    k["play_ui"] = pair.play_ui(x)
    return k


def _sample(pair, rec, open_, ran, how):
    for x in ("A", "B"):
        st = pair.state(x)
        g = rec["games"][x]
        k = _keep(pair, x, st, ran)
        last = g["samples"][-1] if g["samples"] else None
        if last is None or ran % (5 * LOOK) == 0 or last.get("event") != k.get("event"):
            g["samples"].append(k)
        if x in open_ and pair.ready(x, st):
            g["ended"] = dict(k, how=how)
            open_.discard(x)


@pytest.mark.budget(BUDGET_S)
@pytest.mark.needs_env(*PROBE_ENV)
@pytest.mark.row("PROBE VolvagiaStage's second event after the arrival (B #174, volvagia-rocks-182cd53b-3; not a row)")
def test_probe_volvagia_arrival_event(pair, back_after):
    rec = {"watch_frames": WATCH_FRAMES, "probe_frames": PROBE_FRAMES, "look": LOOK,
           "games": {x: {"samples": [], "presses": []} for x in ("A", "B")}}
    pos = {}
    try:
        with pair.setup():
            BR.start_check(pair)
            BR.give_hearts(pair)
        for x in ("A", "B"):
            pos[x] = pair.log[x].mark()
            st = pair.warp_one(x, STAGE, entrance=STAGE, arrive_in_event=True)
            rec["games"][x]["after_own_warp"] = _keep(pair, x, st, 0)
            assert (st.get("scene") or "").lower() == STAGE.lower(), "%s is in %s, not %s" % (x, st.get("scene"), STAGE)
        ran, open_ = 0, {"A", "B"}
        while True:                           # 1. watch, as boss_arrival's 2400
            _sample(pair, rec, open_, ran, "by itself")
            if ran % SHOT_EVERY == 0:
                for x in ("A", "B"):
                    BR.screen(pair, x, "watch-%d" % ran)
            if not open_ or ran >= WATCH_FRAMES:
                break
            pair.step_both(LOOK)
            ran += LOOK
        for x in sorted(open_):               # 2. a waiting text box? A presses on each game still in its event
            for i in range(PRESSES):
                if x not in open_:
                    break
                r = pair.press(x, ["A"], frames=8)
                pair.step_both(PRESS_GAP)
                ran += 8 + PRESS_GAP
                v = BR.screen(pair, x, "press%d" % (i + 1))
                _sample(pair, rec, open_, ran, "after A press %d" % (i + 1))
                rec["games"][x]["presses"].append({"n": i + 1, "reply": r, "stepped": ran, "mean": v,
                                                   "ended": x not in open_})
        while open_ and ran < PROBE_FRAMES:   # 3. on to the event ceiling
            pair.step_both(LOOK)
            ran += LOOK
            _sample(pair, rec, open_, ran, "by itself after the presses")
        for x in ("A", "B"):
            BR.screen(pair, x, "probe-end")
        rec["stepped"] = ran
    except P.SetupFail as e:                  # (a set-up miss stays SETUP-FAIL, never a probe result; JSON kept)
        rec["error"] = "SetupFail: %s" % e
        raise
    except Exception as e:                   # noqa: BLE001 (recorded; the JSON is the product)
        rec["error"] = "%s: %s" % (type(e).__name__, e)
    finally:
        for x in ("A", "B"):
            rec["games"][x]["why"] = pair.why_now(x) + pair.event_note(x)
            rec["games"][x]["lines"] = [ln for ln in pair.log[x].since(pos.get(x, 0)) if LINES.search(ln)][-200:]
        with open(os.path.join(pair.out_dir or ".", "probe-volvagia-arrival.json"), "w", encoding="utf-8") as f:
            json.dump(rec, f, indent=1, default=str)
    out = []
    for x, g in sorted(rec["games"].items()):
        e = g.get("ended")
        if e:
            out.append("%s: event over %s after %d stepped frames (frame %s)" % (x, e["how"], e["stepped"], e.get("frame")))
        else:
            out.append("%s: still in its event after %s stepped frames and %d A presses (%s)" % (
                x, rec.get("stepped"), len(g["presses"]), g["why"]))
    pair.notes.extend(out)
    pair.unproven("probe only (probe-volvagia-arrival.json): %s%s" % (
        "; ".join(out), "; error " + rec["error"] if rec.get("error") else ""))
