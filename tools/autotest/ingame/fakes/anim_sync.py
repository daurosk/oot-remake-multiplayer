"""Fake-game reactions for row 310 (ingame/test_anim_sync.py; owner anim-sync-310-2). Lines are the module's, in the
formats test_anim_sync.py cites (coop_link_sound.cpp Frame / Emit / Next / Finish).

Scoped to the row (B #149): only when B's boot has LICHII_COOP_DEBUG_SOUND=1 (the test's own needs_env), only B's
Y press reacts, and no state key is written.
- B holding Y (the sword): B logs the charge key sent once; A logs P2 starting it on the puppet 4 frames later.
- B letting go (the first Y-less input or step after the hold): B logs the spin key sent and, 20 frames on, the
  charge key's end; A starts the spin key and ends the charge 4 frames after each.
Selftest switch BT_FAKE_ANIM_SYNC_BUG (tests/test_ingame_anim_sync.py): nosend -> B sends nothing; nostart -> A never
starts the spin key; nouser -> A says the puppet has no effect user; noend -> B never sends the end; nosword -> B's
state wears no sword; nopack -> A's pack manifest has effects false.
- A's co-op pack manifest (coop_puppet.json in its coop-puppet folder, testpack.PACK_REL) says "effects": true
  (tools/puppet_pack/build.py), merged into the file at B's first press of this row's boot (its switch on), so the
  manifest another row's fake writes at its setup (levitate.py: characters) is kept, not replaced.
"""
import json
import os

CHARGE, SPIN = "Charge_RotationCut_Loop", "Charge_RotationCut"
PROBE = "LICHII_COOP_DEBUG_SOUND=1"


def _bug():
    return os.environ.get("BT_FAKE_ANIM_SYNC_BUG", "")


def _on(game):
    b = game if game.tag == "B" else game.other
    return b is not None and b.env_on(PROBE)


PACK = os.path.join("mods", "contents", "0400486024DEC000", "coop-puppet")   # testpack.PACK_REL


def setup(game):
    game.anim_sync = {"held": False, "n": 0, "packed": False}


def _pack(a):
    """A's manifest gets "effects" (once a boot)."""
    d = os.path.join(a.folder, PACK)
    os.makedirs(d, exist_ok=True)
    f = os.path.join(d, "coop_puppet.json")
    try:
        with open(f, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError):
        doc = {"tool": "tools/puppet_pack"}
    doc["effects"] = _bug() != "nopack"
    with open(f, "w", encoding="utf-8") as fh:
        json.dump(doc, fh)


def _sent(game, key, end=False):
    game.anim_sync["n"] += 1
    game.say("coop: link effect '%s'%s sent (%d sent)" % (key, " end" if end else "", game.anim_sync["n"]))
    a = game.other
    if _bug() == "nouser":
        a.later(4, lambda: a.say("coop: puppet: P2 has no effect user: the player's effects are not played"))
    elif end:
        a.later(4, lambda: a.say("coop: puppet: P2 effect '%s' ended" % key))
    elif not (_bug() == "nostart" and key == SPIN):
        a.later(4, lambda: a.say("coop: puppet: P2 effect '%s'" % key))


def _let_go(game):
    game.anim_sync["held"] = False
    _sent(game, SPIN)
    if _bug() != "noend":
        game.later(20, lambda: _sent(game, CHARGE, end=True))


def press(game, buttons):
    if game.tag != "B" or not _on(game):
        return False
    if not game.anim_sync["packed"]:
        game.anim_sync["packed"] = True
        _pack(game.other)
    if _bug() == "nosend":
        return False
    held = "Y" in buttons
    if held and not game.anim_sync["held"]:
        game.anim_sync["held"] = True
        _sent(game, CHARGE)
    elif not held and game.anim_sync["held"]:
        _let_go(game)
    return False


def after(game, m, r):
    # a step after the hold is the let-go too (the test's step_both after the charge)
    if m.get("cmd") == "step" and game.tag == "B" and game.anim_sync["held"] and _on(game) and _bug() != "nosend":
        _let_go(game)


def state(game, st):
    if game.tag == "B" and _on(game) and _bug() == "nosword" and isinstance(st.get("equips"), dict):
        st["equips"]["weapon"] = None
