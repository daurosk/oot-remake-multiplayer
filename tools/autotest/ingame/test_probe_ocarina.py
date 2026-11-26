"""PROBE (not a row): what D-pad Left does on each game of the lane, B #140 17:3x (songs-frogs-733c4d8f and -58f13b22
SETUP-FAIL: B pressed LEFT, no 'ocarina probe: our Link plays the ocarina' line, also after Pair.need_ocarina).

Runs only with --env BT_PROBE_OCARINA=1 (needs_env: every other run deselects it, so no rc pass carries it). Ends NO-REF
(Pair.unproven), never PASS: its product is <out>/probe-ocarina-<X>.json + the bridge shots, for the root cause.
Per game (A first, then B, the other game stepped alongside):
- before: the bridge state's scene/age/event/loading/equips, PlayerStatus.EquipOcarina (flag_read; docs/items.md:24,
  the name D-pad Left takes out, docs/items.md:320), the module's 'equipped by the game from its flags' lines since the
  last 'native ui: scene load ended' (Pair.ocarina_ready) and Pair.need_ocarina's outcome; a shot.
- the press: the exact `input` request sent (buttons ["LEFT"], frames 12: Longshot host/control.c:606 LEFT = pad bit
  12, D-pad Left; docs/playtest.md:118) and its reply's from/frame/state.
- after: shots at +30 and +240 frames, the state, and every module line since the press that names the ocarina, a
  probe, an event, an item or a note.
- if nothing came out: once more with LEFT held 30 frames (a press read on a later frame than the first), same records.
Then B (8 frames) puts away whatever came out.
"""
import json
import os
import re

import pytest

from test_songs import OCARINA_OUT, ROW_ENV

PROBE_ENV = ("BT_PROBE_OCARINA=1",) + ROW_ENV
WATCH = re.compile(r"ocarina|Ocarina|probe|event|items:|note|AS slot", re.I)
STATE_KEYS = ("scene", "age", "event", "loading", "not_ready", "equips", "pos", "frame", "prompt")


def _state(pair, x):
    st = pair.state(x)
    return {k: st.get(k) for k in STATE_KEYS}


def _equip_flag(pair, x):
    try:
        return pair.flag_read(x, "PlayerStatus.EquipOcarina")
    except Exception as e:                    # noqa: BLE001 (recorded, never judged)
        return {"error": "%s: %s" % (type(e).__name__, e)}


def _press_and_watch(pair, x, frames, tag, rec):
    pos = pair.log[x].mark()
    sent = {"cmd": "input", "buttons": ["LEFT"], "frames": frames}
    r = pair.press(x, ["LEFT"], frames=frames)
    rec["presses"].append({"tag": tag, "sent": sent, "reply_from": r.get("from"), "reply_frame": r.get("frame"),
                           "reply_state": {k: (r.get("state") or {}).get(k) for k in STATE_KEYS}})
    pair.step_both(30)
    pair.check_shot(x, "probe-oca-%s-30" % tag, tolerance=0.0)
    pair.step_both(210)
    pair.check_shot(x, "probe-oca-%s-240" % tag, tolerance=0.0)
    lines = [ln for ln in pair.log[x].since(pos) if WATCH.search(ln)]
    rec["presses"][-1].update(state_240=_state(pair, x), lines=lines[:200],
                              probe_line=bool(pair.log[x].find(OCARINA_OUT, pos=pos)))
    return rec["presses"][-1]["probe_line"]


@pytest.mark.budget(600)
@pytest.mark.needs_env(*PROBE_ENV)
@pytest.mark.row("PROBE ocarina D-pad Left on A and B (B #140 17:3x; not a row)")
def test_probe_ocarina_left(pair):
    with pair.setup():
        pair.until("both games in play", lambda: all(pair.in_play(pair.state(x)) for x in ("A", "B")), max_frames=600)
    out = {}
    for x in ("A", "B"):
        rec = {"game": x, "before": _state(pair, x), "equip_flag": _equip_flag(pair, x),
               "ocarina_ready": pair.ocarina_ready(x),
               "equip_lines": [ln for ln in pair.log[x].since(0) if "equipped by the game from its flags" in ln][-5:],
               "presses": []}
        try:
            pair.need_ocarina(x)
            rec["need_ocarina"] = "ok"
        except AssertionError as e:
            rec["need_ocarina"] = str(e)
        rec["equip_flag_after_wait"] = _equip_flag(pair, x)
        pair.check_shot(x, "probe-oca-before", tolerance=0.0)
        if not _press_and_watch(pair, x, 12, "12f", rec):
            _press_and_watch(pair, x, 30, "30f", rec)
        pair.press(x, ["B"], frames=8)
        pair.step_both(60)
        out[x] = rec
        with open(os.path.join(pair.out_dir or ".", "probe-ocarina-%s.json" % x), "w", encoding="utf-8") as f:
            json.dump(rec, f, indent=1)
        pair.notes.append("%s: need_ocarina %s; probe line after LEFT: %s" % (
            x, rec["need_ocarina"], [p["probe_line"] for p in rec["presses"]]))
    pair.unproven("probe only: see probe-ocarina-A.json / -B.json and the probe-oca-* shots")
