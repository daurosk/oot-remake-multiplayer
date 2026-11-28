"""Row 317 probes (reports/carry-revive-317.md, B #159 GO 23:55 UTC 2026-11-27): what the game does before any 317 code.

| Test | Probe | Boot | Kind |
|---|---|---|---|
| test_probe_lift_prompt_317 | P1: the A prompt at a young copy and at Ruto, near and carrying; the first lift of a player in a game (N2) | --pack native (NATIVE_PUPPETS=body,lift) + LICHII_COOP_LIFT_PLAYER=1 | state `prompt` + module log + LOOK shots |
| test_probe_revive_button_317 | P2: which buttons do nothing beside a downed copy (the revive button's pick) | any | state before/after each press + LOOK shots of A |

Both are PROBES: their result is NO-REF with every value in the notes (never PASS), unless the set-up fails
(SETUP-FAIL) or the module logs a refusal it should not (FAIL). The values they find are cited by the 317 code.

P1 (pack from tools/puppet_pack/build.py --puppet-liftable: the young copy has Ruto's LiftableRef AND, row 317 (a),
her Lift attention client `NpcZoraRutoChild_Grab`, build.py add_lift_prompt). Owner add 23:58: the player's prompts
must be exactly Ruto's (near, carrying: throw / put down): the probe records the state `prompt` (the game's own label,
hookshot/src/coop_playtest_core.cpp LinkMembers) at the copy and at Ruto, near and carrying, on A, and the shots of
both on each game. The compare of the HUD's prompt box needs the box measured from these shots first (no guessed
box): the notes give the shot names; equality of the labels is noted as `same`/`DIFFERENT`.
P2: B downed beside A (test_dbno_crawl's own set-up, imported), A presses each of PRESSES for PRESS_FRAMES; before and
after: A's pos, facing, equips, prompt, event, life, rupees; B still downed (no revive: today's revive is A only,
lichii/hook/coop_dbno_game.cpp OnPad). A button is inert when none of those changed.
"""
import math
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_dbno_crawl as dc   # noqa: E402  (to_place, rules, no_fairy, down, restored, event, dist, LYING_CHILD)

OPEN = ("KokiriForest_Child", "FrontDekuTree")   # test_downed_move.py: hs-dbno-child's open ground
JABU = ("InsideJabuJabuBelly", "InsideJabuJabuBelly_FromZoraFountain")   # scenarios/hs-lift-ride.A.txt:13
RUTO = "Npc_Zora_Ruto_Child_001"
# romfs Banc/InsideJabuJabuBelly_Static.bcett.byml.zs: Npc_Zora_Ruto_Child_001 'Step1', DefaultDungeonRoomIndex 2
RUTO_AT = (1.6263792514801025, 91.25308990478516, -77.98184967041016)
NEAR_M = 0.8          # A's feet this far from the one lifted (Ruto's Grab client CheckRideSpace CheckLength 0.85 m)
FAR_M = 6.0           # A this far away: the prompt with nothing to lift (beyond kFarM 3.0, coop_lift_player.h)
PRESS_FRAMES = 8      # one press (pair.press default)
LIFT_FRAMES = 120     # ESTIMATED wait for the lift lines (Ruto's lift: Lift_Ready + Lift_Up; kAnswerWaitUs 1.5 s)
SETTLE = 10           # frames after a place / face before the prompt is read
# lichii/hook/coop_lift_player_game.cpp (the line numbers of integrate-0121 7c721a9ec)
ASKING = r"coop: lift: our Link lifted P2's copy: asking"                          # :244
NO_LIFT = r"coop: lift: P2's copy rose but our Link's animation '([^']*)' is no lift"   # :240
ANSWERED = r"coop: lift: P2 answered our lift: (.*)"                               # :207
TAKEN = r"coop: lift: P1 lifts our Link: (.*)"                                     # :287
INPUT = r"coop: lift: our Link's input (off|back) \(held by P1\)"                  # :301
CARRIED = r"coop: lift: carried: our own Link plays '([^']*)'"                    # :115
PRESSES = ("B", "X", "L", "R", "DDOWN", "DUP")   # coop_testinput.cpp kNames; Y is the sword (docs/playtest.md:505)
MOVED_M = 0.1         # = test_dbno_crawl CRAWL_MIN_M ("moved at all")
TURNED_DEG = 5.0      # ESTIMATED: a facing change smaller than this is the sample's noise
WATCH = ("pos", "facing", "equips", "prompt", "event", "life", "rupees")


