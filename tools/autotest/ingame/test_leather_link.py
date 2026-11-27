"""leather-link (report 293, Snail parity P3): Link's Leather and Boots colour (part 28), on your own Link and on your
puppet in the other game; one owner per texture (it wins over the boots colour); unset is 0.11.12 byte for byte.

| Test | What it proves | Kind |
|---|---|---|
| test_leather_own_and_puppet | part 28 paints A's own Link and A's puppet in B | log + LOOK (colour + shots on each game) |
| test_leather_wins_over_boots | Leather set with a boots colour: the leather's colour, both parts on the wire | log + LOOK |
| test_leather_unset_is_01112 | Leather back to unset: the gear and puppet boots paintings are 0.11.12's again | log + LOOK |
| test_leather_without_character_select | the game without the mod paints its own Link, the other sees its puppet | log + LOOK; needs_env (no_cs.ENV) |

Set-up and judging, each cited where used:
- `menu look KEY=VALUE`: test only, gear-link's MENU verb (hookshot coop_pause.cpp TestMenu "look "): the part chosen
  as the Customize page chooses it and worn as when the menu closes, so it goes on the wire as a player's look. KEY is
  the part's key (hookshot/src/coop_look.cpp Parts(): `link_leather`, `boots`), VALUE a settings value (`RRGGBB`, no '#': the bridge refuses it in an act line, coop_playtest_core.cpp ParseRequest;
  CoopLook::ReadParts) or `none`.
- Own Link: the gear wardrobe's line `coop: look: your Link's gear is <name> now (...)` (hookshot/src/coop_look_tunic.cpp
  Wardrobe step, host.what "gear": coop_look_live.cpp); <name> is CoopLookParts::Name: `.leather-RRGGBB` last when
  part 28 is set, else 0.11.12's name.
- Puppet: `coop: look: the puppets' boots are <name> now` (coop_look_live.cpp FollowBoots); <name> is
  CoopLookParts::BootsPaintingName: `.<letter>[boots]L<leather>` per tunic colour.
- Colour: the share of magenta pixels in a bridge shot (MAGENTA: nothing in Kokiri Forest is magenta, so the count
  before is near 0) must rise on each game when A's leather is magenta; shots are also compared to their references.
- No Character Select: the ONE no-CS lane game, tools/autotest/ingame/no_cs.py: MARK (needs_env
  B:COOP_TEST_NO_CHARACTER_SELECT=1, harness-c132's switch; deselected and NOT-RUN without it) and prove() (B has no
  mod: flag_read of its saved Int, lichii/hook/coop_char_colours.cpp NoteOwnPick).
Not here: adult Link's gloves and upper leather (unit tests, tests/hookshot/look_test.cpp "leather-link") and solo
without Host/Join (the lanes are a co-op pair).
"""
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import no_cs        # noqa: E402  (the ONE no-Character-Select game)
from test_rows import need_child_in_play, place_in_front   # noqa: E402  (the shared set-up helpers)

MAGENTA, GOLD = "FF00FF", "FFD700"
OWN_GEAR = r"coop: look: your Link's gear is (gear\S*) now"
PUPPET_BOOTS = r"coop: look: the puppets' boots are (boots\S*) now"
# The share threshold is a starting value, NOT MEASURED (review-rows-c130-5 20:55 / 22:18, NO GUESSED VALUES): MORE is
# the least rise of the magenta share (and, unset, the most left). Until MEASURED cites a lane run's share lines (the
# "share <game> <what> before -> after" notes of each test), a run whose log checks all hold ends NO-REF
# (pair.unproven), never PASS, and a share off MORE is noted, not failed (as test_vest_link.py MEASURED).
MORE = 0.0005
MEASURED = None   # e.g. "run <name>: own A .. -> .., puppet B .. -> .., over-boots .., unset .."


def magenta_share(path):
    """Share of pixels near magenta (red and blue alike, green far under both), any brightness the shading gives."""
    from PIL import Image
    with Image.open(path) as im:
        px = list(im.convert("RGB").getdata())
    hits = sum(1 for r, g, b in px if r > 60 and b > 60 and g * 2 < min(r, b) and abs(r - b) < 0.35 * max(r, b))
    return hits / max(1, len(px))


