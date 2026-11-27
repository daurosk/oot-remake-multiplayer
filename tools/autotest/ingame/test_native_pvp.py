"""True PvP (owner 2026-11-27, 0.12.1; docs/native-puppets.md section 5, plan N3/N4) as bridge tests. With PvP on and
both games native (LICHII_COOP_PVP_NATIVE=1, a pack built --puppet-body --puppet-pvp: packs/native.py with
NATIVE_PUPPETS=body,pvp), A's own weapons hit B's `_Pvp` copy through the game's own hit path; A's game reads the
copy's hit record and sends a strike (PVP_STRIKE 540); B's game judges it (path B, coop_pvp_native.h DecidePathB)
and answers PVP_RESULT (541).

| Test | Checks | Kind |
|---|---|---|
| test_pvp_probe | the old pvp-probe scenario (scenarios/pvp-probe.A/B.txt) as a bridge test: per young kind (slash, spin, jump attack, Deku Stick) A's game logs the copy's hit record (U2) and its strike, B's game its outcome; a slash at B guarding (ZL) logs B's guard read (U5). Every value is written to <out>/pvp-probe.json for the kind table (U2/U7) | log + state + shots |

The probe proves the chain A hit -> strike -> B outcome for the slash (FAIL without it) and collects the rest; a kind
that left no line is named in the notes (the kind table is filled only from kinds that did).

Values and their sources:
- place: pvp-probe.A.txt 'place 198.63 163.4 309.87', pvp-probe.B.txt 'place 196.93 163.4 309.87' (1.7 m apart,
  facing each other: the 'toward' lines), scene/entrance: both files' 'warp HyruleField_Child
  HyruleField_FrontOfKakarikoVillage'; age child (the files' "young kinds").
- the inputs: pvp-probe.A.txt ('hold ZL+Y 8' slash, 'hold ZL+Y 70' spin, 'hold ZL+A 20' jump, 'assign DekuStick' +
  'hold ZL+ZR 8' stick, 180 frames between kinds); the button names from kit_fight.py (LOCK/SWORD/SHIELD: Longshot's
  own map, ZL locks on and, held without the sword, raises the shield).
- the log lines: lichii/hook/coop_pvp_game.cpp:152 (mode), coop_pvp_native_game.cpp:232 (copies), :328 (strike),
  :371 (U5), :383 (U5 bits), :398 (outcome), coop_objects.cpp:5436 (U2 record).
"""
import json
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from test_enemies import face, place   # noqa: E402  (bridge place / face, as the scenarios' 'place' / 'toward')
import kit_fight   # noqa: E402  (LOCK, SWORD, SHIELD: the one button map)

SCENE, ENTRANCE = "HyruleField_Child", "HyruleField_FrontOfKakarikoVillage"   # pvp-probe.A/B.txt 'warp'
A_AT = (198.63, 163.4, 309.87)          # pvp-probe.A.txt 'place'
B_AT = (196.93, 163.4, 309.87)          # pvp-probe.B.txt 'place'
LOCK, SWORD, SHIELD = kit_fight.LOCK, kit_fight.SWORD, kit_fight.SHIELD
JUMP, ITEM = "A", "ZR"                  # pvp-probe.A.txt 'hold ZL+A' (jump attack), 'assign' puts the item on ZR
# (name, setup act line or None, buttons, frames): pvp-probe.A.txt's four kinds in its order
KINDS = (("slash", None, (LOCK, SWORD), 8),
         ("spin", None, (LOCK, SWORD), 70),
         ("jump", None, (LOCK, JUMP), 20),
         ("stick", "assign DekuStick", (LOCK, ITEM), 8))
KIND_GAP = 180                          # pvp-probe.A.txt 'wait 180' after each kind
HIT_FRAMES = 240                        # a strike and its answer within this after the input (strike late 550 ms)
GUARD_FRAMES = 30                       # B raises the shield this long before A's slash, and keeps it through it
MODE_FRAMES = 300                       # pvp-probe.A.txt 'chat /pvp on' then 'wait 240'

