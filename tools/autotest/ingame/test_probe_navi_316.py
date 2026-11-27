"""PROBE (row 316, not a judged row): what our own Navi (Npc_Navi) and the co-op fairies do, read from the bridge
state's `navi` member (lichii/hook/coop_navi_probe.h; docs/playtest.md "State" `navi`). Answers, for the Navi sync
design (coord-multiplayer/reports/navi-sync-316.md "Probes needed"):
- P2: which AS slot holds the material command (`Status_Red`, AS/Npc_Navi.root.asb) - our Link at one heart;
- P3: what her present state (+0x2c) does across a load (a warp) - the hidden/event signal;
- the slot names while locked on (ZL held: Longshot's lock-on button, docs/playtest.md `fight`, host/nn_hid.c), and
  each live fairy's actor/place (the receiver side).

Runs only with --env BT_PROBE_NAVI=1 (needs_env: every other run deselects it, so no rc pass carries it). Ends NO-REF
(Pair.unproven), never PASS: its product is <out>/probe-navi-<X>.json + the bridge shots. Reads only, except the set-up
writes it restores: PlayerStatus.Life (give/flag need LICHII_COOP_CHEATS=1, test_songs.ROW_ENV) back to what it read.
Per game (A then B; each game's JSON is written in a finally with any exception's text; one game's exception never
skips the other's phases):
- idle: 12 samples 10 frames apart (the slots' names and frames move with her Wait animation);
- low life: PlayerStatus.Life 4 (quarter hearts: one heart; fakegame.py LINK_LIFE), 12 samples, Life put back;
- lock-on: the other game's Link placed 2.5 m in front (test_rows.place_in_front), ZL held 60 frames, 6 samples
  during (each `input` returns after its frames: the samples are taken between 10-frame holds of ZL);
- load: a warp to the boot's own place (test_gear SCENE_CHILD / GATE, the lane's way back), samples right after.
A shot on each game in every phase (Pair.check_shot, no reference: probe).
"""
import json
import os

import pytest

from pair import SetupFail
from test_gear import GATE, SCENE_CHILD
from test_rows import need_child_in_play, place_in_front
from test_songs import ROW_ENV

PROBE_ENV = ("BT_PROBE_NAVI=1",) + ROW_ENV
LIFE = "PlayerStatus.Life"           # quarter hearts (fakegame.py LINK_LIFE; docs/items.md)
LOW_LIFE = 4                         # one heart
SAMPLES, EVERY = 12, 10


def _navi(pair, x):
    st = pair.state(x)
    return {"frame": st.get("frame"), "scene": st.get("scene"), "loading": st.get("loading"), "event": st.get("event"),
            "navi": st.get("navi")}


def _samples(pair, x, n=SAMPLES, every=EVERY, hold=None):
    out = []
    for _ in range(n):
        if hold:
            pair.press(x, hold, frames=every)
        else:
            pair.step_both(every)
        out.append(_navi(pair, x))
    return out


def _names(samples):
    """Every slot name seen, per slot index (for the notes line)."""
    seen = {}
    for s in samples:
        navi = (s.get("navi") or {}).get("navi") or {}
        for i, slot in enumerate(navi.get("slots") or []):
            if slot.get("name"):
                seen.setdefault(i, set()).add(slot["name"])
    return {i: sorted(v) for i, v in sorted(seen.items())}


def _phase(rec, name, fn):
    try:
        rec["phases"][name] = fn()
    except Exception as e:                    # noqa: BLE001 (recorded; the next phase still runs)
        rec["phases"][name] = {"error": "%s: %s" % (type(e).__name__, e)}


@pytest.mark.budget(900)
@pytest.mark.needs_env(*PROBE_ENV)
@pytest.mark.row("PROBE row 316 Navi: AS slots, present state, fairies (navi-sync-316-2; not a row)")
def test_probe_navi_316(pair):
    with pair.setup():
        need_child_in_play(pair)
        for x in ("A", "B"):
            pair.state(x)                     # (the first `state` turns the module's Navi reads on: Wanted)
        pair.step_both(30)
        first = {x: pair.state(x).get("navi") for x in ("A", "B")}
        if any(v is None for v in first.values()):
            raise SetupFail("navi state member null after 30 frames: %s (module without coop_navi_probe, or fairies "
                            "off: LICHII_COOP_PUPPET)" % {x: v is None for x, v in first.items()})
    for x in ("A", "B"):
        other = "B" if x == "A" else "A"
        rec = {"game": x, "phases": {}}
        try:
            _phase(rec, "idle", lambda: _samples(pair, x))
            pair.check_shot(x, "probe-navi-idle", tolerance=0.0)

            def low():
                was = pair.flag_read(x, LIFE)
                rec["life_was"] = was
                pair.flag(x, LIFE, LOW_LIFE)
                try:
                    got = _samples(pair, x)
                    pair.check_shot(x, "probe-navi-low", tolerance=0.0)
                    return got
                finally:
                    value = was.get("value") if isinstance(was, dict) else was
                    if isinstance(value, int) and value > 0:
                        pair.flag(x, LIFE, value)
            _phase(rec, "low_life", low)

            def lock():
                place_in_front(pair, other, x)
                pair.step_both(20)
                got = _samples(pair, x, n=6, hold=["ZL"])
                pair.check_shot(x, "probe-navi-lock", tolerance=0.0)
                pair.step_both(30)
                return got
            _phase(rec, "lock_on", lock)

            def load():
                pair.warp_one(x, SCENE_CHILD, GATE)
                got = [_navi(pair, x)] + _samples(pair, x, n=6)
                pair.check_shot(x, "probe-navi-load", tolerance=0.0)
                return got
            _phase(rec, "load", load)
        except Exception as e:                # noqa: BLE001 (recorded; the other game is still probed)
            rec["error"] = "%s: %s" % (type(e).__name__, e)
        finally:
            with open(os.path.join(pair.out_dir or ".", "probe-navi-%s.json" % x), "w", encoding="utf-8") as f:
                json.dump(rec, f, indent=1)
            pair.notes.append("%s: slot names %s; error %s" % (
                x, {k: _names(v) for k, v in rec["phases"].items() if isinstance(v, list)}, rec.get("error")))
    pair.unproven("probe only: see probe-navi-A.json / -B.json and the probe-navi-* shots")