def last(pair, x, pattern):
    rx = re.compile(pattern)
    found = [rx.search(ln) for ln in pair.log[x].since(0)]
    found = [m for m in found if m]
    return found[-1] if found else None


def prompt(pair, x="A"):
    return pair.state(x).get("prompt")


def beside(pair, at, m):
    """A placed m from `at` on +x, facing it (docs/playtest.md place / face); SETTLE frames."""
    pair.act("A", "place %.2f %.2f %.2f" % (at[0] + m, at[1], at[2]))
    pair.act("A", "face %.2f %.2f" % (at[0], at[2]))
    pair.step_both(SETTLE)


def ruto_in_world(pair):
    w = pair.c["A"].world().get("world") or {}
    for a in w.get("actors", []):
        if (a.get("name") or "").startswith("Npc_Zora_Ruto_Child") and a.get("pos"):
            return a
    return None


def lift_once(pair, what):
    """A presses A; up to LIFT_FRAMES for any lift line. Returns {'carry_prompt', 'lines'} (notes only)."""
    pair.press("A", ["A"], frames=PRESS_FRAMES)
    ran = 0
    while ran < LIFT_FRAMES and not (last(pair, "A", ASKING) or last(pair, "A", NO_LIFT)) and what == "copy":
        pair.step_both(10)
        ran += 10
    if what != "copy":
        pair.step_both(LIFT_FRAMES)
    return prompt(pair)


@pytest.mark.budget(480)   # ESTIMATED: two warps + ~900 frames + 8 shots
@pytest.mark.look
@pytest.mark.needs_pack("native")
@pytest.mark.needs_env("LICHII_COOP_LIFT_PLAYER=1")
@pytest.mark.row("317 probe P1: the game's A prompt at a young copy vs Ruto (near, carrying), and the first player lift (N2)")
def test_probe_lift_prompt_317(pair):
    found = {}
    with dc.restored(pair, {}):
        with pair.setup():
            pair.need_boot_place("child")
            dc.to_place(pair, "child", *OPEN)
            b = pair.state("B")["pos"]
            beside(pair, b, FAR_M)
            found["alone"] = prompt(pair)
        # 1. the copy: near, lift, carrying, throw
        beside(pair, pair.state("B")["pos"], NEAR_M)
        found["copy_near"] = prompt(pair)
        dc.shots(pair, "lift317-copy-near", twice=False)
        found["copy_carry"] = lift_once(pair, "copy")
        dc.shots(pair, "lift317-copy-carry", twice=False)
        for k, pat in (("asking", ASKING), ("no_lift", NO_LIFT), ("answered", ANSWERED)):
            m = last(pair, "A", pat)
            found["A_" + k] = (m.group(1) if m.groups() else m.group(0)) if m else None
        for k, pat in (("taken", TAKEN), ("input", INPUT), ("carried_pose", CARRIED)):
            m = last(pair, "B", pat)
            found["B_" + k] = m.group(1) if m else None
        before = pair.state("B")["pos"]
        pair.press("A", ["A"], frames=PRESS_FRAMES)       # the throw (or put down) as the game offers it
        pair.step_both(LIFT_FRAMES)
        found["after_throw_prompt"] = prompt(pair)
        found["B_moved_m"] = round(dc.dist(before, pair.state("B")["pos"]), 2)
        m = last(pair, "B", INPUT)
        found["B_input_end"] = m.group(1) if m else None
        # 2. Ruto (A's own game only; B goes along so the pair stays in one scene)
        with pair.setup():
            pair.warp_both(*JABU)
            pair.step_both(30)
            beside(pair, RUTO_AT, NEAR_M)
            pair.step_both(30)
            ruto = ruto_in_world(pair)
            assert ruto is not None, "A: no %s in the world list near her placement %s" % (RUTO, RUTO_AT)
        beside(pair, ruto["pos"], NEAR_M)
        found["ruto_near"] = prompt(pair)
        found["ruto_event"] = pair.state("A").get("event")
        pair.check_shot("A", "lift317-ruto-near", tolerance=0.80)
        found["ruto_carry"] = lift_once(pair, "ruto")
        pair.check_shot("A", "lift317-ruto-carry", tolerance=0.80)
        pair.press("A", ["A"], frames=PRESS_FRAMES)
        pair.step_both(LIFT_FRAMES)
        found["ruto_after_throw_prompt"] = prompt(pair)
    for k in ("near", "carry"):
        found["same_" + k] = "same" if found["copy_" + k] == found["ruto_" + k] else "DIFFERENT"
    pair.notes.append("P1 values: " + ", ".join("%s=%r" % kv for kv in sorted(found.items())))
    refused = found.get("A_answered")
    # coop_lift_player_game.cpp RefusalText: LiftRefusal::none says "yes"; anything else refused a young standing copy
    assert refused in (None, "yes"), "B refused A's lift of a young standing copy: %r" % refused
    pair.unproven("probe P1: values in the notes (shots lift317-copy-near/-carry on each game, lift317-ruto-near/-carry "
                  "on A): the HUD prompt box is measured from them before any shot compare")


