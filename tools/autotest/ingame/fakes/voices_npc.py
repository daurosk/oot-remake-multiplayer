"""Fake-game reactions for row 319 (ingame/test_voices_npc.py). Only on a boot with LICHII_COOP_DEBUG_VOICE=1 (the
test's needs_env), so no other row sees them. Lines are the module's (test_voices_npc.py cites each writer).

- Each game's co-op pack manifest (coop_puppet.json) gets characters.numbers {"0": 2, "3": 5} and the child sound_lists
  of lane1-A's pack (merged into what another fake wrote there).
- At the boot each game says its puppet's spawn line again with its sound list, its sound user line and its own
  'our sound user' line.
- A game's CharacterMenu_SelectedCharacter set to 0 / 2 / 5: 60 frames later the other game makes its puppet again as
  CoopPuppet_Child_Green / _C000 / _C003 (spawn line with the list, then its sound user line).
- Each ZL+Y press of a game: the other starts 'Voice_Attack' on that game's puppet.
Selftest switch BT_FAKE_VOICES_NPC_BUG (tests/test_ingame_voices_npc.py): nouser -> the Malon puppet's sound user is 0x0
and every sound on it is 'not in its sound user' (FAIL); wronglist -> Malon's spawn line says sound list CoopPuppet_Child (FAIL).
"""
import json
import os

SWITCH = "LICHII_COOP_DEBUG_VOICE=1"
CHAR_FLAG = "CharacterMenu_SelectedCharacter"
PACK = os.path.join("mods", "contents", "0400486024DEC000", "coop-puppet")   # testpack.PACK_REL
ACTORS = {0: "CoopPuppet_Child_Green", 2: "CoopPuppet_Child_C000", 5: "CoopPuppet_Child_C003"}
LISTS = {"CoopPuppet_Child": "CoopPuppet_Child", "CoopPuppet_Child_Green": "CoopPuppet_Child",
         "CoopPuppet_Child_C000": "Player_Child_V02", "CoopPuppet_ChildQuiet_C000": "Player_Child_V02",
         "CoopPuppet_Child_C003": "Player_Child_V05", "CoopPuppet_ChildQuiet_C003": "Player_Child_V05"}
USERS = {"CoopPuppet_Child_Green": "0x2a1c0", "CoopPuppet_Child_C000": "0x2b2c0", "CoopPuppet_Child_C003": "0x2c3c0"}


def _bug():
    return os.environ.get("BT_FAKE_VOICES_NPC_BUG", "")


def _on(game):
    return game.env_on(SWITCH)


def _them(game):
    return 2 if game.tag == "A" else 1


def _spawn(w, actor):
    """Watcher w makes the other game's puppet as `actor`."""
    w.vn_actor = actor
    n = _them(w)
    lst = "CoopPuppet_Child" if (_bug() == "wronglist" and actor.endswith("C003")) else LISTS[actor]
    w.say("coop: puppet: P%d %s spawned after 1000 ms: request 0x1, actor 0x2, state 4, at (0, 0, 0), sound list %s"
          % (n, actor, lst))
    user = "0x0" if (_bug() == "nouser" and actor.endswith("C003")) else USERS[actor]
    w.say("coop: puppet: P%d sound user %s is '%s'" % (n, user, "" if user == "0x0" else "Player_Child"))


def setup(game):
    game.vn_actor = ACTORS[0]
    if not _on(game):
        return
    d = os.path.join(game.folder, PACK)
    os.makedirs(d, exist_ok=True)
    f = os.path.join(d, "coop_puppet.json")
    try:
        with open(f, encoding="utf-8") as fh:
            j = json.load(fh)
    except (OSError, ValueError):
        j = {"tool": "tools/puppet_pack"}
    c = j.setdefault("characters", {})
    c.setdefault("numbers", {}).update({"0": 2, "3": 5})
    v = c.setdefault("variants", {})
    v["0"] = v["3"] = ["Child", "Adult"]
    j.setdefault("sound_lists", {}).update(LISTS)
    with open(f, "w", encoding="utf-8") as fh:
        json.dump(j, fh)
    _spawn(game, ACTORS[0])
    game.say("coop: puppet: our sound user 0x1a0c0 is 'Player_Child'")


def after(game, m, r):
    if not _on(game) or m.get("cmd") != "flag" or m.get("name") != CHAR_FLAG:
        return
    actor = ACTORS.get(m.get("value"))
    w = game.other
    if actor and actor != w.vn_actor:
        w.later(60, lambda: _spawn(w, actor))


def press(game, buttons):
    if not _on(game) or "Y" not in buttons or "ZL" not in buttons:
        return False
    w, n = game.other, 1 if game.tag == "A" else 2
    if _bug() == "nouser" and w.vn_actor.endswith("C003"):
        w.later(4, lambda: w.say("coop: puppet: P%d: the sound 'Voice_Attack' is not in its sound user" % n))
    else:
        w.later(4, lambda: w.say("coop: puppet: P%d sound 'Voice_Attack'" % n))
    return False
