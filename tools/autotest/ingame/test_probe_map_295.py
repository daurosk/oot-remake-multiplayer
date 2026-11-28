"""Probe (measures only) for gate-ui-epona-0121-4: why test_map_back_295 FAILED on gate-ui-182cd53b (2026-11-28 04:54 UTC).

A, co-op's main page in the - menu, L: the module logged 'the - menu: Map (L)' and '- menu's page gone', then no
'the Map page's content' line (FollowMapUnder, coop_pause_tab_module.cpp) and menu.map_page never up with
shown == panes in 90 frames, on both A tries (game-out-A.txt.2:4449, .1:4502). The test keeps no values on a
timeout. This probe writes the menu state (map_page, minus, coop) every 10 frames for 300 frames after L, and shoots
A's framebuffer at +30 and +240 (NO-REF = measured, the shots kept), so the cause is read from game state, not guessed.
"""
import json
import sys

import pytest

from test_map_back_295 import PRESS, SHUT_FRAMES
from test_menu_pages import open_page, page_shown

STEP, STEPS = 10, 30          # 300 frames = 10 s of game, past the test's 90-frame bound
SHOTS = {3: "probe295-map-l30", 24: "probe295-map-l240"}


def note(pair, line):
    pair.notes.append(line)
    print("probe-map-295: " + line, file=sys.stderr, flush=True)


def menu_now(pair, x):
    s = pair.state(x)
    m = s.get("menu") or {}
    return json.dumps({"map_page": m.get("map_page"), "minus": m.get("minus"), "coop": m.get("coop"),
                       "in_play": pair.in_play(s)}, sort_keys=True)[:600]


PROBE = "BT_PROBE_MAP_295=1"   # never in a default run (CI 46109945809: an always-NO-REF probe broke the Suite selftests)


@pytest.mark.budget(200)
@pytest.mark.needs_env(PROBE)
@pytest.mark.row("probe gate-ui-epona-0121-4: menu.map_page after L from co-op's main page (test_map_back_295) - "
                 "measures only")
def test_probe_map_back(pair):
    x = "A"
    with pair.setup():
        s = pair.state(x)
        assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
        assert not page_shown(s), "%s: co-op's page shows before the probe" % x
    note(pair, "before: %s" % menu_now(pair, x))
    open_page(pair, x, "main")
    note(pair, "co-op main page open: %s" % menu_now(pair, x))
    mark = pair.log[x].mark()
    pair.press(x, buttons=["L"], frames=PRESS)
    note(pair, "L +0: %s" % menu_now(pair, x))
    for i in range(1, STEPS + 1):
        pair.step_both(STEP)
        note(pair, "L +%d: %s" % (i * STEP, menu_now(pair, x)))
        if i in SHOTS:
            pair.check_shot(x, SHOTS[i], tolerance=0.80)
    lines = [ln.strip()[:200] for ln in pair.log[x].since(mark) if "pause tab" in ln]
    note(pair, "log since L (%d pause tab lines): %s" % (len(lines), " || ".join(lines[:40])))
    m = pair.state(x).get("menu") or {}
    if (m.get("minus") or {}).get("open") or (m.get("map_page") or {}).get("up"):   # (a menu still up: MINUS shuts it)
        pair.press(x, buttons=["MINUS"], frames=PRESS)
        pair.step_both(SHUT_FRAMES)
    note(pair, "end: %s" % menu_now(pair, x))
