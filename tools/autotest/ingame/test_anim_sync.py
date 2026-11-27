"""Row 310 (anim-sync-310): the other Link's effects on his puppet, frame-true (docs/puppet.md "Link effects").

B charges a spin with the sword held, lets go (the spin), and A sees the same effects on B's puppet: every key B's game
started on its Link's ELink user and sent as LINK_EFFECT (501) is started on the puppet's own ELink user in A's game,
and the held charge's key is stopped on A when B's game no longer plays it (the end).

| Test | Proves |
|---|---|
| test_spin_charge_effects_reach_the_puppet | B sent at least one effect start; A started every key B sent on the puppet (none missing from its user); each end B sent was played on A for a key A started; shots of A's view of B (and B's own view) 3 times during the charge and 3 times after the spin, a few frames apart (NO-REF until owner-approved refs) |

Results: FAIL = B sent no effect (the catch does not work), A did not start a key B sent, a key is not in the puppet's
effect user, or an end did not stop it on A. SETUP-FAIL = the puppet has no effect user (a pack built without row
310's ELinkRef), or the Links are not side by side. NO-REF = the shots have no refs yet.

NOT covered here (said in reports/anim-sync-310-2.md): Din's Fire / Nayru's Love / Farore's Wind (no cited `give`
names for the magic in docs/playtest.md yet: a probe first), the great spin (adult save).

Sources (worker-rules "no guessed values"):
- Switches: LICHII_COOP_DEBUG_SOUND (hookshot/src/coop_hookshot.cpp CoopLinkSound::SetProbe) on B: the probe names
  each user's emits, and CoopLinkSound::Frame logs every effect sent; LICHII_COOP_DEBUG_ANIM
  (lichii/hook/coop_puppet_config.cpp settings.debug_anim, the director's anim_) on A: the effect Player says each
  start and end (CoopLinkSound::Emit / Next).
- Lines: B 'coop: link effect '<key>'[ end] sent (<n> sent)' (coop_link_sound.cpp Frame); A 'coop: puppet: P<n> effect
  '<key>'' (Emit), '... effect '<key>' ended' (Next), 'coop: puppet: P<n>: the effect '<key>' is not in its effect
  user' (Finish), 'coop: puppet: P<n> has no effect user' (Next; said without the debug switch too).
- Inputs: the sword is Y (test_boss_hit_react: the scenario's 'hold ZL+Y 8'); a held sword button charges the spin and
  letting go spins (the game's own; this test is the probe: no start sent fails it, with the probe lines in the notes).
- Place and camera: test_tunic.stand_apart (imported, never copied); need_child_in_play (test_rows).
- A's pack: coop_puppet.json beside the pack (hookshot.pack_folders, as test_levitate.pack_anims) has "effects": true
  (tools/puppet_pack/build.py manifest, row 310: the puppet keeps ELinkRef); the lane prep makes the pack with the tested
  sha's own pack tool (offline/lane-prep-c143.ps1, lib/lane-pack.ps1). Missing or false: SETUP-FAIL.
- B's sword: the state's equips.weapon (docs/playtest.md state shape, e.g. "Weapon_Sword_Kokiri" =
  game_names.KOKIRI_SWORD); any Weapon_Sword_* charges. None or another weapon: SETUP-FAIL (a save fault, not the row).
- Ends: the sender's end comes kEndAfterMs (300 ms) after a start at the earliest, read every kTrackEveryMs (100 ms)
  (lichii/hook/coop_link_sound.h); SETTLE's 120 frames (2 s) leave room. Which key is the held loop is the probe's to
  show, so a run where B sent no end at all is NO-REF ('no end sent'), never PASS.
"""
import json
import os
import re
import sys

import pytest

import pair as P
from test_rows import need_child_in_play
from test_tunic import stand_apart

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import hookshot   # noqa: E402  (pack_folders: where a game's co-op pack is, as the kit finds it; as test_levitate)

PROBE = "B:LICHII_COOP_DEBUG_SOUND=1"
ANIM = "A:LICHII_COOP_DEBUG_ANIM=1"
SENT = r"coop: link effect '([A-Za-z0-9_]+)'( end)? sent"                  # coop_link_sound.cpp Frame
STARTED = r"coop: puppet: P\d+ effect '([A-Za-z0-9_]+)'(?! ended)"           # Emit (debug)
ENDED = r"coop: puppet: P\d+ effect '([A-Za-z0-9_]+)' ended"                # Next (debug)
MISSING = r"coop: puppet: P\d+: the effect '([A-Za-z0-9_]+)' is not in its effect user"   # Finish (debug)
NO_USER = r"coop: puppet: P\d+ has no effect user"                         # Next
PROBE_EFFECTS = r"coop: sound probe: user 0x[0-9a-f]+ \(our Link's effects\)"   # OnEmit (probe)

