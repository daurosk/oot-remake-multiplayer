"""The no-Character-Select lane game, ONE place for every test that needs it (C #132: gear-link, vest-link, leather-link,
hair-link use this, never their own copy).

The boot switch is harness-c132's (briefs/harness-c132.md item 3): `B:COOP_TEST_NO_CHARACTER_SELECT=1` boots lane game
B without the mod (lanes.py mod list / boot layer). Until a run's --env gives it, a test marked with `MARK` is
DESELECTED (bt_plugin needs_env, B #129) and the rc summary says NOT-RUN: NOT PROVEN, never PASS.
"""
import pytest

SWITCH = "COOP_TEST_NO_CHARACTER_SELECT"
GAME = "B"
ENV = "%s:%s=1" % (GAME, SWITCH)
MARK = pytest.mark.needs_env(ENV)
PICK_FLAG = "CharacterMenu_SelectedCharacter"   # the mod's saved Int (coop_char_colours.cpp NoteOwnPick: ModPresent)


def has_character_select(pair, x):
    """The module's ModPresent read through the bridge: the mod's pick Int is there (docs/playtest.md "flag_read":
    kind `unknown`, value null when the game has no such flag)."""
    r = pair.flag_read(x, PICK_FLAG)
    return r.get("kind") not in (None, "unknown") and r.get("value") is not None


def prove(pair):
    """Setup proof (inside pair.setup()): game B really has no Character Select, game A has it (so a test compares
    the two)."""
    assert not has_character_select(pair, GAME), "%s still has Character Select (%s set, the switch did not drop the " \
        "mod)" % (GAME, PICK_FLAG)
