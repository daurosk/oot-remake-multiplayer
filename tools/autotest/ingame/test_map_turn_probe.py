"""G2b map icons (#255 item 8): the PROBE of the map arrow's turn place, on ONE game (the --solo boot, solo.py).

coop_map_icons.h kPaneTurn is 0 (not known: no guessed value ships, B #142) and with no turn place the game's copies
stay down (coop_hookshot.cpp MapIcons), so the other players' arrows stay the overlay's (no loss against 0.11.12).
Its candidate is +0x68 of the arrow pane N_PlayerRotate_00 (the .buil pane record order, coop_map_icons.h). This test
answers it on the game's OWN arrow (Link's, the one the game turns itself): while the bridge is on and the copies are
not stepped, the module publishes `map_native` from Icons::ProbeArrow (`probe` true): the words +0x60..+0x7c of the
arrow in MapMenu_00's own Pa_MapIcon_00 (`game_arrow_words`) and of up to 4 of the game's run-time icon copies under
the map screen (`run_arrow_words`), every pane looked up fresh, nothing written or shown.

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_map_turn_probe | map-icons-probe-3 (offline/map-icons-probe-3-run.ps1, never ran: held B #16) | --solo (lane-prep of map-icons-255) | state map_native (probe) + check_shot A |

A faces 0/90/180/270 (docs/playtest.md `move`), each read from the state's `facing` (and `camera`: a minimap turned
with the view would make the arrow follow facing minus camera). A word FOLLOWS when, at all 4 facings, word - s * ref
is one constant (mod a full turn, within QUARTER_TOL) for s = +1 or -1, in radians (or in degrees: then reported as
such, since the module writes radians). PASS: word index 2 (+0x68) follows in radians on the template or a run-time
copy (the notes name which, the sign and the reference): kPaneTurn = 0x68 may then be set citing this run. FAIL: it does
not while another word does (the notes name that one). NO-REF (unproven): no word follows (the game turns another pane).

PROBE v2 (map-icons-0121, B #152 20:38: no stop at kPaneTurn 0): a module with `arrow_raw` (the raw words +0x28..+0xbc
of each arrow: [0] the template's, [1..] the run-time copies'; the global matrix +0x28..+0x54 first) also gives the
DRAWN turn of each arrow, atan2(m[1], m[0]) of its global matrix, read from the frame itself, and every own word
+0x28..+0xbc is tried, not only +0x60..+0x7c. The notes then say whether the arrow is drawn turned (the matrix follows)
and which own word drives it (its place in hex, "+0x.."): FAIL names that place (try it with
LICHII_COOP_MAP_TURN_AT=<hex>); with the matrix following and no own word, NO-REF says "drawn turned, no own word: the
turn comes from the matrix / an animation" (the next probe: the arrow's animation or its parent's). A module without
`arrow_raw` runs the v1 test unchanged.

Set-up as test_map_icons.py (hs-map-facing.A.txt): 'warp InsideTheDekuTree Entrance', the dungeon's map flag (the
minimap needs it).
"""
import math

import pytest

import solo

SCENE, ENTRANCE = "InsideTheDekuTree", "Entrance"     # hs-map-facing.A.txt 'warp InsideTheDekuTree Entrance'
MAP_FLAG = "DungeonInfo.InsideTheDekuTree.IsGetMap"  # hs-map-facing.A.txt 'setflag DungeonInfo.InsideTheDekuTree.IsGetMap 1'
YAWS = (0, 90, 180, 270)            # world yaw degrees (docs/playtest.md `move`: 0 toward +z, 90 toward +x)
TURN_FRAMES = 20                    # hs-map-facing.B.txt: 'a short stick push of 20 ticks each'
SETTLE = 60                         # the probe publishes every 30 frames (coop_hookshot.cpp MapIcons probe_tick % 30)
#                                     + the game's own arrow update: 2 publishes after the walk ends
FIRST_WAIT = 60                     # hs-map-facing.A.txt 'setflag ...; wait 60; mark mini-1'
TURN_WORD = (0x68 - 0x60) // 4      # index 2 of the words (+0x60..)
QUARTER = math.pi / 2
QUARTER_TOL = math.radians(30)      # as test_map_icons.QUARTER_TOL (30 deg)
SHOT_TOL = 0.90                     # pair.check_shot's default
SHOT_AT = (0, 2)                    # the minimap at two opposite facings (Link's arrow seen turned)


