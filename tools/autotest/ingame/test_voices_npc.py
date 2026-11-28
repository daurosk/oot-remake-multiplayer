"""Row 319 (the voices of playable NPCs on every game) as a bridge test.

| Test | Boot | Kind |
|---|---|---|
| test_voices_npc | default (Kakariko by the well) | B as Link, B as Malon, A as Saria (B still Malon), B then A Link again; the watcher hears the actor's sword swings |

Read from the code (voices-npc-319 report, section 1): a puppet's ActorInfo row names its own archive list
(tools/puppet_pack/build.py puppet_sound_list_for; the pack manifest's `sound_lists`), and the other player's sounds are
started on the puppet's own SLink sound user. Open risk this probe decides: neither SLink2 product has a user named
`Player_<Age>_V<NN>` or `CoopPuppet_<Age>`; if the row name also binds the sound user, every puppet has none and is silent.

Judged on the WATCHER's module log (the game that sees the actor's puppet), each phase:
 - the puppet's spawn line (coop_puppet.cpp:456 'P<n> <actor> spawned after ..., sound list <list>') names the list the
   watcher's own pack manifest gives that actor (coop_puppet.json sound_lists[actor]);
 - 'P<n> sound user 0x<nonzero> is '<name>'' (coop_puppet_sound.cpp LogPlaying), never 'P<n> has no sound user'
   (coop_link_sound.cpp:332);
 - >= 1 'P<n> sound '<asset>'' started on the puppet (coop_link_sound.cpp:131), and no 'P<n>: the sound '...' is not in
   its sound user' (coop_link_sound.cpp:418).
The actor's own 'our sound user 0x... is '<name>'' (coop_puppet.cpp, row 319) goes into the notes. The test ends NO-REF
(pair.unproven): which archive the engine loaded for the user is not read, so the VOICE itself is not proven here.

Every value is cited:
 - Malon (child): Character Select number 5, file 3; Saria: number 2, file 0 (lane1-A's coop_puppet.json characters.numbers
   {"0": 2, "3": 5}, variants Child + Adult for both; sound_lists CoopPuppet_Child_C000 = Player_Child_V02,
   CoopPuppet_Child_C003 = Player_Child_V05, CoopPuppet_Child_Green = CoopPuppet_Child: read 2026-11-28 01:0x UTC).
 - CHAR_FLAG, CHAR_WAIT, the spawn line and the Link / Malon actor patterns: test_puppet_run.py's.
 - The sword is Y, held with ZL: test_boss_hit_react.py ('pair.press("B", ["ZL", "Y"], frames=SLASH_HOLD)').
 - The hearing distance: coop_link_sound.h kHearDistance 30 (game units, about metres); the watcher is placed 2 m from
   the actor (bridge `place`, metres: docs/playtest.md).
 - The switch: LICHII_COOP_DEBUG_VOICE=1 (coop_puppet_config.cpp, row 319) turns on every line above.
"""
import json
import math
import os
import re
import sys

import pytest

from test_gear import GATE, SCENE_CHILD   # noqa: E402  (the boot's place: Kakariko by the well)

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import hookshot   # noqa: E402  (pack_folders)

SWITCH = "LICHII_COOP_DEBUG_VOICE=1"
CHAR_FLAG = "CharacterMenu_SelectedCharacter"   # test_levitate.CHAR_FLAG
LINK, SARIA, MALON = 0, 2, 5                     # characters.json / the pack's characters.numbers
CHAR_WAIT = 900                                  # test_levitate.CHAR_WAIT (frames after the setflag)
SPAWN = r"coop: puppet: P(\d+) (CoopPuppet_\w+) spawned after .*?(?:, sound list (\S+))?$"   # coop_puppet.cpp:456
LINK_RX = r"CoopPuppet_Child(?!(?:Quiet)?_C\d{3})\w*"     # test_puppet_run (a Link / colour puppet)
MALON_RX = r"CoopPuppet_Child(?:Quiet)?_C003"
SARIA_RX = r"CoopPuppet_Child(?:Quiet)?_C000"
SWINGS, HOLD, AFTER = 5, 8, 40                   # 5 swings of 8 frames, 40 frames after each
NEAR = 2.0                                       # metres beside the actor (kHearDistance 30)


