"""G2b (#255 item 8, branch map-icons-01113 + map-icons-255) as a bridge test: the other player on the game's OWN map
icons. The pack (tools/puppet_pack/map_icons.py) adds 15 hidden copies of MapMenu_00's Pa_MapIcon_00 part; the module
(hookshot/src/coop_map_icons.cpp) shows one per other player: Link's arrow turned their way, their name on the map's
place label. They are game panes, so the BRIDGE shot (framebuffer) sees them: check_shot on each game.

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_map_icons_native | map-icons-probe-3 (offline/map-icons-probe-3-run.ps1, never ran: held B #16) | default (lane-prep of this sha: its build.py makes the copies) | state map_native + check_shot A and B, minimap then the Map page on each game |

Judged by the state's `map_native` (coop_map_icons.h State, docs/playtest.md): `up` (the copies found through the pause
tab's proven part route), per shown copy its pane place, the arrow's turn READ BACK from the pane at kPaneTurn and what
was written, the label's name and whether the game's path setter took it. The probe part (the old probe-3's question,
kPaneTurn is 0 = not turned until proven, B #142; this run tries its candidate +0x68 through
--env LICHII_COOP_MAP_TURN_AT=68, needs_env) is answered by the game's own arrow:
`game_arrow_words` (+0x60..+0x7c of its N_PlayerRotate_00) while A turns a quarter at a time; the word that moves a
quarter (pi/2) per quarter is the turn. Its index goes into the notes; the test FAILS when the word at kPaneTurn does
not follow while another does (then kPaneTurn is that one), and is NO-REF when none moves (the game turns another pane).

Set-up as test_map_arrow (G2a) and hs-map-facing.A/B.txt: both 'warp InsideTheDekuTree Entrance', the dungeon's map
flag (the minimap and the Map page need it), MINUS opens the - menu on the Map page there.
"""
import math

import pytest

SCENE, ENTRANCE = "InsideTheDekuTree", "Entrance"     # hs-map-facing.A/B.txt 'warp InsideTheDekuTree Entrance'
MAP_FLAG = "DungeonInfo.InsideTheDekuTree.IsGetMap"  # hs-map-facing.A.txt 'setflag DungeonInfo.InsideTheDekuTree.IsGetMap 1'
YAWS = (0, 90, 180, 270)            # world yaw degrees (docs/playtest.md `move`: 0 toward +z, 90 toward +x)
TURN_FRAMES = 20                    # hs-map-facing.B.txt: 'a short stick push of 20 ticks each'
SETTLE = 120                        # ESTIMATED (as test_map_arrow): the new facing reaches the other game and its map
OPEN_WAIT = 180                     # hs-map-facing.A.txt 'press MINUS; wait 180; mark panel'
CLOSE_WAIT = 240                    # hs-map-facing.A.txt 'press B; wait 240; mark end'
QUARTER = math.pi / 2
QUARTER_TOL = math.radians(30)      # as test_map_arrow's QUARTER_TOL (30 deg): the walk's facing within a few degrees
TURN_TRIAL = "LICHII_COOP_MAP_TURN_AT=68"   # the probe's switch: coop_map_icons.h kPaneTurn is 0 (not known) until
#                                             this run proves +0x68 (its candidate, from the .buil record order)
TURN_WORD = (0x68 - 0x60) // 4      # index 2 of game_arrow_words (+0x60..)
SHOT_TOL = 0.90                     # pair.check_shot's default
# The shots: a few, not one per facing (the harness keeps a test at 8 or fewer, tests/test_ingame.py Suite): the minimap
# at two opposite facings (the arrow seen turned), the Map page at one; on each game = 6.
SHOT_AT = {"minimap": (0, 2), "page": (1,)}


def _diff(a, b):
    d = (a - b) % (2 * math.pi)
    return d - 2 * math.pi if d > math.pi else d


def others(st):
    return [p["number"] for p in (st.get("coop") or {}).get("players", []) if not p.get("self")]


def native(pair, x):
    mn = pair.state(x).get("map_native")
    assert mn is not None, "%s: map_native is null: the module showed no copy yet (no frame with another player on a map)" % x
    return mn


def shown_one(pair, x, view):
    mn = native(pair, x)
    assert mn.get("up"), "%s (%s): the copies are not up: %r (table %s)" % (x, view, mn.get("why"), mn.get("table"))
    sh = mn.get("shown") or []
    assert len(sh) == 1, "%s (%s): %d copies shown, not 1 (the one other player): %s" % (x, view, len(sh), sh)
    c = sh[0]
    assert c.get("name_set") and c.get("name"), "%s (%s): the label's name not set by the game's path setter: %s" % (x, view, c)
    assert c.get("wrote_turn") is not None, "%s (%s): no turn written (facing unknown or no N_PlayerRotate_00): %s" % (x, view, c)
    assert c.get("turn") is not None and abs(_diff(c["turn"], c["wrote_turn"])) < 1e-3, (
        "%s (%s): the arrow's turn read back %s, not the %s written (kPaneTurn %#x)" % (
            x, view, c.get("turn"), c["wrote_turn"], mn.get("turn_at", 0)))
    return c