def _diff(a, b, full=2 * math.pi):
    d = (a - b) % full
    return d - full if d > full / 2 else d


def follows(values, refs, full):
    """The sign s (+1 / -1) for which value - s * ref is one constant at every sample (mod `full`), else None."""
    for s in (1, -1):
        offs = [v - s * r * full / (2 * math.pi) for v, r in zip(values, refs)]
        if all(abs(_diff(o, offs[0], full)) <= QUARTER_TOL * full / (2 * math.pi) for o in offs[1:]):
            return s
    return None


def probe(pair):
    mn = pair.state("A").get("map_native")
    assert mn is not None, "A: map_native is null: no probe published (no map screen, or the map flag not taken)"
    assert mn.get("probe"), "A: map_native is not the probe's (the copies are stepped: %r); a solo boot steps none" % mn
    return mn


RAW_AT = 0x28                       # coop_map_icons.h State::arrow_raw: +0x28..+0xbc
MATRIX_WORDS = 12                   # coop_map_icons.h kPaneMatrix: 3 x 4 by column at +0x28 (m[0], m[1] = the x axis)


def drawn_turn(raw):
    """The turn the arrow is drawn with: the angle of its global matrix's x axis (None: no matrix)."""
    if len(raw) < 2 or (abs(raw[0]) < 1e-6 and abs(raw[1]) < 1e-6):
        return None
    return math.atan2(raw[1], raw[0])


def raw_matches(rows, refs):
    """probe v2: every (source, place, reference, sign, unit) among the own words +0x28+12*4.. that follows, and every
    (source, reference, sign) whose drawn turn (matrix) follows. Sources: template, run0, run1, ... ([0] = template)."""
    n = min(len(r["arrow_raw"]) for r in rows)
    words, drawn = [], []
    for k in range(n):
        name = "template" if k == 0 else "run%d" % (k - 1)
        series = [r["arrow_raw"][k] for r in rows]
        turns = [drawn_turn(w) for w in series]
        if all(t is not None for t in turns) and max(turns) - min(turns) > 1e-3:
            for ref_name, ref in refs.items():
                s = follows(turns, ref, 2 * math.pi)
                if s is not None:
                    drawn.append((name, ref_name, s))
        for i in range(MATRIX_WORDS, min(len(w) for w in series)):
            values = [w[i] for w in series]
            if max(values) - min(values) < 1e-3:
                continue
            for ref_name, ref in refs.items():
                for unit, full in (("rad", 2 * math.pi), ("deg", 360.0)):
                    s = follows(values, ref, full)
                    if s is not None:
                        words.append((name, "+0x%x" % (RAW_AT + 4 * i), ref_name, s, unit))
    return words, drawn


def matches(rows, refs):
    """Every (source, word index, reference, sign, unit) that follows."""
    out = []
    sources = {"template": [r["game_arrow_words"] for r in rows]}
    for k in range(min(len(r["run_arrow_words"]) for r in rows)):
        sources["run%d" % k] = [r["run_arrow_words"][k] for r in rows]
    for name, series in sources.items():
        if not all(series):
            continue
        for i in range(min(len(w) for w in series)):
            values = [w[i] for w in series]
            if max(values) - min(values) < 1e-3:
                continue   # (a word that never moves follows nothing)
            for ref_name, ref in refs.items():
                for unit, full in (("rad", 2 * math.pi), ("deg", 360.0)):
                    s = follows(values, ref, full)
                    if s is not None:
                        out.append((name, i, ref_name, s, unit))
    return out


