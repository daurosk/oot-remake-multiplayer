"""G2a (the map arrow regression, #124 C-list) as a bridge test: the port of coord offline/regress-fps-map-run.ps1's
scenario hs-map-facing (scenarios/hs-map-facing.A/B.txt): the other player on A's maps as the game's own map arrow,
turned the way they face, on the HUD minimap and on the Map page.

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_map_arrow_turns | hs-map-facing (regress-fps-map) | default + --pack hud | B's arrow on A's minimap, then on A's Map page, turns a quarter with each quarter turn of B (state map_icons), window shots of A |

The arrows and names are drawn through Longshot's overlay (hookshot/src/coop_draw.cpp DrawMap) and a bridge shot leaves
the overlays out (docs/playtest.md "map_icons"), so the test judges by the state's `map_icons` (coop_map_drawn.h: what
DrawMap drew that present) and the picture proof is a WINDOW shot with the overlay (pair.window_shot: harness-c133 item
8, C #133 12:0x). Until that helper is in the harness the test ends NO-REF after its state checks (never PASS on state
alone: user-facing).

Set-up as the old scenario: both games 'warp InsideTheDekuTree Entrance', A 'setflag
DungeonInfo.InsideTheDekuTree.IsGetMap 1' (the minimap and the Map page need the dungeon's map), A presses MINUS for the
pause menu, which opens on the Map page there (hs-map-facing.A.txt: 'no R: shot ... shows the Map tab chosen at once'),
B turns a quarter at a time (old: a 20-tick stick push; here the bridge's `move` toward a world yaw for the same 20
frames, so each facing is known). The arrow pictures come from the tested sha's hud.py through --pack hud
(packs/hud.py, as regress-fps-map-run.ps1:91 made them by hand); A's log line 'map arrows loaded' is the set-up proof.
"""
import math

import pytest

SCENE, ENTRANCE = "InsideTheDekuTree", "Entrance"     # hs-map-facing.A/B.txt 'warp InsideTheDekuTree Entrance'
MAP_FLAG = "DungeonInfo.InsideTheDekuTree.IsGetMap"  # hs-map-facing.A.txt 'setflag DungeonInfo.InsideTheDekuTree.IsGetMap 1'
LOADED = r"coop: hud: game look on Hookshot: .*map arrows loaded"   # hookshot/src/coop_hookshot.cpp:1075 (LoadLook)
NO_TURN = r"no turned pictures"     # regress-fps-map-run.ps1: A ran a Hookshot without hos_coop_draw3 (patch 0001)
YAWS = (0, 90, 180, 270)            # B's facings, world yaw degrees (docs/playtest.md `move`: 0 toward +z, 90 toward +x)
TURN_FRAMES = 20                    # hs-map-facing.B.txt: 'a short stick push of 20 ticks each'
SETTLE = 120                        # ESTIMATED: frames for B's new facing to reach A and A's next present to draw it
OPEN_WAIT = 180                     # hs-map-facing.A.txt 'press MINUS; wait 180; mark panel'
CLOSE_WAIT = 240                    # hs-map-facing.A.txt 'press B; wait 240; mark end'
QUARTER_TOL = 30.0                  # degrees: a quarter turn of B turns the arrow 90 +- 30 (draw_test.cpp: a quarter
#                                     of the player turns it a quarter, exactly, at a known rotation; the game's facing
#                                     after a 20-frame walk is the walk's way within a few degrees)


def _diff(a, b):
    """Signed difference a - b of two angles in degrees, in (-180, 180]."""
    d = (a - b) % 360.0
    return d - 360.0 if d > 180.0 else d


def others(st):
    return [p["number"] for p in (st.get("coop") or {}).get("players", []) if not p.get("self")]


