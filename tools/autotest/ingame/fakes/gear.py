"""Fake-game reactions for row 148's port (ingame/test_gear.py; owner port-sweep-c131). Each line cites the module.

- `wear hand|armor|boots NAME` writes the PlayerStatus flag and logs 'coop: test wear SLOT NAME: set'
  (lichii/hook/coop_testinput.cpp Wear); 'none' empties it.
- the other game logs 'coop: puppet: P<n> PLAYER_EQUIP: ..., hand '<hand>'' only when the SENT equipment changes
  (coop/host/src/sampler.cpp; the Note: lichii/hook/coop_puppet.cpp player_equip branch).
- what is sent (coop/host/src/player_reader.cpp read_equipment + wear_from_links): a CHILD's hand is what the game
  draws (none of the adult pieces: ''), an ADULT's named flag stands, but only once `dress` made the game wear it.
- age: KakarikoVillage_Adult = adult (the row's age-flag + warp); every other scene keeps the built-in child.
Selftest switches (tests/test_ingame_gear.py): BT_FAKE_GEAR_BUG=child -> a child's gauntlet flag is sent (the 0.11.4
bug); BT_FAKE_GEAR_BUG=rewarp -> an adult's rewarp sends hand '' (the links rule losing a real piece);
BT_FAKE_GEAR_BUG=stuck -> a game that was adult stays adult after the way back (the boot left broken for later tests).
"""
import os

FLAG = {"hand": "PlayerStatus.EquipHandArmor", "armor": "PlayerStatus.EquipArmor", "boots": "PlayerStatus.EquipBoot"}
ADULT_SCENE = "KakarikoVillage_Adult"


def _bug():
    return os.environ.get("BT_FAKE_GEAR_BUG", "")


def _adult(game):
    return game.scene == ADULT_SCENE and not game.at_title


def _send(game, hand):
    """game's equipment (hand) as the sampler sends it: the other game notes it only on a change."""
    if getattr(game, "gear_sent", None) == hand:
        return
    game.gear_sent = hand
    o, me = game.other, 1 if game.tag == "A" else 2
    o.later(8, lambda: o.say("coop: puppet: P%d PLAYER_EQUIP: weapon 'Weapon_Sword_Kokiri', shield '?', held '', "
                             "slots 0 '?' 1 '?' 2 '?' 5 '?' 6 '?', hand '%s'" % (me, hand)))


def lane(fl):
    for g in fl.g.values():
        _send(g, "")          # the session start: each game's equipment once (equipment_sent_ empty -> sent)


def act(game, w):
    if w[0] == "wear" and len(w) == 3 and w[1] in FLAG:
        game.flags[FLAG[w[1]]] = "" if w[2] == "none" else w[2]
        game.say("coop: test wear %s %s: set" % (w[1], w[2]))
        if w[1] == "hand" and not _adult(game) and _bug() == "child":
            _send(game, w[2])
        return True
    if w[0] == "dress" and len(w) == 2 and w[1] in ("hand", "all"):
        if _adult(game):
            _send(game, game.flags.get(FLAG["hand"], ""))
        return True
    return False


def after(game, m, r):
    if m.get("cmd") == "warp" and m.get("scene") == ADULT_SCENE and _bug() == "rewarp" and \
            getattr(game, "gear_sent", "") not in ("", None):
        _send(game, "")


def state(game, st):
    if _adult(game):
        game.was_adult = True
    if _adult(game) or (_bug() == "stuck" and getattr(game, "was_adult", False)):
        st["age"] = "adult"