@solo.MARK
@pytest.mark.budget(180)   # ESTIMATED: one warp + ~600 frames + 2 shots
@pytest.mark.look
@pytest.mark.row("G2b map icons (#255 item 8): the probe of the map arrow's turn place (kPaneTurn) on the game's own arrow, solo")
def test_map_turn_probe(pair):
    with pair.setup():
        solo.prove(pair)
        pair.warp_both(SCENE, ENTRANCE)
        s = pair.state("A")
        assert pair.in_play(s), "A not in play (%s)" % s.get("not_ready")
        assert s.get("scene") == SCENE, "A in %s, not %s" % (s.get("scene"), SCENE)
        assert "map_native" in s, "A: no map_native member (a module before map-icons-255)"
        pair.flag("A", MAP_FLAG, 1)
        assert pair.flag_read("A", MAP_FLAG).get("value"), "A: %s not 1 after the setflag" % MAP_FLAG
        pair.step_both(FIRST_WAIT)
        mn = probe(pair)   # (a module before map-icons-255-2 publishes no probe: SETUP-FAIL, not FAIL)
        assert mn.get("game_arrow") or mn.get("run_arrow_words"), (
            "A: the probe found no arrow of the game's (%r, table %s)" % (mn.get("why"), mn.get("table")))
    rows, facing, camera = [], [], []
    for k, yaw in enumerate(YAWS):
        pair.call("A", "move", yaw=yaw, strength=1.0, frames=TURN_FRAMES)
        pair.step_both(SETTLE)
        st = pair.state("A")
        assert st.get("facing") is not None, "A: no facing in the state after the walk toward %d" % yaw
        facing.append(math.radians(st["facing"]))
        camera.append(math.radians(st["camera"]) if st.get("camera") is not None else None)
        mn = probe(pair)
        rows.append({"game_arrow_words": list(mn.get("game_arrow_words") or []),
                     "run_arrow_words": [list(w) for w in mn.get("run_arrow_words") or []],
                     "arrow_raw": [list(w) for w in mn.get("arrow_raw") or []] if "arrow_raw" in mn else None})
        if k in SHOT_AT:
            pair.check_shot("A", "map-turn-%d" % k, SHOT_TOL)
    pair.notes.append("A facing per walk %s: %s deg, camera %s" % (
        YAWS, ["%.1f" % math.degrees(f) for f in facing],
        ["%.1f" % math.degrees(c) if c is not None else None for c in camera]))
    pair.notes.append("words +0x60.. per facing: %s" % [
        {k: v for k, v in r.items() if k != "arrow_raw"} for r in rows])
    v2 = all(r["arrow_raw"] is not None for r in rows)
    if v2:
        pair.notes.append("probe v2 raw +0x28..+0xbc per facing: %s" % [r["arrow_raw"] for r in rows])
    steps = [_diff(facing[k + 1], facing[k]) for k in range(len(facing) - 1)]
    assert all(abs(abs(d) - QUARTER) <= QUARTER_TOL for d in steps), (
        "the walks did not turn Link a quarter each (facing %s): the probe has nothing to follow" % (
            ["%.1f" % math.degrees(f) for f in facing]))
    refs = {"facing": facing}
    if all(c is not None for c in camera):
        refs["facing-camera"] = [f - c for f, c in zip(facing, camera)]
    found = matches(rows, refs)
    pair.notes.append("words that follow (source, index, reference, sign, unit): %s (kPaneTurn index %d)" % (found, TURN_WORD))
    raw_words, drawn = raw_matches(rows, refs) if v2 else ([], [])
    if v2:
        pair.notes.append("probe v2: drawn turn (matrix) follows on %s; own words that follow (source, place, reference, "
                          "sign, unit): %s" % (drawn or "none", raw_words))
    ours = [m for m in found if m[1] == TURN_WORD and m[4] == "rad"]
    ours += [(m[0], TURN_WORD, m[2], m[3], m[4]) for m in raw_words if m[1] == "+0x68" and m[4] == "rad" and not ours]
    if ours:
        pair.notes.append("kPaneTurn = 0x68 PROVEN on %s: word = %+d * %s + constant (radians)" % (
            ours[0][0], ours[0][3], ours[0][2]))
        return
    if raw_words:
        pytest.fail("the game's own arrow turns at %s (probe v2, drawn turn follows on %s), not at +0x68 in radians: try "
                    "LICHII_COOP_MAP_TURN_AT=<that place> and set kPaneTurn from it" % (raw_words, drawn or "none"))
    if found:
        pytest.fail("the game's own arrow turns at %s, not at +0x68 in radians (kPaneTurn index %d): set kPaneTurn "
                    "from that one" % (found, TURN_WORD))
    if drawn:
        pair.unproven("probe v2: the arrow is drawn turned (matrix follows on %s) but no own word +0x28..+0xbc follows: "
                      "the turn comes from the matrix / an animation; kPaneTurn stays 0 (next: the arrow's animation)" % drawn)
        return
    pair.unproven("no word +0x60..+0x7c of the game's own arrows (template or run-time copy) followed Link's facing "
                  "(notes): the game turns another pane; kPaneTurn stays 0")