def turn(pair, x, yaw):
    other = "B" if x == "A" else "A"
    pair.call(x, "move", yaw=yaw, strength=1.0, frames=TURN_FRAMES)
    pair.step_other(other, TURN_FRAMES)
    pair.step_both(SETTLE)


def probe_turn_word(pair):
    """A turns a quarter at a time; which of the game's own arrow's words follows (the old probe-3's question)."""
    rows = []
    for yaw in YAWS:
        turn(pair, "A", yaw)
        mn = native(pair, "A")
        rows.append(list(mn.get("game_arrow_words") or []))
    pair.notes.append("game arrow words +0x60.. per A yaw %s: %s" % (YAWS, [["%.3f" % w for w in r] for r in rows]))
    if not rows or len(rows[0]) <= TURN_WORD:
        return None
    follows = []
    for i in range(len(rows[0])):
        steps = [_diff(rows[k + 1][i], rows[k][i]) for k in range(len(rows) - 1)]
        if all(abs(abs(s) - QUARTER) <= QUARTER_TOL for s in steps):
            follows.append(i)
    pair.notes.append("words that turn a quarter per quarter: %s (kPaneTurn index %d)" % (follows, TURN_WORD))
    return follows


def copy_turns_with(pair, viewer, mover, view, tag):
    seen = []
    for k, yaw in enumerate(YAWS):
        turn(pair, mover, yaw)
        c = shown_one(pair, viewer, view)
        seen.append(c["turn"])
        if k in SHOT_AT[view]:
            pair.check_shot(viewer, "map-icons-%s-%d" % (tag, k), SHOT_TOL)
    steps = [_diff(seen[k + 1], seen[k]) for k in range(len(seen) - 1)]
    for k, s in enumerate(steps):
        assert abs(abs(s) - QUARTER) <= QUARTER_TOL, "%s on %s: %s turned a quarter but its arrow turned %.2f rad (%s)" % (
            view, viewer, mover, s, ["%.2f" % t for t in seen])
    assert len({round(s / QUARTER) for s in steps}) == 1, "%s on %s: the arrow turned back and forth (%s)" % (view, viewer, steps)
    pair.notes.append("%s on %s: turns %s" % (view, viewer, ["%.2f" % t for t in seen]))


@pytest.mark.budget(540)   # ESTIMATED: two warps + ~5'000 frames + 6 shots
@pytest.mark.look
@pytest.mark.needs_env(TURN_TRIAL)
@pytest.mark.row("G2b map icons (#255 item 8): the other player on the game's own map icons (arrow turned, name on the place label), minimap and Map page, on each game")
def test_map_icons_native(pair):
    with pair.setup():
        pair.warp_both(SCENE, ENTRANCE)
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
            assert s.get("scene") == SCENE, "%s in %s, not %s" % (x, s.get("scene"), SCENE)
            assert "map_native" in s, "%s: no map_native member (a module before map-icons-255)" % x
            pair.flag(x, MAP_FLAG, 1)
            assert pair.flag_read(x, MAP_FLAG).get("value"), "%s: %s not 1 after the setflag" % (x, MAP_FLAG)
        b = others(pair.state("A"))
        assert len(b) == 1, "A sees players %s, not one other" % b
    pair.step_both(60)                                   # hs-map-facing.A.txt 'setflag ...; wait 60; mark mini-1'
    follows = probe_turn_word(pair)
    copy_turns_with(pair, "A", "B", "minimap", "mini-A")
    copy_turns_with(pair, "B", "A", "minimap", "mini-B")
    pair.press("A", buttons=["MINUS"], frames=8)
    pair.step_both(OPEN_WAIT)
    copy_turns_with(pair, "A", "B", "page", "page-A")
    pair.press("A", buttons=["B"], frames=8)
    pair.step_both(CLOSE_WAIT)
    pair.press("B", buttons=["MINUS"], frames=8)        # (each game's Map page: user-facing shots on both)
    pair.step_both(OPEN_WAIT)
    copy_turns_with(pair, "B", "A", "page", "page-B")
    pair.press("B", buttons=["B"], frames=8)
    pair.step_both(CLOSE_WAIT)
    if follows is not None and follows and TURN_WORD not in follows:
        pytest.fail("the game's own arrow turns at word %s (+0x60 + 4 i), not kPaneTurn's %d: set kPaneTurn" % (follows, TURN_WORD))
    if not follows:
        pair.unproven("no word of the game's own arrow N_PlayerRotate_00 +0x60..+0x7c turned with A (notes): the "
                      "copies' turn is read back, but that the game draws it turned rests on the shots")
