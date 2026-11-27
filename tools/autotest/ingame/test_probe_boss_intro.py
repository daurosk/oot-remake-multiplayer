"""PROBE (not a row): does Twinrova's intro event end under the bridge's held + stepped frames, and after how many?
(row-215-confirm-2, B #149: row215-309d7180 SETUP-FAIL 'lane1 boot: both games in play after the boot: not after 2400
frames; A and B: scene=TwinrovaStage loading=False event=True'.)

What the logs already show (reports/row-215-confirm.md, Update 03:34 + row-215-confirm-2): the renderer's present
counter ('texture frame N' / 'sampled again at frame N', Longshot's vk lines in game-out-X.txt) puts Kotake's cuts at
the same frame after the arrival in the stepped boot as in the free-running .ps1 boot (C07: +3960 stepped, +3993
free); the free-running intro ended between +3994 and +5048 ('event ended after 76.1 s'), the stepped boot proof gave
up at about +4210. So the stepped intro looked on its way, cut off by the frame ceiling (cause a), not stopped by
something stepping does not advance (cause b). That is read off two runs; this probe measures it in one.

Runs only with --env BT_PROBE_BOSS_INTRO=1 (needs_env: every other run deselects it, so no rc pass carries it), on the
DEFAULT boot (a live-twinrova-room boot cannot pass the boot proof, which is what this probes). Ends NO-REF
(Pair.unproven), never PASS: its product is <out>/probe-boss-intro.json:
- per game: the state's frame/scene/event before the warp and after it (Pair.warp_one, arrive_in_event=True, the
  shared warp the boot proof and test_row215 use), then a sample every LOOK frames (both games stepped in turn, CHUNK
  at a time: Pair.step_both, the boot proof's own stepping) until both games are out of the event or PROBE_FRAMES ran;
- each game's 'ended' frame: the first sample with event False after the arrival, and the frames stepped to it;
- the module's event and boss-cut lines since the warp (event began / still in its event / ended, 'AS-C..' anims).
Reading it: an 'ended' for both games = cause (a), and the frames give Pair.BOOT_EVENT_FRAMES its measured value;
no 'ended' in PROBE_FRAMES (about twice the free run's +5048) = cause (b).
"""
import json
import os
import re

import pytest

import pair as P
from test_boss_wake import STAGE, SWITCHES

PROBE_ENV = ("BT_PROBE_BOSS_INTRO=1",) + SWITCHES
PROBE_FRAMES = 9000   # frames stepped after the warps: ~1.8 x the free run's latest possible end (+5048, the docstring)
LOOK = 60             # frames between samples: the 'ended' frame is known to within LOOK
NOTE_EVERY = 300      # frames between samples kept in the JSON (the transitions are always kept)
LINES = re.compile(r"coop: event: our Link|coop: objects: anim \S+ \S+ 'AS-|coop: boss: (phase|the game asked)")
STATE_KEYS = ("frame", "scene", "event", "loading", "not_ready", "age")


def _keep(st):
    return {k: st.get(k) for k in STATE_KEYS}


@pytest.mark.budget(1200)       # 9000 frames at the boot proof's measured ~12 a second (row215-309d7180: 2400 in ~200 s) + 2 warps
@pytest.mark.needs_env(*PROBE_ENV)
@pytest.mark.row("PROBE Twinrova's intro under stepping (B #149, row215-309d7180 boot SETUP-FAIL; not a row)")
def test_probe_twinrova_intro_frames(pair):
    rec = {"probe_frames": PROBE_FRAMES, "look": LOOK, "games": {x: {"samples": []} for x in pair.c}}
    pos = {}
    try:
        with pair.setup():
            pair.need_boot_place()
            for x in pair.c:
                rec["games"][x]["before_warp"] = _keep(pair.state(x))
            for x in [y for y in ("A", "B") if y in pair.c]:
                pos[x] = pair.log[x].mark()
                st = pair.warp_one(x, STAGE, entrance=STAGE, arrive_in_event=True)
                rec["games"][x]["after_own_warp"] = _keep(st)
        for x in pair.c:
            rec["games"][x]["after_warps"] = _keep(pair.state(x))
        ran, open_ = 0, set(pair.c)
        while True:
            for x in pair.c:
                g, st = rec["games"][x], _keep(pair.state(x))
                st["stepped"] = ran
                last = g["samples"][-1] if g["samples"] else None
                if last is None or ran % NOTE_EVERY == 0 or last.get("event") != st.get("event"):
                    g["samples"].append(st)
                if x in open_ and not st.get("event") and not st.get("loading"):
                    g["ended"] = st
                    open_.discard(x)
            if not open_ or ran >= PROBE_FRAMES:
                break
            pair.step_both(LOOK)
            ran += LOOK
        rec["stepped"] = ran
    except P.SetupFail as e:                  # (a set-up miss stays SETUP-FAIL, never a probe result; JSON kept)
        rec["error"] = "SetupFail: %s" % e
        raise
    except Exception as e:                   # noqa: BLE001 (recorded; the JSON is the product)
        rec["error"] = "%s: %s" % (type(e).__name__, e)
    finally:
        for x in pair.c:
            rec["games"][x]["lines"] = [ln for ln in pair.log[x].since(pos.get(x, 0)) if LINES.search(ln)][-200:]
        with open(os.path.join(pair.out_dir or ".", "probe-boss-intro.json"), "w", encoding="utf-8") as f:
            json.dump(rec, f, indent=1)
    out = []
    for x, g in sorted(rec["games"].items()):
        e, a = g.get("ended"), g.get("after_own_warp") or {}
        if e:
            out.append("%s: event over after %d stepped frames (frame %s; its warp returned at frame %s)"
                       % (x, e["stepped"], e.get("frame"), a.get("frame")))
        else:
            out.append("%s: still in the event after %s stepped frames" % (x, rec.get("stepped")))
    pair.notes.extend(out)
    pair.unproven("probe only (probe-boss-intro.json): %s%s" % ("; ".join(out), "; error " + rec["error"]
                                                               if rec.get("error") else ""))