def manifest_lists(pair, x):
    """Game x's pack manifest: sound_lists (actor -> archive list)."""
    _, pack, _ = hookshot.pack_folders(pair.games[x]["folder"])
    f = os.path.join(pack, "coop_puppet.json")
    with open(f, encoding="utf-8") as fh:
        j = json.load(fh)
    nums = (j.get("characters") or {}).get("numbers") or {}
    assert nums.get("0") == SARIA and nums.get("3") == MALON, (
        "%s's pack %s: characters.numbers %s, not Saria 2 at file 0 and Malon 5 at file 3" % (x, f, nums))
    lists = j.get("sound_lists") or {}
    assert lists, "%s's pack %s has no sound_lists (a pack made before row 319's lists)" % (x, f)
    return lists


def last_spawn(pair, w, want, at=0):
    """The last spawn line on watcher w since `at` whose actor matches `want`: (n, actor, list, line index)."""
    rx, wx = re.compile(SPAWN), re.compile(want)
    found = None
    for i, ln in enumerate(pair.log[w].since(at)):
        m = rx.search(ln.rstrip())
        if m and wx.fullmatch(m.group(2)):
            found = (m.group(1), m.group(2), m.group(3), i)
    return found


def wait_spawn(pair, w, at, want, what):
    return pair.until("%s: the other player's puppet made again as %s ('%s')" % (w, what, want),
                      lambda: last_spawn(pair, w, want, at), max_frames=CHAR_WAIT)


def near(pair, w, x):
    """The watcher w placed NEAR metres from x and facing it."""
    p = pair.state(x)["pos"]
    pair.act(w, "place %.2f %.2f %.2f" % (p[0] + NEAR, p[1], p[2]))
    pair.step_both(2)
    q = pair.state(w)["pos"]
    d = math.dist((p[0], p[2]), (q[0], q[2]))
    assert d < 10.0, "%s is %.1f m from %s after the place (not within hearing)" % (w, d, x)
    pair.act(w, "face %.2f %.2f" % (p[0], p[2]))


def swing(pair, x):
    for _ in range(SWINGS):
        pair.press(x, ["ZL", "Y"], frames=HOLD)
        pair.step_both(AFTER)


def judge(pair, tag, w, x, want, lists, at):
    """The watcher's lines about its puppet of x since the spawn line; [] or the faults."""
    sp = last_spawn(pair, w, want)
    assert sp, "%s: %s's log has no spawn line for %s's puppet ('%s')" % (tag, w, x, want)
    n, actor, got_list, idx = sp
    lines = pair.log[w].since(0)[idx:]
    p = "coop: puppet: P%s " % n
    users = [ln for ln in lines if re.search(r"P%s sound user (0x[0-9a-f]+) is '" % n, ln)]
    none = [ln for ln in lines if p + "has no sound user" in ln]
    phase = pair.log[w].since(at)
    started = [ln for ln in phase if re.search(r"P%s sound '[^']+'" % n, ln)]
    missing = [ln for ln in phase if re.search(r"P%s: the sound '[^']+' is not in its sound user" % n, ln)]
    ours = [ln for ln in pair.log[x].since(0) if "coop: puppet: our sound user " in ln]
    pair.notes.append("%s: %s's puppet of %s = %s, spawn sound list %s (manifest %s)" % (
        tag, w, x, actor, got_list, lists.get(actor)))
    pair.notes.append("%s: %s sound user lines: %s" % (tag, w, " | ".join(users[-2:]) or "none"))
    pair.notes.append("%s: %s's own: %s" % (tag, x, " | ".join(ours[-1:]) or "none"))
    pair.notes.append("%s: %s started %d sounds on P%s (%s), %d not in its sound user" % (
        tag, w, len(started), n, " | ".join(started[:3]), len(missing)))
    bad = []
    if got_list != lists.get(actor):
        bad.append("%s: spawn line's sound list is %s, the manifest's for %s is %s" % (tag, got_list, actor,
                                                                                     lists.get(actor)))
    zero = [ln for ln in users if re.search(r"sound user 0x0 is", ln)]
    if none or not users or (users and users[-1] in zero):
        bad.append("%s: %s's puppet P%s has no sound user (%s)" % (tag, w, n, (none or zero or ["no user line"])[-1]))
    if not started:
        bad.append("%s: %s started no sound on P%s during %d swings of %s" % (tag, w, n, SWINGS, x))
    if missing:
        bad.append("%s: %d sounds of %s not in P%s's sound user (first: %s)" % (tag, len(missing), x, n, missing[0]))
    return bad