CHARGE_FRAMES = 90        # the sword held: the charge (brief: "B holds ~90 frames")
SHOT_EVERY = 6            # frames between two shots of one moment (a few frames apart: the animation shows)
SHOTS = 3                 # per moment, per game
SETTLE = 120              # after the spin: the ends come (TrackOurEffects every 100 ms, kEndAfterMs 300) and relay


def need_effects_pack(pair, x):
    """SETUP-FAIL unless game x's co-op pack was built with the puppets' ELink user (manifest "effects": true)."""
    _, pack, _ = hookshot.pack_folders(pair.games[x]["folder"])
    f = os.path.join(pack, "coop_puppet.json")
    try:
        with open(f, encoding="utf-8") as fh:
            effects = json.load(fh).get("effects")
    except (OSError, ValueError) as e:
        raise P.SetupFail("%s: no readable co-op pack manifest %s (%s)" % (x, f, e))
    if effects is not True:
        raise P.SetupFail("%s's pack (%s) has effects %r: built without row 310's ELinkRef (make it from this sha)"
                          % (x, f, effects))


def need_sword(pair, x):
    """SETUP-FAIL unless game x's Link wears a sword (equips.weapon Weapon_Sword_*)."""
    weapon = ((pair.state(x).get("equips") or {}).get("weapon")) or ""
    if not weapon.startswith("Weapon_Sword_"):
        raise P.SetupFail("%s wears no sword (equips.weapon %r): the spin charge needs one" % (x, weapon or None))


def keys(lines, pattern):
    """Group 1 of each line matching `pattern`, in order."""
    return [m.group(1) for ln in lines for m in [re.search(pattern, ln)] if m]


def sent_keys(lines):
    """B's sent lines as (key, is_end), in order."""
    return [(m.group(1), bool(m.group(2))) for ln in lines for m in [re.search(SENT, ln)] if m]


def shots(pair, moment, hold=None):
    """SHOTS shots of both games SHOT_EVERY frames apart; with `hold`, B keeps those buttons held in between (two
    bridge inputs with no frame between them: the game never sees the button let go)."""
    for i in range(1, SHOTS + 1):
        for x in ("A", "B"):
            pair.check_shot(x, "anim-sync-310-%s-%02d" % (moment, i), tolerance=0.80)   # moving Links: as tunic 0.80
        if hold:
            pair.press("B", hold, frames=SHOT_EVERY)
        else:
            pair.step_both(SHOT_EVERY)


@pytest.mark.budget(240)
@pytest.mark.look
@pytest.mark.needs_env(PROBE, ANIM)
@pytest.mark.row("#310 the other Link's effects on his puppet (spin charge, spin)")
def test_spin_charge_effects_reach_the_puppet(pair):
    with pair.setup():
        need_child_in_play(pair)
        stand_apart(pair)
        need_sword(pair, "B")
        need_effects_pack(pair, "A")
    start = {x: pair.log[x].size() for x in ("A", "B")}
    # The charge: Y held (the sword), shots part way in and near its end; then letting go spins.
    pair.press("B", ["Y"], frames=CHARGE_FRAMES - SHOTS * SHOT_EVERY)
    shots(pair, "charge", hold=["Y"])
    shots(pair, "spin")            # the first frame without Y: let go, the spin
    pair.step_both(SETTLE)

    a = pair.log["A"].since(start["A"])
    b = pair.log["B"].since(start["B"])
    if any(re.search(NO_USER, ln) for ln in a):
        raise P.SetupFail("A: '%s': the puppet pack has no ELinkRef (build it from this sha: manifest effects true)"
                          % next(ln for ln in a if re.search(NO_USER, ln)))
    sent = sent_keys(b)
    starts = [k for k, end in sent if not end]
    ends = [k for k, end in sent if end]
    pair.notes.append("B sent starts %s ends %s" % (starts, ends))
    pair.notes.append("B probe (effects user): %s" % [ln for ln in b if re.search(PROBE_EFFECTS, ln)][:12])
    pair.notes.append("A started %s ended %s missing %s" % (keys(a, STARTED), keys(a, ENDED), keys(a, MISSING)))
    assert starts, "B sent no LINK_EFFECT start for the spin charge and spin (the catch on our Link's ELink user)"
    missing = sorted(set(keys(a, MISSING)))
    assert not missing, "A: key(s) %s are not in the puppet's effect user" % missing
    not_started = sorted(set(starts) - set(keys(a, STARTED)))
    assert not not_started, "A never started %s of B's keys on the puppet (B sent %s)" % (not_started, starts)
    not_ended = sorted((set(ends) & set(keys(a, STARTED))) - set(keys(a, ENDED)))
    assert not not_ended, "A did not stop %s on the puppet after B's end" % not_ended
    if not ends:
        pair.unproven("no end sent: B's game sent no LINK_EFFECT end in %d frames after the spin (starts %s)"
                      % (SETTLE, starts))