def face_each_other(pair):
    place_in_front(pair, "A", of="B")
    pair.act("B", "face %.2f %.2f" % tuple(pair.state("A")["pos"][::2]))
    pair.step_both(10)


def note(pair, line):
    print(line)
    pair.notes.append(line)


def rise(pair, what, before, after, low, games=("A", "B")):
    """Each game's magenta share from `before` to `after`, noted; a rise under MORE goes to `low`."""
    for x in games:
        note(pair, "share %s %s %.5f -> %.5f" % (x, what, before[x], after[x]))
        if after[x] < before[x] + MORE:
            low.append("%s %s: magenta share %.5f -> %.5f (want a rise of %.4f)" % (x, what, before[x], after[x], MORE))


def judged(pair, low):
    """The share checks: judged only once MEASURED cites a lane run (else NO-REF, the low ones in its why)."""
    if not MEASURED:
        pair.unproven("the leather share threshold is not measured yet (MEASURED: the share lines of a lane run)%s"
                      % ("; low: " + "; ".join(low) if low else ""))
    assert not low, "; ".join(low)


def choose(pair, key, value, x="A"):
    r = pair.act(x, "menu look %s=%s" % (key, value))
    assert r.get("ok", True), "%s refused 'menu look %s=%s': %s" % (x, key, value, r)



def last(pair, x, pattern, pos):
    """The newest match's group 1 since `pos` in game x's log, or None."""
    found = re.findall(pattern, "\n".join(pair.log[x].since(pos)))
    return found[-1] if found else None


def shots(pair, name):
    """A shot of each game compared to its reference; the magenta share of each."""
    return {x: magenta_share(pair.check_shot(x, name, tolerance=0.80)["path"]) for x in ("A", "B")}