def set_char(pair, x, value):
    pair.flag(x, CHAR_FLAG, value)
    got = pair.flag_read(x, CHAR_FLAG).get("value")
    assert got == value, "%s: %s is %s after the setflag, not %d" % (x, CHAR_FLAG, got, value)


@pytest.mark.budget(480)   # ESTIMATED: 5 phases x ~250 frames of swings + up to 4 puppet rebuilds (<= 900 frames each)
@pytest.mark.needs_env(SWITCH)
@pytest.mark.row("#319 a playable NPC's puppet plays the other player's sounds on its own sound user (voice lists)")
def test_voices_npc(pair):
    was, done, faults = {}, False, []
    try:
        with pair.setup():
            lists = {x: manifest_lists(pair, x) for x in ("A", "B")}
            pair.warp_both(SCENE_CHILD, entrance=GATE)   # (inside the set-up: C #145; the boot's own place)
            for x in ("A", "B"):
                s = pair.state(x)
                assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
                assert (s.get("scene") or "") == SCENE_CHILD, "%s in %s, not %s" % (x, s.get("scene"), SCENE_CHILD)
                assert s.get("age") == "child", "%s is %s, not child (the child puppets' lists)" % (x, s.get("age"))
                was[x] = pair.flag_read(x, CHAR_FLAG).get("value")
            for x, w in (("B", "A"), ("A", "B")):
                if was[x] != LINK:
                    at = pair.log[w].mark()
                    set_char(pair, x, LINK)
                    wait_spawn(pair, w, at, LINK_RX, "Link")
        # (tag, who changes, to what, the watcher, the actor that swings, the watcher's puppet pattern)
        for tag, x, value, w, want in (("link", None, None, "A", LINK_RX),
                                       ("malon", "B", MALON, "A", MALON_RX),
                                       ("saria", "A", SARIA, "B", SARIA_RX),
                                       ("link-again", "B", LINK, "A", LINK_RX),
                                       ("link-again-a", "A", LINK, "B", LINK_RX)):
            actor = "B" if w == "A" else "A"
            at = pair.log[w].mark()
            if x is not None:
                set_char(pair, x, value)
                wait_spawn(pair, w, at, want, tag)
            near(pair, w, actor)
            at = pair.log[w].mark()
            swing(pair, actor)
            faults += judge(pair, tag, w, actor, want, lists[w], at)
        done = True
        assert not faults, "; ".join(faults)
        pair.unproven("the puppets' sound users, lists and started sounds are right (notes); which archive the engine "
                      "loaded for each user (the voice itself) is not read")
    finally:
        try:
            for x in ("A", "B"):
                if was.get(x) is not None:
                    pair.flag(x, CHAR_FLAG, was[x])
            pair.warp_both(SCENE_CHILD, entrance=GATE)
        except Exception as e:
            if done:
                raise
            pair.notes.append("way back after the failure: %s: %s" % (type(e).__name__, e))