PVP_ON = r"coop: pvp: on \(set by \d+\)"                                         # coop_pvp_game.cpp:152
COPIES = r"coop: pvp native: _Pvp copies for players 0x[0-9a-f]+ \(PvP on\)"     # coop_pvp_native_game.cpp:232
NO_PACK = r"coop: pvp native: on; the puppet pack has no _Pvp copies"             # coop_pvp_native_game.cpp:112
RECORD = r"coop: pvp native: U2 record of our hit on P(\d+)'s copy \(id (0x[0-9a-f]+), \+0x228 (-?\d+)\): ([0-9a-f ]+)"
STRIKE = (r"coop: pvp native: strike (\d+) on P(\d+) \(source (\d+), our game counted (-?\d+)\) seen at \d+; "
          r"U7: our main AS (\S+), weapon (\d+)")                                  # coop_pvp_native_game.cpp:328
OUTCOME = r"coop: pvp native: P(\d+)'s strike %s: outcome (\d+), take (-?\d+) \(attacker said (-?\d+)\), life (-?\d+)"
U5 = r"coop: pvp native: U5: our Link's main AS at a strike: (\S+) \(guard (yes|no)\)"   # coop_pvp_native_game.cpp:371
BITS = (r"coop: pvp native: U5 bits at P\d+'s strike %s: player \+0x32 (0x[0-9a-f]+) \(bit 4 (\d)\), "
        r"EquipmentUser \+0x13a0 (0x[0-9a-f]+) \(bit 5 (\d)\), AS guard (yes|no)")   # coop_pvp_native_game.cpp:383
OUTCOMES = ("taken", "guarded", "guard_broken", "invincible", "late", "out_of_reach", "kept_off")   # messages.hpp
TOL = 0.80                              # ESTIMATED: a hit frame in an open field (grass moves); refs are owner-approved


def shots(pair, mark):
    for x in ("A", "B"):
        pair.check_shot(x, "native-pvp-%s" % mark, tolerance=TOL)


def to_field(pair):
    """Both games young at the probe's place, facing each other, PvP on (set by the host A), the copies made."""
    for x in ("A", "B"):
        pair.flag(x, "Sequence_IsChildhood", 1)
    pair.step_both(30)
    pair.warp_both(SCENE, ENTRANCE)
    for x in ("A", "B"):
        s = pair.state(x)
        assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
        assert (s.get("scene") or "") == SCENE, "%s in %s, not %s" % (x, s.get("scene"), SCENE)
    pair.give("A", "dekustick")                     # pvp-probe.A.txt 'give dekustick'
    place(pair, "B", B_AT)
    place(pair, "A", A_AT)
    pair.step_both(30)
    face(pair, "A", B_AT)
    face(pair, "B", A_AT)
    pair.act("A", "chat /pvp on")
    for x in ("A", "B"):
        pair.expect_log(x, PVP_ON, max_frames=MODE_FRAMES)
    for x in ("A", "B"):
        assert pair.log[x].find(NO_PACK) is None, "%s: the pack has no _Pvp copies (not built --puppet-pvp)" % x
        pair.expect_log(x, COPIES, max_frames=MODE_FRAMES)