@pytest.mark.budget(420)   # ESTIMATED: the down (test_dbno_crawl: ~12 hits x 60 frames) + 6 presses x 40 frames + 6 shots
@pytest.mark.look
@pytest.mark.row("317 probe P2: which buttons do nothing beside a downed copy (the revive button's pick)")
def test_probe_revive_button_317(pair):
    rows = []
    with dc.restored(pair, {}):
        with pair.setup():
            pair.need_boot_place("child")
            dc.to_place(pair, "child", *OPEN)
            dc.rules(pair, 238)        # long: no bleed-out during the presses (test_downed_move.py)
            dc.no_fairy(pair, "B")
        dc.down(pair)
        pair.expect_log("B", dc.LYING_CHILD, max_frames=600)
        for button in PRESSES:
            b = pair.state("B")["pos"]
            beside(pair, b, dc.BESIDE_M)
            a0 = pair.state("A")
            assert dc.dist(a0["pos"], b) < dc.REACH_M, "A not beside B (%.2f m)" % dc.dist(a0["pos"], b)
            pair.press("A", [button], frames=PRESS_FRAMES)
            pair.step_both(30)
            a1 = pair.state("A")
            changed = []
            if dc.dist(a0["pos"], a1["pos"]) > MOVED_M or abs(a0["pos"][1] - a1["pos"][1]) > MOVED_M:
                changed.append("pos")
            if a0.get("facing") is not None and a1.get("facing") is not None and \
                    abs((float(a1["facing"]) - float(a0["facing"]) + 180) % 360 - 180) > TURNED_DEG:
                changed.append("facing")
            changed += [k for k in WATCH[2:] if a0.get(k) != a1.get(k)]
            rows.append((button, changed, a1.get("prompt"), a1.get("event")))
            pair.check_shot("A", "revive317-press-%s" % button.lower(), tolerance=0.80)
            pair.never_log("B", dc.event(dc.EV_REVIVED))
            pair.never_log("B", dc.event(dc.EV_BLED_OUT))
    inert = [r[0] for r in rows if not r[1]]
    pair.notes.append("P2: " + "; ".join("%s -> %s (prompt %r, event %r)" % (r[0], ", ".join(r[1]) or "nothing",
                                                                            r[2], r[3]) for r in rows))
    pair.notes.append("P2 inert beside a downed copy: %s" % (", ".join(inert) or "none"))
    pair.unproven("probe P2: inert buttons %s (the revive button is picked from them: B first)" % (inert or "none"))