def icon_of(pair, x, number, view):
    """The map icon of player `number` that game x drew in its last present, on `view` ('minimap' or 'page')."""
    mi = pair.state(x).get("map_icons")
    assert mi is not None, "%s: no map_icons in the state (a module before coop_map_drawn.h)" % x
    assert mi.get("view") == view, "%s: map_icons view %r, not %r (icons %s)" % (x, mi.get("view"), view, mi.get("icons"))
    mine = [i for i in mi.get("icons", []) if i.get("number") == number]
    assert mine, "%s: P%d not on the %s (icons %s)" % (x, number, view, mi.get("icons"))
    i = mine[0]
    assert i.get("arrow"), "%s: P%d on the %s without the game's map arrow (name alone): %s" % (x, number, view, i)
    assert i.get("turn") is not None, "%s: P%d arrow with no turn: %s" % (x, number, i)
    return i


def window_shots(pair, name):
    """A's window with the overlay (the arrows are drawn there), via the harness helper (harness-c133 item 8)."""
    shot = getattr(pair, "window_shot", None)
    if shot is None:
        return False
    shot("A", name)
    return True


def turns_with_b(pair, number, view, tag):
    """B faces each of YAWS; A's arrow of B turns by B's turn each time (either way round: the map's handedness is
    the picture's). Returns whether window shots were taken."""
    seen, shot = [], True
    for k, yaw in enumerate(YAWS):
        pair.call("B", "move", yaw=yaw, strength=1.0, frames=TURN_FRAMES)
        pair.step_other("A", TURN_FRAMES)             # (the move ran B only: A to the same frame)
        pair.step_both(SETTLE)
        i = icon_of(pair, "A", number, view)
        seen.append(math.degrees(float(i["turn"])))
        shot = window_shots(pair, "map-arrow-%s-%d" % (tag, k)) and shot
    steps = [_diff(seen[k + 1], seen[k]) for k in range(len(seen) - 1)]
    for k, s in enumerate(steps):
        assert abs(abs(s) - 90.0) <= QUARTER_TOL, (
            "%s: B turned a quarter (yaw %d -> %d) but its arrow turned %.1f deg (turns %s)" % (
                view, YAWS[k], YAWS[k + 1], s, ["%.1f" % t for t in seen]))
    assert len({round(s / 90.0) for s in steps}) == 1, "%s: the arrow turned back and forth (steps %s)" % (view, steps)
    pair.notes.append("%s turns %s" % (view, ["%.1f" % t for t in seen]))
    return shot


@pytest.mark.budget(300)   # ESTIMATED: two warps + ~2'000 frames + shots
@pytest.mark.look
@pytest.mark.needs_pack("hud")
@pytest.mark.row("G2a map arrow (#124 C-list): the other player on the minimap and the Map page as the game's arrow, turned the way they face")
def test_map_arrow_turns(pair):
    with pair.setup():
        pair.warp_both(SCENE, ENTRANCE)
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
            assert s.get("scene") == SCENE, "%s in %s, not %s" % (x, s.get("scene"), SCENE)
        pair.flag("A", MAP_FLAG, 1)
        assert pair.flag_read("A", MAP_FLAG).get("value"), "A: %s not 1 after the setflag" % MAP_FLAG
        assert pair.log["A"].find(LOADED, pos=0), "A did not load the map arrows (no '%s': the pack's coop_hud)" % LOADED
        assert not pair.log["A"].find(NO_TURN, pos=0), "A's Hookshot cannot turn pictures (not patch 0001's draw3)"
        b = others(pair.state("A"))
        assert len(b) == 1, "A sees players %s, not one other" % b
    number = b[0]
    pair.step_both(60)                                   # hs-map-facing.A.txt 'setflag ...; wait 60; mark mini-1'
    shot = turns_with_b(pair, number, "minimap", "mini")
    pair.press("A", buttons=["MINUS"], frames=8)
    pair.step_both(OPEN_WAIT)
    shot = turns_with_b(pair, number, "page", "page") and shot
    pair.press("A", buttons=["B"], frames=8)
    pair.step_both(CLOSE_WAIT)
    mi = pair.state("A").get("map_icons") or {}
    pair.notes.append("after B: view %s" % mi.get("view"))
    if not shot:
        pair.unproven("arrows judged by state only: no window shot helper in this harness (harness-c133 item 8); "
                      "the arrows are overlay, a bridge shot has none")
