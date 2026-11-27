"""The short gate's kit smoke on the PACKAGED kit (needs_kit: runs only with --kit-ls, the kit pair the gate built from
the kit zip; the dev lanes are not what players install). Replaces the old groups.py smoke of
coord-multiplayer/offline/testkit-01113.ps1 ('[rc7]' hs-players + hs-puppets):

| Old check (testkit-01113.ps1 smoke group) | Here |
|---|---|
| A + B 'coop: connected' | test_smoke.py test_connect (same line + both games list two connected players) |
| A "coop: drawing over Hookshot's picture" | test_kit_overlay_game_look: the line on A AND B, in the game look (not the 'plain look' fallback of a game built without our patch) |
| A 'coop: mods: oot-remake-randomizer .*: on' | test_kit_randomizer_on: the mod list line on A, its exact form cited below |
| B P1 / A P2 CoopPuppet spawned | test_rows.py test_puppets_spawn (same lines + each game's world list) |
| (none: no screenshot, so every run was UNPROVEN) | the smoke tier's pause shots + a bridge shot of each game here, compared to refs |

Run (the gate's kit pair laid out as KIT_LS/lane1-A/-B, lane-server.txt by lane_record.py):
  pytest tools/autotest/ingame --lanes 1 --kit-ls KIT_LS --expect-commit SHA -k "test_smoke or test_puppets_spawn or test_kit"

Sources:
- the overlay lines: hookshot/src/coop_hookshot.cpp DrawGameLook ("coop: drawing over Hookshot's picture: chat lines,
  name tags (WxH)" + optional "; no turned pictures") and the fallback ("...); plain look: this Hookshot has no
  hos_coop_draw2"), each said once per game.
- the mod line: hookshot/src/coop_mods.cpp Loader (line = name + " (" + id + " " + version + "): " + "on"/"off"), with
  the "coop: mods: " prefix (coop_mods_runtime.cpp hooks); the mod folder and id 'oot-remake-randomizer' are the kit's
  (tools/release/kit/scripts/kit-paths.ps1 '$mods\\oot-remake-randomizer'); 0.11.13's gate log:
  "coop: mods: oot-remake-randomizer (oot-remake-randomizer " (offline/testkit-01113/result.txt).
"""
import pytest

from test_rows import need_child_in_play

pytestmark = pytest.mark.needs_kit

OVERLAY = r"coop: drawing over Hookshot's picture: chat lines, name tags \(\d+x\d+\)"
PLAIN_LOOK = r"coop: drawing over Hookshot's picture: .*plain look"
RANDO_ON = r"coop: mods: oot-remake-randomizer \(oot-remake-randomizer \S+\): on"


@pytest.mark.budget(120)
@pytest.mark.look
@pytest.mark.row("kit smoke: co-op overlay in the game look on each game (hs-players overlay)")
def test_kit_overlay_game_look(pair):
    with pair.setup():
        need_child_in_play(pair)
    for x in ("A", "B"):
        line = pair.log[x].find(OVERLAY, pos=0)
        assert line, "%s: no '%s' since the start (the overlay never drew)" % (x, OVERLAY)
        assert not pair.log[x].find(PLAIN_LOOK, pos=0), (
            "%s draws the plain look: the kit's game misses our hos_coop_draw2 patch: %s" % (x, line))
    for x in ("A", "B"):
        pair.check_shot(x, "kit-in-play")


@pytest.mark.budget(60)
@pytest.mark.row("kit smoke: the kit's randomizer mod on (hs-players randomizer)")
def test_kit_randomizer_on(pair):
    with pair.setup():
        need_child_in_play(pair)
    assert pair.log["A"].find(RANDO_ON, pos=0), "A: no '%s' since the start (the kit's randomizer is not on)" % RANDO_ON
    if not pair.log["B"].find(RANDO_ON, pos=0):
        pair.notes.append("B: no randomizer-on line (the old smoke judged A only)")