def reset(pair, x="A"):
    choose(pair, "link_leather", "none", x)
    choose(pair, "boots", "none", x)
    pair.step_both(30)


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("#293 P3 Link's Leather and Boots colour (own Link + puppet)")
def test_leather_own_and_puppet(pair):
    with pair.setup():
        need_child_in_play(pair)
        face_each_other(pair)
    try:
        before = shots(pair, "leather-link-before")
        mark_a, mark_b = pair.log["A"].mark(), pair.log["B"].mark()
        choose(pair, "link_leather", MAGENTA)
        pair.expect_log("A", OWN_GEAR.replace(r"(gear\S*)", r"gear\S*\.leather-" + MAGENTA), max_frames=240)
        pair.expect_log("B", PUPPET_BOOTS.replace(r"(boots\S*)", r"boots\S*L" + MAGENTA), max_frames=600)
        pair.step_both(30)
        after = shots(pair, "leather-link-magenta")
        low = []
        rise(pair, "A's leather (A own Link, B puppet)", before, after, low)
        assert last(pair, "A", OWN_GEAR, mark_a), "A: no gear painting line"
        assert last(pair, "B", PUPPET_BOOTS, mark_b), "B: no puppets' boots painting line"
        judged(pair, low)
    finally:
        reset(pair)


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("#293 P3 Leather wins over the boots colour (one owner)")
def test_leather_wins_over_boots(pair):
    with pair.setup():
        need_child_in_play(pair)
        face_each_other(pair)
    try:
        choose(pair, "boots", GOLD)
        pair.expect_log("B", PUPPET_BOOTS.replace(r"(boots\S*)", r"boots\S*" + GOLD), max_frames=600)
        pair.step_both(30)
        gold = {x: magenta_share(pair.check_shot(x, "leather-link-gold-boots", tolerance=0.80)["path"])
                for x in ("A", "B")}
        choose(pair, "link_leather", MAGENTA)
        # both on the wire (B's painting name has the boots and the leather), the leather's name on A's own gear
        pair.expect_log("B", PUPPET_BOOTS.replace(r"(boots\S*)", r"boots\S*%sL%s" % (GOLD, MAGENTA)), max_frames=600)
        pair.expect_log("A", OWN_GEAR.replace(r"(gear\S*)", r"gear\S*%s\S*\.leather-%s" % (GOLD, MAGENTA)),
                        max_frames=240)
        pair.step_both(30)
        both = shots(pair, "leather-link-over-boots")
        low = []
        rise(pair, "leather over gold boots", gold, both, low)
        judged(pair, low)
    finally:
        reset(pair)


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("#293 P3 Leather unset: as 0.11.12")
def test_leather_unset_is_01112(pair):
    with pair.setup():
        need_child_in_play(pair)
        face_each_other(pair)
    try:
        mark_a, mark_b = pair.log["A"].mark(), pair.log["B"].mark()
        choose(pair, "boots", GOLD)   # (a gear colour, so the gear painting is drawn and logged)
        # (the GOLD line since mark_a: a late line of the previous test's reset matches OWN_GEAR too; review 20:55 (2))
        pair.until("A's gear painting with the gold boots",
                   lambda: GOLD in (last(pair, "A", OWN_GEAR, mark_a) or ""), max_frames=240)
        pair.until("B's puppets' boots painting with the gold boots",
                   lambda: GOLD in (last(pair, "B", PUPPET_BOOTS, mark_b) or ""), max_frames=600)
        pair.step_both(30)
        gear_01112, boots_01112 = last(pair, "A", OWN_GEAR, mark_a), last(pair, "B", PUPPET_BOOTS, mark_b)
        assert gear_01112 and "leather" not in gear_01112, "A: a boots-only gear name with leather: %s" % gear_01112
        assert boots_01112 and "L" not in boots_01112[len("boots"):], "B: a boots-only puppet name with L: %s" % (
            boots_01112)
        choose(pair, "link_leather", MAGENTA)
        pair.expect_log("B", PUPPET_BOOTS.replace(r"(boots\S*)", r"boots\S*L" + MAGENTA), max_frames=600)
        pair.step_both(30)
        mark_a, mark_b = pair.log["A"].mark(), pair.log["B"].mark()
        choose(pair, "link_leather", "none")
        pair.until("A's gear painting is 0.11.12's again",
                   lambda: last(pair, "A", OWN_GEAR, mark_a) == gear_01112, max_frames=240)
        pair.until("B's puppets' boots painting is 0.11.12's again",
                   lambda: last(pair, "B", PUPPET_BOOTS, mark_b) == boots_01112, max_frames=600)
        pair.step_both(30)
        back = shots(pair, "leather-link-unset")
        low = []
        for x in ("A", "B"):
            note(pair, "share %s unset %.5f" % (x, back[x]))
            if back[x] >= MORE:
                low.append("%s: magenta left after the leather was unset (%.5f)" % (x, back[x]))
        judged(pair, low)
    finally:
        reset(pair)


@pytest.mark.budget(180)
@pytest.mark.look
@no_cs.MARK
@pytest.mark.row("#293 P3 Leather without Character Select (C #132)")
def test_leather_without_character_select(pair):
    with pair.setup():
        need_child_in_play(pair)
        no_cs.prove(pair)
        face_each_other(pair)
    x, y = no_cs.GAME, "A"   # the game without the mod chooses; the other (with it) sees its puppet
    try:
        before = shots(pair, "leather-link-no-cs-before")
        choose(pair, "link_leather", MAGENTA, x)
        pair.expect_log(x, OWN_GEAR.replace(r"(gear\S*)", r"gear\S*\.leather-" + MAGENTA), max_frames=240)
        pair.expect_log(y, PUPPET_BOOTS.replace(r"(boots\S*)", r"boots\S*L" + MAGENTA), max_frames=600)
        pair.step_both(30)
        after = shots(pair, "leather-link-no-cs-magenta")
        low = []
        rise(pair, "%s's leather, no Character Select" % x, before, after, low, games=(x, y))
        judged(pair, low)
    finally:
        reset(pair, x)
