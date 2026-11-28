"""PROBE (not a row; test_probe_pg_falldown_287.py): what starts Phantom Ganon's fall-down (PhantomGanon.actioninterruptfalldown, CCT move state 1) in
game? (row 287, phantom-ganon-287-5, B #165 GO after pganon-287-182cd53b.)

Why: the row's run pganon-287-182cd53b (reports/phantom-ganon-287-4.md RESULT) was NOT PROVEN: the bridge's
`verb PG 139` did not start the move (game-out-A.txt:13306-13307 'action stream: ours asks slot 0 verb 139 (changed 1)',
the next frame the AI writes 40 back; 0 'attack starts (action verbs 139' lines), the move probe showed his CCT move
state 2 the whole run, and `hit PG 6` x5 (life 42 -> 12, a straight Life write: coop_objects.cpp:5810-5822) changed
neither his anim nor his move state. The player's log had 139 as 'attack starts (action verbs 139 0 0 0 0, attributes
0x6)' started by the GAME's AI, after a run of 112 attacks, with him in 'Fly'. Which game event starts it is not known.

Runs only with --env BT_PROBE_PG_FALLDOWN=1 (needs_env: every other run deselects it) on the boot live-phantom-ganon
(its '#! switches' turn on LICHII_COOP_MOVE_PROBE, live-phantom-ganon.A.txt:8). One boot, the candidates in order,
each in its own log window:
- c1 `verb PG 139` asked of both games (only the owner's runs it), 60 frames;
- c2 the game's own damage while he rides: the owner's Link placed 1.5 m from him (`place`, `face`:
  lichii/hook/coop_testinput.h:38-47) and a sword swing (Y with the lock-on ZL held: tools/autotest/kit_fight.py:7-9,
  26-29, Longshot host/nn_hid.c), 30 frames after each, up to SWORD_ROUNDS swings or his life <= STOP_LIFE (never 0);
  then IDLE_FRAMES with no input (his AI goes on, e.g. to Fly);
- c3 only once an anim with 'Fly' in its name was seen: `verb PG 139`, `hit PG 1`, then FLY_IDLE_FRAMES with no input.
A candidate STARTS the fall-down when its window has a move probe line '... move state N (..) -> 1 (type' for him
(coop_move_probe.cpp Watch::Step) or an 'attack starts (action verbs 139' line (coop_objects.cpp:388). Ends NO-REF
(Pair.unproven), never PASS: the product is <out>/probe-pg-falldown.json (every window's PG lines, his anims and
verbs seen, life per swing) and the summary line naming the first starting candidate with its quoted lines, or
'UNPROVEN' with every anim and verb seen. No shots (nothing new a player sees).
"""
import json
import os
import re
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pair as P                                           # noqa: E402
from test_bosses import PG, SCENE, ASKED_OURS, boss, dead  # noqa: E402  (the row's own names and readers)

PROBE_ENV = ("BT_PROBE_PG_FALLDOWN=1",)
SWORD_ROUNDS = 40       # swings in c2 (the row's 5 hit turns x 8; each 4 + 8 + 30 frames)
STOP_LIFE = 6           # no swing once his life is this low (a probe must not end the fight: c3 needs him alive)
SWING = ("ZL", "Y")     # kit_fight.py:26-29 (ZL lock-on held through the slash, Y the sword)
SWING_FRAMES = 4        # kit_fight.py SWING_FRAMES
AFTER_SWING = 30        # frames after each swing before his life is read
NEAR_M = 1.5            # Link's place from him for a swing (kit_fight.py REACH_M 2.2: inside the sword's reach)
IDLE_FRAMES = 1800      # c2's no-input tail (the row's phase-2 frames x 3)
FLY_IDLE_FRAMES = 1800  # c3's no-input tail in Fly (light balls, 112, and whatever the AI does next)

# The module's lines for him (the texts the run's logs show; writers cited):
MOVE_TO_1 = re.compile(r"move probe: %s 0x[0-9a-f]+ [^:]+:.* move state \d+ \(type \d+, before \d+\) -> 1 \(type" % PG)
ATTACK = re.compile(r"%s 0x[0-9a-f]+ attack starts \(action verbs (\d+)" % PG)          # coop_objects.cpp:388
ATTACK_139 = re.compile(r"%s 0x[0-9a-f]+ attack starts \(action verbs 139\b" % PG)
ANIM = re.compile(r"objects: anim %s 0x[0-9a-f]+ '([^']+)'" % PG)                       # game-out-A.txt:7221 class
PROBE_ANIM = re.compile(r"move probe: %s 0x[0-9a-f]+ .*verbs (\d+) \d+ \d+ \d+ \d+, ([^,]+), at " % PG)
PG_LINE = re.compile(r"(%s|DungeonBoss_PhantomGanon)" % PG)
KEEP = 400              # PG lines kept per window and game in the JSON


def _window(pair, name, pos, rec):
    """The lines of every game since `pos` (its log marks) as window `name`: PG lines kept, what started, anims, verbs."""
    w = {"games": {}}
    started = []
    for x in pair.c:
        lines = pair.log[x].since(pos[x])
        pg = [ln for ln in lines if PG_LINE.search(ln)]
        anims = sorted({m.group(1) for m in map(ANIM.search, lines) if m}
                       | {m.group(2) for m in map(PROBE_ANIM.search, lines) if m})
        verbs = sorted({int(m.group(1)) for m in map(ATTACK.search, lines) if m})
        hit = [ln for ln in lines if MOVE_TO_1.search(ln) or ATTACK_139.search(ln)]
        w["games"][x] = {"lines": pg[-KEEP:], "anims": anims, "attack_verbs": verbs, "fall_down": hit[:6]}
        started += ["%s: %s" % (x, ln.strip()) for ln in hit[:2]]
    w["started"] = started
    rec["windows"][name] = w
    return w


