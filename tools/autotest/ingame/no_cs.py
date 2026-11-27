"""The no-Character-Select lane game, ONE place for every test that needs it (C #132: gear-link, vest-link, leather-link,
hair-link use this, never their own copy).

The boot switch is harness-c132's (briefs/harness-c132.md item 3): `B:COOP_TEST_NO_CHARACTER_SELECT=1` boots lane game
B without the mod (lanes.py mod list / boot layer). Until a run's --env gives it, a test marked with `MARK` is
DESELECTED (bt_plugin needs_env, B #129) and the rc summary says NOT-RUN: NOT PROVEN, never PASS.
"""
import re

import pytest

import pair as P   # the ONE CS_MOD (pair.CS_MOD); no_cs sits next to pair.py

SWITCH = "COOP_TEST_NO_CHARACTER_SELECT"
GAME = "B"
ENV = "%s:%s=1" % (GAME, SWITCH)
MARK = pytest.mark.needs_env(ENV)
PICK_FLAG = "CharacterMenu_SelectedCharacter"   # the pick's saved Int: NOT a sign of the mod (see has_character_select)
MOD_LINE = r"\] mods: %s: " % re.escape(P.CS_MOD)   # '[hos host] mods: OoT Character Select: 6903 RomFS files ...' (one per boot)
MODS_DONE = r"\] mods: \d+ mods from "   # '[hos host] mods: 2 mods from <folder>\mods: ...': the boot's mod list ends


def has_character_select(pair, x):
    """Did game x's LAST boot load the Character Select mod? Read from the host's own boot lines in x's log: the block
    up to the last 'mods: N mods from' line holds 'mods: OoT Character Select: ...' only when the mod was loaded.
    NOT the pick Int (PICK_FLAG): co-op's own Characters tab (tools/puppet_pack/charsel, z02.cdelta) adds that Int to
    the pack's GameDataList, so it is there without the mod too (hair-link-7, leather-nocs-309d7180: B booted
    lane1-B-nocs, 1 mod, coop-puppet's GameDataList has Int 0xd6b580ea, the vanilla list has none)."""
    lines = pair.log[x].since(0)
    done = [i for i, ln in enumerate(lines) if re.search(MODS_DONE, ln)]
    assert done, "%s: no '%s' line in its log: its mod list is not known" % (x, MODS_DONE)
    start = done[-2] + 1 if len(done) > 1 else 0
    return any(re.search(MOD_LINE, ln) for ln in lines[start:done[-1] + 1])


def prove(pair):
    """Setup proof (inside pair.setup()): game B really has no Character Select, game A has it (so a test compares
    the two)."""
    assert not has_character_select(pair, GAME), (
        "%s still has Character Select ('%s' in its boot's mod list, the switch did not drop the mod)" % (GAME, MOD_LINE))
    assert has_character_select(pair, "A"), (
        "A has no Character Select ('%s' not in its boot's mod list): nothing to compare B with" % MOD_LINE)