def one_kind(pair, name, buttons, frames, b_buttons=()):
    """A presses `buttons` for `frames` frames (B holds `b_buttons` the same frames, 2 frames at a time so both pads
    are down together); then the strike on A and B's outcome for it. Returns the kind's facts (None where no line)."""
    pos = {x: pair.log[x].mark() for x in ("A", "B")}
    life0 = pair.state("B").get("life")
    left = int(frames)
    while left > 0:
        n = min(2, left)
        if b_buttons:
            pair.call("B", "input", buttons=list(b_buttons), frames=n)
        pair.call("A", "input", buttons=list(buttons), frames=n)
        if not b_buttons:
            pair.call("B", "step", frames=n)
        left -= n
    got = {"kind": name, "buttons": list(buttons), "frames": frames, "b_buttons": list(b_buttons), "life_before": life0}
    try:
        line = pair.until("A strike (%s)" % name, lambda: pair.log["A"].find(STRIKE, pos["A"]), max_frames=HIT_FRAMES)
    except AssertionError:
        line = None
    shots(pair, name)
    if line:
        m = re.search(STRIKE, line)
        got.update(seq=int(m.group(1)), victim=int(m.group(2)), source=int(m.group(3)), counted=int(m.group(4)),
                   attacker_as=m.group(5), weapon=int(m.group(6)))
        rec = pair.log["A"].find(RECORD, pos["A"])
        if rec:
            r = re.search(RECORD, rec)
            got.update(hit_id=r.group(2), copy_hurt=int(r.group(3)), record=r.group(4).split())
        try:
            out = pair.until("B outcome of strike %d" % got["seq"],
                             lambda: pair.log["B"].find(OUTCOME % got["seq"], pos["B"]), max_frames=HIT_FRAMES)
        except AssertionError:
            out = None
        if out:
            o = re.search(OUTCOME % got["seq"], out)
            got.update(outcome=OUTCOMES[int(o.group(2))] if int(o.group(2)) < len(OUTCOMES) else o.group(2),
                       take=int(o.group(3)), said=int(o.group(4)), life_after=int(o.group(5)))
        bits = pair.log["B"].find(BITS % got["seq"], pos["B"])
        if bits:
            m = re.search(BITS % got["seq"], bits)
            got.update(b_part_byte=m.group(1), b_front_bit=int(m.group(2)), b_eu_word=m.group(3),
                       b_eu_bit=int(m.group(4)), b_as_guard=m.group(5) == "yes")
        u5 = pair.log["B"].find(U5, pos["B"])
        if u5:
            g = re.search(U5, u5)
            got.update(b_as=g.group(1), b_guard_anim=g.group(2) == "yes")
    got["life_end"] = pair.state("B").get("life")
    pair.step_both(KIND_GAP)
    return got


@pytest.mark.budget(420)   # ESTIMATED: one warp + ~2'500 game frames (the old scenario's waits) + 10 shots
@pytest.mark.look
@pytest.mark.needs_pack("native")
@pytest.mark.needs_env("LICHII_COOP_PVP_NATIVE=1", "LICHII_COOP_DEBUG_OBJECTS=1", "LICHII_COOP_CHEATS=1")
@pytest.mark.row("true PvP probe: A's weapons on B's _Pvp copy -> strike -> B's outcome (U2/U5/U7 values)")
def test_pvp_probe(pair):
    with pair.setup():
        to_field(pair)
    facts = []
    for name, line, buttons, frames in KINDS:
        if line:
            pair.act("A", line)
            pair.step_both(60)                       # pvp-probe.A.txt 'wait 60' after 'assign'
        face(pair, "A", B_AT)
        pair.step_both(4)
        facts.append(one_kind(pair, name, buttons, frames))
    # The guard (U5 / N4): B raises the shield toward A, A slashes.
    face(pair, "A", B_AT)
    face(pair, "B", A_AT)
    for _ in range(GUARD_FRAMES // 2):
        pair.call("B", "input", buttons=[SHIELD], frames=2)
        pair.call("A", "step", frames=2)
    facts.append(one_kind(pair, "slash-at-guard", (LOCK, SWORD), 8, b_buttons=(SHIELD,)))
    out = os.path.join(pair.out_dir or os.path.join(HERE, "out"), "pvp-probe.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(facts, f, indent=1)
    for k in facts:
        pair.notes.append("%s: %s" % (k["kind"], ", ".join("%s=%s" % (a, k[a]) for a in (
            "counted", "copy_hurt", "outcome", "take", "life_before", "life_end", "attacker_as", "b_as", "b_guard_anim",
            "b_front_bit", "b_eu_bit")
            if a in k)))
    missing = [k["kind"] for k in facts if "outcome" not in k]
    if missing:
        pair.notes.append("no strike/outcome for: %s" % ", ".join(missing))
    slash = facts[0]
    assert "seq" in slash, "A's slash at B's copy sent no strike (no U2 record or no strike line): the copy took no hit"
    assert "outcome" in slash, "B's game answered no outcome for A's slash strike %d" % slash["seq"]