def _marks(pair):
    return {x: pair.log[x].mark() for x in pair.c}


def _flying(w):
    return any("Fly" in a for g in w["games"].values() for a in g["anims"])


def _swing(pair, x, rec):
    """One sword swing of game x's Link at him: placed NEAR_M off him on his +x side, facing him, ZL+Y then ZL."""
    b, me = boss(pair, x), pair.state(x)
    if dead(b) or not me.get("pos"):
        return None
    bx, _, bz = b["pos"]
    pair.act(x, "place %.2f %.2f %.2f" % (bx + NEAR_M, me["pos"][1], bz))
    pair.act(x, "face %.2f %.2f" % (bx, bz))
    pair.press(x, SWING, frames=SWING_FRAMES)
    pair.press(x, ("ZL",), frames=8)
    pair.step_both(AFTER_SWING)
    after = boss(pair, x)
    life = None if after is None else after.get("life")
    rec["swings"].append({"life_before": b.get("life"), "life_after": life})
    return life


@pytest.mark.budget(1200)       # ~7000 frames stepped at ~12 a second (row215-309d7180's measure) + the set-up
@pytest.mark.needs_env(*PROBE_ENV)
@pytest.mark.needs_boot("live-phantom-ganon")
@pytest.mark.row("PROBE Phantom Ganon's fall-down trigger (row 287, B #165; not a row)")
def test_probe_pg_falldown(pair):
    rec = {"windows": {}, "swings": [], "owner": None}
    order = []
    try:
        with pair.setup():
            if (pair.info or {}).get("fake"):
                pair.warp_both(SCENE)               # (the fake lane has no boot scripts)
            st = pair.need_boot_place()
            for x in ("A", "B"):
                assert st[x].get("scene") == SCENE, "%s is in %s, not %s (the boot live-phantom-ganon)" % (
                    x, st[x].get("scene"), SCENE)
                pair.until("%s sees %s" % (x, PG), lambda x=x: boss(pair, x), max_frames=120)
        # the row's start (test_bosses.py): lock on, 600 frames
        pair.press("A", ["ZL"], frames=10)
        pair.press("B", ["ZL"], frames=10)
        pair.step_both(600)
        # c1: verb 139 asked of both games (the owner's game answers 'asked of our boss')
        pos = _marks(pair)
        for x in ("A", "B"):
            pair.act(x, "verb %s 139" % PG)
        pair.step_both(60)
        _window(pair, "c1-verb139", pos, rec)
        order.append("c1-verb139")
        owner = next((x for x in ("A", "B") if any(re.search(ASKED_OURS, ln) for ln in pair.log[x].since(pos[x]))),
                     None)
        rec["owner"] = owner
        if owner is None:
            raise P.SetupFail("no game answered '%s' to verb 139: no owner of %s to probe" % (ASKED_OURS, PG))
        # c2: the game's own damage on the horse (sword), then his AI with no input
        pos = _marks(pair)
        for _ in range(SWORD_ROUNDS):
            b = boss(pair, owner)
            if dead(b) or (b.get("life") or 0) <= STOP_LIFE:
                break
            _swing(pair, owner, rec)
        pair.step_both(IDLE_FRAMES)
        w = _window(pair, "c2-sword", pos, rec)
        order.append("c2-sword")
        # c3: in Fly only
        if _flying(w) and not dead(boss(pair, owner)):
            pos = _marks(pair)
            pair.act(owner, "verb %s 139" % PG)
            pair.step_both(60)
            if not dead(boss(pair, owner)) and (boss(pair, owner).get("life") or 0) > 1:
                pair.act(owner, "hit %s 1" % PG)
            pair.step_both(FLY_IDLE_FRAMES)
            _window(pair, "c3-fly", pos, rec)
            order.append("c3-fly")
    except P.SetupFail as e:                  # (a set-up miss stays SETUP-FAIL, never a probe result; JSON kept)
        rec["error"] = "SetupFail: %s" % e
        raise
    except Exception as e:                   # noqa: BLE001 (recorded; the JSON is the product)
        rec["error"] = "%s: %s" % (type(e).__name__, e)
    finally:
        with open(os.path.join(pair.out_dir or ".", "probe-pg-falldown.json"), "w", encoding="utf-8") as f:
            json.dump(rec, f, indent=1)
    first = next((n for n in order if rec["windows"][n]["started"]), None)
    lives = [s["life_after"] for s in rec["swings"]]
    if first:
        out = "STARTED by %s (owner %s): %s" % (first, rec["owner"], " | ".join(rec["windows"][first]["started"]))
    else:
        seen = "; ".join("%s %s: anims %s, attack verbs %s" % (n, x, g["anims"], g["attack_verbs"])
                         for n in order for x, g in sorted(rec["windows"][n]["games"].items()))
        out = "UNPROVEN: no '-> 1' move probe line and no 'attack starts (action verbs 139' in %s; seen: %s" % (
            ", ".join(order), seen)
    out += "; swings %d, his life after them %s" % (len(lives), lives[-3:])
    if rec.get("error"):
        out += "; error " + rec["error"]
    print(out)
    pair.notes.append(out)
    pair.unproven("probe only (probe-pg-falldown.json): " + out)
